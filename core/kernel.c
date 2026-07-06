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
 * Global variables
 * ***********************************************************************************************************
*/

static volatile bool os_kernel_running = false;

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
