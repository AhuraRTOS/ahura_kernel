/**
 * @file os_arch_port_v7m.c
 * @brief Shared port implementation for ARMv7-M (Cortex-M3, M4, M7),
 *        ARMv8-M mainline (Cortex-M33, M35P) and ARMv8.1-M (Cortex-M52, M55,
 *        M85) cores. FPU support is compile-time conditional; on ARMv8-M
 *        mainline the port adds per-task PSPLIM and, when the linker script
 *        provides __StackLimit, an MSPLIM guard for the handler stack.
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
#define OS_ARCH_REG_DEMCR                    (*(volatile uint32_t *)0xE000EDFCUL)
#define OS_ARCH_REG_DWT_CTRL                 (*(volatile uint32_t *)0xE0001000UL)
#define OS_ARCH_REG_DWT_CYCCNT               (*(volatile uint32_t *)0xE0001004UL)
#define OS_ARCH_REG_DWT_LAR                  (*(volatile uint32_t *)0xE0001FB0UL)
#define OS_ARCH_REG_SYST_CSR                 (*(volatile uint32_t *)0xE000E010UL)
#define OS_ARCH_REG_SYST_RVR                 (*(volatile uint32_t *)0xE000E014UL)
#define OS_ARCH_REG_SYST_CVR                 (*(volatile uint32_t *)0xE000E018UL)

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
 * ARMv8-M mainline (Cortex-M33/M55/M85) adds per-task hardware stack overflow
 * detection: PSPLIM is saved/restored with the context and a push below the
 * limit raises a UsageFault instead of silently corrupting memory.
 */
#if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__)
#define OS_ARCH_HAS_STACK_LIMIT              1
#else
#define OS_ARCH_HAS_STACK_LIMIT              0
#endif

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
 *   [ PSPLIM ]   only on ARMv8-M mainline (per-task stack limit)
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
#if (OS_ARCH_HAS_STACK_LIMIT == 1)
"    mrs     r2, psplim\n"                 /* save the per-task stack limit */
"    stmdb   r0!, {r2, r4-r11, lr}\n"
#else
"    stmdb   r0!, {r4-r11, lr}\n"
#endif
"    bl      os_task_stack_save_current\n" /* r0 = stack pointer of outgoing task */
"    bl      os_task_stack_select_next\n"  /* r0 = stack pointer of incoming task */
"    b       os_arch_context_restore_asm\n"
"1:\n"
"    bx      lr\n"

".global os_arch_context_restore_asm\n"
".type   os_arch_context_restore_asm, %function\n"
".thumb_func\n"
"os_arch_context_restore_asm:\n"           /* r0 = stack pointer of task to restore */
#if (OS_ARCH_HAS_STACK_LIMIT == 1)
"    ldmia   r0!, {r2, r4-r11, lr}\n"
"    msr     psplim, r2\n"                 /* restore the stack limit before PSP */
#else
"    ldmia   r0!, {r4-r11, lr}\n"
#endif
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

extern uint32_t SystemCoreClock;
extern void     os_task_exit(void);

#if (OS_ARCH_HAS_STACK_LIMIT == 1)
/* Bottom of the main (handler) stack. CMSIS linker scripts define it; the
 * weak reference resolves to address 0 when a custom script omits it. */
extern uint32_t __StackLimit __attribute__((weak));
#endif

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
     * context switches never preempt application interrupts. */
    shpr2 &= ~(0xFFUL << OS_ARCH_SHPR2_SVC_PRI_POS);
    shpr2 |= ((uint32_t)OS_ARCH_PRIORITY_HIGHEST << OS_ARCH_SHPR2_SVC_PRI_POS);

    shpr3 &= ~((0xFFUL << OS_ARCH_SHPR3_PENDSV_PRI_POS) | (0xFFUL << OS_ARCH_SHPR3_SYSTICK_PRI_POS));
    shpr3 |= ((uint32_t)OS_ARCH_PRIORITY_LOWEST << OS_ARCH_SHPR3_PENDSV_PRI_POS);
    shpr3 |= ((uint32_t)OS_ARCH_PRIORITY_LOWEST << OS_ARCH_SHPR3_SYSTICK_PRI_POS);

    OS_ARCH_REG_SHPR2 = shpr2;
    OS_ARCH_REG_SHPR3 = shpr3;

    /* Start the cycle counter used for precise busy-wait delays and tickless
     * accounting. The LAR write unlocks DWT on cores implementing the
     * CoreSight software lock (Cortex-M7); it is ignored elsewhere. */
    OS_ARCH_REG_DEMCR      |= OS_ARCH_DEMCR_TRCENA_MSK;
    OS_ARCH_REG_DWT_LAR     = OS_ARCH_DWT_LAR_UNLOCK_KEY;
    OS_ARCH_REG_DWT_CYCCNT  = 0U;
    OS_ARCH_REG_DWT_CTRL   |= OS_ARCH_DWT_CTRL_CYCCNTENA_MSK;

    os_arch_sleep_entry_cycles = OS_ARCH_REG_DWT_CYCCNT;
    os_arch_planned_idle_ticks = 0U;

#if (OS_ARCH_HAS_STACK_LIMIT == 1)
    /* Guard the handler stack: an MSP push below __StackLimit raises a
     * UsageFault instead of corrupting whatever sits below the stack.
     * MSPLIM ignores its low 3 bits, so round the limit up. Skipped (MSPLIM
     * keeps its reset value 0 = no checking) when the symbol is absent. */
    if (&__StackLimit != (uint32_t *)0)
    {
        uint32_t msp_limit = ((uint32_t)(uintptr_t)&__StackLimit + 7U) & ~(uint32_t)0x7U;

        __asm volatile("msr msplim, %0" :: "r"(msp_limit));
    }
#endif
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

#if (OS_ARCH_HAS_STACK_LIMIT == 1)
    /* PSPLIM ignores its low 3 bits, so round the limit up: an overflow then
     * always faults before writing outside the caller's stack memory. */
    *(--stack_top) = ((uint32_t)(uintptr_t)stack_base + 7U) & ~(uint32_t)0x7U; /* PSPLIM */
#endif

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
