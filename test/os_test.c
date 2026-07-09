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

#if (OS_CONFIG_MEMORY_POOL_ENABLE == 1U)
#define POOL_BLOCK_SIZE  16U
#define POOL_BLOCK_COUNT 4U
static uint8_t           g_pool_buffer[POOL_BLOCK_SIZE * POOL_BLOCK_COUNT];
static uint8_t           g_pool_usage[POOL_BLOCK_COUNT];
static os_memory_pool_t  g_pool;
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
#if (OS_CONFIG_MEMORY_POOL_ENABLE == 1U)
static void test_memory_pool(void);
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
    (void)os_task_pause((os_task_t *)0);
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

    status = os_task_create(&helper_TASK, OS_TASK_CONFIG(helper, test_helper_entry, (void *)0, 3U));
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    return os_task_start(&helper_TASK);
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

    AHURA_TEST_CHECK(os_arch_primask_get() == 0U, "interrupts are unmasked before entering a critical section");

    os_critical_enter();
    AHURA_TEST_CHECK(os_arch_primask_get() != 0U, "os_critical_enter() masks interrupts");

    os_critical_enter(); /* nested */
    AHURA_TEST_CHECK(os_arch_primask_get() != 0U, "a nested os_critical_enter() keeps interrupts masked");

    os_critical_exit(); /* inner exit: outer level still held */
    AHURA_TEST_CHECK(os_arch_primask_get() != 0U, "exiting the inner level keeps interrupts masked (nesting works)");

    os_critical_exit(); /* outer exit */
    AHURA_TEST_CHECK(os_arch_primask_get() == 0U, "the matching outer os_critical_exit() unmasks interrupts");
}

/******************************************************************************************************/
static void test_task_lifecycle(void)
{
    os_task_config_t cfg;
    os_status        status;
    uint32_t         snapshot;

    test_print_section("Task Lifecycle");

    /* --- Reject invalid creation parameters (should not touch any handle). --- */
    cfg = *OS_TASK_CONFIG(helper, test_worker_entry, (void *)0, 1U);

    cfg.priority = 0U;
    AHURA_TEST_CHECK(os_task_create(&helper_TASK, &cfg) == OS_STATUS_INVALID_ARG,
                      "os_task_create() rejects priority 0 (idle-reserved)");

    cfg.priority = OS_CONFIG_MAX_PRIORITY;
    AHURA_TEST_CHECK(os_task_create(&helper_TASK, &cfg) == OS_STATUS_INVALID_ARG,
                      "os_task_create() rejects priority %u (kernel-reserved)", (unsigned)OS_CONFIG_MAX_PRIORITY);

    cfg.priority    = OS_TASK_PRIORITY_USER_MIN;
    cfg.stack_bytes = OS_CONFIG_MIN_STACK_SIZE - 8U;
    AHURA_TEST_CHECK(os_task_create(&helper_TASK, &cfg) == OS_STATUS_INVALID_ARG,
                      "os_task_create() rejects a stack smaller than OS_CONFIG_MIN_STACK_SIZE");

    cfg.stack_bytes  = sizeof(helper_STACK) - 8U;
    cfg.stack_memory = &helper_STACK[1];
    AHURA_TEST_CHECK(os_task_create(&helper_TASK, &cfg) == OS_STATUS_INVALID_ARG,
                      "os_task_create() rejects a misaligned stack pointer");

    /* --- Real worker: create / start / observe / pause / resume / delete. --- */
    g_worker_counter    = 0U;
    g_worker_should_run = true;

    status = os_task_create(&worker_TASK, OS_TASK_CONFIG(worker, test_worker_entry, (void *)0, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "os_task_create() creates the worker task");
    AHURA_TEST_CHECK(os_task_state_get(&worker_TASK) == OS_TASK_STATE_SUSPENDED,
                      "a created-but-not-started task reports SUSPENDED");

    AHURA_TEST_CHECK(os_task_start(&worker_TASK) == OS_STATUS_OK, "os_task_start() starts the worker task");
    (void)os_delay_ms(20U);
    AHURA_TEST_CHECK(g_worker_counter > 0U, "worker task actually executed (counter=%lu)",
                      (unsigned long)g_worker_counter);
    AHURA_TEST_CHECK(os_task_state_get(&worker_TASK) == OS_TASK_STATE_READY,
                      "a lower-priority runnable task reports READY while this task runs");

    AHURA_TEST_CHECK(os_task_pause(&worker_TASK) == OS_STATUS_OK, "os_task_pause() suspends the worker task");
    AHURA_TEST_CHECK(os_task_state_get(&worker_TASK) == OS_TASK_STATE_SUSPENDED, "paused task reports SUSPENDED");
    snapshot = g_worker_counter;
    (void)os_delay_ms(20U);
    AHURA_TEST_CHECK(g_worker_counter == snapshot, "counter is frozen while the worker is paused");

    AHURA_TEST_CHECK(os_task_start(&worker_TASK) == OS_STATUS_OK, "os_task_start() resumes a paused task");
    (void)os_delay_ms(20U);
    AHURA_TEST_CHECK(g_worker_counter > snapshot, "counter resumes advancing after os_task_start()");

    AHURA_TEST_CHECK(os_task_delete(&worker_TASK) == OS_STATUS_OK, "os_task_delete() deletes the live worker task");
    AHURA_TEST_CHECK(os_task_state_get(&worker_TASK) == OS_TASK_STATE_INACTIVE,
                      "a deleted task's handle reports INACTIVE");
    snapshot = g_worker_counter;
    (void)os_delay_ms(20U);
    AHURA_TEST_CHECK(g_worker_counter == snapshot, "counter is frozen after deletion (worker truly stopped)");

    /* --- NULL means "current task": the worker pauses itself; we resume it. --- */
    g_worker_counter = 0U;
    status = os_task_create(&worker_TASK, OS_TASK_CONFIG(worker, test_self_pause_worker_entry, (void *)0, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "worker task re-created for the self-pause test");
    AHURA_TEST_CHECK(os_task_start(&worker_TASK) == OS_STATUS_OK, "os_task_start() starts it");

    (void)os_delay_ms(40U); /* let it reach os_task_pause(NULL) */
    AHURA_TEST_CHECK(os_task_state_get(&worker_TASK) == OS_TASK_STATE_SUSPENDED,
                      "os_task_pause(NULL) suspends the calling task itself");

    AHURA_TEST_CHECK(os_task_start(&worker_TASK) == OS_STATUS_OK,
                      "os_task_start() resumes a task that paused itself");
    (void)os_delay_ms(20U);
    AHURA_TEST_CHECK(g_worker_counter == 42U, "the resumed task continued executing past its self-pause point");

    /* test_self_pause_worker_entry() already returned above (auto-exiting via the arch port's
     * os_task_exit() trampoline) - no explicit os_task_delete() here, that would fail with
     * INVALID_ARG since the slot is already freed. Just confirm the self-exit completed. */
    AHURA_TEST_CHECK(test_wait_inactive(&worker_TASK, 200U), "the resumed worker terminates cleanly on its own");
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
    status = os_task_create(&worker_TASK, OS_TASK_CONFIG(worker, test_busy_spin_entry, (void *)0, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "low-priority spinner task created (priority 1)");
    AHURA_TEST_CHECK(os_task_start(&worker_TASK) == OS_STATUS_OK, "low-priority spinner started");

    (void)os_delay_ms(20U);
    snapshot_before = g_busy_counter;
    AHURA_TEST_CHECK(snapshot_before > 0U,
                      "the low-priority spinner gets CPU time when nothing outranks it (count=%lu)",
                      (unsigned long)snapshot_before);

    /* A task at a strictly higher priority than both the spinner and this test task never
     * yields/delays for its whole burst - so the spinner cannot possibly run until it is gone. */
    status = os_task_create(&helper_TASK, OS_TASK_CONFIG(helper, test_burst_spin_entry, (void *)0,
                                                            OS_CONFIG_TEST_PRIORITY + 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "higher-priority burst task created (priority %u)",
                      (unsigned)(OS_CONFIG_TEST_PRIORITY + 1U));

    AHURA_TEST_CHECK(os_task_start(&helper_TASK) == OS_STATUS_OK, "higher-priority burst task started");
    snapshot_immediate = g_busy_counter;
    AHURA_TEST_CHECK(snapshot_immediate == snapshot_before,
                      "the spinner has not advanced right after the higher-priority task starts (count=%lu)",
                      (unsigned long)snapshot_immediate);

    AHURA_TEST_CHECK(test_wait_inactive(&helper_TASK, 200U),
                      "the higher-priority burst task ran to completion and self-terminated");

    (void)os_delay_ms(10U);
    snapshot_after = g_busy_counter;
    AHURA_TEST_CHECK(snapshot_after > snapshot_before,
                      "the spinner resumes running once the higher-priority task is gone (count=%lu)",
                      (unsigned long)snapshot_after);

    g_busy_should_run = false;
    AHURA_TEST_CHECK(test_wait_inactive(&worker_TASK, 200U), "low-priority spinner stops cleanly");
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
    AHURA_TEST_CHECK(test_wait_inactive(&helper_TASK, 200U), "mutex-holder helper task terminated cleanly");
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
    AHURA_TEST_CHECK(test_wait_inactive(&helper_TASK, 200U), "semaphore-giver helper task terminated cleanly");
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
    AHURA_TEST_CHECK(test_wait_inactive(&helper_TASK, 200U), "queue-sender helper task terminated cleanly");
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
    AHURA_TEST_CHECK(os_event_group_wait_bits(&g_event, 0x03U, false, &matched, OS_WAIT_NOTHING) == OS_STATUS_BUSY,
                      "wait-any on unset bits with OS_WAIT_NOTHING returns BUSY");
    AHURA_TEST_CHECK(matched == 0U, "matched_bits reports 0 when nothing matched");

    AHURA_TEST_CHECK(os_event_group_set_bits(&g_event, 0x01U) == OS_STATUS_OK,
                      "os_event_group_set_bits(0x01) succeeds");
    AHURA_TEST_CHECK(os_event_group_wait_bits(&g_event, 0x03U, false, &matched, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "wait-any matches once one of the requested bits is set");
    AHURA_TEST_CHECK(matched == 0x01U, "matched_bits reports the intersecting bits (0x%02lx)",
                      (unsigned long)matched);

    AHURA_TEST_CHECK(os_event_group_wait_bits(&g_event, 0x03U, true, &matched, OS_WAIT_NOTHING) == OS_STATUS_BUSY,
                      "wait-all is still BUSY while only some requested bits are set");

    AHURA_TEST_CHECK(os_event_group_clear_bits(&g_event, 0x01U) == OS_STATUS_OK,
                      "os_event_group_clear_bits(0x01) succeeds");
    AHURA_TEST_CHECK(os_event_group_wait_bits(&g_event, 0x01U, false, &matched, OS_WAIT_NOTHING) == OS_STATUS_BUSY,
                      "a cleared bit no longer matches");

    AHURA_TEST_CHECK(test_spawn_helper(HELPER_EVENT_SET_AFTER, 80U, 0x06U, 0U) == OS_STATUS_OK,
                      "helper spawned to set bits 0x06 after 80 ms");
    t0     = os_tick_get();
    status = os_event_group_wait_bits(&g_event, 0x06U, true, &matched, 500U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK((status == OS_STATUS_OK) && (matched == 0x06U),
                      "wait-all matches once the helper sets both bits (matched=0x%02lx)", (unsigned long)matched);
    AHURA_TEST_CHECK((delta >= 70U) && (delta <= 200U), "wait woke ~when the helper set the bits (%lu ticks)",
                      (unsigned long)delta);
    AHURA_TEST_CHECK(test_wait_inactive(&helper_TASK, 200U), "event-setter helper task terminated cleanly");
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
                                    (void *)0) == OS_STATUS_OK,
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
                                    timer_periodic_cb, (void *)0) == OS_STATUS_OK,
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

    AHURA_TEST_CHECK(os_work_init(&g_work, work_handler, (void *)0) == OS_STATUS_OK, "os_work_init() succeeds");
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
 * Memory pool
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_MEMORY_POOL_ENABLE == 1U)
/******************************************************************************************************/
static void test_memory_pool(void)
{
    void     *blocks[POOL_BLOCK_COUNT] = { 0 };
    void     *reused;
    uint32_t i;
    bool     all_unique = true;

    test_print_section("Memory Pool");

    AHURA_TEST_CHECK(os_memory_pool_init(&g_pool, g_pool_buffer, g_pool_usage, POOL_BLOCK_SIZE, POOL_BLOCK_COUNT) ==
                          OS_STATUS_OK,
                      "os_memory_pool_init() creates a %u x %u-byte pool", (unsigned)POOL_BLOCK_COUNT,
                      (unsigned)POOL_BLOCK_SIZE);

    for (i = 0U; i < POOL_BLOCK_COUNT; i++)
    {
        blocks[i] = os_memory_pool_alloc(&g_pool);
        if (blocks[i] == (void *)0)
        {
            all_unique = false;
        }
    }
    AHURA_TEST_CHECK(all_unique, "pool yields %u distinct blocks", (unsigned)POOL_BLOCK_COUNT);
    AHURA_TEST_CHECK(os_memory_pool_alloc(&g_pool) == (void *)0, "pool returns NULL once exhausted");

    AHURA_TEST_CHECK(os_memory_pool_free(&g_pool, blocks[1]) == OS_STATUS_OK,
                      "os_memory_pool_free() releases a block");
    AHURA_TEST_CHECK(os_memory_pool_free(&g_pool, blocks[1]) == OS_STATUS_ERROR,
                      "double-freeing the same block returns ERROR");

    reused = os_memory_pool_alloc(&g_pool);
    AHURA_TEST_CHECK(reused == blocks[1], "the freed block is reused by the next alloc");

    AHURA_TEST_CHECK(os_memory_pool_free(&g_pool, g_pool_buffer - 1) == OS_STATUS_INVALID_ARG,
                      "freeing a pointer before the pool buffer is rejected");
    AHURA_TEST_CHECK(os_memory_pool_free(&g_pool, g_pool_buffer + 3) == OS_STATUS_INVALID_ARG,
                      "freeing a misaligned pointer is rejected");

    for (i = 0U; i < POOL_BLOCK_COUNT; i++)
    {
        (void)os_memory_pool_free(&g_pool, blocks[i]);
    }
}
#endif /* OS_CONFIG_MEMORY_POOL_ENABLE */

/*
 * ***********************************************************************************************************
 * Kernel heap (os_alloc / os_free)
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

    test_print_section("Kernel Heap (os_alloc)");

    free0 = os_alloc_free_bytes_get();
    AHURA_TEST_CHECK(free0 > 0U, "heap reports free bytes at start (%lu)", (unsigned long)free0);

    p1 = os_alloc(128U);
    AHURA_TEST_CHECK(p1 != (void *)0, "os_alloc(128) succeeds");
    free1 = os_alloc_free_bytes_get();
    AHURA_TEST_CHECK(free1 < free0, "free bytes decreased after alloc (%lu -> %lu)", (unsigned long)free0,
                      (unsigned long)free1);

    p2 = os_alloc(64U);
    AHURA_TEST_CHECK(p2 != (void *)0, "a second os_alloc(64) succeeds");
    AHURA_TEST_CHECK(p1 != p2, "two live allocations return distinct blocks");

    os_free(p1);
    os_free(p2);
    free2 = os_alloc_free_bytes_get();
    AHURA_TEST_CHECK(free2 == free0, "freeing both blocks restores the original free-byte count (coalescing works)");

    min_free = os_alloc_min_free_bytes_get();
    AHURA_TEST_CHECK(min_free <= free0, "watermark min-free (%lu) never exceeds the current free count (%lu)",
                      (unsigned long)min_free, (unsigned long)free0);

    AHURA_TEST_CHECK(os_alloc((size_t)OS_CONFIG_HEAP_SIZE * 2U) == (void *)0,
                      "an allocation larger than the whole heap fails cleanly");

    os_free((void *)0); /* must not crash */
    AHURA_TEST_CHECK(true, "os_free(NULL) is a safe no-op");
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

    AHURA_TEST_CHECK(os_task_stack_watermark_get((os_task_t *)0, &min_free) == OS_STATUS_OK,
                      "os_task_stack_watermark_get(NULL) reports the calling task");
    AHURA_TEST_CHECK(min_free < OS_CONFIG_TEST_STACK_SIZE,
                      "watermark (%lu) is less than the full stack (%lu bytes)",
                      (unsigned long)min_free, (unsigned long)OS_CONFIG_TEST_STACK_SIZE);

    AHURA_TEST_CHECK(os_task_stack_watermark_get((os_task_t *)0, (size_t *)0) == OS_STATUS_INVALID_ARG,
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
    status = os_task_create(&worker_TASK, OS_TASK_CONFIG(worker, test_busy_spin_entry, (void *)0, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "busy worker task created to load the CPU (priority 1)");
    AHURA_TEST_CHECK(os_task_start(&worker_TASK) == OS_STATUS_OK, "busy worker task started");

    (void)os_cpu_usage_get(); /* reset the sampling window right before the load starts */
    (void)os_delay_ms(300U);
    busy_usage = os_cpu_usage_get();
    AHURA_TEST_CHECK(busy_usage >= 90U, "usage rises sharply under a busy lower-priority task (%lu%%)",
                      (unsigned long)busy_usage);
    AHURA_TEST_CHECK(g_busy_counter > 0U, "the busy worker actually made progress (count=%lu)",
                      (unsigned long)g_busy_counter);

    g_busy_should_run = false;
    AHURA_TEST_CHECK(test_wait_inactive(&worker_TASK, 200U), "busy worker task stops cleanly");
}
#endif /* OS_CONFIG_CPU_USAGE_ENABLE */

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
#if (OS_CONFIG_MAIN_TASK_ENABLE == 1U)
    printf("  [INFO] OS_CONFIG_MAIN_TASK_STACK_SIZE = %lu bytes (tsk_main)\r\n",
           (unsigned long)OS_CONFIG_MAIN_TASK_STACK_SIZE);
#endif
    printf("  [INFO] OS_CONFIG_TEST_STACK_SIZE      = %lu bytes (tsk_test, this task)\r\n",
           (unsigned long)OS_CONFIG_TEST_STACK_SIZE);

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
    {
        size_t    self_min_free;
        size_t    worker_min_free;
        os_status status;

        if (os_task_stack_watermark_get((os_task_t *)0, &self_min_free) == OS_STATUS_OK)
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
        status = os_task_create(&worker_TASK, OS_TASK_CONFIG(worker, test_busy_spin_entry, (void *)0, 1U));
        if (status == OS_STATUS_OK)
        {
            (void)os_task_start(&worker_TASK);
            (void)os_delay_ms(20U);
            g_busy_should_run = false;

            if (os_task_stack_watermark_get(&worker_TASK, &worker_min_free) == OS_STATUS_OK)
            {
                printf("  [INFO] worker task peak stack usage: %lu / %lu bytes (%lu%% headroom left)\r\n",
                       (unsigned long)(sizeof(worker_STACK) - worker_min_free),
                       (unsigned long)sizeof(worker_STACK),
                       (unsigned long)((worker_min_free * 100U) / sizeof(worker_STACK)));
            }

            (void)test_wait_inactive(&worker_TASK, 200U);
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

    status = os_task_create(&worker_TASK, OS_TASK_CONFIG(worker, test_switch_ping_entry, (void *)0, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "ping task created for the switch benchmark (priority 1)");
    status = os_task_create(&helper_TASK, OS_TASK_CONFIG(helper, test_switch_ping_entry, (void *)0, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "pong task created for the switch benchmark (priority 1)");

    t0 = os_tick_get();
    (void)os_task_start(&worker_TASK);
    (void)os_task_start(&helper_TASK);
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

    AHURA_TEST_CHECK(test_wait_inactive(&worker_TASK, 200U), "ping task stops cleanly");
    AHURA_TEST_CHECK(test_wait_inactive(&helper_TASK, 200U), "pong task stops cleanly");
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
    os_list_insert_before(&list, (os_list_node_t *)0, &b);
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
#if (OS_CONFIG_MEMORY_POOL_ENABLE == 1U)
    test_memory_pool();
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
