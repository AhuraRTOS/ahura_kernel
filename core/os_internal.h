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
 * @brief Initialize the task management subsystem (task.c).
 */
void os_task_system_init(void);

/******************************************************************************************************/
/**
 * @brief Create the mandatory idle task (task.c).
 */
os_status os_task_idle_create(void);

/******************************************************************************************************/
/**
 * @brief Check whether the idle task is already created (task.c).
 */
bool os_task_idle_is_created(void);

/******************************************************************************************************/
/**
 * @brief Update task delays with elapsed kernel ticks; wakes expired tasks (task.c, ISR context).
 */
void os_task_tick_update(uint32_t elapsed_ticks);

/******************************************************************************************************/
/**
 * @brief Create a task without the user priority restriction; kernel use only (task.c).
 */
os_status os_task_create_system(os_task_t *task, const os_task_config_t *config);

/******************************************************************************************************/
/**
 * @brief Block the calling task for ticks; OS_WAIT_FOREVER blocks until os_task_wake (task.c).
 */
void os_task_sleep_ticks(uint32_t ticks);

/******************************************************************************************************/
/**
 * @brief Wake a BLOCKED task by id; no-op for other states (task.c, ISR-safe).
 */
void os_task_wake(uint32_t task_id);

/******************************************************************************************************/
/**
 * @brief Get the id of the current task, 0 when idle/none/pre-scheduler (task.c).
 */
uint32_t os_task_current_id_get(void);

/******************************************************************************************************/
/**
 * @brief Terminate the calling task; used when a task entry function returns (task.c).
 */
void os_task_exit(void);

/******************************************************************************************************/
/**
 * @brief Save the stack pointer of the task being switched out (task.c, called from PendSV).
 */
void os_task_stack_save_current(uint32_t *stack_ptr);

/******************************************************************************************************/
/**
 * @brief Select the next task to run and return its stack pointer; never NULL (task.c, called from PendSV/SVC).
 */
uint32_t *os_task_stack_select_next(void);

/******************************************************************************************************/
/**
 * @brief Initialize the kernel tick source and bookkeeping (tick.c).
 */
void os_tick_init(void);

/******************************************************************************************************/
/**
 * @brief Handle one periodic tick event; call from the tick interrupt (tick.c).
 */
void os_tick_handler(void);

/******************************************************************************************************/
/**
 * @brief Announce multiple elapsed ticks to the kernel time base (tick.c).
 */
void os_tick_announce(uint32_t elapsed_ticks);

/******************************************************************************************************/
/**
 * @brief Create and start the kernel timer service task (timer.c).
 */
os_status os_timer_system_init(void);

/******************************************************************************************************/
/**
 * @brief Advance all registered software timers by elapsed ticks (timer.c, ISR context).
 */
void os_timer_tick_process(uint32_t elapsed_ticks);

/******************************************************************************************************/
/**
 * @brief Create and start the kernel work service task (work.c).
 */
os_status os_work_system_init(void);

/******************************************************************************************************/
/**
 * @brief Advance delayed work items by elapsed ticks (work.c, ISR context).
 */
void os_work_tick_process(uint32_t elapsed_ticks);

/******************************************************************************************************/
/**
 * @brief Return ticks until the next active timer expiry, UINT32_MAX when none (timer.c).
 */
uint32_t os_timer_next_expiry_ticks_get(void);

/*
 * ***********************************************************************************************************
 * Internal helpers
 * ***********************************************************************************************************
*/

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
 * @brief Convert a millisecond timeout to ticks, preserving OS_WAIT_FOREVER.
 */
static inline uint32_t os_internal_timeout_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == OS_WAIT_FOREVER)
    {
        return OS_WAIT_FOREVER;
    }

    return OS_TICKS_FROM_MS(timeout_ms);
}

#ifdef __cplusplus
}
#endif

#endif /* OS_INTERNAL_H */
