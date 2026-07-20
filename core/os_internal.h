/**
 * @file os_internal.h
 * @brief Ahura kernel internal (cross-module) interfaces. Not part of the public API.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

#ifndef OS_INTERNAL_H
#define OS_INTERNAL_H

#include "../ahura.h"
#include "os_arch_port.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Internal function prototypes
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize the task management subsystem (os_task.c).
 */
void os_task_system_init(void);

/******************************************************************************************************/
/**
 * @brief Create the mandatory idle task (os_task.c).
 */
os_status os_task_idle_create(void);

/******************************************************************************************************/
/**
 * @brief Check whether the idle task is already created (os_task.c).
 */
bool os_task_idle_is_created(void);

/******************************************************************************************************/
/**
 * @brief Update task delays with elapsed kernel ticks; wakes expired tasks (os_task.c, ISR context).
 */
void os_task_tick_update(uint32_t elapsed_ticks);

/******************************************************************************************************/
/**
 * @brief Create a task without the user priority restriction; kernel use only (os_task.c).
 */
os_status os_task_create_system(os_task_t *task, const os_task_config_t *config);

/******************************************************************************************************/
/**
 * @brief Block the calling task for ticks; OS_WAIT_FOREVER blocks until os_task_wake (os_task.c).
 */
void os_task_sleep_ticks(uint32_t ticks);

/******************************************************************************************************/
/**
 * @brief Wake a BLOCKED task by id; no-op for other states (os_task.c, ISR-safe).
 */
void os_task_wake(uint32_t task_id);

/******************************************************************************************************/
/**
 * @brief Resolve a task id to an opaque handle once, for os_task_wake_tcb (os_task.c, ISR-safe).
 */
void* os_task_tcb_resolve(uint32_t task_id);

/******************************************************************************************************/
/**
 * @brief Wake a BLOCKED task by its os_task_tcb_resolve handle, skipping the id lookup and the
 *        nested critical section os_task_wake pays - caller must already hold the kernel mask
 *        (and, on multi-core builds, os_critical_multicore_lock) (os_task.c, ISR-safe).
 */
void os_task_wake_tcb(void *tcb_handle);

/******************************************************************************************************/
/**
 * @brief Queue the calling task on an object's waiter list and block it; call inside a critical
 *        section from task context, the switch happens when the caller exits it (os_task.c).
 */
void os_task_wait_begin(os_list_t *waiters, uint32_t timeout_ticks);

/******************************************************************************************************/
/**
 * @brief After resuming from a wait: true = object signaled, false = timeout (os_task.c).
 */
bool os_task_wait_signaled(void);

/******************************************************************************************************/
/**
 * @brief Attach two words of per-wait data to the calling task before os_task_wait_begin;
 *        a waker's match callback reads them to evaluate the waiter's condition (os_task.c).
 */
void os_task_wait_data_set(uint32_t data0, uint32_t data1);

/******************************************************************************************************/
/**
 * @brief After a signaled resume: the result value the waker stored for this task via
 *        os_task_waiters_wake_match, 0 when woken any other way (os_task.c).
 */
uint32_t os_task_wait_result_get(void);

/******************************************************************************************************/
/**
 * @brief Waker-side condition callback for os_task_waiters_wake_match: receives one waiter's
 *        wait data; returns true to wake that waiter and stores its delivery in *result_out.
 */
typedef bool (*os_task_wait_match_fn)(uint32_t data0, uint32_t data1, void *context, uint32_t *result_out);

/******************************************************************************************************/
/**
 * @brief Wake every waiter whose condition the callback confirms, storing each waiter's result
 *        for os_task_wait_result_get. Call inside a critical section; ISR-safe (os_task.c).
 *
 * @return uint32_t  Number of waiters woken.
 */
uint32_t os_task_waiters_wake_match(os_list_t *waiters, os_task_wait_match_fn match, void *context);

/******************************************************************************************************/
/**
 * @brief Wake the highest-priority waiter of an object; true when one was woken
 *        (os_task.c, call inside a critical section, ISR-safe).
 */
bool os_task_waiters_wake_one(os_list_t *waiters);

/******************************************************************************************************/
/**
 * @brief Wake every waiter of an object (os_task.c, call inside a critical section, ISR-safe).
 */
void os_task_waiters_wake_all(os_list_t *waiters);

/******************************************************************************************************/
/**
 * @brief Get the id of the current task, 0 when idle/none/pre-scheduler (os_task.c).
 */
uint32_t os_task_current_id_get(void);

/******************************************************************************************************/
/**
 * @brief Check whether the idle task is currently running (os_task.c, ISR-safe).
 */
bool os_task_current_is_idle(void);

/******************************************************************************************************/
/**
 * @brief Whether a PendSV on this core would actually switch or round-robin (os_task.c,
 *        ISR-safe). Lets the tick handler skip a pointless PendSV round trip.
 */
bool os_task_reschedule_possible(void);

/******************************************************************************************************/
/**
 * @brief Terminate the calling task; used when a task entry function returns (os_task.c).
 */
void os_task_exit(void);

/******************************************************************************************************/
/**
 * @brief Save the stack pointer of the task being switched out (os_task.c, called from PendSV).
 */
void os_task_stack_save_current(uint32_t *stack_ptr);

/******************************************************************************************************/
/**
 * @brief Select the next task to run and return its stack pointer; never NULL (os_task.c, called from PendSV/SVC).
 */
uint32_t* os_task_stack_select_next(void);

/******************************************************************************************************/
/**
 * @brief Initialize the kernel tick source and bookkeeping (os_tick.c).
 */
void os_tick_init(void);

/******************************************************************************************************/
/**
 * @brief Handle one periodic tick event; call from the tick interrupt (os_tick.c).
 */
void os_tick_handler(void);

/******************************************************************************************************/
/**
 * @brief Announce multiple elapsed ticks to the kernel time base (os_tick.c).
 */
void os_tick_announce(uint32_t elapsed_ticks);

/******************************************************************************************************/
/**
 * @brief Create and start the kernel timer service task (os_timer.c).
 */
os_status os_timer_system_init(void);

/******************************************************************************************************/
/**
 * @brief Advance all registered software timers by elapsed ticks (os_timer.c, ISR context).
 */
void os_timer_tick_process(uint32_t elapsed_ticks);

/******************************************************************************************************/
/**
 * @brief Create and start the kernel work service task (os_work.c).
 */
os_status os_work_system_init(void);

/******************************************************************************************************/
/**
 * @brief Advance delayed work items by elapsed ticks (os_work.c, ISR context).
 */
void os_work_tick_process(uint32_t elapsed_ticks);

/******************************************************************************************************/
/**
 * @brief Return ticks until the next active timer expiry, UINT32_MAX when none (os_timer.c).
 */
uint32_t os_timer_next_expiry_ticks_get(void);

/******************************************************************************************************/
/**
 * @brief Return ticks until the next delayed work item becomes ready, 0 when one is already
 *        ready, UINT32_MAX when none (os_work.c).
 */
uint32_t os_work_next_ready_ticks_get(void);

/******************************************************************************************************/
/**
 * @brief Return ticks until the next finite-delay sleeper wakes, UINT32_MAX when none (os_task.c).
 */
uint32_t os_task_next_delay_ticks_get(void);

/*
 * ***********************************************************************************************************
 * Internal helpers
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief Acquire the cross-core kernel spinlock only (os_critical.c); caller manages its own
 *        local kernel mask directly. See os_critical_multicore_lock for the deadlock rule.
 */
void os_critical_multicore_lock(void);

/******************************************************************************************************/
/**
 * @brief Release the cross-core kernel spinlock acquired by os_critical_multicore_lock.
 */
void os_critical_multicore_unlock(void);
#else
/* No cross-core exclusion needed on single-core builds: the local kernel mask
 * (already raised by every caller) is already sufficient by itself. */
static inline void os_critical_multicore_lock(void)   { }
static inline void os_critical_multicore_unlock(void) { }
#endif

/******************************************************************************************************/
/**
 * @brief Check whether the caller is allowed to block (task context, scheduler running).
 */
static inline bool os_internal_can_block(void)
{
    return (os_kernel_is_running() && !os_arch_in_isr());
}

/******************************************************************************************************/
/**
 * @brief Convert a millisecond timeout to ticks, preserving OS_WAIT_FOREVER and saturating
 *        huge finite values one tick short of the sentinel (a finite request must never
 *        silently become "wait forever").
 */
static inline uint32_t os_internal_timeout_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == OS_WAIT_FOREVER)
    {
        return OS_WAIT_FOREVER;
    }

#if (OS_CONFIG_TICK_HZ == 1000U)
    /* 1 ms = 1 tick: identity, no 64-bit math on the hot path. */
    return timeout_ms;
#else
    {
        uint64_t ticks = (((uint64_t)timeout_ms * (uint64_t)OS_CONFIG_TICK_HZ) + 999ULL) / 1000ULL;

        if (ticks >= (uint64_t)OS_WAIT_FOREVER)
        {
            return OS_WAIT_FOREVER - 1U;
        }

        return (uint32_t)ticks;
    }
#endif
}

/******************************************************************************************************/
/**
 * @brief Remaining wait budget measured against the wall clock: budget minus ticks elapsed
 *        since start_tick (wrap-safe), never below 0, OS_WAIT_FOREVER passed through.
 *
 * Blocking primitives recompute their budget with this after every spurious wake, so time
 * spent READY (preempted between wake and re-check) counts against the timeout - a relative
 * re-arm would freeze the clock and stretch timeouts unboundedly under wake traffic.
 */
static inline uint32_t os_internal_wait_remaining(uint32_t budget_ticks, uint32_t start_tick)
{
    uint32_t elapsed;

    if (budget_ticks == OS_WAIT_FOREVER)
    {
        return OS_WAIT_FOREVER;
    }

    elapsed = os_tick_get() - start_tick;

    return (elapsed >= budget_ticks) ? 0U : (budget_ticks - elapsed);
}

#ifdef __cplusplus
}
#endif

#endif /* OS_INTERNAL_H */
