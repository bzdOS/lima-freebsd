// SPDX-License-Identifier: GPL-2.0 OR MIT
// Copyright 2017-2019 Qiang Yu <yuq825@gmail.com>
// FreeBSD port: bsdOS project, 2026

// MODULE: hal/lima/lima_gem.h
// PURPOSE: Lima GEM buffer object type and inline accessors for Mali-400 GPU
// PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_gem.h
// sideEffects: none — declarations only

#ifndef __LIMA_GEM_H__
#define __LIMA_GEM_H__

/*
 * drm-66-kmod on FreeBSD provides drm_gem_shmem_helper.h via LinuxKPI.
 * The struct drm_gem_shmem_object and all its helpers are available
 * unchanged; no substitution required for this header.
 */
#include <drm/drm_gem_shmem_helper.h>

struct lima_submit;
struct lima_vm;

/**
 * struct lima_bo - Lima GEM buffer object
 *
 * purpose:   Represents a GPU buffer allocation visible to the Mali-400 MMU.
 *            Extends drm_gem_shmem_object with a per-BO VA mapping list and
 *            optional heap tracking for tiler/fragment scratch BOs.
 * sideEffects: @lock serialises VA list mutations across submit/map/unmap.
 */
struct lima_bo {
	struct drm_gem_shmem_object base;

	struct mutex     lock;
	struct list_head va;

	size_t heap_size;
};

/**
 * to_lima_bo - cast drm_gem_object pointer to lima_bo
 * purpose:  Recover the embedding lima_bo from a generic DRM GEM object.
 * input:    obj - pointer to drm_gem_object embedded in a drm_gem_shmem_object
 *               which is itself embedded in lima_bo.base
 * output:   pointer to the enclosing lima_bo
 * sideEffects: none
 *
 * LinuxKPI exposes container_of() and to_drm_gem_shmem_obj() identically
 * to Linux; no porting changes required here.
 */
static inline struct lima_bo *
to_lima_bo(struct drm_gem_object *obj)
{
	return container_of(to_drm_gem_shmem_obj(obj), struct lima_bo, base);
}

/**
 * lima_bo_size - return the byte size of a lima_bo backing store
 * purpose:  Thin accessor to avoid open-coding the .base.base.size chain.
 * input:    bo - valid lima_bo pointer
 * output:   size in bytes of the GEM object's backing allocation
 * sideEffects: none
 */
static inline size_t lima_bo_size(struct lima_bo *bo)
{
	return bo->base.base.size;
}

/**
 * lima_bo_resv - return the dma_resv (fence/reservation) object for a BO
 * purpose:  Accessor used by submit and wait paths to attach/query fences.
 * input:    bo - valid lima_bo pointer
 * output:   pointer to the dma_resv embedded in the GEM base object
 * sideEffects: none
 */
static inline struct dma_resv *lima_bo_resv(struct lima_bo *bo)
{
	return bo->base.base.resv;
}

/* ── function declarations ──────────────────────────────────────────────── */

/* purpose: allocate tiler/fragment heap backing for a BO in vm
 * input:   bo - heap BO to grow, vm - target GPU VM
 * output:  0 on success, -errno on failure                               */
int lima_heap_alloc(struct lima_bo *bo, struct lima_vm *vm);

/* purpose: allocate a lima_bo and initialise shmem backing
 * input:   dev - DRM device, size - allocation size in bytes
 * output:  pointer to embedded drm_gem_object, or ERR_PTR on failure    */
struct drm_gem_object *lima_gem_create_object(struct drm_device *dev,
					      size_t size);

/* purpose: create a BO and return a userspace GEM handle
 * input:   dev, file - DRM context; size, flags - BO parameters
 * output:  *handle set to new GEM handle; returns 0 or -errno            */
int lima_gem_create_handle(struct drm_device *dev, struct drm_file *file,
			   u32 size, u32 flags, u32 *handle);

/* purpose: query the GPU VA and mmap offset for an allocated BO
 * input:   file - drm_file context, handle - GEM handle
 * output:  *va set to GPU VA, *offset set to mmap cookie; 0 or -errno   */
int lima_gem_get_info(struct drm_file *file, u32 handle, u32 *va, u64 *offset);

/* purpose: submit a batch of GP/PP jobs to the Lima scheduler
 * input:   file - drm_file, submit - validated job descriptor
 * output:  0 on success, -errno on scheduler or fence error              */
int lima_gem_submit(struct drm_file *file, struct lima_submit *submit);

/* purpose: wait for GPU completion fence(s) on a BO
 * input:   file, handle - BO to wait on
 *          op - LIMA_GEM_WAIT_READ / LIMA_GEM_WAIT_WRITE bitmask
 *          timeout_ns - max wait; negative means infinite
 * output:  0 on success, -ETIME on timeout, -errno on error             */
int lima_gem_wait(struct drm_file *file, u32 handle, u32 op, s64 timeout_ns);

/* purpose: configure vm_area_struct page-protection for Lima BO CPU mmap
 * input:   vma - vm_area_struct from do_mmap / drm_gem_mmap
 * output:  none (mutates vma->vm_page_prot and vm_flags)
 * sideEffects: sets write-combining pgprot so Mali shader traffic is coherent
 *
 * LinuxKPI provides vm_area_struct + pgprot_writecombine() on FreeBSD;
 * implementation in lima_gem.c requires no porting changes.             */
void lima_set_vma_flags(struct vm_area_struct *vma);

#endif /* __LIMA_GEM_H__ */
