/**
 * @file os_arch_atomic.c
 * @brief Atomic word operations for the ARM ports.
 *
 * Included by each shared port implementation (os_arch_port_v6m.c, _v7m.c, _v8m.c) rather than
 * compiled on its own, the same way those are included by the per-core os_arch_port.c files.
 *
 * The whole os_atomic_* API bottoms out here, because making a word update indivisible is a
 * property of the core and nothing else:
 *
 *   Cores WITH exclusives (ARMv7-M, ARMv7E-M, ARMv8-M baseline and mainline) can detect that
 *   something else touched the word. LDREX takes a reservation, STREX stores only if it survived,
 *   and a failed store just means going round again. Interrupts stay enabled the whole time.
 *
 *   Cores WITHOUT them (ARMv6-M: Cortex-M0, M0+, M1) have no such instruction, so interference
 *   cannot be detected - it has to be prevented. The update runs inside os_critical_enter/exit,
 *   which masks this core's kernel interrupts and, on a multi-core build, locks out the other
 *   cores. This is the one backend where an atomic operation adds to interrupt latency, and where
 *   it can wait on unrelated kernel work holding the same lock. Worth knowing before putting one
 *   in an ARMv6-M hot path; reusing the kernel's own critical section rather than a second private
 *   lock is deliberate, since two independent locks around the same data is how lock-ordering bugs
 *   start.
 *
 * The file is arranged so that each of those appears exactly once:
 *
 *   os_arch_atomic_compute()  what the new value is        - pure arithmetic, no atomicity
 *   os_arch_atomic_apply()    how it is made indivisible   - one definition per backend
 *   os_arch_atomic_*()        the public operations        - one line each
 *
 * So a port's behaviour is decided by two small functions, and adding an operation means adding
 * one enumerator and one wrapper, with nothing to get wrong in either backend.
 *
 * Every read-modify-write returns the value the word held BEFORE the operation.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

#if (OS_CONFIG_ATOMIC_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Private types
 * ***********************************************************************************************************
*/

/**
 * @brief Which update os_arch_atomic_apply() should perform. Internal to this file: callers name
 *        the operation by calling the matching os_arch_atomic_* function.
 */
typedef enum
{
    OS_ARCH_ATOMIC_OP_STORE = 0, /**< Replace with value.  */
    OS_ARCH_ATOMIC_OP_ADD,       /**< current + value.     */
    OS_ARCH_ATOMIC_OP_SUB,       /**< current - value.     */
    OS_ARCH_ATOMIC_OP_OR,        /**< current | value.     */
    OS_ARCH_ATOMIC_OP_AND,       /**< current & value.     */
    OS_ARCH_ATOMIC_OP_XOR,       /**< current ^ value.     */
    OS_ARCH_ATOMIC_OP_NAND,      /**< ~(current & value).  */

} os_arch_atomic_op_t;

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Work out the new value. Pure arithmetic: no atomicity, no side effects, no memory access.
 *
 * Separated from the two backends below so the arithmetic is written once and read as ordinary C,
 * and so neither backend can quietly disagree with the other about what an operation means. Every
 * call site passes a constant op, so this folds away to the single instruction the operation
 * actually is - the switch costs nothing at run time. Force-inlined rather than left to the
 * compiler's judgement because the exclusives backend calls it between an LDREX and its STREX,
 * where an actual call would put a branch and stack traffic inside the reservation window.
 *
 * ADD and SUB compute in the unsigned domain and convert back. Signed overflow is undefined
 * behaviour the moment a counter runs past INT32_MAX - not merely a negative-looking result, but
 * licence for the compiler to assume it cannot happen and optimise accordingly. Unsigned
 * arithmetic wraps with defined behaviour, and the conversion back reproduces the same
 * two's-complement bit pattern the hardware would have stored anyway.
 *
 * @param[in] op       Operation to perform.
 * @param[in] current  Value the word holds now.
 * @param[in] value    Operand supplied by the caller.
 * @return int32_t  Value to store.
 */
OS_FORCE_INLINE int32_t os_arch_atomic_compute(os_arch_atomic_op_t op, int32_t current, int32_t value)
{
    int32_t updated;

    switch (op)
    {
        case OS_ARCH_ATOMIC_OP_ADD:
            updated = (int32_t)((uint32_t)current + (uint32_t)value);
            break;

        case OS_ARCH_ATOMIC_OP_SUB:
            updated = (int32_t)((uint32_t)current - (uint32_t)value);
            break;

        case OS_ARCH_ATOMIC_OP_OR:
            updated = current | value;
            break;

        case OS_ARCH_ATOMIC_OP_AND:
            updated = current & value;
            break;

        case OS_ARCH_ATOMIC_OP_XOR:
            updated = current ^ value;
            break;

        case OS_ARCH_ATOMIC_OP_NAND:
            updated = ~(current & value);
            break;

        case OS_ARCH_ATOMIC_OP_STORE:
        default:
            /* The default arm is unreachable - op is always one of the enumerators above, from a
             * call site in this file - and shares STORE's body so that adding an enumerator
             * without a case here degrades to a plain store rather than an uninitialized read. */
            updated = value;
            break;
    }

    return updated;
}

#if (OS_ARCH_HAS_EXCLUSIVES == 1U)
/******************************************************************************************************/
/**
 * @brief Perform op indivisibly, on a core that has exclusives.
 *
 * LDREX takes a reservation on the word, the new value is worked out from what it held, and STREX
 * stores it only if the reservation survived; if it did not, go round again. Nothing is masked, so
 * an ISR - or another core - landing in the middle costs this loop another pass rather than
 * costing anyone correctness.
 *
 * The reload has to be INSIDE the loop. A failed STREX means the value read at the top is no
 * longer known to be the value in memory, and computing from it anyway would write back a result
 * derived from a value nobody holds any more: exactly the lost update these operations exist to
 * prevent.
 *
 * This is why the arithmetic sits between the two instructions rather than being expressed as a
 * compare-and-swap loop around os_arch_atomic_cas(): a CAS shape has to load the word a second
 * time and compare it, where the reservation already carries that information. Roughly seven
 * instructions per attempt become four. os_arch_atomic_compute() is force-inlined so nothing but
 * that arithmetic lands inside the window - ARM guarantees only one outstanding reservation per
 * core and leaves it implementation-defined whether unrelated accesses clear it, so a call there
 * would be borrowing trouble.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     op      Operation to perform.
 * @param[in]     value   Operand supplied by the caller.
 * @return int32_t  Value held before the operation.
 */
static int32_t os_arch_atomic_apply(__IO int32_t *target, os_arch_atomic_op_t op, int32_t value)
{
    int32_t  current;
    int32_t  updated;
    uint32_t store_failed;

    do
    {
        __asm volatile("ldrex %0, [%1]" : "=r"(current) : "r"(target) : "memory");

        updated = os_arch_atomic_compute(op, current, value);

        __asm volatile("strex %0, %1, [%2]"
                       : "=&r"(store_failed)
                       : "r"(updated), "r"(target)
                       : "memory");

    } while (store_failed != 0U);

    return current;
}
#else
/******************************************************************************************************/
/**
 * @brief Perform op indivisibly, on a core with no exclusives.
 *
 * No retry loop, and no second read: nothing can interfere while the critical section is held, so
 * the value read is still the value in memory when it is written back. That is the whole reason
 * the two backends are written separately instead of one being made to serve both - forcing this
 * core through a compare-and-swap retry would pay for detecting interference that has already been
 * prevented.
 *
 * os_critical_enter/exit is nesting-aware and safe from interrupt context, so an atomic used
 * inside a critical section, or from an ISR, behaves like any other nested use.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     op      Operation to perform.
 * @param[in]     value   Operand supplied by the caller.
 * @return int32_t  Value held before the operation.
 */
static int32_t os_arch_atomic_apply(__IO int32_t *target, os_arch_atomic_op_t op, int32_t value)
{
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = os_arch_atomic_compute(op, current, value);

    os_critical_exit();

    return current;
}
#endif /* OS_ARCH_HAS_EXCLUSIVES */

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/* os_arch_atomic_load() is not here: a single aligned 32-bit load is already indivisible on every
 * core this port targets, so it is one LDR and lives inline in os_arch_port_common.h rather than
 * costing a call across to the port. */

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
    return os_arch_atomic_apply(target, OS_ARCH_ATOMIC_OP_STORE, value);
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
    return os_arch_atomic_apply(target, OS_ARCH_ATOMIC_OP_ADD, value);
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
    return os_arch_atomic_apply(target, OS_ARCH_ATOMIC_OP_SUB, value);
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
    return os_arch_atomic_apply(target, OS_ARCH_ATOMIC_OP_OR, value);
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
    return os_arch_atomic_apply(target, OS_ARCH_ATOMIC_OP_AND, value);
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
    return os_arch_atomic_apply(target, OS_ARCH_ATOMIC_OP_XOR, value);
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
    return os_arch_atomic_apply(target, OS_ARCH_ATOMIC_OP_NAND, value);
}

/******************************************************************************************************/
/**
 * @brief Atomic compare-and-swap. See os_arch_port_common.h.
 *
 * The one operation that does not retry: a false return is the answer the caller asked for, which
 * is what makes it usable as a building block for lock-free algorithms the kernel knows nothing
 * about. The operations above do not go through it - they retry their own STREX, which is both
 * fewer instructions and fewer memory accesses than a compare-and-swap loop would need.
 *
 * @param[in,out] target    Word to update.
 * @param[in]     expected  Value the caller believes target holds.
 * @param[in]     desired   Value to store if it still does.
 * @return bool  true if desired was stored.
 */
bool os_arch_atomic_cas(__IO int32_t *target, int32_t expected, int32_t desired)
{
#if (OS_ARCH_HAS_EXCLUSIVES == 1U)
    int32_t  current;
    uint32_t store_failed;

    __asm volatile("ldrex %0, [%1]" : "=r"(current) : "r"(target) : "memory");

    if (current != expected)
    {
        /* Drop the reservation rather than leaving it set on a word this call is walking away
         * from: a stale one can make an unrelated later STREX succeed when it should not, and the
         * architecture only guarantees one outstanding reservation per core. */
        __asm volatile("clrex" ::: "memory");
        return false;
    }

    __asm volatile("strex %0, %1, [%2]" : "=&r"(store_failed) : "r"(desired), "r"(target) : "memory");

    /* A failed STREX here is a spurious failure, not a mismatch: the word did still hold expected,
     * the reservation was simply lost. The portable contract is written to allow that, which is
     * why callers needing certainty re-read instead of reading false as "someone else won". */
    return (store_failed == 0U);
#else
    bool swapped = false;

    os_critical_enter();

    if (*target == expected)
    {
        *target = desired;
        swapped = true;
    }

    os_critical_exit();

    /* Never fails spuriously on this backend - nothing could have interfered - but callers still
     * loop, because the portable contract is written to the weaker guarantee. */
    return swapped;
#endif
}

#endif /* OS_CONFIG_ATOMIC_ENABLE */
