/**
 * @file os_kernel.c
 * @brief Kernel lifecycle core implementation.
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
 * Private function prototypes
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TEST_ENABLE == 0U)
static os_status os_main_system_init(void);
static void      os_main_task_entry(void *context);
#endif

#if (OS_CONFIG_TEST_ENABLE == 1U)
static os_status os_test_system_init(void);
static void      os_test_task_entry(void *context);
#endif

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static __IO bool os_kernel_running = false;

#if (OS_CONFIG_TEST_ENABLE == 0U)
static uint8_t   os_main_task_stack[OS_CONFIG_MAIN_TASK_STACK_SIZE] OS_STACK_ALIGNED;
static os_task_t os_main_task_handle;
#endif

#if (OS_CONFIG_TEST_ENABLE == 1U)
static uint8_t   os_test_task_stack[OS_CONFIG_TEST_STACK_SIZE] OS_STACK_ALIGNED;
static os_task_t os_test_task_handle;
#endif

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize kernel subsystems. Call once before any other kernel API.
 *
 * @return None.
 */
void os_init(void)
{
    os_arch_init();
    os_task_system_init();
    (void)os_task_idle_create();

    /* Kernel service tasks at the reserved highest priority: the work queue
     * and the timer callback task. */
#if (OS_CONFIG_WORK_ENABLE == 1U)
    (void)os_work_system_init();
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    (void)os_timer_system_init();
#endif
    /* The log task sits at the opposite end from work/timer: lowest priority,
     * so draining the log never preempts application work. Created before the
     * main/test task so anything they log at startup already has a consumer. */
#if (OS_CONFIG_LOG_ENABLE == 1U)
    (void)os_log_system_init();
#endif
    /* The self-test suite takes priority over the default application task:
     * both otherwise run tsk_main-priority-range code from os_init(), and a
     * test build's job is to exercise the kernel in isolation, not race the
     * application's own task against it. Outside test builds, tsk_main is
     * created unconditionally. */
#if (OS_CONFIG_TEST_ENABLE == 0U)
    (void)os_main_system_init();
#endif
#if (OS_CONFIG_TEST_ENABLE == 1U)
    (void)os_test_system_init();
#endif

    os_tick_init();
}

/******************************************************************************************************/
/**
 * @brief Start the scheduler and switch to task context. Does not return.
 *
 * @return None.
 */
void os_start(void)
{
    if (!os_task_idle_is_created())
    {
        (void)os_task_idle_create();
    }

    os_kernel_running = true;
    os_arch_start_first_task();

    /* Never reached. */
    while (1)
    {
    }
}

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief Enter the scheduler on a secondary core. Does not return.
 *
 * Call from the secondary core after os_start() is running on core 0, once
 * the SoC layer has booted the core with a vector table routing SVC, PendSV
 * and SysTick to the kernel handlers. SHPR, SysTick, DWT and MSPLIM are all
 * banked per core, so the same architecture init runs here; the per-core
 * SysTick drives this core's preemption while core 0 owns the time base.
 *
 * @return None.
 */
void os_core_start(void)
{
    os_arch_init();
    os_arch_tick_init();
    os_arch_start_first_task();

    /* Never reached. */
    while (1)
    {
    }
}
#endif /* OS_CONFIG_CORE_COUNT > 1U */

/******************************************************************************************************/
/**
 * @brief Return true once the scheduler has been started.
 *
 * @return bool  True when the scheduler is running.
 */
bool os_kernel_is_running(void)
{
    return os_kernel_running;
}

#if (OS_CONFIG_ASSERT_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Report a failed OS_ASSERT and halt.
 *
 * The application hook runs first, while the failure's context is still intact, so it can print
 * or store the location. It then falls through to the same trap the boot-time configuration
 * checks use: interrupts masked, core parked, debugger stops here. There is deliberately no way
 * to continue - an assertion means an invariant the rest of the kernel relies on is already
 * broken, so running on would only corrupt more state before the eventual failure.
 *
 * @param[in] file  Source file of the failed check.
 * @param[in] line  Line number of the failed check.
 * @return None. Never returns.
 */
void os_assert_failed(const char *file, uint32_t line)
{
    os_assert_failed_cb(file, line);
    os_arch_config_fault_trap();

    /* os_arch_config_fault_trap never returns; the loop only convinces the
     * compiler of that when it is inlined as a plain call. */
    while (1)
    {
    }
}

/*
 * os_assert_failed_cb() is deliberately NOT defined here, not even as a weak stub. A stub that
 * did nothing would make the one thing an assertion exists to do - tell you where it fired -
 * silently unavailable, leaving only an unexplained halt. Enabling assertions therefore requires
 * the application to say what happens on failure (see os_cb_template.c), and forgetting to is a
 * link error rather than a debugging session spent wondering why the core stopped.
 */
#endif /* OS_CONFIG_ASSERT_ENABLE */

/*
 * os_main() and os_test() are deliberately NOT defined here, not even as weak stubs.
 *
 * os_main() is the application's own code, supplied by its os_main.c (copied from
 * os_main_template.c). A weak "idle forever" stub would let a project that simply forgot the
 * file link and boot into a task that does nothing, which is far harder to diagnose than the
 * undefined-reference error the linker gives instead. It is only referenced when
 * OS_CONFIG_TEST_ENABLE is 0, so a self-test build needs no os_main.c at all.
 *
 * os_test() comes from the ahura_kernel/test library. Leaving it undefined here is also what
 * makes plain static-library linking work: the reference below is unresolved, so the linker has
 * a reason to pull os_test.c.o out of libos_test.a. A weak stub would satisfy the reference
 * first and the archive would never be searched, silently dropping the entire suite - which is
 * why linking it used to require -Wl,--whole-archive.
 */

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TEST_ENABLE == 0U)
/******************************************************************************************************/
/**
 * @brief Create and start the default application task. Called from os_init().
 *
 * Not compiled in when OS_CONFIG_TEST_ENABLE is also 1: the self-test suite
 * runs instead of the application's own task in that build (see os_init()).
 *
 * @return os_status  Status code.
 */
static os_status os_main_system_init(void)
{
    os_status status;

    os_task_config_t config =
    {
        "tsk_main",
        os_main_task_entry,
        NULL,
        OS_CONFIG_MAIN_TASK_PRIORITY,
        (void *)os_main_task_stack,
        sizeof(os_main_task_stack),
        OS_TASK_CORE_ANY
    };

    status = os_task_create(&os_main_task_handle, &config);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    return os_task_start(&os_main_task_handle);
}

/******************************************************************************************************/
/**
 * @brief Default application task entry: wraps os_main() so a return from it cleanly
 *        exits the task instead of falling off the end of an entry function.
 *
 * @param[in] context  Unused.
 * @return None.
 */
static void os_main_task_entry(void *context)
{
    (void)context;
    os_main();
}
#endif /* OS_CONFIG_TEST_ENABLE == 0U */

#if (OS_CONFIG_TEST_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Create and start the self-test task. Called from os_init().
 *
 * @return os_status  Status code.
 */
static os_status os_test_system_init(void)
{
    os_status status;

    os_task_config_t config =
    {
        "tsk_test",
        os_test_task_entry,
        NULL,
        OS_CONFIG_TEST_PRIORITY,
        (void *)os_test_task_stack,
        sizeof(os_test_task_stack),
        OS_TASK_CORE_ANY
    };

    status = os_task_create(&os_test_task_handle, &config);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    return os_task_start(&os_test_task_handle);
}

/******************************************************************************************************/
/**
 * @brief Self-test task entry: wraps os_test() so a return from it cleanly exits the
 *        task instead of falling off the end of an entry function.
 *
 * @param[in] context  Unused.
 * @return None.
 */
static void os_test_task_entry(void *context)
{
    (void)context;
    os_test();
}
#endif /* OS_CONFIG_TEST_ENABLE */
