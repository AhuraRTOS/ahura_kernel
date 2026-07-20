/**
 * @file os_timer.c
 * @brief Software timers: expiry detected by the kernel tick, callbacks run on
 *        a kernel timer task at the highest priority.
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

#include <limits.h>

#if (OS_CONFIG_TIMER_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static uint8_t              os_timer_task_stack[OS_CONFIG_TIMER_STACK_SIZE] OS_STACK_ALIGNED;
static os_task_t            os_timer_task_handle;

/* Resolved once in os_timer_system_init: the timer task is never deleted, so
 * this handle stays valid for the kernel's lifetime and the tick-time expiry
 * wake skips the id lookup. */
static void                 *os_timer_task_tcb = NULL;

/* Registry of started timers, advanced on every kernel tick. Fixed slots so
 * tick-time iteration stays safe against concurrent start/stop calls.
 * The slot (the pointer itself) is what the ISR and tasks race on, so the
 * typedef lets __IO qualify the slot rather than the pointed-to timer. */
typedef os_timer_t *os_timer_slot_t;

static __IO os_timer_slot_t os_timer_registry[OS_CONFIG_MAX_TIMERS];

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static void        os_timer_task_entry(void *context);
static os_timer_t* os_timer_expired_fetch(void);
static bool        os_timer_expired_exists(void);
static uint32_t    os_timer_registry_slot_find(const os_timer_t *timer);
static uint32_t    os_timer_registry_slot_acquire(const os_timer_t *timer);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize a software timer as one-shot or periodic.
 *
 * @param[in,out] timer         Timer object.
 * @param[in]     period_ticks  Timer period in ticks (see OS_TICKS_FROM_MS).
 * @param[in]     mode          OS_TIMER_MODE_ONE_SHOT or OS_TIMER_MODE_PERIODIC.
 * @param[in]     callback      Expiry callback (runs in the kernel timer task).
 * @param[in]     context       Callback context pointer.
 * @return os_status       Status code.
 */
os_status os_timer_init(os_timer_t *timer, uint32_t period_ticks, os_timer_mode_t mode, os_timer_callback_t callback, void *context)
{
    if ((timer == NULL) || (period_ticks == 0U) || (callback == NULL) ||
        ((mode != OS_TIMER_MODE_ONE_SHOT) && (mode != OS_TIMER_MODE_PERIODIC)))
    {
        return OS_STATUS_INVALID_ARG;
    }

    timer->period_ticks    = period_ticks;
    timer->remaining_ticks = period_ticks;
    timer->mode            = mode;
    timer->active          = false;
    timer->expired         = false;
    timer->callback        = callback;
    timer->context         = context;

    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Start a software timer (registers it with the kernel tick).
 *
 * @param[in,out] timer  Timer object (must be initialized).
 * @return os_status  OK on start, FULL when no registry slot is free.
 */
os_status os_timer_start(os_timer_t *timer)
{
    uint32_t slot;

    if ((timer == NULL) || (timer->callback == NULL) || (timer->period_ticks == 0U))
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    /* Single pass: restarting an already-registered timer takes its own
     * slot rather than a free one, and both the match and the free-slot
     * fallback are found in one walk instead of two. */
    slot = os_timer_registry_slot_acquire(timer);

    if (slot >= OS_CONFIG_MAX_TIMERS)
    {
        os_critical_exit();
        return OS_STATUS_FULL;
    }

    os_timer_registry[slot] = timer;
    timer->remaining_ticks  = timer->period_ticks;
    timer->active           = true;

    os_critical_exit();
    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Stop a software timer, discarding any not-yet-delivered expiry.
 *
 * @param[in,out] timer  Timer object.
 * @return os_status Status code.
 */
os_status os_timer_stop(os_timer_t *timer)
{
    uint32_t slot;

    if (timer == NULL)
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    timer->active  = false;
    timer->expired = false;
    slot           = os_timer_registry_slot_find(timer);
    if (slot < OS_CONFIG_MAX_TIMERS)
    {
        os_timer_registry[slot] = NULL;
    }

    os_critical_exit();
    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Create and start the kernel timer service task. Called from os_init.
 *
 * @return os_status  Status code.
 */
os_status os_timer_system_init(void)
{
    uint32_t  slot;
    os_status status;

    os_task_config_t config =
    {
        "tsk_timer",
        os_timer_task_entry,
        NULL,
        OS_TASK_PRIO_MAX,
        (void *)os_timer_task_stack,
        sizeof(os_timer_task_stack),
        OS_CONFIG_TIMER_CORE_AFFINITY
    };

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        os_timer_registry[slot] = NULL;
    }

    status = os_task_create_system(&os_timer_task_handle, &config);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    status = os_task_start(&os_timer_task_handle);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    /* Resolved once: the timer task is never deleted, so the tick-time
     * expiry wake can skip the id lookup from here on. */
    os_timer_task_tcb = os_task_tcb_resolve(os_timer_task_handle.id);

    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Advance all registered timers by elapsed ticks; hand expiries to the timer task.
 *
 * Called from the tick interrupt. Periods are reloaded here so periodic
 * timers do not drift with callback latency; expiries arriving faster than
 * the timer task can run them coalesce into one callback invocation.
 *
 * @param[in] elapsed_ticks  Number of elapsed ticks.
 * @return None.
 */
void os_timer_tick_process(uint32_t elapsed_ticks)
{
    uint32_t mask_state;
    uint32_t slot;
    bool     wake_needed = false;

    if (elapsed_ticks == 0U)
    {
        return;
    }

    /* The kernel mask is raised so a preempting ISR starting or stopping
     * timers cannot interleave with the active-check/expired-write pair
     * below (a stop landing in between would be silently undone). Also
     * covers the tickless announce path, which calls this from task context.
     * On multi-core builds the cross-core spinlock additionally excludes the
     * other cores' os_timer_start/os_timer_stop callers, who hold it via
     * os_critical_enter - the local mask alone only stops this core's own
     * interrupts. Released before os_task_wake below, which acquires the
     * same lock itself (never hold both at once - not recursive). */
    mask_state = os_arch_kernel_mask_save();
    os_critical_multicore_lock();

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        os_timer_t *timer = os_timer_registry[slot];

        if ((timer == NULL) || (!timer->active))
        {
            continue;
        }

        if (timer->remaining_ticks > elapsed_ticks)
        {
            timer->remaining_ticks -= elapsed_ticks;
            continue;
        }

        if (timer->mode == OS_TIMER_MODE_PERIODIC)
        {
            timer->remaining_ticks = timer->period_ticks;
        }
        else
        {
            /* One-shot: keep the registry slot until the callback has run. */
            timer->active = false;
        }

        timer->expired = true;
        wake_needed    = true;
    }

    if (wake_needed)
    {
        /* Direct-handle wake: skips both the id lookup and the nested
         * critical section os_task_wake would pay on every expiring tick;
         * safe here because the kernel mask and (multi-core) spinlock this
         * function already holds are exactly what os_task_wake_tcb
         * requires the caller to provide. */
        os_task_wake_tcb(os_timer_task_tcb);
    }

    os_critical_multicore_unlock();
    os_arch_kernel_mask_restore(mask_state);
}

/******************************************************************************************************/
/**
 * @brief Return ticks until next active timer expiry.
 *
 * @return uint32_t  Minimum ticks until next timer, or UINT32_MAX if none.
 */
uint32_t os_timer_next_expiry_ticks_get(void)
{
    uint32_t slot;
    uint32_t minimum = UINT32_MAX;

    os_critical_enter();

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        const os_timer_t *timer = os_timer_registry[slot];

        if ((timer != NULL) && timer->active && (timer->remaining_ticks < minimum))
        {
            minimum = timer->remaining_ticks;
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
 * @brief Timer task body: run expiry callbacks, sleep until woken otherwise.
 *
 * @param[in] context  Unused.
 * @return None.
 */
static void os_timer_task_entry(void *context)
{
    (void)context;

    while (1)
    {
        os_timer_t *timer = os_timer_expired_fetch();

        if (timer != NULL)
        {
            timer->callback(timer->context);
            continue;
        }

        /* The emptiness check and the block form one atomic unit (outer
         * critical section), so an expiry arriving in between cannot be
         * lost: it is seen here, or its wake lands after the block. */
        os_critical_enter();

        if (!os_timer_expired_exists())
        {
            os_task_sleep_ticks(OS_WAIT_FOREVER);
        }

        os_critical_exit();
    }
}

/******************************************************************************************************/
/**
 * @brief Take one expired timer out of the pending set.
 *
 * A finished one-shot releases its registry slot before the callback runs,
 * so the callback may safely restart its own timer.
 *
 * @return os_timer_t*  Expired timer, or NULL when none.
 */
static os_timer_t* os_timer_expired_fetch(void)
{
    os_timer_t *timer = NULL;
    uint32_t   slot;

    os_critical_enter();

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        os_timer_t *candidate = os_timer_registry[slot];

        if ((candidate != NULL) && candidate->expired)
        {
            candidate->expired = false;

            if (!candidate->active)
            {
                os_timer_registry[slot] = NULL;
            }

            timer = candidate;
            break;
        }
    }

    os_critical_exit();
    return timer;
}

/******************************************************************************************************/
/**
 * @brief Check whether any registered timer has a pending expiry. Caller holds the critical section.
 *
 * @return bool  True when at least one expiry awaits delivery.
 */
static bool os_timer_expired_exists(void)
{
    uint32_t slot;

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        if ((os_timer_registry[slot] != NULL) && os_timer_registry[slot]->expired)
        {
            return true;
        }
    }

    return false;
}

/******************************************************************************************************/
/**
 * @brief Find the registry slot holding the given timer pointer (NULL finds a free slot).
 *
 * @param[in] timer  Timer pointer to search for, or NULL.
 * @return uint32_t  Slot index, or OS_CONFIG_MAX_TIMERS when not found.
 */
static uint32_t os_timer_registry_slot_find(const os_timer_t *timer)
{
    uint32_t slot;

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        if (os_timer_registry[slot] == timer)
        {
            return slot;
        }
    }

    return OS_CONFIG_MAX_TIMERS;
}

/******************************************************************************************************/
/**
 * @brief Find timer's existing slot, or the first free slot if it has none - in one pass.
 *
 * Used by os_timer_start, where restarting an already-registered timer must take that
 * timer's own slot rather than a free one; a match found mid-scan still wins even if a free
 * slot was noticed earlier, since the loop keeps searching for it until either the match is
 * found or the whole registry has been seen.
 *
 * @param[in] timer  Timer pointer to search for.
 * @return uint32_t  timer's existing slot, else the first free slot, else OS_CONFIG_MAX_TIMERS.
 */
static uint32_t os_timer_registry_slot_acquire(const os_timer_t *timer)
{
    uint32_t free_slot = OS_CONFIG_MAX_TIMERS;
    uint32_t slot;

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        if (os_timer_registry[slot] == timer)
        {
            return slot;
        }

        if ((os_timer_registry[slot] == NULL) && (free_slot >= OS_CONFIG_MAX_TIMERS))
        {
            free_slot = slot;
        }
    }

    return free_slot;
}

#endif /* OS_CONFIG_TIMER_ENABLE */
