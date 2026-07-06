/**
 * @file task.c
 * @brief Task subsystem implementation: TCB pool, scheduling, blocking, idle task.
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

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#define OS_TASK_PRIORITY_IDLE            0U
#define OS_TASK_STACK_FILL_BYTE          0xA5U

/*
 * ***********************************************************************************************************
 * Types
 * ***********************************************************************************************************
*/

typedef struct
{
    const char      *name;
    uint8_t         *stack_base;
    uint32_t        *stack_ptr;
    size_t          stack_bytes;
    uint32_t        priority;
    os_task_entry_t entry;
    void            *context;
    uint32_t        id;
    uint32_t        delay_ticks;
    os_task_state_t state;

} os_task_tcb_t;

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static uint8_t                 os_task_idle_stack[OS_CONFIG_MIN_STACK_SIZE] OS_STACK_ALIGNED;
static os_task_tcb_t           os_task_idle_tcb;
static os_task_tcb_t           os_task_table[OS_CONFIG_MAX_TASKS];
static bool                    os_task_in_use[OS_CONFIG_MAX_TASKS];
static uint32_t                os_task_next_id  = 1U;
static uint32_t                os_task_rr_index = 0U;
static os_task_tcb_t *volatile os_task_current  = (os_task_tcb_t *)0;

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static os_status      os_task_create_any(os_task_t *task, const os_task_config_t *config);
#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
static void           os_task_stack_fill(uint8_t *stack_base, size_t stack_bytes);
#endif
static void           os_task_idle_entry(void *context);
static void           os_task_tcb_clear(os_task_tcb_t *tcb);
static os_task_tcb_t *os_task_find_by_id(uint32_t id);
static uint32_t       os_task_find_index_by_id(uint32_t id);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Create a task using the provided configuration.
 *
 * Priority 0 (idle) and OS_CONFIG_MAX_PRIORITY (kernel work/timer service
 * tasks) are reserved: user tasks must use OS_TASK_PRIORITY_USER_MIN to
 * OS_TASK_PRIORITY_USER_MAX.
 *
 * @param[out] task    Output task handle.
 * @param[in]  config  Task creation configuration.
 * @return os_status   Status code.
 */
os_status os_task_create(os_task_t *task, const os_task_config_t *config)
{
    if (config == (const os_task_config_t *)0)
    {
        return OS_STATUS_INVALID_ARG;
    }

    if ((config->priority < OS_TASK_PRIORITY_USER_MIN) || (config->priority > OS_TASK_PRIORITY_USER_MAX))
    {
        return OS_STATUS_INVALID_ARG;
    }

    return os_task_create_any(task, config);
}

/******************************************************************************************************/
/**
 * @brief Create a task without the user priority restriction; kernel use only.
 *
 * @param[out] task    Output task handle.
 * @param[in]  config  Task creation configuration.
 * @return os_status   Status code.
 */
os_status os_task_create_system(os_task_t *task, const os_task_config_t *config)
{
    return os_task_create_any(task, config);
}

/******************************************************************************************************/
/**
 * @brief Start a created task (make it ready to run).
 *
 * @param[in] task  Task handle.
 * @return os_status  Status code.
 */
os_status os_task_start(os_task_t *task)
{
    os_task_tcb_t *tcb;

    if ((task == (os_task_t *)0) || (task->id == 0U))
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    tcb = os_task_find_by_id(task->id);
    if (tcb == (os_task_tcb_t *)0)
    {
        os_critical_exit();
        return OS_STATUS_INVALID_ARG;
    }

    tcb->delay_ticks = 0U;
    tcb->state       = OS_TASK_STATE_READY;

    /* Let the scheduler decide immediately in case the new task outranks the
     * current one. */
    if (os_kernel_is_running())
    {
        OS_ARCH_CONTEXT_SWITCH_REQUEST();
    }

    os_critical_exit();
    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Pause a task (NULL means current running task).
 *
 * @param[in] task  Task handle, or NULL for the calling task.
 * @return os_status  Status code.
 */
os_status os_task_pause(os_task_t *task)
{
    os_task_tcb_t *tcb;
    bool          is_self;

    os_critical_enter();

    if (task == (os_task_t *)0)
    {
        tcb = os_task_current;
    }
    else if (task->id == 0U)
    {
        os_critical_exit();
        return OS_STATUS_INVALID_ARG;
    }
    else
    {
        tcb = os_task_find_by_id(task->id);
    }

    if ((tcb == (os_task_tcb_t *)0) || (tcb == &os_task_idle_tcb))
    {
        os_critical_exit();
        return (tcb == &os_task_idle_tcb) ? OS_STATUS_BUSY : OS_STATUS_INVALID_ARG;
    }

    is_self          = (tcb == os_task_current);
    tcb->delay_ticks = 0U;
    tcb->state       = OS_TASK_STATE_SUSPENDED;

    if (is_self && os_kernel_is_running())
    {
        OS_ARCH_CONTEXT_SWITCH_REQUEST();
    }

    os_critical_exit();
    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Delete a task and release its TCB slot (NULL means current running task).
 *
 * @param[in,out] task  Task handle, or NULL for the calling task.
 * @return os_status    Status code.
 */
os_status os_task_delete(os_task_t *task)
{
    uint32_t      index;
    os_task_tcb_t *tcb;
    bool          is_self;

    os_critical_enter();

    if (task == (os_task_t *)0)
    {
        tcb = os_task_current;
        if ((tcb == (os_task_tcb_t *)0) || (tcb == &os_task_idle_tcb))
        {
            os_critical_exit();
            return (tcb == &os_task_idle_tcb) ? OS_STATUS_BUSY : OS_STATUS_INVALID_ARG;
        }

        index = os_task_find_index_by_id(tcb->id);
    }
    else if (task->id == 0U)
    {
        os_critical_exit();
        return OS_STATUS_INVALID_ARG;
    }
    else
    {
        index = os_task_find_index_by_id(task->id);
    }

    if (index >= OS_CONFIG_MAX_TASKS)
    {
        os_critical_exit();
        return OS_STATUS_INVALID_ARG;
    }

    tcb     = &os_task_table[index];
    is_self = (tcb == os_task_current);

    os_task_in_use[index] = false;
    os_task_tcb_clear(tcb);

    if (task != (os_task_t *)0)
    {
        task->id = 0U;
    }

    if (is_self)
    {
        /* The calling task ceases to exist: drop the current pointer so the
         * switch-out path does not touch the freed TCB, then switch away. */
        os_task_current = (os_task_tcb_t *)0;
        if (os_kernel_is_running())
        {
            OS_ARCH_CONTEXT_SWITCH_REQUEST();
        }
    }

    os_critical_exit();
    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Yield the processor to another ready task of equal or higher priority.
 *
 * @return None.
 */
void os_task_yield(void)
{
    if (os_kernel_is_running() && !os_arch_in_isr())
    {
        OS_ARCH_CONTEXT_SWITCH_REQUEST();
    }
}

/******************************************************************************************************/
/**
 * @brief Get the current state of a task.
 *
 * @param[in] task  Task handle, or NULL for the calling task.
 * @return os_task_state_t  Task state; OS_TASK_STATE_INACTIVE for unknown handles.
 */
os_task_state_t os_task_state_get(const os_task_t *task)
{
    os_task_tcb_t   *tcb;
    os_task_state_t state;

    os_critical_enter();

    if (task == (const os_task_t *)0)
    {
        tcb = os_task_current;
    }
    else if (task->id == 0U)
    {
        tcb = (os_task_tcb_t *)0;
    }
    else
    {
        tcb = os_task_find_by_id(task->id);
    }

    state = (tcb == (os_task_tcb_t *)0) ? OS_TASK_STATE_INACTIVE : tcb->state;

    os_critical_exit();
    return state;
}

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Get the minimum stack headroom a task has ever had, in bytes.
 *
 * Counts the fill-pattern bytes still untouched at the bottom of the stack,
 * i.e. the smallest distance the stack pointer has ever had to the overflow
 * boundary since the task was created.
 *
 * @param[in]  task            Task handle, or NULL for the calling task.
 * @param[out] min_free_bytes  Worst-case remaining stack in bytes.
 * @return os_status  Status code.
 */
os_status os_task_stack_watermark_get(const os_task_t *task, size_t *min_free_bytes)
{
    os_task_tcb_t *tcb;
    uint8_t       *stack_base;
    size_t        stack_bytes;
    size_t        index;

    if (min_free_bytes == (size_t *)0)
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    if (task == (const os_task_t *)0)
    {
        tcb = os_task_current;
    }
    else if (task->id == 0U)
    {
        tcb = (os_task_tcb_t *)0;
    }
    else
    {
        tcb = os_task_find_by_id(task->id);
    }

    if ((tcb == (os_task_tcb_t *)0) || (tcb->stack_base == (uint8_t *)0))
    {
        os_critical_exit();
        return OS_STATUS_INVALID_ARG;
    }

    stack_base  = tcb->stack_base;
    stack_bytes = tcb->stack_bytes;

    os_critical_exit();

    /* The scan itself needs no critical section: bytes below the deepest
     * stack excursion are never rewritten. */
    for (index = 0U; index < stack_bytes; index++)
    {
        if (stack_base[index] != OS_TASK_STACK_FILL_BYTE)
        {
            break;
        }
    }

    *min_free_bytes = index;
    return OS_STATUS_OK;
}
#endif /* OS_CONFIG_STACK_WATERMARK_ENABLE */

/******************************************************************************************************/
/**
 * @brief Update task delays with elapsed kernel ticks; wakes expired tasks.
 *
 * @param[in] elapsed_ticks  Number of elapsed ticks.
 * @return None.
 */
void os_task_tick_update(uint32_t elapsed_ticks)
{
    uint32_t index;

    if (elapsed_ticks == 0U)
    {
        return;
    }

    for (index = 0U; index < OS_CONFIG_MAX_TASKS; index++)
    {
        os_task_tcb_t *tcb = &os_task_table[index];

        if (!os_task_in_use[index] || (tcb->state != OS_TASK_STATE_BLOCKED))
        {
            continue;
        }

        /* OS_WAIT_FOREVER sleepers are only released by os_task_wake. */
        if (tcb->delay_ticks == OS_WAIT_FOREVER)
        {
            continue;
        }

        if (tcb->delay_ticks > elapsed_ticks)
        {
            tcb->delay_ticks -= elapsed_ticks;
        }
        else
        {
            tcb->delay_ticks = 0U;
            tcb->state       = OS_TASK_STATE_READY;
        }
    }
}

/******************************************************************************************************/
/**
 * @brief Block the calling task for the given number of ticks.
 *
 * @param[in] ticks  Number of ticks to sleep; 0 returns immediately;
 *                   OS_WAIT_FOREVER blocks until os_task_wake is called.
 * @return None.
 */
void os_task_sleep_ticks(uint32_t ticks)
{
    if (ticks == 0U)
    {
        return;
    }

    os_critical_enter();

    /* Only a real task in task context can block. */
    if (!os_kernel_is_running() || os_arch_in_isr() ||
        (os_task_current == (os_task_tcb_t *)0) || (os_task_current == &os_task_idle_tcb))
    {
        os_critical_exit();
        return;
    }

    os_task_current->delay_ticks = ticks;
    os_task_current->state       = OS_TASK_STATE_BLOCKED;

    /* The switch is taken as soon as the critical section is left. */
    OS_ARCH_CONTEXT_SWITCH_REQUEST();
    os_critical_exit();
}

/******************************************************************************************************/
/**
 * @brief Wake a BLOCKED task by id; no-op for other states.
 *
 * Safe from interrupt and task context. Used by the kernel service modules
 * to release their OS_WAIT_FOREVER sleepers.
 *
 * @param[in] task_id  Id of the task to wake.
 * @return None.
 */
void os_task_wake(uint32_t task_id)
{
    os_task_tcb_t *tcb;

    if (task_id == 0U)
    {
        return;
    }

    os_critical_enter();

    tcb = os_task_find_by_id(task_id);
    if ((tcb != (os_task_tcb_t *)0) && (tcb->state == OS_TASK_STATE_BLOCKED))
    {
        tcb->delay_ticks = 0U;
        tcb->state       = OS_TASK_STATE_READY;

        if (os_kernel_is_running())
        {
            OS_ARCH_CONTEXT_SWITCH_REQUEST();
        }
    }

    os_critical_exit();
}

/******************************************************************************************************/
/**
 * @brief Get the id of the current task, 0 when idle/none/pre-scheduler.
 *
 * @return uint32_t  Current task id.
 */
uint32_t os_task_current_id_get(void)
{
    os_task_tcb_t *tcb = os_task_current;

    return (tcb == (os_task_tcb_t *)0) ? 0U : tcb->id;
}

/******************************************************************************************************/
/**
 * @brief Terminate the calling task; used when a task entry function returns.
 *
 * @return None.
 */
void os_task_exit(void)
{
    (void)os_task_delete((os_task_t *)0);

    /* Deletion switches away; if it could not (pre-scheduler misuse), park. */
    while (1)
    {
        OS_ARCH_IDLE();
    }
}

/******************************************************************************************************/
/**
 * @brief Create the mandatory idle task.
 *
 * @return os_status  Status code.
 */
os_status os_task_idle_create(void)
{
    uint32_t *stack_ptr;

    if (os_task_idle_tcb.state != OS_TASK_STATE_INACTIVE)
    {
        return OS_STATUS_OK;
    }

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
    os_task_stack_fill(os_task_idle_stack, sizeof(os_task_idle_stack));
#endif

    stack_ptr = os_arch_task_stack_initialize(os_task_idle_stack, sizeof(os_task_idle_stack), os_task_idle_entry, (void *)0);
    if (stack_ptr == (uint32_t *)0)
    {
        return OS_STATUS_ERROR;
    }

    os_task_idle_tcb.name        = "tsk_idle";
    os_task_idle_tcb.stack_base  = os_task_idle_stack;
    os_task_idle_tcb.stack_ptr   = stack_ptr;
    os_task_idle_tcb.stack_bytes = sizeof(os_task_idle_stack);
    os_task_idle_tcb.priority    = OS_TASK_PRIORITY_IDLE;
    os_task_idle_tcb.entry       = os_task_idle_entry;
    os_task_idle_tcb.context     = (void *)0;
    os_task_idle_tcb.id          = 0U;
    os_task_idle_tcb.delay_ticks = 0U;
    os_task_idle_tcb.state       = OS_TASK_STATE_READY;

    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Check whether idle task is already created.
 *
 * @return bool  True when idle task exists.
 */
bool os_task_idle_is_created(void)
{
    return (os_task_idle_tcb.state != OS_TASK_STATE_INACTIVE);
}

/******************************************************************************************************/
/**
 * @brief Initialize task management subsystem.
 *
 * @return None.
 */
void os_task_system_init(void)
{
    uint32_t index;

    for (index = 0U; index < OS_CONFIG_MAX_TASKS; index++)
    {
        os_task_in_use[index] = false;
        os_task_tcb_clear(&os_task_table[index]);
    }

    os_task_next_id  = 1U;
    os_task_rr_index = 0U;
    os_task_current  = (os_task_tcb_t *)0;
    os_task_tcb_clear(&os_task_idle_tcb);
}

/******************************************************************************************************/
/**
 * @brief Save the current running task stack pointer (called from PendSV).
 *
 * @param[in] stack_ptr  Updated process stack pointer.
 * @return None.
 */
void os_task_stack_save_current(uint32_t *stack_ptr)
{
    os_task_tcb_t *current_task = os_task_current;

    if (current_task == (os_task_tcb_t *)0)
    {
        return;
    }

    current_task->stack_ptr = stack_ptr;

    /* Preempted tasks go back to READY; a task that just blocked or suspended
     * itself must keep that state or it would never actually wait. */
    if (current_task->state == OS_TASK_STATE_RUNNING)
    {
        current_task->state = OS_TASK_STATE_READY;
    }
}

/******************************************************************************************************/
/**
 * @brief Select the next task to run and return its stack pointer (called from PendSV/SVC).
 *
 * @return uint32_t*  Stack pointer for the selected task; never NULL (idle fallback).
 */
uint32_t *os_task_stack_select_next(void)
{
    os_task_tcb_t *next          = (os_task_tcb_t *)0;
    bool          found_ready    = false;
    uint32_t      best_priority  = 0U;
    uint32_t      index;
    uint32_t      offset;

    /* Pass 1: find the highest priority among READY tasks. */
    for (index = 0U; index < OS_CONFIG_MAX_TASKS; index++)
    {
        if (os_task_in_use[index] && (os_task_table[index].state == OS_TASK_STATE_READY))
        {
            if (!found_ready || (os_task_table[index].priority > best_priority))
            {
                best_priority = os_task_table[index].priority;
                found_ready   = true;
            }
        }
    }

    /* Pass 2: round-robin among the tasks at that priority. */
    if (found_ready)
    {
        for (offset = 1U; offset <= OS_CONFIG_MAX_TASKS; offset++)
        {
            index = (os_task_rr_index + offset) % OS_CONFIG_MAX_TASKS;

            if (os_task_in_use[index] &&
                (os_task_table[index].state == OS_TASK_STATE_READY) &&
                (os_task_table[index].priority == best_priority))
            {
                next             = &os_task_table[index];
                os_task_rr_index = index;
                break;
            }
        }
    }

    if (next == (os_task_tcb_t *)0)
    {
        next = &os_task_idle_tcb;
    }

    next->state     = OS_TASK_STATE_RUNNING;
    os_task_current = next;

    return next->stack_ptr;
}

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Shared task creation core used by the public and kernel-internal paths.
 *
 * @param[out] task    Output task handle.
 * @param[in]  config  Task creation configuration.
 * @return os_status   Status code.
 */
static os_status os_task_create_any(os_task_t *task, const os_task_config_t *config)
{
    uint32_t  index;
    uintptr_t stack_addr;
    uint32_t  *stack_ptr;

    if ((task == (os_task_t *)0) ||
        (config == (const os_task_config_t *)0) ||
        (config->entry == (os_task_entry_t)0) ||
        (config->priority > OS_CONFIG_MAX_PRIORITY) ||
        (config->stack_memory == (void *)0) ||
        (config->stack_bytes < OS_CONFIG_MIN_STACK_SIZE))
    {
        return OS_STATUS_INVALID_ARG;
    }

    stack_addr = (uintptr_t)config->stack_memory;
    if (((stack_addr % OS_ARCH_STACK_ALIGNMENT_BYTES) != 0U) ||
        ((config->stack_bytes % OS_ARCH_STACK_ALIGNMENT_BYTES) != 0U))
    {
        return OS_STATUS_INVALID_ARG;
    }

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
    /* Pre-fill so os_task_stack_watermark_get can measure peak usage. */
    os_task_stack_fill((uint8_t *)config->stack_memory, config->stack_bytes);
#endif

    /* Build the initial frame before taking the critical section: it only
     * touches the caller's stack memory. */
    stack_ptr = os_arch_task_stack_initialize((uint8_t *)config->stack_memory, config->stack_bytes, config->entry, config->context);
    if (stack_ptr == (uint32_t *)0)
    {
        return OS_STATUS_ERROR;
    }

    os_critical_enter();

    for (index = 0U; index < OS_CONFIG_MAX_TASKS; index++)
    {
        if (!os_task_in_use[index])
        {
            os_task_tcb_t *tcb = &os_task_table[index];

            os_task_in_use[index] = true;
            tcb->name             = config->name;
            tcb->stack_base       = (uint8_t *)config->stack_memory;
            tcb->stack_ptr        = stack_ptr;
            tcb->stack_bytes      = config->stack_bytes;
            tcb->priority         = config->priority;
            tcb->entry            = config->entry;
            tcb->context          = config->context;
            tcb->id               = os_task_next_id;
            tcb->delay_ticks      = 0U;
            tcb->state            = OS_TASK_STATE_SUSPENDED;

            task->id = os_task_next_id;
            os_task_next_id++;

            if (os_task_next_id == 0U)
            {
                os_task_next_id = 1U;
            }

            os_critical_exit();
            return OS_STATUS_OK;
        }
    }

    os_critical_exit();
    return OS_STATUS_FULL;
}

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Fill a stack with the watermark pattern before its first use.
 *
 * @param[in,out] stack_base   Base address of the stack memory.
 * @param[in]     stack_bytes  Size of the stack memory in bytes.
 * @return None.
 */
static void os_task_stack_fill(uint8_t *stack_base, size_t stack_bytes)
{
    size_t index;

    for (index = 0U; index < stack_bytes; index++)
    {
        stack_base[index] = OS_TASK_STACK_FILL_BYTE;
    }
}
#endif /* OS_CONFIG_STACK_WATERMARK_ENABLE */

/******************************************************************************************************/
/**
 * @brief Idle task body: wait for interrupts forever.
 *
 * @param[in] context  Unused.
 * @return None.
 */
static void os_task_idle_entry(void *context)
{
    (void)context;

    while (1)
    {
        OS_ARCH_IDLE();
    }
}

/******************************************************************************************************/
/**
 * @brief Reset a TCB to the inactive state.
 *
 * @param[in,out] tcb  Task control block to clear.
 * @return None.
 */
static void os_task_tcb_clear(os_task_tcb_t *tcb)
{
    tcb->name        = (const char *)0;
    tcb->stack_base  = (uint8_t *)0;
    tcb->stack_ptr   = (uint32_t *)0;
    tcb->stack_bytes = 0U;
    tcb->priority    = 0U;
    tcb->entry       = (os_task_entry_t)0;
    tcb->context     = (void *)0;
    tcb->id          = 0U;
    tcb->delay_ticks = 0U;
    tcb->state       = OS_TASK_STATE_INACTIVE;
}

/******************************************************************************************************/
/**
 * @brief Find a task by its ID.
 *
 * @param[in] id  Task ID.
 * @return os_task_tcb_t*  Pointer to the task control block, or NULL if not found.
 */
static os_task_tcb_t *os_task_find_by_id(uint32_t id)
{
    uint32_t index = os_task_find_index_by_id(id);

    return (index < OS_CONFIG_MAX_TASKS) ? &os_task_table[index] : (os_task_tcb_t *)0;
}

/******************************************************************************************************/
/**
 * @brief Find the index of a task by its ID.
 *
 * @param[in] id  Task ID.
 * @return uint32_t  Index of the task in the task table, or OS_CONFIG_MAX_TASKS if not found.
 */
static uint32_t os_task_find_index_by_id(uint32_t id)
{
    uint32_t index;

    for (index = 0U; index < OS_CONFIG_MAX_TASKS; index++)
    {
        if (os_task_in_use[index] && (os_task_table[index].id == id))
        {
            return index;
        }
    }

    return OS_CONFIG_MAX_TASKS;
}
