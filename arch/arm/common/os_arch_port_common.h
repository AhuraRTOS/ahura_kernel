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

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#define OS_ARCH_REG_ICSR                  (*(volatile uint32_t *)0xE000ED04UL)
#define OS_ARCH_ICSR_PENDSVSET_MSK        (1UL << 28)
#define OS_ARCH_STACK_UNIT_BITS           32U
#define OS_ARCH_STACK_ALIGNMENT_BYTES     4U
#define OS_ARCH_DSB()                     __asm volatile("dsb 0xF" ::: "memory")
#define OS_ARCH_ISB()                     __asm volatile("isb 0xF" ::: "memory")

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
uint32_t *os_arch_task_stack_initialize(uint8_t *stack_base, size_t stack_bytes, void (*entry)(void *context), void *context);

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
