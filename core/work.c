/**
 * @file work.c
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

static uint8_t             work_task_stack[OS_CONFIG_WORK_STACK_SIZE] OS_STACK_ALIGNED;
static os_task_t           work_task_handle;

/* Registry of submitted work items, advanced on every kernel tick. Fixed
 * slots so tick-time iteration stays safe against concurrent submit/cancel. */
static os_work_t *volatile work_registry[OS_CONFIG_MAX_WORKS];

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static void       work_task_entry(void *context);
static os_work_t* work_ready_fetch(void);
static bool       work_ready_exists(void);
static uint32_t   work_registry_slot_find(const os_work_t *work);

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

    /* Re-submitting an already-registered item just reschedules it. */
    slot = work_registry_slot_find(work);
    if (slot >= OS_CONFIG_MAX_WORKS)
    {
        slot = work_registry_slot_find(NULL);
    }

    if (slot >= OS_CONFIG_MAX_WORKS)
    {
        os_critical_exit();
        return OS_STATUS_FULL;
    }

    work_registry[slot] = work;

    if (delay_ticks == 0U)
    {
        work->delay_ticks = 0U;
        work->pending     = false;
        work->ready       = true;
        os_task_wake(work_task_handle.id);
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

    slot = work_registry_slot_find(work);
    if (slot < OS_CONFIG_MAX_WORKS)
    {
        work_registry[slot] = NULL;
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
        work_task_entry,
        (void *)0,
        OS_CONFIG_MAX_PRIORITY,
        (void *)work_task_stack,
        sizeof(work_task_stack)
    };

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        work_registry[slot] = NULL;
    }

    status = os_task_create_system(&work_task_handle, &config);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    return os_task_start(&work_task_handle);
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
    uint32_t slot;
    bool     wake_needed = false;

    if (elapsed_ticks == 0U)
    {
        return;
    }

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        os_work_t *work = work_registry[slot];

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
        os_task_wake(work_task_handle.id);
    }
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
static void work_task_entry(void *context)
{
    (void)context;

    while (1)
    {
        os_work_t *work = work_ready_fetch();

        if (work != NULL)
        {
            work->handler(work->context);
            continue;
        }

        /* The emptiness check and the block form one atomic unit (outer
         * critical section), so a submission arriving in between cannot be
         * lost: it is seen here, or its wake lands after the block. */
        os_critical_enter();

        if (!work_ready_exists())
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
static os_work_t *work_ready_fetch(void)
{
    os_work_t *work = NULL;
    uint32_t  slot;

    os_critical_enter();

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        os_work_t* candidate = work_registry[slot];

        if ((candidate != NULL) && candidate->ready)
        {
            candidate->ready    = false;
            work_registry[slot] = NULL;
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
static bool work_ready_exists(void)
{
    uint32_t slot;

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        if ((work_registry[slot] != NULL) && work_registry[slot]->ready)
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
static uint32_t work_registry_slot_find(const os_work_t *work)
{
    uint32_t slot;

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        if (work_registry[slot] == work)
        {
            return slot;
        }
    }

    return OS_CONFIG_MAX_WORKS;
}

#endif /* OS_CONFIG_WORK_ENABLE */
