/**
 * @file os_atomic.c
 * @brief Atomic operations on a single word.
 *
 * Portable half of the atomic API. Nothing here knows how a word is made to update indivisibly -
 * that is the core's business and lives entirely in the port (arch/arm/common/os_arch_atomic.c),
 * which is what lets a core with LDREX/STREX stay lock-free while one without pays interrupt
 * latency instead, with no trace of either choice in this file.
 *
 * What is left over is genuinely portable: argument validation, and the handful of operations that
 * are just another one under a different name - increment is an add of 1, clearing a bit is an AND
 * with its complement. Composing those here rather than in each port keeps the set a port has to
 * implement small, and keeps their behaviour identical on every core by construction.
 *
 * Each function validates and then makes exactly one call into the port, rather than routing
 * through another os_atomic_* function that would validate the same pointer again. The port is a
 * separate translation unit, so without link-time optimization nothing here can be inlined into
 * its caller and every layer is a real branch and stack frame: os_atomic_set_bit built on
 * os_atomic_test_and_set_bit built on os_atomic_or would be three of them to reach a single
 * instruction's worth of work. The cost is that the NULL check is written out repeatedly below.
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

/* The operations below index bits and swap whole words, both of which assume this width, and the
 * port's atomics are declared over a 32-bit word. */
_Static_assert(sizeof(os_atomic_t) == 4U,
               "the atomic operations assume a 32-bit word");

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Read the current value.
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

    return os_arch_atomic_load((const __IO int32_t *)target);
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

    return os_arch_atomic_exchange((__IO int32_t *)target, value);
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
    OS_ASSERT(target != NULL);

    if (target == NULL)
    {
        return 0;
    }

    return os_arch_atomic_exchange((__IO int32_t *)target, 0);
}

/******************************************************************************************************/
/**
 * @brief Add, returning the previous value.
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

    return os_arch_atomic_add((__IO int32_t *)target, value);
}

/******************************************************************************************************/
/**
 * @brief Subtract, returning the previous value.
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

    return os_arch_atomic_sub((__IO int32_t *)target, value);
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
    OS_ASSERT(target != NULL);

    if (target == NULL)
    {
        return 0;
    }

    return os_arch_atomic_add((__IO int32_t *)target, 1);
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
    OS_ASSERT(target != NULL);

    if (target == NULL)
    {
        return 0;
    }

    return os_arch_atomic_sub((__IO int32_t *)target, 1);
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

    return os_arch_atomic_or((__IO int32_t *)target, value);
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

    return os_arch_atomic_and((__IO int32_t *)target, value);
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

    return os_arch_atomic_xor((__IO int32_t *)target, value);
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

    return os_arch_atomic_nand((__IO int32_t *)target, value);
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

    return os_arch_atomic_cas((__IO int32_t *)target, expected, desired);
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

    return ((os_arch_atomic_load((const __IO int32_t *)target) & (int32_t)(1UL << bit)) != 0);
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
    previous = os_arch_atomic_or((__IO int32_t *)target, mask);

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
    previous = os_arch_atomic_and((__IO int32_t *)target, (int32_t)(~mask));

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
    OS_ASSERT(target != NULL);
    OS_ASSERT(bit < OS_ATOMIC_BITS);

    if ((target == NULL) || (bit >= OS_ATOMIC_BITS))
    {
        return;
    }

    (void)os_arch_atomic_or((__IO int32_t *)target, (int32_t)(1UL << bit));
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
    OS_ASSERT(target != NULL);
    OS_ASSERT(bit < OS_ATOMIC_BITS);

    if ((target == NULL) || (bit >= OS_ATOMIC_BITS))
    {
        return;
    }

    (void)os_arch_atomic_and((__IO int32_t *)target, (int32_t)(~(1UL << bit)));
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
    int32_t mask;

    OS_ASSERT(target != NULL);
    OS_ASSERT(bit < OS_ATOMIC_BITS);

    if ((target == NULL) || (bit >= OS_ATOMIC_BITS))
    {
        return;
    }

    mask = (int32_t)(1UL << bit);

    if (value)
    {
        (void)os_arch_atomic_or((__IO int32_t *)target, mask);
    }
    else
    {
        (void)os_arch_atomic_and((__IO int32_t *)target, (int32_t)(~mask));
    }
}

#endif /* OS_CONFIG_ATOMIC_ENABLE */
