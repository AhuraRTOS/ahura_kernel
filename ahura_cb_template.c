/**
 * @file ahura_cb_template.c
 * @brief Template for the application-side kernel callbacks (_cb functions).
 *
 * NOT part of the kernel build (and it must never be added to it): copy this
 * file into the application source tree as ahura_cb.c, add that copy to the
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

/******************************************************************************************************/
/**
 * @brief Return the CPU clock in Hz (0 = unknown; tick setup and busy-waits then refuse to run).
 *
 * This body matches the kernel default for CMSIS platforms. Adapt for
 * anything else: return a constant, query the clock driver, or report the
 * current frequency under dynamic frequency scaling.
 */
uint32_t os_clock_hz_get_cb(void)
{
    extern uint32_t SystemCoreClock;    /* CMSIS platforms */

    return SystemCoreClock;
}

/*
 * ***********************************************************************************************************
 * Tickless idle hooks
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Called right before the idle sleep: select the sleep mode (e.g. SLEEPDEEP), gate clocks.
 */
void os_tickless_pre_sleep_cb(void)
{
}

/******************************************************************************************************/
/**
 * @brief Called right after wakeup: clear SLEEPDEEP, restore clocks.
 */
void os_tickless_post_sleep_cb(void)
{
}

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
