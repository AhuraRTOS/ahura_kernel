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

#if (OS_CONFIG_CPU_CLOCK_HZ == 0U)
/* CMSIS platforms provide the SystemCoreClock global; the weak reference
 * resolves to address 0 on platforms without it, so linking never fails. */
extern uint32_t SystemCoreClock OS_WEAK;
#endif

#if (OS_CONFIG_MAIN_TASK_ENABLE == 1U)
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

#if (OS_CONFIG_MAIN_TASK_ENABLE == 1U)
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
#if (OS_CONFIG_MAIN_TASK_ENABLE == 1U)
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

/******************************************************************************************************/
/**
 * @brief Platform callback: return the CPU clock in Hz (0 = unknown).
 *
 * Weak default: the fixed OS_CONFIG_CPU_CLOCK_HZ when configured, else the
 * CMSIS SystemCoreClock global when the platform provides one, else 0 (tick
 * setup and busy-wait delays then refuse to run rather than misbehave).
 * Platforms with another clock convention override this function.
 *
 * @return uint32_t  CPU clock frequency in Hz.
 */
OS_WEAK uint32_t os_clock_hz_get_cb(void)
{
#if (OS_CONFIG_CPU_CLOCK_HZ > 0U)
    return OS_CONFIG_CPU_CLOCK_HZ;
#else
    if (&SystemCoreClock != (uint32_t *)0)
    {
        return SystemCoreClock;
    }

    return 0U;
#endif
}

#if (OS_CONFIG_MAIN_TASK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Default application task body (see OS_CONFIG_MAIN_TASK_* in os_config.h).
 *
 * Weak default: idles forever. Override in the application (copy of os_main_template.c
 * as os_main.c) with real application code - a plain while(1) loop, or spawn further
 * tasks from here. Not a "_cb" hook: this is where the application's own code runs.
 *
 * @return None.
 */
OS_WEAK void os_main(void)
{
    while (1)
    {
        (void)os_delay_ms(1000U);
    }
}
#endif /* OS_CONFIG_MAIN_TASK_ENABLE */

#if (OS_CONFIG_TEST_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Kernel self-test suite entry point (see OS_CONFIG_TEST_* in os_config.h).
 *
 * Weak default: does nothing. Link the ahura_kernel/test library (CMake target "os_test")
 * to get the real suite's strong override instead - see README "Self-test suite". Not a
 * "_cb" hook, same reasoning as os_main().
 *
 * @return None.
 */
OS_WEAK void os_test(void)
{
}
#endif /* OS_CONFIG_TEST_ENABLE */

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_MAIN_TASK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Create and start the default application task. Called from os_init().
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
        (void *)0,
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
#endif /* OS_CONFIG_MAIN_TASK_ENABLE */

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
        (void *)0,
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
