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
 * Laid out in three parts, most important first:
 *
 *   PART 1  CORE - always compiled in, so these always apply. Set them first.
 *   PART 2  OPTIONAL FEATURES - one section per feature, each holding its
 *           _ENABLE switch together with the sizing that switch controls, so
 *           turning a feature off tells you exactly which values stop
 *           mattering. Same order as PART 2 of ahura.h.
 *   PART 3  PLATFORM - target properties (TrustZone, core count, tickless).
 *           Dictated by the hardware, not by application design.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

#ifndef OS_CONFIG_H
#define OS_CONFIG_H

/*
 * ***********************************************************************************************************
 * PART 1 - CORE (always compiled in; no switch removes any of this)
 * ***********************************************************************************************************
 *
 * Tasks, the tick, delays, critical sections and the intrusive list have no
 * switch at all - the scheduler itself runs on them - so their API is always
 * present and the values below always apply.
*/

/*
 * ***********************************************************************************************************
 * Tick rate
 * ***********************************************************************************************************
*/

#define OS_CONFIG_TICK_HZ                   1000U

/* The CPU clock is NOT configured here. The kernel reads the live CMSIS
 * SystemCoreClock variable, which the device's own SystemInit() sets and
 * SystemCoreClockUpdate() refreshes after every clock-tree change - a
 * build-time constant could not follow a runtime switch. Devices whose startup
 * code does not define that symbol just define it themselves; see the kernel
 * README, "Platform clock". */

/*
 * ***********************************************************************************************************
 * Task table and stacks
 * ***********************************************************************************************************
*/

/* Task table size; each enabled kernel service task (work, timer, log) occupies
 * one of these slots - and so does the default application task (tsk_main,
 * unconditional unless OS_CONFIG_TEST_ENABLE in PART 2 is 1) and the self-test
 * task (tsk_test) when enabled. Budget for all of them plus the application's
 * own tasks. */
#define OS_CONFIG_MAX_TASKS                 10U

/**
 * Minimum stack size in bytes. Must leave room for one hardware exception
 * frame (104 bytes with FPU lazy stacking) plus one software context frame
 * (100 bytes with FPU) on top of the task's own usage.
 */
#define OS_CONFIG_MIN_STACK_SIZE            256U

/*
 * ***********************************************************************************************************
 * Default application task
 * ***********************************************************************************************************
*/

/* os_init() unconditionally creates and starts a default application task running os_main(),
 * which the application must define in its own os_main.c (copied from os_main_template.c) -
 * the kernel ships no stub, so a missing one is a link error. See the README "Default
 * application task" section. Not created when OS_CONFIG_TEST_ENABLE (PART 2) is 1: the
 * self-test task runs alone instead, and no os_main.c is needed at all.
 *
 * os_init() discards the creation status for this task (void, matching the work/timer
 * system-init calls) - an out-of-range priority (must be
 * OS_TASK_PRIO_USER_MIN..USER_MAX) or a too-small stack (must be at least
 * OS_CONFIG_MIN_STACK_SIZE above) fails SILENTLY: the firmware still builds, boots and
 * schedules, but os_main() simply never runs. */
#define OS_CONFIG_MAIN_TASK_STACK_SIZE      1024U
#define OS_CONFIG_MAIN_TASK_PRIORITY        1U

/*
 * ***********************************************************************************************************
 * Kernel interrupt mask (zero-latency interrupts, BASEPRI)
 * ***********************************************************************************************************
*/

/**
 * A priority NUMBER, not a bitmask: the raw 8-bit byte the NVIC stores, i.e. the logical
 * priority already shifted into the device's implemented priority bits.
 *
 *     value = logical_priority << (8 - __NVIC_PRIO_BITS)
 *
 * Example, a device with 4 implemented bits: logical 5 -> (5 << 4) = 0x50.
 *
 *   0        Critical sections mask ALL interrupts (PRIMASK). Every ISR may call the ISR-safe
 *            kernel APIs. The only choice on cores without BASEPRI (Cortex-M0/M0+/M23).
 *   nonzero  Critical sections raise BASEPRI to this value. ISRs more urgent than it
 *            (numerically lower) are never masked - zero kernel-induced latency - but MUST NOT
 *            call any kernel API. ISRs at this value or higher keep full API access.
 *
 * So with 0x50: logical 0..4 are zero-latency/no-kernel-API, logical 5..15 may use the kernel.
 * Verified at boot (parks in os_arch_config_fault_trap on violation): the value must fit the
 * implemented priority bits exactly, and NVIC grouping must give every implemented bit to
 * preemption, with no subpriority bits.
 */

#define OS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY  0U

/*
 * ***********************************************************************************************************
 * PART 2 - OPTIONAL FEATURES (1 = compiled in, 0 = compiled out)
 * ***********************************************************************************************************
 *
 * Disabling a feature removes its code, its API, and - for timer/work/log -
 * its kernel service task and stack. Each section below carries its own
 * sizing, which is dead weight to reason about once the switch is 0.
*/

/*
 * ***********************************************************************************************************
 * Mutex
 * ***********************************************************************************************************
*/

/* Mutexes always do single-level priority inheritance: os_mutex_lock
 * boosts a lower-priority owner to the blocking waiter's (effective) priority for as long as it
 * holds the mutex, restoring it on os_mutex_unlock (accounting for other mutexes the task still
 * holds). Transitive/chained inheritance across multiple mutexes is NOT implemented - see
 * README "Timeout semantics". */
#define OS_CONFIG_MUTEX_ENABLE              1U

/*
 * ***********************************************************************************************************
 * Semaphore, queue, event group
 * ***********************************************************************************************************
*/

#define OS_CONFIG_SEMAPHORE_ENABLE          1U
#define OS_CONFIG_QUEUE_ENABLE              1U
#define OS_CONFIG_EVENT_ENABLE              1U

/*
 * ***********************************************************************************************************
 * Software timers
 * ***********************************************************************************************************
*/

/* Timer callbacks run on the kernel timer task (tsk_timer), which occupies one
 * OS_CONFIG_MAX_TASKS slot and runs the callbacks on the stack sized here. */
#define OS_CONFIG_TIMER_ENABLE              1U
#define OS_CONFIG_MAX_TIMERS                8U
#define OS_CONFIG_TIMER_STACK_SIZE          512U

/* Which cores the timer task (and so the timer callbacks) may run on:
 * a core-affinity bitmask, 0 = any core. Only meaningful when
 * OS_CONFIG_CORE_COUNT (PART 3) > 1; keep 0 on single-core builds. */
#define OS_CONFIG_TIMER_CORE_AFFINITY       0U

/*
 * ***********************************************************************************************************
 * Work queue
 * ***********************************************************************************************************
*/

/* Work handlers run on the kernel work task (tsk_work), which occupies one
 * OS_CONFIG_MAX_TASKS slot and runs the handlers on the stack sized here. */
#define OS_CONFIG_WORK_ENABLE               1U
#define OS_CONFIG_MAX_WORKS                 8U
#define OS_CONFIG_WORK_STACK_SIZE           512U

/* Which cores the work task (and so the work handlers) may run on: a
 * core-affinity bitmask, 0 = any core. Only meaningful when
 * OS_CONFIG_CORE_COUNT (PART 3) > 1; keep 0 on single-core builds. */
#define OS_CONFIG_WORK_CORE_AFFINITY        0U

/*
 * ***********************************************************************************************************
 * Task notifications
 * ***********************************************************************************************************
*/

/* Task notifications: a single overwrite uint32_t "mailbox" built into every task's own
 * control block (os_task_notify_give / os_task_notify_wait) - lets one task or an ISR signal
 * a specific task directly without allocating a separate semaphore/queue object. */
#define OS_CONFIG_TASK_NOTIFY_ENABLE        1U

/*
 * ***********************************************************************************************************
 * Kernel heap
 * ***********************************************************************************************************
*/

/* Kernel heap (os_mem_alloc/os_mem_free): first-fit allocator with coalescing over a
 * static heap of OS_CONFIG_HEAP_SIZE bytes. Also what os_queue_create allocates from. */
#define OS_CONFIG_ALLOC_ENABLE              1U
#define OS_CONFIG_HEAP_SIZE                 4096U

/*
 * ***********************************************************************************************************
 * Atomics
 * ***********************************************************************************************************
*/

/* Atomic operations on a single word (os_atomic_*): indivisible add/sub/bitwise/compare-and-swap
 * updates that no task, ISR or core can observe half-finished. Lock-free where the ISA has
 * LDREX/STREX, otherwise implemented by briefly excluding interrupts. Costs no RAM and no kernel
 * task; unused operations are dropped by the linker, so disabling this removes API surface rather
 * than footprint. */
#define OS_CONFIG_ATOMIC_ENABLE             1U

/*
 * ***********************************************************************************************************
 * Diagnostics
 * ***********************************************************************************************************
*/

/* Fill task stacks with a pattern at creation and provide
 * os_task_stack_watermark_get() to measure worst-case stack usage. */
#define OS_CONFIG_STACK_WATERMARK_ENABLE    1U

/* Sample CPU load from the tick interrupt (idle vs non-idle) and provide
 * os_cpu_usage_get(): percentage of ticks that interrupted a non-idle task
 * since the previous call. Costs two counter updates per tick. */
#define OS_CONFIG_CPU_USAGE_ENABLE          1U

/*
 * ***********************************************************************************************************
 * Assertions
 * ***********************************************************************************************************
*/

/* OS_ASSERT(): catch static programming errors (a NULL object handle, a blocking call made
 * from an ISR, an unbalanced critical section) at the point they happen instead of only
 * through a status code the caller may ignore. Runtime outcomes that have a documented
 * status - NOT_OWNER, BUSY, FULL, EMPTY, TIMEOUT - are never asserted on: callers are
 * entitled to attempt the operation and handle the result.
 *
 * A failure calls os_assert_failed_cb(), which the application must define, then parks the
 * core with interrupts masked so a debugger lands on the cause. Assertions only ADD checks:
 * every API still returns the same status either way, so turning this off leaves behavior
 * unchanged. */
#define OS_CONFIG_ASSERT_ENABLE             1U

/*
 * ***********************************************************************************************************
 * Buffered logging
 * ***********************************************************************************************************
*/

/* Buffered debug logging (OS_LOG_ERROR/WARN/INFO/DEBUG): printf-style calls format into a ring
 * buffer and return immediately, and a low-priority kernel task (tsk_log) hands finished bytes
 * to os_log_output_cb() for the application to transmit. Safe from tasks and ISRs, and it never
 * blocks the caller. Two costs to budget for: tsk_log occupies one OS_CONFIG_MAX_TASKS slot,
 * and formatting uses libc vsnprintf, which pulls newlib's formatter into the link (~1-3 KB) if
 * the application does not already use printf. As usual, %f additionally needs the linker flag
 * -u _printf_float. See the README "Debugging" section. */
#define OS_CONFIG_LOG_ENABLE                1U

/**
 * Log sizing (only meaningful when OS_CONFIG_LOG_ENABLE above is 1).
 *
 * LEVEL        Calls above this compile to nothing at the call site, arguments included:
 *              OS_LOG_LEVEL_NONE / _ERROR / _WARN / _INFO / _DEBUG.
 * BUFFER_SIZE  Ring buffer in bytes. Sized for the burst you want to survive: a task logging
 *              faster than the UART drains simply loses the excess (counted, then reported).
 * LINE_MAX     Longest single formatted line. Also the scratch buffer os_log_write() puts on
 *              the CALLER's stack, so every task that logs needs this much headroom. Longer
 *              lines are truncated, never overflowed.
 * TASK_*       tsk_log, which drains the ring and calls os_log_output_cb(). Its stack must hold
 *              that callback. Keep the priority low: logging must never preempt real work.
 */
#define OS_CONFIG_LOG_LEVEL                 OS_LOG_LEVEL_INFO
#define OS_CONFIG_LOG_BUFFER_SIZE           1024U
#define OS_CONFIG_LOG_LINE_MAX              128U
#define OS_CONFIG_LOG_TASK_STACK_SIZE       512U
#define OS_CONFIG_LOG_TASK_PRIORITY         1U

/*
 * ***********************************************************************************************************
 * Self-test suite
 * ***********************************************************************************************************
*/

/* os_init() creates and starts a self-test task running os_test(), which comes from the
 * ahura_kernel/test library (the "os_test" CMake target) - link it and the suite runs, forget
 * it and the link fails. Off by default: opt in per project. When 1, the default application
 * task (OS_CONFIG_MAIN_TASK_* in PART 1) is NOT created and the suite runs alone; see the
 * README "Self-test suite" section.
 *
 * The suite itself needs a generous stack - it exercises every kernel feature, including
 * nested helper tasks. Same silent-failure caveat as OS_CONFIG_MAIN_TASK_*. */
#define OS_CONFIG_TEST_ENABLE               0U
#define OS_CONFIG_TEST_STACK_SIZE           2048U
#define OS_CONFIG_TEST_PRIORITY             2U

/*
 * ***********************************************************************************************************
 * PART 3 - PLATFORM (target properties, not application design choices)
 * ***********************************************************************************************************
*/

/*
 * ***********************************************************************************************************
 * TrustZone security state (ARMv8-M cores only)
 * ***********************************************************************************************************
*/

/**
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

#define OS_CONFIG_TRUSTZONE                 OS_CONFIG_TRUSTZONE_DISABLED

/*
 * ***********************************************************************************************************
 * Multi-core (experimental scaffold)
 * ***********************************************************************************************************
*/

/**
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
 *
 * The kernel service tasks are placed with OS_CONFIG_WORK_CORE_AFFINITY and
 * OS_CONFIG_TIMER_CORE_AFFINITY (PART 2).
 */

#define OS_CONFIG_CORE_COUNT                1U

/**
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

#define OS_CONFIG_TICKLESS_ENABLE           0U
#define OS_CONFIG_TICKLESS_MIN_IDLE         2U
#define OS_CONFIG_LPTIM_CLOCK_HZ            32768U
#define OS_CONFIG_MAX_SUPPRESSED_TICKS      0x00FFFFFFUL

#endif /* OS_CONFIG_H */
