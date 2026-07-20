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

#define OS_TASK_PRIO_IDLE            0U
#define OS_TASK_STACK_FILL_BYTE          0xA5U

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
    uint32_t        running_core;  /* which core dispatched this task and has not
                                     * yet saved its context; OS_CONFIG_CORE_COUNT
                                     * when the context is safely saved/not running */
    os_list_node_t  state_node;    /* links into one ready list or the delay list  */
    os_list_node_t  wait_node;     /* links into one object's waiter list          */
    os_list_t       *wait_list;    /* joined waiter list, NULL when waiting on none */
    bool            wait_signaled; /* wakeup reason: object signal vs timeout      */
    uint32_t        wait_data[2];  /* per-wait condition data read by match wakers */
    uint32_t        wait_result;   /* delivery stored by a match waker, else 0     */
    os_list_t       *woken_from;   /* waiter list a pending wake came from, until consumed */

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
static os_list_t               os_task_ready_list[OS_TASK_PRIO_MAX + 1U];
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
static void           os_task_wake_compensate(os_task_tcb_t *tcb);
static void           os_task_wake_locked(os_task_tcb_t *tcb);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Create a task using the provided configuration.
 *
 * Priority 0 (idle) and OS_TASK_PRIO_MAX (kernel work/timer service
 * tasks) are reserved: user tasks must use OS_TASK_PRIO_USER_MIN to
 * OS_TASK_PRIO_USER_MAX.
 *
 * @param[out] task    Output task handle.
 * @param[in]  config  Task creation configuration.
 * @return os_status   Status code.
 */
os_status os_task_create(os_task_t *task, const os_task_config_t *config)
{
    if (config == NULL)
    {
        return OS_STATUS_INVALID_ARG;
    }

    if ((config->priority < OS_TASK_PRIO_USER_MIN) || (config->priority > OS_TASK_PRIO_USER_MAX))
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

    if ((task == NULL) || (task->id == 0U))
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    tcb = os_task_find_by_id(task->id);
    if (tcb == NULL)
    {
        os_critical_exit();
        return OS_STATUS_INVALID_ARG;
    }

    /* Reviving a task blocked on a primitive reads as a forced (spurious)
     * signal: the primitive re-checks its condition with its timeout budget
     * preserved, instead of misreporting OS_STATUS_TIMEOUT - even on an
     * OS_WAIT_FOREVER wait. Unlink runs first (it inspects delay_ticks). */
    if (tcb->state == OS_TASK_STATE_BLOCKED)
    {
        os_task_unlink(tcb);
        tcb->wait_signaled = true;
    }
    else
    {
        os_task_unlink(tcb);
        tcb->delay_ticks = 0U;
    }

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

    if (task == NULL)
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

    if ((tcb == NULL) || (tcb == &os_task_idle_tcb[core]))
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

    /* This task may be READY only because a give/send/set_bits woke it and
     * it has not yet run its retry loop to consume the notification (see
     * os_task_wake_compensate): pass the notification on to another waiter
     * of the same object before suspending, so it is not silently dropped. */
    os_task_wake_compensate(tcb);

    /* Suspending a task blocked on a primitive reads as a forced (spurious)
     * signal, mirroring os_task_wake/os_task_start: on a later os_task_start
     * the primitive re-checks its condition instead of misreporting
     * OS_STATUS_TIMEOUT, even on an OS_WAIT_FOREVER wait. */
    if (tcb->state == OS_TASK_STATE_BLOCKED)
    {
        tcb->wait_signaled = true;
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

    if (task == NULL)
    {
        tcb = os_task_current[core];
        if ((tcb == NULL) || (tcb == &os_task_idle_tcb[core]))
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

    /* See os_task_pause: pass on an unconsumed wake before this task's TCB
     * is torn down, so the notification is not silently dropped. */
    os_task_wake_compensate(tcb);

    os_task_unlink(tcb);
    os_task_in_use[index] = false;
    os_task_tcb_clear(tcb);

    if (task != NULL)
    {
        task->id = 0U;
    }

    if (is_self)
    {
        /* The calling task ceases to exist: drop the current pointer so the
         * switch-out path does not touch the freed TCB, then switch away. */
        os_task_current[core] = NULL;
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

    if (task == NULL)
    {
        tcb = os_task_current[os_arch_core_id_get()];
    }
    else if (task->id == 0U)
    {
        tcb = NULL;
    }
    else
    {
        tcb = os_task_find_by_id(task->id);
    }

    state = (tcb == NULL) ? OS_TASK_STATE_INACTIVE : tcb->state;

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

    if (min_free_bytes == NULL)
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    if (task == NULL)
    {
        tcb = os_task_current[os_arch_core_id_get()];
    }
    else if (task->id == 0U)
    {
        tcb = NULL;
    }
    else
    {
        tcb = os_task_find_by_id(task->id);
    }

    if ((tcb == NULL) || (tcb->stack_base == NULL))
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
    uint32_t       mask_state;
    os_list_node_t *node;

    if (elapsed_ticks == 0U)
    {
        return;
    }

    /* Only finite-delay sleepers live in the delay list, so the cost is
     * O(sleeping tasks), not O(task table). OS_WAIT_FOREVER sleepers are in
     * no list and only os_task_wake releases them. The kernel mask is raised
     * so a preempting ISR cannot resize the list mid-walk, and on multi-core
     * builds the cross-core spinlock additionally excludes the other cores'
     * os_task_wait_begin/os_task_sleep_ticks callers, who insert into this
     * same shared delay list under os_critical_enter. */
    mask_state = os_arch_kernel_mask_save();
    os_critical_multicore_lock();

    node = os_task_delay_list.head;
    while (node != NULL)
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

    os_critical_multicore_unlock();
    os_arch_kernel_mask_restore(mask_state);
}

/******************************************************************************************************/
/**
 * @brief Return ticks until the next finite-delay sleeper wakes (tickless planning).
 *
 * OS_WAIT_FOREVER sleepers are in no list and never bound the idle time.
 *
 * @return uint32_t  Minimum remaining delay in ticks, UINT32_MAX when no task is delaying.
 */
uint32_t os_task_next_delay_ticks_get(void)
{
    uint32_t       mask_state = os_arch_kernel_mask_save();
    uint32_t       minimum    = UINT32_MAX;
    os_list_node_t *node;

    /* See os_task_tick_update: the cross-core spinlock excludes the other
     * cores' insertions into this same shared delay list. */
    os_critical_multicore_lock();

    for (node = os_task_delay_list.head; node != NULL; node = node->next)
    {
        const os_task_tcb_t *tcb = OS_TASK_TCB_FROM_NODE(node);

        if (tcb->delay_ticks < minimum)
        {
            minimum = tcb->delay_ticks;
        }
    }

    os_critical_multicore_unlock();
    os_arch_kernel_mask_restore(mask_state);

    return minimum;
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
        (current == NULL) || (current == &os_task_idle_tcb[core]))
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
    os_task_wake_locked(tcb);
    os_critical_exit();
}

/******************************************************************************************************/
/**
 * @brief Resolve a task id to an opaque handle once, for os_task_wake_tcb (ISR-safe).
 *
 * The handle is a raw TCB pointer; it stays valid for the task's lifetime, so callers that
 * never delete the target (e.g. the kernel's own work/timer service tasks) may resolve it
 * once at init and cache it, skipping the O(OS_CONFIG_MAX_TASKS) id scan on every later wake.
 *
 * @param[in] task_id  Id to resolve.
 * @return void*  Opaque handle, or NULL when the id does not exist.
 */
void* os_task_tcb_resolve(uint32_t task_id)
{
    return (void *)os_task_find_by_id(task_id);
}

/******************************************************************************************************/
/**
 * @brief Wake a BLOCKED task by its os_task_tcb_resolve handle (ISR-safe), skipping both the
 *        id lookup and the nested critical section os_task_wake pays: the caller must
 *        already hold the kernel mask and, on multi-core builds, the cross-core spinlock
 *        (os_critical_multicore_lock) - exactly what the tick-time work/timer wake path
 *        already holds around its registry walk. No-op for a NULL handle or a task not
 *        currently BLOCKED, same as os_task_wake.
 *
 * @param[in] tcb_handle  Handle from os_task_tcb_resolve.
 * @return None.
 */
void os_task_wake_tcb(void *tcb_handle)
{
    os_task_wake_locked((os_task_tcb_t *)tcb_handle);
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
    if ((current == NULL) || (current == &os_task_idle_tcb[core]))
    {
        return;
    }

    /* Insert before the first waiter of strictly lower priority: wakeups go
     * to the highest-priority waiter, FIFO within one priority level. */
    position = waiters->head;
    while (position != NULL)
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

    return (current != NULL) ? current->wait_signaled : false;
}

/******************************************************************************************************/
/**
 * @brief Attach two words of per-wait condition data to the calling task; call inside the
 *        critical section right before os_task_wait_begin. A waker's match callback reads
 *        them through os_task_waiters_wake_match. The pending result is reset here.
 *
 * @param[in] data0  First condition word (meaning owned by the primitive).
 * @param[in] data1  Second condition word.
 * @return None.
 */
void os_task_wait_data_set(uint32_t data0, uint32_t data1)
{
    os_task_tcb_t *current = os_task_current[os_arch_core_id_get()];

    if (current != NULL)
    {
        current->wait_data[0] = data0;
        current->wait_data[1] = data1;
        current->wait_result  = 0U;
    }
}

/******************************************************************************************************/
/**
 * @brief After a signaled resume: the delivery a match waker stored for this task, or 0 when
 *        the wake came from any other source (plain wake-one, forced wake, timeout).
 *
 * @return uint32_t  Stored wait result.
 */
uint32_t os_task_wait_result_get(void)
{
    os_task_tcb_t *current = os_task_current[os_arch_core_id_get()];

    return (current != NULL) ? current->wait_result : 0U;
}

/******************************************************************************************************/
/**
 * @brief Wake every waiter whose stored condition the callback confirms, storing each
 *        waiter's delivery for os_task_wait_result_get (call inside a critical section;
 *        ISR-safe). The walk is unlink-safe and evaluates in list order, i.e. highest
 *        priority first.
 *
 * @param[in,out] waiters  The object's waiter list.
 * @param[in]     match    Condition callback; true = wake this waiter.
 * @param[in]     context  Opaque pointer handed to the callback.
 * @return uint32_t  Number of waiters woken.
 */
uint32_t os_task_waiters_wake_match(os_list_t *waiters, os_task_wait_match_fn match, void *context)
{
    os_list_node_t *node  = waiters->head;
    uint32_t       woken  = 0U;

    while (node != NULL)
    {
        os_list_node_t *next_node = node->next; /* the node may leave the list below */
        os_task_tcb_t  *tcb       = OS_TASK_TCB_FROM_WAIT_NODE(node);
        uint32_t       result     = 0U;

        if (match(tcb->wait_data[0], tcb->wait_data[1], context, &result))
        {
            uint32_t remaining = tcb->delay_ticks;

            os_task_unlink(tcb);              /* leaves the delay list and the waiter list */
            tcb->delay_ticks   = remaining;   /* keep the timeout budget for the retry     */
            tcb->wait_signaled = true;
            tcb->wait_result   = result;
            tcb->woken_from    = waiters;     /* until consumed: see os_task_wake_compensate */
            os_task_make_ready(tcb);

            if (os_kernel_is_running())
            {
                os_task_preempt_request(tcb);
            }

            woken++;
        }

        node = next_node;
    }

    return woken;
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

    if (node == NULL)
    {
        return false;
    }

    tcb       = OS_TASK_TCB_FROM_WAIT_NODE(node);
    remaining = tcb->delay_ticks;

    os_task_unlink(tcb);              /* leaves the delay list and the waiter list */
    tcb->delay_ticks   = remaining;   /* keep the timeout budget for the retry     */
    tcb->wait_signaled = true;
    tcb->woken_from    = waiters;     /* until consumed: see os_task_wake_compensate */
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

    return (tcb == NULL) ? 0U : tcb->id;
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

/******************************************************************************************************/
/**
 * @brief Whether a PendSV on this core would actually switch or round-robin (ISR-safe).
 *
 * The running task is never queued in a ready list, so a bit at or above its
 * own priority can only belong to another task: strictly higher means a real
 * preemption is due, and the task's own priority bit means an equal-priority
 * peer is waiting to round-robin. Used by the tick handler to skip the full
 * PendSV round trip on a quiescent tick (nothing woke, no peer waiting).
 *
 * @return bool  True when a reschedule is currently possible on this core.
 */
bool os_task_reschedule_possible(void)
{
    uint32_t       mask_state = os_arch_kernel_mask_save();
    uint32_t       core;
    os_task_tcb_t  *current;
    bool           result;

    os_critical_multicore_lock();

    core    = os_arch_core_id_get();
    current = os_task_current[core];

    if (current == NULL)
    {
        result = (os_task_ready_bitmap != 0U);
    }
    else
    {
        uint32_t at_or_above_mask = ~((1UL << current->priority) - 1U);

        result = ((os_task_ready_bitmap & at_or_above_mask) != 0U);
    }

    os_critical_multicore_unlock();
    os_arch_kernel_mask_restore(mask_state);

    return result;
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

    if ((task == NULL) || (task->id == 0U) ||
        ((core_affinity >> OS_CONFIG_CORE_COUNT) != 0U))
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    tcb = os_task_find_by_id(task->id);
    if (tcb == NULL)
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
    (void)os_task_delete(NULL);

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
                                                  os_task_idle_entry, NULL);
        if (stack_ptr == NULL)
        {
            return OS_STATUS_ERROR;
        }

        tcb->name          = "tsk_idle";
        tcb->stack_base    = os_task_idle_stack[core];
        tcb->stack_ptr     = stack_ptr;
        tcb->stack_bytes   = sizeof(os_task_idle_stack[core]);
        tcb->priority      = OS_TASK_PRIO_IDLE;
        tcb->entry         = os_task_idle_entry;
        tcb->context       = NULL;
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

    for (priority = 0U; priority <= OS_TASK_PRIO_MAX; priority++)
    {
        os_list_init(&os_task_ready_list[priority]);
    }

    os_task_ready_bitmap = 0U;
    os_list_init(&os_task_delay_list);

    os_task_next_id = 1U;

    for (core = 0U; core < OS_CONFIG_CORE_COUNT; core++)
    {
        os_task_current[core] = NULL;
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
    uint32_t      mask_state;
    uint32_t      core;
    os_task_tcb_t *current_task;

    /* PendSV runs with interrupts enabled: raise the kernel mask so an ISR
     * waking or pausing tasks cannot touch the ready lists mid-update, and
     * take the cross-core spinlock so another core's os_critical_enter
     * callers (which push/pop these same shared ready/delay lists and
     * bitmap) are excluded too - the local mask alone only stops this
     * core's own interrupts, not the other core's task-context callers. */
    mask_state = os_arch_kernel_mask_save();
    os_critical_multicore_lock();

    core         = os_arch_core_id_get();
    current_task = os_task_current[core];

    if (current_task != NULL)
    {
        current_task->stack_ptr = stack_ptr;

        /* The context is now safely saved: from this point any core may
         * dispatch this task (see the running_core check in
         * os_task_stack_select_next). Before this write, a wake arriving on
         * another core while this task was still physically executing here
         * (state already flipped to BLOCKED/READY by the waker, but this
         * core had not yet run this function) must not let that other core
         * pick it up - its stack_ptr would still be stale. */
        current_task->running_core = OS_CONFIG_CORE_COUNT;

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

    os_critical_multicore_unlock();
    os_arch_kernel_mask_restore(mask_state);
}

/******************************************************************************************************/
/**
 * @brief Select the next task to run and return its stack pointer (called from PendSV/SVC).
 *
 * @return uint32_t*  Stack pointer for the selected task; never NULL (idle fallback).
 */
uint32_t* os_task_stack_select_next(void)
{
    uint32_t      mask_state = os_arch_kernel_mask_save();
    uint32_t      core;
    uint32_t      bitmap;
    os_task_tcb_t *next;

    /* See os_task_stack_save_current: the cross-core spinlock excludes the
     * other cores' os_critical_enter callers on these same shared lists. */
    os_critical_multicore_lock();

    core = os_arch_core_id_get();
    next = &os_task_idle_tcb[core];

    /* O(1) pick on single-core: the bitmap names the highest non-empty
     * priority and the FIFO head is the next task (round-robin). On
     * multi-core builds each list is additionally walked past tasks whose
     * affinity excludes this core, and past a task still mid-switch-out on
     * another core (running_core != CORE_COUNT: it was woken here before
     * that core's os_task_stack_save_current saved its context, so its
     * stack_ptr is not yet safe to restore from - see the running_core
     * write there); either skip leaves the bitmap bit set. */
    bitmap = os_task_ready_bitmap;
    while (bitmap != 0U)
    {
        uint32_t       priority = os_arch_highest_bit_get(bitmap);
        os_list_t      *list    = &os_task_ready_list[priority];
        os_list_node_t *node    = list->head;

        while (node != NULL)
        {
            os_task_tcb_t *tcb = OS_TASK_TCB_FROM_NODE(node);

#if (OS_CONFIG_CORE_COUNT > 1U)
            if ((tcb->core_affinity != OS_TASK_CORE_ANY) &&
                ((tcb->core_affinity & (1UL << core)) == 0U))
            {
                node = node->next;
                continue;
            }

            if (tcb->running_core != OS_CONFIG_CORE_COUNT)
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
    next->running_core     = core;
    os_task_current[core] = next;

    os_critical_multicore_unlock();
    os_arch_kernel_mask_restore(mask_state);

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

    if ((task == NULL) ||
        (config == NULL) ||
        (config->entry == (os_task_entry_t)0) ||
        (config->priority > OS_TASK_PRIO_MAX) ||
        (config->stack_memory == NULL) ||
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
    if (stack_ptr == NULL)
    {
        return OS_STATUS_ERROR;
    }

    os_critical_enter();

    for (index = 0U; index < OS_CONFIG_MAX_TASKS; index++)
    {
        if (!os_task_in_use[index])
        {
            os_task_tcb_t *tcb = &os_task_table[index];

            /* Skip ids still owned by live tasks: the counter wraps at 2^32,
             * and a reused id would hand the new task another task's
             * identity (e.g. the right to unlock a dead owner's mutex). */
            while ((os_task_next_id == 0U) ||
                   (os_task_find_index_by_id(os_task_next_id) < OS_CONFIG_MAX_TASKS))
            {
                os_task_next_id++;
            }

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
    tcb->name        = NULL;
    tcb->stack_base  = NULL;
    tcb->stack_ptr   = NULL;
    tcb->stack_bytes = 0U;
    tcb->priority    = 0U;
    tcb->entry       = (os_task_entry_t)0;
    tcb->context     = NULL;
    tcb->id            = 0U;
    tcb->delay_ticks   = 0U;
    tcb->core_affinity = OS_TASK_CORE_ANY;
    tcb->state         = OS_TASK_STATE_INACTIVE;
    tcb->running_core  = OS_CONFIG_CORE_COUNT;
    tcb->wait_list     = NULL;
    tcb->wait_signaled = false;
    tcb->wait_data[0]  = 0U;
    tcb->wait_data[1]  = 0U;
    tcb->wait_result   = 0U;
    tcb->woken_from    = NULL;

    tcb->state_node.next = NULL;
    tcb->state_node.prev = NULL;
    tcb->wait_node.next  = NULL;
    tcb->wait_node.prev  = NULL;
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

    return (index < OS_CONFIG_MAX_TASKS) ? &os_task_table[index] : NULL;
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
    if (tcb->wait_list != NULL)
    {
        os_list_remove(tcb->wait_list, &tcb->wait_node);
        tcb->wait_list = NULL;
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
    uint32_t            core = os_arch_core_id_get();
    const os_task_tcb_t *current;

#if (OS_CONFIG_CORE_COUNT > 1U)
    if ((tcb->core_affinity != OS_TASK_CORE_ANY) &&
        ((tcb->core_affinity & (1UL << core)) == 0U))
    {
        /* The task cannot run here: nudge the first core it may run on
         * (weak default IPI does nothing - that core then picks the task
         * up at its own next tick). */
        os_arch_core_ipi_request_cb(os_arch_lowest_bit_get(tcb->core_affinity));
        return;
    }
#endif

    /* Only a strictly higher-priority task warrants an immediate switch:
     * equal priorities round-robin at the tick, so skipping the PendSV here
     * both saves the full context-switch cost and stops every wake from
     * acting as an implicit yield of the waker's remaining timeslice. */
    current = os_task_current[core];

    if ((current == NULL) || (tcb->priority > current->priority))
    {
        OS_ARCH_CONTEXT_SWITCH_REQUEST();
        return;
    }

#if (OS_CONFIG_CORE_COUNT > 1U)
    /* This core keeps its own (higher-or-equal priority) task, but another
     * core allowed by the woken task's affinity may be idle or running
     * something lower priority: without this scan the task would wait for
     * that core's own next tick even though it could run immediately.
     * Reading os_task_current[] here is safe because every caller of this
     * function already holds os_critical_enter, which on multi-core builds
     * holds the same spinlock os_task_stack_select_next takes before
     * writing os_task_current[]. */
    {
        uint32_t other;

        for (other = 0U; other < OS_CONFIG_CORE_COUNT; other++)
        {
            const os_task_tcb_t *other_current;

            if (other == core)
            {
                continue;
            }

            if ((tcb->core_affinity != OS_TASK_CORE_ANY) && ((tcb->core_affinity & (1UL << other)) == 0U))
            {
                continue;
            }

            other_current = os_task_current[other];

            if ((other_current == NULL) || (tcb->priority > other_current->priority))
            {
                os_arch_core_ipi_request_cb(other);
                return;
            }
        }
    }
#endif
}

/******************************************************************************************************/
/**
 * @brief Shared body of os_task_wake / os_task_wake_tcb: caller already holds whatever
 *        locking that entry point's contract requires.
 *
 * @param[in,out] tcb  Task to wake, or NULL (no-op).
 * @return None.
 */
static void os_task_wake_locked(os_task_tcb_t *tcb)
{
    if ((tcb != NULL) && (tcb->state == OS_TASK_STATE_BLOCKED))
    {
        /* Unlink inspects delay_ticks to pick the list, so it runs first.
         * A forced wake reads as a (spurious) signal: a primitive waiter
         * then re-checks its condition instead of reporting a timeout.
         * delay_ticks is left untouched so the waiter keeps its remaining
         * timeout budget for the retry (recomputed against the wall clock). */
        os_task_unlink(tcb);
        tcb->wait_signaled = true;
        os_task_make_ready(tcb);

        if (os_kernel_is_running())
        {
            os_task_preempt_request(tcb);
        }
    }
}

/******************************************************************************************************/
/**
 * @brief Pass on an unconsumed wake before a READY task is suspended or deleted.
 *
 * A task is READY with wait_signaled still true only in the narrow window
 * between os_task_waiters_wake_one/wake_match handing it a notification and
 * the task itself running its retry loop (which either takes the resource,
 * in which case it is RUNNING, not READY, or re-blocks, which resets
 * wait_signaled to false in os_task_wait_begin) - so this combination is a
 * race-free signal that the notification was never consumed. Without this,
 * pausing or deleting the task in that window would strand the resource's
 * availability (e.g. an item already sitting in a queue) with nobody left
 * to signal the next waiter.
 *
 * @param[in,out] tcb  Task about to be suspended or deleted.
 * @return None.
 */
static void os_task_wake_compensate(os_task_tcb_t *tcb)
{
    if ((tcb->state == OS_TASK_STATE_READY) && tcb->wait_signaled && (tcb->woken_from != NULL))
    {
        os_list_t *list = tcb->woken_from;

        tcb->woken_from = NULL;
        (void)os_task_waiters_wake_one(list);
    }
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
