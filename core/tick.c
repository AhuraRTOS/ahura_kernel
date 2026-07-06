/**
 * @file tick.c
 * @brief Kernel tick management implementation.
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

#if defined(__GNUC__)
#define OS_WEAK __attribute__((weak))
#else
#define OS_WEAK
#endif

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static volatile uint32_t os_tick_count = 0U;

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

void os_tickless_pre_sleep(uint32_t planned_idle_ticks);
void os_tickless_post_sleep(uint32_t elapsed_ticks);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize system tick source and bookkeeping.
 *
 * @return None.
 */
void os_tick_init(void)
{
    os_tick_count = 0U;
    os_arch_tick_init();
}

/******************************************************************************************************/
/**
 * @brief Get the kernel tick counter (wraps at 32 bits).
 *
 * @return uint32_t  Current tick count.
 */
uint32_t os_tick_get(void)
{
    return os_tick_count;
}

/******************************************************************************************************/
/**
 * @brief Handle periodic tick events. Call from the tick interrupt.
 *
 * @return None.
 */
void os_tick_handler(void)
{
    os_tick_count++;
#if (OS_CONFIG_WORK_ENABLE == 1U)
    os_work_tick_process(1U);
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    os_timer_tick_process(1U);
#endif
    os_task_tick_update(1U);

    /* Preempt every tick: wakes expired delays and round-robins tasks of
     * equal priority. PendSV is the lowest priority, so it runs after all
     * pending interrupts complete. */
    if (os_kernel_is_running())
    {
        OS_ARCH_CONTEXT_SWITCH_REQUEST();
    }
}

/******************************************************************************************************/
/**
 * @brief Announce elapsed ticks to kernel time base (tickless wakeup path).
 *
 * @param[in] elapsed_ticks  Number of elapsed ticks since previous update.
 * @return None.
 */
void os_tick_announce(uint32_t elapsed_ticks)
{
    os_tick_count += elapsed_ticks;
#if (OS_CONFIG_WORK_ENABLE == 1U)
    os_work_tick_process(elapsed_ticks);
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    os_timer_tick_process(elapsed_ticks);
#endif
    os_task_tick_update(elapsed_ticks);

    if (os_kernel_is_running())
    {
        OS_ARCH_CONTEXT_SWITCH_REQUEST();
    }
}

/******************************************************************************************************/
/**
 * @brief Get expected idle ticks for tickless decision.
 *
 * @return uint32_t  Expected idle duration in ticks.
 */
uint32_t os_tickless_expected_idle_ticks_get(void)
{
#if (OS_CONFIG_TICKLESS_ENABLE == 1U)
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    uint32_t idle_ticks = os_timer_next_expiry_ticks_get();
#else
    uint32_t idle_ticks = OS_CONFIG_MAX_SUPPRESSED_TICKS;
#endif

    if (idle_ticks > OS_CONFIG_MAX_SUPPRESSED_TICKS)
    {
        idle_ticks = OS_CONFIG_MAX_SUPPRESSED_TICKS;
    }

    return idle_ticks;
#else
    return 0U;
#endif
}

/******************************************************************************************************/
/**
 * @brief Execute tickless idle flow.
 *
 * @return None.
 */
void os_tickless_idle_process(void)
{
#if (OS_CONFIG_TICKLESS_ENABLE == 1U)
    uint32_t planned_idle_ticks = os_tickless_expected_idle_ticks_get();
    uint32_t elapsed_ticks      = 0U;

    if (planned_idle_ticks < OS_CONFIG_TICKLESS_MIN_IDLE)
    {
        return;
    }

    os_tickless_pre_sleep(planned_idle_ticks);

    OS_ARCH_SLEEP(planned_idle_ticks);
    elapsed_ticks = os_arch_elapsed_ticks_get();

    if (elapsed_ticks == 0U)
    {
        elapsed_ticks = planned_idle_ticks;
    }

    os_tickless_post_sleep(elapsed_ticks);
    os_tick_announce(elapsed_ticks);
#endif
}

/******************************************************************************************************/
/**
 * @brief Pre-sleep hook called before entering low-power mode.
 *
 * @param[in] planned_idle_ticks  Planned sleep duration in ticks.
 * @return None.
 */
OS_WEAK void os_tickless_pre_sleep(uint32_t planned_idle_ticks)
{
    (void)planned_idle_ticks;
}

/******************************************************************************************************/
/**
 * @brief Post-sleep hook called after leaving low-power mode.
 *
 * @param[in] elapsed_ticks  Actual elapsed ticks while sleeping.
 * @return None.
 */
OS_WEAK void os_tickless_post_sleep(uint32_t elapsed_ticks)
{
    (void)elapsed_ticks;
}
