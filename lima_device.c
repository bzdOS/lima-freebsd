// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright 2017-2019 Qiang Yu <yuq825@gmail.com>
 * Copyright 2024 bsdOS Project (FreeBSD port)
 *
 * MODULE: hal/lima/lima_device.c
 * PURPOSE: Mali-400/450 IP block lifecycle — init, fini, resume, suspend for
 *          all sub-IPs (GP, PP0-7, L2 cache, MMU, PMU, DLBU, bcast) and the
 *          two scheduler pipes (GP pipe, PP pipe).
 * PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_device.c
 *
 * FreeBSD porting notes (drm-66-kmod / LinuxKPI):
 *   - clk_*, reset_*, devm_*, platform_*, dma_*  → provided by linuxkpi
 *   - regulator_* → provided by linuxkpi regulator compat layer
 *   - list_for_each_entry_safe / mutex_* → linuxkpi (sys/linux/linux_compat.h)
 *   - dma_alloc_wc / dma_free_wc → linuxkpi DMA helpers
 *   - dev_err / dev_info → linuxkpi device.h
 *   - No Linux IOMMU framework used; Mali-400 has its own internal MMU (lima_mmu.c)
 *   - LIMA_MMIO_BASE / LIMA_MMIO_SIZE from ../mali_uio.h (A64: 0x01C40000, 64 KB)
 */

#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>

#include "lima_device.h"
#include "lima_gp.h"
#include "lima_pp.h"
#include "lima_mmu.h"
#include "lima_pmu.h"
#include "lima_l2_cache.h"
#include "lima_dlbu.h"
#include "lima_bcast.h"
#include "lima_vm.h"
#include "lima_freebsd_compat.h"
#include "lima_ccu_debug.h"

/*
 * ============================================================================
 * BISECT KNOB: hw.lima.clk_stage
 * ============================================================================
 * lima_clk_enable() performs three distinct hardware operations in sequence:
 * enable the GPU bus clock, enable the GPU core clock, deassert the GPU reset.
 * Until 2026-08-11 all three were no-ops (linux/clk.h and linux/reset.h were
 * stubs, see those files' headers), and probe failed cleanly with the guest and
 * the whole board unharmed:
 *
 *     lima_platform_driver0: mmu gpmmu dte write test fail
 *     lima_platform_driver0: probe failed: -5
 *
 * With all three made real, kldload of this module takes the ENTIRE BOARD down
 * to U-Boot -- not just the guest, the hypervisor with it. Reproduced twice.
 * That is a qualitatively worse failure than the one the fix was meant to
 * address, and "one of these three operations is fatal" is as much as the
 * evidence supports. This knob is how to find out which, one step at a time,
 * instead of guessing:
 *
 *     0  enable nothing        -- must reproduce the OLD clean failure exactly.
 *                                 If it does not, the fatal change is somewhere
 *                                 other than these three operations and the
 *                                 whole hypothesis is wrong.
 *     1  bus clock only
 *     2  bus + core clock
 *     3  bus + core + reset deassert   (default: the real, full sequence)
 *
 * Read from the kernel environment at attach time, so it is settable from the
 * running guest with no rebuild and no reboot:
 *
 *     kenv hw.lima.clk_stage=1 && kldload lima
 *
 * Each stage announces itself on the console before acting, and the console is
 * captured into a ring that now survives the reload a whole-board crash forces
 * (bzdctl.py console --postmortem) -- which is what makes this bisect readable
 * at all. Without that, every stage that kills the board would look identical
 * from outside.
 *
 * This is deliberately NOT temporary debug scaffolding to be ripped out: an
 * un-bisectable three-in-one hardware sequence is exactly what cost this
 * investigation a day, and the knob defaults to the full sequence, so leaving
 * it in costs a single kenv read per attach.
 */
#define LIMA_CLK_STAGE_NONE	0
#define LIMA_CLK_STAGE_BUS	1
#define LIMA_CLK_STAGE_CORE	2
#define LIMA_CLK_STAGE_FULL	3

/*
 * purpose:  Read the hw.lima.clk_stage bisect knob from the kernel environment.
 * input:    none (reads kenv "hw.lima.clk_stage")
 * output:   LIMA_CLK_STAGE_NONE..LIMA_CLK_STAGE_FULL; LIMA_CLK_STAGE_FULL when
 *           the variable is unset or out of range, so an absent/typo'd knob can
 *           never silently disable the real clock enable.
 * sideEffects: none
 */
static int
lima_clk_stage(void)
{
	int stage = LIMA_CLK_STAGE_FULL;

	TUNABLE_INT_FETCH("hw.lima.clk_stage", &stage);
	if (stage < LIMA_CLK_STAGE_NONE || stage > LIMA_CLK_STAGE_FULL)
		stage = LIMA_CLK_STAGE_FULL;
	return (stage);
}

/*
 * struct lima_ip_desc
 *
 * purpose:  Static per-IP descriptor: name, IRQ name, per-GPU-variant
 *           presence flags, register offsets, and lifecycle callbacks.
 * input:    indexed by enum lima_ip_id; populated by LIMA_IP_DESC macro
 * output:   used by lima_init_ip / lima_fini_ip / lima_resume_ip /
 *           lima_suspend_ip to dispatch to the correct IP handler
 * sideEffects: none — read-only after module load
 */
struct lima_ip_desc {
	char *name;
	char *irq_name;
	bool must_have[lima_gpu_num];
	int offset[lima_gpu_num];

	int (*init)(struct lima_ip *ip);
	void (*fini)(struct lima_ip *ip);
	int (*resume)(struct lima_ip *ip);
	void (*suspend)(struct lima_ip *ip);
};

/*
 * LIMA_IP_DESC — convenience macro to build a lima_ip_desc entry.
 *
 * purpose:  Reduce verbosity of the static IP descriptor table; each
 *           expansion fully populates one lima_ip_desc for ipname.
 * input:    ipname — suffix used to resolve enum and function names
 *           mst0/mst1 — must_have flags for Mali-400 and Mali-450
 *           off0/off1 — MMIO offsets for Mali-400 and Mali-450 (-1 = absent)
 *           func — function prefix (resolves to lima_##func##_{init,fini,...})
 *           irq  — IRQ resource name string or NULL
 * output:   designated initialiser for lima_ip_desc[lima_ip_##ipname]
 * sideEffects: none
 */
#define LIMA_IP_DESC(ipname, mst0, mst1, off0, off1, func, irq) \
	[lima_ip_##ipname] = { \
		.name = #ipname, \
		.irq_name = irq, \
		.must_have = { \
			[lima_gpu_mali400] = mst0, \
			[lima_gpu_mali450] = mst1, \
		}, \
		.offset = { \
			[lima_gpu_mali400] = off0, \
			[lima_gpu_mali450] = off1, \
		}, \
		.init    = lima_##func##_init, \
		.fini    = lima_##func##_fini, \
		.resume  = lima_##func##_resume, \
		.suspend = lima_##func##_suspend, \
	}

/*
 * lima_ip_desc[] — static descriptor table for all Mali-400/450 IP blocks.
 *
 * purpose:  Central registry mapping each lima_ip_id to its MMIO offset
 *           (per GPU variant), IRQ name, presence requirement, and
 *           lifecycle function pointers.
 * input:    indexed by lima_ip_id enum (0 .. lima_ip_num-1)
 * output:   consumed by lima_{init,fini,resume,suspend}_ip
 * sideEffects: none — read-only data; initialised at compile time
 *
 * Allwinner A64 (PinePhone Pro / Squirrel v0.1.x) is Mali-400 MP2:
 *   only mali400 column applies; mali450 entries with offset -1 are skipped.
 * MMIO base: MALI_MMIO_BASE (0x01C40000) from ../mali_uio.h.
 * Each ip->iomem = dev->iomem + offset[dev->id].
 */
static struct lima_ip_desc lima_ip_desc[lima_ip_num] = {
	LIMA_IP_DESC(pmu,         false, false, 0x02000, 0x02000, pmu,      "pmu"),
	LIMA_IP_DESC(l2_cache0,   true,  true,  0x01000, 0x10000, l2_cache, NULL),
	LIMA_IP_DESC(l2_cache1,   false, true,  -1,      0x01000, l2_cache, NULL),
	LIMA_IP_DESC(l2_cache2,   false, false, -1,      0x11000, l2_cache, NULL),
	LIMA_IP_DESC(gp,          true,  true,  0x00000, 0x00000, gp,       "gp"),
	LIMA_IP_DESC(pp0,         true,  true,  0x08000, 0x08000, pp,       "pp0"),
	LIMA_IP_DESC(pp1,         false, false, 0x0A000, 0x0A000, pp,       "pp1"),
	LIMA_IP_DESC(pp2,         false, false, 0x0C000, 0x0C000, pp,       "pp2"),
	LIMA_IP_DESC(pp3,         false, false, 0x0E000, 0x0E000, pp,       "pp3"),
	LIMA_IP_DESC(pp4,         false, false, -1,      0x28000, pp,       "pp4"),
	LIMA_IP_DESC(pp5,         false, false, -1,      0x2A000, pp,       "pp5"),
	LIMA_IP_DESC(pp6,         false, false, -1,      0x2C000, pp,       "pp6"),
	LIMA_IP_DESC(pp7,         false, false, -1,      0x2E000, pp,       "pp7"),
	LIMA_IP_DESC(gpmmu,       true,  true,  0x03000, 0x03000, mmu,      "gpmmu"),
	LIMA_IP_DESC(ppmmu0,      true,  true,  0x04000, 0x04000, mmu,      "ppmmu0"),
	LIMA_IP_DESC(ppmmu1,      false, false, 0x05000, 0x05000, mmu,      "ppmmu1"),
	LIMA_IP_DESC(ppmmu2,      false, false, 0x06000, 0x06000, mmu,      "ppmmu2"),
	LIMA_IP_DESC(ppmmu3,      false, false, 0x07000, 0x07000, mmu,      "ppmmu3"),
	LIMA_IP_DESC(ppmmu4,      false, false, -1,      0x1C000, mmu,      "ppmmu4"),
	LIMA_IP_DESC(ppmmu5,      false, false, -1,      0x1D000, mmu,      "ppmmu5"),
	LIMA_IP_DESC(ppmmu6,      false, false, -1,      0x1E000, mmu,      "ppmmu6"),
	LIMA_IP_DESC(ppmmu7,      false, false, -1,      0x1F000, mmu,      "ppmmu7"),
	LIMA_IP_DESC(dlbu,        false, true,  -1,      0x14000, dlbu,     NULL),
	LIMA_IP_DESC(bcast,       false, true,  -1,      0x13000, bcast,    NULL),
	LIMA_IP_DESC(pp_bcast,    false, true,  -1,      0x16000, pp_bcast, "pp"),
	LIMA_IP_DESC(ppmmu_bcast, false, true,  -1,      0x15000, mmu,      NULL),
};

/*
 * lima_ip_name
 *
 * purpose:  Return the human-readable name of an IP block (e.g. "gp", "pp0").
 * input:    ip — pointer to a lima_ip whose .id indexes lima_ip_desc[]
 * output:   pointer to static string; caller must not free
 * sideEffects: none
 */
const char *lima_ip_name(struct lima_ip *ip)
{
	return lima_ip_desc[ip->id].name;
}

/* ------------------------------------------------------------------ */
/*  Clock and reset helpers                                            */
/* ------------------------------------------------------------------ */

/*
 * lima_clk_enable
 *
 * purpose:  Prepare and enable both bus clock and GPU core clock, then
 *           deassert the reset line if one is registered.
 * input:    dev — lima_device whose clk_bus, clk_gpu, and reset are set
 * output:   0 on success; negative errno on failure (clocks disabled on err)
 * sideEffects: clk_bus and clk_gpu transition to enabled; reset deasserted
 *
 * FreeBSD/linuxkpi: clk_prepare_enable and reset_control_deassert are
 * provided by linuxkpi clk.h and reset.h shims.
 */
static int lima_clk_enable(struct lima_device *dev)
{
	int err;
	int stage = lima_clk_stage();

	dev_info(dev->dev, "clk_enable: stage=%d (%s)\n", stage,
		 stage >= LIMA_CLK_STAGE_FULL ? "bus+core+reset" :
		 stage == LIMA_CLK_STAGE_CORE ? "bus+core, reset SKIPPED" :
		 stage == LIMA_CLK_STAGE_BUS  ? "bus only, core+reset SKIPPED" :
		 "NOTHING enabled (reproduces the pre-2026-08-11 stubs)");

	/*
	 * Dump the real CCU bits around every step. "bus clock ON" above is a
	 * statement about clk_prepare_enable()'s return value; these lines are
	 * what the SoC actually has. In this port that distinction has mattered
	 * three separate times — see lima_ccu_debug.c.
	 */
	lima_ccu_dump("entry");

	if (stage < LIMA_CLK_STAGE_BUS)
		return 0;

	err = clk_prepare_enable(dev->clk_bus);
	if (err)
		return err;
	dev_info(dev->dev, "clk_enable: bus clock ON\n");
	lima_ccu_dump("after-bus");

	if (stage < LIMA_CLK_STAGE_CORE)
		return 0;

	/* PLL_GPU must be running before its child core-clock gate is of any
	 * use. clk(9) cannot enable it on this SoC -- see
	 * lima_ccu_force_pll_gpu() for the exact reason -- so this is the
	 * opt-in stopgap that makes the core clock real. */
	err = lima_ccu_force_pll_gpu();
	if (err) {
		dev_err(dev->dev, "PLL_GPU could not be enabled: %d\n", err);
		goto error_out0;
	}

	err = clk_prepare_enable(dev->clk_gpu);
	if (err)
		goto error_out0;
	dev_info(dev->dev, "clk_enable: core clock ON\n");
	lima_ccu_dump("after-core");

	if (stage < LIMA_CLK_STAGE_FULL)
		return 0;

	if (dev->reset) {
		err = reset_control_deassert(dev->reset);
		if (err) {
			dev_err(dev->dev,
				"reset controller deassert failed %d\n", err);
			goto error_out1;
		}
		dev_info(dev->dev, "clk_enable: reset DEASSERTED\n");
	}
	lima_ccu_dump("after-reset");

	return 0;

error_out1:
	clk_disable_unprepare(dev->clk_gpu);
error_out0:
	clk_disable_unprepare(dev->clk_bus);
	return err;
}

/*
 * lima_clk_disable
 *
 * purpose:  Assert the reset line (if present) then disable both clocks.
 * input:    dev — lima_device with clk_bus, clk_gpu, reset populated
 * output:   void
 * sideEffects: clocks gated; reset asserted
 */
static void lima_clk_disable(struct lima_device *dev)
{
	if (dev->reset)
		reset_control_assert(dev->reset);
	clk_disable_unprepare(dev->clk_gpu);
	clk_disable_unprepare(dev->clk_bus);
}

/*
 * lima_clk_init
 *
 * purpose:  Obtain devm-managed handles to the bus clock, GPU core clock,
 *           and optional shared reset controller, then enable them.
 * input:    dev — lima_device; dev->dev must be the platform device's
 *           struct device (FDT-sourced clock names: "bus", "core")
 * output:   0 on success; negative errno on failure
 * sideEffects: dev->clk_bus, dev->clk_gpu, dev->reset populated;
 *              clocks enabled via lima_clk_enable
 *
 * FreeBSD/linuxkpi note: devm_clk_get and
 * devm_reset_control_array_get_optional_shared are provided by linuxkpi.
 * On FreeBSD 15.1 these forward to the FDT clock and reset frameworks;
 * the DT node for Allwinner A64 Mali-400 must export clocks named
 * "bus" (clock-names) and "core" matching the A64 CCU bindings.
 */
static int lima_clk_init(struct lima_device *dev)
{
	int err;

	dev->clk_bus = devm_clk_get(dev->dev, "bus");
	if (IS_ERR(dev->clk_bus)) {
		err = PTR_ERR(dev->clk_bus);
		if (err != -EPROBE_DEFER)
			dev_err(dev->dev, "get bus clk failed %d\n", err);
		dev->clk_bus = NULL;
		return err;
	}

	dev->clk_gpu = devm_clk_get(dev->dev, "core");
	if (IS_ERR(dev->clk_gpu)) {
		err = PTR_ERR(dev->clk_gpu);
		if (err != -EPROBE_DEFER)
			dev_err(dev->dev, "get core clk failed %d\n", err);
		dev->clk_gpu = NULL;
		return err;
	}

	dev->reset = devm_reset_control_array_get_optional_shared(dev->dev);
	if (IS_ERR(dev->reset)) {
		err = PTR_ERR(dev->reset);
		if (err != -EPROBE_DEFER)
			dev_err(dev->dev,
				"get reset controller failed %d\n", err);
		dev->reset = NULL;
		return err;
	}

	return lima_clk_enable(dev);
}

/*
 * lima_clk_fini
 *
 * purpose:  Disable clocks obtained in lima_clk_init (devm handles freed
 *           automatically on device removal; this only gates the clocks).
 * input:    dev — lima_device with clk_bus/clk_gpu/reset set
 * output:   void
 * sideEffects: clocks disabled; reset asserted
 */
static void lima_clk_fini(struct lima_device *dev)
{
	lima_clk_disable(dev);
}

/* ------------------------------------------------------------------ */
/*  Regulator helpers                                                  */
/* ------------------------------------------------------------------ */

/*
 * lima_regulator_enable
 *
 * purpose:  Enable the "mali" regulator if one was found during init.
 * input:    dev — lima_device; dev->regulator may be NULL (optional supply)
 * output:   0 on success or if no regulator; negative errno on failure
 * sideEffects: regulator output enabled
 *
 * FreeBSD/linuxkpi: regulator_enable provided by linuxkpi regulator shim.
 */
static int lima_regulator_enable(struct lima_device *dev)
{
	int ret;

	if (!dev->regulator)
		return 0;

	ret = regulator_enable(dev->regulator);
	if (ret < 0) {
		dev_err(dev->dev, "failed to enable regulator: %d\n", ret);
		return ret;
	}

	return 0;
}

/*
 * lima_regulator_disable
 *
 * purpose:  Disable the "mali" regulator if one is active.
 * input:    dev — lima_device
 * output:   void
 * sideEffects: regulator output disabled
 */
static void lima_regulator_disable(struct lima_device *dev)
{
	if (dev->regulator)
		regulator_disable(dev->regulator);
}

/*
 * lima_regulator_init
 *
 * purpose:  Obtain an optional devm-managed handle to the "mali" supply
 *           regulator from device tree, then enable it.
 * input:    dev — lima_device; DT property "mali-supply" is optional
 * output:   0 on success or if regulator absent (-ENODEV treated as absent);
 *           negative errno for real errors
 * sideEffects: dev->regulator populated; regulator enabled if present
 */
static int lima_regulator_init(struct lima_device *dev)
{
	int ret;

	dev->regulator = devm_regulator_get_optional(dev->dev, "mali");
	if (IS_ERR(dev->regulator)) {
		ret = PTR_ERR(dev->regulator);
		dev->regulator = NULL;
		if (ret == -ENODEV)
			return 0;
		if (ret != -EPROBE_DEFER)
			dev_err(dev->dev, "failed to get regulator: %d\n", ret);
		return ret;
	}

	return lima_regulator_enable(dev);
}

/*
 * lima_regulator_fini
 *
 * purpose:  Disable regulator on device removal.
 * input:    dev — lima_device
 * output:   void
 * sideEffects: regulator output disabled; devm frees handle
 */
static void lima_regulator_fini(struct lima_device *dev)
{
	lima_regulator_disable(dev);
}

/* ------------------------------------------------------------------ */
/*  Per-IP lifecycle dispatch                                          */
/* ------------------------------------------------------------------ */

/*
 * lima_init_ip
 *
 * purpose:  Initialise one Mali IP block: map its MMIO sub-window, acquire
 *           its IRQ (if named), then call the IP's own init callback.
 *           If the IP is optional for this GPU variant, failures are silent.
 * input:    dev   — lima_device with iomem and id set
 *           index — enum lima_ip_id value
 * output:   0 on success or on absent/optional IP; negative errno only when
 *           a must-have IP fails to initialise
 * sideEffects: dev->ip[index].iomem, .irq, .present set on success
 *
 * FreeBSD/linuxkpi note: platform_get_irq_byname and
 * platform_get_irq_byname_optional resolve IRQ resources registered via
 * the FDT interrupt-names property.  The A64 DT node supplies names
 * "gp", "gpmmu", "pp0", "ppmmu0", "pmu" matching the irq_name fields.
 */
static int lima_init_ip(struct lima_device *dev, int index)
{
	struct platform_device *pdev = to_platform_device(dev->dev);
	struct lima_ip_desc *desc = lima_ip_desc + index;
	struct lima_ip *ip = dev->ip + index;
	const char *irq_name = desc->irq_name;
	int offset = desc->offset[dev->id];
	bool must = desc->must_have[dev->id];
	int err;

	if (offset < 0)
		return 0;

	ip->dev = dev;
	ip->id  = index;
	ip->iomem = (void __iomem *)((uint8_t __iomem *)dev->iomem + offset);

	if (irq_name) {
		err = must ? platform_get_irq_byname(pdev, irq_name) :
			     platform_get_irq_byname_optional(pdev, irq_name);
		if (err < 0)
			goto out;
		ip->irq = err;
	}

	/*
	 * Announce each IP before touching it.  desc->init() is where this driver
	 * first reads and writes real GPU registers, and on the A64 under the
	 * bzdOS hypervisor a bad access there does not fail — it stalls the SoC
	 * bus, taking every core down with no panic and no backtrace, until the
	 * hardware watchdog resets the board.  When that happens the last line in
	 * the console ring is the only evidence of where it got to, and without
	 * this line there is nothing between "reset DEASSERTED" and silence.
	 *
	 * Console writes here are synchronous stage-2 faults into EL2 (each byte
	 * is captured before the next instruction retires), so the last line
	 * printed really is the last thing that executed — which is what makes
	 * this usable as a bisect.
	 */
	dev_info(dev->dev, "init_ip[%d] %s off=0x%x irq=%d\n", index,
		 desc->name ? desc->name : "?", offset,
		 irq_name ? ip->irq : -1);

	err = desc->init(ip);
	if (!err) {
		ip->present = true;
		return 0;
	}

out:
	return must ? err : 0;
}

/*
 * lima_fini_ip
 *
 * purpose:  Call the IP's fini callback if the IP was successfully
 *           initialised (ip->present).
 * input:    ldev  — lima_device
 *           index — enum lima_ip_id value
 * output:   void
 * sideEffects: IP hardware placed in quiescent state; ip->present unchanged
 */
static void lima_fini_ip(struct lima_device *ldev, int index)
{
	struct lima_ip_desc *desc = lima_ip_desc + index;
	struct lima_ip *ip = ldev->ip + index;

	if (ip->present)
		desc->fini(ip);
}

/*
 * lima_resume_ip
 *
 * purpose:  Re-initialise one IP block after system resume.
 * input:    ldev  — lima_device
 *           index — enum lima_ip_id value
 * output:   0 on success or if IP absent; negative errno on failure
 * sideEffects: IP registers restored to operating state
 */
static int lima_resume_ip(struct lima_device *ldev, int index)
{
	struct lima_ip_desc *desc = lima_ip_desc + index;
	struct lima_ip *ip = ldev->ip + index;
	int ret = 0;

	if (ip->present)
		ret = desc->resume(ip);

	return ret;
}

/*
 * lima_suspend_ip
 *
 * purpose:  Quiesce one IP block before system suspend.
 * input:    ldev  — lima_device
 *           index — enum lima_ip_id value
 * output:   void
 * sideEffects: IP placed in low-power state
 */
static void lima_suspend_ip(struct lima_device *ldev, int index)
{
	struct lima_ip_desc *desc = lima_ip_desc + index;
	struct lima_ip *ip = ldev->ip + index;

	if (ip->present)
		desc->suspend(ip);
}

/* ------------------------------------------------------------------ */
/*  Scheduler pipe setup                                               */
/* ------------------------------------------------------------------ */

/*
 * lima_init_gp_pipe
 *
 * purpose:  Construct the GP (geometry processor) scheduler pipe:
 *           attach L2 cache, GPMMU, and GP processor; then call the
 *           GP-specific pipe init for fence/queue setup.
 * input:    dev — fully probed lima_device with ip[] populated
 * output:   0 on success; negative errno on failure (pipe torn down on err)
 * sideEffects: dev->pipe[lima_pipe_gp] populated; DRM scheduler started
 */
static int lima_init_gp_pipe(struct lima_device *dev)
{
	struct lima_sched_pipe *pipe = dev->pipe + lima_pipe_gp;
	int err;

	pipe->ldev = dev;

	err = lima_sched_pipe_init(pipe, "gp");
	if (err)
		return err;

	pipe->l2_cache[pipe->num_l2_cache++] = dev->ip + lima_ip_l2_cache0;
	pipe->mmu[pipe->num_mmu++]           = dev->ip + lima_ip_gpmmu;
	pipe->processor[pipe->num_processor++] = dev->ip + lima_ip_gp;

	err = lima_gp_pipe_init(dev);
	if (err) {
		lima_sched_pipe_fini(pipe);
		return err;
	}

	return 0;
}

/*
 * lima_fini_gp_pipe
 *
 * purpose:  Tear down the GP scheduler pipe and release associated resources.
 * input:    dev — lima_device
 * output:   void
 * sideEffects: GP DRM scheduler stopped; pipe resources freed
 *
 * Ordering note: lima_sched_pipe_fini() (stops/drains the DRM scheduler,
 * including reaping any job sitting in its pending_list via
 * drm_sched_stop()) MUST run before lima_gp_pipe_fini() (which destroys the
 * task_slab kmem_cache those jobs are allocated from). The reverse order —
 * matching upstream Linux's lima_fini_gp_pipe(), which this port originally
 * copied verbatim — races kmem_cache_destroy() against the scheduler
 * kthread's own kmem_cache_free() calls, since drm_sched_fini() does not
 * stop that kthread until later. See lima_sched_pipe_fini() in
 * lima_sched.c for the full analysis; this is the direct cause of the
 * "Freed UMA keg (lima_pp_task) was not empty" warning observed on unload.
 */
static void lima_fini_gp_pipe(struct lima_device *dev)
{
	struct lima_sched_pipe *pipe = dev->pipe + lima_pipe_gp;

	lima_sched_pipe_fini(pipe);
	lima_gp_pipe_fini(dev);
}

/*
 * lima_init_pp_pipe
 *
 * purpose:  Construct the PP (pixel processor) scheduler pipe:
 *           walk the PP0-PP7 / PPMMU0-PPMMU7 pairs, attach those whose
 *           hardware is present, wire the correct L2 cache (one per 4 PPs
 *           on Mali-450; single shared on Mali-400), and set up the
 *           broadcast processor if present (Mali-450 only).
 * input:    dev — fully probed lima_device with ip[] populated
 * output:   0 on success; negative errno on failure (pipe torn down on err)
 * sideEffects: dev->pipe[lima_pipe_pp] populated; DRM scheduler started
 *
 * For Allwinner A64 (Mali-400 MP2): PP0+PP1 and PPMMU0+PPMMU1 are present;
 * all mali450-only IPs (DLBU, bcast, ppmmu_bcast) have offset -1 and
 * will have ip->present == false, so the bcast path is skipped.
 */
static int lima_init_pp_pipe(struct lima_device *dev)
{
	struct lima_sched_pipe *pipe = dev->pipe + lima_pipe_pp;
	int err, i;

	pipe->ldev = dev;

	err = lima_sched_pipe_init(pipe, "pp");
	if (err)
		return err;

	for (i = 0; i < LIMA_SCHED_PIPE_MAX_PROCESSOR; i++) {
		struct lima_ip *pp     = dev->ip + lima_ip_pp0   + i;
		struct lima_ip *ppmmu  = dev->ip + lima_ip_ppmmu0 + i;
		struct lima_ip *l2_cache;

		if (dev->id == lima_gpu_mali400)
			l2_cache = dev->ip + lima_ip_l2_cache0;
		else
			l2_cache = dev->ip + lima_ip_l2_cache1 + (i >> 2);

		if (pp->present && ppmmu->present && l2_cache->present) {
			pipe->mmu[pipe->num_mmu++]             = ppmmu;
			pipe->processor[pipe->num_processor++] = pp;
			if (!pipe->l2_cache[i >> 2])
				pipe->l2_cache[pipe->num_l2_cache++] = l2_cache;
		}
	}

	if (dev->ip[lima_ip_bcast].present) {
		pipe->bcast_processor = dev->ip + lima_ip_pp_bcast;
		pipe->bcast_mmu       = dev->ip + lima_ip_ppmmu_bcast;
	}

	err = lima_pp_pipe_init(dev);
	if (err) {
		lima_sched_pipe_fini(pipe);
		return err;
	}

	return 0;
}

/*
 * lima_fini_pp_pipe
 *
 * purpose:  Tear down the PP scheduler pipe and release associated resources.
 * input:    dev — lima_device
 * output:   void
 * sideEffects: PP DRM scheduler stopped; pipe resources freed
 *
 * Ordering note: see lima_fini_gp_pipe() above — lima_sched_pipe_fini()
 * must run before lima_pp_pipe_fini() destroys the task_slab kmem_cache.
 */
static void lima_fini_pp_pipe(struct lima_device *dev)
{
	struct lima_sched_pipe *pipe = dev->pipe + lima_pipe_pp;

	lima_sched_pipe_fini(pipe);
	lima_pp_pipe_fini(dev);
}

/* ------------------------------------------------------------------ */
/*  Device-level lifecycle                                             */
/* ------------------------------------------------------------------ */

/*
 * lima_device_init
 *
 * purpose:  Full Mali-400/450 device bringup: DMA mask, clocks, regulator,
 *           empty VM, optional DLBU DMA buffer (Mali-450 only), MMIO map,
 *           per-IP init, GP pipe, PP pipe, error-task list.
 * input:    ldev — lima_device with .dev (platform_device) and .id set by
 *                  the DRM driver probe
 * output:   0 on success; negative errno with full teardown on failure
 * sideEffects: all sub-IPs probed and initialised; DRM scheduler pipes live;
 *              dev_info messages printed with bus/GPU clock rates
 *
 * FreeBSD/linuxkpi notes:
 *   - dma_set_coherent_mask: linuxkpi dma-mapping shim; on aarch64/A64 the
 *     Mali MMU constrains addresses to 32-bit physical, so DMA_BIT_MASK(32)
 *     is correct regardless of host PA width.
 *   - devm_platform_ioremap_resource: maps resource index 0 (the Mali MMIO
 *     window at MALI_MMIO_BASE / MALI_MMIO_SIZE from ../mali_uio.h).
 *   - LIMA_VA_RESERVE_START / LIMA_VA_RESERVE_END defined in lima_device.h.
 *   - LIMA_PAGE_SIZE typically 4096; LIMA_DUMP_* constants in lima_device.h.
 */
int lima_device_init(struct lima_device *ldev)
{
	struct platform_device *pdev = to_platform_device(ldev->dev);
	int err, i;

	dma_set_coherent_mask(ldev->dev, DMA_BIT_MASK(32));
	dma_set_max_seg_size(ldev->dev, UINT_MAX);

	err = lima_clk_init(ldev);
	if (err)
		return err;

	err = lima_regulator_init(ldev);
	if (err)
		goto err_out0;
	/* Same reason as the per-IP trace in lima_init_ip(): between the clock
	 * enable and the first IP there are four steps and, until now, not one
	 * console byte, so an SoC bus stall anywhere in here was indistinguishable
	 * from one anywhere else in here. */
	dev_info(ldev->dev, "device_init: clocks+regulator done (regulator %s)\n",
		 ldev->regulator ? "present" : "absent");

	ldev->empty_vm = lima_vm_create(ldev);
	if (!ldev->empty_vm) {
		err = -ENOMEM;
		goto err_out1;
	}

	ldev->va_start = 0;
	if (ldev->id == lima_gpu_mali450) {
		ldev->va_end = LIMA_VA_RESERVE_START;
		ldev->dlbu_cpu = dma_alloc_wc(
			ldev->dev, LIMA_PAGE_SIZE,
			&ldev->dlbu_dma, GFP_KERNEL | __GFP_NOWARN);
		if (!ldev->dlbu_cpu) {
			err = -ENOMEM;
			goto err_out2;
		}
	} else {
		ldev->va_end = LIMA_VA_RESERVE_END;
	}

	dev_info(ldev->dev, "device_init: vm created, mapping iomem\n");

	ldev->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ldev->iomem)) {
		dev_err(ldev->dev, "fail to ioremap iomem\n");
		err = PTR_ERR(ldev->iomem);
		goto err_out3;
	}
	dev_info(ldev->dev, "device_init: iomem mapped, entering per-IP init\n");

	for (i = 0; i < lima_ip_num; i++) {
		err = lima_init_ip(ldev, i);
		if (err)
			goto err_out4;
	}

	err = lima_init_gp_pipe(ldev);
	if (err)
		goto err_out4;

	err = lima_init_pp_pipe(ldev);
	if (err)
		goto err_out5;

	ldev->dump.magic         = LIMA_DUMP_MAGIC;
	ldev->dump.version_major = LIMA_DUMP_MAJOR;
	ldev->dump.version_minor = LIMA_DUMP_MINOR;
	INIT_LIST_HEAD(&ldev->error_task_list);
	mutex_init(&ldev->error_task_list_lock);

	dev_info(ldev->dev, "bus rate = %lu\n",  clk_get_rate(ldev->clk_bus));
	dev_info(ldev->dev, "mod rate = %lu",    clk_get_rate(ldev->clk_gpu));

	return 0;

err_out5:
	lima_fini_gp_pipe(ldev);
err_out4:
	while (--i >= 0)
		lima_fini_ip(ldev, i);
err_out3:
	if (ldev->dlbu_cpu)
		dma_free_wc(ldev->dev, LIMA_PAGE_SIZE,
			    ldev->dlbu_cpu, ldev->dlbu_dma);
err_out2:
	lima_vm_put(ldev->empty_vm);
err_out1:
	lima_regulator_fini(ldev);
err_out0:
	lima_clk_fini(ldev);
	return err;
}

/*
 * lima_device_fini
 *
 * purpose:  Orderly teardown of the Mali device: drain the error-task list,
 *           destroy pipes, fini all IPs (reverse order), free DLBU DMA
 *           buffer, release the empty VM, then gate regulator and clocks.
 * input:    ldev — initialised lima_device
 * output:   void
 * sideEffects: all hardware quiesced; all kernel resources freed; devm
 *              handles cleaned up automatically by device core
 */
void lima_device_fini(struct lima_device *ldev)
{
	int i;
	struct lima_sched_error_task *et, *tmp;

	list_for_each_entry_safe(et, tmp, &ldev->error_task_list, list) {
		list_del(&et->list);
		kvfree(et);
	}
	mutex_destroy(&ldev->error_task_list_lock);

	lima_fini_pp_pipe(ldev);
	lima_fini_gp_pipe(ldev);

	for (i = lima_ip_num - 1; i >= 0; i--)
		lima_fini_ip(ldev, i);

	if (ldev->dlbu_cpu)
		dma_free_wc(ldev->dev, LIMA_PAGE_SIZE,
			    ldev->dlbu_cpu, ldev->dlbu_dma);

	lima_vm_put(ldev->empty_vm);

	lima_regulator_fini(ldev);

	lima_clk_fini(ldev);
}

/*
 * lima_device_resume
 *
 * purpose:  System-resume path: re-enable clocks and regulator, then
 *           call resume on each IP in forward order; start devfreq.
 * input:    dev — struct device (DRM pm_ops callback argument)
 * output:   0 on success; negative errno with partial rollback on failure
 * sideEffects: hardware fully operational; devfreq OPP management resumed
 *
 * FreeBSD/linuxkpi: pm_runtime_* callbacks wire into this via
 * lima_drv.c's dev_pm_ops; no direct FreeBSD newbus equivalent needed.
 */
int lima_device_resume(struct device *dev)
{
	struct lima_device *ldev = dev_get_drvdata(dev);
	int i, err;

	err = lima_clk_enable(ldev);
	if (err) {
		dev_err(dev, "resume clk fail %d\n", err);
		return err;
	}

	err = lima_regulator_enable(ldev);
	if (err) {
		dev_err(dev, "resume regulator fail %d\n", err);
		goto err_out0;
	}

	for (i = 0; i < lima_ip_num; i++) {
		err = lima_resume_ip(ldev, i);
		if (err) {
			dev_err(dev, "resume ip %d fail\n", i);
			goto err_out1;
		}
	}

	err = lima_devfreq_resume(&ldev->devfreq);
	if (err) {
		dev_err(dev, "devfreq resume fail\n");
		goto err_out1;
	}

	return 0;

err_out1:
	while (--i >= 0)
		lima_suspend_ip(ldev, i);
	lima_regulator_disable(ldev);
err_out0:
	lima_clk_disable(ldev);
	return err;
}

/*
 * lima_device_suspend
 *
 * purpose:  System-suspend path: reject if any pipe has work queued, then
 *           suspend devfreq, quiesce all IPs (reverse order), disable
 *           regulator, gate clocks.
 * input:    dev — struct device (DRM pm_ops callback argument)
 * output:   0 on success; -EBUSY if hardware is still processing;
 *           negative errno on devfreq failure
 * sideEffects: all IP blocks quiesced; power/clocks gated
 *
 * The -EBUSY guard prevents data corruption from suspending mid-job.
 * Callers (PM core) are expected to retry or abort the suspend cycle.
 */
int lima_device_suspend(struct device *dev)
{
	struct lima_device *ldev = dev_get_drvdata(dev);
	int i, err;

	/* reject suspend while any pipe has jobs in flight */
	for (i = 0; i < lima_pipe_num; i++) {
		if (atomic_read(&ldev->pipe[i].base.hw_rq_count))
			return -EBUSY;
	}

	err = lima_devfreq_suspend(&ldev->devfreq);
	if (err) {
		dev_err(dev, "devfreq suspend fail\n");
		return err;
	}

	for (i = lima_ip_num - 1; i >= 0; i--)
		lima_suspend_ip(ldev, i);

	lima_regulator_disable(ldev);

	lima_clk_disable(ldev);

	return 0;
}
