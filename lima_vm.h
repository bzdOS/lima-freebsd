// MODULE: hal/lima/lima_vm.h
// PURPOSE: GPU virtual address space manager for Mali-400 internal MMU
// PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_vm.h
// input:  lima_device, lima_bo — device and buffer-object handles
// output: IOVA mappings managed via drm_mm range allocator
// sideEffects: DMA-coherent pages allocated/freed; Mali MMU page tables written

/* SPDX-License-Identifier: BSD-2-Clause OR MIT */
/* Copyright 2017-2019 Qiang Yu <yuq825@gmail.com> */
/* FreeBSD port: bsdOS project — drm-66-kmod LinuxKPI, 2024 */

#ifndef __LIMA_VM_H__
#define __LIMA_VM_H__

/*
 * drm-66-kmod provides these headers via the LinuxKPI compatibility layer.
 * Include paths are identical to Linux — no FreeBSD-specific paths needed.
 *
 * struct mutex  → LinuxKPI mutex backed by FreeBSD sx(9)
 * struct kref   → LinuxKPI kref backed by atomic_t
 * dma_addr_t    → LinuxKPI type backed by bus_addr_t
 * struct drm_mm → drm-66-kmod range allocator, API identical to Linux
 */
#include <drm/drm_mm.h>
#include <linux/kref.h>

/*
 * Mali-400 internal MMU page geometry.
 * Hardware-defined: independent of host OS page size.
 * All values identical to the Linux driver.
 */
#define LIMA_PAGE_SIZE    4096
#define LIMA_PAGE_MASK    (LIMA_PAGE_SIZE - 1)
#define LIMA_PAGE_ENT_NUM (LIMA_PAGE_SIZE / sizeof(u32))

/*
 * Two-level page table layout:
 *   L1 = page directory (pd): one page, LIMA_PAGE_ENT_NUM entries
 *   L2 = page tables (bts): each PD entry covers LIMA_VM_NUM_PT_PER_BT PTs
 *
 * LIMA_VM_NUM_PT_PER_BT_SHIFT = 3  →  8 page tables per backing-table slot
 * LIMA_VM_NUM_BT               = 128 backing-table slots total
 */
#define LIMA_VM_NUM_PT_PER_BT_SHIFT 3
#define LIMA_VM_NUM_PT_PER_BT (1 << LIMA_VM_NUM_PT_PER_BT_SHIFT)
#define LIMA_VM_NUM_BT (LIMA_PAGE_ENT_NUM >> LIMA_VM_NUM_PT_PER_BT_SHIFT)

/*
 * Reserved IOVA range at the top of the 32-bit address space.
 * DLBU (Dynamic Load Balancing Unit) registers are mapped here.
 * Range: [0x0FFF00000, 0x100000000) — 1 MiB
 */
#define LIMA_VA_RESERVE_START  0x0FFF00000ULL
#define LIMA_VA_RESERVE_DLBU   LIMA_VA_RESERVE_START
#define LIMA_VA_RESERVE_END    0x100000000ULL

struct lima_device;

/*
 * lima_vm_page — one DMA-coherent page used as an MMU table.
 *
 * @cpu: kernel-virtual pointer; written directly to program Mali MMU hardware.
 * @dma: bus address; stored in the parent page-directory entry so Mali's
 *       MMU walker can fetch the page-table page over the AXI bus.
 *
 * On FreeBSD, dma_addr_t is a LinuxKPI typedef backed by bus_addr_t.
 * Allocation: dma_alloc_coherent() → LinuxKPI → bus_dmamem_alloc(9).
 */
struct lima_vm_page {
	u32        *cpu;
	dma_addr_t  dma;
};

/*
 * lima_vm — per-context GPU virtual address space.
 *
 * @lock:     serialises bo_add/bo_del against concurrent GPU submissions.
 *            LinuxKPI mutex maps to FreeBSD sx(9) (sleepable, non-spin).
 * @refcount: kref-style reference count; last put calls lima_vm_release.
 * @mm:       drm_mm IOVA range allocator covering [0, LIMA_VA_RESERVE_START).
 * @dev:      owning lima_device — needed to reach DMA ops and MMU registers.
 * @pd:       page-directory page (L1 table, one per VM).
 * @bts:      backing page-table pages (L2 tables); lazily allocated on first
 *            mapping into each 4 MiB PD region.
 */
u32 lima_vm_probe_pte(struct lima_vm *vm, u32 va, int *bt_present);

struct lima_vm {
	struct mutex        lock;
	struct kref         refcount;

	struct drm_mm       mm;

	struct lima_device *dev;

	struct lima_vm_page pd;
	struct lima_vm_page bts[LIMA_VM_NUM_BT];
};

/*
 * lima_vm_bo_add — map a buffer object into a VM, allocating an IOVA.
 *
 * purpose:     Reserve a contiguous IOVA range for @bo in @vm and write
 *              the Mali page-table entries that cover it.
 * input:       vm     — target address space
 *              bo     — GEM buffer object to map
 *              create — true: allocate new IOVA; false: reuse existing
 * output:      0 on success, negative errno on failure
 * sideEffects: drm_mm range allocated; L2 page-table pages allocated if
 *              the backing slot was previously empty; Mali PT entries written.
 */
int lima_vm_bo_add(struct lima_vm *vm, struct lima_bo *bo, bool create);

/*
 * lima_vm_bo_del — unmap a buffer object from a VM, releasing its IOVA.
 *
 * purpose:     Clear the Mali page-table entries for @bo and free the
 *              drm_mm IOVA range.
 * input:       vm — address space
 *              bo — buffer object to unmap
 * output:      void
 * sideEffects: drm_mm range freed; PT entries zeroed; L2 pages freed if
 *              the backing slot is now entirely empty.
 */
void lima_vm_bo_del(struct lima_vm *vm, struct lima_bo *bo);

/*
 * lima_vm_get_va — read the IOVA assigned to a buffer object.
 *
 * purpose:     Return the GPU virtual address for @bo within @vm so that
 *              command-stream builders can embed it in job descriptors.
 * input:       vm — address space that contains @bo
 *              bo — mapped buffer object
 * output:      32-bit IOVA (always fits: Mali-400 uses a 32-bit VA space)
 * sideEffects: none (read-only)
 */
u32 lima_vm_get_va(struct lima_vm *vm, struct lima_bo *bo);

/*
 * lima_vm_create — allocate and initialise a new GPU address space.
 *
 * purpose:     Construct a lima_vm for a new DRM file context.
 * input:       dev — lima_device that owns this VM
 * output:      pointer to new lima_vm, or NULL on allocation failure
 * sideEffects: DMA-coherent page allocated for @pd; drm_mm initialised
 *              over [0, LIMA_VA_RESERVE_START); refcount set to 1.
 */
struct lima_vm *lima_vm_create(struct lima_device *dev);

/*
 * lima_vm_release — kref release callback; tears down a VM.
 *
 * purpose:     Called by kref_put when the last reference is dropped.
 *              Frees all L2 page-table pages and the L1 page directory.
 * input:       kref — embedded in lima_vm; use container_of to reach it
 * output:      void
 * sideEffects: All dma_free_coherent calls for @pd and populated @bts;
 *              drm_mm_takedown; kfree of the lima_vm struct itself.
 */
void lima_vm_release(struct kref *kref);

/*
 * lima_vm_get — increment the reference count.
 *
 * purpose:     Grab a reference so the VM is not freed while in use.
 * input:       vm — address space to reference
 * output:      vm (for call-site convenience)
 * sideEffects: atomic increment of vm->refcount
 */
static inline struct lima_vm *lima_vm_get(struct lima_vm *vm)
{
	kref_get(&vm->refcount);
	return vm;
}

/*
 * lima_vm_put — decrement the reference count; destroy if last.
 *
 * purpose:     Drop a reference; calls lima_vm_release when count hits zero.
 * input:       vm — address space to release (NULL-safe)
 * output:      void
 * sideEffects: atomic decrement; may invoke lima_vm_release (see above)
 */
static inline void lima_vm_put(struct lima_vm *vm)
{
	if (vm)
		kref_put(&vm->refcount, lima_vm_release);
}

/*
 * lima_vm_print — dump VM page-table state to kernel log.
 *
 * purpose:     Debug helper — prints all allocated IOVA ranges and
 *              page-table entries for @vm.
 * input:       vm — address space to print
 * output:      void
 * sideEffects: printf(9) / DRM_DEBUG output; holds vm->lock briefly
 */
void lima_vm_print(struct lima_vm *vm);

/*
 * lima_vm_map_bo — write page-table entries for a buffer object.
 *
 * purpose:     Install the physical→IOVA mappings in the Mali page tables
 *              starting at page offset @pageoff within @bo's IOVA range.
 *              Called after lima_vm_bo_add once scatter-gather is resolved.
 * input:       vm      — address space
 *              bo      — buffer object whose pages to map
 *              pageoff — starting page index within the IOVA allocation
 * output:      0 on success, negative errno on failure
 * sideEffects: L2 page-table entries written; Mali MMU cache flush issued
 *              via lima_mmu_flush_tlb() after all entries are set.
 */
int lima_vm_map_bo(struct lima_vm *vm, struct lima_bo *bo, int pageoff);

#endif /* __LIMA_VM_H__ */
