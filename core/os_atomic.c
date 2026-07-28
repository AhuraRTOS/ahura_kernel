/**
 * @file os_atomic.c
 * @brief Atomic operations on a single word.
 *
 * Everything here is built from one architecture primitive, os_arch_atomic_cas(), so a port only
 * has to provide a correct compare-and-swap and gets the whole API. On cores with LDREX/STREX
 * that primitive is genuinely lock-free and never masks interrupts; on ARMv6-M, which has no
 * exclusives, it briefly excludes interrupts and other cores instead.
 *
 * Every read-modify-write follows the same shape: read the current value, compute the new one,
 * and try to swap it in, repeating while the swap fails. The read has to happen INSIDE the loop.
 * A CAS can fail either because another writer changed the word or because the exclusive access
 * was lost, and in the first case a value cached from before the loop is already stale - reusing
 * it would write back a result computed from a value nobody holds any more, which is exactly the
 * lost update these functions exist to prevent.
 *
 * All of them return the value the word held BEFORE the operation, not after it.
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

#include "os_internal.h"

#if (OS_CONFIG_ATOMIC_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

/* Number of bits in an os_atomic_t, and so the exclusive upper bound for every bit index. */
#define OS_ATOMIC_BITS  32U

/* The operations below index bits and swap whole words, both of which assume this width. */
_Static_assert(sizeof(os_atomic_t) == 4U,
               "the atomic operations assume a 32-bit word");

/*
 * Shared body of every read-modify-write below. Declared as a macro rather than repeated by hand
 * so the retry structure - and in particular the reload of the current value on each pass - is
 * written exactly once.
 */
#define OS_ATOMIC_FETCH_OP(target, expr)                                       \
    do {                                                                       \
        int32_t current;                                                       \
                                                                               \
        do                                                                     \
        {                                                                      \
            current = *(__IO int32_t *)(target);                           \
        } while (!os_arch_atomic_cas((__IO int32_t *)(target),             \
                                     current, (int32_t)(expr)));               \
                                                                               \
        return current;                                                        \
    } while (0)

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Read the current value.
 *
 * A single aligned 32-bit load is already indivisible on Cortex-M, so there is no retry loop here.
 * What this does add over reading the variable directly is the volatile access, which stops the
 * compiler from reusing a value it cached before some other code path changed it.
 *
 * @param[in] target  Word to read.
 * @return int32_t  Current value, or 0 for a NULL target.
 */
int32_t os_atomic_get(const os_atomic_t *target)
{
    if (target == NULL)
    {
        return 0;
    }

    return *(__I int32_t *)target;
}

/******************************************************************************************************/
/**
 * @brief Store a value, returning the previous one.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Value to store.
 * @return int32_t  Value held before the store.
 */
int32_t os_atomic_set(os_atomic_t *target, int32_t value)
{
    OS_ASSERT(target != NULL);

    if (target == NULL)
    {
        return 0;
    }

    OS_ATOMIC_FETCH_OP(target, value);
}

/******************************************************************************************************/
/**
 * @brief Store 0, returning the previous value.
 *
 * @param[in,out] target  Word to clear.
 * @return int32_t  Value held before the clear.
 */
int32_t os_atomic_clear(os_atomic_t *target)
{
    return os_atomic_set(target, 0);
}

/******************************************************************************************************/
/**
 * @brief Add, returning the previous value.
 *
 * The sum is computed in uint32_t and converted back. Adding in the signed domain would be
 * undefined behaviour the moment a counter ran past INT32_MAX - not merely a negative-looking
 * result, but licence for the compiler to assume it cannot happen and optimise accordingly.
 * Unsigned arithmetic wraps with defined behaviour, and the conversion back reproduces the same
 * two's-complement bit pattern the hardware would have stored anyway.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Amount to add.
 * @return int32_t  Value held before the addition.
 */
int32_t os_atomic_add(os_atomic_t *target, int32_t value)
{
    OS_ASSERT(target != NULL);

    if (target == NULL)
    {
        return 0;
    }

    OS_ATOMIC_FETCH_OP(target, (int32_t)((uint32_t)current + (uint32_t)value));
}

/******************************************************************************************************/
/**
 * @brief Subtract, returning the previous value.
 *
 * Computed in uint32_t for the same reason as os_atomic_add: signed underflow past INT32_MIN is
 * undefined behaviour, unsigned wrapping is not.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Amount to subtract.
 * @return int32_t  Value held before the subtraction.
 */
int32_t os_atomic_sub(os_atomic_t *target, int32_t value)
{
    OS_ASSERT(target != NULL);

    if (target == NULL)
    {
        return 0;
    }

    OS_ATOMIC_FETCH_OP(target, (int32_t)((uint32_t)current - (uint32_t)value));
}

/******************************************************************************************************/
/**
 * @brief Add 1, returning the previous value.
 *
 * @param[in,out] target  Word to increment.
 * @return int32_t  Value held before the increment.
 */
int32_t os_atomic_inc(os_atomic_t *target)
{
    return os_atomic_add(target, 1);
}

/******************************************************************************************************/
/**
 * @brief Subtract 1, returning the previous value.
 *
 * @param[in,out] target  Word to decrement.
 * @return int32_t  Value held before the decrement.
 */
int32_t os_atomic_dec(os_atomic_t *target)
{
    return os_atomic_sub(target, 1);
}

/******************************************************************************************************/
/**
 * @brief Bitwise OR, returning the previous value.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Bits to set.
 * @return int32_t  Value held before the operation.
 */
int32_t os_atomic_or(os_atomic_t *target, int32_t value)
{
    OS_ASSERT(target != NULL);

    if (target == NULL)
    {
        return 0;
    }

    OS_ATOMIC_FETCH_OP(target, current | value);
}

/******************************************************************************************************/
/**
 * @brief Bitwise AND, returning the previous value.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Mask to keep.
 * @return int32_t  Value held before the operation.
 */
int32_t os_atomic_and(os_atomic_t *target, int32_t value)
{
    OS_ASSERT(target != NULL);

    if (target == NULL)
    {
        return 0;
    }

    OS_ATOMIC_FETCH_OP(target, current & value);
}

/******************************************************************************************************/
/**
 * @brief Bitwise XOR, returning the previous value.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Bits to flip.
 * @return int32_t  Value held before the operation.
 */
int32_t os_atomic_xor(os_atomic_t *target, int32_t value)
{
    OS_ASSERT(target != NULL);

    if (target == NULL)
    {
        return 0;
    }

    OS_ATOMIC_FETCH_OP(target, current ^ value);
}

/******************************************************************************************************/
/**
 * @brief Bitwise NAND (~(old & value)), returning the previous value.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Operand.
 * @return int32_t  Value held before the operation.
 */
int32_t os_atomic_nand(os_atomic_t *target, int32_t value)
{
    OS_ASSERT(target != NULL);

    if (target == NULL)
    {
        return 0;
    }

    OS_ATOMIC_FETCH_OP(target, ~(current & value));
}

/******************************************************************************************************/
/**
 * @brief Compare-and-swap: store desired only if the word still holds expected.
 *
 * Unlike everything above, this does NOT retry. A false return is the answer the caller asked
 * for - the word no longer held expected - and is what makes CAS the building block for
 * lock-free algorithms the kernel does not know about.
 *
 * The port's primitive may also fail spuriously when it loses exclusive access, so a false return
 * does not by itself prove another writer won. Callers that only care about the final state
 * should loop; callers implementing "try once, otherwise do something else" should re-read the
 * value to find out which happened.
 *
 * @param[in,out] target    Word to update.
 * @param[in]     expected  Value the caller believes the word holds.
 * @param[in]     desired   Value to store if it still does.
 * @return bool  true if desired was stored.
 */
bool os_atomic_cas(os_atomic_t *target, int32_t expected, int32_t desired)
{
    OS_ASSERT(target != NULL);

    if (target == NULL)
    {
        return false;
    }

    return os_arch_atomic_cas((__IO int32_t *)target, (int32_t)expected, (int32_t)desired);
}

/******************************************************************************************************/
/**
 * @brief Test one bit.
 *
 * @param[in] target  Word to read.
 * @param[in] bit     Bit index, 0 to 31.
 * @return bool  true if the bit is set. false for a NULL target or an out-of-range index.
 */
bool os_atomic_test_bit(const os_atomic_t *target, uint32_t bit)
{
    OS_ASSERT(bit < OS_ATOMIC_BITS);

    if ((target == NULL) || (bit >= OS_ATOMIC_BITS))
    {
        return false;
    }

    return ((os_atomic_get(target) & (int32_t)(1UL << bit)) != 0);
}

/******************************************************************************************************/
/**
 * @brief Set one bit, returning its previous state.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     bit     Bit index, 0 to 31.
 * @return bool  true if the bit was already set.
 */
bool os_atomic_test_and_set_bit(os_atomic_t *target, uint32_t bit)
{
    int32_t mask;
    int32_t previous;

    OS_ASSERT(target != NULL);
    OS_ASSERT(bit < OS_ATOMIC_BITS);

    if ((target == NULL) || (bit >= OS_ATOMIC_BITS))
    {
        return false;
    }

    mask     = (int32_t)(1UL << bit);
    previous = os_atomic_or(target, mask);

    return ((previous & mask) != 0);
}

/******************************************************************************************************/
/**
 * @brief Clear one bit, returning its previous state.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     bit     Bit index, 0 to 31.
 * @return bool  true if the bit was set before the call.
 */
bool os_atomic_test_and_clear_bit(os_atomic_t *target, uint32_t bit)
{
    int32_t mask;
    int32_t previous;

    OS_ASSERT(target != NULL);
    OS_ASSERT(bit < OS_ATOMIC_BITS);

    if ((target == NULL) || (bit >= OS_ATOMIC_BITS))
    {
        return false;
    }

    mask     = (int32_t)(1UL << bit);
    previous = os_atomic_and(target, (int32_t)(~mask));

    return ((previous & mask) != 0);
}

/******************************************************************************************************/
/**
 * @brief Set one bit.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     bit     Bit index, 0 to 31.
 * @return None.
 */
void os_atomic_set_bit(os_atomic_t *target, uint32_t bit)
{
    (void)os_atomic_test_and_set_bit(target, bit);
}

/******************************************************************************************************/
/**
 * @brief Clear one bit.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     bit     Bit index, 0 to 31.
 * @return None.
 */
void os_atomic_clear_bit(os_atomic_t *target, uint32_t bit)
{
    (void)os_atomic_test_and_clear_bit(target, bit);
}

/******************************************************************************************************/
/**
 * @brief Set one bit to the given state.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     bit     Bit index, 0 to 31.
 * @param[in]     value   true to set the bit, false to clear it.
 * @return None.
 */
void os_atomic_set_bit_to(os_atomic_t *target, uint32_t bit, bool value)
{
    if (value)
    {
        os_atomic_set_bit(target, bit);
    }
    else
    {
        os_atomic_clear_bit(target, bit);
    }
}

#endif /* OS_CONFIG_ATOMIC_ENABLE */
