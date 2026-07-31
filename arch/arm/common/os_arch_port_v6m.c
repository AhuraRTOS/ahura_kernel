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
 * TrustZone (ARMv8-M baseline, Cortex-M23), selected with OS_CONFIG_TRUSTZONE:
 * disabled (default; the only choice on ARMv6-M cores), secure (whole kernel
 * compiled with -mcmse, tasks run secure) or non-secure (kernel runs
 * non-secure beside secure firmware; the context switch banks per-task secure
 * state through the weak os_arch_tz_context_save_cb /
 * os_arch_tz_context_restore_cb callbacks and the initial frames use the
 * non-secure EXC_RETURN).
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

#if !defined(__ARM_ARCH_6M__) && !defined(__ARM_ARCH_8M_BASE__)
#error "os_arch_port_v6m.c targets ARMv6-M / ARMv8-M baseline cores (check -mcpu / -march)."
#endif

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#define OS_ARCH_REG_SHPR2                    (*(__IO uint32_t *)0xE000ED1CUL)
#define OS_ARCH_REG_SHPR3                    (*(__IO uint32_t *)0xE000ED20UL)
#define OS_ARCH_REG_SYST_CSR                 (*(__IO uint32_t *)0xE000E010UL)
#define OS_ARCH_REG_SYST_RVR                 (*(__IO uint32_t *)0xE000E014UL)
#define OS_ARCH_REG_SYST_CVR                 (*(__IO uint32_t *)0xE000E018UL)

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
 * EXC_RETURN keeps the frame layout identical to the mainline port. A
 * non-secure TrustZone kernel (v8-M baseline) returns with the S and ES bits
 * clear (0xFFFFFFBC); the secure and TrustZone-less encodings are both
 * 0xFFFFFFFD.
 */
#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
#define OS_ARCH_EXC_RETURN_THREAD_PSP        0xFFFFFFBCUL
#else
#define OS_ARCH_EXC_RETURN_THREAD_PSP        0xFFFFFFFDUL
#endif

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
#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
"    mov     r4, r0\n"                     /* r4 survives the call (callee-saved) */
"    bl      os_arch_tz_context_restore\n" /* load the first task's secure context */
"    mov     r0, r4\n"
#endif
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
#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
"    bl      os_arch_tz_context_save\n"    /* bank the outgoing task's secure context */
#endif
"    bl      os_task_stack_select_next\n"  /* r0 = stack pointer of incoming task */
#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
"    mov     r4, r0\n"                     /* r4 was saved above; free to clobber here */
"    bl      os_arch_tz_context_restore\n" /* load the incoming task's secure context */
"    mov     r0, r4\n"
#endif
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

extern void     os_task_exit(void);
extern uint32_t os_task_current_id_get(void);

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
 * ARMv6-M has no LDREX/STREX, so there is no way to DETECT that something else touched the word
 * mid-update - it has to be PREVENTED. Each operation runs inside os_critical_enter/exit, which
 * masks this core's kernel interrupts and, on a multi-core build, locks out the other cores.
 *
 * Two consequences worth knowing before putting an atomic in a hot path here. It adds the length
 * of the update to interrupt latency, unlike the exclusives ports where interrupts stay enabled
 * throughout. And on a multi-core build it can wait on unrelated kernel work holding the same
 * lock - reusing the kernel's critical section rather than a second private lock is deliberate,
 * since two independent locks over the same data is how lock-ordering bugs start.
 *
 * No retry loop and no second read, unlike the exclusives ports: nothing can interfere while the
 * section is held, so the value read is still the value in memory when it is written back.
 *
 * ADD and SUB compute in the unsigned domain and convert back. Signed overflow is undefined
 * behaviour the moment a counter runs past INT32_MAX - not merely a negative-looking result, but
 * licence for the compiler to assume it cannot happen and optimise accordingly. Unsigned
 * arithmetic wraps with defined behaviour, and the conversion back reproduces the same
 * two's-complement bit pattern the hardware would have stored anyway.
 *
 * Every operation returns the value the word held BEFORE it ran.
 *
 * Note for Cortex-M23: ARMv8-M baseline reaches this file for its Thumb-1-compatible context
 * switch, but unlike ARMv6-M it DOES have LDREX/STREX, so it runs these under a critical section
 * when it could be lock-free. See os_arch_port_v7m.c for the exclusives form.
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
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = value;

    os_critical_exit();

    return current;
}

/******************************************************************************************************/
/**
 * @brief Atomic add. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Amount to add.
 * @return int32_t  Value held before the addition.
 */
int32_t os_arch_atomic_add(__IO int32_t *target, int32_t value)
{
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = (int32_t)((uint32_t)current + (uint32_t)value);

    os_critical_exit();

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
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = (int32_t)((uint32_t)current - (uint32_t)value);

    os_critical_exit();

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
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = current | value;

    os_critical_exit();

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
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = current & value;

    os_critical_exit();

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
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = current ^ value;

    os_critical_exit();

    return current;
}

/******************************************************************************************************/
/**
 * @brief Atomic bitwise NAND. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Operand.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_nand(__IO int32_t *target, int32_t value)
{
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = ~(current & value);

    os_critical_exit();

    return current;
}

/******************************************************************************************************/
/**
 * @brief Atomic compare-and-swap. See os_arch_port_common.h.
 *
 * Never fails spuriously on this port - nothing could have interfered - but callers still loop,
 * because the portable contract is written to the weaker guarantee the exclusives ports give.
 *
 * @param[in,out] target    Word to update.
 * @param[in]     expected  Value the caller believes target holds.
 * @param[in]     desired   Value to store if it still does.
 * @return bool  true if desired was stored.
 */
bool os_arch_atomic_cas(__IO int32_t *target, int32_t expected, int32_t desired)
{
    bool swapped = false;

    os_critical_enter();

    if (*target == expected)
    {
        *target = desired;
        swapped = true;
    }

    os_critical_exit();

    return swapped;
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
 * which does) - os_arch_sleep_prepare/os_arch_elapsed_ticks_get above still measure via a
 * SysTick-derived software cycle counter around a plain WFI, so there is no register-width-limited
 * window to report here. 0 tells callers (os_tick.c, tests) not to expect a real suppressed sleep
 * on this port yet.
 *
 * @return uint32_t  Always 0 until this port gets the same fix as os_arch_port_v8m.c.
 */
uint32_t os_arch_max_suppressed_ticks_get(void)
{
    return 0U;
}

/*
 * ***********************************************************************************************************
 * TrustZone glue and multi-core weak callback defaults
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
/******************************************************************************************************/
/**
 * @brief Weak defaults: applications that let tasks call secure code override these to bank the
 *        per-task secure context (secure stack / PSP_S) on the secure side.
 */
OS_WEAK void os_arch_tz_context_save_cb(uint32_t task_id)
{
    (void)task_id;
}

OS_WEAK void os_arch_tz_context_restore_cb(uint32_t task_id)
{
    (void)task_id;
}

/******************************************************************************************************/
/**
 * @brief Context-switch trampolines called from the SVC/PendSV handlers. Save runs while
 *        os_task_current still names the outgoing task; restore runs after selection.
 */
void os_arch_tz_context_save(void)
{
    os_arch_tz_context_save_cb(os_task_current_id_get());
}

void os_arch_tz_context_restore(void)
{
    os_arch_tz_context_restore_cb(os_task_current_id_get());
}
#endif /* OS_CONFIG_TRUSTZONE_NON_SECURE */

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief Weak defaults for the SoC multi-core callbacks: single-core behavior (core 0, no IPI;
 *        cross-core wakeups then happen at the next tick).
 */
OS_WEAK uint32_t os_arch_core_id_get_cb(void)
{
    return 0U;
}

OS_WEAK void os_arch_core_ipi_request_cb(uint32_t core_id)
{
    (void)core_id;
}
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
