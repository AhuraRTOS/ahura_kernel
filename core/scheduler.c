/**
 * @file scheduler.c
 * @brief Scheduler core implementation.
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

#include "../ahura.h"
#include "os_arch_port.h"

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize scheduler state.
 *
 * @return None.
 */
void os_scheduler_init(void)
{
	/* TODO: initialize run queues and priority bitmap. */
}

/******************************************************************************************************/
/**
 * @brief Run scheduler selection loop.
 *
 * @return None.
 */
void os_scheduler_run(void)
{
	/* TODO: pick next runnable task. */
	OS_ARCH_CONTEXT_SWITCH_REQUEST();
}
