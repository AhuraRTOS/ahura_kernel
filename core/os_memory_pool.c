/**
 * @file os_memory_pool.c
 * @brief Fixed-block memory pool module implementation.
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

#if (OS_CONFIG_MEMORY_POOL_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

/* Blocks are handed out as raw pointers with no type information, so callers
 * routinely store aligned types (uint32_t/pointers/structs) in them; both the
 * block size and the backing buffer's base must be a multiple of this or such
 * an access can fault (LDM/STM/LDRD/exclusives all require natural alignment
 * on Cortex-M). Matches the kernel heap's alignment (os_alloc.c). */
#define OS_MEMORY_POOL_ALIGNMENT      8U
#define OS_MEMORY_POOL_ALIGN_MSK      ((size_t)(OS_MEMORY_POOL_ALIGNMENT - 1U))

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize a fixed-block memory pool.
 *
 * Re-initializing a pool that still has blocks outstanding is refused (the
 * usage map would be reset while a live allocation still points into the
 * buffer, handing that same block out again to a second owner). block_size
 * and the buffer's base address must both be 8-byte aligned so a block may
 * safely hold any C type - rounding block_size up instead would silently
 * grow the pool's total footprint past the caller-sized buffer, so it is
 * rejected rather than adjusted.
 *
 * @param[in,out] pool        Memory pool object.
 * @param[in]     buffer      Backing storage for blocks (block_count * block_size bytes).
 * @param[in]     usage_map   Per-block usage flags (block_count bytes).
 * @param[in]     block_size  Block size in bytes; must be a multiple of 8.
 * @param[in]     block_count Number of blocks.
 * @return os_status  OK, INVALID_ARG on bad arguments/alignment, BUSY while blocks are outstanding.
 */
os_status os_memory_pool_init(os_memory_pool_t *pool, void *buffer, void *usage_map, size_t block_size, size_t block_count)
{
    size_t index = 0U;

    if ((pool == NULL) || (buffer == NULL) || (usage_map == NULL) || (block_size == 0U) || (block_count == 0U))
    {
        return OS_STATUS_INVALID_ARG;
    }

    if (((block_size & OS_MEMORY_POOL_ALIGN_MSK) != 0U) ||
        (((uintptr_t)buffer & OS_MEMORY_POOL_ALIGN_MSK) != 0U))
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

    /* A freshly zero-initialized static pool has block_count == 0 and skips
     * this check; a live pool scans its own (old) usage map. Matches the
     * re-init guard every sibling primitive (mutex/semaphore/queue/event)
     * already has. */
    if (pool->block_count != 0U)
    {
        size_t existing_index;

        for (existing_index = 0U; existing_index < pool->block_count; existing_index++)
        {
            if (pool->in_use[existing_index] != 0U)
            {
                os_critical_exit();
                return OS_STATUS_BUSY;
            }
        }
    }

    pool->buffer      = (uint8_t *)buffer;
    pool->in_use      = (uint8_t *)usage_map;
    pool->block_size  = block_size;
    pool->block_count = block_count;

    for (index = 0U; index < block_count; index++)
    {
        pool->in_use[index] = 0U;
    }

    os_critical_exit();
    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Allocate one block from a memory pool.
 *
 * @param[in,out] pool  Memory pool object.
 * @return void*        Pointer to block or NULL when unavailable.
 */
void* os_memory_pool_alloc(os_memory_pool_t *pool)
{
    size_t index = 0U;

    if (pool == NULL)
    {
        return NULL;
    }

    os_critical_enter();

    for (index = 0U; index < pool->block_count; index++)
    {
        if (pool->in_use[index] == 0U)
        {
            pool->in_use[index] = 1U;
            os_critical_exit();
            return &pool->buffer[index * pool->block_size];
        }
    }

    os_critical_exit();
    return NULL;
}

/******************************************************************************************************/
/**
 * @brief Free one block back to a memory pool.
 *
 * @param[in,out] pool   Memory pool object.
 * @param[in]     block  Block pointer previously allocated from this pool.
 * @return os_status Status code.
 */
os_status os_memory_pool_free(os_memory_pool_t *pool, void *block)
{
    uintptr_t base_addr   = 0U;
    uintptr_t block_addr  = 0U;
    uintptr_t offset      = 0U;
    size_t    block_index = 0U;

    if ((pool == NULL) || (block == NULL))
    {
        return OS_STATUS_INVALID_ARG;
    }

    /* pool->buffer/block_size/block_count are read under the critical
     * section, like every field access in the sibling primitives
     * (mutex/semaphore/queue/event): a re-init running concurrently on
     * another core (already refused while blocks are outstanding, so only
     * reachable through caller misuse) must not pair a stale buffer/size
     * with a fresh block_count or vice versa. */
    os_critical_enter();

    base_addr  = (uintptr_t)pool->buffer;
    block_addr = (uintptr_t)block;

    if (block_addr < base_addr)
    {
        os_critical_exit();
        return OS_STATUS_INVALID_ARG;
    }

    offset = block_addr - base_addr;

    if ((offset % pool->block_size) != 0U)
    {
        os_critical_exit();
        return OS_STATUS_INVALID_ARG;
    }

    block_index = (size_t)(offset / pool->block_size);

    if (block_index >= pool->block_count)
    {
        os_critical_exit();
        return OS_STATUS_INVALID_ARG;
    }

    if (pool->in_use[block_index] == 0U)
    {
        os_critical_exit();
        return OS_STATUS_ERROR;
    }

    pool->in_use[block_index] = 0U;

    os_critical_exit();
    return OS_STATUS_OK;
}

#endif /* OS_CONFIG_MEMORY_POOL_ENABLE */
