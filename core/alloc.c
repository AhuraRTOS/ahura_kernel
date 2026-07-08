/**
 * @file alloc.c
 * @brief Kernel heap: os_alloc/os_free over a static heap array.
 *
 * First-fit allocator with an address-ordered free list and coalescing of
 * adjacent free blocks, so mixed-size alloc/free patterns do not fragment
 * the heap permanently. All operations run inside the kernel critical
 * section: safe from tasks and (though discouraged) from interrupts.
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

#if (OS_CONFIG_ALLOC_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#define OS_ALLOC_ALIGNMENT       8U
#define OS_ALLOC_ALIGN_MSK       ((size_t)(OS_ALLOC_ALIGNMENT - 1U))
#define OS_ALLOC_ALLOCATED_MSK   ((size_t)1 << ((sizeof(size_t) * 8U) - 1U))
#define OS_ALLOC_HEADER_SIZE     ((sizeof(os_alloc_block_t) + OS_ALLOC_ALIGN_MSK) & ~OS_ALLOC_ALIGN_MSK)
#define OS_ALLOC_MIN_BLOCK_SIZE  (OS_ALLOC_HEADER_SIZE + OS_ALLOC_ALIGNMENT)

#if (OS_CONFIG_HEAP_SIZE < 64U)
#error "OS_CONFIG_HEAP_SIZE is too small to hold the allocator bookkeeping."
#endif

/*
 * ***********************************************************************************************************
 * Types
 * ***********************************************************************************************************
*/

/* Every heap block starts with this header; size covers header + payload and
 * carries the allocated flag in its top bit. */
typedef struct os_alloc_block
{
    struct os_alloc_block *next;
    size_t                 size;

} os_alloc_block_t;

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static uint8_t           os_alloc_heap[OS_CONFIG_HEAP_SIZE];
static os_alloc_block_t  os_alloc_start;
static os_alloc_block_t  *os_alloc_end           = (os_alloc_block_t *)0;
static size_t            os_alloc_free_bytes     = 0U;
static size_t            os_alloc_min_free_bytes = 0U;

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static void os_alloc_init(void);
static void os_alloc_block_insert(os_alloc_block_t *block);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Allocate memory from the kernel heap.
 *
 * @param[in] size  Requested payload size in bytes.
 * @return void*    8-byte aligned memory, or NULL when size is 0 or no fitting block exists.
 */
void* os_alloc(size_t size)
{
    os_alloc_block_t *prev;
    os_alloc_block_t *block;
    void             *memory = (void *)0;
    size_t           need;

    if (size == 0U)
    {
        return (void *)0;
    }

    /* Whole-block size: header + payload rounded up to the alignment. The
     * top bit is the allocated flag, so a size that reaches it (or wraps)
     * can never be satisfied. */
    need = OS_ALLOC_HEADER_SIZE + ((size + OS_ALLOC_ALIGN_MSK) & ~OS_ALLOC_ALIGN_MSK);
    if ((need < size) || ((need & OS_ALLOC_ALLOCATED_MSK) != 0U))
    {
        return (void *)0;
    }

    os_critical_enter();

    if (os_alloc_end == (os_alloc_block_t *)0)
    {
        os_alloc_init();
    }

    /* First fit: the free list is address ordered and ends at the marker. */
    prev  = &os_alloc_start;
    block = os_alloc_start.next;
    while ((block != os_alloc_end) && (block->size < need))
    {
        prev  = block;
        block = block->next;
    }

    if (block != os_alloc_end)
    {
        prev->next = block->next;

        /* Split when the leftover still makes a usable free block. */
        if ((block->size - need) >= OS_ALLOC_MIN_BLOCK_SIZE)
        {
            os_alloc_block_t *remainder = (os_alloc_block_t *)(void *)((uint8_t *)block + need);

            remainder->size = block->size - need;
            block->size     = need;
            os_alloc_block_insert(remainder);
        }

        os_alloc_free_bytes -= block->size;
        if (os_alloc_free_bytes < os_alloc_min_free_bytes)
        {
            os_alloc_min_free_bytes = os_alloc_free_bytes;
        }

        block->size |= OS_ALLOC_ALLOCATED_MSK;
        block->next = (os_alloc_block_t *)0;

        memory = (void *)((uint8_t *)block + OS_ALLOC_HEADER_SIZE);
    }

    os_critical_exit();

    return memory;
}

/******************************************************************************************************/
/**
 * @brief Return memory obtained from os_alloc to the kernel heap.
 *
 * NULL, foreign and double-freed pointers are ignored.
 *
 * @param[in] memory  Pointer previously returned by os_alloc.
 * @return None.
 */
void os_free(void *memory)
{
    os_alloc_block_t *block;

    if ((memory == (void *)0) || (os_alloc_end == (os_alloc_block_t *)0))
    {
        return;
    }

    if (((uint8_t *)memory <= &os_alloc_heap[0]) ||
        ((uint8_t *)memory >= &os_alloc_heap[OS_CONFIG_HEAP_SIZE]))
    {
        return;
    }

    block = (os_alloc_block_t *)(void *)((uint8_t *)memory - OS_ALLOC_HEADER_SIZE);

    /* An os_alloc block carries the allocated flag and a cleared link. */
    if (((block->size & OS_ALLOC_ALLOCATED_MSK) == 0U) || (block->next != (os_alloc_block_t *)0))
    {
        return;
    }

    block->size &= ~OS_ALLOC_ALLOCATED_MSK;

    os_critical_enter();

    os_alloc_free_bytes += block->size;
    os_alloc_block_insert(block);

    os_critical_exit();
}

/******************************************************************************************************/
/**
 * @brief Get the number of bytes currently free in the kernel heap.
 *
 * @return size_t  Free bytes (including per-block headers).
 */
size_t os_alloc_free_bytes_get(void)
{
    size_t free_bytes;

    os_critical_enter();

    if (os_alloc_end == (os_alloc_block_t *)0)
    {
        os_alloc_init();
    }

    free_bytes = os_alloc_free_bytes;

    os_critical_exit();

    return free_bytes;
}

/******************************************************************************************************/
/**
 * @brief Get the smallest amount of free heap ever observed (worst case since boot).
 *
 * @return size_t  Minimum free bytes watermark.
 */
size_t os_alloc_min_free_bytes_get(void)
{
    size_t min_free_bytes;

    os_critical_enter();

    if (os_alloc_end == (os_alloc_block_t *)0)
    {
        os_alloc_init();
    }

    min_free_bytes = os_alloc_min_free_bytes;

    os_critical_exit();

    return min_free_bytes;
}

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Lazily set up the free list: one block spanning the heap plus the end marker.
 *
 * Runtime alignment of the array bounds keeps the heap storage free of
 * compiler-specific attributes. Called inside the critical section.
 *
 * @return None.
 */
static void os_alloc_init(void)
{
    uintptr_t         heap_start = ((uintptr_t)os_alloc_heap + OS_ALLOC_ALIGN_MSK) & ~(uintptr_t)OS_ALLOC_ALIGN_MSK;
    uintptr_t         heap_end   = ((uintptr_t)os_alloc_heap + OS_CONFIG_HEAP_SIZE - OS_ALLOC_HEADER_SIZE) &
                                   ~(uintptr_t)OS_ALLOC_ALIGN_MSK;
    os_alloc_block_t *first      = (os_alloc_block_t *)heap_start;

    os_alloc_end       = (os_alloc_block_t *)heap_end;
    os_alloc_end->next = (os_alloc_block_t *)0;
    os_alloc_end->size = 0U;

    first->size = (size_t)(heap_end - heap_start);
    first->next = os_alloc_end;

    os_alloc_start.next = first;
    os_alloc_start.size = 0U;

    os_alloc_free_bytes     = first->size;
    os_alloc_min_free_bytes = first->size;
}

/******************************************************************************************************/
/**
 * @brief Insert a free block into the address-ordered list, merging with touching neighbors.
 *
 * Called inside the critical section; the block's allocated flag must be clear.
 *
 * @param[in] block  Free block to insert.
 * @return None.
 */
static void os_alloc_block_insert(os_alloc_block_t *block)
{
    os_alloc_block_t *iter;
    uint8_t          *address;

    /* Find the free block we insert after (list is address ordered). */
    for (iter = &os_alloc_start; iter->next < block; iter = iter->next)
    {
    }

    /* Merge with the predecessor when they touch (never the list head:
     * it lives outside the heap, so the addresses cannot line up). */
    address = (uint8_t *)iter;
    if ((address + iter->size) == (uint8_t *)block)
    {
        iter->size += block->size;
        block       = iter;
    }

    /* Merge with the successor when they touch; the end marker only ever
     * becomes the link target, never part of a block. */
    address = (uint8_t *)block;
    if ((address + block->size) == (uint8_t *)iter->next)
    {
        if (iter->next != os_alloc_end)
        {
            block->size += iter->next->size;
            block->next  = iter->next->next;
        }
        else
        {
            block->next = os_alloc_end;
        }
    }
    else
    {
        block->next = iter->next;
    }

    if (iter != block)
    {
        iter->next = block;
    }
}

#endif /* OS_CONFIG_ALLOC_ENABLE */
