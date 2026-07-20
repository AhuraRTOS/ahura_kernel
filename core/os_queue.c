/**
 * @file os_queue.c
 * @brief Queue module implementation with timeouts.
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

#include <string.h>

#if (OS_CONFIG_QUEUE_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize a queue object.
 *
 * @param[in,out] queue      Queue object to initialize.
 * @param[in]     buffer     Backing storage buffer.
 * @param[in]     item_size  Size of one item in bytes.
 * @param[in]     capacity   Number of items buffer can hold.
 * @return os_status    Status code.
 */
os_status os_queue_init(os_queue_t *queue, void *buffer, size_t item_size, size_t capacity)
{
    if ((queue == NULL) || (buffer == NULL) || (item_size == 0U) || (capacity == 0U))
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    /* Re-initializing with queued waiters would strand them on dangling
     * intrusive nodes and corrupt the lists (first-time init must run on
     * zero-initialized storage - static objects are). */
    if ((queue->send_waiters.head != NULL) || (queue->receive_waiters.head != NULL))
    {
        os_critical_exit();
        return OS_STATUS_BUSY;
    }

    queue->buffer    = (uint8_t *)buffer;
    queue->item_size = item_size;
    queue->capacity  = capacity;
    queue->head      = 0U;
    queue->tail      = 0U;
    queue->count     = 0U;
    os_list_init(&queue->send_waiters);
    os_list_init(&queue->receive_waiters);

    os_critical_exit();
    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Send one item into queue, waiting up to timeout_ms when full.
 *
 * The item copy happens inside a critical section: keep item_size small.
 * Nonzero timeouts are only honored from task context after os_start.
 *
 * @param[in,out] queue       Queue object.
 * @param[in]     item        Item data to copy.
 * @param[in]     timeout_ms  OS_WAIT_NOTHING, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_status  OK on send, FULL when no space without waiting,
 *                    TIMEOUT when the wait elapsed.
 */
os_status os_queue_send(os_queue_t *queue, const void *item, uint32_t timeout_ms)
{
    uint32_t budget_ticks;
    uint32_t start_tick;
    uint32_t remaining_ticks;

    if ((queue == NULL) || (item == NULL))
    {
        return OS_STATUS_INVALID_ARG;
    }

    budget_ticks    = os_internal_timeout_to_ticks(timeout_ms);
    start_tick      = os_tick_get();
    remaining_ticks = budget_ticks;

    for (;;)
    {
        os_critical_enter();

        if (queue->count < queue->capacity)
        {
            uint8_t *slot = &queue->buffer[queue->tail * queue->item_size];

            (void)memcpy(slot, item, queue->item_size);
            queue->tail = (queue->tail + 1U) % queue->capacity;
            queue->count++;

            /* An item arrived: release the highest-priority receiver. */
            (void)os_task_waiters_wake_one(&queue->receive_waiters);

            os_critical_exit();
            return OS_STATUS_OK;
        }

        if ((timeout_ms == OS_WAIT_NOTHING) || !os_internal_can_block())
        {
            os_critical_exit();
            return OS_STATUS_FULL;
        }

        if (remaining_ticks == 0U)
        {
            os_critical_exit();
            return OS_STATUS_TIMEOUT;
        }

        /* Join the senders' waiter list inside the same critical section
         * that saw the queue full (no lost-wakeup window). */
        os_task_wait_begin(&queue->send_waiters, remaining_ticks);
        os_critical_exit();

        /* Resumed: a receive freed a slot (retry) or the wait timed out.
         * The budget is recomputed against the wall clock so READY time
         * counts toward the timeout. */
        if (!os_task_wait_signaled())
        {
            return OS_STATUS_TIMEOUT;
        }

        remaining_ticks = os_internal_wait_remaining(budget_ticks, start_tick);
    }
}

/******************************************************************************************************/
/**
 * @brief Receive one item from queue, waiting up to timeout_ms when empty.
 *
 * @param[in,out] queue       Queue object.
 * @param[out]    item_out    Destination buffer.
 * @param[in]     timeout_ms  OS_WAIT_NOTHING, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_status  OK on receive, EMPTY when no items without waiting,
 *                    TIMEOUT when the wait elapsed.
 */
os_status os_queue_receive(os_queue_t *queue, void *item_out, uint32_t timeout_ms)
{
    uint32_t budget_ticks;
    uint32_t start_tick;
    uint32_t remaining_ticks;

    if ((queue == NULL) || (item_out == NULL))
    {
        return OS_STATUS_INVALID_ARG;
    }

    budget_ticks    = os_internal_timeout_to_ticks(timeout_ms);
    start_tick      = os_tick_get();
    remaining_ticks = budget_ticks;

    for (;;)
    {
        os_critical_enter();

        if (queue->count > 0U)
        {
            const uint8_t *slot = &queue->buffer[queue->head * queue->item_size];

            (void)memcpy(item_out, slot, queue->item_size);
            queue->head = (queue->head + 1U) % queue->capacity;
            queue->count--;

            /* A slot freed up: release the highest-priority sender. */
            (void)os_task_waiters_wake_one(&queue->send_waiters);

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

        /* Join the receivers' waiter list inside the same critical section
         * that saw the queue empty (no lost-wakeup window). */
        os_task_wait_begin(&queue->receive_waiters, remaining_ticks);
        os_critical_exit();

        /* Resumed: a send delivered an item (retry) or the wait timed out.
         * The budget is recomputed against the wall clock so READY time
         * counts toward the timeout. */
        if (!os_task_wait_signaled())
        {
            return OS_STATUS_TIMEOUT;
        }

        remaining_ticks = os_internal_wait_remaining(budget_ticks, start_tick);
    }
}

/******************************************************************************************************/
/**
 * @brief Get current queue item count.
 *
 * @param[in] queue  Queue object.
 * @return size_t    Number of items currently stored.
 */
size_t os_queue_count_get(const os_queue_t *queue)
{
    size_t count;

    if (queue == NULL)
    {
        return 0U;
    }

    os_critical_enter();
    count = queue->count;
    os_critical_exit();

    return count;
}

#endif /* OS_CONFIG_QUEUE_ENABLE */
