/**
 * @file os_test.c
 * @brief Boot-time self-test suite for the Ahura RTOS kernel.
 *
 * Strong override of the weak os_test() (declared in ahura.h, empty default in os_kernel.c; not
 * named with the "_cb" suffix - this is where the suite's own code runs, not a kernel query for
 * platform behavior): link this file's library (ahura_kernel/test, target "os_test") and, when
 * OS_CONFIG_TEST_ENABLE is 1, os_init() creates a task that calls this automatically - no explicit
 * call needed from the application. Runs once, exercises whichever OS_CONFIG_<FEATURE>_ENABLE
 * switches are on, and
 * prints a detailed PASS/FAIL log via printf, finishing with a pass/fail summary. Depends on
 * nothing but ahura.h - no board/HAL headers - so it runs on any arch/board the kernel supports;
 * printf's destination (typically a UART) is the linking application's concern.
 */

#include "ahura.h"

#include <stdio.h>
#include <stdbool.h>

/*
 * ***********************************************************************************************************
 * Test bookkeeping
 * ***********************************************************************************************************
*/

static uint32_t g_pass_count = 0U;
static uint32_t g_fail_count = 0U;

#define AHURA_TEST_CHECK(cond, fmt, ...) \
    do { \
        if (cond) { g_pass_count++; printf("  [PASS] " fmt "\r\n", ##__VA_ARGS__); } \
        else      { g_fail_count++; printf("  [FAIL] " fmt "  (os_test.c:%d)\r\n", ##__VA_ARGS__, __LINE__); } \
    } while (0)

static void test_print_section(const char *title)
{
    printf("\r\n--- %s ---\r\n", title);
}

/*
 * ***********************************************************************************************************
 * Shared kernel objects under test
 * ***********************************************************************************************************
*/

OS_TASK_DEFINE(worker, 512U);
OS_TASK_DEFINE(helper, 512U);
/* Two more concurrent task slots for the combined-scenario tests below, which run 3-4 tasks
 * at once (single-primitive tests above only ever run one helper at a time). */
OS_TASK_DEFINE(helper2, 512U);
OS_TASK_DEFINE(helper3, 512U);

static volatile uint32_t g_worker_counter    = 0U;
static volatile bool     g_worker_should_run = true;

/* Shared between test_priority_preemption() and test_cpu_usage(): a task that spins
 * incrementing this counter, without ever yielding/delaying, so it only runs on ticks
 * nothing higher-priority is ready for. */
static volatile uint32_t g_busy_counter    = 0U;
static volatile bool     g_busy_should_run = true;

#define TEST_BURST_ITERATIONS 200000UL

/* Shared between two equal-priority tasks in test_context_switch_timing(): each increments
 * this once per loop turn, then yields - so its total over a fixed window is (approximately)
 * the number of context switches that occurred. */
static volatile uint32_t g_switch_count      = 0U;
static volatile bool     g_switch_should_run = true;

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static os_mutex_t g_mutex;
#endif

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
static os_semaphore_t g_sync_sem;   /* helper -> main "ready" signal, reused across sections */
static os_semaphore_t g_bin_sem;
static os_semaphore_t g_count_sem;
#endif

#if (OS_CONFIG_QUEUE_ENABLE == 1U)
static os_queue_t g_queue;
static uint32_t   g_queue_buf[3];
#endif

#if (OS_CONFIG_EVENT_ENABLE == 1U)
static os_event_group_t g_event;
#endif

#if (OS_CONFIG_TIMER_ENABLE == 1U)
static os_timer_t        g_timer_oneshot;
static os_timer_t        g_timer_periodic;
static volatile uint32_t g_oneshot_fired  = 0U;
static volatile uint32_t g_periodic_fired = 0U;
#endif

#if (OS_CONFIG_WORK_ENABLE == 1U)
static os_work_t         g_work;
static volatile bool     g_work_ran       = false;
static volatile uint32_t g_work_run_count = 0U;
#endif

typedef enum
{
    HELPER_NONE = 0,
    HELPER_MUTEX_HOLD,
    HELPER_SEM_GIVE_AFTER,
    HELPER_EVENT_SET_AFTER,
    HELPER_QUEUE_SEND_AFTER,

} helper_role_t;

typedef struct
{
    helper_role_t role;
    uint32_t      hold_ms;
    uint32_t      bits;
    uint32_t      value;

} helper_ctx_t;

static helper_ctx_t g_helper_ctx;

/*
 * ***********************************************************************************************************
 * Combined-scenario context types and objects (see "Integration / Combined Scenarios" below)
 * ***********************************************************************************************************
 *
 * Unlike the single-primitive tests above (one helper task, one role at a time via
 * g_helper_ctx), these run several DIFFERENT tasks concurrently, each with its own behavior -
 * so each gets its own context struct, passed through OS_TASK_CONFIG's context pointer instead
 * of the shared dispatch-by-role pattern.
*/

#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_MUTEX_ENABLE == 1U)
#define TEST_PIPELINE_ITEMS_PER_PRODUCER 6U
#define TEST_PIPELINE_TOTAL_ITEMS        (2U * TEST_PIPELINE_ITEMS_PER_PRODUCER)

typedef struct
{
    uint32_t base_value;
    uint32_t count;

} test_producer_ctx_t;

static test_producer_ctx_t g_producer_ctx[2];
static os_mutex_t          g_pipeline_mutex;
static volatile uint32_t   g_pipeline_total;
static volatile uint32_t   g_pipeline_processed;
#endif

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
typedef struct
{
    uint32_t priority_tag;

} test_prio_ctx_t;

static test_prio_ctx_t   g_prio_ctx[3];
static os_mutex_t        g_prio_mutex;
static volatile uint32_t g_prio_order[3];
static volatile uint32_t g_prio_order_count;
#endif

#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_EVENT_ENABLE == 1U)
typedef struct
{
    uint32_t bit;
    uint32_t value;
    uint32_t work_ms;

} test_fanin_ctx_t;

static test_fanin_ctx_t g_fanin_ctx[3];
#endif

#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEMAPHORE_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && \
    (OS_CONFIG_EVENT_ENABLE == 1U) && (OS_CONFIG_ALLOC_ENABLE == 1U)
/* Concurrent multi-primitive stress/soak (see "Stress/Soak" below): unlike every scenario
 * above, which runs a small fixed handful of tasks each doing ONE thing, this runs
 * OS_TEST_STRESS_WORKER_COUNT tasks at distinct priorities that each hit a mutex, a
 * deliberately under-provisioned semaphore and queue, an event group, and the kernel heap -
 * all at once, repeatedly, for many iterations, then check hard invariants instead of just
 * "the call returned OK". Bump OS_TEST_STRESS_ITERATIONS for a longer soak run; the default
 * is sized to add at most a couple of seconds to a boot-time log, not to replace a real
 * multi-hour soak. */
#define OS_TEST_STRESS_WORKER_COUNT   4U
#define OS_TEST_STRESS_ITERATIONS     300U
#define OS_TEST_STRESS_SEM_MAX        2U    /* < worker count: forces real blocking/timeouts */
#define OS_TEST_STRESS_QUEUE_CAPACITY 3U    /* < worker count: forces real FULL/EMPTY paths  */

typedef struct
{
    uint32_t worker_id;
    uint32_t prng_state; /* xorshift32 stream, seeded distinctly per worker; never 0 */

} test_stress_ctx_t;

static test_stress_ctx_t g_stress_ctx[OS_TEST_STRESS_WORKER_COUNT];
static volatile uint32_t g_stress_done[OS_TEST_STRESS_WORKER_COUNT];        /* iterations completed   */
static volatile uint32_t g_stress_mutex_hits[OS_TEST_STRESS_WORKER_COUNT];  /* successful mutex locks */
static volatile bool     g_stress_corrupt[OS_TEST_STRESS_WORKER_COUNT];    /* heap/queue corruption seen */
static size_t            g_stress_watermark[OS_TEST_STRESS_WORKER_COUNT];  /* self-reported stack watermark */

static os_mutex_t        g_stress_mutex;
static volatile uint32_t g_stress_shared_counter; /* protected exclusively by g_stress_mutex */

static os_semaphore_t    g_stress_sem;
static os_event_group_t  g_stress_event;
static os_queue_t        g_stress_queue;
static uint32_t          g_stress_queue_buf[OS_TEST_STRESS_QUEUE_CAPACITY];
#endif

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static bool      test_wait_inactive(const os_task_t *task, uint32_t timeout_ms);
static void      test_worker_entry(void *context);
static void      test_self_pause_worker_entry(void *context);
static void      test_helper_entry(void *context);
#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
static os_status test_spawn_helper(helper_role_t role, uint32_t hold_ms, uint32_t bits, uint32_t value);
#endif

static void test_kernel_core(void);
static void test_delay(void);
static void test_critical_section(void);
static void test_task_lifecycle(void);
static void test_priority_preemption(void);
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static void test_mutex(void);
#endif
#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
static void test_semaphore(void);
#endif
#if (OS_CONFIG_QUEUE_ENABLE == 1U)
static void test_queue(void);
#endif
#if (OS_CONFIG_EVENT_ENABLE == 1U)
static void test_event_group(void);
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
static void test_timer(void);
#endif
#if (OS_CONFIG_WORK_ENABLE == 1U)
static void test_work(void);
#endif
#if (OS_CONFIG_ALLOC_ENABLE == 1U)
static void test_alloc(void);
#endif
#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
static void test_stack_watermark(void);
#endif
#if (OS_CONFIG_CPU_USAGE_ENABLE == 1U)
static void test_cpu_usage(void);
#endif
static void test_task_footprint(void);
static void test_context_switch_timing(void);
static void test_tickless_hooks(void);
static void test_list(void);
static void test_unsupported_features(void);
#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_MUTEX_ENABLE == 1U)
static void test_pipeline_producer_entry(void *context);
static void test_pipeline_consumer_entry(void *context);
static void test_pipeline(void);
#endif
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static void test_prio_waiter_entry(void *context);
static void test_mutex_priority_ordering(void);
#endif
#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_EVENT_ENABLE == 1U)
static void test_fanin_worker_entry(void *context);
static void test_event_queue_fanin(void);
#endif
#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEMAPHORE_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && \
    (OS_CONFIG_EVENT_ENABLE == 1U) && (OS_CONFIG_ALLOC_ENABLE == 1U)
static uint32_t  test_stress_prng_next(uint32_t *state);
static void      test_stress_worker_entry(void *context);
static void      test_stress_soak(void);
#endif
static void      test_stress_task_churn(void);
#if (OS_CONFIG_TIMER_ENABLE == 1U)
static void      test_stress_timer_churn(void);
#endif

/*
 * ***********************************************************************************************************
 * Shared helpers
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Poll (bounded) until a task reports INACTIVE, i.e. it has fully self-terminated
 *        and its stack is free to reuse for the next helper.
 */
static bool test_wait_inactive(const os_task_t *task, uint32_t timeout_ms)
{
    uint32_t start = os_tick_get();

    while (os_task_state_get(task) != OS_TASK_STATE_INACTIVE)
    {
        if ((os_tick_get() - start) > OS_TICKS_FROM_MS(timeout_ms))
        {
            return false;
        }

        (void)os_delay_ms(5U);
    }

    return true;
}

/******************************************************************************************************/
static void test_worker_entry(void *context)
{
    (void)context;

    while (g_worker_should_run)
    {
        g_worker_counter++;
        os_task_yield();
    }
}

/******************************************************************************************************/
/**
 * @brief Busy-spins incrementing g_busy_counter until told to stop - never yields or delays, so
 *        it only gets CPU time on ticks nothing higher-priority is ready for. Shared by
 *        test_priority_preemption() and test_cpu_usage().
 */
static void test_busy_spin_entry(void *context)
{
    (void)context;

    while (g_busy_should_run)
    {
        g_busy_counter++;
    }
}

/******************************************************************************************************/
/**
 * @brief Burns a fixed number of cycles then returns (self-exiting) - never yields, delays, or
 *        calls any blocking kernel API, so for its whole run nothing at an equal or lower
 *        priority can execute. Used by test_priority_preemption() to prove strict priority
 *        ordering, not just "eventually runs".
 */
static void test_burst_spin_entry(void *context)
{
    volatile uint32_t i;

    (void)context;

    for (i = 0U; i < TEST_BURST_ITERATIONS; i++)
    {
        /* Burn cycles; the loop body is intentionally empty. */
    }
}

/******************************************************************************************************/
/**
 * @brief Increments g_switch_count then immediately yields, in a loop, until told to stop.
 *        Run on two equal-priority tasks at once (see test_context_switch_timing()), they
 *        ping-pong the CPU between them - each turn is one context switch in, so the total
 *        count over a fixed window approximates how many switches occurred.
 */
static void test_switch_ping_entry(void *context)
{
    (void)context;

    while (g_switch_should_run)
    {
        g_switch_count++;
        os_task_yield();
    }
}

/******************************************************************************************************/
/**
 * @brief Worker body for the self-pause test: waits briefly, pauses itself (NULL means the
 *        calling task), then - once resumed by another task - proves it by setting a sentinel.
 */
static void test_self_pause_worker_entry(void *context)
{
    (void)context;

    (void)os_delay_ms(20U);
    (void)os_task_pause(NULL);
    /* execution resumes here once another task calls os_task_start() on us */
    g_worker_counter = 42U;
}

/******************************************************************************************************/
/**
 * @brief Generic helper task body: reads g_helper_ctx (set by test_spawn_helper before create)
 *        to decide what to do, then returns - the port auto-deletes the task on return.
 */
static void test_helper_entry(void *context)
{
    (void)context;

    switch (g_helper_ctx.role)
    {
#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    case HELPER_MUTEX_HOLD:
        (void)os_mutex_lock(&g_mutex, OS_WAIT_FOREVER);
        (void)os_semaphore_give(&g_sync_sem);
        (void)os_delay_ms(g_helper_ctx.hold_ms);
        (void)os_mutex_unlock(&g_mutex);
        break;
#endif

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    case HELPER_SEM_GIVE_AFTER:
        (void)os_delay_ms(g_helper_ctx.hold_ms);
        (void)os_semaphore_give(&g_count_sem);
        break;
#endif

#if (OS_CONFIG_EVENT_ENABLE == 1U)
    case HELPER_EVENT_SET_AFTER:
        (void)os_delay_ms(g_helper_ctx.hold_ms);
        (void)os_event_group_set_bits(&g_event, g_helper_ctx.bits);
        break;
#endif

#if (OS_CONFIG_QUEUE_ENABLE == 1U)
    case HELPER_QUEUE_SEND_AFTER:
        (void)os_delay_ms(g_helper_ctx.hold_ms);
        (void)os_queue_send(&g_queue, &g_helper_ctx.value, OS_WAIT_FOREVER);
        break;
#endif

    default:
        break;
    }
}

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
/******************************************************************************************************/
static os_status test_spawn_helper(helper_role_t role, uint32_t hold_ms, uint32_t bits, uint32_t value)
{
    os_status status;

    g_helper_ctx.role    = role;
    g_helper_ctx.hold_ms = hold_ms;
    g_helper_ctx.bits    = bits;
    g_helper_ctx.value   = value;

    status = os_task_create(&helper, OS_TASK_CONFIG(helper, test_helper_entry, NULL, 3U));
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    return os_task_start(&helper);
}
#endif /* OS_CONFIG_SEMAPHORE_ENABLE */

/*
 * ***********************************************************************************************************
 * Kernel core: lifecycle, tick, delay, critical sections
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void test_kernel_core(void)
{
    uint32_t t0;
    uint32_t t1;

    test_print_section("Kernel / Tick");

    AHURA_TEST_CHECK(os_kernel_is_running(), "os_kernel_is_running() is true once a task is executing");

    t0 = os_tick_get();
    (void)os_delay_ms(20U);
    t1 = os_tick_get();
    AHURA_TEST_CHECK((t1 - t0) >= OS_TICKS_FROM_MS(20U), "os_tick_get() advances with time (delta=%lu ticks)",
                      (unsigned long)(t1 - t0));
}

/******************************************************************************************************/
static void test_delay(void)
{
    os_status status;
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;

    test_print_section("Delay APIs");

    /* Capture status/t1 before any AHURA_TEST_CHECK() runs: its printf() on a match blocks on
     * a polled UART transmit (~3 ticks/line at 115200 baud) - checking status inline between
     * t0 and t1 would fold that print time into the measured delay. */
    t0     = os_tick_get();
    status = os_delay_ms(50U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "os_delay_ms(50) returns OK");
    AHURA_TEST_CHECK((delta >= 50U) && (delta <= 65U), "os_delay_ms(50) elapsed %lu ticks (expected ~50)",
                      (unsigned long)delta);

    t0     = os_tick_get();
    status = os_delay_us(3000U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "os_delay_us(3000) returns OK");
    AHURA_TEST_CHECK((delta >= 2U) && (delta <= 10U), "os_delay_us(3000) elapsed %lu ticks (expected ~3)",
                      (unsigned long)delta);

    t0     = os_tick_get();
    status = os_delay_s(1U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "os_delay_s(1) returns OK");
    AHURA_TEST_CHECK((delta >= 1000U) && (delta <= 1060U), "os_delay_s(1) elapsed %lu ticks (expected ~1000)",
                      (unsigned long)delta);
}

/******************************************************************************************************/
static void test_critical_section(void)
{
    test_print_section("Critical Sections");

    /* os_arch_kernel_mask_active reads PRIMASK or BASEPRI depending on
     * OS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY, so the checks hold in both
     * kernel mask modes. */
    AHURA_TEST_CHECK(os_arch_kernel_mask_active() == 0U, "the kernel mask is lowered before entering a critical section");

    os_critical_enter();
    AHURA_TEST_CHECK(os_arch_kernel_mask_active() != 0U, "os_critical_enter() raises the kernel mask");

    os_critical_enter(); /* nested */
    AHURA_TEST_CHECK(os_arch_kernel_mask_active() != 0U, "a nested os_critical_enter() keeps the kernel mask raised");

    os_critical_exit(); /* inner exit: outer level still held */
    AHURA_TEST_CHECK(os_arch_kernel_mask_active() != 0U, "exiting the inner level keeps the kernel mask raised (nesting works)");

    os_critical_exit(); /* outer exit */
    AHURA_TEST_CHECK(os_arch_kernel_mask_active() == 0U, "the matching outer os_critical_exit() lowers the kernel mask");
}

/******************************************************************************************************/
static void test_task_lifecycle(void)
{
    os_task_config_t cfg;
    os_status        status;
    uint32_t         snapshot;

    test_print_section("Task Lifecycle");

    /* --- Reject invalid creation parameters (should not touch any handle). --- */
    cfg = *OS_TASK_CONFIG(helper, test_worker_entry, NULL, 1U);

    cfg.priority = 0U;
    AHURA_TEST_CHECK(os_task_create(&helper, &cfg) == OS_STATUS_INVALID_ARG,
                      "os_task_create() rejects priority 0 (idle-reserved)");

    cfg.priority = OS_TASK_PRIO_MAX;
    AHURA_TEST_CHECK(os_task_create(&helper, &cfg) == OS_STATUS_INVALID_ARG,
                      "os_task_create() rejects priority %u (kernel-reserved)", (unsigned)OS_TASK_PRIO_MAX);

    cfg.priority    = OS_TASK_PRIO_USER_MIN;
    cfg.stack_bytes = OS_CONFIG_MIN_STACK_SIZE - 8U;
    AHURA_TEST_CHECK(os_task_create(&helper, &cfg) == OS_STATUS_INVALID_ARG,
                      "os_task_create() rejects a stack smaller than OS_CONFIG_MIN_STACK_SIZE");

    cfg.stack_bytes  = sizeof(helper_STACK) - 8U;
    cfg.stack_memory = &helper_STACK[1];
    AHURA_TEST_CHECK(os_task_create(&helper, &cfg) == OS_STATUS_INVALID_ARG,
                      "os_task_create() rejects a misaligned stack pointer");

    /* --- Real worker: create / start / observe / pause / resume / delete. --- */
    g_worker_counter    = 0U;
    g_worker_should_run = true;

    status = os_task_create(&worker, OS_TASK_CONFIG(worker, test_worker_entry, NULL, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "os_task_create() creates the worker task");
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_SUSPENDED,
                      "a created-but-not-started task reports SUSPENDED");

    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK, "os_task_start() starts the worker task");
    (void)os_delay_ms(20U);
    AHURA_TEST_CHECK(g_worker_counter > 0U, "worker task actually executed (counter=%lu)",
                      (unsigned long)g_worker_counter);
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_READY,
                      "a lower-priority runnable task reports READY while this task runs");

    AHURA_TEST_CHECK(os_task_pause(&worker) == OS_STATUS_OK, "os_task_pause() suspends the worker task");
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_SUSPENDED, "paused task reports SUSPENDED");
    snapshot = g_worker_counter;
    (void)os_delay_ms(20U);
    AHURA_TEST_CHECK(g_worker_counter == snapshot, "counter is frozen while the worker is paused");

    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK, "os_task_start() resumes a paused task");
    (void)os_delay_ms(20U);
    AHURA_TEST_CHECK(g_worker_counter > snapshot, "counter resumes advancing after os_task_start()");

    AHURA_TEST_CHECK(os_task_delete(&worker) == OS_STATUS_OK, "os_task_delete() deletes the live worker task");
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_INACTIVE,
                      "a deleted task's handle reports INACTIVE");
    snapshot = g_worker_counter;
    (void)os_delay_ms(20U);
    AHURA_TEST_CHECK(g_worker_counter == snapshot, "counter is frozen after deletion (worker truly stopped)");

    /* --- NULL means "current task": the worker pauses itself; we resume it. --- */
    g_worker_counter = 0U;
    status = os_task_create(&worker, OS_TASK_CONFIG(worker, test_self_pause_worker_entry, NULL, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "worker task re-created for the self-pause test");
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK, "os_task_start() starts it");

    (void)os_delay_ms(40U); /* let it reach os_task_pause(NULL) */
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_SUSPENDED,
                      "os_task_pause(NULL) suspends the calling task itself");

    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK,
                      "os_task_start() resumes a task that paused itself");
    (void)os_delay_ms(20U);
    AHURA_TEST_CHECK(g_worker_counter == 42U, "the resumed task continued executing past its self-pause point");

    /* test_self_pause_worker_entry() already returned above (auto-exiting via the arch port's
     * os_task_exit() trampoline) - no explicit os_task_delete() here, that would fail with
     * INVALID_ARG since the slot is already freed. Just confirm the self-exit completed. */
    AHURA_TEST_CHECK(test_wait_inactive(&worker, 200U), "the resumed worker terminates cleanly on its own");
}

/*
 * ***********************************************************************************************************
 * Priority-based preemption
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Proves strict priority ordering, not just "eventually runs": a lower-priority task
 *        spinning without ever yielding is fully starved for as long as a higher-priority task
 *        is ready, and resumes the instant that higher-priority task is gone.
 */
static void test_priority_preemption(void)
{
    uint32_t  snapshot_before;
    uint32_t  snapshot_immediate;
    uint32_t  snapshot_after;
    os_status status;

    test_print_section("Priority-Based Preemption");

    g_busy_counter    = 0U;
    g_busy_should_run = true;
    status = os_task_create(&worker, OS_TASK_CONFIG(worker, test_busy_spin_entry, NULL, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "low-priority spinner task created (priority 1)");
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK, "low-priority spinner started");

    (void)os_delay_ms(20U);
    snapshot_before = g_busy_counter;
    AHURA_TEST_CHECK(snapshot_before > 0U,
                      "the low-priority spinner gets CPU time when nothing outranks it (count=%lu)",
                      (unsigned long)snapshot_before);

    /* A task at a strictly higher priority than both the spinner and this test task never
     * yields/delays for its whole burst - so the spinner cannot possibly run until it is gone. */
    status = os_task_create(&helper, OS_TASK_CONFIG(helper, test_burst_spin_entry, NULL,
                                                            OS_CONFIG_TEST_PRIORITY + 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "higher-priority burst task created (priority %u)",
                      (unsigned)(OS_CONFIG_TEST_PRIORITY + 1U));

    AHURA_TEST_CHECK(os_task_start(&helper) == OS_STATUS_OK, "higher-priority burst task started");
    snapshot_immediate = g_busy_counter;
    AHURA_TEST_CHECK(snapshot_immediate == snapshot_before,
                      "the spinner has not advanced right after the higher-priority task starts (count=%lu)",
                      (unsigned long)snapshot_immediate);

    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U),
                      "the higher-priority burst task ran to completion and self-terminated");

    (void)os_delay_ms(10U);
    snapshot_after = g_busy_counter;
    AHURA_TEST_CHECK(snapshot_after > snapshot_before,
                      "the spinner resumes running once the higher-priority task is gone (count=%lu)",
                      (unsigned long)snapshot_after);

    g_busy_should_run = false;
    AHURA_TEST_CHECK(test_wait_inactive(&worker, 200U), "low-priority spinner stops cleanly");
}

/*
 * ***********************************************************************************************************
 * Mutex
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
/******************************************************************************************************/
static void test_mutex(void)
{
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;
    os_status status;

    test_print_section("Mutex");

    AHURA_TEST_CHECK(os_mutex_init(&g_mutex) == OS_STATUS_OK, "os_mutex_init() succeeds");
    AHURA_TEST_CHECK(os_mutex_unlock(&g_mutex) == OS_STATUS_ERROR, "unlocking a free mutex returns ERROR");

    AHURA_TEST_CHECK(os_mutex_lock(&g_mutex, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "os_mutex_lock() acquires a free mutex");
    AHURA_TEST_CHECK(os_mutex_try_lock(&g_mutex) == OS_STATUS_BUSY,
                      "re-locking from the owner fails BUSY (not recursive)");
    AHURA_TEST_CHECK(os_mutex_unlock(&g_mutex) == OS_STATUS_OK, "owner os_mutex_unlock() releases the mutex");

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    /* Contention: a helper task holds the mutex for 150 ms. */
    (void)os_semaphore_init(&g_sync_sem, 0U, 1U);
    AHURA_TEST_CHECK(test_spawn_helper(HELPER_MUTEX_HOLD, 150U, 0U, 0U) == OS_STATUS_OK,
                      "helper task spawned to hold the mutex");
    AHURA_TEST_CHECK(os_semaphore_take(&g_sync_sem, 200U) == OS_STATUS_OK, "helper signals once it holds the mutex");

    AHURA_TEST_CHECK(os_mutex_try_lock(&g_mutex) == OS_STATUS_BUSY,
                      "os_mutex_try_lock() fails while another task holds the mutex");
    AHURA_TEST_CHECK(os_mutex_unlock(&g_mutex) == OS_STATUS_NOT_OWNER,
                      "unlocking a mutex owned by another task returns NOT_OWNER");

    t0     = os_tick_get();
    status = os_mutex_lock(&g_mutex, 500U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "blocking os_mutex_lock() succeeds once the holder releases it");
    AHURA_TEST_CHECK((delta >= 100U) && (delta <= 250U), "blocking lock woke ~when the holder unlocked (%lu ticks)",
                      (unsigned long)delta);

    AHURA_TEST_CHECK(os_mutex_unlock(&g_mutex) == OS_STATUS_OK, "final os_mutex_unlock() releases the mutex");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "mutex-holder helper task terminated cleanly");
#endif
}
#endif /* OS_CONFIG_MUTEX_ENABLE */

/*
 * ***********************************************************************************************************
 * Semaphore
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
/******************************************************************************************************/
static void test_semaphore(void)
{
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;
    os_status status;

    test_print_section("Semaphore");

    AHURA_TEST_CHECK(os_semaphore_init(&g_bin_sem, 0U, 1U) == OS_STATUS_OK,
                      "os_semaphore_init() creates a binary semaphore (0/1)");
    AHURA_TEST_CHECK(os_semaphore_take(&g_bin_sem, OS_WAIT_NOTHING) == OS_STATUS_EMPTY,
                      "take on an empty semaphore with OS_WAIT_NOTHING returns EMPTY");
    AHURA_TEST_CHECK(os_semaphore_give(&g_bin_sem) == OS_STATUS_OK, "os_semaphore_give() adds a token");
    AHURA_TEST_CHECK(os_semaphore_give(&g_bin_sem) == OS_STATUS_FULL, "giving beyond max_count returns FULL");
    AHURA_TEST_CHECK(os_semaphore_take(&g_bin_sem, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "take succeeds once a token is available");

    t0     = os_tick_get();
    status = os_semaphore_take(&g_bin_sem, 100U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_STATUS_TIMEOUT, "take on an empty semaphore times out");
    AHURA_TEST_CHECK((delta >= 95U) && (delta <= 150U), "timeout elapsed ~100 ticks (%lu)", (unsigned long)delta);

    AHURA_TEST_CHECK(os_semaphore_init(&g_count_sem, 0U, 3U) == OS_STATUS_OK,
                      "os_semaphore_init() creates a counting semaphore (0/3)");
    AHURA_TEST_CHECK(test_spawn_helper(HELPER_SEM_GIVE_AFTER, 80U, 0U, 0U) == OS_STATUS_OK,
                      "helper spawned to give the counting semaphore after 80 ms");

    t0     = os_tick_get();
    status = os_semaphore_take(&g_count_sem, 500U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "blocking take succeeds once the helper gives");
    AHURA_TEST_CHECK((delta >= 70U) && (delta <= 200U), "take woke ~when the helper gave (%lu ticks)",
                      (unsigned long)delta);
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "semaphore-giver helper task terminated cleanly");
}
#endif /* OS_CONFIG_SEMAPHORE_ENABLE */

/*
 * ***********************************************************************************************************
 * Queue
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_QUEUE_ENABLE == 1U)
/******************************************************************************************************/
static void test_queue(void)
{
    uint32_t  items[3] = { 0 };
    uint32_t  value;
    uint32_t  i;
    bool      fifo_ok = true;
    os_status status;
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;

    test_print_section("Queue");

    AHURA_TEST_CHECK(os_queue_init(&g_queue, g_queue_buf, sizeof(uint32_t), 3U) == OS_STATUS_OK,
                      "os_queue_init() creates a 3-slot uint32 queue");
    AHURA_TEST_CHECK(os_queue_count_get(&g_queue) == 0U, "a fresh queue reports 0 items");
    AHURA_TEST_CHECK(os_queue_receive(&g_queue, &value, OS_WAIT_NOTHING) == OS_STATUS_EMPTY,
                      "receive on an empty queue with OS_WAIT_NOTHING returns EMPTY");

    for (i = 0U; i < 3U; i++)
    {
        AHURA_TEST_CHECK(os_queue_send(&g_queue, &i, OS_WAIT_NOTHING) == OS_STATUS_OK,
                          "send #%lu succeeds while the queue has room", (unsigned long)i);
    }
    AHURA_TEST_CHECK(os_queue_count_get(&g_queue) == 3U, "queue count reports 3/3 full");

    value = 99U;
    AHURA_TEST_CHECK(os_queue_send(&g_queue, &value, OS_WAIT_NOTHING) == OS_STATUS_FULL,
                      "send on a full queue with OS_WAIT_NOTHING returns FULL");

    for (i = 0U; i < 3U; i++)
    {
        status = os_queue_receive(&g_queue, &items[i], OS_WAIT_NOTHING);
        if ((status != OS_STATUS_OK) || (items[i] != i))
        {
            fifo_ok = false;
        }
    }
    AHURA_TEST_CHECK(fifo_ok, "queue preserves FIFO order (got %lu,%lu,%lu)",
                      (unsigned long)items[0], (unsigned long)items[1], (unsigned long)items[2]);

    t0     = os_tick_get();
    status = os_queue_receive(&g_queue, &value, 100U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_STATUS_TIMEOUT, "receive on an empty queue times out");
    AHURA_TEST_CHECK((delta >= 95U) && (delta <= 150U), "timeout elapsed ~100 ticks (%lu)", (unsigned long)delta);

    AHURA_TEST_CHECK(test_spawn_helper(HELPER_QUEUE_SEND_AFTER, 80U, 0U, 42U) == OS_STATUS_OK,
                      "helper spawned to send item 42 after 80 ms");
    t0     = os_tick_get();
    status = os_queue_receive(&g_queue, &value, 500U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK((status == OS_STATUS_OK) && (value == 42U),
                      "blocking receive gets the helper's item (value=%lu)", (unsigned long)value);
    AHURA_TEST_CHECK((delta >= 70U) && (delta <= 200U), "receive woke ~when the helper sent (%lu ticks)",
                      (unsigned long)delta);
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "queue-sender helper task terminated cleanly");
}
#endif /* OS_CONFIG_QUEUE_ENABLE */

/*
 * ***********************************************************************************************************
 * Event group
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_EVENT_ENABLE == 1U)
/******************************************************************************************************/
static void test_event_group(void)
{
    uint32_t  matched;
    os_status status;
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;

    test_print_section("Event Group");

    AHURA_TEST_CHECK(os_event_group_init(&g_event) == OS_STATUS_OK, "os_event_group_init() succeeds");

    matched = 0xFFFFFFFFU;
    AHURA_TEST_CHECK(os_event_group_wait_bits(&g_event, 0x03U, false, false, &matched, OS_WAIT_NOTHING) == OS_STATUS_BUSY,
                      "wait-any on unset bits with OS_WAIT_NOTHING returns BUSY");
    AHURA_TEST_CHECK(matched == 0U, "matched_bits reports 0 when nothing matched");

    AHURA_TEST_CHECK(os_event_group_set_bits(&g_event, 0x01U) == OS_STATUS_OK,
                      "os_event_group_set_bits(0x01) succeeds");
    AHURA_TEST_CHECK(os_event_group_wait_bits(&g_event, 0x03U, false, false, &matched, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "wait-any matches once one of the requested bits is set");
    AHURA_TEST_CHECK(matched == 0x01U, "matched_bits reports the intersecting bits (0x%02lx)",
                      (unsigned long)matched);

    AHURA_TEST_CHECK(os_event_group_wait_bits(&g_event, 0x03U, true, false, &matched, OS_WAIT_NOTHING) == OS_STATUS_BUSY,
                      "wait-all is still BUSY while only some requested bits are set");

    AHURA_TEST_CHECK(os_event_group_wait_bits(&g_event, 0x01U, false, true, &matched, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "wait-any with clear_on_exit consumes the matched bit");
    AHURA_TEST_CHECK(os_event_group_wait_bits(&g_event, 0x01U, false, false, &matched, OS_WAIT_NOTHING) == OS_STATUS_BUSY,
                      "a consumed (atomically cleared) bit no longer matches");

    AHURA_TEST_CHECK(test_spawn_helper(HELPER_EVENT_SET_AFTER, 80U, 0x06U, 0U) == OS_STATUS_OK,
                      "helper spawned to set bits 0x06 after 80 ms");
    t0     = os_tick_get();
    status = os_event_group_wait_bits(&g_event, 0x06U, true, false, &matched, 500U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK((status == OS_STATUS_OK) && (matched == 0x06U),
                      "wait-all matches once the helper sets both bits (matched=0x%02lx)", (unsigned long)matched);
    AHURA_TEST_CHECK((delta >= 70U) && (delta <= 200U), "wait woke ~when the helper set the bits (%lu ticks)",
                      (unsigned long)delta);
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "event-setter helper task terminated cleanly");
}
#endif /* OS_CONFIG_EVENT_ENABLE */

/*
 * ***********************************************************************************************************
 * Software timer
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TIMER_ENABLE == 1U)
/******************************************************************************************************/
static void timer_oneshot_cb(void *context)
{
    (void)context;
    g_oneshot_fired++;
}

/******************************************************************************************************/
static void timer_periodic_cb(void *context)
{
    (void)context;
    g_periodic_fired++;
}

/******************************************************************************************************/
static void test_timer(void)
{
    uint32_t snapshot;

    test_print_section("Software Timer");

    g_oneshot_fired = 0U;
    AHURA_TEST_CHECK(os_timer_init(&g_timer_oneshot, OS_TICKS_FROM_MS(50U), OS_TIMER_MODE_ONE_SHOT, timer_oneshot_cb,
                                    NULL) == OS_STATUS_OK,
                      "os_timer_init() configures a one-shot timer (50 ms)");
    AHURA_TEST_CHECK(os_timer_start(&g_timer_oneshot) == OS_STATUS_OK, "os_timer_start() arms the one-shot timer");

    (void)os_delay_ms(30U);
    AHURA_TEST_CHECK(g_oneshot_fired == 0U, "one-shot timer has not fired before its period elapses");
    (void)os_delay_ms(50U);
    AHURA_TEST_CHECK(g_oneshot_fired == 1U, "one-shot timer fires exactly once (fired=%lu)",
                      (unsigned long)g_oneshot_fired);
    (void)os_delay_ms(80U);
    AHURA_TEST_CHECK(g_oneshot_fired == 1U, "one-shot timer does not fire again on its own");

    g_periodic_fired = 0U;
    AHURA_TEST_CHECK(os_timer_init(&g_timer_periodic, OS_TICKS_FROM_MS(30U), OS_TIMER_MODE_PERIODIC,
                                    timer_periodic_cb, NULL) == OS_STATUS_OK,
                      "os_timer_init() configures a periodic timer (30 ms)");
    AHURA_TEST_CHECK(os_timer_start(&g_timer_periodic) == OS_STATUS_OK, "os_timer_start() arms the periodic timer");
    (void)os_delay_ms(160U);
    AHURA_TEST_CHECK((g_periodic_fired >= 4U) && (g_periodic_fired <= 7U),
                      "periodic timer fires repeatedly (~5x expected in 160 ms, fired=%lu)",
                      (unsigned long)g_periodic_fired);

    AHURA_TEST_CHECK(os_timer_stop(&g_timer_periodic) == OS_STATUS_OK, "os_timer_stop() disarms the periodic timer");
    snapshot = g_periodic_fired;
    (void)os_delay_ms(90U);
    AHURA_TEST_CHECK(g_periodic_fired == snapshot, "no further fires after os_timer_stop()");
}
#endif /* OS_CONFIG_TIMER_ENABLE */

/*
 * ***********************************************************************************************************
 * Work queue
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_WORK_ENABLE == 1U)
/******************************************************************************************************/
static void work_handler(void *context)
{
    (void)context;
    g_work_ran = true;
    g_work_run_count++;
}

/******************************************************************************************************/
static void test_work(void)
{
    uint32_t snapshot;

    test_print_section("Work Queue");

    AHURA_TEST_CHECK(os_work_init(&g_work, work_handler, NULL) == OS_STATUS_OK, "os_work_init() succeeds");
    AHURA_TEST_CHECK(!os_work_is_pending(&g_work), "a fresh work item is not pending");

    g_work_ran = false;
    AHURA_TEST_CHECK(os_work_submit(&g_work, 0U) == OS_STATUS_OK, "os_work_submit(delay=0) accepts the item");
    (void)os_delay_ms(20U);
    AHURA_TEST_CHECK(g_work_ran, "zero-delay work runs almost immediately");

    g_work_run_count = 0U;
    AHURA_TEST_CHECK(os_work_submit(&g_work, 80U) == OS_STATUS_OK, "os_work_submit(delay=80ms) accepts the item");
    AHURA_TEST_CHECK(os_work_is_pending(&g_work), "delayed work reports pending before it runs");
    (void)os_delay_ms(30U);
    AHURA_TEST_CHECK(g_work_run_count == 0U, "delayed work has not run yet (30/80 ms)");
    (void)os_delay_ms(80U);
    AHURA_TEST_CHECK(g_work_run_count == 1U, "delayed work ran once its delay elapsed");

    AHURA_TEST_CHECK(os_work_submit(&g_work, 100U) == OS_STATUS_OK, "os_work_submit() re-armed for the cancel test");
    AHURA_TEST_CHECK(os_work_cancel(&g_work) == OS_STATUS_OK, "os_work_cancel() cancels a pending item");
    AHURA_TEST_CHECK(!os_work_is_pending(&g_work), "cancelled work is no longer pending");
    snapshot = g_work_run_count;
    (void)os_delay_ms(150U);
    AHURA_TEST_CHECK(g_work_run_count == snapshot, "cancelled work never runs");

    AHURA_TEST_CHECK(os_work_cancel(&g_work) == OS_STATUS_EMPTY, "cancelling an already-idle item returns EMPTY");
}
#endif /* OS_CONFIG_WORK_ENABLE */

/*
 * ***********************************************************************************************************
 * Kernel heap (os_mem_alloc / os_mem_free)
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
/******************************************************************************************************/
static void test_alloc(void)
{
    size_t free0;
    size_t free1;
    size_t free2;
    size_t min_free;
    void   *p1;
    void   *p2;

    test_print_section("Kernel Heap (os_mem_alloc)");

    free0 = os_mem_free_get();
    AHURA_TEST_CHECK(free0 > 0U, "heap reports free bytes at start (%lu)", (unsigned long)free0);

    p1 = os_mem_alloc(128U);
    AHURA_TEST_CHECK(p1 != NULL, "os_mem_alloc(128) succeeds");
    free1 = os_mem_free_get();
    AHURA_TEST_CHECK(free1 < free0, "free bytes decreased after alloc (%lu -> %lu)", (unsigned long)free0,
                      (unsigned long)free1);

    p2 = os_mem_alloc(64U);
    AHURA_TEST_CHECK(p2 != NULL, "a second os_mem_alloc(64) succeeds");
    AHURA_TEST_CHECK(p1 != p2, "two live allocations return distinct blocks");

    os_mem_free(p1);
    os_mem_free(p2);
    free2 = os_mem_free_get();
    AHURA_TEST_CHECK(free2 == free0, "freeing both blocks restores the original free-byte count (coalescing works)");

    min_free = os_mem_watermark_get();
    AHURA_TEST_CHECK(min_free <= free0, "watermark min-free (%lu) never exceeds the current free count (%lu)",
                      (unsigned long)min_free, (unsigned long)free0);

    AHURA_TEST_CHECK(os_mem_alloc((size_t)OS_CONFIG_HEAP_SIZE * 2U) == NULL,
                      "an allocation larger than the whole heap fails cleanly");

    os_mem_free(NULL); /* must not crash */
    AHURA_TEST_CHECK(true, "os_mem_free(NULL) is a safe no-op");
}
#endif /* OS_CONFIG_ALLOC_ENABLE */

/*
 * ***********************************************************************************************************
 * Stack watermark
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
/******************************************************************************************************/
static void test_stack_watermark(void)
{
    size_t min_free;

    test_print_section("Stack Watermark");

    AHURA_TEST_CHECK(os_task_stack_watermark_get(NULL, &min_free) == OS_STATUS_OK,
                      "os_task_stack_watermark_get(NULL) reports the calling task");
    AHURA_TEST_CHECK(min_free < OS_CONFIG_TEST_STACK_SIZE,
                      "watermark (%lu) is less than the full stack (%lu bytes)",
                      (unsigned long)min_free, (unsigned long)OS_CONFIG_TEST_STACK_SIZE);

    AHURA_TEST_CHECK(os_task_stack_watermark_get(NULL, NULL) == OS_STATUS_INVALID_ARG,
                      "a NULL output pointer is rejected");
}
#endif /* OS_CONFIG_STACK_WATERMARK_ENABLE */

/*
 * ***********************************************************************************************************
 * CPU usage
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_CPU_USAGE_ENABLE == 1U)
/******************************************************************************************************/
static void test_cpu_usage(void)
{
    uint32_t  idle_usage;
    uint32_t  busy_usage;
    os_status status;

    test_print_section("CPU Usage");

    /* Idle baseline: this task is the only thing besides tsk_main (mostly asleep) that could
     * run, and it spends the whole window blocked in os_delay_ms(), so usage should be low. */
    (void)os_cpu_usage_get(); /* reset the sampling window */
    (void)os_delay_ms(300U);
    idle_usage = os_cpu_usage_get();
    AHURA_TEST_CHECK(idle_usage <= 20U, "usage stays low while nothing is busy (%lu%%)",
                      (unsigned long)idle_usage);

    /* Busy load: a lower-priority task spins without yielding for the whole window, so it runs
     * on every tick this task would otherwise be idle for (it is itself blocked in
     * os_delay_ms() below, and outranks the spinner, so the spinner only gets what idle would
     * have gotten). */
    g_busy_counter    = 0U;
    g_busy_should_run = true;
    status = os_task_create(&worker, OS_TASK_CONFIG(worker, test_busy_spin_entry, NULL, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "busy worker task created to load the CPU (priority 1)");
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK, "busy worker task started");

    (void)os_cpu_usage_get(); /* reset the sampling window right before the load starts */
    (void)os_delay_ms(300U);
    busy_usage = os_cpu_usage_get();
    AHURA_TEST_CHECK(busy_usage >= 90U, "usage rises sharply under a busy lower-priority task (%lu%%)",
                      (unsigned long)busy_usage);
    AHURA_TEST_CHECK(g_busy_counter > 0U, "the busy worker actually made progress (count=%lu)",
                      (unsigned long)g_busy_counter);

    g_busy_should_run = false;
    AHURA_TEST_CHECK(test_wait_inactive(&worker, 200U), "busy worker task stops cleanly");
}
#endif /* OS_CONFIG_CPU_USAGE_ENABLE */

/*
 * ***********************************************************************************************************
 * Integration / Combined Scenarios: several primitives at once, driven by several concurrent
 * tasks - the single-primitive tests above each involve at most one helper task; these prove
 * the primitives compose correctly under real multi-task contention, not just in isolation.
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_MUTEX_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Sends ctx->count items (ctx->base_value .. +count-1) into the shared pipeline queue,
 *        blocking whenever it is full - one of two producers running concurrently.
 */
static void test_pipeline_producer_entry(void *context)
{
    const test_producer_ctx_t *ctx = (const test_producer_ctx_t *)context;
    uint32_t                  i;

    for (i = 0U; i < ctx->count; i++)
    {
        uint32_t value = ctx->base_value + i;

        (void)os_queue_send(&g_queue, &value, OS_WAIT_FOREVER);
    }
}

/******************************************************************************************************/
/**
 * @brief Drains the shared pipeline queue, accumulating into a mutex-protected running total -
 *        one of two consumers running concurrently, so the mutex is under real contention:
 *        if it ever failed to serialize the read-modify-write, the total would come out wrong.
 *        Stops once the known total item count has been processed (by either consumer), or
 *        after a receive timeout (the other consumer got the last item).
 */
static void test_pipeline_consumer_entry(void *context)
{
    (void)context;

    for (;;)
    {
        uint32_t  value;
        os_status status;
        bool      done;

        status = os_queue_receive(&g_queue, &value, 300U);
        if (status != OS_STATUS_OK)
        {
            break;
        }

        (void)os_mutex_lock(&g_pipeline_mutex, OS_WAIT_FOREVER);
        g_pipeline_total     += value;
        g_pipeline_processed += 1U;
        done                  = (g_pipeline_processed >= TEST_PIPELINE_TOTAL_ITEMS);
        (void)os_mutex_unlock(&g_pipeline_mutex);

        if (done)
        {
            break;
        }
    }
}

/******************************************************************************************************/
/**
 * @brief Two producers and two consumers share a queue (capacity 3, far smaller than the 12
 *        items produced, so both directions really block) and a mutex-protected accumulator.
 *        The pass criterion is an exact sum: any lost mutex update or dropped/duplicated queue
 *        item would show up as a wrong total, not just "some items arrived".
 */
static void test_pipeline(void)
{
    os_status status;
    uint32_t  expected_total = 0U;
    uint32_t  i;

    test_print_section("Combined: Queue + Mutex, 2 producers + 2 consumers");

    AHURA_TEST_CHECK(os_queue_init(&g_queue, g_queue_buf, sizeof(uint32_t), 3U) == OS_STATUS_OK,
                      "pipeline queue initialized (capacity 3, %u items will be produced)",
                      (unsigned)TEST_PIPELINE_TOTAL_ITEMS);
    AHURA_TEST_CHECK(os_mutex_init(&g_pipeline_mutex) == OS_STATUS_OK, "pipeline mutex initialized");

    g_pipeline_total     = 0U;
    g_pipeline_processed = 0U;

    g_producer_ctx[0].base_value = 0U;
    g_producer_ctx[0].count      = TEST_PIPELINE_ITEMS_PER_PRODUCER;
    g_producer_ctx[1].base_value = 100U;
    g_producer_ctx[1].count      = TEST_PIPELINE_ITEMS_PER_PRODUCER;

    for (i = 0U; i < TEST_PIPELINE_ITEMS_PER_PRODUCER; i++)
    {
        expected_total += (g_producer_ctx[0].base_value + i);
        expected_total += (g_producer_ctx[1].base_value + i);
    }

    /* Consumers at a higher priority than producers so they drain the small queue promptly,
     * keeping both producers genuinely blocking on a full queue rather than racing ahead. */
    status = os_task_create(&helper2, OS_TASK_CONFIG(helper2, test_pipeline_consumer_entry, NULL, 4U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "consumer task 1 created (priority 4)");
    status = os_task_create(&helper3, OS_TASK_CONFIG(helper3, test_pipeline_consumer_entry, NULL, 4U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "consumer task 2 created (priority 4)");
    status = os_task_create(&worker, OS_TASK_CONFIG(worker, test_pipeline_producer_entry, &g_producer_ctx[0], 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "producer task 1 created (priority 3, values 0-5)");
    status = os_task_create(&helper, OS_TASK_CONFIG(helper, test_pipeline_producer_entry, &g_producer_ctx[1], 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "producer task 2 created (priority 3, values 100-105)");

    (void)os_task_start(&helper2);
    (void)os_task_start(&helper3);
    (void)os_task_start(&worker);
    (void)os_task_start(&helper);

    AHURA_TEST_CHECK(test_wait_inactive(&worker, 1000U), "producer 1 finished sending its items");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 1000U), "producer 2 finished sending its items");
    AHURA_TEST_CHECK(test_wait_inactive(&helper2, 1000U), "consumer 1 drained and stopped");
    AHURA_TEST_CHECK(test_wait_inactive(&helper3, 1000U), "consumer 2 drained and stopped");

    AHURA_TEST_CHECK(g_pipeline_processed == TEST_PIPELINE_TOTAL_ITEMS,
                      "both consumers together processed all %u items (processed=%lu)",
                      (unsigned)TEST_PIPELINE_TOTAL_ITEMS, (unsigned long)g_pipeline_processed);
    AHURA_TEST_CHECK(g_pipeline_total == expected_total,
                      "mutex-protected total is exact under two-consumer contention (got=%lu expected=%lu)",
                      (unsigned long)g_pipeline_total, (unsigned long)expected_total);
}
#endif /* OS_CONFIG_QUEUE_ENABLE && OS_CONFIG_MUTEX_ENABLE */

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Locks g_prio_mutex (blocking until granted), records ctx->priority_tag as the next
 *        entry in the shared wake-order log, then unlocks and exits.
 */
static void test_prio_waiter_entry(void *context)
{
    const test_prio_ctx_t *ctx = (const test_prio_ctx_t *)context;

    (void)os_mutex_lock(&g_prio_mutex, OS_WAIT_FOREVER);
    g_prio_order[g_prio_order_count] = ctx->priority_tag;
    g_prio_order_count++;
    (void)os_mutex_unlock(&g_prio_mutex);
}

/******************************************************************************************************/
/**
 * @brief Three tasks at three different priorities (started low-to-high, to rule out arrival
 *        order) all block on a mutex this test task holds; releasing it must wake them
 *        highest-priority-first, not creation/arrival order - proving the mutex waiter list is
 *        genuinely priority-ordered under contention from more than one waiter (the
 *        single-waiter test_mutex() above cannot distinguish priority order from FIFO order).
 */
static void test_mutex_priority_ordering(void)
{
    os_status status;

    test_print_section("Combined: Mutex + Priority, ordered contention across 3 tasks");

    AHURA_TEST_CHECK(os_mutex_init(&g_prio_mutex) == OS_STATUS_OK, "priority-contention mutex initialized");
    AHURA_TEST_CHECK(os_mutex_lock(&g_prio_mutex, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "test task takes the mutex first, so all 3 waiters below must block");

    g_prio_order_count         = 0U;
    g_prio_ctx[0].priority_tag = 4U;
    g_prio_ctx[1].priority_tag = 5U;
    g_prio_ctx[2].priority_tag = 6U;

    status = os_task_create(&worker, OS_TASK_CONFIG(worker, test_prio_waiter_entry, &g_prio_ctx[0], 4U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "low-priority waiter created (priority 4)");
    status = os_task_create(&helper, OS_TASK_CONFIG(helper, test_prio_waiter_entry, &g_prio_ctx[1], 5U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "medium-priority waiter created (priority 5)");
    status = os_task_create(&helper2, OS_TASK_CONFIG(helper2, test_prio_waiter_entry, &g_prio_ctx[2], 6U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "high-priority waiter created (priority 6)");

    /* Start low first, high last: if the wake order below still comes out high-to-low, that
     * proves it is driven by priority, not by creation/start order. */
    (void)os_task_start(&worker);
    (void)os_task_start(&helper);
    (void)os_task_start(&helper2);

    (void)os_delay_ms(30U); /* let all 3 reach os_mutex_lock() and join the waiter list */

    AHURA_TEST_CHECK(os_mutex_unlock(&g_prio_mutex) == OS_STATUS_OK,
                      "test task releases the mutex with all 3 tasks queued");

    AHURA_TEST_CHECK(test_wait_inactive(&worker, 300U), "low-priority waiter finished");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 300U), "medium-priority waiter finished");
    AHURA_TEST_CHECK(test_wait_inactive(&helper2, 300U), "high-priority waiter finished");

    AHURA_TEST_CHECK(g_prio_order_count == 3U, "all 3 waiters recorded their turn (count=%lu)",
                      (unsigned long)g_prio_order_count);
    AHURA_TEST_CHECK((g_prio_order[0] == 6U) && (g_prio_order[1] == 5U) && (g_prio_order[2] == 4U),
                      "mutex was granted highest-priority-first, not arrival order (got %lu,%lu,%lu)",
                      (unsigned long)g_prio_order[0], (unsigned long)g_prio_order[1],
                      (unsigned long)g_prio_order[2]);
}
#endif /* OS_CONFIG_MUTEX_ENABLE */

#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_EVENT_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Waits ctx->work_ms (staggered per task so completion order is not predictable), sends
 *        ctx->value into the shared queue, then sets ctx->bit in the shared event group - one
 *        of three independent workers in a fan-out/fan-in pattern.
 */
static void test_fanin_worker_entry(void *context)
{
    const test_fanin_ctx_t *ctx = (const test_fanin_ctx_t *)context;

    (void)os_delay_ms(ctx->work_ms);
    (void)os_queue_send(&g_queue, &ctx->value, OS_WAIT_FOREVER);
    (void)os_event_group_set_bits(&g_event, ctx->bit);
}

/******************************************************************************************************/
/**
 * @brief Three tasks each do "work" for a different duration, then deliver a queue item and set
 *        their own event bit. The test task wait-alls on all 3 bits (proving the event group
 *        correctly rendezvous-es 3 independent, differently-timed setters) then drains the
 *        queue and checks the exact multiset of values arrived - order-independent, since which
 *        worker finishes first is not deterministic.
 */
static void test_event_queue_fanin(void)
{
    uint32_t  matched;
    os_status status;
    uint32_t  received[3] = { 0 };
    uint32_t  i;
    uint32_t  sum          = 0U;
    uint32_t  expected_sum;
    bool      saw[3]       = { false, false, false };

    test_print_section("Combined: Event Group + Queue, fan-out/fan-in across 3 tasks");

    AHURA_TEST_CHECK(os_queue_init(&g_queue, g_queue_buf, sizeof(uint32_t), 3U) == OS_STATUS_OK,
                      "fan-in queue initialized (capacity 3, one slot per worker)");
    AHURA_TEST_CHECK(os_event_group_init(&g_event) == OS_STATUS_OK, "fan-in event group initialized");

    g_fanin_ctx[0].bit = 0x01U; g_fanin_ctx[0].value = 10U; g_fanin_ctx[0].work_ms = 60U;
    g_fanin_ctx[1].bit = 0x02U; g_fanin_ctx[1].value = 20U; g_fanin_ctx[1].work_ms = 20U;
    g_fanin_ctx[2].bit = 0x04U; g_fanin_ctx[2].value = 30U; g_fanin_ctx[2].work_ms = 40U;
    expected_sum = g_fanin_ctx[0].value + g_fanin_ctx[1].value + g_fanin_ctx[2].value;

    status = os_task_create(&worker, OS_TASK_CONFIG(worker, test_fanin_worker_entry, &g_fanin_ctx[0], 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "fan-in worker 1 created (bit 0x01, 60 ms work)");
    status = os_task_create(&helper, OS_TASK_CONFIG(helper, test_fanin_worker_entry, &g_fanin_ctx[1], 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "fan-in worker 2 created (bit 0x02, 20 ms work)");
    status = os_task_create(&helper2, OS_TASK_CONFIG(helper2, test_fanin_worker_entry, &g_fanin_ctx[2], 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "fan-in worker 3 created (bit 0x04, 40 ms work)");

    (void)os_task_start(&worker);
    (void)os_task_start(&helper);
    (void)os_task_start(&helper2);

    status = os_event_group_wait_bits(&g_event, 0x07U, true, false, &matched, 500U);
    AHURA_TEST_CHECK((status == OS_STATUS_OK) && (matched == 0x07U),
                      "wait-all sees all 3 workers' bits despite different finish times (matched=0x%02lx)",
                      (unsigned long)matched);

    AHURA_TEST_CHECK(os_queue_count_get(&g_queue) == 3U, "queue holds exactly the 3 workers' results");

    for (i = 0U; i < 3U; i++)
    {
        AHURA_TEST_CHECK(os_queue_receive(&g_queue, &received[i], OS_WAIT_NOTHING) == OS_STATUS_OK,
                          "received fan-in result #%lu", (unsigned long)i);
        sum += received[i];

        if (received[i] == 10U) { saw[0] = true; }
        if (received[i] == 20U) { saw[1] = true; }
        if (received[i] == 30U) { saw[2] = true; }
    }

    AHURA_TEST_CHECK(sum == expected_sum, "the 3 delivered values sum correctly (got=%lu expected=%lu)",
                      (unsigned long)sum, (unsigned long)expected_sum);
    AHURA_TEST_CHECK(saw[0] && saw[1] && saw[2],
                      "all 3 distinct worker values arrived exactly once each, in any order");

    AHURA_TEST_CHECK(test_wait_inactive(&worker, 300U), "fan-in worker 1 terminated cleanly");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 300U), "fan-in worker 2 terminated cleanly");
    AHURA_TEST_CHECK(test_wait_inactive(&helper2, 300U), "fan-in worker 3 terminated cleanly");
}
#endif /* OS_CONFIG_QUEUE_ENABLE && OS_CONFIG_EVENT_ENABLE */

#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEMAPHORE_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && \
    (OS_CONFIG_EVENT_ENABLE == 1U) && (OS_CONFIG_ALLOC_ENABLE == 1U)
/*
 * ***********************************************************************************************************
 * Stress/Soak: several tasks contend on every primitive at once (see the type/object block near
 * the top of this file for OS_TEST_STRESS_* and the rationale)
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Small, fast xorshift32 PRNG - just enough spread to pick different operations and
 *        sizes per worker per iteration; not meant to be statistically strong.
 */
static uint32_t test_stress_prng_next(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;

    return x;
}

/******************************************************************************************************/
/**
 * @brief One stress worker: OS_TEST_STRESS_ITERATIONS times, pick one of 5 operations at
 *        random and do it. Every operation either self-verifies (pattern-filled heap memory
 *        read back unchanged, a received queue item decodes to a plausible sender/sequence) or
 *        feeds a counter the parent checks after every worker has finished (successful mutex
 *        locks vs. the shared counter they protect).
 */
static void test_stress_worker_entry(void *context)
{
    test_stress_ctx_t *ctx = (test_stress_ctx_t *)context;
    uint32_t           iteration;

    for (iteration = 0U; iteration < OS_TEST_STRESS_ITERATIONS; iteration++)
    {
        uint32_t pick = test_stress_prng_next(&ctx->prng_state) % 5U;

        switch (pick)
        {
            case 0U: /* mutex: protected read-modify-write; g_stress_shared_counter must end up
                      * exactly equal to the total successful locks across every worker below, or
                      * the lock let two tasks in at once and a lost update reveals it. */
            {
                if (os_mutex_lock(&g_stress_mutex, 20U) == OS_STATUS_OK)
                {
                    uint32_t before = g_stress_shared_counter;

                    os_task_yield(); /* widen the window: a broken lock would let another worker in here */
                    g_stress_shared_counter = before + 1U;
                    (void)os_mutex_unlock(&g_stress_mutex);
                    g_stress_mutex_hits[ctx->worker_id]++;
                }
                break;
            }

            case 1U: /* semaphore: take then give back, so the run is self-balancing */
            {
                if (os_semaphore_take(&g_stress_sem, 5U) == OS_STATUS_OK)
                {
                    (void)os_delay_ms(test_stress_prng_next(&ctx->prng_state) % 3U);
                    (void)os_semaphore_give(&g_stress_sem);
                }
                break;
            }

            case 2U: /* queue: send and receive both, so the queue is self-draining; a received
                      * tag that does not decode to a real sender/sequence means corruption. */
            {
                uint32_t tag = (ctx->worker_id << 16) | (iteration & 0xFFFFU);
                uint32_t received;

                if ((test_stress_prng_next(&ctx->prng_state) & 1U) != 0U)
                {
                    (void)os_queue_send(&g_stress_queue, &tag, 5U);
                }
                else if (os_queue_receive(&g_stress_queue, &received, 5U) == OS_STATUS_OK)
                {
                    uint32_t sender = received >> 16;
                    uint32_t seq    = received & 0xFFFFU;

                    if ((sender >= OS_TEST_STRESS_WORKER_COUNT) || (seq >= OS_TEST_STRESS_ITERATIONS))
                    {
                        g_stress_corrupt[ctx->worker_id] = true;
                    }
                }
                break;
            }

            case 3U: /* event group: set a couple of bits, then a short bounded wait - mainly
                      * here to add concurrent set/wait/clear-on-exit pressure on top of the rest. */
            {
                uint32_t bit     = 1UL << (test_stress_prng_next(&ctx->prng_state) % 4U);
                uint32_t matched = 0U;

                (void)os_event_group_set_bits(&g_stress_event, bit);
                (void)os_event_group_wait_bits(&g_stress_event, bit, false, true, &matched, 2U);
                break;
            }

            case 4U: /* kernel heap: alloc, pattern-fill, verify, free - catches corruption/overlap */
            default:
            {
                size_t  size = 1U + (test_stress_prng_next(&ctx->prng_state) % 64U);
                uint8_t *mem = (uint8_t *)os_mem_alloc(size);

                if (mem != NULL)
                {
                    uint8_t pattern = (uint8_t)(ctx->worker_id + iteration);
                    size_t  i;

                    for (i = 0U; i < size; i++) { mem[i] = pattern; }
                    os_task_yield(); /* widen the window for a racy allocator to let it overlap */
                    for (i = 0U; i < size; i++)
                    {
                        if (mem[i] != pattern) { g_stress_corrupt[ctx->worker_id] = true; }
                    }
                    os_mem_free(mem);
                }
                break;
            }
        }
    }

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
    (void)os_task_stack_watermark_get(NULL, &g_stress_watermark[ctx->worker_id]);
#endif

    g_stress_done[ctx->worker_id] = iteration;
}

/******************************************************************************************************/
/**
 * @brief Concurrent multi-primitive stress/soak: OS_TEST_STRESS_WORKER_COUNT tasks at distinct
 *        priorities hit a mutex, an under-provisioned semaphore and queue, an event group, and
 *        the kernel heap simultaneously and repeatedly, then the results are checked against
 *        hard invariants (exact mutex-protected counter, exact semaphore token reconciliation,
 *        no heap leak, no pattern/queue corruption) rather than just "the call returned OK".
 *        Unlike every test above, several DIFFERENT primitives are under contention from
 *        several tasks at once for many iterations, so this is the closest thing in the suite
 *        to actually shaking out a wakeup-ordering or allocator race instead of only ever
 *        exercising the one deterministic interleaving a scripted single-shot test happens to
 *        produce on a given boot.
 */
static void test_stress_soak(void)
{
    size_t    heap_before;
    size_t    heap_after;
    uint32_t  total_iterations = 0U;
    uint32_t  total_mutex_hits = 0U;
    uint32_t  drained_tokens   = 0U;
    uint32_t  leftover_items   = 0U;
    bool      any_corruption   = false;
    uint32_t  dummy;
    uint32_t  i;
    os_status status;

    test_print_section("Stress/Soak: 4 tasks contend on mutex+semaphore+queue+event+heap at once");

    AHURA_TEST_CHECK(os_mutex_init(&g_stress_mutex) == OS_STATUS_OK, "stress mutex initialized");
    AHURA_TEST_CHECK(os_semaphore_init(&g_stress_sem, OS_TEST_STRESS_SEM_MAX, OS_TEST_STRESS_SEM_MAX) == OS_STATUS_OK,
                      "stress semaphore initialized (max=%u, deliberately < %u workers)",
                      (unsigned)OS_TEST_STRESS_SEM_MAX, (unsigned)OS_TEST_STRESS_WORKER_COUNT);
    AHURA_TEST_CHECK(os_event_group_init(&g_stress_event) == OS_STATUS_OK, "stress event group initialized");
    AHURA_TEST_CHECK(os_queue_init(&g_stress_queue, g_stress_queue_buf, sizeof(uint32_t), OS_TEST_STRESS_QUEUE_CAPACITY) == OS_STATUS_OK,
                      "stress queue initialized (capacity=%u, deliberately < %u workers)",
                      (unsigned)OS_TEST_STRESS_QUEUE_CAPACITY, (unsigned)OS_TEST_STRESS_WORKER_COUNT);

    g_stress_shared_counter = 0U;
    heap_before = os_mem_free_get();

    for (i = 0U; i < OS_TEST_STRESS_WORKER_COUNT; i++)
    {
        g_stress_done[i]       = 0U;
        g_stress_corrupt[i]    = false;
        g_stress_mutex_hits[i] = 0U;
        g_stress_watermark[i]  = 0U;
        g_stress_ctx[i].worker_id  = i;
        g_stress_ctx[i].prng_state = 0x9E3779B9U ^ (i * 0x2545F491U) ^ (os_tick_get() | 1U);
    }

    status = os_task_create(&worker, OS_TASK_CONFIG(worker, test_stress_worker_entry, &g_stress_ctx[0], 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "stress worker 0 created (priority 3)");
    status = os_task_create(&helper, OS_TASK_CONFIG(helper, test_stress_worker_entry, &g_stress_ctx[1], 4U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "stress worker 1 created (priority 4)");
    status = os_task_create(&helper2, OS_TASK_CONFIG(helper2, test_stress_worker_entry, &g_stress_ctx[2], 5U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "stress worker 2 created (priority 5)");
    status = os_task_create(&helper3, OS_TASK_CONFIG(helper3, test_stress_worker_entry, &g_stress_ctx[3], 6U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "stress worker 3 created (priority 6)");

    (void)os_task_start(&worker);
    (void)os_task_start(&helper);
    (void)os_task_start(&helper2);
    (void)os_task_start(&helper3);

    AHURA_TEST_CHECK(test_wait_inactive(&worker, 15000U), "stress worker 0 terminated cleanly (no deadlock/hang)");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 15000U), "stress worker 1 terminated cleanly (no deadlock/hang)");
    AHURA_TEST_CHECK(test_wait_inactive(&helper2, 15000U), "stress worker 2 terminated cleanly (no deadlock/hang)");
    AHURA_TEST_CHECK(test_wait_inactive(&helper3, 15000U), "stress worker 3 terminated cleanly (no deadlock/hang)");

    for (i = 0U; i < OS_TEST_STRESS_WORKER_COUNT; i++)
    {
        total_iterations += g_stress_done[i];
        total_mutex_hits += g_stress_mutex_hits[i];
        any_corruption    = any_corruption || g_stress_corrupt[i];
    }

    AHURA_TEST_CHECK(total_iterations == (OS_TEST_STRESS_WORKER_COUNT * OS_TEST_STRESS_ITERATIONS),
                      "all workers completed every iteration (%lu of %lu total)",
                      (unsigned long)total_iterations, (unsigned long)(OS_TEST_STRESS_WORKER_COUNT * OS_TEST_STRESS_ITERATIONS));

    AHURA_TEST_CHECK(!any_corruption, "no worker observed corrupted heap memory or a malformed queue item");

    AHURA_TEST_CHECK(g_stress_shared_counter == total_mutex_hits,
                      "mutex gave exclusive access every time (counter=%lu, successful locks=%lu - a mismatch would mean two tasks were inside at once)",
                      (unsigned long)g_stress_shared_counter, (unsigned long)total_mutex_hits);

    while (os_semaphore_take(&g_stress_sem, OS_WAIT_NOTHING) == OS_STATUS_OK)
    {
        drained_tokens++;
    }
    AHURA_TEST_CHECK(drained_tokens == OS_TEST_STRESS_SEM_MAX,
                      "every semaphore token was given back exactly once (drained %lu of %lu)",
                      (unsigned long)drained_tokens, (unsigned long)OS_TEST_STRESS_SEM_MAX);

    while (os_queue_receive(&g_stress_queue, &dummy, OS_WAIT_NOTHING) == OS_STATUS_OK)
    {
        uint32_t sender = dummy >> 16;
        uint32_t seq    = dummy & 0xFFFFU;

        if ((sender >= OS_TEST_STRESS_WORKER_COUNT) || (seq >= OS_TEST_STRESS_ITERATIONS))
        {
            any_corruption = true;
        }

        leftover_items++;
    }
    AHURA_TEST_CHECK(!any_corruption, "every leftover queue item (if any: %lu) still decoded to a valid sender/sequence",
                      (unsigned long)leftover_items);

    AHURA_TEST_CHECK(os_mutex_try_lock(&g_stress_mutex) == OS_STATUS_OK, "stress mutex ended unlocked");
    (void)os_mutex_unlock(&g_stress_mutex);

    heap_after = os_mem_free_get();
    AHURA_TEST_CHECK(heap_after == heap_before,
                      "kernel heap has no leak after the alloc/free churn (before=%lu after=%lu bytes free)",
                      (unsigned long)heap_before, (unsigned long)heap_after);

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
    for (i = 0U; i < OS_TEST_STRESS_WORKER_COUNT; i++)
    {
        printf("  [INFO] stress worker %lu peak stack usage watermark: %lu bytes free at minimum\r\n",
               (unsigned long)i, (unsigned long)g_stress_watermark[i]);
    }
#endif

    printf("  [INFO] stress run: %u workers x %u iterations = %lu total operations\r\n",
           (unsigned)OS_TEST_STRESS_WORKER_COUNT, (unsigned)OS_TEST_STRESS_ITERATIONS, (unsigned long)total_iterations);
}
#endif /* OS_CONFIG_MUTEX_ENABLE && OS_CONFIG_SEMAPHORE_ENABLE && OS_CONFIG_QUEUE_ENABLE && OS_CONFIG_EVENT_ENABLE && OS_CONFIG_ALLOC_ENABLE */

/*
 * ***********************************************************************************************************
 * Additional targeted churn/stress tests: unlike test_stress_soak() above (several DIFFERENT
 * primitives contended by several concurrent tasks), each of these hammers ONE subsystem's
 * create/destroy or alloc/free path back-to-back, many times, in a tight loop from a single task.
 * The single-primitive tests earlier in this file only exercise create/delete or alloc/free a
 * handful of times each - not nearly enough repetition to shake out a slot-reuse bug, a list-
 * corruption bug, or a leak that only shows up after hundreds of cycles.
 * ***********************************************************************************************************
*/

#define OS_TEST_CHURN_ITERATIONS 500U

static volatile uint32_t g_churn_counter = 0U;

/******************************************************************************************************/
static void test_churn_worker_entry(void *context)
{
    (void)context;
    g_churn_counter++;
    /* returns immediately - self-exits via the arch port's os_task_exit() trampoline, freeing the
     * slot for the next iteration's os_task_create() as fast as the port allows. */
}

/******************************************************************************************************/
/**
 * @brief Creates, starts, and waits for a task to self-exit, back-to-back OS_TEST_CHURN_ITERATIONS
 *        times on the same slot - a create/run/exit/slot-reuse cycle the earlier lifecycle test
 *        only exercises a handful of times. Catches slot-reuse bugs (stale state left over from
 *        the previous occupant) or ready-list corruption that only show up under repeated churn.
 */
static void test_stress_task_churn(void)
{
    uint32_t  i;
    bool      all_created  = true;
    bool      all_started  = true;
    bool      all_finished = true;
    os_status status;

    test_print_section("Stress: rapid task create/start/exit churn");

    g_churn_counter = 0U;

    for (i = 0U; i < OS_TEST_CHURN_ITERATIONS; i++)
    {
        status = os_task_create(&worker, OS_TASK_CONFIG(worker, test_churn_worker_entry, NULL, 1U));
        if (status != OS_STATUS_OK)
        {
            all_created = false;
            break;
        }

        status = os_task_start(&worker);
        if (status != OS_STATUS_OK)
        {
            all_started = false;
            break;
        }

        if (!test_wait_inactive(&worker, 100U))
        {
            all_finished = false;
            break;
        }
    }

    AHURA_TEST_CHECK(all_created, "task slot creates cleanly on every one of %u churn cycles",
                      (unsigned)OS_TEST_CHURN_ITERATIONS);
    AHURA_TEST_CHECK(all_started, "task starts cleanly on every churn cycle");
    AHURA_TEST_CHECK(all_finished, "task self-exits and frees its slot on every churn cycle (no leak/hang)");
    AHURA_TEST_CHECK(g_churn_counter == OS_TEST_CHURN_ITERATIONS,
                      "each cycle's task body ran exactly once (counter=%lu of %lu)",
                      (unsigned long)g_churn_counter, (unsigned long)OS_TEST_CHURN_ITERATIONS);

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
    {
        size_t min_free;

        if (os_task_stack_watermark_get(&worker, &min_free) == OS_STATUS_OK)
        {
            AHURA_TEST_CHECK(min_free <= sizeof(worker_STACK),
                              "repeated slot reuse leaves a sane stack watermark (%lu / %lu bytes free)",
                              (unsigned long)min_free, (unsigned long)sizeof(worker_STACK));
        }
    }
#endif
}

#if (OS_CONFIG_TIMER_ENABLE == 1U)
#define OS_TEST_TIMER_CHURN_ITERATIONS 500U

static volatile uint32_t g_churn_timer_fired = 0U;

/******************************************************************************************************/
static void test_churn_timer_cb(void *context)
{
    (void)context;
    g_churn_timer_fired++;
}

/******************************************************************************************************/
/**
 * @brief Hammers os_timer_init()/os_timer_start()/os_timer_stop() on the same timer object back-
 *        to-back, many times, always stopping it long before its (long) period could elapse -
 *        purely to shake out add/remove bugs in the timer list under rapid churn. Finishes with
 *        one real run to prove the timer list is still healthy afterward, not just that the API
 *        calls returned OK.
 */
static void test_stress_timer_churn(void)
{
    uint32_t i;
    bool     all_ok = true;

    test_print_section("Stress: rapid timer init/start/stop churn");

    g_churn_timer_fired = 0U;

    for (i = 0U; i < OS_TEST_TIMER_CHURN_ITERATIONS; i++)
    {
        if (os_timer_init(&g_timer_oneshot, OS_TICKS_FROM_MS(1000U), OS_TIMER_MODE_ONE_SHOT, test_churn_timer_cb,
                           NULL) != OS_STATUS_OK)
        {
            all_ok = false;
            break;
        }

        if (os_timer_start(&g_timer_oneshot) != OS_STATUS_OK)
        {
            all_ok = false;
            break;
        }

        if (os_timer_stop(&g_timer_oneshot) != OS_STATUS_OK)
        {
            all_ok = false;
            break;
        }
    }

    AHURA_TEST_CHECK(all_ok, "timer init/start/stop succeeds on every one of %u rapid churn cycles",
                      (unsigned)OS_TEST_TIMER_CHURN_ITERATIONS);
    AHURA_TEST_CHECK(g_churn_timer_fired == 0U, "none of the stopped-before-expiry timers fired (fired=%lu)",
                      (unsigned long)g_churn_timer_fired);

    AHURA_TEST_CHECK(os_timer_init(&g_timer_oneshot, OS_TICKS_FROM_MS(30U), OS_TIMER_MODE_ONE_SHOT,
                                    test_churn_timer_cb, NULL) == OS_STATUS_OK,
                      "timer re-armed for a real run after the churn");
    AHURA_TEST_CHECK(os_timer_start(&g_timer_oneshot) == OS_STATUS_OK, "timer starts normally after the churn");
    (void)os_delay_ms(60U);
    AHURA_TEST_CHECK(g_churn_timer_fired == 1U, "the post-churn timer still fires correctly (fired=%lu)",
                      (unsigned long)g_churn_timer_fired);
}
#endif /* OS_CONFIG_TIMER_ENABLE */

/*
 * ***********************************************************************************************************
 * Task / stack footprint and context-switch timing (informational - no "correct" value to assert)
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Prints task sizing info: the public handle size, each configured task stack size, and
 *        actual peak stack usage (watermark) for this task and a freshly spun-up worker.
 *
 * The kernel's internal TCB struct is a private implementation detail (not exposed via ahura.h,
 * by design - see os_internal.h), so "task size" here means what the public API can actually
 * report: os_task_t's own size, the configured stack budgets, and measured watermark usage.
 */
static void test_task_footprint(void)
{
    test_print_section("Task / Stack Footprint (informational)");

    printf("  [INFO] sizeof(os_task_t) = %lu bytes (the public task handle)\r\n",
           (unsigned long)sizeof(os_task_t));
    printf("  [INFO] OS_CONFIG_MIN_STACK_SIZE       = %lu bytes\r\n", (unsigned long)OS_CONFIG_MIN_STACK_SIZE);
#if (OS_CONFIG_WORK_ENABLE == 1U)
    printf("  [INFO] OS_CONFIG_WORK_STACK_SIZE      = %lu bytes (tsk_work)\r\n",
           (unsigned long)OS_CONFIG_WORK_STACK_SIZE);
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    printf("  [INFO] OS_CONFIG_TIMER_STACK_SIZE     = %lu bytes (tsk_timer)\r\n",
           (unsigned long)OS_CONFIG_TIMER_STACK_SIZE);
#endif
    printf("  [INFO] OS_CONFIG_MAIN_TASK_STACK_SIZE = %lu bytes (tsk_main)\r\n",
           (unsigned long)OS_CONFIG_MAIN_TASK_STACK_SIZE);
    printf("  [INFO] OS_CONFIG_TEST_STACK_SIZE      = %lu bytes (tsk_test, this task)\r\n",
           (unsigned long)OS_CONFIG_TEST_STACK_SIZE);

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
    {
        size_t    self_min_free;
        size_t    worker_min_free;
        os_status status;

        if (os_task_stack_watermark_get(NULL, &self_min_free) == OS_STATUS_OK)
        {
            printf("  [INFO] tsk_test peak stack usage so far: %lu / %lu bytes (%lu%% headroom left)\r\n",
                   (unsigned long)(OS_CONFIG_TEST_STACK_SIZE - self_min_free),
                   (unsigned long)OS_CONFIG_TEST_STACK_SIZE,
                   (unsigned long)((self_min_free * 100U) / OS_CONFIG_TEST_STACK_SIZE));
        }

        /* Give a freshly created task a moment to run, then read its watermark too - the same
         * feature applied to a task other than "self". */
        g_busy_counter    = 0U;
        g_busy_should_run = true;
        status = os_task_create(&worker, OS_TASK_CONFIG(worker, test_busy_spin_entry, NULL, 1U));
        if (status == OS_STATUS_OK)
        {
            (void)os_task_start(&worker);
            (void)os_delay_ms(20U);
            g_busy_should_run = false;

            if (os_task_stack_watermark_get(&worker, &worker_min_free) == OS_STATUS_OK)
            {
                printf("  [INFO] worker task peak stack usage: %lu / %lu bytes (%lu%% headroom left)\r\n",
                       (unsigned long)(sizeof(worker_STACK) - worker_min_free),
                       (unsigned long)sizeof(worker_STACK),
                       (unsigned long)((worker_min_free * 100U) / sizeof(worker_STACK)));
            }

            (void)test_wait_inactive(&worker, 200U);
        }
    }
#else
    printf("  [SKIP] OS_CONFIG_STACK_WATERMARK_ENABLE=0: no watermark data available\r\n");
#endif
}

/******************************************************************************************************/
/**
 * @brief Estimates context-switch overhead: two equal-priority tasks ping-pong the CPU (each
 *        increments a shared counter then yields) for a fixed window; dividing the window by
 *        the total switch count gives an average, tick-resolution estimate of switch cost.
 *
 * There is no public cycle-counter API (os_test.c deliberately depends on nothing but ahura.h),
 * so this cannot report single-switch microsecond precision - only an average over many
 * thousands of switches, which is precise enough to be meaningful at 1 ms tick resolution.
 */
static void test_context_switch_timing(void)
{
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  window_ms;
    uint32_t  switches;
    os_status status;

    test_print_section("Context Switch Timing (informational, tick-resolution estimate)");

    g_switch_count      = 0U;
    g_switch_should_run = true;

    status = os_task_create(&worker, OS_TASK_CONFIG(worker, test_switch_ping_entry, NULL, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "ping task created for the switch benchmark (priority 1)");
    status = os_task_create(&helper, OS_TASK_CONFIG(helper, test_switch_ping_entry, NULL, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "pong task created for the switch benchmark (priority 1)");

    t0 = os_tick_get();
    (void)os_task_start(&worker);
    (void)os_task_start(&helper);
    (void)os_delay_ms(200U); /* let them ping-pong for a fixed window */
    g_switch_should_run = false;
    t1 = os_tick_get();

    switches  = g_switch_count;
    window_ms = t1 - t0;
    AHURA_TEST_CHECK(switches > 0U, "ping/pong tasks performed context switches (count=%lu)",
                      (unsigned long)switches);

    if (switches > 0U)
    {
        uint32_t avg_switch_us = (window_ms * 1000U) / switches;

        printf("  [INFO] ~%lu switches in %lu ms -> ~%lu us/switch average (includes loop overhead)\r\n",
               (unsigned long)switches, (unsigned long)window_ms, (unsigned long)avg_switch_us);
    }

    AHURA_TEST_CHECK(test_wait_inactive(&worker, 200U), "ping task stops cleanly");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "pong task stops cleanly");
}

/*
 * ***********************************************************************************************************
 * Tickless sleep hooks (called directly, in isolation - see the caveat printed below)
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Exercises os_tickless_pre_sleep_cb()/os_tickless_post_sleep_cb() directly, in isolation
 *        from the idle task and tick accounting.
 *
 * os_tickless_idle_process() is not yet invoked by the idle task (see the kernel README "Tickless
 * idle") - the idle task still just does a plain WFI - so OS_CONFIG_TICKLESS_ENABLE currently has
 * no other observable runtime effect. This only proves the two hooks themselves run safely and
 * quickly and compose correctly back-to-back; it is not an end-to-end tickless sleep test.
 */
static void test_tickless_hooks(void)
{
    uint32_t t0;
    uint32_t t1;

    test_print_section("Tickless Sleep Hooks (called directly, not via the idle task)");

    printf("  [INFO] os_tickless_idle_process() is not invoked by the idle task yet (see kernel\r\n"
           "         README \"Tickless idle\") - this only tests the two hooks in isolation.\r\n");

    /* Call right after a print still in flight: the realistic scenario the pre-sleep hook exists
     * for on this project (flush COM1 before the CPU would idle - see os_cb.c). */
    printf("  [INFO] flushing this line before the (simulated) sleep point...\r\n");
    t0 = os_tick_get();
    os_tickless_pre_sleep_cb();
    t1 = os_tick_get();
    AHURA_TEST_CHECK((t1 - t0) <= 20U, "os_tickless_pre_sleep_cb() returns promptly (%lu ticks)",
                      (unsigned long)(t1 - t0));

    t0 = os_tick_get();
    os_tickless_post_sleep_cb();
    t1 = os_tick_get();
    AHURA_TEST_CHECK((t1 - t0) <= 20U, "os_tickless_post_sleep_cb() returns promptly (%lu ticks)",
                      (unsigned long)(t1 - t0));

    AHURA_TEST_CHECK(os_kernel_is_running(), "kernel state is intact after calling both hooks directly");

    /* Paired back-to-back, the same way os_tickless_idle_process() calls them. */
    os_tickless_pre_sleep_cb();
    os_tickless_post_sleep_cb();
    AHURA_TEST_CHECK(os_kernel_is_running(), "kernel state is intact after a paired pre/post call");
}

/*
 * ***********************************************************************************************************
 * Intrusive list (always compiled in - the scheduler runs on it)
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void test_list(void)
{
    os_list_t      list;
    os_list_node_t a;
    os_list_node_t b;
    os_list_node_t c;

    test_print_section("Intrusive List");

    os_list_init(&list);
    AHURA_TEST_CHECK(os_list_is_empty(&list), "a freshly initialized list is empty");

    os_list_push_back(&list, &a);
    os_list_push_back(&list, &b);
    os_list_push_back(&list, &c);
    AHURA_TEST_CHECK(!os_list_is_empty(&list), "list is non-empty after push_back");
    AHURA_TEST_CHECK(os_list_pop_front(&list) == &a, "pop_front returns nodes in FIFO order (1st = a)");

    os_list_remove(&list, &c);
    AHURA_TEST_CHECK(os_list_pop_front(&list) == &b, "removing a non-head node leaves the rest intact (2nd = b)");
    AHURA_TEST_CHECK(os_list_is_empty(&list), "list is empty after removing/popping everything pushed");

    os_list_push_back(&list, &a);
    os_list_insert_before(&list, &a, &b);
    AHURA_TEST_CHECK(os_list_pop_front(&list) == &b, "insert_before(head) places the new node ahead of it");
    AHURA_TEST_CHECK(os_list_pop_front(&list) == &a, "the original head follows");

    os_list_push_back(&list, &a);
    os_list_insert_before(&list, NULL, &b);
    AHURA_TEST_CHECK(os_list_pop_front(&list) == &a, "insert_before(NULL) appends at the tail (a stays head)");
    AHURA_TEST_CHECK(os_list_pop_front(&list) == &b, "the appended node comes out last");

    os_list_remove(&list, &c); /* c is not in any list: must be a safe no-op */
    AHURA_TEST_CHECK(os_list_is_empty(&list), "removing a node that is not in the list is a safe no-op");
}

/*
 * ***********************************************************************************************************
 * Config-gated features (multi-core / TrustZone / tickless)
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void test_unsupported_features(void)
{
    test_print_section("Multi-core / TrustZone / Tickless (config-gated, informational)");

#if (OS_CONFIG_CORE_COUNT > 1U)
    printf("  [INFO] multi-core APIs compiled in (OS_CONFIG_CORE_COUNT=%u) - not exercised by this suite\r\n",
           (unsigned)OS_CONFIG_CORE_COUNT);
#else
    printf("  [SKIP] multi-core APIs compiled out (OS_CONFIG_CORE_COUNT=1: this build is single-core)\r\n");
#endif

#if (OS_CONFIG_TRUSTZONE != OS_CONFIG_TRUSTZONE_DISABLED)
    printf("  [INFO] TrustZone callbacks compiled in - not exercised by this suite\r\n");
#else
    printf("  [SKIP] TrustZone disabled (OS_CONFIG_TRUSTZONE_DISABLED)\r\n");
#endif

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)
    printf("  [INFO] tickless idle enabled - not functionally wired in yet (see kernel README)\r\n");
#else
    printf("  [SKIP] tickless idle disabled (OS_CONFIG_TICKLESS_ENABLE=0)\r\n");
#endif
}

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Kernel self-test suite entry point: strong override of the weak os_test() declared
 *        in ahura.h. os_kernel.c creates a task that calls this automatically when
 *        OS_CONFIG_TEST_ENABLE is 1 - nothing else to call.
 */
void os_test(void)
{
    printf("\r\n========================================\r\n");
    printf(" Ahura RTOS self-test suite starting...\r\n");
    printf("========================================\r\n");

    test_kernel_core();
    test_delay();
    test_critical_section();
    test_task_lifecycle();
    test_priority_preemption();

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
    test_mutex();
#endif
#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    test_semaphore();
#endif
#if (OS_CONFIG_QUEUE_ENABLE == 1U)
    test_queue();
#endif
#if (OS_CONFIG_EVENT_ENABLE == 1U)
    test_event_group();
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    test_timer();
#endif
#if (OS_CONFIG_WORK_ENABLE == 1U)
    test_work();
#endif
#if (OS_CONFIG_ALLOC_ENABLE == 1U)
    test_alloc();
#endif
#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
    test_stack_watermark();
#endif
#if (OS_CONFIG_CPU_USAGE_ENABLE == 1U)
    test_cpu_usage();
#endif

#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_MUTEX_ENABLE == 1U)
    test_pipeline();
#endif
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
    test_mutex_priority_ordering();
#endif
#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_EVENT_ENABLE == 1U)
    test_event_queue_fanin();
#endif
#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEMAPHORE_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && \
    (OS_CONFIG_EVENT_ENABLE == 1U) && (OS_CONFIG_ALLOC_ENABLE == 1U)
    test_stress_soak();
#endif
    test_stress_task_churn();
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    test_stress_timer_churn();
#endif

    test_task_footprint();
    test_context_switch_timing();
    test_tickless_hooks();
    test_list();
    test_unsupported_features();

    printf("\r\n========================================\r\n");
    printf(" RESULT: %lu passed, %lu failed (of %lu checks)\r\n", (unsigned long)g_pass_count,
           (unsigned long)g_fail_count, (unsigned long)(g_pass_count + g_fail_count));
    printf("%s\r\n", (g_fail_count == 0U) ? " ALL RTOS FEATURES VERIFIED OK" : " SOME CHECKS FAILED - see log above");
    printf("========================================\r\n\r\n");
}
