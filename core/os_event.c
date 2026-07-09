/**
 * @file os_event.c
 * @brief Event group module implementation with timeouts.
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

#if (OS_CONFIG_EVENT_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize an event group object.
 *
 * @param[in,out] group  Event group object.
 * @return os_status Status code.
 */
os_status os_event_group_init(os_event_group_t *group)
{
    if (group == NULL)
    {
        return OS_STATUS_INVALID_ARG;
    }

    group->flags = 0U;
    os_list_init(&group->waiters);

    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Set event bits in the group (ISR-safe).
 *
 * @param[in,out] group  Event group object.
 * @param[in]     bits   Bits to set.
 * @return os_status Status code.
 */
os_status os_event_group_set_bits(os_event_group_t *group, uint32_t bits)
{
    if (group == NULL)
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    group->flags |= bits;

    /* Every waiter re-evaluates its own bit condition; non-matching ones
     * re-join the list with their remaining timeout. */
    os_task_waiters_wake_all(&group->waiters);

    os_critical_exit();

    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Clear event bits in the group (ISR-safe).
 *
 * @param[in,out] group  Event group object.
 * @param[in]     bits   Bits to clear.
 * @return os_status Status code.
 */
os_status os_event_group_clear_bits(os_event_group_t *group, uint32_t bits)
{
    if (group == NULL)
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();
    group->flags &= ~bits;
    os_critical_exit();

    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Wait for event bits, waiting up to timeout_ms until they match.
 *
 * The bits are not cleared on exit; use os_event_group_clear_bits when the
 * event is consumed. Nonzero timeouts are only honored from task context
 * after os_start.
 *
 * @param[in]  group        Event group object.
 * @param[in]  bits         Bits to wait for.
 * @param[in]  wait_all     True to require all bits, false for any bit.
 * @param[out] matched_bits Matched bits snapshot (also written on failure).
 * @param[in]  timeout_ms   OS_WAIT_NOTHING, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_status  OK on match, BUSY when unmatched without waiting,
 *                    TIMEOUT when the wait elapsed.
 */
os_status os_event_group_wait_bits(os_event_group_t *group, uint32_t bits, bool wait_all, uint32_t *matched_bits, uint32_t timeout_ms)
{
    uint32_t remaining_ticks;

    if ((group == NULL) || (matched_bits == NULL) || (bits == 0U))
    {
        return OS_STATUS_INVALID_ARG;
    }

    remaining_ticks = os_internal_timeout_to_ticks(timeout_ms);

    for (;;)
    {
        uint32_t current_flags;
        bool     is_match;

        os_critical_enter();

        current_flags = group->flags & bits;
        *matched_bits = current_flags;

        if (wait_all)
        {
            is_match = (current_flags == bits);
        }
        else
        {
            is_match = (current_flags != 0U);
        }

        if (is_match)
        {
            os_critical_exit();
            return OS_STATUS_OK;
        }

        if ((timeout_ms == OS_WAIT_NOTHING) || !os_internal_can_block())
        {
            os_critical_exit();
            return OS_STATUS_BUSY;
        }

        if (remaining_ticks == 0U)
        {
            os_critical_exit();
            return OS_STATUS_TIMEOUT;
        }

        /* Join the waiter list inside the same critical section that saw the
         * bits unmatched (no lost-wakeup window against set_bits). */
        os_task_wait_begin(&group->waiters, remaining_ticks);
        os_critical_exit();

        /* Resumed: set_bits signaled (re-evaluate) or the wait timed out. */
        if (!os_task_wait_signaled())
        {
            return OS_STATUS_TIMEOUT;
        }

        remaining_ticks = os_task_wait_remaining_ticks();
    }
}

#endif /* OS_CONFIG_EVENT_ENABLE */
