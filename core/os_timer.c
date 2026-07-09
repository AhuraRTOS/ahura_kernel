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

static uint8_t              timer_task_stack[OS_CONFIG_TIMER_STACK_SIZE] OS_STACK_ALIGNED;
static os_task_t            timer_task_handle;

/* Registry of started timers, advanced on every kernel tick. Fixed slots so
 * tick-time iteration stays safe against concurrent start/stop calls.
 * The slot (the pointer itself) is what the ISR and tasks race on, so the
 * typedef lets __IO qualify the slot rather than the pointed-to timer. */
typedef os_timer_t *os_timer_slot_t;

static __IO os_timer_slot_t timer_registry[OS_CONFIG_MAX_TIMERS];

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static void        timer_task_entry(void *context);
static os_timer_t* timer_expired_fetch(void);
static bool        timer_expired_exists(void);
static uint32_t    timer_registry_slot_find(const os_timer_t *timer);

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

    /* Restarting an already-registered timer just reloads it. */
    slot = timer_registry_slot_find(timer);
    if (slot >= OS_CONFIG_MAX_TIMERS)
    {
        slot = timer_registry_slot_find(NULL);
    }

    if (slot >= OS_CONFIG_MAX_TIMERS)
    {
        os_critical_exit();
        return OS_STATUS_FULL;
    }

    timer_registry[slot]   = timer;
    timer->remaining_ticks = timer->period_ticks;
    timer->active          = true;

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
    slot           = timer_registry_slot_find(timer);
    if (slot < OS_CONFIG_MAX_TIMERS)
    {
        timer_registry[slot] = NULL;
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
        timer_task_entry,
        (void *)0,
        OS_CONFIG_MAX_PRIORITY,
        (void *)timer_task_stack,
        sizeof(timer_task_stack),
        OS_CONFIG_TIMER_CORE_AFFINITY
    };

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        timer_registry[slot] = NULL;
    }

    status = os_task_create_system(&timer_task_handle, &config);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    return os_task_start(&timer_task_handle);
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
    uint32_t slot;
    bool     wake_needed = false;

    if (elapsed_ticks == 0U)
    {
        return;
    }

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        os_timer_t *timer = timer_registry[slot];

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
        os_task_wake(timer_task_handle.id);
    }
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
        const os_timer_t *timer = timer_registry[slot];

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
static void timer_task_entry(void *context)
{
    (void)context;

    while (1)
    {
        os_timer_t *timer = timer_expired_fetch();

        if (timer != NULL)
        {
            timer->callback(timer->context);
            continue;
        }

        /* The emptiness check and the block form one atomic unit (outer
         * critical section), so an expiry arriving in between cannot be
         * lost: it is seen here, or its wake lands after the block. */
        os_critical_enter();

        if (!timer_expired_exists())
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
static os_timer_t* timer_expired_fetch(void)
{
    os_timer_t *timer = NULL;
    uint32_t   slot;

    os_critical_enter();

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        os_timer_t *candidate = timer_registry[slot];

        if ((candidate != NULL) && candidate->expired)
        {
            candidate->expired = false;

            if (!candidate->active)
            {
                timer_registry[slot] = NULL;
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
static bool timer_expired_exists(void)
{
    uint32_t slot;

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        if ((timer_registry[slot] != NULL) && timer_registry[slot]->expired)
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
static uint32_t timer_registry_slot_find(const os_timer_t *timer)
{
    uint32_t slot;

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        if (timer_registry[slot] == timer)
        {
            return slot;
        }
    }

    return OS_CONFIG_MAX_TIMERS;
}

#endif /* OS_CONFIG_TIMER_ENABLE */
