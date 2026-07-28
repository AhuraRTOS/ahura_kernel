/**
 * @file os_cb_template.c
 * @brief Template for the application-side kernel callbacks (_cb functions).
 *
 * NOT part of the kernel build (and it must never be added to it): copy this
 * file into the application source tree as os_cb.c, add that copy to the
 * application build and adapt the bodies to the platform. Every function
 * here overrides a weak kernel default, so both the file and every single
 * function in it are optional — delete what you do not need.
 *
 * Functions guarded by configuration (#if blocks) only exist when the
 * matching OS_CONFIG_ option is enabled, so the file compiles cleanly under
 * any configuration.
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

#include "ahura.h"

/*
 * ***********************************************************************************************************
 * Platform clock
 * ***********************************************************************************************************
*/

/* Nothing to implement here. The kernel reads the CPU frequency straight from the CMSIS
 * SystemCoreClock variable, which the device's SystemInit() sets and SystemCoreClockUpdate()
 * refreshes after every clock-tree change, so a board that boots on an internal oscillator and
 * later switches to a PLL is handled with no kernel involvement.
 *
 * Only devices whose startup code does not provide that symbol need to act, and then only by
 * defining it (anywhere in the application):
 *
 *     uint32_t SystemCoreClock = 120000000U;
 *
 * Keep it updated if the clock tree changes at runtime; the kernel re-reads it on every use.
 */

/*
 * ***********************************************************************************************************
 * Debug hooks
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_ASSERT_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Called when an OS_ASSERT fails, just before the kernel halts.
 *
 * REQUIRED when OS_CONFIG_ASSERT_ENABLE is 1: the kernel ships no default, so leaving this out
 * is a link error. That is deliberate - a stub that did nothing would turn every assertion into
 * an unexplained halt with no clue where it came from.
 *
 * The kernel parks the core right after this returns, so there is no way to continue. Record
 * enough to find the cause: print it, store it in a retained/backup register or a noinit
 * section that survives reset, or just break into the debugger as below.
 *
 * Do not log from here through OS_LOG_*: the log task cannot run once the core is parked, so
 * the line would sit unsent in the buffer. Write directly to the transport instead.
 */
void os_assert_failed_cb(const char *file, uint32_t line)
{
    (void)file;
    (void)line;

    /* Example: __asm volatile("bkpt 0"); or a direct blocking UART write. */
}
#endif /* OS_CONFIG_ASSERT_ENABLE */

#if (OS_CONFIG_LOG_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Transmit finished log bytes.
 *
 * Called from the kernel log task, never from an ISR or a critical section, so it may block or
 * start a DMA transfer. The buffer is only valid for the duration of the call: copy it if the
 * transport completes asynchronously, or block here until it has been consumed.
 *
 * Keep this reasonably prompt. It runs at OS_CONFIG_LOG_TASK_PRIORITY, so a slow transport
 * delays only the log, but the ring keeps filling while it runs and lines are dropped once it
 * is full.
 */
void os_log_output_cb(const uint8_t *data, size_t length)
{
    (void)data;
    (void)length;

    /* Example: HAL_UART_Transmit(&huart, (uint8_t *)data, length, HAL_MAX_DELAY); */
}
#endif /* OS_CONFIG_LOG_ENABLE */

/*
 * ***********************************************************************************************************
 * Tickless idle hooks
 *
 * Both are MANDATORY when OS_CONFIG_TICKLESS_ENABLE is 1: the kernel declares them but defines
 * neither, so a missing one is a link error rather than a silently empty hook. Delete the pair
 * (and this block) when tickless idle is off.
 *
 * A common trap worth checking before leaving pre-sleep empty: if the vendor HAL drives its own
 * periodic tick from a separate timer, that interrupt wakes the WFI at its own period no matter
 * how long the kernel planned to sleep, so every suppressed sleep is cut short. Suspending it here
 * and resuming it in the post-sleep hook is what makes tickless idle actually save power.
 *
 * BOTH hooks run with the kernel's interrupts masked. That is what stops an ISR from moving a
 * deadline after the sleep length has been decided but before the WFI. The consequence for these
 * functions is that polling a hardware flag is fine, but waiting on anything an interrupt has to
 * deliver - a DMA completion callback, an RTOS object, a HAL call that spins on HAL_GetTick() -
 * will hang, because the interrupt that would end the wait cannot run. Keep both short: their
 * duration is added directly to interrupt latency.
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Called right before the idle sleep: select the sleep mode (e.g. SLEEPDEEP), gate clocks.
 *
 * Empty body = plain SLEEP (SLEEPDEEP left clear): the CPU clock stops but every peripheral clock -
 * UARTs, timers, DMA - keeps running, so nothing needs saving here and os_tickless_post_sleep_cb()
 * has nothing to restore. If a peripheral's completion must be guaranteed before the CPU naps (e.g.
 * flush a debug UART so the last line is fully transmitted), block on its busy/TX-complete flag
 * here. Selecting a deeper mode (STOP/SLEEPDEEP) instead means gated peripheral and system clocks -
 * restore them (and re-run the clock configuration if PLL/HSE were affected) in
 * os_tickless_post_sleep_cb() before anything relies on them again.
 */
void os_tickless_pre_sleep_cb(void)
{
}

/******************************************************************************************************/
/**
 * @brief Called right after wakeup: clear SLEEPDEEP, restore clocks.
 *
 * Runs with the kernel's interrupts still masked and before the sleep has been announced, so the
 * kernel clock is still short by the whole sleep duration while this executes. Restore hardware
 * here; do not call kernel APIs that block, delay, or read the tick expecting it to be current.
 * Keep it short for the same reason: everything in here is added to interrupt latency.
 */
void os_tickless_post_sleep_cb(void)
{
}
#endif /* OS_CONFIG_TICKLESS_ENABLE */

/*
 * ***********************************************************************************************************
 * TrustZone secure-context management (OS_CONFIG_TRUSTZONE_NON_SECURE only)
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
/******************************************************************************************************/
/**
 * @brief Bank the secure-side context (secure stack / PSP_S) of the task being switched out.
 *        task_id 0 is the idle task (never owns a secure context). Typically calls a secure
 *        gateway (cmse_nonsecure_entry) provided by the secure firmware.
 */
void os_arch_tz_context_save_cb(uint32_t task_id)
{
    (void)task_id;
}

/******************************************************************************************************/
/**
 * @brief Restore the secure-side context of the task being switched in.
 */
void os_arch_tz_context_restore_cb(uint32_t task_id)
{
    (void)task_id;
}
#endif /* OS_CONFIG_TRUSTZONE_NON_SECURE */

/*
 * ***********************************************************************************************************
 * Multi-core SoC glue (OS_CONFIG_CORE_COUNT > 1 only)
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief Return the index of the calling core (0-based). SoC-specific: e.g. SIO CPUID on the RP2040.
 */
uint32_t os_arch_core_id_get_cb(void)
{
    return 0U;
}

/******************************************************************************************************/
/**
 * @brief Interrupt another core so it re-evaluates scheduling. SoC-specific: e.g. the RP2040
 *        inter-core FIFO/doorbell. Without an implementation the target core reacts at its
 *        next tick instead.
 */
void os_arch_core_ipi_request_cb(uint32_t core_id)
{
    (void)core_id;
}

#if (OS_ARCH_HAS_EXCLUSIVES == 0)
/******************************************************************************************************/
/**
 * @brief Kernel spinlock backing on cores without LDREX/STREX (ARMv6-M multi-core SoCs).
 *        MANDATORY there — route to the SoC's hardware spinlocks (e.g. RP2040 SIO); the
 *        kernel deliberately ships no default, so leaving these out fails at link time.
 */
void os_arch_spinlock_acquire_cb(os_arch_spinlock_t *lock)
{
    (void)lock;
}

void os_arch_spinlock_release_cb(os_arch_spinlock_t *lock)
{
    (void)lock;
}
#endif /* OS_ARCH_HAS_EXCLUSIVES == 0 */
#endif /* OS_CONFIG_CORE_COUNT > 1U */
