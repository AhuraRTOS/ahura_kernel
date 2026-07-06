/**
 * @file queue.c
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

    queue->buffer    = (uint8_t *)buffer;
    queue->item_size = item_size;
    queue->capacity  = capacity;
    queue->head      = 0U;
    queue->tail      = 0U;
    queue->count     = 0U;

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
 * @param[in]     timeout_ms  OS_NO_WAIT, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_status  OK on send, FULL when no space without waiting,
 *                    TIMEOUT when the wait elapsed.
 */
os_status os_queue_send(os_queue_t *queue, const void *item, uint32_t timeout_ms)
{
    uint32_t start_tick;
    uint32_t timeout_ticks;

    if ((queue == NULL) || (item == NULL))
    {
        return OS_STATUS_INVALID_ARG;
    }

    timeout_ticks = os_internal_timeout_to_ticks(timeout_ms);
    start_tick    = os_tick_get();

    for (;;)
    {
        os_critical_enter();

        if (queue->count < queue->capacity)
        {
            uint8_t *slot = &queue->buffer[queue->tail * queue->item_size];

            (void)memcpy(slot, item, queue->item_size);
            queue->tail = (queue->tail + 1U) % queue->capacity;
            queue->count++;

            os_critical_exit();
            return OS_STATUS_OK;
        }

        os_critical_exit();

        if (timeout_ms == OS_NO_WAIT)
        {
            return OS_STATUS_FULL;
        }

        if (!os_internal_can_block())
        {
            return OS_STATUS_FULL;
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
 * @brief Receive one item from queue, waiting up to timeout_ms when empty.
 *
 * @param[in,out] queue       Queue object.
 * @param[out]    item_out    Destination buffer.
 * @param[in]     timeout_ms  OS_NO_WAIT, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_status  OK on receive, EMPTY when no items without waiting,
 *                    TIMEOUT when the wait elapsed.
 */
os_status os_queue_receive(os_queue_t *queue, void *item_out, uint32_t timeout_ms)
{
    uint32_t start_tick;
    uint32_t timeout_ticks;

    if ((queue == NULL) || (item_out == NULL))
    {
        return OS_STATUS_INVALID_ARG;
    }

    timeout_ticks = os_internal_timeout_to_ticks(timeout_ms);
    start_tick    = os_tick_get();

    for (;;)
    {
        os_critical_enter();

        if (queue->count > 0U)
        {
            const uint8_t *slot = &queue->buffer[queue->head * queue->item_size];

            (void)memcpy(item_out, slot, queue->item_size);
            queue->head = (queue->head + 1U) % queue->capacity;
            queue->count--;

            os_critical_exit();
            return OS_STATUS_OK;
        }

        os_critical_exit();

        if (timeout_ms == OS_NO_WAIT)
        {
            return OS_STATUS_EMPTY;
        }

        if (!os_internal_can_block())
        {
            return OS_STATUS_EMPTY;
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
