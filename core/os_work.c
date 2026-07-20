/**
 * @file os_work.c
 * @brief Deferrable work queue: items run on a kernel task at the highest priority.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include "os_internal.h"

#if (OS_CONFIG_WORK_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static uint8_t             os_work_task_stack[OS_CONFIG_WORK_STACK_SIZE] OS_STACK_ALIGNED;
static os_task_t           os_work_task_handle;

/* Resolved once in os_work_system_init: the work task is never deleted, so
 * this handle stays valid for the kernel's lifetime and every later wake
 * (submit and the tick-time expiry path) skips the id lookup. */
static void                *os_work_task_tcb = NULL;

/* Registry of submitted work items, advanced on every kernel tick. Fixed
 * slots so tick-time iteration stays safe against concurrent submit/cancel.
 * The slot (the pointer itself) is what the ISR and tasks race on, so the
 * typedef lets __IO qualify the slot rather than the pointed-to item. */
typedef os_work_t *os_work_slot_t;

static __IO os_work_slot_t os_work_registry[OS_CONFIG_MAX_WORKS];

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static void       os_work_task_entry(void *context);
static os_work_t* os_work_ready_fetch(void);
static bool       os_work_ready_exists(void);
static uint32_t   os_work_registry_slot_find(const os_work_t *work);
static uint32_t   os_work_registry_slot_acquire(const os_work_t *work);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize a work item with its handler and user-data pointer.
 *
 * @param[in,out] work     Work item.
 * @param[in]     handler  Function executed by the kernel work task.
 * @param[in]     context  User data passed to the handler.
 * @return os_status  Status code.
 */
os_status os_work_init(os_work_t *work, os_work_handler_t handler, void *context)
{
    if ((work == NULL) || (handler == NULL))
    {
        return OS_STATUS_INVALID_ARG;
    }

    work->handler     = handler;
    work->context     = context;
    work->delay_ticks = 0U;
    work->pending     = false;
    work->ready       = false;

    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Submit work to run after delay_ms on the kernel work task.
 *
 * ISR-safe. Submitting an already-pending item reschedules it with the new
 * delay. delay_ms == 0 makes the item ready immediately; because the work
 * task runs at the highest priority, it preempts any user task as soon as
 * the scheduler is invoked.
 *
 * @param[in,out] work      Work item (must be initialized).
 * @param[in]     delay_ms  Delay before execution in milliseconds (0 = as soon as possible).
 * @return os_status  OK on submission, FULL when no registry slot is free.
 */
os_status os_work_submit(os_work_t *work, uint32_t delay_ms)
{
    uint32_t slot;
    uint32_t delay_ticks;

    if ((work == NULL) || (work->handler == NULL) || (delay_ms == OS_WAIT_FOREVER))
    {
        return OS_STATUS_INVALID_ARG;
    }

    delay_ticks = OS_TICKS_FROM_MS(delay_ms);

    os_critical_enter();

    /* Single pass: an already-registered item (re-submit) takes precedence
     * over any free slot noticed along the way, so the match and the
     * free-slot fallback are both found in one walk of the registry - the
     * common case (item not yet registered) previously paid a full scan
     * just to learn that, then a second to find a free slot. */
    slot = os_work_registry_slot_acquire(work);

    if (slot >= OS_CONFIG_MAX_WORKS)
    {
        os_critical_exit();
        return OS_STATUS_FULL;
    }

    os_work_registry[slot] = work;

    if (delay_ticks == 0U)
    {
        work->delay_ticks = 0U;
        work->pending     = false;
        work->ready       = true;

        /* Direct-handle wake: skips the id lookup os_task_wake would do;
         * safe here because os_critical_enter above already holds the
         * kernel mask (and, on multi-core builds, the same spinlock
         * os_task_wake_tcb requires the caller to hold). */
        os_task_wake_tcb(os_work_task_tcb);
    }
    else
    {
        work->delay_ticks = delay_ticks;
        work->pending     = true;
        work->ready       = false;
    }

    os_critical_exit();
    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Cancel submitted work that has not started executing yet.
 *
 * ISR-safe. Work whose handler is already running cannot be stopped.
 *
 * @param[in,out] work  Work item.
 * @return os_status  OK when a submission was cancelled, EMPTY when none was pending.
 */
os_status os_work_cancel(os_work_t *work)
{
    uint32_t slot;
    bool     was_submitted;

    if (work == NULL)
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    was_submitted     = (work->pending || work->ready);
    work->delay_ticks = 0U;
    work->pending     = false;
    work->ready       = false;

    slot = os_work_registry_slot_find(work);
    if (slot < OS_CONFIG_MAX_WORKS)
    {
        os_work_registry[slot] = NULL;
    }

    os_critical_exit();
    return was_submitted ? OS_STATUS_OK : OS_STATUS_EMPTY;
}

/******************************************************************************************************/
/**
 * @brief Check whether a work item is submitted and not yet executed.
 *
 * @param[in] work  Work item.
 * @return bool  True while the item is waiting for its delay or for execution.
 */
bool os_work_is_pending(const os_work_t *work)
{
    bool is_pending;

    if (work == NULL)
    {
        return false;
    }

    os_critical_enter();
    is_pending = (work->pending || work->ready);
    os_critical_exit();

    return is_pending;
}

/******************************************************************************************************/
/**
 * @brief Create and start the kernel work service task. Called from os_init.
 *
 * @return os_status  Status code.
 */
os_status os_work_system_init(void)
{
    uint32_t  slot;
    os_status status;

    os_task_config_t config =
    {
        "tsk_work",
        os_work_task_entry,
        NULL,
        OS_TASK_PRIO_MAX,
        (void *)os_work_task_stack,
        sizeof(os_work_task_stack),
        OS_CONFIG_WORK_CORE_AFFINITY
    };

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        os_work_registry[slot] = NULL;
    }

    status = os_task_create_system(&os_work_task_handle, &config);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    status = os_task_start(&os_work_task_handle);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    /* Resolved once: the work task is never deleted, so every later wake
     * (submit and the tick-time expiry path) can skip the id lookup. */
    os_work_task_tcb = os_task_tcb_resolve(os_work_task_handle.id);

    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Advance delayed work items by elapsed ticks. Called from the tick interrupt.
 *
 * @param[in] elapsed_ticks  Number of elapsed ticks.
 * @return None.
 */
void os_work_tick_process(uint32_t elapsed_ticks)
{
    uint32_t mask_state;
    uint32_t slot;
    bool     wake_needed = false;

    if (elapsed_ticks == 0U)
    {
        return;
    }

    /* The kernel mask is raised so a preempting ISR submitting or cancelling
     * cannot interleave with the pending-check/ready-write pair below
     * (a cancel landing in between would be silently undone). Also covers
     * the tickless announce path, which calls this from task context. On
     * multi-core builds the cross-core spinlock additionally excludes the
     * other cores' os_work_submit/os_work_cancel callers, who hold it via
     * os_critical_enter - the local mask alone only stops this core's own
     * interrupts. Released before os_task_wake below, which acquires the
     * same lock itself (never hold both at once - not recursive). */
    mask_state = os_arch_kernel_mask_save();
    os_critical_multicore_lock();

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        os_work_t *work = os_work_registry[slot];

        if ((work == NULL) || (!work->pending))
        {
            continue;
        }

        if (work->delay_ticks > elapsed_ticks)
        {
            work->delay_ticks -= elapsed_ticks;
        }
        else
        {
            work->delay_ticks = 0U;
            work->pending     = false;
            work->ready       = true;
            wake_needed       = true;
        }
    }

    if (wake_needed)
    {
        /* Direct-handle wake: skips both the id lookup and the nested
         * critical section os_task_wake would pay on every expiring tick;
         * safe here because the kernel mask and (multi-core) spinlock this
         * function already holds are exactly what os_task_wake_tcb
         * requires the caller to provide. */
        os_task_wake_tcb(os_work_task_tcb);
    }

    os_critical_multicore_unlock();
    os_arch_kernel_mask_restore(mask_state);
}

/******************************************************************************************************/
/**
 * @brief Return ticks until the next delayed work item becomes ready (tickless planning).
 *
 * @return uint32_t  Minimum remaining delay in ticks, 0 when an item is
 *                   already ready, UINT32_MAX when nothing is registered.
 */
uint32_t os_work_next_ready_ticks_get(void)
{
    uint32_t slot;
    uint32_t minimum = UINT32_MAX;

    os_critical_enter();

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        const os_work_t *work = os_work_registry[slot];

        if (work == NULL)
        {
            continue;
        }

        if (work->ready)
        {
            minimum = 0U;
            break;
        }

        if (work->pending && (work->delay_ticks < minimum))
        {
            minimum = work->delay_ticks;
        }
    }

    os_critical_exit();
    return minimum;
}

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Work task body: execute ready items, sleep until woken otherwise.
 *
 * @param[in] context  Unused.
 * @return None.
 */
static void os_work_task_entry(void *context)
{
    (void)context;

    while (1)
    {
        os_work_t *work = os_work_ready_fetch();

        if (work != NULL)
        {
            work->handler(work->context);
            continue;
        }

        /* The emptiness check and the block form one atomic unit (outer
         * critical section), so a submission arriving in between cannot be
         * lost: it is seen here, or its wake lands after the block. */
        os_critical_enter();

        if (!os_work_ready_exists())
        {
            os_task_sleep_ticks(OS_WAIT_FOREVER);
        }

        os_critical_exit();
    }
}

/******************************************************************************************************/
/**
 * @brief Take one ready work item out of the registry.
 *
 * The slot is released before the handler runs so the handler may safely
 * re-submit its own item.
 *
 * @return os_work_t*  Ready work item, or NULL when none.
 */
static os_work_t* os_work_ready_fetch(void)
{
    os_work_t *work = NULL;
    uint32_t  slot;

    os_critical_enter();

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        os_work_t* candidate = os_work_registry[slot];

        if ((candidate != NULL) && candidate->ready)
        {
            candidate->ready    = false;
            os_work_registry[slot] = NULL;
            work                = candidate;
            break;
        }
    }

    os_critical_exit();
    return work;
}

/******************************************************************************************************/
/**
 * @brief Check whether any registered work item is ready. Caller holds the critical section.
 *
 * @return bool  True when at least one item awaits execution.
 */
static bool os_work_ready_exists(void)
{
    uint32_t slot;

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        if ((os_work_registry[slot] != NULL) && os_work_registry[slot]->ready)
        {
            return true;
        }
    }

    return false;
}

/******************************************************************************************************/
/**
 * @brief Find the registry slot holding the given work pointer (NULL finds a free slot).
 *
 * @param[in] work  Work pointer to search for, or NULL.
 * @return uint32_t  Slot index, or OS_CONFIG_MAX_WORKS when not found.
 */
static uint32_t os_work_registry_slot_find(const os_work_t *work)
{
    uint32_t slot;

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        if (os_work_registry[slot] == work)
        {
            return slot;
        }
    }

    return OS_CONFIG_MAX_WORKS;
}

/******************************************************************************************************/
/**
 * @brief Find work's existing slot, or the first free slot if it has none - in one pass.
 *
 * Used by os_work_submit, where re-submitting an already-registered item must take that
 * item's own slot rather than a free one; a match found mid-scan still wins even if a free
 * slot was noticed earlier, since the loop keeps searching for it until either the match is
 * found or the whole registry has been seen.
 *
 * @param[in] work  Work pointer to search for.
 * @return uint32_t  work's existing slot, else the first free slot, else OS_CONFIG_MAX_WORKS.
 */
static uint32_t os_work_registry_slot_acquire(const os_work_t *work)
{
    uint32_t free_slot = OS_CONFIG_MAX_WORKS;
    uint32_t slot;

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        if (os_work_registry[slot] == work)
        {
            return slot;
        }

        if ((os_work_registry[slot] == NULL) && (free_slot >= OS_CONFIG_MAX_WORKS))
        {
            free_slot = slot;
        }
    }

    return free_slot;
}

#endif /* OS_CONFIG_WORK_ENABLE */
