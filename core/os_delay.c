/**
 * @file os_delay.c
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

static os_status os_delay_forever(void);
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
 * OS_WAIT_FOREVER parks the calling task permanently (never returns).
 *
 * @param[in] milliseconds  Delay duration in milliseconds, or OS_WAIT_FOREVER.
 * @return os_status        Status code.
 */
os_status os_delay_ms(uint32_t milliseconds)
{
    uint64_t ticks_u64;

    if (milliseconds == OS_WAIT_FOREVER)
    {
        return os_delay_forever();
    }

    ticks_u64 = ((uint64_t)milliseconds * (uint64_t)OS_CONFIG_TICK_HZ + (OS_DELAY_MS_PER_SECOND - 1ULL)) /
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
 * OS_WAIT_FOREVER parks the calling task permanently (never returns).
 *
 * @param[in] seconds  Delay duration in seconds, or OS_WAIT_FOREVER.
 * @return os_status   Status code.
 */
os_status os_delay_s(uint32_t seconds)
{
    uint64_t ticks_u64;

    if (seconds == OS_WAIT_FOREVER)
    {
        return os_delay_forever();
    }

    ticks_u64 = (uint64_t)seconds * (uint64_t)OS_CONFIG_TICK_HZ;

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
 * @brief Park the calling task permanently (OS_WAIT_FOREVER delay). Never returns on success.
 *
 * @return os_status  INVALID_ARG when the caller cannot block (ISR or pre-scheduler).
 */
static os_status os_delay_forever(void)
{
    if (!os_internal_can_block())
    {
        /* A permanent busy-wait would hang the system: refuse instead. */
        return OS_STATUS_INVALID_ARG;
    }

    /* Re-arm on any spurious wake (forced os_task_wake aimed at a kernel
     * service task): forever really is forever. */
    while (1)
    {
        os_task_sleep_ticks(OS_WAIT_FOREVER);
    }
}

/******************************************************************************************************/
/**
 * @brief Delay execution by a finite number of scheduler ticks: block when possible,
 *        busy-wait otherwise.
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

    /* The callers route an intentional OS_WAIT_FOREVER to os_delay_forever
     * before converting, so the sentinel value can only be reached here as
     * a FINITE duration whose tick conversion collides with it numerically.
     * One tick short keeps it out of the sleep primitive's "until woken"
     * meaning at a cost of 1 tick in ~49 days (at 1 kHz). */
    if (ticks == OS_WAIT_FOREVER)
    {
        ticks--;
    }

    /* Preferred path: yield the CPU to other tasks until the delay expires.
     * The sleep is re-armed until the duration has really elapsed: a forced
     * os_task_wake aimed at a kernel service task (new work/timer expiry
     * while its handler delays) must not cut the delay short. */
    if (os_internal_can_block())
    {
        uint32_t start_tick = os_tick_get();
        uint32_t elapsed    = 0U;

        while (elapsed < ticks)
        {
            os_task_sleep_ticks(ticks - elapsed);
            elapsed = os_tick_get() - start_tick; /* wrap-safe unsigned diff */
        }

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
