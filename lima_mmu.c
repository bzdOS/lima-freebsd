// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2017-2019 Qiang Yu <yuq825@gmail.com>
 * Copyright (c) 2024 bsdOS contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE.
 */

/*
 * MODULE:      hal/lima/lima_mmu.c
 * PURPOSE:     Initialise, suspend/resume, and service IRQs for the Mali-400
 *              internal MMU under FreeBSD 15.1 drm-66-kmod LinuxKPI.
 * PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_mmu.c
 * INPUT:       struct lima_ip * — per-IP block descriptor (iomem, IRQ, parent dev)
 * OUTPUT:      0 on success; negative errno on hw-init failure;
 *              IRQ_HANDLED / IRQ_NONE from ISR.
 * SIDE_EFFECTS: Writes LIMA_MMU_COMMAND / LIMA_MMU_INT_MASK registers;
 *              registers IRQ handler via devm_request_irq;
 *              signals lima_sched_pipe_mmu_error on page fault.
 * DELTA_VS_LINUX: Headers only — Linux <linux/interrupt.h> + <linux/iopoll.h>
 *              replaced by drm-66-kmod LinuxKPI equivalents; all register
 *              logic and algorithms are byte-for-byte identical.
 */

/*
 * Linux origin:                    FreeBSD drm-66-kmod equivalent:
 *   <linux/interrupt.h>       →    <linux/interrupt.h>  (LinuxKPI)
 *   <linux/iopoll.h>          →    <linux/iopoll.h>     (LinuxKPI)
 *   <linux/device.h>          →    <linux/device.h>     (LinuxKPI)
 * All three are provided verbatim by drm-66-kmod's LinuxKPI layer;
 * no include path changes required — the kmod Makefile adds the right -I flags.
 */
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/device.h>

#include "lima_device.h"
#include "lima_mmu.h"
#include "lima_vm.h"
#include "lima_sched.h"   /* struct lima_sched_pipe: current_vm */
#include "lima_regs.h"
#include "lima_freebsd_compat.h"

/*
 * purpose:  Zero-overhead MMIO read/write wrappers scoped to the current ip.
 * input:    reg  — byte offset from ip->iomem base (LIMA_MMU_* constant)
 *           data — 32-bit value to write (mmu_write only)
 * output:   mmu_read returns u32; mmu_write returns void
 * sideEffects: single 32-bit MMIO transaction; no caching, no barriers beyond
 *             those implied by writel/readl (LinuxKPI maps to bus_space_write_4).
 *
 * FreeBSD note: writel/readl are provided by LinuxKPI <asm/io.h> (pulled in via
 * <linux/device.h>) and map to bus_space_{write,read}_4 with correct barriers.
 * Semantics are identical to the Linux originals.
 */
#define mmu_write(reg, data)  writel(data, (uint8_t __iomem *)ip->iomem + (reg))
#define mmu_read(reg)         readl((uint8_t __iomem *)ip->iomem + (reg))

/*
 * lima_mmu_send_command
 *
 * purpose:     Write a command to LIMA_MMU_COMMAND, then busy-poll a register
 *              field until a condition is satisfied (or 100 µs elapses).
 * input:       cmd  — LIMA_MMU_COMMAND_* constant to write
 *              addr — register offset to poll (LIMA_MMU_DTE_ADDR or LIMA_MMU_STATUS)
 *              val  — scratch variable declared by caller; receives polled value
 *              cond — boolean expression evaluated against val
 * output:      0 on success; -ETIMEDOUT (from readl_poll_timeout) on timeout
 * sideEffects: writes LIMA_MMU_COMMAND; multiple MMIO reads during poll;
 *              logs dev_err on timeout.
 *
 * FreeBSD note: readl_poll_timeout is provided by LinuxKPI <linux/iopoll.h>
 * with identical semantics. The delay_us=0 (spin-only) and timeout_us=100
 * arguments are appropriate for a one-shot HW command on Cortex-A53.
 * The ({...}) GNU statement-expression extension is supported by clang
 * (the FreeBSD kernel build compiler).
 */
#define lima_mmu_send_command(cmd, addr, val, cond)           \
({                                                            \
	int __ret;                                            \
                                                              \
	mmu_write(LIMA_MMU_COMMAND, cmd);                     \
	__ret = readl_poll_timeout((uint8_t __iomem *)ip->iomem + (addr), val,  \
				   cond, 0, 100);             \
	if (__ret)                                            \
		dev_err(dev->dev,                             \
			"mmu command %x timeout\n", cmd);     \
	__ret;                                                \
})

/*
 * lima_mmu_irq_handler
 *
 * purpose:     Service Mali-400 MMU interrupt: report page faults and bus
 *              errors, mask all interrupts, then schedule pipeline recovery.
 * input:       irq  — interrupt number (unused; status read from MMIO)
 *              data — struct lima_ip * for this MMU block
 * output:      IRQ_HANDLED if status != 0; IRQ_NONE for shared-IRQ spurious
 * sideEffects: writes LIMA_MMU_INT_MASK=0 and LIMA_MMU_INT_CLEAR;
 *              calls lima_sched_pipe_mmu_error to abort in-flight jobs.
 *
 * FreeBSD note: irqreturn_t, IRQ_NONE, IRQ_HANDLED are provided by LinuxKPI
 * <linux/interrupt.h>. The handler signature is identical to Linux.
 * The shared-IRQ early-return on status==0 is required on FreeBSD too —
 * the A64 GIC can deliver GP and PP MMU interrupts on the same SPI line
 * depending on pinmux configuration.
 */
static irqreturn_t lima_mmu_irq_handler(int irq, void *data)
{
	struct lima_ip         *ip     = data;
	struct lima_device     *dev    = ip->dev;
	u32                     status = mmu_read(LIMA_MMU_INT_STATUS);
	struct lima_sched_pipe *pipe;

	/* for shared irq case */
	if (!status)
		return IRQ_NONE;

	if (status & LIMA_MMU_INT_PAGE_FAULT) {
		u32 fault = mmu_read(LIMA_MMU_PAGE_FAULT_ADDR);
		struct lima_sched_pipe *fpipe;

		dev_err(dev->dev,
			"mmu page fault at 0x%x from bus id %d of type %s on %s\n",
			fault,
			LIMA_MMU_STATUS_BUS_ID(status),
			status & LIMA_MMU_STATUS_PAGE_FAULT_IS_WRITE
				? "write" : "read",
			lima_ip_name(ip));

		/*
		 * Say WHY, not just where. The address alone cannot distinguish
		 * "this page was never mapped" from "it is mapped and the GPU
		 * cannot see the entry" -- and those have entirely different
		 * causes (a missing lima_vm_bo_add, versus page-table coherency
		 * or a stale MMU TLB). Walk the live page table with the same
		 * macros the mapper uses and print the actual PTE.
		 */
		fpipe = dev->pipe + (ip->id == lima_ip_gpmmu
				     ? lima_pipe_gp : lima_pipe_pp);
		if (fpipe->current_vm != NULL) {
			int bt_present = 0;
			u32 pte = lima_vm_probe_pte(fpipe->current_vm,
						    fault, &bt_present);

			if (!bt_present)
				dev_err(dev->dev, "  -> no backing page table "
				    "for that region at all (nothing in it was "
				    "ever mapped)\n");
			else if (pte == 0)
				dev_err(dev->dev, "  -> PTE is 0: the page is "
				    "genuinely UNMAPPED (mapping bug, not "
				    "coherency)\n");
			else
				dev_err(dev->dev, "  -> PTE = 0x%08x: the page "
				    "IS mapped, so the GPU is not seeing the "
				    "table (coherency or stale TLB)\n", pte);
		} else {
			dev_err(dev->dev, "  -> no current_vm on that pipe\n");
		}
	}

	if (status & LIMA_MMU_INT_READ_BUS_ERROR)
		dev_err(dev->dev, "mmu %s irq bus error\n", lima_ip_name(ip));

	/* mask all interrupts before resume */
	mmu_write(LIMA_MMU_INT_MASK,  0);
	mmu_write(LIMA_MMU_INT_CLEAR, status);

	pipe = dev->pipe + (ip->id == lima_ip_gpmmu
			    ? lima_pipe_gp : lima_pipe_pp);
	lima_sched_pipe_mmu_error(pipe);

	return IRQ_HANDLED;
}

/*
 * lima_mmu_hw_init
 *
 * purpose:     Issue a hard reset to the MMU block, install the empty-VM page
 *              directory, then enable address-space paging.
 * input:       ip — per-IP descriptor; ip->dev->empty_vm must already be mapped
 * output:      0 on success; -ETIMEDOUT if hard-reset or enable-paging stalls
 * sideEffects: writes LIMA_MMU_COMMAND, LIMA_MMU_INT_MASK, LIMA_MMU_DTE_ADDR
 *
 * Sequence note: the first mmu_write(HARD_RESET) kicks the MMU FSM; the
 * subsequent lima_mmu_send_command(HARD_RESET, ...) confirms completion by
 * watching LIMA_MMU_DTE_ADDR return 0. The double-write is intentional and
 * matches the Linux original.
 */
static int lima_mmu_hw_init(struct lima_ip *ip)
{
	struct lima_device *dev = ip->dev;
	int err;
	u32 v;

	mmu_write(LIMA_MMU_COMMAND, LIMA_MMU_COMMAND_HARD_RESET);
	err = lima_mmu_send_command(LIMA_MMU_COMMAND_HARD_RESET,
				    LIMA_MMU_DTE_ADDR, v, v == 0);
	if (err)
		return err;

	mmu_write(LIMA_MMU_INT_MASK,
		  LIMA_MMU_INT_PAGE_FAULT | LIMA_MMU_INT_READ_BUS_ERROR);
	mmu_write(LIMA_MMU_DTE_ADDR, dev->empty_vm->pd.dma);
	return lima_mmu_send_command(LIMA_MMU_COMMAND_ENABLE_PAGING,
				     LIMA_MMU_STATUS, v,
				     v & LIMA_MMU_STATUS_PAGING_ENABLED);
}

/*
 * lima_mmu_resume
 *
 * purpose:     Re-initialise MMU hardware after system/runtime suspend.
 * input:       ip — per-IP descriptor
 * output:      0 on success; negative errno from lima_mmu_hw_init
 * sideEffects: delegates to lima_mmu_hw_init; bcast IP is a no-op (returns 0).
 */
int lima_mmu_resume(struct lima_ip *ip)
{
	if (ip->id == lima_ip_ppmmu_bcast)
		return 0;

	return lima_mmu_hw_init(ip);
}

/*
 * lima_mmu_suspend
 *
 * purpose:     Called before system/runtime suspend; MMU self-quiesces when
 *              the GPU power domain is gated, so no explicit teardown needed.
 * input:       ip — per-IP descriptor (unused)
 * output:      void
 * sideEffects: none
 */
void lima_mmu_suspend(struct lima_ip *ip)
{
	/* intentionally empty — GPU power domain gates the MMU */
}

/*
 * lima_mmu_init
 *
 * purpose:     Verify the MMU hardware is alive (DTE readback test), register
 *              the IRQ handler, then run the initial HW reset+enable sequence.
 * input:       ip — per-IP descriptor; irq line must be populated by device probe
 * output:      0 on success; -EIO on DTE test failure;
 *              negative errno from devm_request_irq or lima_mmu_hw_init
 * sideEffects: registers irq handler (devm-managed, freed on device removal);
 *              writes LIMA_MMU_DTE_ADDR twice (test pattern + real PD address).
 *
 * FreeBSD note: devm_request_irq is provided by LinuxKPI <linux/interrupt.h>.
 * IRQF_SHARED maps to RF_SHAREABLE in FreeBSD bus_setup_intr; LinuxKPI
 * handles the translation transparently.
 *
 * DTE readback test: write 0xCAFEBABE, read back masked value 0xCAFEB000.
 * The MMU masks the low 12 bits of DTE_ADDR (page-aligned). Any other value
 * means the MMIO window is broken or the wrong physical base address was used.
 * On A64 the correct base is 0x01C40000 (see mali_uio.h: MALI_MMIO_BASE).
 */
int lima_mmu_init(struct lima_ip *ip)
{
	struct lima_device *dev = ip->dev;
	int err;
	u32 v;

	if (ip->id == lima_ip_ppmmu_bcast)
		return 0;

	/*
	 * Bracket the DTE self-test with console writes.
	 *
	 * With the GPU out of reset this pair of MMIO accesses is where the board
	 * dies: not a panic, no backtrace, all four cores gone, output stopping
	 * mid-line and the hardware watchdog eventually resetting to U-Boot. From
	 * outside, "stalled on the write", "stalled on the read-back" and "test
	 * passed, died later" are indistinguishable — the failure prints nothing
	 * either way, and the test's own failure message only appears when the GPU
	 * is NOT out of reset. Bracketing is the only way to tell them apart.
	 *
	 * These are dev_info, not dev_dbg, and they stay: each console byte here is
	 * a synchronous stage-2 fault into EL2 that is captured before the next
	 * instruction retires, so the last line in the ring is exactly the last
	 * thing that executed. That property is what makes this readable at all
	 * after the board is gone.
	 */
	dev_info(dev->dev, "mmu %s: DTE write 0xCAFEBABE\n", lima_ip_name(ip));
	mmu_write(LIMA_MMU_DTE_ADDR, 0xCAFEBABE);
	dev_info(dev->dev, "mmu %s: DTE write returned, reading back\n",
		 lima_ip_name(ip));
	v = mmu_read(LIMA_MMU_DTE_ADDR);
	dev_info(dev->dev, "mmu %s: DTE read back 0x%08x (want 0xCAFEB000)\n",
		 lima_ip_name(ip), v);
	if (v != 0xCAFEB000) {
		dev_err(dev->dev, "mmu %s dte write test fail\n",
			lima_ip_name(ip));
		return -EIO;
	}

	/*
	 * Silence the MMU BEFORE its interrupt line goes live.
	 *
	 * devm_request_irq() below enables the GP MMU's SPI at the GIC (A64: SPI
	 * 98 / INTID 130).  Everything that tells the MMU to stop asserting —
	 * HARD_RESET, and the INT_MASK write — happens in lima_mmu_hw_init(),
	 * which runs AFTER.  So there was a window with the line enabled and the
	 * MMU in its raw post-reset state, INT_MASK untouched and INT_RAWSTAT
	 * uncleared.
	 *
	 * On bare-metal Linux that window is survivable.  Under the bzdOS EL2
	 * hypervisor this project has already lost a board to exactly this shape
	 * once (at the time the guest owned the GIC directly, HCR_EL2.IMO=0; that
	 * is no longer how it runs -- the hypervisor now forwards physical IRQs
	 * into guest List Registers under IMO=1 -- but a level-triggered source
	 * nobody clears storms either way, and the lesson stands): an enabled
	 * level-triggered SPI whose source nobody had cleared storms at ~145 kHz
	 * (EHCI INTID 106, see the hypervisor's own notes) and starves every core,
	 * including the debug core that pets the hardware watchdog.  Observed here
	 * as: no panic, no backtrace, console output stops mid-line, all four
	 * cores gone, board reset by the watchdog.  It only appeared once the
	 * clocks became real, because until then the DTE self-test above failed
	 * first and this code was never reached.
	 *
	 * Masking and clearing first costs two register writes and makes the
	 * ordering correct rather than merely lucky: never enable an interrupt
	 * line for a device you have not first told to be quiet.
	 */
	mmu_write(LIMA_MMU_INT_MASK, 0);
	mmu_write(LIMA_MMU_INT_CLEAR,
		  LIMA_MMU_INT_PAGE_FAULT | LIMA_MMU_INT_READ_BUS_ERROR);

	err = devm_request_irq(dev->dev, ip->irq,
			       lima_mmu_irq_handler,
			       IRQF_SHARED,
			       lima_ip_name(ip), ip);
	if (err) {
		dev_err(dev->dev, "mmu %s fail to request irq\n",
			lima_ip_name(ip));
		return err;
	}

	return lima_mmu_hw_init(ip);
}

/*
 * lima_mmu_fini
 *
 * purpose:     Release MMU resources on device removal.
 * input:       ip — per-IP descriptor
 * output:      void
 * sideEffects: none — IRQ was registered with devm_request_irq and is
 *              automatically freed by devres on driver detach.
 */
void lima_mmu_fini(struct lima_ip *ip)
{
	/* intentionally empty — devm handles IRQ teardown */
}

/*
 * lima_mmu_flush_tlb
 *
 * purpose:     Invalidate the MMU TLB cache by issuing ZAP_CACHE command.
 * input:       ip — per-IP descriptor
 * output:      void
 * sideEffects: writes LIMA_MMU_COMMAND; all cached TLB entries are discarded.
 *              Called after page-table modifications to ensure coherency.
 */
void lima_mmu_flush_tlb(struct lima_ip *ip)
{
	mmu_write(LIMA_MMU_COMMAND, LIMA_MMU_COMMAND_ZAP_CACHE);
}

/*
 * lima_mmu_switch_vm
 *
 * purpose:     Atomically switch the MMU's active address space to a new
 *              lima_vm by: stalling the bus, swapping the DTE pointer,
 *              flushing the TLB, then unstalling.
 * input:       ip — per-IP descriptor
 *              vm — target VM whose page directory DMA address is installed
 * output:      void (errors from lima_mmu_send_command are logged but not returned)
 * sideEffects: writes LIMA_MMU_COMMAND (ENABLE_STALL, ZAP_CACHE, DISABLE_STALL)
 *              and LIMA_MMU_DTE_ADDR; bus transactions are stalled during swap.
 *
 * Hot path — called on every GPU context switch. The four-step sequence must
 * be atomic with respect to the GPU bus arbiter. A stall timeout means the
 * GPU is wedged; the job scheduler handles recovery via mmu_error.
 */
void lima_mmu_switch_vm(struct lima_ip *ip, struct lima_vm *vm)
{
	struct lima_device *dev = ip->dev;
	u32 v;

	lima_mmu_send_command(LIMA_MMU_COMMAND_ENABLE_STALL,
			      LIMA_MMU_STATUS, v,
			      v & LIMA_MMU_STATUS_STALL_ACTIVE);

	mmu_write(LIMA_MMU_DTE_ADDR, vm->pd.dma);

	/* flush the TLB */
	mmu_write(LIMA_MMU_COMMAND, LIMA_MMU_COMMAND_ZAP_CACHE);

	lima_mmu_send_command(LIMA_MMU_COMMAND_DISABLE_STALL,
			      LIMA_MMU_STATUS, v,
			      !(v & LIMA_MMU_STATUS_STALL_ACTIVE));
}

/*
 * lima_mmu_page_fault_resume
 *
 * purpose:     Reset the MMU out of PAGE_FAULT_ACTIVE state after the
 *              scheduler has aborted the faulting job and flushed the queue.
 *              Re-arms the interrupt mask and re-installs the empty VM so
 *              subsequent jobs can run.
 * input:       ip — per-IP descriptor; ip->dev->empty_vm must be valid
 * output:      void
 * sideEffects: writes LIMA_MMU_INT_MASK (clear then restore),
 *              LIMA_MMU_DTE_ADDR (test pattern then empty VM),
 *              LIMA_MMU_COMMAND (HARD_RESET, ENABLE_PAGING);
 *              logs dev_info on entry.
 *
 * The MMU stays in PAGE_FAULT_ACTIVE with bus transactions stalled until this
 * function runs. The mask-disable / write-test-pattern / hard-reset /
 * mask-re-enable / install-empty-VM sequence is the only documented way to
 * exit fault state without a power cycle.
 */
void lima_mmu_page_fault_resume(struct lima_ip *ip)
{
	struct lima_device *dev = ip->dev;
	u32 status = mmu_read(LIMA_MMU_STATUS);
	u32 v;

	if (status & LIMA_MMU_STATUS_PAGE_FAULT_ACTIVE) {
		dev_info(dev->dev, "mmu resume\n");

		mmu_write(LIMA_MMU_INT_MASK, 0);
		mmu_write(LIMA_MMU_DTE_ADDR, 0xCAFEBABE);
		lima_mmu_send_command(LIMA_MMU_COMMAND_HARD_RESET,
				      LIMA_MMU_DTE_ADDR, v, v == 0);
		mmu_write(LIMA_MMU_INT_MASK,
			  LIMA_MMU_INT_PAGE_FAULT | LIMA_MMU_INT_READ_BUS_ERROR);
		mmu_write(LIMA_MMU_DTE_ADDR, dev->empty_vm->pd.dma);
		lima_mmu_send_command(LIMA_MMU_COMMAND_ENABLE_PAGING,
				      LIMA_MMU_STATUS, v,
				      v & LIMA_MMU_STATUS_PAGING_ENABLED);
	}
}
