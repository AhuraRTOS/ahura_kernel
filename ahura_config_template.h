/**
 * @file ahura_config_template.h
 * @brief Template for the application's ahura_config.h — the complete Ahura
 *        kernel configuration with every option at its default value.
 *
 * NOT included by the kernel: copy this file into the application source
 * tree as ahura_config.h, adjust the values, and make its directory visible
 * to BOTH the application and the kernel library build (set OS_CONFIG_DIR
 * before add_subdirectory(ahura_kernel) — see the README "Configuration"
 * section). The kernel refuses to build without a complete ahura_config.h.
 *
 * This file is the single source of configuration: all options are plain
 * defines, so do not additionally define OS_CONFIG_ macros from the build
 * system (that would redefine them). Do not remove options either: an
 * incomplete configuration is rejected with a compile-time error rather
 * than silently misconfiguring the kernel.
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

#define OS_CONFIG_MUTEX_ENABLE            1U
#define OS_CONFIG_SEMAPHORE_ENABLE        1U
#define OS_CONFIG_QUEUE_ENABLE            1U
#define OS_CONFIG_EVENT_ENABLE            1U
#define OS_CONFIG_TIMER_ENABLE            1U
#define OS_CONFIG_WORK_ENABLE             1U
#define OS_CONFIG_MEMORY_POOL_ENABLE      1U

/* Kernel heap (os_alloc/os_free): first-fit allocator with coalescing over a
 * static heap of OS_CONFIG_HEAP_SIZE bytes. */
#define OS_CONFIG_ALLOC_ENABLE            1U

/* Note: the intrusive list module has no switch - the scheduler itself runs
 * on it, so it is always compiled in and its os_list_ API is always there. */

/* Fill task stacks with a pattern at creation and provide
 * os_task_stack_watermark_get() to measure worst-case stack usage. */
#define OS_CONFIG_STACK_WATERMARK_ENABLE  1U

/* Sample CPU load from the tick interrupt (idle vs non-idle) and provide
 * os_cpu_usage_get(): percentage of ticks that interrupted a non-idle task
 * since the previous call. Costs two counter updates per tick. */
#define OS_CONFIG_CPU_USAGE_ENABLE        1U

/*
 * ***********************************************************************************************************
 * Kernel sizing
 * ***********************************************************************************************************
*/

#define OS_CONFIG_TICK_HZ               1000U

/*
 * CPU clock source. The kernel reads the clock through the weak callback
 * os_clock_hz_get_cb() so any platform can plug in:
 *   0            auto: the callback returns the CMSIS SystemCoreClock global
 *                when the platform provides it (weak reference), else 0.
 *   > 0          fixed clock in Hz; the callback returns this constant
 *                (platforms without CMSIS and without dynamic scaling).
 * Platforms with their own convention override os_clock_hz_get_cb() instead.
 */
#define OS_CONFIG_CPU_CLOCK_HZ          0U

/* Kernel heap size in bytes for os_alloc/os_free. */
#define OS_CONFIG_HEAP_SIZE             4096U

/*
 * Priority 0 is the idle task and OS_CONFIG_MAX_PRIORITY is reserved for the
 * kernel service tasks (work queue, timers). User tasks must use priorities
 * 1 .. OS_CONFIG_MAX_PRIORITY-1, so this value must be at least 2 - and at
 * most 31, because the scheduler's ready bitmap is 32 bits wide.
 */
#define OS_CONFIG_MAX_PRIORITY          15U

/* Task table size; each enabled kernel service task (work, timer) occupies
 * one of these slots. */
#define OS_CONFIG_MAX_TASKS             10U

#define OS_CONFIG_MAX_TIMERS            8U
#define OS_CONFIG_MAX_WORKS             8U

/*
 * Minimum stack size in bytes. Must leave room for one hardware exception
 * frame (104 bytes with FPU lazy stacking) plus one software context frame
 * (100 bytes with FPU) on top of the task's own usage.
 */
#define OS_CONFIG_MIN_STACK_SIZE        256U

/* Stack sizes for the kernel service tasks; user callbacks run on these. */
#define OS_CONFIG_WORK_STACK_SIZE       512U
#define OS_CONFIG_TIMER_STACK_SIZE      512U

/* Which cores the kernel service tasks (and so the work handlers and timer
 * callbacks) may run on: core-affinity bitmasks, 0 = any core. Only
 * meaningful when OS_CONFIG_CORE_COUNT > 1; keep 0 on single-core builds. */
#define OS_CONFIG_WORK_CORE_AFFINITY    0U
#define OS_CONFIG_TIMER_CORE_AFFINITY   0U

/*
 * ***********************************************************************************************************
 * TrustZone security state (ARMv8-M cores only)
 * ***********************************************************************************************************
 *
 * Selects which ARMv8-M security state the kernel runs in; the three value
 * macros are kernel-owned (os_arch_port_common.h). On cores without the
 * Security Extension (M0/M0+/M3/M4/M7, or v8-M devices with TrustZone
 * disabled) keep OS_CONFIG_TRUSTZONE_DISABLED.
 *
 *   OS_CONFIG_TRUSTZONE_DISABLED    The kernel ignores TrustZone. Use when the
 *                                   Security Extension is absent or disabled.
 *   OS_CONFIG_TRUSTZONE_NON_SECURE  The kernel and all tasks run non-secure
 *                                   alongside separate secure firmware. Task
 *                                   frames use the non-secure EXC_RETURN and
 *                                   the port calls os_arch_tz_context_save_cb()
 *                                   / os_arch_tz_context_restore_cb() around
 *                                   every context switch so the application's
 *                                   secure-side glue can bank per-task secure
 *                                   contexts (secure stack / PSP_S).
 *   OS_CONFIG_TRUSTZONE_SECURE      The kernel and all tasks run entirely in
 *                                   the secure state; compile with -mcmse.
*/

#define OS_CONFIG_TRUSTZONE             OS_CONFIG_TRUSTZONE_DISABLED

/*
 * ***********************************************************************************************************
 * Multi-core (experimental scaffold)
 * ***********************************************************************************************************
 *
 * Number of cores that schedule tasks (max 31). Every scheduling core runs
 * its own PendSV/idle task and pulls from the shared ready lists honoring
 * each task's core_affinity mask; core 0 owns the time base and secondary
 * cores enter the scheduler through os_core_start(). The SoC layer must
 * provide os_arch_core_id_get_cb() (plus the IPI callback, and the hardware
 * spinlock callbacks on cores without LDREX/STREX, e.g. Cortex-M0+ SoCs);
 * see ahura_cb_template.c and the README "Multi-core" section.
*/

#define OS_CONFIG_CORE_COUNT            1U

/*
 * ***********************************************************************************************************
 * Tickless idle (experimental scaffold, not functional yet)
 * ***********************************************************************************************************
*/

#define OS_CONFIG_TICKLESS_ENABLE       0U
#define OS_CONFIG_TICKLESS_MIN_IDLE     2U
#define OS_CONFIG_LPTIM_CLOCK_HZ        32768U
#define OS_CONFIG_MAX_SUPPRESSED_TICKS  0x00FFFFFFUL

#endif /* AHURA_CONFIG_H */
