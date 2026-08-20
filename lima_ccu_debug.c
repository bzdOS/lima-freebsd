// SPDX-License-Identifier: BSD-2-Clause
/*
 * lima_ccu_debug.c — read back the A64 CCU's GPU clock/reset bits.
 *
 * WHY THIS EXISTS
 *
 * `clk_enable()` returning 0 has meant nothing in this port. Three shims in
 * hal/lima were no-ops that reported success while doing nothing (linux/clk.h,
 * linux/regulator/consumer.h, lima_pmu.c), and each cost real debugging time
 * before the hardware disagreed. So when lima_clk_enable() says "bus clock ON /
 * core clock ON / reset DEASSERTED", that is a claim about a return value, not
 * an observation about the SoC.
 *
 * This reads the actual bits, straight out of the CCU, at the moment they are
 * supposed to be set. It answers the one question the stage bisect could not:
 * after the clock enables, is the GPU genuinely clocked, its PLL locked and its
 * reset released — or does the whole clk(9) path report success without
 * touching the hardware?
 *
 * It has to run here, inside the driver, rather than from the debug channel:
 * the enable is undone by lima_clk_disable() on any probe failure, so by the
 * time an external reader can look, the bits are clear again regardless of what
 * happened. And when probe does NOT fail, it stalls the SoC bus on the first
 * GPU read and takes the debug core with it. Either way the window is only
 * observable from inside. The console survives both: each byte is a synchronous
 * stage-2 fault captured in a ring the hypervisor now carries across a reload
 * (see microkernel `bzdctl.py console --postmortem`).
 *
 * Deliberately a separate translation unit with FreeBSD-only includes: the rest
 * of hal/lima compiles inside LinuxKPI's header universe, where <machine/pmap.h>
 * and friends collide. Nothing here needs LinuxKPI.
 *
 * READ-ONLY. It maps the CCU page, reads four registers and unmaps. It never
 * writes: 0x64 and 0x2c4 carry many unrelated peripherals' gates and resets,
 * and this file exists to observe the driver, not to substitute for it.
 *
 * Register offsets and bit positions are taken from FreeBSD's own
 * sys/dev/clk/allwinner/ccu_a64.c, which is the code that actually programs
 * them:
 *   FRAC_CLK(pll_gpu_clk, ... 0x38, ... 31, 28, 1000, AW_CLK_HAS_LOCK ...)
 *                                     gate 31, lock 28
 *   M_CLK(gpu_clk, ... 0x1A0, ... 31, AW_CLK_HAS_GATE)      gate 31
 *   CCU_GATE(CLK_BUS_GPU, "bus-gpu", "ahb1", 0x64, 20)
 *   CCU_RESET(RST_BUS_GPU, 0x2c4, 20)
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/kernel.h>		/* TUNABLE_INT_FETCH */

#include <machine/bus.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <machine/pmap.h>

#include "lima_ccu_debug.h"

#define A64_CCU_PA		0x01c20000UL
#define A64_CCU_SIZE		0x400UL

#define CCU_PLL_GPU_CTRL	0x038u
#define   PLL_GPU_ENABLE_BIT	31
#define   PLL_GPU_LOCK_BIT	28
#define CCU_GPU_CLK_REG		0x1A0u
#define   GPU_CLK_GATE_BIT	31
#define CCU_BUS_GATING1		0x064u
#define   BUS_GPU_GATE_BIT	20
#define CCU_BUS_SOFT_RST1	0x2C4u
#define   BUS_GPU_RST_BIT	20		/* 1 = reset deasserted */

/*
 * purpose:     Read the four CCU registers that govern the GPU's clocks and
 *              reset, and print them with each relevant bit decoded.
 * input:       when — short label for the call site, printed so several dumps
 *              in one attach can be told apart ("after-bus", "after-core", ...).
 * output:      none; everything goes to the console via printf().
 * sideEffects: maps and unmaps one device page. Reads only — no CCU register is
 *              written. Prints ~6 console lines, each of which is a synchronous
 *              stage-2 fault under the hypervisor, so the output is durable even
 *              if the very next instruction stalls the bus.
 */
void
lima_ccu_dump(const char *when)
{
	void *map;
	volatile uint8_t *ccu;
	uint32_t pll, gpu, gate, rst;

	map = pmap_mapdev(A64_CCU_PA, A64_CCU_SIZE);
	ccu = map;
	if (map == NULL) {
		printf("lima ccu[%s]: pmap_mapdev(0x%lx) failed\n", when,
		    A64_CCU_PA);
		return;
	}

	pll  = *(volatile uint32_t *)(ccu + CCU_PLL_GPU_CTRL);
	gpu  = *(volatile uint32_t *)(ccu + CCU_GPU_CLK_REG);
	gate = *(volatile uint32_t *)(ccu + CCU_BUS_GATING1);
	rst  = *(volatile uint32_t *)(ccu + CCU_BUS_SOFT_RST1);

	pmap_unmapdev(map, A64_CCU_SIZE);

	printf("lima ccu[%s]: PLL_GPU=0x%08x en=%u lock=%u\n", when, pll,
	    (pll >> PLL_GPU_ENABLE_BIT) & 1u, (pll >> PLL_GPU_LOCK_BIT) & 1u);
	printf("lima ccu[%s]: GPU_CLK=0x%08x gate=%u\n", when, gpu,
	    (gpu >> GPU_CLK_GATE_BIT) & 1u);
	printf("lima ccu[%s]: BUS_GATE1=0x%08x bus_gpu=%u\n", when, gate,
	    (gate >> BUS_GPU_GATE_BIT) & 1u);
	printf("lima ccu[%s]: BUS_RST1=0x%08x gpu_rst_released=%u\n", when, rst,
	    (rst >> BUS_GPU_RST_BIT) & 1u);
	printf("lima ccu[%s]: VERDICT pll=%u lock=%u core=%u bus=%u rel=%u\n",
	    when,
	    (pll >> PLL_GPU_ENABLE_BIT) & 1u, (pll >> PLL_GPU_LOCK_BIT) & 1u,
	    (gpu >> GPU_CLK_GATE_BIT) & 1u, (gate >> BUS_GPU_GATE_BIT) & 1u,
	    (rst >> BUS_GPU_RST_BIT) & 1u);
}

/*
 * purpose:     Enable PLL_GPU and wait for it to lock, working around clk(9)
 *              being unable to do it. Opt-in via `kenv hw.lima.force_pll_gpu=1`.
 * input:       none (reads the kenv tunable itself).
 * output:      0 if the PLL is enabled and locked (or was already, or the
 *              tunable is off); ETIMEDOUT if it enables but never locks; ENXIO
 *              if the CCU page cannot be mapped.
 * sideEffects: sets bit 31 of CCU+0x38 when the tunable is on. Read-modify-write
 *              of one bit; nothing but PLL_GPU lives in that register.
 *
 * WHY THIS EXISTS — and why it is NOT the real fix.
 *
 * Measured on this board: clk_enable(gpu) opens the GPU's core-clock gate
 * (CCU+0x1A0 bit 31) and the AHB gate (CCU+0x64 bit 20), but leaves PLL_GPU
 * (CCU+0x38 bit 31) OFF and unlocked. So the core clock is gated open onto a
 * dead PLL, and the GPU has no functional clock even though every clk(9) call
 * returned 0.
 *
 * clk(9) is not at fault for failing to try: clknode_enable() (sys/dev/clk/clk.c)
 * walks parents first, so aw_clk_frac_set_gate() WAS called on pll_gpu. It
 * returned 0 without touching the register, because
 * sys/dev/clk/allwinner/ccu_a64.c declares pll_gpu_clk with the gate bit (31)
 * and lock bit (28) filled in but with flags = AW_CLK_HAS_LOCK only — no
 * AW_CLK_HAS_GATE — and aw_clk_frac_set_gate()'s first statement is
 * `if ((sc->flags & AW_CLK_HAS_GATE) == 0) return (0);`.
 *
 * That omission is shared by every FRAC_CLK in ccu_a64.c (pll_video0, pll_ve,
 * pll_video1, pll_gpu, pll_hsic, pll_de) and in ccu_h3.c, so no fractional PLL
 * on these SoCs can be enabled through clk(9) at all. It goes unnoticed because
 * U-Boot leaves the video/de PLLs running; PLL_GPU is the one nobody turns on
 * before FreeBSD, because nothing but a GPU driver ever wants it.
 *
 * THE REAL FIX is one word in ccu_a64.c — add AW_CLK_HAS_GATE to pll_gpu_clk's
 * flags — and it belongs in the guest kernel, not here. This function exists to
 * prove the diagnosis end to end before paying for a kernel rebuild and
 * redeploy, and to keep the GPU usable until that lands. It is off by default
 * precisely so it cannot quietly become the fix.
 */
int
lima_ccu_force_pll_gpu(void)
{
	void *map;
	volatile uint32_t *reg;
	uint32_t v;
	int force = 0;
	int i;

	TUNABLE_INT_FETCH("hw.lima.force_pll_gpu", &force);
	if (!force)
		return (0);

	map = pmap_mapdev(A64_CCU_PA, A64_CCU_SIZE);
	if (map == NULL) {
		printf("lima ccu: force_pll_gpu: pmap_mapdev failed\n");
		return (ENXIO);
	}
	reg = (volatile uint32_t *)((volatile uint8_t *)map + CCU_PLL_GPU_CTRL);

	v = *reg;
	printf("lima ccu: force_pll_gpu: PLL_GPU=0x%08x en=%u lock=%u -> enabling\n",
	    v, (v >> PLL_GPU_ENABLE_BIT) & 1u, (v >> PLL_GPU_LOCK_BIT) & 1u);

	if (((v >> PLL_GPU_ENABLE_BIT) & 1u) == 0) {
		*reg = v | (1u << PLL_GPU_ENABLE_BIT);
		__asm__ volatile("dsb sy" ::: "memory");
	}

	/* ccu_a64.c's own FRAC_CLK declares 1000 lock retries; match that order
	 * of magnitude. A PLL that never locks must be reported, not assumed. */
	for (i = 0; i < 1000; i++) {
		v = *reg;
		if ((v >> PLL_GPU_LOCK_BIT) & 1u)
			break;
		DELAY(10);
	}

	v = *reg;
	pmap_unmapdev(map, A64_CCU_SIZE);

	printf("lima ccu: force_pll_gpu: PLL_GPU=0x%08x en=%u lock=%u after %d polls\n",
	    v, (v >> PLL_GPU_ENABLE_BIT) & 1u, (v >> PLL_GPU_LOCK_BIT) & 1u, i);

	if (((v >> PLL_GPU_LOCK_BIT) & 1u) == 0) {
		printf("lima ccu: force_pll_gpu: PLL never locked\n");
		return (ETIMEDOUT);
	}
	return (0);
}
