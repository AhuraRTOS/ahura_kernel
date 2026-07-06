/**
 * @file ahura.h
 * @brief Ahura kernel umbrella public API.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

#ifndef AHURA_H
#define AHURA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ahura_config.h"
#include "os_arch_port.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Types
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Common status code for Ahura kernel APIs.
 */
typedef enum
{
    OS_STATUS_OK          = 0, /**< Operation succeeded.                          */
    OS_STATUS_ERROR       = 1, /**< Generic failure.                              */
    OS_STATUS_INVALID_ARG = 2, /**< A required argument was invalid or NULL.      */
    OS_STATUS_EMPTY       = 3, /**< Object holds no items/tokens.                 */
    OS_STATUS_FULL        = 4, /**< Object cannot accept more items/tokens.       */
    OS_STATUS_BUSY        = 5, /**< Object unavailable without blocking.          */
    OS_STATUS_TIMEOUT     = 6, /**< Wait aborted because the timeout elapsed.     */
    OS_STATUS_NOT_OWNER   = 7, /**< Caller does not own the object.               */

} os_status;

#if (OS_CONFIG_QUEUE_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Queue object.
 */
typedef struct
{
    uint8_t *buffer;
    size_t  item_size;
    size_t  capacity;
    size_t  head;
    size_t  tail;
    size_t  count;

} os_queue_t;
#endif /* OS_CONFIG_QUEUE_ENABLE */

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Mutex object.
 */
typedef struct
{
    bool     locked;   /**< True while held.                                  */
    uint32_t owner_id; /**< Task id of the holder, 0 when free/unknown owner. */

} os_mutex_t;
#endif /* OS_CONFIG_MUTEX_ENABLE */

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Semaphore object.
 */
typedef struct
{
    uint32_t count;
    uint32_t max_count;

} os_semaphore_t;
#endif /* OS_CONFIG_SEMAPHORE_ENABLE */

#if (OS_CONFIG_EVENT_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Event group object.
 */
typedef struct
{
    uint32_t flags;

} os_event_group_t;
#endif /* OS_CONFIG_EVENT_ENABLE */

#if (OS_CONFIG_TIMER_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Timer operating mode.
 */
typedef enum
{
    OS_TIMER_MODE_ONE_SHOT = 0, /**< Fires once, then stops.            */
    OS_TIMER_MODE_PERIODIC = 1, /**< Reloads and fires every period.    */

} os_timer_mode_t;

/******************************************************************************************************/
/**
 * @brief Timer callback signature.
 */
typedef void (*os_timer_callback_t)(void *context);

/******************************************************************************************************/
/**
 * @brief Software timer object.
 */
typedef struct
{
    uint32_t            period_ticks;
    uint32_t            remaining_ticks;
    os_timer_mode_t     mode;
    bool                active;
    bool                expired; /**< Expiry noted by the tick, callback not yet run. */
    os_timer_callback_t callback;
    void                *context;

} os_timer_t;
#endif /* OS_CONFIG_TIMER_ENABLE */

#if (OS_CONFIG_WORK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Work handler signature; context is the pointer given at os_work_init.
 */
typedef void (*os_work_handler_t)(void *context);

/******************************************************************************************************/
/**
 * @brief Deferrable work item, executed by the kernel work task.
 */
typedef struct
{
    os_work_handler_t handler;
    void              *context;
    uint32_t          delay_ticks; /**< Remaining ticks until the item becomes ready. */
    bool              pending;     /**< Submitted, waiting for its delay to elapse.   */
    bool              ready;       /**< Delay elapsed, awaiting execution.            */

} os_work_t;
#endif /* OS_CONFIG_WORK_ENABLE */

#if (OS_CONFIG_MEMORY_POOL_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Fixed-block memory pool object.
 */
typedef struct
{
    uint8_t *buffer;
    uint8_t *in_use;
    size_t  block_size;
    size_t  block_count;

} os_memory_pool_t;
#endif /* OS_CONFIG_MEMORY_POOL_ENABLE */

#if (OS_CONFIG_LIST_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Intrusive list node object.
 */
typedef struct os_list_node
{
    struct os_list_node *next;
    struct os_list_node *prev;

} os_list_node_t;

/******************************************************************************************************/
/**
 * @brief Intrusive list container.
 */
typedef struct
{
    os_list_node_t *head;
    os_list_node_t *tail;

} os_list_t;
#endif /* OS_CONFIG_LIST_ENABLE */

/******************************************************************************************************/
/**
 * @brief Task lifecycle state.
 */
typedef enum
{
    OS_TASK_STATE_INACTIVE = 0, /**< Not created / deleted.                    */
    OS_TASK_STATE_READY,        /**< Runnable, waiting for the CPU.            */
    OS_TASK_STATE_RUNNING,      /**< Currently executing.                      */
    OS_TASK_STATE_BLOCKED,      /**< Waiting for a delay/timeout to expire.    */
    OS_TASK_STATE_SUSPENDED,    /**< Paused until os_task_start is called.     */

} os_task_state_t;

/******************************************************************************************************/
/**
 * @brief Task entry function signature.
 */
typedef void (*os_task_entry_t)(void *context);

/******************************************************************************************************/
/**
 * @brief Public task handle object.
 */
typedef struct
{
    uint32_t id;

} os_task_t;

/******************************************************************************************************/
/**
 * @brief Task creation parameters.
 */
typedef struct
{
    const char      *name;
    os_task_entry_t entry;
    void            *context;
    uint32_t        priority;
    void            *stack_memory;
    size_t          stack_bytes;

} os_task_config_t;

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#if (OS_ARCH_STACK_UNIT_BITS == 8U)
typedef uint8_t os_stack_unit_t;
#elif (OS_ARCH_STACK_UNIT_BITS == 16U)
typedef uint16_t os_stack_unit_t;
#elif (OS_ARCH_STACK_UNIT_BITS == 32U)
typedef uint32_t os_stack_unit_t;
#elif (OS_ARCH_STACK_UNIT_BITS == 64U)
typedef uint64_t os_stack_unit_t;
#else
#error "Unsupported OS_ARCH_STACK_UNIT_BITS value"
#endif

/** Timeout value: wait forever (never time out). */
#define OS_WAIT_FOREVER         0xFFFFFFFFU

/** Timeout value: do not wait, fail immediately when unavailable. */
#define OS_NO_WAIT              0U

/** User task priority range: 0 is idle, OS_CONFIG_MAX_PRIORITY is reserved
 *  for the kernel work/timer service tasks. */
#define OS_TASK_PRIORITY_USER_MIN   1U
#define OS_TASK_PRIORITY_USER_MAX   (OS_CONFIG_MAX_PRIORITY - 1U)

#define OS_TICKS_FROM_S(sec)    ((uint32_t)((uint64_t)(sec) * (uint64_t)OS_CONFIG_TICK_HZ))
#define OS_TICKS_FROM_MS(ms)    ((uint32_t)((((uint64_t)(ms) * (uint64_t)OS_CONFIG_TICK_HZ) + 999ULL) / 1000ULL))
#define OS_TICKS_FROM_US(us)    ((uint32_t)((((uint64_t)(us) * (uint64_t)OS_CONFIG_TICK_HZ) + 999999ULL) / 1000000ULL))

#if defined(__GNUC__)
#define OS_STACK_ALIGNED        __attribute__((aligned(8)))
#else
#define OS_STACK_ALIGNED
#endif

#define OS_TASK_DEFINE(name, stack_units) \
    static os_stack_unit_t name##_STACK[(stack_units)] OS_STACK_ALIGNED; \
    static os_task_t       name##_TASK

#define OS_TASK_CONFIG(name, entry, context, priority) \
    &(os_task_config_t) { \
        #name, \
        (entry), \
        (context), \
        (priority), \
        (void *)(name##_STACK), \
        sizeof(name##_STACK) \
    }

/*
 * ***********************************************************************************************************
 * Public function prototypes
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize kernel subsystems. Call once before any other kernel API.
 */
void os_init(void);

/******************************************************************************************************/
/**
 * @brief Start the scheduler and switch to task context. Does not return.
 */
void os_start(void);

/******************************************************************************************************/
/**
 * @brief Return true once the scheduler has been started.
 */
bool os_kernel_is_running(void);

/******************************************************************************************************/
/**
 * @brief Get the kernel tick counter (wraps at 32 bits).
 */
uint32_t os_tick_get(void);

/******************************************************************************************************/
/**
 * @brief Block the calling task for the requested milliseconds (busy-waits before os_start).
 */
os_status os_delay_ms(uint32_t milliseconds);

/******************************************************************************************************/
/**
 * @brief Busy-wait for the requested microseconds (precise, does not yield).
 */
os_status os_delay_us(uint32_t microseconds);

/******************************************************************************************************/
/**
 * @brief Block the calling task for the requested seconds (busy-waits before os_start).
 */
os_status os_delay_s(uint32_t seconds);

/******************************************************************************************************/
/**
 * @brief Create a task; priority must be OS_TASK_PRIORITY_USER_MIN..OS_TASK_PRIORITY_USER_MAX.
 */
os_status os_task_create(os_task_t *task, const os_task_config_t *config);

/******************************************************************************************************/
/**
 * @brief Start a created task (make it ready to run).
 */
os_status os_task_start(os_task_t *task);

/******************************************************************************************************/
/**
 * @brief Pause a task (NULL means current running task).
 */
os_status os_task_pause(os_task_t *task);

/******************************************************************************************************/
/**
 * @brief Delete a task and release its TCB slot (NULL means current running task).
 */
os_status os_task_delete(os_task_t *task);

/******************************************************************************************************/
/**
 * @brief Yield the processor to another ready task of equal or higher priority.
 */
void os_task_yield(void);

/******************************************************************************************************/
/**
 * @brief Get the current state of a task (NULL means current running task).
 */
os_task_state_t os_task_state_get(const os_task_t *task);

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Get the minimum stack headroom a task has ever had, in bytes (NULL means current task).
 */
os_status os_task_stack_watermark_get(const os_task_t *task, size_t *min_free_bytes);
#endif /* OS_CONFIG_STACK_WATERMARK_ENABLE */

/******************************************************************************************************/
/**
 * @brief Enter a critical section (disables interrupts, supports nesting).
 */
void os_critical_enter(void);

/******************************************************************************************************/
/**
 * @brief Exit a critical section (re-enables interrupts at outermost level).
 */
void os_critical_exit(void);

#if (OS_CONFIG_QUEUE_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Initialize a queue object.
 */
os_status os_queue_init(os_queue_t *queue, void *buffer, size_t item_size, size_t capacity);

/******************************************************************************************************/
/**
 * @brief Send one item into queue, waiting up to timeout_ms when full.
 */
os_status os_queue_send(os_queue_t *queue, const void *item, uint32_t timeout_ms);

/******************************************************************************************************/
/**
 * @brief Receive one item from queue, waiting up to timeout_ms when empty.
 */
os_status os_queue_receive(os_queue_t *queue, void *item_out, uint32_t timeout_ms);

/******************************************************************************************************/
/**
 * @brief Get current queue item count.
 */
size_t os_queue_count_get(const os_queue_t *queue);
#endif /* OS_CONFIG_QUEUE_ENABLE */

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Initialize a mutex object.
 */
os_status os_mutex_init(os_mutex_t *mutex);

/******************************************************************************************************/
/**
 * @brief Acquire a mutex, waiting up to timeout_ms when contended.
 */
os_status os_mutex_lock(os_mutex_t *mutex, uint32_t timeout_ms);

/******************************************************************************************************/
/**
 * @brief Attempt to acquire a mutex without blocking.
 */
os_status os_mutex_try_lock(os_mutex_t *mutex);

/******************************************************************************************************/
/**
 * @brief Release a mutex object (only the owner may unlock).
 */
os_status os_mutex_unlock(os_mutex_t *mutex);
#endif /* OS_CONFIG_MUTEX_ENABLE */

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Initialize a semaphore object.
 */
os_status os_semaphore_init(os_semaphore_t *semaphore, uint32_t initial_count, uint32_t max_count);

/******************************************************************************************************/
/**
 * @brief Give one token to semaphore (ISR-safe, never blocks).
 */
os_status os_semaphore_give(os_semaphore_t *semaphore);

/******************************************************************************************************/
/**
 * @brief Take one token from semaphore, waiting up to timeout_ms when empty.
 */
os_status os_semaphore_take(os_semaphore_t *semaphore, uint32_t timeout_ms);
#endif /* OS_CONFIG_SEMAPHORE_ENABLE */

#if (OS_CONFIG_EVENT_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Initialize an event group object.
 */
os_status os_event_group_init(os_event_group_t *group);

/******************************************************************************************************/
/**
 * @brief Set event bits in the group (ISR-safe).
 */
os_status os_event_group_set_bits(os_event_group_t *group, uint32_t bits);

/******************************************************************************************************/
/**
 * @brief Clear event bits in the group (ISR-safe).
 */
os_status os_event_group_clear_bits(os_event_group_t *group, uint32_t bits);

/******************************************************************************************************/
/**
 * @brief Wait for event bits, waiting up to timeout_ms until they match.
 */
os_status os_event_group_wait_bits(os_event_group_t *group, uint32_t bits, bool wait_all, uint32_t *matched_bits, uint32_t timeout_ms);
#endif /* OS_CONFIG_EVENT_ENABLE */

#if (OS_CONFIG_TIMER_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Initialize a software timer as one-shot or periodic.
 */
os_status os_timer_init(os_timer_t *timer, uint32_t period_ticks, os_timer_mode_t mode, os_timer_callback_t callback, void *context);

/******************************************************************************************************/
/**
 * @brief Start a software timer (callback runs on the kernel timer task).
 */
os_status os_timer_start(os_timer_t *timer);

/******************************************************************************************************/
/**
 * @brief Stop a software timer.
 */
os_status os_timer_stop(os_timer_t *timer);
#endif /* OS_CONFIG_TIMER_ENABLE */

#if (OS_CONFIG_WORK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Initialize a work item with its handler and user-data pointer.
 */
os_status os_work_init(os_work_t *work, os_work_handler_t handler, void *context);

/******************************************************************************************************/
/**
 * @brief Submit work to run after delay_ms on the kernel work task (0 = as soon as possible; ISR-safe).
 */
os_status os_work_submit(os_work_t *work, uint32_t delay_ms);

/******************************************************************************************************/
/**
 * @brief Cancel submitted work that has not started executing yet (ISR-safe).
 */
os_status os_work_cancel(os_work_t *work);

/******************************************************************************************************/
/**
 * @brief Check whether a work item is submitted and not yet executed.
 */
bool os_work_is_pending(const os_work_t *work);
#endif /* OS_CONFIG_WORK_ENABLE */

#if (OS_CONFIG_MEMORY_POOL_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Initialize a fixed-block memory pool.
 */
os_status os_memory_pool_init(os_memory_pool_t *pool, void *buffer, void *usage_map, size_t block_size, size_t block_count);

/******************************************************************************************************/
/**
 * @brief Allocate one block from a memory pool (ISR-safe).
 */
void *os_memory_pool_alloc(os_memory_pool_t *pool);

/******************************************************************************************************/
/**
 * @brief Free one block back to a memory pool (ISR-safe).
 */
os_status os_memory_pool_free(os_memory_pool_t *pool, void *block);
#endif /* OS_CONFIG_MEMORY_POOL_ENABLE */

#if (OS_CONFIG_LIST_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Initialize list container.
 */
void os_list_init(os_list_t *list);

/******************************************************************************************************/
/**
 * @brief Check whether list is empty.
 */
bool os_list_is_empty(const os_list_t *list);

/******************************************************************************************************/
/**
 * @brief Push node at list tail.
 */
void os_list_push_back(os_list_t *list, os_list_node_t *node);

/******************************************************************************************************/
/**
 * @brief Pop one node from list head.
 */
os_list_node_t *os_list_pop_front(os_list_t *list);
#endif /* OS_CONFIG_LIST_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* AHURA_H */
