/**
 * @file os_arch_port.c
 * @brief Architecture port for ARM Cortex-M1: uses the shared ARMv6-M implementation.
 *        Note: SysTick is optional on this FPGA-oriented core; the kernel tick
 *        requires an implementation of it (or a replacement os_arch_tick_init).
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

#include "../common/os_arch_port_v6m.c"
