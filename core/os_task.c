/**
 * @file os_task.c
 * @brief Task subsystem implementation: static TCB pool, O(1) list-based
 *        scheduling (per-priority ready lists + ready bitmap, round-robin by
 *        list rotation), delay list, blocking, idle task.
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

#if (OS_CONFIG_MAX_PRIORITY > 31U)
#error "OS_CONFIG_MAX_PRIORITY must be at most 31 (the ready bitmap is 32 bits wide)."
#endif

/* TCB back-references from the embedded intrusive list nodes. */
#define OS_TASK_TCB_FROM_NODE(node)      ((os_task_tcb_t *)(void *)((uint8_t *)(node) - offsetof(os_task_tcb_t, state_node)))
#define OS_TASK_TCB_FROM_WAIT_NODE(node) ((os_task_tcb_t *)(void *)((uint8_t *)(node) - offsetof(os_task_tcb_t, wait_node)))

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
    uint32_t        core_affinity; /* bitmask of cores the task may run on, 0 = any */
    os_task_state_t state;
    os_list_node_t  state_node;    /* links into one ready list or the delay list  */
    os_list_node_t  wait_node;     /* links into one object's waiter list          */
    os_list_t       *wait_list;    /* joined waiter list, NULL when waiting on none */
    bool            wait_signaled; /* wakeup reason: object signal vs timeout      */

} os_task_tcb_t;

/* The current-task pointer is written by PendSV and read from task/ISR
 * context: the pointer itself is the shared object, so the typedef lets
 * __IO qualify it rather than the pointed-to TCB. */
typedef os_task_tcb_t *os_task_tcb_ptr_t;

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

/* Every scheduling core owns one idle task and one current-task slot; the
 * task table and the ready/delay lists are shared between the cores (the
 * critical sections and the explicit PRIMASK guards protect them, plus the
 * kernel spinlock on multi-core builds). */
static uint8_t                 os_task_idle_stack[OS_CONFIG_CORE_COUNT][OS_CONFIG_MIN_STACK_SIZE] OS_STACK_ALIGNED;
static os_task_tcb_t           os_task_idle_tcb[OS_CONFIG_CORE_COUNT];
static os_task_tcb_t           os_task_table[OS_CONFIG_MAX_TASKS];
static bool                    os_task_in_use[OS_CONFIG_MAX_TASKS];
static uint32_t                os_task_next_id = 1U;
static __IO os_task_tcb_ptr_t  os_task_current[OS_CONFIG_CORE_COUNT];

/* Scheduler structures: one FIFO ready list per priority plus a bitmap of
 * non-empty priorities (bit n = priority n has ready tasks), and one list of
 * finite-delay sleepers. OS_WAIT_FOREVER sleepers and suspended tasks sit in
 * no list; the running tasks and the idle tasks are never queued. */
static os_list_t               os_task_ready_list[OS_CONFIG_MAX_PRIORITY + 1U];
static uint32_t                os_task_ready_bitmap = 0U;
static os_list_t               os_task_delay_list;

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
static os_task_tcb_t* os_task_find_by_id(uint32_t id);
static uint32_t       os_task_find_index_by_id(uint32_t id);
static void           os_task_make_ready(os_task_tcb_t *tcb);
static void           os_task_unlink(os_task_tcb_t *tcb);
static void           os_task_preempt_request(const os_task_tcb_t *tcb);
static uint32_t       os_task_running_core(const os_task_tcb_t *tcb);

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

    os_task_unlink(tcb);
    tcb->delay_ticks = 0U;

    /* Starting a running task keeps it running; anything else queues at the
     * tail of its priority's ready list. */
    if (os_task_running_core(tcb) < OS_CONFIG_CORE_COUNT)
    {
        tcb->state = OS_TASK_STATE_RUNNING;
    }
    else
    {
        os_task_make_ready(tcb);
    }

    /* Let the scheduler decide immediately in case the new task outranks the
     * current one (on this core, or via IPI on a core its affinity allows). */
    if (os_kernel_is_running())
    {
        os_task_preempt_request(tcb);
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
    uint32_t      core = os_arch_core_id_get();
    os_task_tcb_t *tcb;
    bool          is_self;

    os_critical_enter();

    if (task == (os_task_t *)0)
    {
        tcb = os_task_current[core];
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

    if ((tcb == (os_task_tcb_t *)0) || (tcb == &os_task_idle_tcb[core]))
    {
        os_critical_exit();
        return (tcb == &os_task_idle_tcb[core]) ? OS_STATUS_BUSY : OS_STATUS_INVALID_ARG;
    }

    is_self = (tcb == os_task_current[core]);

    /* A task executing on another core cannot be paused from here: its
     * context is live over there. */
    if (!is_self && (os_task_running_core(tcb) < OS_CONFIG_CORE_COUNT))
    {
        os_critical_exit();
        return OS_STATUS_BUSY;
    }

    os_task_unlink(tcb);
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
    uint32_t      core = os_arch_core_id_get();
    uint32_t      index;
    os_task_tcb_t *tcb;
    bool          is_self;

    os_critical_enter();

    if (task == (os_task_t *)0)
    {
        tcb = os_task_current[core];
        if ((tcb == (os_task_tcb_t *)0) || (tcb == &os_task_idle_tcb[core]))
        {
            os_critical_exit();
            return (tcb == &os_task_idle_tcb[core]) ? OS_STATUS_BUSY : OS_STATUS_INVALID_ARG;
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
    is_self = (tcb == os_task_current[core]);

    /* A task executing on another core cannot be deleted from here: its
     * context is live over there. */
    if (!is_self && (os_task_running_core(tcb) < OS_CONFIG_CORE_COUNT))
    {
        os_critical_exit();
        return OS_STATUS_BUSY;
    }

    os_task_unlink(tcb);
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
        os_task_current[core] = (os_task_tcb_t *)0;
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
        tcb = os_task_current[os_arch_core_id_get()];
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
        tcb = os_task_current[os_arch_core_id_get()];
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
    uint32_t       primask = os_arch_primask_get();
    os_list_node_t *node;

    if (elapsed_ticks == 0U)
    {
        return;
    }

    /* Only finite-delay sleepers live in the delay list, so the cost is
     * O(sleeping tasks), not O(task table). OS_WAIT_FOREVER sleepers are in
     * no list and only os_task_wake releases them. Interrupts are masked so
     * a preempting ISR cannot resize the list mid-walk. */
    OS_ARCH_IRQ_DISABLE();

    node = os_task_delay_list.head;
    while (node != (os_list_node_t *)0)
    {
        os_list_node_t *next_node = node->next; /* the node may leave the list below */
        os_task_tcb_t  *tcb       = OS_TASK_TCB_FROM_NODE(node);

        if (tcb->delay_ticks > elapsed_ticks)
        {
            tcb->delay_ticks -= elapsed_ticks;
        }
        else
        {
            /* Unlink leaves the delay list and any object waiter list;
             * wait_signaled stays false = the wait timed out. */
            tcb->delay_ticks = 0U;
            os_task_unlink(tcb);
            os_task_make_ready(tcb);
        }

        node = next_node;
    }

    if (primask == 0U)
    {
        OS_ARCH_IRQ_ENABLE();
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
    uint32_t      core;
    os_task_tcb_t *current;

    if (ticks == 0U)
    {
        return;
    }

    os_critical_enter();

    core    = os_arch_core_id_get();
    current = os_task_current[core];

    /* Only a real task in task context can block. */
    if (!os_kernel_is_running() || os_arch_in_isr() ||
        (current == (os_task_tcb_t *)0) || (current == &os_task_idle_tcb[core]))
    {
        os_critical_exit();
        return;
    }

    current->delay_ticks = ticks;
    current->state       = OS_TASK_STATE_BLOCKED;

    /* Finite delays wait in the delay list; forever-sleepers stay out of
     * every list until os_task_wake releases them. The running task is in
     * no ready list, so no unlink is needed first. */
    if (ticks != OS_WAIT_FOREVER)
    {
        os_list_push_back(&os_task_delay_list, &current->state_node);
    }

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
        /* Unlink inspects delay_ticks to pick the list, so it runs first.
         * A forced wake reads as a (spurious) signal: a primitive waiter
         * then re-checks its condition instead of reporting a timeout. */
        os_task_unlink(tcb);
        tcb->delay_ticks   = 0U;
        tcb->wait_signaled = true;
        os_task_make_ready(tcb);

        if (os_kernel_is_running())
        {
            os_task_preempt_request(tcb);
        }
    }

    os_critical_exit();
}

/******************************************************************************************************/
/**
 * @brief Queue the calling task on an object's waiter list and block it (priority ordered,
 *        FIFO among equals). Call from task context inside a critical section, right after
 *        finding the object unavailable; the switch happens when the caller leaves the
 *        critical section. On resume, os_task_wait_signaled tells signal from timeout.
 *
 * @param[in,out] waiters        The object's waiter list.
 * @param[in]     timeout_ticks  Wait budget in ticks; OS_WAIT_FOREVER waits indefinitely.
 * @return None.
 */
void os_task_wait_begin(os_list_t *waiters, uint32_t timeout_ticks)
{
    uint32_t       core    = os_arch_core_id_get();
    os_task_tcb_t  *current = os_task_current[core];
    os_list_node_t *position;

    /* Only a real task can wait; the caller checks os_internal_can_block. */
    if ((current == (os_task_tcb_t *)0) || (current == &os_task_idle_tcb[core]))
    {
        return;
    }

    /* Insert before the first waiter of strictly lower priority: wakeups go
     * to the highest-priority waiter, FIFO within one priority level. */
    position = waiters->head;
    while (position != (os_list_node_t *)0)
    {
        if (OS_TASK_TCB_FROM_WAIT_NODE(position)->priority < current->priority)
        {
            break;
        }

        position = position->next;
    }

    os_list_insert_before(waiters, position, &current->wait_node);

    current->wait_list     = waiters;
    current->wait_signaled = false;
    current->delay_ticks   = timeout_ticks;
    current->state         = OS_TASK_STATE_BLOCKED;

    if (timeout_ticks != OS_WAIT_FOREVER)
    {
        os_list_push_back(&os_task_delay_list, &current->state_node);
    }

    OS_ARCH_CONTEXT_SWITCH_REQUEST();
}

/******************************************************************************************************/
/**
 * @brief After resuming from os_task_wait_begin: true when the object signaled the task,
 *        false when the wait timed out.
 *
 * @return bool  Wakeup reason.
 */
bool os_task_wait_signaled(void)
{
    os_task_tcb_t *current = os_task_current[os_arch_core_id_get()];

    return (current != (os_task_tcb_t *)0) ? current->wait_signaled : false;
}

/******************************************************************************************************/
/**
 * @brief After a signaled resume: the remaining timeout budget in ticks, for the retry loop.
 *
 * @return uint32_t  Remaining ticks (OS_WAIT_FOREVER when the wait was unbounded).
 */
uint32_t os_task_wait_remaining_ticks(void)
{
    os_task_tcb_t *current = os_task_current[os_arch_core_id_get()];

    return (current != (os_task_tcb_t *)0) ? current->delay_ticks : 0U;
}

/******************************************************************************************************/
/**
 * @brief Wake the highest-priority task waiting on an object (call inside a critical
 *        section; ISR-safe). The task resumes with its remaining timeout preserved and
 *        os_task_wait_signaled() true, so it re-checks the object's condition.
 *
 * @param[in,out] waiters  The object's waiter list.
 * @return bool  True when a task was woken.
 */
bool os_task_waiters_wake_one(os_list_t *waiters)
{
    os_list_node_t *node = waiters->head;
    os_task_tcb_t  *tcb;
    uint32_t       remaining;

    if (node == (os_list_node_t *)0)
    {
        return false;
    }

    tcb       = OS_TASK_TCB_FROM_WAIT_NODE(node);
    remaining = tcb->delay_ticks;

    os_task_unlink(tcb);              /* leaves the delay list and the waiter list */
    tcb->delay_ticks   = remaining;   /* keep the timeout budget for the retry     */
    tcb->wait_signaled = true;
    os_task_make_ready(tcb);

    if (os_kernel_is_running())
    {
        os_task_preempt_request(tcb);
    }

    return true;
}

/******************************************************************************************************/
/**
 * @brief Wake every task waiting on an object (call inside a critical section; ISR-safe).
 *        Used by event groups, where each waiter re-evaluates its own bit condition.
 *
 * @param[in,out] waiters  The object's waiter list.
 * @return None.
 */
void os_task_waiters_wake_all(os_list_t *waiters)
{
    while (os_task_waiters_wake_one(waiters))
    {
    }
}

/******************************************************************************************************/
/**
 * @brief Get the id of the current task, 0 when idle/none/pre-scheduler.
 *
 * @return uint32_t  Current task id.
 */
uint32_t os_task_current_id_get(void)
{
    os_task_tcb_t *tcb = os_task_current[os_arch_core_id_get()];

    return (tcb == (os_task_tcb_t *)0) ? 0U : tcb->id;
}

/******************************************************************************************************/
/**
 * @brief Check whether the calling core's idle task is currently running (ISR-safe).
 *
 * @return bool  True when the current task is this core's idle task.
 */
bool os_task_current_is_idle(void)
{
    uint32_t core = os_arch_core_id_get();

    return (os_task_current[core] == &os_task_idle_tcb[core]);
}

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief Change which cores a task may run on.
 *
 * The task keeps its place in the shared ready/delay lists; each core simply
 * skips tasks its bit is missing from. When the task is currently executing
 * on a core the new mask excludes, that core is asked to reschedule.
 *
 * @param[in] task           Task handle.
 * @param[in] core_affinity  Bitmask of allowed cores; OS_TASK_CORE_ANY (0) = any core.
 * @return os_status  Status code.
 */
os_status os_task_core_affinity_set(os_task_t *task, uint32_t core_affinity)
{
    os_task_tcb_t *tcb;
    uint32_t      running;

    if ((task == (os_task_t *)0) || (task->id == 0U) ||
        ((core_affinity >> OS_CONFIG_CORE_COUNT) != 0U))
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

    tcb->core_affinity = core_affinity;

    running = os_task_running_core(tcb);
    if ((running < OS_CONFIG_CORE_COUNT) && os_kernel_is_running() &&
        (core_affinity != OS_TASK_CORE_ANY) && ((core_affinity & (1UL << running)) == 0U))
    {
        if (running == os_arch_core_id_get())
        {
            OS_ARCH_CONTEXT_SWITCH_REQUEST();
        }
        else
        {
            os_arch_core_ipi_request_cb(running);
        }
    }

    os_critical_exit();
    return OS_STATUS_OK;
}
#endif /* OS_CONFIG_CORE_COUNT > 1U */

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
    uint32_t core;

    if (os_task_idle_tcb[0].state != OS_TASK_STATE_INACTIVE)
    {
        return OS_STATUS_OK;
    }

    /* One idle task per scheduling core; each is pinned to its core and is
     * never queued in a ready list (it is the empty-bitmap fallback). */
    for (core = 0U; core < OS_CONFIG_CORE_COUNT; core++)
    {
        os_task_tcb_t *tcb = &os_task_idle_tcb[core];
        uint32_t      *stack_ptr;

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
        os_task_stack_fill(os_task_idle_stack[core], sizeof(os_task_idle_stack[core]));
#endif

        stack_ptr = os_arch_task_stack_initialize(os_task_idle_stack[core], sizeof(os_task_idle_stack[core]),
                                                  os_task_idle_entry, (void *)0);
        if (stack_ptr == (uint32_t *)0)
        {
            return OS_STATUS_ERROR;
        }

        tcb->name          = "tsk_idle";
        tcb->stack_base    = os_task_idle_stack[core];
        tcb->stack_ptr     = stack_ptr;
        tcb->stack_bytes   = sizeof(os_task_idle_stack[core]);
        tcb->priority      = OS_TASK_PRIORITY_IDLE;
        tcb->entry         = os_task_idle_entry;
        tcb->context       = (void *)0;
        tcb->id            = 0U;
        tcb->delay_ticks   = 0U;
        tcb->core_affinity = (1UL << core);
        tcb->state         = OS_TASK_STATE_READY;
    }

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
    return (os_task_idle_tcb[0].state != OS_TASK_STATE_INACTIVE);
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
    uint32_t priority;
    uint32_t core;

    for (index = 0U; index < OS_CONFIG_MAX_TASKS; index++)
    {
        os_task_in_use[index] = false;
        os_task_tcb_clear(&os_task_table[index]);
    }

    for (priority = 0U; priority <= OS_CONFIG_MAX_PRIORITY; priority++)
    {
        os_list_init(&os_task_ready_list[priority]);
    }

    os_task_ready_bitmap = 0U;
    os_list_init(&os_task_delay_list);

    os_task_next_id = 1U;

    for (core = 0U; core < OS_CONFIG_CORE_COUNT; core++)
    {
        os_task_current[core] = (os_task_tcb_t *)0;
        os_task_tcb_clear(&os_task_idle_tcb[core]);
    }
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
    uint32_t      primask = os_arch_primask_get();
    uint32_t      core;
    os_task_tcb_t *current_task;

    /* PendSV runs with interrupts enabled: mask them so an ISR waking or
     * pausing tasks cannot touch the ready lists mid-update. */
    OS_ARCH_IRQ_DISABLE();

    core         = os_arch_core_id_get();
    current_task = os_task_current[core];

    if (current_task != (os_task_tcb_t *)0)
    {
        current_task->stack_ptr = stack_ptr;

        /* A preempted task goes back to the TAIL of its priority's ready
         * list - that is the round-robin rotation. A task that just blocked
         * or suspended itself keeps its state (it is already in the right
         * list, or in none), and the idle tasks are never queued. */
        if (current_task->state == OS_TASK_STATE_RUNNING)
        {
            if (current_task == &os_task_idle_tcb[core])
            {
                current_task->state = OS_TASK_STATE_READY;
            }
            else
            {
                os_task_make_ready(current_task);
            }
        }
    }

    if (primask == 0U)
    {
        OS_ARCH_IRQ_ENABLE();
    }
}

/******************************************************************************************************/
/**
 * @brief Select the next task to run and return its stack pointer (called from PendSV/SVC).
 *
 * @return uint32_t*  Stack pointer for the selected task; never NULL (idle fallback).
 */
uint32_t* os_task_stack_select_next(void)
{
    uint32_t      primask = os_arch_primask_get();
    uint32_t      core;
    uint32_t      bitmap;
    os_task_tcb_t *next;

    OS_ARCH_IRQ_DISABLE();

    core = os_arch_core_id_get();
    next = &os_task_idle_tcb[core];

    /* O(1) pick on single-core: the bitmap names the highest non-empty
     * priority and the FIFO head is the next task (round-robin). On
     * multi-core builds each list is additionally walked past tasks whose
     * affinity excludes this core; a priority whose tasks all belong to
     * other cores is skipped (without clearing its bitmap bit). */
    bitmap = os_task_ready_bitmap;
    while (bitmap != 0U)
    {
        uint32_t       priority = os_arch_highest_bit_get(bitmap);
        os_list_t      *list    = &os_task_ready_list[priority];
        os_list_node_t *node    = list->head;

        while (node != (os_list_node_t *)0)
        {
            os_task_tcb_t *tcb = OS_TASK_TCB_FROM_NODE(node);

#if (OS_CONFIG_CORE_COUNT > 1U)
            if ((tcb->core_affinity != OS_TASK_CORE_ANY) &&
                ((tcb->core_affinity & (1UL << core)) == 0U))
            {
                node = node->next;
                continue;
            }
#endif

            os_list_remove(list, node);
            if (os_list_is_empty(list))
            {
                os_task_ready_bitmap &= ~(1UL << priority);
            }

            next = tcb;
            break;
        }

        if (next != &os_task_idle_tcb[core])
        {
            break;
        }

        bitmap &= ~(1UL << priority);
    }

    next->state           = OS_TASK_STATE_RUNNING;
    os_task_current[core] = next;

    if (primask == 0U)
    {
        OS_ARCH_IRQ_ENABLE();
    }

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

    /* The affinity mask may only name existing cores (0 = any core). */
    if ((config->core_affinity >> OS_CONFIG_CORE_COUNT) != 0U)
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
            tcb->core_affinity    = config->core_affinity;
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
    tcb->id            = 0U;
    tcb->delay_ticks   = 0U;
    tcb->core_affinity = OS_TASK_CORE_ANY;
    tcb->state         = OS_TASK_STATE_INACTIVE;
    tcb->wait_list     = (os_list_t *)0;
    tcb->wait_signaled = false;

    tcb->state_node.next = (os_list_node_t *)0;
    tcb->state_node.prev = (os_list_node_t *)0;
    tcb->wait_node.next  = (os_list_node_t *)0;
    tcb->wait_node.prev  = (os_list_node_t *)0;
}

/******************************************************************************************************/
/**
 * @brief Find a task by its ID.
 *
 * @param[in] id  Task ID.
 * @return os_task_tcb_t*  Pointer to the task control block, or NULL if not found.
 */
static os_task_tcb_t* os_task_find_by_id(uint32_t id)
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

/******************************************************************************************************/
/**
 * @brief Queue a task at the tail of its priority's ready list and flag the priority.
 *
 * Caller must hold a critical section (or have interrupts masked) and must
 * never pass the idle task or a task already sitting in a list.
 *
 * @param[in,out] tcb  Task to make ready.
 * @return None.
 */
static void os_task_make_ready(os_task_tcb_t *tcb)
{
    tcb->state = OS_TASK_STATE_READY;
    os_list_push_back(&os_task_ready_list[tcb->priority], &tcb->state_node);
    os_task_ready_bitmap |= (1UL << tcb->priority);
}

/******************************************************************************************************/
/**
 * @brief Remove a task from whatever scheduler list its state implies (no-op when in none).
 *
 * READY tasks leave their priority's ready list (clearing the bitmap bit when
 * it empties); finite-delay BLOCKED tasks leave the delay list. Running,
 * suspended and forever-blocked tasks are in no list. Caller must hold a
 * critical section (or have interrupts masked).
 *
 * @param[in,out] tcb  Task to unlink.
 * @return None.
 */
static void os_task_unlink(os_task_tcb_t *tcb)
{
    if (tcb->state == OS_TASK_STATE_READY)
    {
        os_list_t *list = &os_task_ready_list[tcb->priority];

        os_list_remove(list, &tcb->state_node);

        if (os_list_is_empty(list))
        {
            os_task_ready_bitmap &= ~(1UL << tcb->priority);
        }
    }
    else if ((tcb->state == OS_TASK_STATE_BLOCKED) && (tcb->delay_ticks != OS_WAIT_FOREVER))
    {
        os_list_remove(&os_task_delay_list, &tcb->state_node);
    }
    else
    {
        /* Running, suspended, inactive or forever-blocked: in no state list. */
    }

    /* A blocked task may additionally sit in an object's waiter list. */
    if (tcb->wait_list != (os_list_t *)0)
    {
        os_list_remove(tcb->wait_list, &tcb->wait_node);
        tcb->wait_list = (os_list_t *)0;
    }
}

/******************************************************************************************************/
/**
 * @brief Request a reschedule wherever the given task may run: locally when its affinity
 *        allows this core, otherwise via IPI to the first core in its mask.
 *
 * Caller checks os_kernel_is_running(). On single-core builds this is a
 * plain local PendSV request.
 *
 * @param[in] tcb  Task that just became ready.
 * @return None.
 */
static void os_task_preempt_request(const os_task_tcb_t *tcb)
{
#if (OS_CONFIG_CORE_COUNT > 1U)
    uint32_t core = os_arch_core_id_get();

    if ((tcb->core_affinity != OS_TASK_CORE_ANY) &&
        ((tcb->core_affinity & (1UL << core)) == 0U))
    {
        /* The task cannot run here: nudge the first core it may run on
         * (weak default IPI does nothing - that core then picks the task
         * up at its own next tick). */
        os_arch_core_ipi_request_cb(os_arch_lowest_bit_get(tcb->core_affinity));
        return;
    }
#else
    (void)tcb;
#endif

    OS_ARCH_CONTEXT_SWITCH_REQUEST();
}

/******************************************************************************************************/
/**
 * @brief Find the core a task is currently executing on.
 *
 * @param[in] tcb  Task to look up.
 * @return uint32_t  Core index, or OS_CONFIG_CORE_COUNT when the task is not running anywhere.
 */
static uint32_t os_task_running_core(const os_task_tcb_t *tcb)
{
    uint32_t core;

    for (core = 0U; core < OS_CONFIG_CORE_COUNT; core++)
    {
        if (os_task_current[core] == tcb)
        {
            return core;
        }
    }

    return OS_CONFIG_CORE_COUNT;
}
