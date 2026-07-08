/**
 * @file critical.c
 * @brief Critical section module implementation (PRIMASK based, nesting aware).
 *
 * On multi-core builds (OS_CONFIG_CORE_COUNT > 1) the outermost enter also
 * takes the global kernel spinlock, so a critical section excludes the other
 * cores as well as local interrupts. Nesting is tracked per core. On
 * single-core builds the spinlock and core-id lookups compile to nothing and
 * the behavior is the classic PRIMASK-only critical section.
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

static os_arch_spinlock_t critical_kernel_lock = OS_ARCH_SPINLOCK_INIT;
static __IO uint32_t      critical_nesting_count[OS_CONFIG_CORE_COUNT];
static uint32_t           critical_saved_primask[OS_CONFIG_CORE_COUNT];

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Enter critical section: mask interrupts, lock out other cores, count nesting.
 *
 * Safe from task and interrupt context. Keep critical sections short:
 * interrupts (including the kernel tick) are masked while inside, and on
 * multi-core builds the other cores spin at their own outermost enter.
 *
 * @return None.
 */
void os_critical_enter(void)
{
    uint32_t primask = os_arch_primask_get();
    uint32_t core;

    OS_ARCH_IRQ_DISABLE();

    core = os_arch_core_id_get();

    /* Remember the interrupt state of the outermost enter so exit can
     * restore it instead of blindly enabling interrupts. */
    if (critical_nesting_count[core] == 0U)
    {
        os_arch_spinlock_acquire(&critical_kernel_lock);
        critical_saved_primask[core] = primask;
    }

    critical_nesting_count[core]++;
}

/******************************************************************************************************/
/**
 * @brief Exit critical section: release the lock and unmask interrupts at the outermost level.
 *
 * @return None.
 */
void os_critical_exit(void)
{
    uint32_t core = os_arch_core_id_get();

    if (critical_nesting_count[core] == 0U)
    {
        return;
    }

    critical_nesting_count[core]--;

    if (critical_nesting_count[core] == 0U)
    {
        os_arch_spinlock_release(&critical_kernel_lock);

        if (critical_saved_primask[core] == 0U)
        {
            OS_ARCH_IRQ_ENABLE();
        }
    }
}
