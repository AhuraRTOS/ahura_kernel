/**
 * @file memory_pool.c
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
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize a fixed-block memory pool.
 *
 * @param[in,out] pool        Memory pool object.
 * @param[in]     buffer      Backing storage for blocks.
 * @param[in]     usage_map   Per-block usage flags.
 * @param[in]     block_size  Block size in bytes.
 * @param[in]     block_count Number of blocks.
 * @return os_status     Status code.
 */
os_status os_memory_pool_init(os_memory_pool_t *pool, void *buffer, void *usage_map, size_t block_size, size_t block_count)
{
    size_t index = 0U;

    if ((pool == NULL) || (buffer == NULL) || (usage_map == NULL) || (block_size == 0U) || (block_count == 0U))
    {
        return OS_STATUS_INVALID_ARG;
    }

    pool->buffer      = (uint8_t *)buffer;
    pool->in_use      = (uint8_t *)usage_map;
    pool->block_size  = block_size;
    pool->block_count = block_count;

    for (index = 0U; index < block_count; index++)
    {
        pool->in_use[index] = 0U;
    }

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

    base_addr  = (uintptr_t)pool->buffer;
    block_addr = (uintptr_t)block;

    if (block_addr < base_addr)
    {
        return OS_STATUS_INVALID_ARG;
    }

    offset = block_addr - base_addr;

    if ((offset % pool->block_size) != 0U)
    {
        return OS_STATUS_INVALID_ARG;
    }

    block_index = (size_t)(offset / pool->block_size);

    if (block_index >= pool->block_count)
    {
        return OS_STATUS_INVALID_ARG;
    }

    os_critical_enter();

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
