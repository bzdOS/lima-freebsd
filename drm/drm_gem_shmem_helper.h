/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * drm/drm_gem_shmem_helper.h — GEM objects backed by pageable memory.
 *
 * MODULE: hal/lima/drm/drm_gem_shmem_helper.h
 * PURPOSE: FreeBSD implementation of Linux' DRM GEM SHMEM helper API, which
 *          drm-kmod (up to and including drm_v6.6.25_13) does not ship at all.
 * PORTED_FROM: Linux 6.6 include/drm/drm_gem_shmem_helper.h
 *
 * WHY THIS FILE EXISTS AT ALL
 * --------------------------
 * Every render-only SoC DRM driver in Linux (lima, panfrost, v3d, vgem, …)
 * allocates its buffer objects out of shmem: swappable, non-contiguous, pinned
 * on demand and handed to the GPU through an sg_table. drm-kmod ports the
 * *TTM* and *DMA* GEM helpers but not the shmem one — there is no header, no
 * .c file, and no mention of "shmem" in its drm/Makefile — because until now
 * every drm-kmod consumer was a discrete PCI GPU with its own VRAM manager.
 *
 * This header is therefore not a shim: it is the real API, and
 * drm_gem_shmem_helper.c next to it is the real implementation, written
 * against FreeBSD's OBJT_SWAP vm_objects via linuxkpi's shmem_file_setup() /
 * shmem_read_mapping_page(). It deliberately keeps the upstream names and
 * semantics so that (a) unmodified Linux drivers compile against it and (b) it
 * can move into drm-kmod's drm.ko unchanged. Nothing in it is Lima-specific.
 *
 * DIFFERENCES FROM UPSTREAM (all documented in the .c as well)
 *   - Locking: identical to Linux 6.6 — the object's dma_resv protects
 *     @pages, @pages_use_count, @vaddr, @vmap_use_count and @madv.
 *   - drm_gem_get_pages()/drm_gem_put_pages() are #ifdef __linux__ in
 *     drm-kmod, so the .c implements FreeBSD equivalents internally.
 *   - Write-combining: Linux only supports @map_wc on x86 (set_pages_array_wc);
 *     the FreeBSD version uses pmap_page_set_memattr() and therefore also
 *     works on arm64.
 *   - Imported (PRIME) objects can be vmap()ed but not mmap()ed, because
 *     drm-kmod's dmabuf module exports dma_buf_vmap()/dma_buf_vunmap() but no
 *     dma_buf_mmap(). See drm_gem_shmem_mmap().
 */

#ifndef __DRM_GEM_SHMEM_HELPER_H__
#define __DRM_GEM_SHMEM_HELPER_H__

#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_prime.h>

struct dma_buf_attachment;
struct drm_mode_create_dumb;
struct drm_printer;
struct iosys_map;
struct sg_table;

/**
 * struct drm_gem_shmem_object - GEM object backed by pageable memory
 *
 * Field-for-field the Linux 6.6 layout, so drivers that embed this struct as
 * their BO base (Lima's struct lima_bo does) need no changes.
 */
struct drm_gem_shmem_object {
	/** @base: Base GEM object */
	struct drm_gem_object base;

	/** @pages: Page array, NULL while the object is not pinned */
	struct page **pages;

	/**
	 * @pages_use_count: Reference count on @pages.
	 * The pages are released when it reaches zero.
	 * Protected by @base.resv.
	 */
	unsigned int pages_use_count;

	/**
	 * @madv: State for madvise.
	 * 0 is active/in-use, negative means purged, positive values are
	 * driver-specific. Protected by @base.resv.
	 */
	int madv;

	/** @madv_list: List entry for driver-side purgeable tracking */
	struct list_head madv_list;

	/** @sgt: Scatter/gather table of the backing pages, DMA-mapped */
	struct sg_table *sgt;

	/** @vaddr: Kernel virtual address of the backing memory */
	void *vaddr;

	/**
	 * @vmap_use_count: Reference count on @vaddr.
	 * Protected by @base.resv.
	 */
	unsigned int vmap_use_count;

	/** @pages_mark_dirty_on_put: Mark pages dirty when they are put */
	bool pages_mark_dirty_on_put : 1;

	/** @pages_mark_accessed_on_put: Mark pages accessed when put */
	bool pages_mark_accessed_on_put : 1;

	/** @map_wc: Map the object write-combined instead of write-back */
	bool map_wc : 1;
};

#define to_drm_gem_shmem_obj(obj) \
	container_of(obj, struct drm_gem_shmem_object, base)

struct drm_gem_shmem_object *drm_gem_shmem_create(struct drm_device *dev,
    size_t size);
void drm_gem_shmem_free(struct drm_gem_shmem_object *shmem);

void drm_gem_shmem_put_pages(struct drm_gem_shmem_object *shmem);
int drm_gem_shmem_pin(struct drm_gem_shmem_object *shmem);
void drm_gem_shmem_unpin(struct drm_gem_shmem_object *shmem);
int drm_gem_shmem_vmap(struct drm_gem_shmem_object *shmem,
    struct iosys_map *map);
void drm_gem_shmem_vunmap(struct drm_gem_shmem_object *shmem,
    struct iosys_map *map);
int drm_gem_shmem_mmap(struct drm_gem_shmem_object *shmem,
    struct vm_area_struct *vma);

int drm_gem_shmem_madvise(struct drm_gem_shmem_object *shmem, int madv);

/*
 * purpose: test whether an object may be reclaimed under memory pressure
 * input:   shmem — the object; caller holds shmem->base.resv
 * output:  true if drm_gem_shmem_purge() may be called on it
 * effects: none
 */
static inline bool
drm_gem_shmem_is_purgeable(struct drm_gem_shmem_object *shmem)
{
	return ((shmem->madv > 0) && !shmem->vmap_use_count && shmem->sgt &&
	    !shmem->base.dma_buf && !shmem->base.import_attach);
}

void drm_gem_shmem_purge(struct drm_gem_shmem_object *shmem);

struct sg_table *drm_gem_shmem_get_sg_table(
    struct drm_gem_shmem_object *shmem);
struct sg_table *drm_gem_shmem_get_pages_sgt(
    struct drm_gem_shmem_object *shmem);

void drm_gem_shmem_print_info(const struct drm_gem_shmem_object *shmem,
    struct drm_printer *p, unsigned int indent);

extern const struct vm_operations_struct drm_gem_shmem_vm_ops;

/*
 * GEM object function wrappers.
 *
 * These are static inline (exactly as upstream) rather than exported
 * functions, so a driver's drm_gem_object_funcs table can point straight at
 * them with no type casts and no extra relocations.
 */

/*
 * purpose: &drm_gem_object_funcs.free handler
 * input:   obj — GEM object whose last reference just went away
 * output:  none
 * effects: releases pages/sgt/vaddr and frees the object
 */
static inline void
drm_gem_shmem_object_free(struct drm_gem_object *obj)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	drm_gem_shmem_free(shmem);
}

/*
 * purpose: &drm_gem_object_funcs.print_info handler
 * input:   p — printer, indent — tab depth, obj — GEM object
 * output:  none
 * effects: prints the object's shmem state to @p
 */
static inline void
drm_gem_shmem_object_print_info(struct drm_printer *p, unsigned int indent,
    const struct drm_gem_object *obj)
{
	const struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	drm_gem_shmem_print_info(shmem, p, indent);
}

/*
 * purpose: &drm_gem_object_funcs.pin handler
 * input:   obj — GEM object
 * output:  0 or negative errno
 * effects: pins the backing pages for the lifetime of the export
 */
static inline int
drm_gem_shmem_object_pin(struct drm_gem_object *obj)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	return (drm_gem_shmem_pin(shmem));
}

/*
 * purpose: &drm_gem_object_funcs.unpin handler
 * input:   obj — GEM object
 * output:  none
 * effects: drops the pin taken by drm_gem_shmem_object_pin()
 */
static inline void
drm_gem_shmem_object_unpin(struct drm_gem_object *obj)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	drm_gem_shmem_unpin(shmem);
}

/*
 * purpose: &drm_gem_object_funcs.get_sg_table handler
 * input:   obj — GEM object, already pinned
 * output:  sg_table, or ERR_PTR on failure
 * effects: allocates a fresh sg_table describing the pinned pages
 */
static inline struct sg_table *
drm_gem_shmem_object_get_sg_table(struct drm_gem_object *obj)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	return (drm_gem_shmem_get_sg_table(shmem));
}

/*
 * purpose: &drm_gem_object_funcs.vmap handler
 * input:   obj — GEM object, map — receives the kernel mapping
 * output:  0 or negative errno
 * effects: creates (or references) a kernel mapping of the object
 */
static inline int
drm_gem_shmem_object_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	return (drm_gem_shmem_vmap(shmem, map));
}

/*
 * purpose: &drm_gem_object_funcs.vunmap handler
 * input:   obj — GEM object, map — mapping returned by vmap
 * output:  none
 * effects: drops a reference on the kernel mapping
 */
static inline void
drm_gem_shmem_object_vunmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	drm_gem_shmem_vunmap(shmem, map);
}

/*
 * purpose: &drm_gem_object_funcs.mmap handler
 * input:   obj — GEM object, vma — the mapping being set up
 * output:  0 or negative errno
 * effects: pins pages and arms the fault handler for @vma
 */
static inline int
drm_gem_shmem_object_mmap(struct drm_gem_object *obj,
    struct vm_area_struct *vma)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	return (drm_gem_shmem_mmap(shmem, vma));
}

/*
 * Driver ops
 */

struct drm_gem_object *drm_gem_shmem_prime_import_sg_table(
    struct drm_device *dev, struct dma_buf_attachment *attach,
    struct sg_table *sgt);
int drm_gem_shmem_dumb_create(struct drm_file *file, struct drm_device *dev,
    struct drm_mode_create_dumb *args);

/**
 * DRM_GEM_SHMEM_DRIVER_OPS - Default shmem GEM operations
 */
#define DRM_GEM_SHMEM_DRIVER_OPS \
	.gem_prime_import_sg_table = drm_gem_shmem_prime_import_sg_table, \
	.dumb_create		   = drm_gem_shmem_dumb_create

#endif /* __DRM_GEM_SHMEM_HELPER_H__ */
