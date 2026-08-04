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
OS_TASK_DEFINE(tsk_main, OS_CONFIG_MAIN_TASK_STACK_SIZE);
#endif

#if (OS_CONFIG_TEST_ENABLE == 1U)
OS_TASK_DEFINE(tsk_test, OS_CONFIG_TEST_STACK_SIZE);
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
    /* os_init() created it, and os_task_idle_create is idempotent - so a missing one here means
     * that creation failed and a retry would fail identically. Assert rather than re-create:
     * without an idle task the first switch restores through a NULL stack_ptr and hard faults. */
    OS_ASSERT(os_task_idle_is_created());

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

#endif /* OS_CONFIG_ASSERT_ENABLE */

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
        os_main_task_entry,
        NULL,
        OS_CONFIG_MAIN_TASK_PRIORITY,
        OS_TASK_CORE_ANY
    };

    status = os_task_create(&tsk_main, &config);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    return os_task_start(&tsk_main);
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
        os_test_task_entry,
        NULL,
        OS_CONFIG_TEST_PRIORITY,
        OS_TASK_CORE_ANY
    };

    status = os_task_create(&tsk_test, &config);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    return os_task_start(&tsk_test);
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
