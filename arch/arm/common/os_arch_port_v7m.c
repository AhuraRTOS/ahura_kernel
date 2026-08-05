/**
 * @file os_arch_port_v7m.c
 * @brief Shared port implementation for ARMv7-M (Cortex-M3) and ARMv7E-M
 *        (Cortex-M4, M7) cores. Thumb-2, FPU support is compile-time
 *        conditional (saves s16-s31 and a per-task EXC_RETURN when built
 *        with a hard/softfp float ABI).
 *
 * This file is textually included by each variant's os_arch_port.c wrapper.
 * ARMv8-M mainline / ARMv8.1-M cores use os_arch_port_v8m.c, which extends
 * this implementation with PSPLIM/MSPLIM stack limits and TrustZone.
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

#include "os_arch_port_common.h"

#if !defined(__ARM_ARCH_7M__) && !defined(__ARM_ARCH_7EM__)
#error "os_arch_port_v7m.c targets ARMv7-M / ARMv7E-M cores (check -mcpu / -march)."
#endif

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#define OS_ARCH_REG_SHPR2                    (*(__IO uint32_t *)0xE000ED1CUL)
#define OS_ARCH_REG_SHPR3                    (*(__IO uint32_t *)0xE000ED20UL)
#define OS_ARCH_REG_AIRCR                    (*(__IO uint32_t *)0xE000ED0CUL)
#define OS_ARCH_REG_DEMCR                    (*(__IO uint32_t *)0xE000EDFCUL)
#define OS_ARCH_REG_DWT_CTRL                 (*(__IO uint32_t *)0xE0001000UL)
#define OS_ARCH_REG_DWT_CYCCNT               (*(__IO uint32_t *)0xE0001004UL)
#define OS_ARCH_REG_DWT_LAR                  (*(__IO uint32_t *)0xE0001FB0UL)
#define OS_ARCH_REG_SYST_CSR                 (*(__IO uint32_t *)0xE000E010UL)
#define OS_ARCH_REG_SYST_RVR                 (*(__IO uint32_t *)0xE000E014UL)
#define OS_ARCH_REG_SYST_CVR                 (*(__IO uint32_t *)0xE000E018UL)

#define OS_ARCH_DEMCR_TRCENA_MSK             (1UL << 24)
#define OS_ARCH_DWT_CTRL_CYCCNTENA_MSK       (1UL << 0)
#define OS_ARCH_DWT_LAR_UNLOCK_KEY           0xC5ACCE55UL

#define OS_ARCH_SYST_CSR_ENABLE_MSK          (1UL << 0)
#define OS_ARCH_SYST_CSR_TICKINT_MSK         (1UL << 1)
#define OS_ARCH_SYST_CSR_CLKSOURCE_MSK       (1UL << 2)
#define OS_ARCH_SYST_RVR_RELOAD_MSK          0x00FFFFFFUL

#define OS_ARCH_SHPR2_SVC_PRI_POS            24U
#define OS_ARCH_SHPR3_PENDSV_PRI_POS         16U
#define OS_ARCH_SHPR3_SYSTICK_PRI_POS        24U

#define OS_ARCH_PRIORITY_HIGHEST             0U
#define OS_ARCH_PRIORITY_LOWEST              255U
#define OS_ARCH_XPSR_THUMB                   (1UL << 24)

/*
 * EXC_RETURN for the initial task frame: return to thread mode, use PSP,
 * basic (non-FPU) stack frame. Stored as part of the software-saved context
 * so each task carries its own frame type across switches.
 */
#define OS_ARCH_EXC_RETURN_THREAD_PSP        0xFFFFFFFDUL

#define OS_ARCH_CONTROL_FPCA_MSK             (1UL << 2)

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static uint32_t os_arch_sleep_entry_cycles = 0U;
static uint32_t os_arch_planned_idle_ticks = 0U;

/*
 * ***********************************************************************************************************
 * Context switch handlers (SVC starts the first task, PendSV switches tasks)
 * ***********************************************************************************************************
 *
 * Software-saved frame layout on a task stack (low address first):
 *   [ s16-s31 ]  only when the task was using the FPU (EXC_RETURN bit 4 clear)
 *   r4-r11, EXC_RETURN
 *   [ hardware frame: r0-r3, r12, lr, pc, xpsr, (s0-s15, fpscr) ]
 *
 * Storing EXC_RETURN with the context lets each task keep its own frame type,
 * which is mandatory with -mfloat-abi=hard where any task or the startup code
 * may touch the FPU. os_task_stack_select_next() never returns NULL (the idle
 * task always exists), so the restore path needs no fallback.
*/

__asm(
".syntax unified\n"
".thumb\n"
".text\n"
".align 2\n"

".global SVC_Handler\n"
".type   SVC_Handler, %function\n"
".thumb_func\n"
"SVC_Handler:\n"
"    bl      os_task_stack_select_next\n"  /* r0 = first task stack pointer */
"    movw    r1, #0xED08\n"                /* reset MSP to the vector-table initial value; */
"    movt    r1, #0xE000\n"                /* the boot (main) context is abandoned here    */
"    ldr     r1, [r1]\n"
"    ldr     r1, [r1]\n"
"    msr     msp, r1\n"
"    b       os_arch_context_restore_asm\n"

".global PendSV_Handler\n"
".type   PendSV_Handler, %function\n"
".thumb_func\n"
"PendSV_Handler:\n"
"    mrs     r0, psp\n"
"    cbz     r0, 1f\n"                     /* no task context yet: nothing to switch */
#if defined(__ARM_FP)
"    tst     lr, #0x10\n"
"    it      eq\n"
"    vstmdbeq r0!, {s16-s31}\n"            /* task used the FPU: save callee-saved FP regs */
#endif
"    stmdb   r0!, {r4-r11, lr}\n"
"    bl      os_task_stack_save_current\n" /* r0 = stack pointer of outgoing task */
"    bl      os_task_stack_select_next\n"  /* r0 = stack pointer of incoming task */
"    b       os_arch_context_restore_asm\n"
"1:\n"
"    bx      lr\n"

".global os_arch_context_restore_asm\n"
".type   os_arch_context_restore_asm, %function\n"
".thumb_func\n"
"os_arch_context_restore_asm:\n"           /* r0 = stack pointer of task to restore */
"    clrex\n"                              /* drop any LDREX reservation the outgoing task left */
"    ldmia   r0!, {r4-r11, lr}\n"
#if defined(__ARM_FP)
"    tst     lr, #0x10\n"
"    it      eq\n"
"    vldmiaeq r0!, {s16-s31}\n"
#endif
"    msr     psp, r0\n"
"    dsb\n"
"    isb\n"
"    bx      lr\n"
);

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

extern void     os_task_exit(void);

static void     os_arch_task_exit_trap(void);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize architecture-specific low-level resources.
 *
 * @return None.
 */
void os_arch_init(void)
{
    uint32_t shpr2 = OS_ARCH_REG_SHPR2;
    uint32_t shpr3 = OS_ARCH_REG_SHPR3;

    /* PSP == 0 is the sentinel PendSV_Handler uses to recognize "no task
     * context yet" (see PendSV_Handler / os_arch_start_first_task below).
     * Primed here - the very first arch call from os_init(), before
     * os_tick_init() ever enables SysTick - rather than only right before
     * the bootstrap SVC in os_arch_start_first_task: os_kernel_running is
     * set true in os_start() a few instructions before that function
     * re-primes PSP, and interrupts stay enabled the whole time (the kernel
     * never masks them at boot), so a tick landing in that gap would pend a
     * PendSV that reads PSP's architecturally-unpredictable power-on-reset
     * value instead of the sentinel - PSP is not the active stack pointer
     * yet (Thread mode still runs on MSP), so priming it this early has no
     * other effect and closes the window unconditionally. */
    __asm volatile("msr psp, %0" :: "r"(0U));
    OS_ARCH_ISB();

    /* SVC highest so os_start always reaches it; PendSV/SysTick lowest so
     * context switches never preempt application interrupts. */
    shpr2 &= ~(0xFFUL << OS_ARCH_SHPR2_SVC_PRI_POS);
    shpr2 |= ((uint32_t)OS_ARCH_PRIORITY_HIGHEST << OS_ARCH_SHPR2_SVC_PRI_POS);

    shpr3 &= ~((0xFFUL << OS_ARCH_SHPR3_PENDSV_PRI_POS) | (0xFFUL << OS_ARCH_SHPR3_SYSTICK_PRI_POS));
    shpr3 |= ((uint32_t)OS_ARCH_PRIORITY_LOWEST << OS_ARCH_SHPR3_PENDSV_PRI_POS);
    shpr3 |= ((uint32_t)OS_ARCH_PRIORITY_LOWEST << OS_ARCH_SHPR3_SYSTICK_PRI_POS);

    OS_ARCH_REG_SHPR2 = shpr2;
    OS_ARCH_REG_SHPR3 = shpr3;

#if (OS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY != 0U)
    /* The raw-byte comparisons in os_arch_isr_priority_check are only exact
     * when (1) the configured threshold lives entirely in this device's
     * implemented priority bits (write-back must return it unchanged; a
     * truncated value would mask at a different level than the check tests)
     * and (2) the priority grouping dedicates every implemented bit to
     * preemption - no subpriority bits (BASEPRI masks by GROUP priority, so
     * subpriority bits would let the byte compare disagree with the
     * hardware's masking decision). Violations park here at boot instead of
     * running with checks that silently differ from the mask. */
    {
        uint32_t readback;
        uint32_t implemented;
        uint32_t prigroup;

        __asm volatile("msr basepri, %0" :: "r"((uint32_t)OS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY) : "memory");
        __asm volatile("mrs %0, basepri" : "=r"(readback));
        __asm volatile("msr basepri, %0" :: "r"(0xFFU) : "memory");
        __asm volatile("mrs %0, basepri" : "=r"(implemented));
        __asm volatile("msr basepri, %0" :: "r"(0U) : "memory");

        prigroup = (OS_ARCH_REG_AIRCR >> 8) & 0x7U;

        if ((readback != (uint32_t)OS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY) ||
            ((implemented & ((1UL << (prigroup + 1U)) - 1U)) != 0U))
        {
            os_arch_config_fault_trap();
        }
    }
#endif

    /* Start the cycle counter used for precise busy-wait delays and tickless
     * accounting. The LAR write unlocks DWT on cores implementing the
     * CoreSight software lock (Cortex-M7); it is ignored elsewhere. */
    OS_ARCH_REG_DEMCR      |= OS_ARCH_DEMCR_TRCENA_MSK;
    OS_ARCH_REG_DWT_LAR    = OS_ARCH_DWT_LAR_UNLOCK_KEY;
    OS_ARCH_REG_DWT_CYCCNT = 0U;
    OS_ARCH_REG_DWT_CTRL   |= OS_ARCH_DWT_CTRL_CYCCNTENA_MSK;

    os_arch_sleep_entry_cycles = OS_ARCH_REG_DWT_CYCCNT;
    os_arch_planned_idle_ticks = 0U;
}

/******************************************************************************************************/
/**
 * @brief Start the first task context. Does not return.
 *
 * @return None.
 */
void os_arch_start_first_task(void)
{
#if defined(__ARM_FP)
    uint32_t control;

    /* Startup/HAL code (hard-float ABI) may have used the FPU: clear FPCA so
     * the bootstrap SVC stacks a basic frame and leaves no lazy FP state
     * pointing at the abandoned main stack. */
    __asm volatile("mrs %0, control" : "=r"(control));
    control &= ~OS_ARCH_CONTROL_FPCA_MSK;
    __asm volatile("msr control, %0" :: "r"(control));
    OS_ARCH_ISB();
#endif

    /* PSP == 0 tells PendSV there is no task context to save yet. */
    __asm volatile("msr psp, %0" :: "r"(0U));
    OS_ARCH_ISB();

    OS_ARCH_IRQ_ENABLE();
    __asm volatile("svc 0");

    /* Never reached: the SVC handler switches to the first task. */
    while (1)
    {
        OS_ARCH_IDLE();
    }
}

/******************************************************************************************************/
/**
 * @brief Initialize SysTick as the kernel tick source.
 *
 * @return None.
 */
void os_arch_tick_init(void)
{
    uint32_t clock_hz = os_arch_clock_hz_get();
    uint32_t reload_value;

    if ((clock_hz == 0U) || (OS_CONFIG_TICK_HZ == 0U))
    {
        return;
    }

    reload_value = (clock_hz / OS_CONFIG_TICK_HZ);
    if ((reload_value == 0U) || (reload_value > (OS_ARCH_SYST_RVR_RELOAD_MSK + 1UL)))
    {
        return;
    }

    OS_ARCH_REG_SYST_CSR = 0U;
    OS_ARCH_REG_SYST_RVR = reload_value - 1UL;
    OS_ARCH_REG_SYST_CVR = 0U;
    OS_ARCH_REG_SYST_CSR = OS_ARCH_SYST_CSR_CLKSOURCE_MSK |
                           OS_ARCH_SYST_CSR_TICKINT_MSK |
                           OS_ARCH_SYST_CSR_ENABLE_MSK;
}

/******************************************************************************************************/
/**
 * @brief Build the initial task stack frame for a newly created task.
 *
 * @param[in] stack_base   Base address of the caller-provided stack memory.
 * @param[in] stack_bytes  Size of the stack memory in bytes.
 * @param[in] entry        Task entry function.
 * @param[in] context      Task argument passed in R0.
 * @return uint32_t*       Initial process stack pointer for first restore, NULL on bad arguments.
 */
uint32_t* os_arch_task_stack_initialize(uint8_t *stack_base, size_t stack_bytes, void (*entry)(void *context), void *context)
{
    uint32_t *stack_top;

    if ((stack_base == NULL) || (entry == (void (*)(void *))0) || (stack_bytes < OS_CONFIG_MIN_STACK_SIZE))
    {
        return NULL;
    }

    /* The hardware exception frame must sit on an 8-byte aligned address. */
    stack_top = (uint32_t *)((uintptr_t)(stack_base + stack_bytes) & ~(uintptr_t)0x7U);

    /* Hardware frame restored by exception return. */
    *(--stack_top) = OS_ARCH_XPSR_THUMB;                    /* xPSR */
    *(--stack_top) = (uint32_t)(uintptr_t)entry;            /* PC   */
    *(--stack_top) = (uint32_t)(uintptr_t)os_arch_task_exit_trap; /* LR */
    *(--stack_top) = 0U;                                    /* R12  */
    *(--stack_top) = 0U;                                    /* R3   */
    *(--stack_top) = 0U;                                    /* R2   */
    *(--stack_top) = 0U;                                    /* R1   */
    *(--stack_top) = (uint32_t)(uintptr_t)context;          /* R0   */

    /* Software frame restored by the context-switch code. */
    *(--stack_top) = OS_ARCH_EXC_RETURN_THREAD_PSP;         /* EXC_RETURN */
    *(--stack_top) = 0U;                                    /* R11  */
    *(--stack_top) = 0U;                                    /* R10  */
    *(--stack_top) = 0U;                                    /* R9   */
    *(--stack_top) = 0U;                                    /* R8   */
    *(--stack_top) = 0U;                                    /* R7   */
    *(--stack_top) = 0U;                                    /* R6   */
    *(--stack_top) = 0U;                                    /* R5   */
    *(--stack_top) = 0U;                                    /* R4   */

    return stack_top;
}

/******************************************************************************************************/
/**
 * @brief Read the free-running core cycle counter (DWT CYCCNT).
 *
 * @return uint32_t  Current cycle count.
 */
uint32_t os_arch_cycle_count_get(void)
{
    return OS_ARCH_REG_DWT_CYCCNT;
}

/******************************************************************************************************/
/**
 * @brief Return elapsed ticks while in low-power mode.
 *
 * @return uint32_t  Elapsed ticks since sleep entry.
 */
uint32_t os_arch_elapsed_ticks_get(void)
{
    /* Always 0 on this port, and deliberately so.
     *
     * os_arch_max_suppressed_ticks_get() returns 0 here: ticking is never suppressed, so the WFI
     * runs with SysTick still counting and its interrupt still enabled, and the interrupt that
     * ends the sleep is normally the very next tick. Every tick that passed was therefore already
     * counted, one at a time, by os_tick_handler().
     *
     * Returning a measured duration as well would have os_tick.c announce time the ISR had just
     * accounted for, advancing os_tick_count at roughly twice real time for the whole idle period
     * - delays and timers would then fire early in proportion to how long the system sat idle. A
     * suppression-capable port (v8m) returns a real figure precisely because there the ISR did not
     * run. Measuring the sleep only becomes meaningful here once this port learns to reprogram
     * SysTick, at which point os_arch_sleep_prepare's recorded entry cycle count is what it needs.
     */
    os_arch_planned_idle_ticks = 0U;

    return 0U;
}

/******************************************************************************************************/
/**
 * @brief Close the tickless window. Nothing to release on this port.
 *
 * @return None.
 */
void os_arch_sleep_finish(void)
{
    /* os_arch_sleep_prepare takes no interrupt mask here: a plain WFI needs interrupts left
     * enabled in order to wake at all, so there is nothing to hand back. */
}

#if (OS_CONFIG_ATOMIC_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Atomics
 * ***********************************************************************************************************
 *
 * One LDREX/STREX retry loop per operation, each a single inline-assembly block: take a
 * reservation on the word, compute from what it held, store only if the reservation survived, and
 * go round again if it did not. Nothing is masked, so an ISR - or another core - landing in the
 * middle costs a second pass rather than costing anyone correctness.
 *
 * Written out per operation rather than funnelled through a shared helper or a CAS loop. Speed:
 * the whole sequence is five instructions, and CAS would re-load and compare what the reservation
 * already tells us. Safety: one asm block per loop is the same five instructions at -O0 as at -O2,
 * with no compiler spill between the LDREX and the STREX - ARM allows only one reservation per
 * core and leaves it implementation-defined whether other accesses clear it.
 *
 * "1:" and "1b" are local numeric labels, so each block stays correct even if the compiler emits
 * it more than once. Every operation returns the value the word held BEFORE it ran.
 *
 * ARMv6-M has no exclusives and does all of this under a critical section instead - see
 * os_arch_port_v6m.c.
*/

/******************************************************************************************************/
/**
 * @brief Atomic store. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Value to store.
 * @return int32_t  Value held before the store.
 */
int32_t os_arch_atomic_exchange(__IO int32_t *target, int32_t value)
{
    int32_t  current;
    uint32_t store_failed;

    __asm volatile(
        "1:  ldrex   %0, [%2]      \n"
        "    strex   %1, %3, [%2]  \n"
        "    cmp     %1, #0        \n"
        "    bne     1b            \n"
        : "=&r"(current), "=&r"(store_failed)
        : "r"(target), "r"(value)
        : "cc", "memory");

    return current;
}

/******************************************************************************************************/
/**
 * @brief Atomic add. See os_arch_port_common.h.
 *
 * ADD and SUB wrap rather than overflow: the assembler works on raw 32-bit registers, so the
 * signed-overflow undefined behaviour that the equivalent C expression would carry never arises.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Amount to add.
 * @return int32_t  Value held before the addition.
 */
int32_t os_arch_atomic_add(__IO int32_t *target, int32_t value)
{
    int32_t  current;
    int32_t  updated;
    uint32_t store_failed;

    __asm volatile(
        "1:  ldrex   %0, [%3]      \n"
        "    add     %1, %0, %4    \n"
        "    strex   %2, %1, [%3]  \n"
        "    cmp     %2, #0        \n"
        "    bne     1b            \n"
        : "=&r"(current), "=&r"(updated), "=&r"(store_failed)
        : "r"(target), "r"(value)
        : "cc", "memory");

    return current;
}

/******************************************************************************************************/
/**
 * @brief Atomic subtract. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Amount to subtract.
 * @return int32_t  Value held before the subtraction.
 */
int32_t os_arch_atomic_sub(__IO int32_t *target, int32_t value)
{
    int32_t  current;
    int32_t  updated;
    uint32_t store_failed;

    __asm volatile(
        "1:  ldrex   %0, [%3]      \n"
        "    sub     %1, %0, %4    \n"
        "    strex   %2, %1, [%3]  \n"
        "    cmp     %2, #0        \n"
        "    bne     1b            \n"
        : "=&r"(current), "=&r"(updated), "=&r"(store_failed)
        : "r"(target), "r"(value)
        : "cc", "memory");

    return current;
}

/******************************************************************************************************/
/**
 * @brief Atomic bitwise OR. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Bits to set.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_or(__IO int32_t *target, int32_t value)
{
    int32_t  current;
    int32_t  updated;
    uint32_t store_failed;

    __asm volatile(
        "1:  ldrex   %0, [%3]      \n"
        "    orr     %1, %0, %4    \n"
        "    strex   %2, %1, [%3]  \n"
        "    cmp     %2, #0        \n"
        "    bne     1b            \n"
        : "=&r"(current), "=&r"(updated), "=&r"(store_failed)
        : "r"(target), "r"(value)
        : "cc", "memory");

    return current;
}

/******************************************************************************************************/
/**
 * @brief Atomic bitwise AND. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Mask to keep.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_and(__IO int32_t *target, int32_t value)
{
    int32_t  current;
    int32_t  updated;
    uint32_t store_failed;

    __asm volatile(
        "1:  ldrex   %0, [%3]      \n"
        "    and     %1, %0, %4    \n"
        "    strex   %2, %1, [%3]  \n"
        "    cmp     %2, #0        \n"
        "    bne     1b            \n"
        : "=&r"(current), "=&r"(updated), "=&r"(store_failed)
        : "r"(target), "r"(value)
        : "cc", "memory");

    return current;
}

/******************************************************************************************************/
/**
 * @brief Atomic bitwise XOR. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Bits to flip.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_xor(__IO int32_t *target, int32_t value)
{
    int32_t  current;
    int32_t  updated;
    uint32_t store_failed;

    __asm volatile(
        "1:  ldrex   %0, [%3]      \n"
        "    eor     %1, %0, %4    \n"
        "    strex   %2, %1, [%3]  \n"
        "    cmp     %2, #0        \n"
        "    bne     1b            \n"
        : "=&r"(current), "=&r"(updated), "=&r"(store_failed)
        : "r"(target), "r"(value)
        : "cc", "memory");

    return current;
}

/******************************************************************************************************/
/**
 * @brief Atomic bitwise NAND. See os_arch_port_common.h.
 *
 * The only operation needing two instructions inside the window: ARM has no single NAND, so the
 * AND result is inverted in place before the store.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Operand.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_nand(__IO int32_t *target, int32_t value)
{
    int32_t  current;
    int32_t  updated;
    uint32_t store_failed;

    __asm volatile(
        "1:  ldrex   %0, [%3]      \n"
        "    and     %1, %0, %4    \n"
        "    mvn     %1, %1        \n"
        "    strex   %2, %1, [%3]  \n"
        "    cmp     %2, #0        \n"
        "    bne     1b            \n"
        : "=&r"(current), "=&r"(updated), "=&r"(store_failed)
        : "r"(target), "r"(value)
        : "cc", "memory");

    return current;
}

/******************************************************************************************************/
/**
 * @brief Atomic compare-and-swap. See os_arch_port_common.h.
 *
 * The one operation that does not retry, so it is the one that needs CLREX: on a mismatch the
 * reservation is dropped rather than left set on a word this call is walking away from, since a
 * stale one can make an unrelated later STREX succeed when it should not.
 *
 * store_failed is seeded with 1 so the mismatch path falls out reporting failure without the
 * STREX having run.
 *
 * @param[in,out] target    Word to update.
 * @param[in]     expected  Value the caller believes target holds.
 * @param[in]     desired   Value to store if it still does.
 * @return bool  true if desired was stored.
 */
bool os_arch_atomic_cas(__IO int32_t *target, int32_t expected, int32_t desired)
{
    int32_t  current;
    uint32_t store_failed;

    __asm volatile(
        "    mov     %1, #1        \n"
        "    ldrex   %0, [%2]      \n"
        "    cmp     %0, %3        \n"
        "    bne     1f            \n"
        "    strex   %1, %4, [%2]  \n"
        "    b       2f            \n"
        "1:  clrex                 \n"
        "2:                        \n"
        : "=&r"(current), "=&r"(store_failed)
        : "r"(target), "r"(expected), "r"(desired)
        : "cc", "memory");

    /* A failed STREX on the matching path is a spurious failure, not a mismatch: the word did hold
     * expected, the reservation was simply lost. The portable contract allows that, which is why
     * callers needing certainty re-read instead of reading false as "someone else won". */
    return (store_failed == 0U);
}

#endif /* OS_CONFIG_ATOMIC_ENABLE */

/******************************************************************************************************/
/**
 * @brief Record low-power entry context for elapsed tick accounting.
 *
 * @param[in] planned_ticks  Planned idle duration in kernel ticks.
 * @return None.
 */
void os_arch_sleep_prepare(uint32_t planned_ticks)
{
    os_arch_planned_idle_ticks = planned_ticks;
    os_arch_sleep_entry_cycles = os_arch_cycle_count_get();
}

/******************************************************************************************************/
/**
 * @brief Maximum ticks this port can suppress in a single tickless window (see
 *        os_arch_port_common.h for the full contract).
 *
 * This port does not yet reprogram SysTick's reload for real suppression (see os_arch_port_v8m.c,
 * which does) - os_arch_sleep_prepare/os_arch_elapsed_ticks_get above still measure via the DWT
 * cycle counter around a plain WFI, so there is no register-width-limited window to report here.
 * 0 tells callers (os_tick.c, tests) not to expect a real suppressed sleep on this port yet.
 *
 * @return uint32_t  Always 0 until this port gets the same fix as os_arch_port_v8m.c.
 */
uint32_t os_arch_max_suppressed_ticks_get(void)
{
    return 0U;
}

/*
 * ***********************************************************************************************************
 * Multi-core weak callback defaults
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_CORE_COUNT > 1U)
/* os_arch_core_id_get_cb() and os_arch_core_ipi_request_cb() are deliberately NOT defined here,
 * not even as weak stubs.
 *
 * Cortex-M has no architectural core id and no architectural IPI, so both are SoC-specific (SIO
 * CPUID and the inter-core FIFO on an RP2040, for instance). Defaults would be worse than absent:
 * an id fixed at 0 makes every core believe it is core 0 and share one current-task slot, and a
 * do-nothing IPI leaves cross-core wakeups waiting for the next tick. Both look like a working
 * build that schedules wrongly.
 *
 * Define them in the application's os_cb.c (see os_cb_template.c) whenever OS_CONFIG_CORE_COUNT
 * is above 1. */
#endif /* OS_CONFIG_CORE_COUNT > 1U */

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Landing point when a task entry function returns; deletes the task.
 *
 * @return None.
 */
static void os_arch_task_exit_trap(void)
{
    os_task_exit();

    /* os_task_exit never returns; trap just in case. */
    while (1)
    {
        __asm volatile("bkpt #0");
    }
}
