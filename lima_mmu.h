// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2017-2019 Qiang Yu <yuq825@gmail.com>
 * Copyright (c) 2024 bsdOS Lima port contributors
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// MODULE:      hal/lima/lima_mmu.h
// PURPOSE:     Declare the Mali-400 internal MMU control interface for the
//              Lima DRM kmod on FreeBSD 15.1 (suspend/resume, TLB flush,
//              VM switch, and page-fault recovery).
// PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_mmu.h

#ifndef __LIMA_MMU_H__
#define __LIMA_MMU_H__

/*
 * Forward declarations — definitions live in lima_ip.h and lima_vm.h.
 *
 * These are identical to the Linux originals: LinuxKPI does not alter the
 * in-kernel struct layout, so no translation is required here.
 */
struct lima_ip;
struct lima_vm;

/*
 * lima_mmu_resume -- Re-enable the MMU after power-on or runtime resume.
 *
 * purpose:     Restore MMU hardware state following a suspend/power-down
 *              cycle.  Writes command and configuration registers, waits
 *              for the STATUS_IDLE acknowledgement, then re-enables the
 *              page-fault interrupt.
 * input:       ip  – Lima IP block descriptor (contains MMIO base, IRQ
 *                    number, and the currently bound lima_vm pointer).
 * output:      0 on success; negative errno on timeout or register error.
 * sideEffects: Writes LIMA_MMU_CMD, LIMA_MMU_INT_RAWSTAT hardware regs;
 *              may call lima_mmu_switch_vm() to restore the active VM.
 */
int  lima_mmu_resume(struct lima_ip *ip);

/*
 * lima_mmu_suspend -- Quiesce the MMU before power-off or runtime suspend.
 *
 * purpose:     Disable the MMU interrupt and put the hardware into a safe
 *              idle state so the power domain can be gated.
 * input:       ip  – Lima IP block descriptor.
 * output:      void.
 * sideEffects: Clears LIMA_MMU_INT_MASK; hardware stops raising IRQs.
 */
void lima_mmu_suspend(struct lima_ip *ip);

/*
 * lima_mmu_init -- One-time hardware initialisation at kmod load.
 *
 * purpose:     Map MMIO, request the page-fault IRQ, and put the MMU into
 *              a known-good state with an empty (dummy) page table.
 *              Under drm-66-kmod the IRQ is requested via
 *              devm_request_irq() so cleanup is automatic on device
 *              detach — no FreeBSD-specific teardown needed.
 * input:       ip  – Lima IP block descriptor (platform_device already
 *                    probed; MMIO resource available via ip->iomem).
 * output:      0 on success; negative errno on IRQ or MMIO failure.
 * sideEffects: Registers IRQ handler; writes LIMA_MMU_DTE_ADDR with the
 *              dummy page-table bus address.
 */
int  lima_mmu_init(struct lima_ip *ip);

/*
 * lima_mmu_fini -- Tear down the MMU at kmod unload.
 *
 * purpose:     Release any resources not covered by devm_* (e.g., the
 *              dummy DMA page).  Under drm-66-kmod most cleanup is
 *              handled by drmm_* managed actions; this function handles
 *              the residual non-managed allocations.
 * input:       ip  – Lima IP block descriptor.
 * output:      void.
 * sideEffects: Frees dummy page-table DMA allocation; disables IRQ.
 */
void lima_mmu_fini(struct lima_ip *ip);

/*
 * lima_mmu_flush_tlb -- Invalidate all TLB entries in this MMU.
 *
 * purpose:     Issue a LIMA_MMU_CMD_ZAP_CACHE command and spin-wait for
 *              STATUS_IDLE.  Must be called after updating any page-table
 *              entry that may already be cached in the MMU hardware.
 * input:       ip  – Lima IP block descriptor.
 * output:      void (timeout logged via DRM_ERROR; hardware left in best-
 *              effort state).
 * sideEffects: Writes LIMA_MMU_CMD register; spins up to ~1 ms.
 */
void lima_mmu_flush_tlb(struct lima_ip *ip);

/*
 * lima_mmu_switch_vm -- Point the MMU at a different address space.
 *
 * purpose:     Write the new VM's top-level directory table address into
 *              LIMA_MMU_DTE_ADDR and flush the TLB, atomically switching
 *              the GPU's address translation context.  Called on every
 *              job submission when the incoming job belongs to a different
 *              lima_vm than the currently active one.
 * input:       ip  – Lima IP block descriptor.
 *              vm  – Target virtual memory context (may be NULL to switch
 *                    to the dummy/empty page table).
 * output:      void.
 * sideEffects: Writes LIMA_MMU_DTE_ADDR; calls lima_mmu_flush_tlb().
 */
void lima_mmu_switch_vm(struct lima_ip *ip, struct lima_vm *vm);

/*
 * lima_mmu_page_fault_resume -- Clear a page-fault and restart the pipeline.
 *
 * purpose:     Called from the MMU IRQ handler after the faulting job has
 *              been terminated.  Writes LIMA_MMU_CMD_PAGE_FAULT_DONE to
 *              acknowledge the fault to hardware, re-enables the interrupt
 *              mask, and allows the next queued job to proceed.
 * input:       ip  – Lima IP block descriptor.
 * output:      void.
 * sideEffects: Writes LIMA_MMU_CMD and LIMA_MMU_INT_MASK registers.
 */
void lima_mmu_page_fault_resume(struct lima_ip *ip);

#endif /* __LIMA_MMU_H__ */
