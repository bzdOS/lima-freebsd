/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * lima_ccu_debug.h — see lima_ccu_debug.c for why this exists.
 *
 * One entry point, callable from the LinuxKPI side of hal/lima. The
 * implementation lives in its own translation unit with FreeBSD-only includes,
 * so this header deliberately declares nothing that would drag <vm/pmap.h> into
 * a LinuxKPI compilation unit.
 */
#ifndef _LIMA_CCU_DEBUG_H_
#define _LIMA_CCU_DEBUG_H_

/*
 * purpose:     Print the A64 CCU's GPU clock/reset bits as they actually are.
 * input:       when — short label identifying the call site.
 * output:      none (console).
 * sideEffects: maps/unmaps one device page; reads only, never writes.
 */
void lima_ccu_dump(const char *when);

/*
 * purpose:     Enable PLL_GPU and wait for lock, working around ccu_a64.c
 *              declaring pll_gpu's gate without AW_CLK_HAS_GATE (so clk(9)
 *              cannot enable it). Opt-in: `kenv hw.lima.force_pll_gpu=1`.
 * input:       none (reads the tunable itself).
 * output:      0 on success / tunable off; ETIMEDOUT if it never locks;
 *              ENXIO if the CCU cannot be mapped.
 * sideEffects: sets one bit in CCU+0x38 when enabled. See the definition for
 *              why this is a diagnostic and stopgap, not the real fix.
 */
int lima_ccu_force_pll_gpu(void);

#endif /* _LIMA_CCU_DEBUG_H_ */
