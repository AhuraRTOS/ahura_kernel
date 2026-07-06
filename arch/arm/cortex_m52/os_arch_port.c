/**
 * @file os_arch_port.c
 * @brief Architecture port for ARM Cortex-M52: uses the shared ARMv7-M/ARMv8-M
 *        mainline implementation. Helium (MVE) needs no extra handling: the
 *        callee-saved vector registers Q4-Q7 alias s16-s31 (already saved) and
 *        the hardware lazy-stacks s0-s15/FPSCR/VPR in the extended frame.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

#include "../common/os_arch_port_v7m.c"
