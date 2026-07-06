/**
 * @file ahura_config.h
 * @brief Ahura kernel build-time configuration defaults.
 *
 * Every option is a default: define it in the build system (e.g.
 * target_compile_definitions(... OS_CONFIG_TIMER_ENABLE=0U)) to override
 * without editing this file.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

#ifndef AHURA_CONFIG_H
#define AHURA_CONFIG_H

/*
 * ***********************************************************************************************************
 * Feature switches (1 = compiled in, 0 = compiled out)
 * ***********************************************************************************************************
 *
 * The core (tasks, tick, delays, critical sections) is always available.
 * Disabling a feature removes its code, its API, and - for timer/work - its
 * kernel service task and stack.
*/

#ifndef OS_CONFIG_MUTEX_ENABLE
#define OS_CONFIG_MUTEX_ENABLE            1U
#endif

#ifndef OS_CONFIG_SEMAPHORE_ENABLE
#define OS_CONFIG_SEMAPHORE_ENABLE        1U
#endif

#ifndef OS_CONFIG_QUEUE_ENABLE
#define OS_CONFIG_QUEUE_ENABLE            1U
#endif

#ifndef OS_CONFIG_EVENT_ENABLE
#define OS_CONFIG_EVENT_ENABLE            1U
#endif

#ifndef OS_CONFIG_TIMER_ENABLE
#define OS_CONFIG_TIMER_ENABLE            1U
#endif

#ifndef OS_CONFIG_WORK_ENABLE
#define OS_CONFIG_WORK_ENABLE             1U
#endif

#ifndef OS_CONFIG_MEMORY_POOL_ENABLE
#define OS_CONFIG_MEMORY_POOL_ENABLE      1U
#endif

#ifndef OS_CONFIG_LIST_ENABLE
#define OS_CONFIG_LIST_ENABLE             1U
#endif

/* Fill task stacks with a pattern at creation and provide
 * os_task_stack_watermark_get() to measure worst-case stack usage. */
#ifndef OS_CONFIG_STACK_WATERMARK_ENABLE
#define OS_CONFIG_STACK_WATERMARK_ENABLE  1U
#endif

/*
 * ***********************************************************************************************************
 * Kernel sizing
 * ***********************************************************************************************************
*/

#ifndef OS_CONFIG_TICK_HZ
#define OS_CONFIG_TICK_HZ               1000U
#endif

/*
 * Priority 0 is the idle task and OS_CONFIG_MAX_PRIORITY is reserved for the
 * kernel service tasks (work queue, timers). User tasks must use priorities
 * 1 .. OS_CONFIG_MAX_PRIORITY-1, so this value must be at least 2.
 */
#ifndef OS_CONFIG_MAX_PRIORITY
#define OS_CONFIG_MAX_PRIORITY          15U
#endif

/* Task table size; each enabled kernel service task (work, timer) occupies
 * one of these slots. */
#ifndef OS_CONFIG_MAX_TASKS
#define OS_CONFIG_MAX_TASKS             10U
#endif

#ifndef OS_CONFIG_MAX_TIMERS
#define OS_CONFIG_MAX_TIMERS            8U
#endif

#ifndef OS_CONFIG_MAX_WORKS
#define OS_CONFIG_MAX_WORKS             8U
#endif

/*
 * Minimum stack size in bytes. Must leave room for one hardware exception
 * frame (104 bytes with FPU lazy stacking) plus one software context frame
 * (100 bytes with FPU) on top of the task's own usage.
 */
#ifndef OS_CONFIG_MIN_STACK_SIZE
#define OS_CONFIG_MIN_STACK_SIZE        256U
#endif

/* Stack sizes for the kernel service tasks; user callbacks run on these. */
#ifndef OS_CONFIG_WORK_STACK_SIZE
#define OS_CONFIG_WORK_STACK_SIZE       512U
#endif

#ifndef OS_CONFIG_TIMER_STACK_SIZE
#define OS_CONFIG_TIMER_STACK_SIZE      512U
#endif

/*
 * ***********************************************************************************************************
 * Tickless idle (experimental scaffold)
 * ***********************************************************************************************************
*/

#ifndef OS_CONFIG_TICKLESS_ENABLE
#define OS_CONFIG_TICKLESS_ENABLE       0U
#endif

#ifndef OS_CONFIG_TICKLESS_MIN_IDLE
#define OS_CONFIG_TICKLESS_MIN_IDLE     2U
#endif

#ifndef OS_CONFIG_LPTIM_CLOCK_HZ
#define OS_CONFIG_LPTIM_CLOCK_HZ        32768U
#endif

#ifndef OS_CONFIG_MAX_SUPPRESSED_TICKS
#define OS_CONFIG_MAX_SUPPRESSED_TICKS  0x00FFFFFFUL
#endif

#endif /* AHURA_CONFIG_H */
