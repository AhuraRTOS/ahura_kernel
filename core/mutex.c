/**
 * @file mutex.c
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
 * @param[in,out] mutex  Mutex object.
 * @return os_status Status code.
 */
os_status os_mutex_init(os_mutex_t *mutex)
{
    if (mutex == NULL)
    {
        return OS_STATUS_INVALID_ARG;
    }

    mutex->locked   = false;
    mutex->owner_id = 0U;

    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Acquire a mutex, waiting up to timeout_ms when contended.
 *
 * Must not be called with a nonzero timeout from interrupt context. The
 * mutex is not recursive: locking a mutex the caller already holds fails
 * with OS_STATUS_BUSY instead of deadlocking.
 *
 * @param[in,out] mutex       Mutex object.
 * @param[in]     timeout_ms  OS_NO_WAIT, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_status  OK on acquisition, BUSY when unavailable without waiting,
 *                    TIMEOUT when the wait elapsed.
 */
os_status os_mutex_lock(os_mutex_t *mutex, uint32_t timeout_ms)
{
    uint32_t self_id;
    uint32_t start_tick;
    uint32_t timeout_ticks;

    if (mutex == NULL)
    {
        return OS_STATUS_INVALID_ARG;
    }

    self_id       = os_task_current_id_get();
    timeout_ticks = os_internal_timeout_to_ticks(timeout_ms);
    start_tick    = os_tick_get();

    for (;;)
    {
        bool held_by_self;

        os_critical_enter();

        if (!mutex->locked)
        {
            mutex->locked   = true;
            mutex->owner_id = self_id;
            os_critical_exit();
            return OS_STATUS_OK;
        }

        held_by_self = ((self_id != 0U) && (mutex->owner_id == self_id));
        os_critical_exit();

        /* Recursive lock attempt would deadlock forever: fail fast. */
        if (held_by_self)
        {
            return OS_STATUS_BUSY;
        }

        if (timeout_ms == OS_NO_WAIT)
        {
            return OS_STATUS_BUSY;
        }

        /* Waiting is only possible from a running task. */
        if (!os_internal_can_block())
        {
            return OS_STATUS_BUSY;
        }

        if ((timeout_ms != OS_WAIT_FOREVER) &&
            ((uint32_t)(os_tick_get() - start_tick) >= timeout_ticks))
        {
            return OS_STATUS_TIMEOUT;
        }

        /* Sleep one tick and retry; wait queues may replace this later. */
        os_task_sleep_ticks(1U);
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
    return os_mutex_lock(mutex, OS_NO_WAIT);
}

/******************************************************************************************************/
/**
 * @brief Release a mutex object (only the owner may unlock).
 *
 * @param[in,out] mutex  Mutex object.
 * @return os_status  OK on release, ERROR when not locked,
 *                    NOT_OWNER when held by another task.
 */
os_status os_mutex_unlock(os_mutex_t *mutex)
{
    uint32_t self_id;

    if (mutex == NULL)
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

    os_critical_exit();
    return OS_STATUS_OK;
}

#endif /* OS_CONFIG_MUTEX_ENABLE */
