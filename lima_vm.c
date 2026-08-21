// MODULE: hal/lima/lima_vm.c
// PURPOSE: Mali-400 GPU virtual memory manager — page directory, page tables, IOVA lifecycle
// PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_vm.c
// input:  lima_device (DMA device + va_start/va_end), lima_bo (scatter-gather DMA pages)
// output: IOVA range in vm->mm; Mali MMU page tables written via DMA-coherent memory
// sideEffects: dma_alloc_coherent / dma_free_coherent; drm_mm_insert_node / drm_mm_remove_node

/* SPDX-License-Identifier: BSD-2-Clause OR MIT                              */
/* Copyright 2017-2019 Qiang Yu <yuq825@gmail.com>                           */
/* FreeBSD port: bsdOS project, drm-66-kmod LinuxKPI, 2024                   */

#include <linux/slab.h>          /* kzalloc, kfree — LinuxKPI */
#include <linux/dma-mapping.h>   /* dma_alloc_coherent — LinuxKPI (no wc variant on FreeBSD) */

#include "lima_device.h"
#include "lima_vm.h"
#include "lima_gem.h"
#include "lima_regs.h"

struct lima_bo_va {
	struct list_head list;
	unsigned int ref_count;

	struct drm_mm_node node;

	struct lima_vm *vm;
};

#define LIMA_VM_PD_SHIFT 22
#define LIMA_VM_PT_SHIFT 12
#define LIMA_VM_PB_SHIFT (LIMA_VM_PD_SHIFT + LIMA_VM_NUM_PT_PER_BT_SHIFT)
#define LIMA_VM_BT_SHIFT LIMA_VM_PT_SHIFT

#define LIMA_VM_PT_MASK ((1 << LIMA_VM_PD_SHIFT) - 1)
#define LIMA_VM_BT_MASK ((1 << LIMA_VM_PB_SHIFT) - 1)

#define LIMA_PDE(va) (va >> LIMA_VM_PD_SHIFT)
#define LIMA_PTE(va) ((va & LIMA_VM_PT_MASK) >> LIMA_VM_PT_SHIFT)
#define LIMA_PBE(va) (va >> LIMA_VM_PB_SHIFT)
#define LIMA_BTE(va) ((va & LIMA_VM_BT_MASK) >> LIMA_VM_BT_SHIFT)


/*
 * lima_vm_probe_pte — what does the page table actually say about @va?
 *
 * Exists to answer one question that a "mmu page fault at 0x..." line cannot:
 * is the address UNMAPPED, or mapped-and-the-GPU-cannot-see-it? Those have
 * completely different causes (a missing lima_vm_bo_add vs a coherency or TLB
 * problem) and guessing between them is exactly the kind of thing that has cost
 * this project days.
 *
 * Returns the raw PTE, or 0 if the backing table for that region was never
 * allocated -- *bt_present distinguishes the two, because a present BT with a
 * zero PTE is a genuinely unmapped page while an absent BT means nothing in that
 * whole 8-page-table region was ever touched.
 *
 * Uses the same LIMA_PBE/LIMA_BTE macros as the mapping path on purpose: a probe
 * that walked the tables its own way could disagree with the mapper and would be
 * worse than no probe at all.
 */
u32 lima_vm_probe_pte(struct lima_vm *vm, u32 va, int *bt_present)
{
	u32 pbe = LIMA_PBE(va);
	u32 bte = LIMA_BTE(va);

	if (pbe >= LIMA_VM_NUM_BT || !vm->bts[pbe].cpu) {
		*bt_present = 0;
		return 0;
	}
	*bt_present = 1;
	return vm->bts[pbe].cpu[bte];
}

static void lima_vm_unmap_range(struct lima_vm *vm, u32 start, u32 end)
{
	u32 addr;

	for (addr = start; addr <= end; addr += LIMA_PAGE_SIZE) {
		u32 pbe = LIMA_PBE(addr);
		u32 bte = LIMA_BTE(addr);

		vm->bts[pbe].cpu[bte] = 0;
	}
}

static int lima_vm_map_page(struct lima_vm *vm, dma_addr_t pa, u32 va)
{
	u32 pbe = LIMA_PBE(va);
	u32 bte = LIMA_BTE(va);

	/* See the page-directory check below: the leaf PTE is a u32 too. */
	if (pa > (dma_addr_t)UINT32_MAX - LIMA_PAGE_SIZE + 1) {
		dev_err(vm->dev->dev,
		    "buffer page at %#jx is above 4 GiB; Mali-400's MMU "
		    "entries are 32-bit\n", (uintmax_t)pa);
		return -ERANGE;
	}

	if (!vm->bts[pbe].cpu) {
		dma_addr_t pts;
		u32 *pd;
		int j;

		/*
		 * FreeBSD: use dma_alloc_coherent instead of dma_alloc_wc.
		 * drm-66-kmod LinuxKPI does not expose a write-combining variant.
		 * Coherent is correct and safe on aarch64 (Allwinner A64).
		 * __GFP_NOWARN and __GFP_ZERO are honoured by LinuxKPI.
		 */
		vm->bts[pbe].cpu = dma_alloc_coherent(
			vm->dev->dev,
			LIMA_PAGE_SIZE << LIMA_VM_NUM_PT_PER_BT_SHIFT,
			&vm->bts[pbe].dma,
			GFP_KERNEL | __GFP_NOWARN | __GFP_ZERO);
		if (!vm->bts[pbe].cpu)
			return -ENOMEM;

		pts = vm->bts[pbe].dma;

		/*
		 * Mali-400's MMU is genuinely 32-bit: both the page-directory
		 * and page-table entries below are u32, so a DMA address above
		 * 4 GiB does not fit and the `pts | flags` store would silently
		 * keep only the low 32 bits -- pointing the GPU's MMU at some
		 * unrelated page instead of failing. Refuse instead of
		 * truncating. Unreachable on a 1 GiB A64, reachable on any
		 * board with memory above 4 GiB, which is why it is checked
		 * rather than assumed.
		 */
		if ((pts + (dma_addr_t)LIMA_PAGE_SIZE *
		    LIMA_VM_NUM_PT_PER_BT - 1) > (dma_addr_t)UINT32_MAX) {
			dev_err(vm->dev->dev,
			    "page tables at %#jx are above 4 GiB; Mali-400's "
			    "MMU entries are 32-bit\n", (uintmax_t)pts);
			dma_free_coherent(vm->dev->dev,
			    LIMA_PAGE_SIZE << LIMA_VM_NUM_PT_PER_BT_SHIFT,
			    vm->bts[pbe].cpu, vm->bts[pbe].dma);
			vm->bts[pbe].cpu = NULL;
			return -ERANGE;
		}

		pd = vm->pd.cpu + (pbe << LIMA_VM_NUM_PT_PER_BT_SHIFT);
		for (j = 0; j < LIMA_VM_NUM_PT_PER_BT; j++) {
			pd[j] = pts | LIMA_VM_FLAG_PRESENT;
			pts += LIMA_PAGE_SIZE;
		}
	}

	vm->bts[pbe].cpu[bte] = pa | LIMA_VM_FLAGS_CACHE;

	return 0;
}

static struct lima_bo_va *
lima_vm_bo_find(struct lima_vm *vm, struct lima_bo *bo)
{
	struct lima_bo_va *bo_va, *ret = NULL;

	list_for_each_entry(bo_va, &bo->va, list) {
		if (bo_va->vm == vm) {
			ret = bo_va;
			break;
		}
	}

	return ret;
}

int lima_vm_bo_add(struct lima_vm *vm, struct lima_bo *bo, bool create)
{
	struct lima_bo_va *bo_va;
	struct sg_dma_page_iter sg_iter;
	int offset = 0, err;

	mutex_lock(&bo->lock);

	bo_va = lima_vm_bo_find(vm, bo);
	if (bo_va) {
		bo_va->ref_count++;
		mutex_unlock(&bo->lock);
		return 0;
	}

	/* should not create new bo_va if not asked by caller */
	if (!create) {
		mutex_unlock(&bo->lock);
		return -ENOENT;
	}

	bo_va = kzalloc(sizeof(*bo_va), GFP_KERNEL);
	if (!bo_va) {
		err = -ENOMEM;
		goto err_out0;
	}

	bo_va->vm = vm;
	bo_va->ref_count = 1;

	mutex_lock(&vm->lock);

	err = drm_mm_insert_node(&vm->mm, &bo_va->node, lima_bo_size(bo));
	if (err)
		goto err_out1;

	for_each_sgtable_dma_page(bo->base.sgt, &sg_iter, 0) {
		err = lima_vm_map_page(vm, sg_page_iter_dma_address(&sg_iter),
				       bo_va->node.start + offset);
		if (err)
			goto err_out2;

		offset += PAGE_SIZE;
	}

	mutex_unlock(&vm->lock);

	list_add_tail(&bo_va->list, &bo->va);

	mutex_unlock(&bo->lock);
	return 0;

err_out2:
	if (offset)
		lima_vm_unmap_range(vm, bo_va->node.start,
				    bo_va->node.start + offset - 1);
	drm_mm_remove_node(&bo_va->node);
err_out1:
	mutex_unlock(&vm->lock);
	kfree(bo_va);
err_out0:
	mutex_unlock(&bo->lock);
	return err;
}

void lima_vm_bo_del(struct lima_vm *vm, struct lima_bo *bo)
{
	struct lima_bo_va *bo_va;
	u32 size;

	mutex_lock(&bo->lock);

	bo_va = lima_vm_bo_find(vm, bo);
	if (--bo_va->ref_count > 0) {
		mutex_unlock(&bo->lock);
		return;
	}

	mutex_lock(&vm->lock);

	size = bo->heap_size ? bo->heap_size : bo_va->node.size;
	lima_vm_unmap_range(vm, bo_va->node.start,
			    bo_va->node.start + size - 1);

	drm_mm_remove_node(&bo_va->node);

	mutex_unlock(&vm->lock);

	list_del(&bo_va->list);

	mutex_unlock(&bo->lock);

	kfree(bo_va);
}

u32 lima_vm_get_va(struct lima_vm *vm, struct lima_bo *bo)
{
	struct lima_bo_va *bo_va;
	u32 ret;

	mutex_lock(&bo->lock);

	bo_va = lima_vm_bo_find(vm, bo);
	ret = bo_va->node.start;

	mutex_unlock(&bo->lock);

	return ret;
}

struct lima_vm *lima_vm_create(struct lima_device *dev)
{
	struct lima_vm *vm;

	vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (!vm)
		return NULL;

	vm->dev = dev;
	mutex_init(&vm->lock);
	kref_init(&vm->refcount);

	/* FreeBSD: dma_alloc_coherent — no wc variant in drm-66-kmod LinuxKPI */
	vm->pd.cpu = dma_alloc_coherent(dev->dev, LIMA_PAGE_SIZE,
				&vm->pd.dma,
				GFP_KERNEL | __GFP_NOWARN | __GFP_ZERO);
	if (!vm->pd.cpu)
		goto err_out0;

	if (dev->dlbu_cpu) {
		int err = lima_vm_map_page(vm, dev->dlbu_dma,
					   LIMA_VA_RESERVE_DLBU);
		if (err)
			goto err_out1;
	}

	drm_mm_init(&vm->mm, dev->va_start, dev->va_end - dev->va_start);

	return vm;

err_out1:
	dma_free_coherent(dev->dev, LIMA_PAGE_SIZE, vm->pd.cpu, vm->pd.dma);
err_out0:
	kfree(vm);
	return NULL;
}

void lima_vm_release(struct kref *kref)
{
	struct lima_vm *vm = container_of(kref, struct lima_vm, refcount);
	int i;

	drm_mm_takedown(&vm->mm);

	for (i = 0; i < LIMA_VM_NUM_BT; i++) {
		if (vm->bts[i].cpu)
			dma_free_coherent(vm->dev->dev,
				LIMA_PAGE_SIZE << LIMA_VM_NUM_PT_PER_BT_SHIFT,
				vm->bts[i].cpu, vm->bts[i].dma);
	}

	if (vm->pd.cpu)
		dma_free_coherent(vm->dev->dev, LIMA_PAGE_SIZE,
			vm->pd.cpu, vm->pd.dma);

	kfree(vm);
}

void lima_vm_print(struct lima_vm *vm)
{
	int i, j, k;
	u32 *pd, *pt;

	if (!vm->pd.cpu)
		return;

	pd = vm->pd.cpu;
	for (i = 0; i < LIMA_VM_NUM_BT; i++) {
		if (!vm->bts[i].cpu)
			continue;

		pt = vm->bts[i].cpu;
		for (j = 0; j < LIMA_VM_NUM_PT_PER_BT; j++) {
			int idx = (i << LIMA_VM_NUM_PT_PER_BT_SHIFT) + j;

			printk(KERN_INFO "lima vm pd %03x:%08x\n", idx, pd[idx]);

			for (k = 0; k < LIMA_PAGE_ENT_NUM; k++) {
				u32 pte = *pt++;

				if (pte)
					printk(KERN_INFO "  pt %03x:%08x\n", k, pte);
			}
		}
	}
}

int lima_vm_map_bo(struct lima_vm *vm, struct lima_bo *bo, int pageoff)
{
	struct lima_bo_va *bo_va;
	struct sg_dma_page_iter sg_iter;
	int offset = 0, err;
	u32 base;

	mutex_lock(&bo->lock);

	bo_va = lima_vm_bo_find(vm, bo);
	if (!bo_va) {
		err = -ENOENT;
		goto err_out0;
	}

	mutex_lock(&vm->lock);

	base = bo_va->node.start + (pageoff << PAGE_SHIFT);
	for_each_sgtable_dma_page(bo->base.sgt, &sg_iter, pageoff) {
		err = lima_vm_map_page(vm, sg_page_iter_dma_address(&sg_iter),
				       base + offset);
		if (err)
			goto err_out1;

		offset += PAGE_SIZE;
	}

	mutex_unlock(&vm->lock);

	mutex_unlock(&bo->lock);
	return 0;

err_out1:
	if (offset)
		lima_vm_unmap_range(vm, base, base + offset - 1);
	mutex_unlock(&vm->lock);
err_out0:
	mutex_unlock(&bo->lock);
	return err;
}
