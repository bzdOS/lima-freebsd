// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * lima_pmu.c — Mali-400/450 Power Management Unit (FreeBSD 15.1)
 *
 * PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_pmu.c
 *
 * ============================================================================
 * HISTORY: THIS FILE WAS A STUB, AND THAT IS WHY THE BOARD DIED (2026-08-11)
 * ============================================================================
 * Until 2026-08-11 lima_pmu_init() was `dev_dbg(...); return 0;` — 36 lines
 * that never touched a register. Its header comment said:
 *
 *     "On PinePhone Pro (A64) the PMU is controlled by the Allwinner CCU.
 *      Real register writes go here when the full port is done."
 *
 * That is wrong twice over. PinePhone *Pro* is RK3399/Mali-T860, not A64;
 * plain PinePhone and this board (Banana Pi M64) are A64/Mali-400. And the
 * Allwinner CCU does not control the Mali PMU at all: the CCU gates the GPU's
 * bus and core CLOCKS and holds its RESET, which is a different thing from the
 * power domains INSIDE the GPU. Those are switched by this block, at MMIO
 * offset 0x2000 of the Mali window, and nothing else on the SoC can switch
 * them.
 *
 * The consequence, traced on real hardware: with the clock and reset shims made
 * real (see linux/clk.h, linux/reset.h) the GPU is clocked and out of reset,
 * but with the PMU never programmed its GP/L2/PP/MMU domains stay powered
 * DOWN. lima_mmu_init()'s directory-table self-test is the first GPU access
 * that needs a bus response, and reading an unpowered block never gets one:
 *
 *     init_ip[0] pmu   off=0x2000 ...      <- the stub: returned without acting
 *     init_ip[1] gpmmu off=0x3000 ...
 *     mmu gpmmu: DTE write 0xCAFEBABE
 *     mmu gpmmu: DTE write returned, reading back
 *     <nothing, ever>
 *
 * The write completes because a write to Device memory is posted and retires
 * without waiting for the bus. The read must wait, so the read is where the CPU
 * stalls — and a stalled bus read on this SoC takes every core with it: no
 * panic, no backtrace, console output stopping mid-line, all four cores gone,
 * and the hardware watchdog eventually resetting the board to U-Boot. For a day
 * that was misattributed to "making the clocks real crashes the board". The
 * clocks were never the problem; making them real merely got execution far
 * enough to reach the first real GPU read.
 *
 * Three shims in this port shared one shape: a no-op whose comment asserted
 * that something else, elsewhere, did the work (clk.h: "FDT overlays and the
 * real CCU driver"; regulator/consumer.h: "managed by the PMIC driver at boot";
 * this file: "controlled by the Allwinner CCU"). Two of the three were false.
 * A stub that returns success is indistinguishable from a working
 * implementation until the hardware disagrees, and here the hardware's way of
 * disagreeing was to take the board down with no diagnostics at all.
 *
 * ============================================================================
 * WHAT THIS DOES NOW
 * ============================================================================
 * The upstream sequence, which is short: mask the PMU's command interrupt (we
 * poll instead of taking an IRQ — the handler is not registered at this point
 * in probe, and enabling a line whose source nobody has silenced is its own
 * hazard on this platform), write the power-up mask for every domain this GPU
 * variant has, then poll PMU_INT_RAWSTAT for the command-complete bit and
 * clear it.
 *
 * Mali-400 and Mali-450 differ in which mask bits exist: on Mali-400 each PP
 * has its own bit (LIMA_PMU_POWER_PP_MASK(i)) alongside GP0 and L2; on Mali-450
 * a domain brings its own L2 up automatically and the PPs are grouped
 * (PP0 / PP1-3 / PP4-7). Both encodings are in lima_regs.h; this picks by
 * ldev->id, same as upstream.
 *
 * Poll rather than sleep: this runs in probe context where a Mali PMU command
 * completes in microseconds, and lima_mmu.c's own send_command helpers use the
 * same busy-poll-with-timeout shape. A timeout is reported as -ETIMEDOUT and
 * fails probe, which is the right outcome — continuing past a GPU that did not
 * power up is exactly what produced the bus stall described above.
 */

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/errno.h>

#include "lima_device.h"
#include "lima_pmu.h"
#include "lima_regs.h"

/*
 * purpose:   32-bit MMIO accessors for the PMU block, relative to ip->iomem
 *            (which lima_device.c has already offset to the PMU's 0x2000).
 * input:     reg — offset from lima_regs.h's LIMA_PMU_*; data — value to write.
 * output:    pmu_read returns u32; pmu_write returns void.
 * sideEffects: touches GPU hardware. Requires the GPU to be clocked, out of
 *            reset, and — for anything other than this block itself — powered.
 */
#define pmu_write(reg, data)  writel(data, (uint8_t __iomem *)ip->iomem + (reg))
#define pmu_read(reg)         readl((uint8_t __iomem *)ip->iomem + (reg))

/* Upstream waits up to 100 attempts; a Mali PMU command is microseconds, so
 * this is already generous. Bounded so a dead PMU fails probe instead of
 * spinning forever in a context where spinning forever is a dead board. */
#define LIMA_PMU_CMD_POLLS	100

/*
 * purpose:   Issue one PMU power command and wait for it to complete.
 * input:     ip — the PMU lima_ip; reg — LIMA_PMU_POWER_UP or _POWER_DOWN;
 *            mask — domain bits for this GPU variant.
 * output:    0 on completion; -ETIMEDOUT if the command never reports done.
 * sideEffects: switches GPU internal power domains; writes PMU_INT_CLEAR.
 */
static int
lima_pmu_wait_cmd(struct lima_ip *ip)
{
	int i;

	for (i = 0; i < LIMA_PMU_CMD_POLLS; i++) {
		if (pmu_read(LIMA_PMU_INT_RAWSTAT) & LIMA_PMU_INT_CMD_MASK) {
			pmu_write(LIMA_PMU_INT_CLEAR, LIMA_PMU_INT_CMD_MASK);
			return 0;
		}
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int
lima_pmu_send_cmd(struct lima_ip *ip, u32 reg, u32 mask)
{
	pmu_write(reg, mask);
	return lima_pmu_wait_cmd(ip);
}


/*
 * purpose:   The set of power domains this GPU variant actually has, as a PMU
 *            mask. Cached in ip->data.mask by the callers.
 * input:     ip — the PMU lima_ip; reads dev->id and dev->ip[].present.
 * output:    mask for LIMA_PMU_POWER_UP / _POWER_DOWN.
 * sideEffects: none
 *
 * Built from which PP blocks are actually present rather than from num_pp,
 * which is still 0 during the IP-init loop (lima_init_pp_pipe() fills it in
 * later). An earlier version of this file iterated num_pp and therefore
 * produced mask=0x3 -- GP0 and L2 only -- on a GPU with two PPs.
 */
static u32
lima_pmu_get_ip_mask(struct lima_ip *ip)
{
	struct lima_device *dev = ip->dev;
	u32 ret = LIMA_PMU_POWER_GP0_MASK;
	int i;

	if (dev->id == lima_gpu_mali400) {
		ret |= LIMA_PMU_POWER_L2_MASK;
		for (i = 0; i < 4; i++) {
			if (dev->ip[lima_ip_pp0 + i].present)
				ret |= LIMA_PMU_POWER_PP_MASK(i);
		}
	} else {
		if (dev->ip[lima_ip_pp0].present)
			ret |= LIMA450_PMU_POWER_PP0_MASK;
		for (i = lima_ip_pp1; i <= lima_ip_pp3; i++)
			if (dev->ip[i].present) {
				ret |= LIMA450_PMU_POWER_PP13_MASK;
				break;
			}
		for (i = lima_ip_pp4; i <= lima_ip_pp7; i++)
			if (dev->ip[i].present) {
				ret |= LIMA450_PMU_POWER_PP47_MASK;
				break;
			}
	}
	return ret;
}

/*
 * purpose:   Power up every GPU domain that is currently off, so the rest of
 *            probe can reach GP, PP, L2 and the MMUs. THE FIRST THING THAT MUST
 *            HAPPEN after the GPU is clocked and out of reset — see this file's
 *            header for what happens when it does not.
 * input:     ip — the PMU lima_ip (iomem already offset to 0x2000).
 * output:    0 on success; -ETIMEDOUT if the power-up command never completes.
 * sideEffects: masks the PMU command interrupt; sets PMU_SW_DELAY; powers on
 *            whichever domains PMU_STATUS reports as off; clears
 *            PMU_INT_RAWSTAT's command bit.
 *
 * The mask comes from PMU_STATUS (1 = off, 0 = on) rather than being computed
 * from the GPU variant, which is upstream's approach and avoids a real trap: at
 * this point in probe ldev->num_pp is still 0 — it is filled in later by
 * lima_init_pp_pipe(), after this IP loop — so any mask built by iterating PPs
 * powers up only GP0 and L2 and silently leaves every pixel processor off. The
 * first cut of this file did exactly that and produced mask=0x3 on a GPU with
 * two PPs.
 */
int lima_pmu_init(struct lima_ip *ip)
{
	struct lima_device *ldev = ip->dev;
	u32 stat;
	int err;

	/*
	 * Mask the PMU's own command interrupt before issuing anything. We poll
	 * for completion; the handler is not registered at this point in probe,
	 * and on this platform enabling an interrupt line whose source nobody has
	 * silenced is a hazard in its own right (see lima_mmu_init()).
	 */
	pmu_write(LIMA_PMU_INT_MASK, 0);

	/*
	 * Not optional, and not obvious: upstream's comment is "if this value is
	 * too low, when in high GPU clk freq, GPU will be in unstable state". It is
	 * the PMU's internal power-switch settling delay, and 0xffff is what every
	 * Mali-400 platform ships.
	 */
	pmu_write(LIMA_PMU_SW_DELAY, 0xffff);

	stat = pmu_read(LIMA_PMU_STATUS);
	dev_info(ldev->dev, "pmu: status=0x%08x (1=off per domain)\n", stat);
	if (stat == 0) {
		dev_info(ldev->dev, "pmu: all domains already powered\n");
		return 0;
	}

	err = lima_pmu_send_cmd(ip, LIMA_PMU_POWER_UP, stat);
	if (err) {
		dev_err(ldev->dev, "pmu: power-up command timed out\n");
		return err;
	}

	dev_info(ldev->dev, "pmu: domains powered, status=0x%08x\n",
		 pmu_read(LIMA_PMU_STATUS));
	return 0;
}

/*
 * purpose:   Power the GPU's domains back down on driver teardown.
 * input:     ip — the PMU lima_ip.
 * output:    none
 * sideEffects: powers off GP/L2/PP domains. A timeout is logged and otherwise
 *            ignored: fini has no way to report failure and no caller that
 *            could act on it, and leaving the domains on is not harmful.
 */
void lima_pmu_fini(struct lima_ip *ip)
{
	u32 stat;

	if (!ip->data.mask)
		ip->data.mask = lima_pmu_get_ip_mask(ip);

	stat = ~pmu_read(LIMA_PMU_STATUS) & ip->data.mask;
	if (stat == 0)
		return;

	pmu_write(LIMA_PMU_POWER_DOWN, stat);

	/*
	 * DO NOT wait for the command interrupt on Mali-400 when the domains
	 * are being powered OFF: the hardware does not generate one in that
	 * case. Upstream documents this explicitly, and the first version of
	 * this file ignored it and polled anyway -- which produced exactly
	 * "pmu: power-down command timed out" on every kldunload on this board,
	 * observed live before this was fixed. Clear the latch and move on.
	 */
	if (ip->dev->id == lima_gpu_mali400)
		pmu_write(LIMA_PMU_INT_CLEAR, LIMA_PMU_INT_CMD_MASK);
	else if (lima_pmu_wait_cmd(ip) != 0)
		dev_err(ip->dev->dev, "pmu: power-down command timed out\n");
}

/*
 * purpose:   Re-power the domains after a runtime/system suspend.
 * input:     ip — the PMU lima_ip.
 * output:    0 on success; negative errno from lima_pmu_init().
 * sideEffects: as lima_pmu_init().
 */
int lima_pmu_resume(struct lima_ip *ip)
{
	return lima_pmu_init(ip);
}

/*
 * purpose:   Power the domains down for suspend.
 * input:     ip — the PMU lima_ip.
 * output:    none
 * sideEffects: as lima_pmu_fini().
 */
void lima_pmu_suspend(struct lima_ip *ip)
{
	lima_pmu_fini(ip);
}
