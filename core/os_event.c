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
 * Macros
 * ***********************************************************************************************************
*/

/* Waiter condition encoding in the TCB wait data: data0 = requested bits,
 * data1 = these mode flags. */
#define OS_EVENT_WAIT_ALL_FLAG        (1UL << 0)
#define OS_EVENT_CLEAR_ON_EXIT_FLAG   (1UL << 1)

/*
 * ***********************************************************************************************************
 * Types
 * ***********************************************************************************************************
*/

/* Context handed through os_task_waiters_wake_match during a set_bits walk. */
typedef struct
{
    uint32_t flags_snapshot; /* group flags every waiter is evaluated against  */
    uint32_t clear_accum;    /* bits consumed by satisfied clear-on-exit waiters */

} os_event_match_context_t;

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static bool os_event_waiter_match(uint32_t data0, uint32_t data1, void *context, uint32_t *result_out);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize an event group object.
 *
 * Re-initializing a group that still has queued waiters is refused: resetting
 * the waiter list would strand the queued tasks on dangling intrusive nodes
 * and corrupt the list. (First-time init must run on zero-initialized
 * storage - static objects are.)
 *
 * @param[in,out] group  Event group object.
 * @return os_status  OK, or BUSY while tasks are waiting on it.
 */
os_status os_event_group_init(os_event_group_t *group)
{
    if (group == NULL)
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    if (group->waiters.head != NULL)
    {
        os_critical_exit();
        return OS_STATUS_BUSY;
    }

    group->flags = 0U;
    os_list_init(&group->waiters);

    os_critical_exit();
    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Set event bits in the group (ISR-safe).
 *
 * Every waiter's condition is evaluated HERE, against one snapshot of the
 * flags, and satisfied waiters receive their matched bits atomically with
 * the set - a later clear cannot revoke a delivery, and only satisfied
 * waiters are woken (no thundering herd). Bits requested by satisfied
 * clear-on-exit waiters are cleared after the walk, so several waiters
 * satisfied by the same set all get delivery.
 *
 * @param[in,out] group  Event group object.
 * @param[in]     bits   Bits to set.
 * @return os_status Status code.
 */
os_status os_event_group_set_bits(os_event_group_t *group, uint32_t bits)
{
    os_event_match_context_t match_context;

    if (group == NULL)
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    group->flags |= bits;

    match_context.flags_snapshot = group->flags;
    match_context.clear_accum    = 0U;

    (void)os_task_waiters_wake_match(&group->waiters, os_event_waiter_match, &match_context);

    group->flags &= ~match_context.clear_accum;

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
 * With clear_on_exit true the matched consumption is ATOMIC: an immediate
 * match clears the requested bits inside the same critical section that
 * observed them, and a match delivered by set_bits is cleared by the setter
 * itself - a set landing between wait-return and a separate manual clear can
 * no longer be lost. Nonzero timeouts are only honored from task context
 * after os_start.
 *
 * @param[in]  group          Event group object.
 * @param[in]  bits           Bits to wait for.
 * @param[in]  wait_all       True to require all bits, false for any bit.
 * @param[in]  clear_on_exit  True to consume (clear) the requested bits on a match.
 * @param[out] matched_bits   Matched bits snapshot (also written on failure).
 * @param[in]  timeout_ms     OS_WAIT_NOTHING, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_status  OK on match, BUSY when unmatched without waiting,
 *                    TIMEOUT when the wait elapsed.
 */
os_status os_event_group_wait_bits(os_event_group_t *group, uint32_t bits, bool wait_all, bool clear_on_exit, uint32_t *matched_bits, uint32_t timeout_ms)
{
    uint32_t budget_ticks;
    uint32_t start_tick;
    uint32_t remaining_ticks;
    uint32_t wait_flags;

    if ((group == NULL) || (matched_bits == NULL) || (bits == 0U))
    {
        return OS_STATUS_INVALID_ARG;
    }

    budget_ticks    = os_internal_timeout_to_ticks(timeout_ms);
    start_tick      = os_tick_get();
    remaining_ticks = budget_ticks;

    wait_flags = (wait_all ? OS_EVENT_WAIT_ALL_FLAG : 0U) |
                 (clear_on_exit ? OS_EVENT_CLEAR_ON_EXIT_FLAG : 0U);

    for (;;)
    {
        uint32_t current_flags;
        uint32_t delivered;
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
            if (clear_on_exit)
            {
                group->flags &= ~bits;
            }

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

        /* Publish the condition for set_bits to evaluate, then join the
         * waiter list inside the same critical section that saw the bits
         * unmatched (no lost-wakeup window against set_bits). */
        os_task_wait_data_set(bits, wait_flags);
        os_task_wait_begin(&group->waiters, remaining_ticks);
        os_critical_exit();

        if (!os_task_wait_signaled())
        {
            return OS_STATUS_TIMEOUT;
        }

        /* A nonzero result is the delivery set_bits captured for us at set
         * time (already cleared there when clear_on_exit): return it as-is.
         * Zero means a forced/spurious wake: re-evaluate with the budget
         * recomputed against the wall clock. */
        delivered = os_task_wait_result_get();

        if (delivered != 0U)
        {
            *matched_bits = delivered;
            return OS_STATUS_OK;
        }

        remaining_ticks = os_internal_wait_remaining(budget_ticks, start_tick);
    }
}

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Waker-side condition evaluation for one waiter, called by set_bits through
 *        os_task_waiters_wake_match against a single flags snapshot.
 *
 * @param[in]  data0       Waiter's requested bits.
 * @param[in]  data1       Waiter's mode flags (OS_EVENT_WAIT_ALL/CLEAR_ON_EXIT).
 * @param[in]  context     os_event_match_context_t of this set_bits call.
 * @param[out] result_out  Delivery for the waiter: its matched bits.
 * @return bool  True when the waiter's condition is satisfied (wake it).
 */
static bool os_event_waiter_match(uint32_t data0, uint32_t data1, void *context, uint32_t *result_out)
{
    os_event_match_context_t *match_context = (os_event_match_context_t *)context;
    uint32_t                 matched        = match_context->flags_snapshot & data0;
    bool                     satisfied;

    if ((data1 & OS_EVENT_WAIT_ALL_FLAG) != 0U)
    {
        satisfied = (matched == data0);
    }
    else
    {
        satisfied = (matched != 0U);
    }

    if (satisfied)
    {
        *result_out = matched;

        if ((data1 & OS_EVENT_CLEAR_ON_EXIT_FLAG) != 0U)
        {
            match_context->clear_accum |= data0;
        }
    }

    return satisfied;
}

#endif /* OS_CONFIG_EVENT_ENABLE */
