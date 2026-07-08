/**
 * @file kernel.c
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

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static __IO bool os_kernel_running = false;

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
