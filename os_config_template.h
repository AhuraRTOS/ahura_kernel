/**
 * @file os_config_template.h
 * @brief Template for the application's os_config.h — the complete Ahura
 *        kernel configuration with every option at its default value.
 *
 * NOT included by the kernel: copy this file into the application source
 * tree as os_config.h, adjust the values, and make its directory visible
 * to BOTH the application and the kernel library build (set OS_CONFIG_DIR
 * before add_subdirectory(ahura_kernel) — see the README "Configuration"
 * section). The kernel refuses to build without a complete os_config.h.
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

#ifndef OS_CONFIG_H
#define OS_CONFIG_H

/*
 * ***********************************************************************************************************
 * Feature switches (1 = compiled in, 0 = compiled out)
 * ***********************************************************************************************************
 *
 * The core (tasks, tick, delays, critical sections) is always available.
 * Disabling a feature removes its code, its API, and - for timer/work - its
 * kernel service task and stack.
*/

/* Mutexes always do single-level priority inheritance (like FreeRTOS/Zephyr): os_mutex_lock
 * boosts a lower-priority owner to the blocking waiter's (effective) priority for as long as it
 * holds the mutex, restoring it on os_mutex_unlock (accounting for other mutexes the task still
 * holds). Transitive/chained inheritance across multiple mutexes is NOT implemented - see
 * README "Timeout semantics". */
#define OS_CONFIG_MUTEX_ENABLE            1U

#define OS_CONFIG_SEMAPHORE_ENABLE        1U
#define OS_CONFIG_QUEUE_ENABLE            1U
#define OS_CONFIG_EVENT_ENABLE            1U
#define OS_CONFIG_TIMER_ENABLE            1U
#define OS_CONFIG_WORK_ENABLE             1U

/* Task notifications: a single overwrite uint32_t "mailbox" built into every task's own
 * control block (os_task_notify_give / os_task_notify_wait) - lets one task or an ISR signal
 * a specific task directly without allocating a separate semaphore/queue object. */
#define OS_CONFIG_TASK_NOTIFY_ENABLE      1U

/* Kernel heap (os_mem_alloc/os_mem_free): first-fit allocator with coalescing over a
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

/* os_init() unconditionally creates and starts a default application task
 * running the weak os_main() (override it in the application, e.g.
 * os_main.c, copied from os_main_template.c) - sized in "Kernel sizing"
 * below, see the README "Default application task" section. Not created
 * when OS_CONFIG_TEST_ENABLE below is 1: the self-test task runs alone
 * instead (see the README "Self-test suite" section).
 *
 * os_init() creates and starts a self-test task running the weak os_test()
 * (empty by default; link ahura_kernel/test - the "os_test" library - to run
 * the real kernel self-test suite there) - sized in "Kernel sizing" below,
 * see the README "Self-test suite" section. Off by default: opt in per
 * project. */
#define OS_CONFIG_TEST_ENABLE             0U

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

/* Kernel heap size in bytes for os_mem_alloc/os_mem_free. */
#define OS_CONFIG_HEAP_SIZE             4096U

/* Task table size; each enabled kernel service task (work, timer) occupies
 * one of these slots - and so does the default application task (tsk_main,
 * unconditional unless OS_CONFIG_TEST_ENABLE above is 1) and the self-test
 * task (tsk_test, OS_CONFIG_TEST_ENABLE above) when enabled. Budget for all
 * of them plus the application's own tasks. */
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

/* Stack size / priority for the default application task (os_main(), see
 * the README "Default application task" section). os_init() discards the
 * creation status for this task (void, matching the work/timer system-init
 * calls above) - an out-of-range priority (must be
 * OS_TASK_PRIO_USER_MIN..USER_MAX) or a too-small stack (must be at
 * least OS_CONFIG_MIN_STACK_SIZE above) fails SILENTLY: the firmware still
 * builds, boots and schedules, but os_main() simply never runs. */
#define OS_CONFIG_MAIN_TASK_STACK_SIZE  1024U
#define OS_CONFIG_MAIN_TASK_PRIORITY    1U

/* Stack size / priority for the self-test task (os_test(), see
 * OS_CONFIG_TEST_ENABLE above and the README "Self-test suite" section).
 * The suite itself needs a generous stack - it exercises every kernel
 * feature, including nested helper tasks. Same silent-failure caveat as
 * OS_CONFIG_MAIN_TASK_* above. */
#define OS_CONFIG_TEST_STACK_SIZE       2048U
#define OS_CONFIG_TEST_PRIORITY         2U

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
 * Kernel interrupt mask (zero-latency interrupts, BASEPRI)
 * ***********************************************************************************************************
 *
 * 0 (default): kernel critical sections mask ALL interrupts with PRIMASK.
 * Simple contract - every ISR may call the ISR-safe kernel APIs - at the
 * cost of the kernel's critical sections adding latency to every interrupt.
 * The only choice on cores without BASEPRI (Cortex-M0/M0+/M23).
 *
 * Nonzero: kernel critical sections raise BASEPRI to this value instead
 * (Cortex-M3/M4/M7/M33/M35P/M52/M55/M85). Interrupts whose NVIC priority is
 * numerically LOWER (more urgent) than this value are never masked by the
 * kernel - zero kernel-induced latency - but they MUST NOT call any kernel
 * API (os_critical_enter traps violations; a misconfigured ISR then parks in
 * os_arch_config_fault_trap). Interrupts at numerically equal or higher
 * priority values keep full kernel API access.
 *
 * The value is the raw 8-bit priority byte as written to the NVIC, i.e.
 * pre-shifted into the device's implemented priority bits:
 *
 *   value = logical_priority << (8 - __NVIC_PRIO_BITS)
 *
 * Example, STM32 (4 priority bits): 0x50 = logical priority 5. ISRs at
 * logical 0..4 become zero-latency/no-kernel-API; ISRs at logical 5..15 may
 * use the kernel. os_arch_init verifies two things at boot and parks in
 * os_arch_config_fault_trap on violation: the value must fit the device's
 * implemented priority bits exactly (no truncation), and the NVIC priority
 * grouping must dedicate every implemented bit to preemption - no
 * subpriority bits (STM32 HAL: NVIC_PRIORITYGROUP_4, the CubeMX default).
*/

#define OS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY  0U

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
 * see os_cb_template.c and the README "Multi-core" section.
 *
 * Two more preconditions the kernel cannot verify or provide for on its own
 * - both are SoC/hardware properties, not something a portable C source file
 * can guarantee - so satisfying them is the SoC integrator's responsibility
 * before setting this above 1:
 *
 *   1. Global exclusive monitor. The kernel's inter-core spinlock
 *      (os_arch_port_common.h) is built on LDREX/STREX whenever the target
 *      has them (all v7-M/v8-M cores). STREX only excludes another core when
 *      the interconnect implements a GLOBAL exclusive monitor for the lock's
 *      address AND that address is Shareable-mapped; both are SoC/MPU
 *      choices. Without them, both cores' local monitors can grant STREX
 *      success simultaneously and the "lock" silently stops excluding
 *      anything - no fault, just corruption. Verify your SoC's TRM documents
 *      a global monitor for the memory region the spinlock lives in (a
 *      static in this library's .bss), and mark that region Shareable.
 *   2. Cache coherency. Cortex-M has no inter-core cache coherency. On D-cache
 *      cores (M7, M55, M85) a volatile store to write-back-cacheable SRAM
 *      only reaches the local cache; DSB is not cache maintenance. Every
 *      cross-core shared kernel object (the ready/delay lists, the work/timer
 *      registries, os_task_current[], the spinlock word itself) must live in
 *      a non-cacheable or cache-coherent region, or the SoC must bracket
 *      cross-core handoffs with explicit clean/invalidate - typically done by
 *      placing the whole kernel's shared statics in a dedicated non-cacheable
 *      MPU region via the linker script.
 *
 * Neither precondition has a portable fallback for cache coherency - that
 * still requires SoC-specific MPU/linker placement. The exclusive-monitor
 * precondition does have one: set OS_CONFIG_MULTICORE_SPINLOCK_SOC_BACKEND
 * below to route the kernel spinlock through your own hardware semaphore
 * (os_arch_spinlock_acquire_cb/_release_cb in os_cb_template.c) instead of
 * the built-in LDREX/STREX backend.
*/

#define OS_CONFIG_CORE_COUNT            1U

/*
 * 0 (default): the kernel spinlock uses the built-in LDREX/STREX backend on
 * cores that have it (all v7-M/v8-M cores); ARMv6-M multi-core SoCs (no
 * LDREX/STREX) always use the callback backend regardless of this setting.
 * 1: force the callback backend even on an exclusives-capable core - set
 * this when your SoC's interconnect has no GLOBAL exclusive monitor for the
 * spinlock's memory, or that memory cannot be marked Shareable (see the
 * OS_CONFIG_CORE_COUNT precondition notes above); implement
 * os_arch_spinlock_acquire_cb/_release_cb (os_cb_template.c) against your
 * SoC's hardware semaphore in that case. Only meaningful when
 * OS_CONFIG_CORE_COUNT > 1; keep 0 on single-core builds.
*/

#define OS_CONFIG_MULTICORE_SPINLOCK_SOC_BACKEND  0U

/*
 * ***********************************************************************************************************
 * Tickless idle (experimental scaffold, not functional yet)
 * ***********************************************************************************************************
*/

#define OS_CONFIG_TICKLESS_ENABLE       0U
#define OS_CONFIG_TICKLESS_MIN_IDLE     2U
#define OS_CONFIG_LPTIM_CLOCK_HZ        32768U
#define OS_CONFIG_MAX_SUPPRESSED_TICKS  0x00FFFFFFUL

#endif /* OS_CONFIG_H */
