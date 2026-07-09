/**
 * @file os_main_template.c
 * @brief Template for the application's default task body.
 *
 * NOT part of the kernel build (like os_cb_template.c): copy this file into
 * the application source tree as os_main.c, add it to the APPLICATION
 * build, and write the application's own code inside os_main(). Its
 * prototype already lives in ahura.h (guarded by OS_CONFIG_MAIN_TASK_ENABLE),
 * so no separate header is needed for this one.
 *
 * os_main() is deliberately not named with the "_cb" suffix used elsewhere
 * in this kernel: that suffix is reserved for callbacks the kernel queries
 * for platform behavior (os_clock_hz_get_cb, os_tickless_pre_sleep_cb, ...).
 * os_main() is different in kind - it is where the application's own code
 * runs, not a query the kernel makes about the platform - even though it is
 * wired up the same way (a weak default in os_kernel.c, overridden here).
 *
 * Task creation itself lives in the kernel (os_kernel.c's os_main_system_init(),
 * called from os_init()) and is unconditional whenever OS_CONFIG_MAIN_TASK_ENABLE
 * is 1 - nothing to call from main(). Sized by OS_CONFIG_MAIN_TASK_STACK_SIZE /
 * OS_CONFIG_MAIN_TASK_PRIORITY in os_config.h.
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
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body: runs once os_start() hands control to task context.
 *        Replace the body with the application's own code.
 *
 * @return None.
 */
void os_main(void)
{
    while (1)
    {
        /* TODO: replace with the application's own code. */
        (void)os_delay_ms(1000U);
    }
}
