/**
 * @file critical.c
 * @brief Critical section module implementation (PRIMASK based, nesting aware).
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

static volatile uint32_t critical_nesting_count = 0U;
static uint32_t          critical_saved_primask = 0U;

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Enter critical section: mask interrupts and count nesting.
 *
 * Safe from task and interrupt context. Keep critical sections short:
 * interrupts (including the kernel tick) are masked while inside.
 *
 * @return None.
 */
void os_critical_enter(void)
{
    uint32_t primask = os_arch_primask_get();

    OS_ARCH_IRQ_DISABLE();

    /* Remember the interrupt state of the outermost enter so exit can
     * restore it instead of blindly enabling interrupts. */
    if (critical_nesting_count == 0U)
    {
        critical_saved_primask = primask;
    }

    critical_nesting_count++;
}

/******************************************************************************************************/
/**
 * @brief Exit critical section: unmask interrupts at the outermost level.
 *
 * @return None.
 */
void os_critical_exit(void)
{
    if (critical_nesting_count == 0U)
    {
        return;
    }

    critical_nesting_count--;

    if ((critical_nesting_count == 0U) && (critical_saved_primask == 0U))
    {
        OS_ARCH_IRQ_ENABLE();
    }
}
