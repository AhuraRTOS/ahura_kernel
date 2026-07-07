/**
 * @file os_arch_port_v6m.c
 * @brief Shared port implementation for ARMv6-M (Cortex-M0, M0+) and
 *        ARMv8-M baseline (Cortex-M23) cores. Thumb-1 subset only, no FPU,
 *        no DWT cycle counter (synthesized from SysTick). Non-secure ARMv8-M
 *        baseline has no stack-limit registers, so there is no PSPLIM
 *        handling here.
 *
 * This file is textually included by each variant's os_arch_port.c wrapper.
 *
 * TrustZone: not yet supported. Build with the Security Extension disabled or
 * run the kernel entirely in one security state; per-task secure contexts are
 * future work and belong in a dedicated v8-M TZ port, not in this file.
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
#include "../../../ahura_config.h"

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#define OS_ARCH_REG_SHPR2                    (*(volatile uint32_t *)0xE000ED1CUL)
#define OS_ARCH_REG_SHPR3                    (*(volatile uint32_t *)0xE000ED20UL)
#define OS_ARCH_REG_SYST_CSR                 (*(volatile uint32_t *)0xE000E010UL)
#define OS_ARCH_REG_SYST_RVR                 (*(volatile uint32_t *)0xE000E014UL)
#define OS_ARCH_REG_SYST_CVR                 (*(volatile uint32_t *)0xE000E018UL)

#define OS_ARCH_SYST_CSR_ENABLE_MSK          (1UL << 0)
#define OS_ARCH_SYST_CSR_TICKINT_MSK         (1UL << 1)
#define OS_ARCH_SYST_CSR_CLKSOURCE_MSK       (1UL << 2)
#define OS_ARCH_SYST_CSR_COUNTFLAG_MSK       (1UL << 16)
#define OS_ARCH_SYST_RVR_RELOAD_MSK          0x00FFFFFFUL

#define OS_ARCH_SHPR2_SVC_PRI_POS            24U
#define OS_ARCH_SHPR3_PENDSV_PRI_POS         16U
#define OS_ARCH_SHPR3_SYSTICK_PRI_POS        24U

#define OS_ARCH_PRIORITY_HIGHEST             0U
#define OS_ARCH_PRIORITY_LOWEST              255U
#define OS_ARCH_XPSR_THUMB                   (1UL << 24)

/*
 * EXC_RETURN for the initial task frame: return to thread mode, use PSP.
 * ARMv6-M has no FPU, so no frame-type handling is needed, but storing
 * EXC_RETURN keeps the frame layout identical to the mainline port.
 */
#define OS_ARCH_EXC_RETURN_THREAD_PSP        0xFFFFFFFDUL

/* Cycles credited per os_arch_cycle_count_get call when SysTick is not yet
 * running. Deliberately below the real call cost so busy-waits only ever get
 * longer, never shorter. */
#define OS_ARCH_CYCLE_FALLBACK_STEP          8U

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static uint32_t os_arch_sleep_entry_cycles = 0U;
static uint32_t os_arch_planned_idle_ticks = 0U;
static uint32_t os_arch_cycle_accum        = 0U;
static uint32_t os_arch_cycle_fallback     = 0U;

/*
 * ***********************************************************************************************************
 * Context switch handlers (SVC starts the first task, PendSV switches tasks)
 * ***********************************************************************************************************
 *
 * Software-saved frame layout on a task stack (low address first):
 *   r4-r11, EXC_RETURN
 *   [ hardware frame: r0-r3, r12, lr, pc, xpsr ]
 *
 * Same layout as the mainline port, built with the Thumb-1 subset: high
 * registers are staged through r4-r7 because ARMv6-M LDM/STM only address
 * low registers, and there is no CBZ/IT/MOVW/MOVT.
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
"    ldr     r1, os_arch_vtor_addr\n"      /* reset MSP to the vector-table initial value; */
"    ldr     r1, [r1]\n"                   /* VTOR reads as zero on cores without it,      */
"    ldr     r1, [r1]\n"                   /* which is the fixed table address anyway      */
"    msr     msp, r1\n"
"    b       os_arch_context_restore_asm\n"
".align 2\n"
"os_arch_vtor_addr:\n"
"    .word   0xE000ED08\n"

".global PendSV_Handler\n"
".type   PendSV_Handler, %function\n"
".thumb_func\n"
"PendSV_Handler:\n"
"    mrs     r0, psp\n"
"    cmp     r0, #0\n"                     /* no task context yet: nothing to switch */
"    beq     os_arch_pendsv_exit\n"
"    subs    r0, r0, #36\n"                /* reserve r4-r11 + EXC_RETURN (9 words) */
"    stmia   r0!, {r4-r7}\n"               /* save r4-r7 */
"    mov     r4, r8\n"                     /* stage and save r8-r11 */
"    mov     r5, r9\n"
"    mov     r6, r10\n"
"    mov     r7, r11\n"
"    stmia   r0!, {r4-r7}\n"
"    mov     r4, lr\n"                     /* save EXC_RETURN */
"    stmia   r0!, {r4}\n"
"    subs    r0, r0, #36\n"                /* r0 = base of the software frame */
"    bl      os_task_stack_save_current\n" /* r0 = stack pointer of outgoing task */
"    bl      os_task_stack_select_next\n"  /* r0 = stack pointer of incoming task */
"    b       os_arch_context_restore_asm\n"
"os_arch_pendsv_exit:\n"
"    bx      lr\n"

".global os_arch_context_restore_asm\n"
".type   os_arch_context_restore_asm, %function\n"
".thumb_func\n"
"os_arch_context_restore_asm:\n"           /* r0 = stack pointer of task to restore */
"    mov     r1, r0\n"                     /* keep frame base for the r4-r7 reload */
"    adds    r0, r0, #16\n"
"    ldmia   r0!, {r4-r7}\n"               /* stage and restore r8-r11 */
"    mov     r8, r4\n"
"    mov     r9, r5\n"
"    mov     r10, r6\n"
"    mov     r11, r7\n"
"    ldmia   r0!, {r2}\n"                  /* restore EXC_RETURN */
"    mov     lr, r2\n"
"    mov     r2, r0\n"                     /* r2 = new PSP (frame base + 36) */
"    mov     r0, r1\n"
"    ldmia   r0!, {r4-r7}\n"               /* restore the task's real r4-r7 */
"    msr     psp, r2\n"
"    dsb\n"
"    isb\n"
"    bx      lr\n"
);

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

extern uint32_t SystemCoreClock;
extern void     os_task_exit(void);

static void os_arch_task_exit_trap(void);

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

    /* SVC highest so os_start always reaches it; PendSV/SysTick lowest so
     * context switches never preempt application interrupts. ARMv6-M
     * requires word access to SHPR registers, which this is. */
    shpr2 &= ~(0xFFUL << OS_ARCH_SHPR2_SVC_PRI_POS);
    shpr2 |= ((uint32_t)OS_ARCH_PRIORITY_HIGHEST << OS_ARCH_SHPR2_SVC_PRI_POS);

    shpr3 &= ~((0xFFUL << OS_ARCH_SHPR3_PENDSV_PRI_POS) | (0xFFUL << OS_ARCH_SHPR3_SYSTICK_PRI_POS));
    shpr3 |= ((uint32_t)OS_ARCH_PRIORITY_LOWEST << OS_ARCH_SHPR3_PENDSV_PRI_POS);
    shpr3 |= ((uint32_t)OS_ARCH_PRIORITY_LOWEST << OS_ARCH_SHPR3_SYSTICK_PRI_POS);

    OS_ARCH_REG_SHPR2 = shpr2;
    OS_ARCH_REG_SHPR3 = shpr3;

    os_arch_sleep_entry_cycles = 0U;
    os_arch_planned_idle_ticks = 0U;
    os_arch_cycle_accum        = 0U;
    os_arch_cycle_fallback     = 0U;
}

/******************************************************************************************************/
/**
 * @brief Start the first task context. Does not return.
 *
 * @return None.
 */
void os_arch_start_first_task(void)
{
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
    uint32_t reload_value;

    if ((SystemCoreClock == 0U) || (OS_CONFIG_TICK_HZ == 0U))
    {
        return;
    }

    reload_value = (SystemCoreClock / OS_CONFIG_TICK_HZ);
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
uint32_t *os_arch_task_stack_initialize(uint8_t *stack_base, size_t stack_bytes, void (*entry)(void *context), void *context)
{
    uint32_t *stack_top;

    if ((stack_base == (uint8_t *)0) || (entry == (void (*)(void *))0) || (stack_bytes < OS_CONFIG_MIN_STACK_SIZE))
    {
        return (uint32_t *)0;
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
 * @brief Read the free-running core cycle counter, synthesized from SysTick.
 *
 * ARMv6-M has no DWT cycle counter, so this derives one from the SysTick
 * down-counter plus a wrap accumulator. Wraps are only observed while this
 * function is being polled (busy-waits do so continuously), so it is meant
 * for measuring short intervals, not absolute time. Before SysTick runs, a
 * conservative software fallback advances the count so busy-waits still
 * terminate (slower than requested, never faster).
 *
 * @return uint32_t  Current cycle count.
 */
uint32_t os_arch_cycle_count_get(void)
{
    uint32_t primask = os_arch_primask_get();
    uint32_t reload;
    uint32_t csr;
    uint32_t value;

    OS_ARCH_IRQ_DISABLE();

    reload = OS_ARCH_REG_SYST_RVR & OS_ARCH_SYST_RVR_RELOAD_MSK;

    /* Single CSR read: it clears COUNTFLAG, so it must be sampled once. */
    csr = OS_ARCH_REG_SYST_CSR;

    if (((csr & OS_ARCH_SYST_CSR_ENABLE_MSK) == 0U) || (reload == 0U))
    {
        os_arch_cycle_fallback += OS_ARCH_CYCLE_FALLBACK_STEP;
        value = os_arch_cycle_accum + os_arch_cycle_fallback;
    }
    else
    {
        if ((csr & OS_ARCH_SYST_CSR_COUNTFLAG_MSK) != 0U)
        {
            os_arch_cycle_accum += (reload + 1U);
        }

        value = os_arch_cycle_accum + (reload - (OS_ARCH_REG_SYST_CVR & OS_ARCH_SYST_RVR_RELOAD_MSK));
    }

    if (primask == 0U)
    {
        OS_ARCH_IRQ_ENABLE();
    }

    return value;
}

/******************************************************************************************************/
/**
 * @brief Return elapsed ticks while in low-power mode.
 *
 * @return uint32_t  Elapsed ticks since sleep entry.
 */
uint32_t os_arch_elapsed_ticks_get(void)
{
    uint32_t now_cycles;
    uint32_t delta_cycles;
    uint32_t elapsed_ticks;

    if (os_arch_planned_idle_ticks == 0U)
    {
        return 0U;
    }

    now_cycles   = os_arch_cycle_count_get();
    delta_cycles = now_cycles - os_arch_sleep_entry_cycles;

    if (SystemCoreClock == 0U)
    {
        elapsed_ticks = os_arch_planned_idle_ticks;
    }
    else
    {
        uint64_t scaled_ticks = ((uint64_t)delta_cycles * (uint64_t)OS_CONFIG_TICK_HZ);

        scaled_ticks /= (uint64_t)SystemCoreClock;
        elapsed_ticks = (uint32_t)scaled_ticks;
    }

    if (elapsed_ticks > os_arch_planned_idle_ticks)
    {
        elapsed_ticks = os_arch_planned_idle_ticks;
    }

    os_arch_planned_idle_ticks = 0U;

    return elapsed_ticks;
}

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
