/**
 * @file os_mutex.c
 * @brief Mutex module implementation with owner tracking and timeouts.
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

#if (OS_CONFIG_MUTEX_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize a mutex object.
 *
 * Re-initializing a mutex that still has queued waiters is refused: resetting
 * the waiter list would strand the queued tasks on dangling intrusive nodes
 * and corrupt the list. (The check reads the object's current memory, so
 * first-time init must run on zero-initialized storage - static objects are.)
 *
 * @param[in,out] mutex  Mutex object.
 * @return os_status  OK, or BUSY while tasks are waiting on it.
 */
os_status os_mutex_init(os_mutex_t *mutex)
{
    if (mutex == NULL)
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    if (mutex->waiters.head != NULL)
    {
        os_critical_exit();
        return OS_STATUS_BUSY;
    }

    mutex->locked   = false;
    mutex->owner_id = 0U;
    os_list_init(&mutex->waiters);
    mutex->owner_node.next = NULL;
    mutex->owner_node.prev = NULL;

    os_critical_exit();
    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Acquire a mutex, waiting up to timeout_ms when contended.
 *
 * A mutex is an ownership object, so it is task-only: calls from interrupt
 * context are rejected (an ISR has no identity of its own - it would
 * silently borrow the identity of whichever task it interrupted). The
 * mutex is not recursive: locking a mutex the caller already holds fails
 * with OS_STATUS_BUSY instead of deadlocking.
 *
 * @param[in,out] mutex       Mutex object.
 * @param[in]     timeout_ms  OS_WAIT_NOTHING, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_status  OK on acquisition, BUSY when unavailable without waiting,
 *                    TIMEOUT when the wait elapsed, INVALID_ARG from an ISR.
 */
os_status os_mutex_lock(os_mutex_t *mutex, uint32_t timeout_ms)
{
    uint32_t self_id;
    uint32_t budget_ticks;
    uint32_t start_tick;
    uint32_t remaining_ticks;

    if ((mutex == NULL) || os_arch_in_isr())
    {
        return OS_STATUS_INVALID_ARG;
    }

    self_id         = os_task_current_id_get();
    budget_ticks    = os_internal_timeout_to_ticks(timeout_ms);
    start_tick      = os_tick_get();
    remaining_ticks = budget_ticks;

    for (;;)
    {
        bool held_by_self;

        os_critical_enter();

        if (!mutex->locked)
        {
            mutex->locked   = true;
            mutex->owner_id = self_id;
            os_task_mutex_owner_link(&mutex->owner_node);
            os_critical_exit();
            return OS_STATUS_OK;
        }

        held_by_self = ((self_id != 0U) && (mutex->owner_id == self_id));

        /* Recursive lock attempt would deadlock forever: fail fast. */
        if (held_by_self || (timeout_ms == OS_WAIT_NOTHING) || !os_internal_can_block())
        {
            os_critical_exit();
            return OS_STATUS_BUSY;
        }

        if (remaining_ticks == 0U)
        {
            os_critical_exit();
            return OS_STATUS_TIMEOUT;
        }

        /* Boost the owner before blocking: closes the priority-inversion
         * window instead of leaving it open until the owner's next unlock. */
        os_task_mutex_priority_inherit(mutex->owner_id);

        /* Join the waiter list inside the same critical section that saw the
         * mutex locked (no lost-wakeup window); the switch happens on exit. */
        os_task_wait_begin(&mutex->waiters, remaining_ticks);
        os_critical_exit();

        /* Resumed: unlock signaled us (retry the take - another task may
         * have been faster) or the wait timed out. The budget is recomputed
         * against the wall clock so READY time counts toward the timeout. */
        if (!os_task_wait_signaled())
        {
            return OS_STATUS_TIMEOUT;
        }

        remaining_ticks = os_internal_wait_remaining(budget_ticks, start_tick);
    }
}

/******************************************************************************************************/
/**
 * @brief Attempt to acquire a mutex without blocking.
 *
 * @param[in,out] mutex  Mutex object.
 * @return os_status Status code.
 */
os_status os_mutex_try_lock(os_mutex_t *mutex)
{
    return os_mutex_lock(mutex, OS_WAIT_NOTHING);
}

/******************************************************************************************************/
/**
 * @brief Release a mutex object (only the owner may unlock; task-only, like os_mutex_lock).
 *
 * @param[in,out] mutex  Mutex object.
 * @return os_status  OK on release, ERROR when not locked,
 *                    NOT_OWNER when held by another task, INVALID_ARG from an ISR.
 */
os_status os_mutex_unlock(os_mutex_t *mutex)
{
    uint32_t self_id;

    if ((mutex == NULL) || os_arch_in_isr())
    {
        return OS_STATUS_INVALID_ARG;
    }

    self_id = os_task_current_id_get();

    os_critical_enter();

    if (!mutex->locked)
    {
        os_critical_exit();
        return OS_STATUS_ERROR;
    }

    /* Enforce ownership when both sides are identifiable tasks. */
    if ((mutex->owner_id != 0U) && (self_id != 0U) && (mutex->owner_id != self_id))
    {
        os_critical_exit();
        return OS_STATUS_NOT_OWNER;
    }

    mutex->locked   = false;
    mutex->owner_id = 0U;

    /* Drop any boost owed to this mutex before waking the next waiter, so
     * the wake's own preempt check compares against the correct priority. */
    os_task_mutex_owner_unlink_and_reprioritize(&mutex->owner_node);

    /* Hand the release to the highest-priority waiter (it re-takes in its
     * own context; no ownership transfer inside the unlock). */
    (void)os_task_waiters_wake_one(&mutex->waiters);

    os_critical_exit();
    return OS_STATUS_OK;
}

#endif /* OS_CONFIG_MUTEX_ENABLE */
