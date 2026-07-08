/**
 * @file semaphore.c
 * @brief Semaphore module implementation with timeouts.
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

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize a semaphore object.
 *
 * @param[in,out] semaphore      Semaphore object.
 * @param[in]     initial_count  Initial token count.
 * @param[in]     max_count      Maximum token count.
 * @return os_status        Status code.
 */
os_status os_semaphore_init(os_semaphore_t *semaphore, uint32_t initial_count, uint32_t max_count)
{
    if ((semaphore == NULL) || (max_count == 0U) || (initial_count > max_count))
    {
        return OS_STATUS_INVALID_ARG;
    }

    semaphore->count     = initial_count;
    semaphore->max_count = max_count;
    os_list_init(&semaphore->waiters);

    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Give one token to semaphore (ISR-safe, never blocks).
 *
 * @param[in,out] semaphore  Semaphore object.
 * @return os_status    Status code.
 */
os_status os_semaphore_give(os_semaphore_t *semaphore)
{
    os_status status = OS_STATUS_OK;

    if (semaphore == NULL)
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    if (semaphore->count >= semaphore->max_count)
    {
        status = OS_STATUS_FULL;
    }
    else
    {
        semaphore->count++;

        /* Hand the token to the highest-priority waiter (it re-takes in its
         * own context). */
        (void)os_task_waiters_wake_one(&semaphore->waiters);
    }

    os_critical_exit();
    return status;
}

/******************************************************************************************************/
/**
 * @brief Take one token from semaphore, waiting up to timeout_ms when empty.
 *
 * Nonzero timeouts are only honored from task context after os_start; from
 * interrupt context the call behaves like OS_WAIT_NOTHING.
 *
 * @param[in,out] semaphore   Semaphore object.
 * @param[in]     timeout_ms  OS_WAIT_NOTHING, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_status  OK on take, EMPTY when unavailable without waiting,
 *                    TIMEOUT when the wait elapsed.
 */
os_status os_semaphore_take(os_semaphore_t *semaphore, uint32_t timeout_ms)
{
    uint32_t remaining_ticks;

    if (semaphore == NULL)
    {
        return OS_STATUS_INVALID_ARG;
    }

    remaining_ticks = os_internal_timeout_to_ticks(timeout_ms);

    for (;;)
    {
        os_critical_enter();

        if (semaphore->count > 0U)
        {
            semaphore->count--;
            os_critical_exit();
            return OS_STATUS_OK;
        }

        if ((timeout_ms == OS_WAIT_NOTHING) || !os_internal_can_block())
        {
            os_critical_exit();
            return OS_STATUS_EMPTY;
        }

        if (remaining_ticks == 0U)
        {
            os_critical_exit();
            return OS_STATUS_TIMEOUT;
        }

        /* Join the waiter list inside the same critical section that saw the
         * semaphore empty (no lost-wakeup window); the switch happens on exit. */
        os_task_wait_begin(&semaphore->waiters, remaining_ticks);
        os_critical_exit();

        /* Resumed: a give signaled us (retry the take) or the wait timed out. */
        if (!os_task_wait_signaled())
        {
            return OS_STATUS_TIMEOUT;
        }

        remaining_ticks = os_task_wait_remaining_ticks();
    }
}

#endif /* OS_CONFIG_SEMAPHORE_ENABLE */
