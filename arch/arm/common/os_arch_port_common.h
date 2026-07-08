/**
 * @file os_arch_port_common.h
 * @brief Common architecture port interface shared by all ARM Cortex-M variants.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

#ifndef OS_ARCH_PORT_COMMON_H
#define OS_ARCH_PORT_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TrustZone mode values for OS_CONFIG_TRUSTZONE: kernel-owned so an
 * application configuration can reference but never change the encoding. */
#define OS_CONFIG_TRUSTZONE_DISABLED    0U
#define OS_CONFIG_TRUSTZONE_NON_SECURE  1U
#define OS_CONFIG_TRUSTZONE_SECURE      2U

/*
 * The application provides the kernel configuration: copy
 * ahura_kernel/ahura_config_template.h into the project as ahura_config.h
 * and make its directory visible to the kernel build (OS_CONFIG_DIR in
 * CMake, see the README "Configuration" section).
 */
#if defined(__has_include)
#if !__has_include("ahura_config.h")
#error "No ahura_config.h found: copy ahura_kernel/ahura_config_template.h into your project as ahura_config.h and put its directory on the kernel include path (OS_CONFIG_DIR)."
#endif
#endif

#include "ahura_config.h"

/* Reject incomplete configurations: a missing option would otherwise read
 * as 0 in #if directives and silently disable or misconfigure features.
 * Start from ahura_config_template.h, which lists every required option. */
#if !defined(OS_CONFIG_MUTEX_ENABLE) || !defined(OS_CONFIG_SEMAPHORE_ENABLE) ||                        \
    !defined(OS_CONFIG_QUEUE_ENABLE) || !defined(OS_CONFIG_EVENT_ENABLE) ||                            \
    !defined(OS_CONFIG_TIMER_ENABLE) || !defined(OS_CONFIG_WORK_ENABLE) ||                             \
    !defined(OS_CONFIG_MEMORY_POOL_ENABLE) || !defined(OS_CONFIG_ALLOC_ENABLE) ||                      \
    !defined(OS_CONFIG_STACK_WATERMARK_ENABLE) ||                                                      \
    !defined(OS_CONFIG_CPU_USAGE_ENABLE) || !defined(OS_CONFIG_TICK_HZ) ||                             \
    !defined(OS_CONFIG_CPU_CLOCK_HZ) || !defined(OS_CONFIG_HEAP_SIZE) ||                               \
    !defined(OS_CONFIG_MAX_PRIORITY) || !defined(OS_CONFIG_MAX_TASKS) ||                               \
    !defined(OS_CONFIG_MAX_TIMERS) || !defined(OS_CONFIG_MAX_WORKS) ||                                 \
    !defined(OS_CONFIG_MIN_STACK_SIZE) || !defined(OS_CONFIG_WORK_STACK_SIZE) ||                       \
    !defined(OS_CONFIG_TIMER_STACK_SIZE) || !defined(OS_CONFIG_WORK_CORE_AFFINITY) ||                  \
    !defined(OS_CONFIG_TIMER_CORE_AFFINITY) || !defined(OS_CONFIG_TRUSTZONE) ||                        \
    !defined(OS_CONFIG_CORE_COUNT) || !defined(OS_CONFIG_TICKLESS_ENABLE) ||                           \
    !defined(OS_CONFIG_TICKLESS_MIN_IDLE) || !defined(OS_CONFIG_LPTIM_CLOCK_HZ) ||                     \
    !defined(OS_CONFIG_MAX_SUPPRESSED_TICKS)
#error "ahura_config.h is incomplete: it must define every option listed in ahura_kernel/ahura_config_template.h."
#endif

#if (OS_CONFIG_CORE_COUNT < 1U)
#error "OS_CONFIG_CORE_COUNT must be at least 1."
#endif

#if (OS_CONFIG_CORE_COUNT > 31U)
#error "OS_CONFIG_CORE_COUNT must be at most 31 (core affinity masks are 32 bits wide)."
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

/* CMSIS-style register qualifiers; defined here so the kernel does not depend
 * on a CMSIS core header (identical to the CMSIS definitions when both are
 * seen). Use __IO instead of a bare volatile for memory-mapped registers and
 * shared kernel state. */
#ifndef __IO
#define __IO volatile             /*!< read/write */
#endif
#ifndef __I
#define __I  volatile const       /*!< read only  */
#endif
#ifndef __O
#define __O  volatile             /*!< write only */
#endif

/* Weak-linkage marker for user-overridable defaults (the _cb callbacks and
 * optional linker symbols). */
#ifndef OS_WEAK
#if defined(__GNUC__)
#define OS_WEAK __attribute__((weak))
#else
#define OS_WEAK
#endif
#endif

/*
 * Architecture capabilities derived from the compiler target: TrustZone (the
 * ARMv8-M Security Extension) and exclusive load/store (LDREX/STREX, absent
 * on ARMv6-M).
 */
#if defined(__ARM_ARCH_8M_BASE__) || defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__)
#define OS_ARCH_HAS_TRUSTZONE             1
#else
#define OS_ARCH_HAS_TRUSTZONE             0
#endif

#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || (OS_ARCH_HAS_TRUSTZONE == 1)
#define OS_ARCH_HAS_EXCLUSIVES            1
#else
#define OS_ARCH_HAS_EXCLUSIVES            0
#endif

/* Validate the configured TrustZone mode against the target early, with
 * readable errors instead of a mis-built kernel. __ARM_FEATURE_CMSE is 3 when
 * compiling secure code (-mcmse) and 1 for plain v8-M builds. */
#if (OS_CONFIG_TRUSTZONE != OS_CONFIG_TRUSTZONE_DISABLED) && (OS_ARCH_HAS_TRUSTZONE == 0)
#error "OS_CONFIG_TRUSTZONE requires an ARMv8-M core (Cortex-M23/M33/M35P/M52/M55/M85)."
#endif

#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_SECURE) && (!defined(__ARM_FEATURE_CMSE) || (__ARM_FEATURE_CMSE < 3))
#error "OS_CONFIG_TRUSTZONE_SECURE: compile the kernel as secure code (-mcmse)."
#endif

#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE) && defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE >= 3)
#error "OS_CONFIG_TRUSTZONE_NON_SECURE: do not compile the kernel with -mcmse."
#endif

#define OS_ARCH_REG_ICSR                  (*(__IO uint32_t *)0xE000ED04UL)
#define OS_ARCH_ICSR_PENDSVSET_MSK        (1UL << 28)
#define OS_ARCH_STACK_ALIGNMENT_BYTES     4U
#define OS_ARCH_DSB()                     __asm volatile("dsb 0xF" ::: "memory")
#define OS_ARCH_ISB()                     __asm volatile("isb 0xF" ::: "memory")

/*
 * The System Control Space is banked per core, so this pends PendSV on the
 * calling core - which is correct now that every scheduling core runs its
 * own PendSV. When a task can only run on another core, the scheduler
 * routes the request through the SoC IPI callback instead (task.c,
 * os_task_preempt_request).
 */
#define OS_ARCH_CONTEXT_SWITCH_REQUEST()  do { OS_ARCH_REG_ICSR = OS_ARCH_ICSR_PENDSVSET_MSK; OS_ARCH_DSB(); OS_ARCH_ISB(); } while (0)
#define OS_ARCH_IRQ_DISABLE()             do { __asm volatile("cpsid i" ::: "memory"); OS_ARCH_DSB(); OS_ARCH_ISB(); } while (0)
#define OS_ARCH_IRQ_ENABLE()              do { __asm volatile("cpsie i" ::: "memory"); OS_ARCH_ISB(); } while (0)
#define OS_ARCH_IDLE()                    do { __asm volatile("wfi"); } while (0)
#define OS_ARCH_SLEEP(ticks)              do { os_arch_sleep_prepare((ticks)); OS_ARCH_DSB(); __asm volatile("wfi"); OS_ARCH_ISB(); } while (0)

/*
 * ***********************************************************************************************************
 * Inline helpers
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Read the PRIMASK register (1 when interrupts are masked).
 */
static inline uint32_t os_arch_primask_get(void)
{
    uint32_t primask;

    __asm volatile("mrs %0, primask" : "=r"(primask));

    return primask;
}

/******************************************************************************************************/
/**
 * @brief Return true when executing in interrupt (handler) context.
 */
static inline bool os_arch_in_isr(void)
{
    uint32_t ipsr;

    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));

    return (ipsr != 0U);
}

/******************************************************************************************************/
/**
 * @brief Index of the highest set bit in a non-zero bitmap (the scheduler's ready-priority pick).
 *        One CLZ instruction on ARMv7-M and up; ARMv6-M has no CLZ, so GCC emits its small
 *        library routine there - still cheaper than scanning the task table.
 */
static inline uint32_t os_arch_highest_bit_get(uint32_t bitmap)
{
    return 31U - (uint32_t)__builtin_clz(bitmap);
}

/******************************************************************************************************/
/**
 * @brief Index of the lowest set bit in a non-zero bitmap (picks the IPI target from an
 *        affinity mask).
 */
static inline uint32_t os_arch_lowest_bit_get(uint32_t bitmap)
{
    return (uint32_t)__builtin_ctz(bitmap);
}

/*
 * ***********************************************************************************************************
 * Multi-core primitives
 * ***********************************************************************************************************
 *
 * Cortex-M has no architectural core-id register and no architectural IPI, so
 * on multi-core builds the SoC layer supplies both through _cb callbacks
 * (e.g. RP2040: SIO CPUID and the inter-core FIFO/doorbell). The spinlock is
 * implemented with LDREX/STREX where the ISA provides them; ARMv6-M SoCs must
 * route it to their hardware spinlocks instead.
*/

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief SoC callback: return the index of the calling core (0-based). Weak default returns 0.
 */
uint32_t os_arch_core_id_get_cb(void);

/******************************************************************************************************/
/**
 * @brief SoC callback: interrupt another core so it re-evaluates scheduling. Weak default does nothing
 *        (the target core then reacts at its next tick).
 */
void os_arch_core_ipi_request_cb(uint32_t core_id);
#endif /* OS_CONFIG_CORE_COUNT > 1U */

typedef struct
{
    __IO uint32_t locked;

} os_arch_spinlock_t;

#define OS_ARCH_SPINLOCK_INIT  { 0U }

/******************************************************************************************************/
/**
 * @brief Index of the calling core; always 0 on single-core builds.
 */
static inline uint32_t os_arch_core_id_get(void)
{
#if (OS_CONFIG_CORE_COUNT > 1U)
    return os_arch_core_id_get_cb();
#else
    return 0U;
#endif
}

#if (OS_CONFIG_CORE_COUNT > 1U) && (OS_ARCH_HAS_EXCLUSIVES == 0)
/******************************************************************************************************/
/**
 * @brief SoC callbacks backing the kernel spinlock on cores without LDREX/STREX
 *        (ARMv6-M multi-core SoCs, e.g. hardware SIO spinlocks on the RP2040).
 *        No default is provided on purpose: a missing implementation must fail
 *        at link time rather than silently not lock.
 */
void os_arch_spinlock_acquire_cb(os_arch_spinlock_t *lock);
void os_arch_spinlock_release_cb(os_arch_spinlock_t *lock);
#endif

/******************************************************************************************************/
/**
 * @brief Acquire an inter-core spinlock (busy-waits; call with interrupts disabled).
 *        Compiles to nothing on single-core builds.
 */
static inline void os_arch_spinlock_acquire(os_arch_spinlock_t *lock)
{
#if (OS_CONFIG_CORE_COUNT == 1U)
    (void)lock;
#elif (OS_ARCH_HAS_EXCLUSIVES == 1)
    uint32_t fail;

    do
    {
        uint32_t current;

        do
        {
            __asm volatile("ldrex %0, [%1]" : "=r"(current) : "r"(&lock->locked) : "memory");
        } while (current != 0U);

        __asm volatile("strex %0, %1, [%2]" : "=&r"(fail) : "r"(1U), "r"(&lock->locked) : "memory");
    } while (fail != 0U);

    OS_ARCH_DSB();
#else
    os_arch_spinlock_acquire_cb(lock);
#endif
}

/******************************************************************************************************/
/**
 * @brief Release an inter-core spinlock. Compiles to nothing on single-core builds.
 */
static inline void os_arch_spinlock_release(os_arch_spinlock_t *lock)
{
#if (OS_CONFIG_CORE_COUNT == 1U)
    (void)lock;
#elif (OS_ARCH_HAS_EXCLUSIVES == 1)
    OS_ARCH_DSB();
    lock->locked = 0U;
    OS_ARCH_DSB();
#else
    os_arch_spinlock_release_cb(lock);
#endif
}

/*
 * ***********************************************************************************************************
 * TrustZone callbacks (OS_CONFIG_TRUSTZONE_NON_SECURE only)
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
/******************************************************************************************************/
/**
 * @brief Application callback: bank the secure-side context of the task being switched out.
 *        Called from the context-switch handler; task_id 0 means the idle task (no secure
 *        context). Weak default does nothing.
 */
void os_arch_tz_context_save_cb(uint32_t task_id);

/******************************************************************************************************/
/**
 * @brief Application callback: restore the secure-side context of the task being switched in.
 *        Weak default does nothing.
 */
void os_arch_tz_context_restore_cb(uint32_t task_id);
#endif /* OS_CONFIG_TRUSTZONE_NON_SECURE */

/*
 * ***********************************************************************************************************
 * Platform callbacks
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Platform callback: return the CPU clock in Hz (0 = unknown). The weak default (kernel.c)
 *        returns OS_CONFIG_CPU_CLOCK_HZ when configured, else the CMSIS SystemCoreClock global when
 *        the platform provides one; platforms with another clock convention override it.
 */
uint32_t os_clock_hz_get_cb(void);

/*
 * ***********************************************************************************************************
 * Public function prototypes
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize architecture-specific low-level resources.
 */
void os_arch_init(void);

/******************************************************************************************************/
/**
 * @brief Start the first task context. Does not return.
 */
void os_arch_start_first_task(void);

/******************************************************************************************************/
/**
 * @brief Initialize the architecture tick source.
 */
void os_arch_tick_init(void);

/******************************************************************************************************/
/**
 * @brief Build the initial stack frame for a newly created task.
 */
uint32_t* os_arch_task_stack_initialize(uint8_t *stack_base, size_t stack_bytes, void (*entry)(void *context), void *context);

/******************************************************************************************************/
/**
 * @brief Read the free-running core cycle counter (DWT, or SysTick-derived when absent).
 */
uint32_t os_arch_cycle_count_get(void);

/******************************************************************************************************/
/**
 * @brief Return elapsed ticks while in low-power mode.
 */
uint32_t os_arch_elapsed_ticks_get(void);

/******************************************************************************************************/
/**
 * @brief Record low-power entry context for elapsed tick accounting.
 */
void os_arch_sleep_prepare(uint32_t planned_ticks);

#ifdef __cplusplus
}
#endif

#endif /* OS_ARCH_PORT_COMMON_H */
