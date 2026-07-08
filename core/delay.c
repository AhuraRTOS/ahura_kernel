/**
 * @file delay.c
 * @brief Delay service implementation: blocking tick delays and precise busy-waits.
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

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#define OS_DELAY_US_PER_SECOND           1000000ULL
#define OS_DELAY_MS_PER_SECOND           1000ULL
#define OS_DELAY_MAX_CYCLE_CHUNK         0x40000000UL

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static os_status os_delay_ticks(uint32_t ticks);
static void      os_delay_cycle_wait(uint64_t cycle_count);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Delay current execution for the requested milliseconds.
 *
 * Blocks the calling task (yields the CPU) once the scheduler is running;
 * falls back to a busy-wait before os_start or from interrupt context.
 *
 * @param[in] milliseconds  Delay duration in milliseconds.
 * @return os_status        Status code.
 */
os_status os_delay_ms(uint32_t milliseconds)
{
    uint64_t ticks_u64 = ((uint64_t)milliseconds * (uint64_t)OS_CONFIG_TICK_HZ + (OS_DELAY_MS_PER_SECOND - 1ULL)) /
                         OS_DELAY_MS_PER_SECOND;

    if (ticks_u64 > (uint64_t)UINT32_MAX)
    {
        return OS_STATUS_INVALID_ARG;
    }

    return os_delay_ticks((uint32_t)ticks_u64);
}

/******************************************************************************************************/
/**
 * @brief Busy-wait for the requested microseconds (precise, does not yield).
 *
 * Uses the DWT cycle counter; intended for short, precise waits. Prefer
 * os_delay_ms for anything at or above the tick period.
 *
 * @param[in] microseconds  Delay duration in microseconds.
 * @return os_status        Status code.
 */
os_status os_delay_us(uint32_t microseconds)
{
    uint32_t clock_hz = os_clock_hz_get_cb();
    uint64_t cycle_count;

    if (microseconds == 0U)
    {
        return OS_STATUS_OK;
    }

    if (clock_hz == 0U)
    {
        return OS_STATUS_ERROR;
    }

    cycle_count = ((uint64_t)microseconds * (uint64_t)clock_hz + (OS_DELAY_US_PER_SECOND - 1ULL)) /
                  OS_DELAY_US_PER_SECOND;

    os_delay_cycle_wait(cycle_count);

    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Delay current execution for the requested seconds.
 *
 * @param[in] seconds  Delay duration in seconds.
 * @return os_status   Status code.
 */
os_status os_delay_s(uint32_t seconds)
{
    uint64_t ticks_u64 = (uint64_t)seconds * (uint64_t)OS_CONFIG_TICK_HZ;

    if (ticks_u64 > (uint64_t)UINT32_MAX)
    {
        return OS_STATUS_INVALID_ARG;
    }

    return os_delay_ticks((uint32_t)ticks_u64);
}

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Delay execution by scheduler ticks: block when possible, busy-wait otherwise.
 *
 * @param[in] ticks  Number of ticks to delay.
 * @return os_status Status code.
 */
static os_status os_delay_ticks(uint32_t ticks)
{
    uint32_t clock_hz;
    uint64_t cycle_count;

    if (ticks == 0U)
    {
        return OS_STATUS_OK;
    }

    /* Preferred path: yield the CPU to other tasks until the delay expires. */
    if (os_internal_can_block())
    {
        os_task_sleep_ticks(ticks);
        return OS_STATUS_OK;
    }

    /* Pre-scheduler or interrupt context: precise busy-wait. */
    clock_hz = os_clock_hz_get_cb();
    if (clock_hz == 0U)
    {
        return OS_STATUS_ERROR;
    }

    cycle_count = ((uint64_t)ticks * (uint64_t)clock_hz) / (uint64_t)OS_CONFIG_TICK_HZ;
    os_delay_cycle_wait(cycle_count);

    return OS_STATUS_OK;
}

/******************************************************************************************************/
/**
 * @brief Busy wait for a cycle-count duration using the DWT cycle counter.
 *
 * @param[in] cycle_count  Number of core cycles to wait.
 * @return None.
 */
static void os_delay_cycle_wait(uint64_t cycle_count)
{
    /* Chunked so 32-bit counter wraparound stays unambiguous. */
    while (cycle_count > 0ULL)
    {
        uint32_t chunk = (cycle_count > (uint64_t)OS_DELAY_MAX_CYCLE_CHUNK) ?
                         OS_DELAY_MAX_CYCLE_CHUNK : (uint32_t)cycle_count;
        uint32_t start = os_arch_cycle_count_get();

        while ((uint32_t)(os_arch_cycle_count_get() - start) < chunk)
        {
        }

        cycle_count -= (uint64_t)chunk;
    }
}
