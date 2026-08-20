// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright 2018 Noralf Trønnes (original Linux implementation)
 * FreeBSD port: bsdOS Project, 2026
 */

// MODULE: hal/lima/drm/drm_gem_shmem_helper.c
// PURPOSE: DRM GEM objects backed by pageable (swap-object) memory — the
//          FreeBSD implementation of Linux' GEM SHMEM helper library, which
//          drm-kmod does not provide in any released version.
// PORTED_FROM: Linux 6.6 drivers/gpu/drm/drm_gem_shmem_helper.c
//
// ─── What this replaces ──────────────────────────────────────────────────────
// drm-kmod (tag drm_v6.6.25_13, the newest) ships neither
// include/drm/drm_gem_shmem_helper.h nor this .c, and its drm/Makefile never
// mentions shmem. Every render-only SoC DRM driver in Linux is built on it, so
// on FreeBSD each such driver links but cannot load: the drm_gem_shmem_*
// symbols resolve nowhere. This file resolves them for real.
//
// ─── How FreeBSD backs a "shmem" GEM object ──────────────────────────────────
// linuxkpi's shmem_file_setup() (sys/compat/linuxkpi/common/src/linux_shmemfs.c)
// allocates an OBJT_SWAP vm_object and hangs it off linux_file::f_shmem;
// drm_gem_object_init() already calls it, so obj->filp->f_shmem is our backing
// store. Individual pages are obtained with shmem_read_mapping_page(), which
// wires them (VM_ALLOC_WIRED, not busied) and returns a vm_page_t — which is
// what linuxkpi calls a struct page. put_page() unwires them again.
//
// Note that drm-kmod's own drm_gem_get_pages()/drm_gem_put_pages() are inside
// an "#ifdef __linux__" block, so they exist as declarations only on FreeBSD;
// the two static helpers below are their FreeBSD equivalents.
//
// ─── Locking ─────────────────────────────────────────────────────────────────
// Same model as Linux 6.6: the GEM object's dma_resv (a ww_mutex) protects
// ->pages, ->pages_use_count, ->vaddr, ->vmap_use_count, ->sgt and ->madv.
// Functions ending in _locked expect it held; the public entry points take it.
//
// ─── Deliberate deviations, and their consequences ───────────────────────────
//  1. Write-combining actually works here. Linux guards set_pages_array_wc()
//     with CONFIG_X86 and leaves other architectures write-back; FreeBSD's
//     pmap_page_set_memattr() is machine-independent, so map_wc objects get
//     VM_MEMATTR_WRITE_COMBINING on arm64 too (which on arm64 is defined as
//     write-through — see sys/arm64/include/vm.h).
//  2. No GFP zone constraint. Linux calls mapping_set_gfp_mask(__GFP_DMA32)
//     for 32-bit GPUs; a FreeBSD OBJT_SWAP object has no equivalent knob, so
//     pages may live above 4 GiB. Harmless on the A64 (max 3 GiB of DRAM) but
//     a real limitation for a 32-bit GPU in a large-memory machine, and one
//     that has to be fixed in linuxkpi, not here.
//  3. Imported PRIME objects can now be mmap()ed: drm-kmod gained
//     dma_buf_mmap() (bzdOS 2026-08-11, patches/drm-kmod-dma-buf-mmap.patch),
//     and drm_gem_shmem_mmap()'s import branch below calls it instead of
//     returning -EOPNOTSUPP. NOT verified on hardware. That FreeBSD port of
//     dma_buf_mmap() also knowingly omits one piece of upstream's reference
//     bookkeeping (vma_set_file() does not typecheck here) -- see the
//     comment on drm_gem_shmem_mmap()'s import branch and
//     patches/README.md for exactly what that does and does not affect.
//  4. drm_gem_shmem_purge() now invalidates already-mapped pages too. It does
//     not call Linux's drm_vma_node_unmap(dev->anon_inode) -- drm-kmod's own
//     FreeBSD side has no anon_inode to give it (struct drm_device::anon_inode
//     is "#ifdef __linux__" in drm-kmod's drm_device.h, and drm_dev_unplug()
//     marks the equivalent call "/* FreeBSD TODO */" in drm_drv.c). Instead it
//     calls pmap_remove_all() directly on each backing page, wherever that
//     page currently lives, deliberately bypassing linuxkpi's own
//     cdev_pager_lookup()-based unmap_mapping_range() (which is reachable but
//     self-deadlocks here on a real, if narrow, race -- see the comment on
//     drm_gem_shmem_purge() and drm/PURGE-NOTES.md). NOT verified on hardware.
//     Nothing in this tree calls drm_gem_shmem_purge() at all today (Lima has
//     no madvise ioctl and no shrinker, matching upstream Lima), so this gap
//     was latent, not active.
//
// NOTHING IN THIS FILE HAS RUN ON HARDWARE YET. It compiles for aarch64 under
// -Werror with the full kmod flag set and links into lima.ko; that is all that
// has been demonstrated.

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/iosys-map.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <sys/sysctl.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/pmap.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>

#include "drm_gem_shmem_logic.h"

static const struct drm_gem_object_funcs drm_gem_shmem_funcs = {
	.free = drm_gem_shmem_object_free,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = drm_gem_shmem_object_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

/* ── FreeBSD equivalents of drm_gem_get_pages()/drm_gem_put_pages() ───────── */

/*
 * purpose: try to back a fresh BO with ONE physically contiguous run of pages
 * input:   vm_obj — the object's OBJT_SWAP backing store, npages — its size
 * output:  true if the object now holds npages contiguous resident pages
 * effects: allocates and inserts them, zeroed and valid; nothing on failure
 *
 * WHY CONTIGUITY IS WORTH ASKING FOR
 *
 * The GPU does not care: lima maps each page individually into the Mali MMU,
 * so a scattered BO renders perfectly. The DISPLAY does care. Allwinner's DE2
 * mixer fetches a layer from one base address plus a fixed pitch and there is
 * no IOMMU in front of it, so a buffer it can scan out must be one run. That
 * makes contiguity the single thing standing between "the GPU renders into a
 * gbm_surface at 332 fps" and "that surface appears on the monitor with no copy
 * at all" -- measured on hardware 2026-08-20, where every frame was refused
 * because page 1 landed 0x2000 BELOW page 0.
 *
 * Attempted for every BO rather than only display-sized ones, because nothing
 * in the GEM_CREATE ioctl tells us which buffers userspace intends to scan out
 * (lima's only flag is LIMA_BO_FLAG_HEAP) and guessing from the size would be
 * a worse contract than simply preferring contiguous memory throughout. It is
 * a PREFERENCE, never a requirement: on failure the caller's per-page path runs
 * exactly as before and the BO is as good as it ever was -- only unscannable.
 *
 * Bounded at CONTIG_MAX_PAGES so a large texture or a heap BO cannot turn one
 * allocation into a long reclaim; single-page objects skip it because a single
 * page is trivially contiguous. Pattern (allocate, zero if not PG_ZERO, mark
 * valid, unbusy) follows uipc_shm.c's large-page path, which populates the same
 * kind of swap object.
 */
#define CONTIG_MAX_PAGES 4096UL   /* 16 MiB */

/*
 * Runtime switch, default ON, so the contiguous path can be taken out of the
 * picture without swapping modules or rebooting:
 *
 *     sysctl compat.linuxkpi.lima_shmem_contig=0
 *
 * It exists because this path was suspected of leaking and the A/B has to be one
 * command, not a rebuild. IT DOES NOT LEAK -- established by measuring around the
 * module rather than around the workload:
 *
 *   no modules loaded          wired 128744
 *   lima + bzfb loaded         wired 129193   (+449)
 *   after one presenting run   wired 130938   (+1745)
 *   after unloading both       wired 128748   (net +4 -- all 2190 returned)
 *
 * and it does not grow under sustained use: six further runs added 53 pages. So
 * the growth is driver-lifetime memory that comes back in full, not orphaned
 * pages. The earlier "+1727 pages per run" reading was v_wire_count sampled
 * around the workload on a machine with almost no free memory, which gave
 * numbers between 4 and 60868 for the same work; none of them meant anything.
  */
static int shmem_contig = 1;
/*
 * Deterministic leak accounting. v_wire_count sampling was far too noisy to
 * characterise this (readings of 4, 228, 1041 and 1702 pages for the same
 * workload), so count the two quantities that must match instead: pages handed
 * out by the contiguous path, and pages that come back through put_pages.
 */
static unsigned long contig_alloc_pages;
static unsigned long teardown_pages;
static unsigned long teardown_objects;
SYSCTL_ULONG(_compat_linuxkpi, OID_AUTO, lima_contig_alloc_pages, CTLFLAG_RD,
    &contig_alloc_pages, 0, "pages allocated by the contiguous BO path");
SYSCTL_ULONG(_compat_linuxkpi, OID_AUTO, lima_teardown_pages, CTLFLAG_RD,
    &teardown_pages, 0, "pages released through drm_gem_shmem put_pages");
SYSCTL_ULONG(_compat_linuxkpi, OID_AUTO, lima_teardown_objects, CTLFLAG_RD,
    &teardown_objects, 0, "objects released through drm_gem_shmem put_pages");
SYSCTL_INT(_compat_linuxkpi, OID_AUTO, lima_shmem_contig, CTLFLAG_RWTUN,
    &shmem_contig, 0,
    "back GEM shmem BOs with one physically contiguous run when possible "
    "(required for DE2 scanout; 0 restores the per-page layout)");

static vm_page_t
drm_gem_shmem_freebsd_try_contig(vm_object_t vm_obj, unsigned long npages)
{
	vm_page_t m;
	unsigned long i;

	if (shmem_contig == 0 || npages < 2 || npages > CONTIG_MAX_PAGES)
		return (NULL);

	VM_OBJECT_WLOCK(vm_obj);
	/*
	 * Only ever populate an untouched object. A partly resident one would
	 * make vm_page_alloc_contig() fail anyway, and asking is cheaper than
	 * discovering it.
	 */
	if (vm_obj->resident_page_count != 0) {
		VM_OBJECT_WUNLOCK(vm_obj);
		return (NULL);
	}
	/*
	 * VM_ALLOC_WIRED matters for correctness, not for pinning: it makes THIS
	 * function the single owner of exactly one wire per page, the same
	 * contract shemem_read_mapping_page() gives the per-page path, so
	 * put_page() balances it exactly once.
	 *
	 * The first version allocated unwired and let the grab loop below wire
	 * the pages, so two parties held state for one page. That is an
	 * unbalanced contract even though it did not orphan memory: fixing it
	 * cut the residual growth of repeated runs from ~85 pages per run to
	 * ~9, which is the measurable part of the difference.
	 */
	m = vm_page_alloc_contig(vm_obj, 0,
	    VM_ALLOC_NORMAL | VM_ALLOC_ZERO | VM_ALLOC_WIRED,
	    npages, 0, ~(vm_paddr_t)0, PAGE_SIZE, 0, VM_MEMATTR_DEFAULT);
	if (m == NULL) {
		VM_OBJECT_WUNLOCK(vm_obj);
		return (NULL);
	}
	contig_alloc_pages += npages;
	for (i = 0; i < npages; i++) {
		if ((m[i].flags & PG_ZERO) == 0)
			pmap_zero_page(&m[i]);
		vm_page_valid(&m[i]);
		vm_page_xunbusy(&m[i]);
	}
	VM_OBJECT_WUNLOCK(vm_obj);
	return (m);
}

/*
 * purpose: fault in and wire every page of the object's swap backing store
 * input:   obj — GEM object initialised by drm_gem_object_init() (so
 *                obj->filp->f_shmem is a valid OBJT_SWAP vm_object)
 * output:  kvmalloc'ed array of obj->size >> PAGE_SHIFT pages, or ERR_PTR
 * effects: allocates the array; wires one reference per page. Release with
 *          drm_gem_shmem_freebsd_put_pages().
 */
static struct page **
drm_gem_shmem_freebsd_get_pages(struct drm_gem_object *obj)
{
	struct page **pages;
	struct page *p;
	vm_page_t m;
	vm_object_t vm_obj;
	unsigned long i, npages;

	if (obj->filp == NULL || obj->filp->f_shmem == NULL)
		return (ERR_PTR(-EINVAL));

	vm_obj = obj->filp->f_shmem;
	npages = drm_gem_shmem_npages(obj->size);
	if (npages == 0)
		return (ERR_PTR(-EINVAL));

	pages = kvmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
	if (pages == NULL)
		return (ERR_PTR(-ENOMEM));

	/*
	 * Prefer one contiguous run. This only PRE-POPULATES the object; the
	 * loop below still fetches every page through shmem_read_mapping_page(),
	 * which finds and wires the pages already sitting there. So the two
	 * paths differ in the physical layout they produce and in nothing else,
	 * and a failure here is invisible to everything except a consumer that
	 * needs to scan the buffer out.
	 */
	m = drm_gem_shmem_freebsd_try_contig(vm_obj, npages);
	if (m != NULL) {
		/*
		 * Hand back the run itself. Re-grabbing each page through
		 * shmem_read_mapping_page() would add a SECOND wire reference
		 * that nothing drops, which is what the first version of this
		 * did. The pages are in the object, valid, and wired exactly
		 * once, so put_page() in put_pages() releases them on the very
		 * same path as the per-page allocation below.
		 */
		for (i = 0; i < npages; i++)
			pages[i] = &m[i];
		return (pages);
	}

	p = NULL;
	for (i = 0; i < npages; i++) {
		/*
		 * Grabs the page, allocating or swapping it in as needed, and
		 * returns it wired but not busy. The gfp argument is fixed at
		 * 0 by linuxkpi's shmem_read_mapping_page() macro.
		 */
		p = shmem_read_mapping_page(vm_obj, i);
		if (IS_ERR(p))
			goto fail;
		pages[i] = p;
	}

	return (pages);

fail:
	while (i-- > 0)
		put_page(pages[i]);
	kvfree(pages);

	return (ERR_CAST(p));
}

/*
 * purpose: drop the wirings taken by drm_gem_shmem_freebsd_get_pages()
 * input:   obj — GEM object, pages — the array, dirty/accessed — whether to
 *          mark each page dirty / referenced before releasing it
 * output:  none
 * effects: unwires every page (the swap object keeps them until reclaimed) and
 *          frees the array
 */
static void
drm_gem_shmem_freebsd_put_pages(struct drm_gem_object *obj,
    struct page **pages, bool dirty, bool accessed)
{
	unsigned long i, npages;

	npages = drm_gem_shmem_npages(obj->size);

	teardown_objects++;
	teardown_pages += npages;

	for (i = 0; i < npages; i++) {
		if (pages[i] == NULL)
			continue;
		if (dirty)
			set_page_dirty(pages[i]);
		if (accessed)
			mark_page_accessed(pages[i]);
		put_page(pages[i]);
	}

	kvfree(pages);
}

/*
 * purpose: change the cache attribute of every backing page
 * input:   pages — page array, npages — its length, attr — vm_memattr_t
 * output:  none
 * effects: rewrites the pages' memattr, which pmap_qenter()-based mappings
 *          (i.e. vmap()) and the fault path both honour
 *
 * This is the machine-independent stand-in for Linux' x86-only
 * set_pages_array_wc()/set_pages_array_wb(). It must be undone before the
 * pages go back to the VM, or unrelated future users of those pages would
 * inherit a non-default cache mode.
 */
static void
drm_gem_shmem_set_pages_memattr(struct page **pages, unsigned long npages,
    vm_memattr_t attr)
{
	unsigned long i;

	for (i = 0; i < npages; i++) {
		/*
		 * A sparsely-populated pages[] is legitimate: lima's growable
		 * heap BO (LIMA_BO_FLAG_HEAP) allocates the array for the
		 * object's FULL size but faults pages in only up to heap_size,
		 * leaving the tail NULL until the GP runs out of tile-list space
		 * and grows it. Callers here pass npages for the whole object, so
		 * without this check the first unpopulated entry reaches
		 * pmap_page_set_memattr(NULL, ...) and panics --
		 *
		 *   far 0x3c   elr pmap_page_set_memattr   pmap.c:8168
		 *
		 * measured on 2026-08-19, the moment heap growth was first
		 * implemented. drm_gem_shmem_freebsd_put_pages() already skips
		 * NULL for exactly the same reason; this path was simply never
		 * reached while heap BOs returned -ENOSYS.
		 */
		if (pages[i] == NULL)
			continue;
		pmap_page_set_memattr(pages[i], attr);
	}
}

/* ── create / free ────────────────────────────────────────────────────────── */

/*
 * purpose: allocate and initialise a shmem GEM object
 * input:   dev — DRM device, size — requested size in bytes, private — true
 *          for objects whose storage comes from elsewhere (PRIME import)
 * output:  the object, or ERR_PTR
 * effects: allocates the object (through the driver's gem_create_object hook
 *          when present), its backing swap object and its mmap offset
 */
static struct drm_gem_shmem_object *
__drm_gem_shmem_create(struct drm_device *dev, size_t size, bool private)
{
	struct drm_gem_shmem_object *shmem;
	struct drm_gem_object *obj;
	int ret = 0;

	size = drm_gem_shmem_align_size(size);

	if (dev->driver->gem_create_object) {
		obj = dev->driver->gem_create_object(dev, size);
		if (IS_ERR(obj))
			return (ERR_CAST(obj));
		shmem = to_drm_gem_shmem_obj(obj);
	} else {
		shmem = kzalloc(sizeof(*shmem), GFP_KERNEL);
		if (shmem == NULL)
			return (ERR_PTR(-ENOMEM));
		obj = &shmem->base;
	}

	if (obj->funcs == NULL)
		obj->funcs = &drm_gem_shmem_funcs;

	if (private) {
		drm_gem_private_object_init(dev, obj, size);
		/* dma-buf mappings are always write-combined. */
		shmem->map_wc = false;
	} else {
		ret = drm_gem_object_init(dev, obj, size);
	}
	if (ret) {
		drm_gem_private_object_fini(obj);
		goto err_free;
	}

	ret = drm_gem_create_mmap_offset(obj);
	if (ret)
		goto err_release;

	INIT_LIST_HEAD(&shmem->madv_list);

	/*
	 * Linux additionally pins the mapping's GFP mask here
	 * (GFP_HIGHUSER | __GFP_RETRY_MAYFAIL | __GFP_NOWARN, plus
	 * __GFP_DMA32 for 32-bit GPUs). FreeBSD's OBJT_SWAP objects have no
	 * per-object allocation policy, so there is nothing to set — see
	 * deviation 2 in the file header.
	 */

	return (shmem);

err_release:
	drm_gem_object_release(obj);
err_free:
	kfree(obj);

	return (ERR_PTR(ret));
}

/*
 * purpose: allocate a shmem GEM object of the given size
 * input:   dev — DRM device, size — size in bytes (rounded up to a page)
 * output:  the object, or ERR_PTR
 * effects: see __drm_gem_shmem_create()
 */
struct drm_gem_shmem_object *
drm_gem_shmem_create(struct drm_device *dev, size_t size)
{
	return (__drm_gem_shmem_create(dev, size, false));
}

/*
 * purpose: release everything owned by a shmem GEM object and free it
 * input:   shmem — the object, at zero references
 * output:  none
 * effects: unmaps and frees the sg table, unwires the pages, releases the GEM
 *          object and frees the containing allocation
 */
void
drm_gem_shmem_free(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;

	if (obj->import_attach) {
		drm_prime_gem_destroy(obj, shmem->sgt);
	} else {
		dma_resv_lock(shmem->base.resv, NULL);

		drm_WARN_ON(obj->dev, shmem->vmap_use_count);

		if (shmem->sgt) {
			dma_unmap_sgtable(obj->dev->dev, shmem->sgt,
			    DMA_BIDIRECTIONAL, 0);
			sg_free_table(shmem->sgt);
			kfree(shmem->sgt);
		}
		if (shmem->pages)
			drm_gem_shmem_put_pages(shmem);

		drm_WARN_ON(obj->dev, shmem->pages_use_count);

		dma_resv_unlock(shmem->base.resv);
	}

	drm_gem_object_release(obj);
	kfree(shmem);
}

/* ── pages ────────────────────────────────────────────────────────────────── */

/*
 * purpose: take a reference on the object's backing pages, faulting them in on
 *          the first reference
 * input:   shmem — the object; caller holds shmem->base.resv
 * output:  0 or negative errno
 * effects: on the first reference, populates shmem->pages and applies the
 *          write-combining attribute when requested
 */
static int
drm_gem_shmem_get_pages(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	struct page **pages;

	dma_resv_assert_held(shmem->base.resv);

	if (!drm_gem_shmem_ref_first(&shmem->pages_use_count))
		return (0);

	pages = drm_gem_shmem_freebsd_get_pages(obj);
	if (IS_ERR(pages)) {
		drm_dbg_kms(obj->dev, "Failed to get pages (%ld)\n",
		    PTR_ERR(pages));
		shmem->pages_use_count = 0;
		return (PTR_ERR(pages));
	}

	if (shmem->map_wc) {
		drm_gem_shmem_set_pages_memattr(pages,
		    drm_gem_shmem_npages(obj->size),
		    VM_MEMATTR_WRITE_COMBINING);
	}

	shmem->pages = pages;

	return (0);
}

/*
 * purpose: drop a reference on the object's backing pages
 * input:   shmem — the object; caller holds shmem->base.resv
 * output:  none
 * effects: on the last reference, restores the default cache attribute and
 *          unwires the pages
 */
void
drm_gem_shmem_put_pages(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	enum drm_gem_shmem_unref state;

	dma_resv_assert_held(shmem->base.resv);

	state = drm_gem_shmem_unref(&shmem->pages_use_count);
	if (state == DRM_GEM_SHMEM_UNREF_UNDERFLOW) {
		drm_WARN_ON_ONCE(obj->dev, 1);
		return;
	}
	if (state == DRM_GEM_SHMEM_UNREF_KEEP)
		return;

	if (shmem->map_wc) {
		drm_gem_shmem_set_pages_memattr(shmem->pages,
		    drm_gem_shmem_npages(obj->size), VM_MEMATTR_DEFAULT);
	}

	drm_gem_shmem_freebsd_put_pages(obj, shmem->pages,
	    shmem->pages_mark_dirty_on_put,
	    shmem->pages_mark_accessed_on_put);
	shmem->pages = NULL;
}

/*
 * purpose: pin the backing pages for as long as the buffer is exported
 * input:   shmem — the object, not imported
 * output:  0 or negative errno (including -EINTR from the interruptible lock)
 * effects: takes a pages reference
 */
int
drm_gem_shmem_pin(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret;

	drm_WARN_ON(obj->dev, obj->import_attach);

	ret = dma_resv_lock_interruptible(shmem->base.resv, NULL);
	if (ret)
		return (ret);
	ret = drm_gem_shmem_get_pages(shmem);
	dma_resv_unlock(shmem->base.resv);

	return (ret);
}

/*
 * purpose: undo drm_gem_shmem_pin()
 * input:   shmem — the object, not imported
 * output:  none
 * effects: drops a pages reference
 */
void
drm_gem_shmem_unpin(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;

	drm_WARN_ON(obj->dev, obj->import_attach);

	dma_resv_lock(shmem->base.resv, NULL);
	drm_gem_shmem_put_pages(shmem);
	dma_resv_unlock(shmem->base.resv);
}

/* ── kernel mapping ───────────────────────────────────────────────────────── */

/*
 * purpose: create (or reference) a contiguous kernel mapping of the object
 * input:   shmem — the object, map — receives the mapping
 * output:  0 or negative errno
 * effects: pins the pages and vmap()s them; imported objects are delegated to
 *          the exporter via dma_buf_vmap()
 *
 * For native objects the caller must hold shmem->base.resv (as upstream);
 * imported objects do not need it.
 */
int
drm_gem_shmem_vmap(struct drm_gem_shmem_object *shmem, struct iosys_map *map)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret = 0;

	if (obj->import_attach) {
		ret = dma_buf_vmap(obj->import_attach->dmabuf, map);
		if (ret == 0) {
			if (drm_WARN_ON(obj->dev, map->is_iomem)) {
				dma_buf_vunmap(obj->import_attach->dmabuf, map);
				return (-EIO);
			}
		}
	} else {
		dma_resv_assert_held(shmem->base.resv);

		if (!drm_gem_shmem_ref_first(&shmem->vmap_use_count)) {
			iosys_map_set_vaddr(map, shmem->vaddr);
			return (0);
		}

		ret = drm_gem_shmem_get_pages(shmem);
		if (ret)
			goto err_zero_use;

		/*
		 * linuxkpi's vmap() maps through pmap_qenter(), which takes the
		 * cache mode from each page's memattr rather than from the
		 * pgprot argument. drm_gem_shmem_get_pages() has already set
		 * VM_MEMATTR_WRITE_COMBINING on the pages when map_wc is
		 * requested, so PAGE_KERNEL is correct here in both cases.
		 */
		shmem->vaddr = vmap(shmem->pages,
		    drm_gem_shmem_npages(obj->size), VM_MAP, PAGE_KERNEL);
		if (shmem->vaddr == NULL)
			ret = -ENOMEM;
		else
			iosys_map_set_vaddr(map, shmem->vaddr);
	}

	if (ret) {
		drm_dbg_kms(obj->dev, "Failed to vmap pages, error %d\n", ret);
		goto err_put_pages;
	}

	return (0);

err_put_pages:
	if (!obj->import_attach)
		drm_gem_shmem_put_pages(shmem);
err_zero_use:
	shmem->vmap_use_count = 0;

	return (ret);
}

/*
 * purpose: drop a reference on the kernel mapping
 * input:   shmem — the object, map — the mapping returned by vmap
 * output:  none
 * effects: on the last reference, tears the mapping down and unpins the pages
 */
void
drm_gem_shmem_vunmap(struct drm_gem_shmem_object *shmem, struct iosys_map *map)
{
	struct drm_gem_object *obj = &shmem->base;
	enum drm_gem_shmem_unref state;

	if (obj->import_attach) {
		dma_buf_vunmap(obj->import_attach->dmabuf, map);
	} else {
		dma_resv_assert_held(shmem->base.resv);

		state = drm_gem_shmem_unref(&shmem->vmap_use_count);
		if (state == DRM_GEM_SHMEM_UNREF_UNDERFLOW) {
			drm_WARN_ON_ONCE(obj->dev, 1);
			return;
		}
		if (state == DRM_GEM_SHMEM_UNREF_KEEP)
			return;

		vunmap(shmem->vaddr);
		drm_gem_shmem_put_pages(shmem);
	}

	shmem->vaddr = NULL;
}

/* ── dumb buffers ─────────────────────────────────────────────────────────── */

/*
 * purpose: allocate a shmem object and give the caller a handle to it
 * input:   file_priv — DRM file, dev — device, size — bytes, handle — out
 * output:  0 or negative errno
 * effects: creates the object; the handle owns the only reference on return
 */
static int
drm_gem_shmem_create_with_handle(struct drm_file *file_priv,
    struct drm_device *dev, size_t size, uint32_t *handle)
{
	struct drm_gem_shmem_object *shmem;
	int ret;

	shmem = drm_gem_shmem_create(dev, size);
	if (IS_ERR(shmem))
		return (PTR_ERR(shmem));

	ret = drm_gem_handle_create(file_priv, &shmem->base, handle);
	/* Drop the reference from the allocation; the handle holds one now. */
	drm_gem_object_put(&shmem->base);

	return (ret);
}

/*
 * purpose: &drm_driver.dumb_create implementation for shmem drivers
 * input:   file — DRM file, dev — device, args — ioctl arguments
 * output:  0 or negative errno
 * effects: fills in args->pitch/size/handle
 */
int
drm_gem_shmem_dumb_create(struct drm_file *file, struct drm_device *dev,
    struct drm_mode_create_dumb *args)
{
	u32 min_pitch = DIV_ROUND_UP(args->width * args->bpp, 8);

	if (!args->pitch || !args->size) {
		args->pitch = min_pitch;
		args->size = drm_gem_shmem_align_size(args->pitch *
		    args->height);
	} else {
		/* Ensure sane minimum values. */
		if (args->pitch < min_pitch)
			args->pitch = min_pitch;
		if (args->size < (uint64_t)args->pitch * args->height)
			args->size = drm_gem_shmem_align_size(args->pitch *
			    args->height);
	}

	return (drm_gem_shmem_create_with_handle(file, dev, args->size,
	    &args->handle));
}

/* ── madvise / purge ──────────────────────────────────────────────────────── */

/*
 * purpose: update the object's madvise state
 * input:   shmem — the object (resv held), madv — new state
 * output:  1 while the object is still usable, 0 once purged
 * effects: sets shmem->madv unless the object was already purged
 */
int
drm_gem_shmem_madvise(struct drm_gem_shmem_object *shmem, int madv)
{
	dma_resv_assert_held(shmem->base.resv);

	return (drm_gem_shmem_madvise_apply(&shmem->madv, madv));
}

/*
 * purpose: strip every existing userspace PTE mapping of one of this
 *          object's backing pages, wherever each page currently lives
 * input:   shmem — the object; caller holds shmem->base.resv, shmem->pages
 *          populated (i.e. called before drm_gem_shmem_put_pages())
 * output:  none
 * effects: for each entry of shmem->pages[], removes it from every pmap that
 *          currently maps it. Does not change which vm_object owns the page,
 *          does not unwire or free it, and does not touch any vm_object's
 *          reference count -- see the comment on drm_gem_shmem_purge() and
 *          drm/PURGE-NOTES.md for why.
 */
static void
drm_gem_shmem_zap_ptes(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	unsigned long i, npages;

	dma_resv_assert_held(shmem->base.resv);

	npages = drm_gem_shmem_npages(obj->size);

	for (i = 0; i < npages; i++) {
		struct page *pg = shmem->pages[i];
		vm_object_t pobj;

		if (pg == NULL)
			continue;

again:
		/*
		 * pg->object names whichever vm_object currently owns this
		 * page: our own OBJT_SWAP backing object until the page is
		 * first faulted in by drm_gem_shmem_fault(), and the shared
		 * OBJT_MGTDEVICE object linux_file_mmap_single() created for
		 * this GEM object's mmaps from then on --
		 * lkpi_vmf_insert_pfn_prot_locked() moves a page there with
		 * vm_page_remove()+vm_page_iter_insert(); it does not copy
		 * it. That move only happens from inside
		 * drm_gem_shmem_fault(), which -- like this function's only
		 * caller -- takes shmem->base.resv first, so pg->object
		 * cannot change while we hold it; the recheck below is
		 * defensive, matching this file's other page-identity
		 * rechecks, not load-bearing.
		 */
		pobj = pg->object;
		if (pobj == NULL)
			continue;

		VM_OBJECT_WLOCK(pobj);
		if (pg->object != pobj) {
			VM_OBJECT_WUNLOCK(pobj);
			goto again;
		}
		if (!vm_page_busy_acquire(pg, VM_ALLOC_WAITFAIL)) {
			/*
			 * VM_ALLOC_WAITFAIL guarantees pobj is still locked
			 * on return; pg's identity may not be, so start over.
			 */
			VM_OBJECT_WUNLOCK(pobj);
			goto again;
		}

		pmap_remove_all(pg);

		vm_page_xunbusy(pg);
		VM_OBJECT_WUNLOCK(pobj);
	}
}

/*
 * purpose: return an idle, driver-marked object's memory to the system
 * input:   shmem — the object, purgeable per drm_gem_shmem_is_purgeable()
 * output:  none
 * effects: strips any existing userspace PTE mapping of every backing page
 *          (drm_gem_shmem_zap_ptes()), unmaps and frees the sg table, unwires
 *          the pages, drops the mmap offset, and truncates the backing swap
 *          object
 *
 * Linux additionally calls drm_vma_node_unmap() against the device's
 * anon_inode mapping so that any live userspace mapping of the BO starts
 * refaulting. drm-kmod's FreeBSD side has no anon_inode to pass it:
 * struct drm_device::anon_inode is guarded "#ifdef __linux__" in drm-kmod's
 * own drm_device.h, and drm_dev_unplug() (drm_drv.c) leaves the equivalent
 * unmap_mapping_range() call as an explicit, unimplemented FreeBSD TODO
 * comment rather than attempting it. So this function does not call it
 * either.
 *
 * drm_gem_shmem_zap_ptes() (above) reaches the same end result at the pmap
 * layer directly instead: pmap_remove_all() on each of shmem->pages[],
 * wherever each currently lives. That is a deliberate choice, not the first
 * thing tried. linuxkpi *does* have an FreeBSD overload of
 * drm_vma_node_unmap()/unmap_mapping_range() that looks like the obvious port
 * of the upstream call above (linux/mm.h, linux_page.c:638-649,
 * lkpi_unmap_mapping_range()) -- but its own implementation ends in
 * cdev_pager_lookup() + cdev_mgtdev_pager_free_pages() +
 * vm_object_deallocate(), and that last call can drop the OBJT_MGTDEVICE
 * object's *last* reference if it races a concurrent munmap(2) on the last
 * live mapping. Dropping the last reference reenters this exact object's own
 * drm_gem_shmem_vm_close() (via linux_cdev_pager_dtor()), which calls
 * dma_resv_lock(shmem->base.resv, NULL) -- and this function's caller already
 * holds that same non-recursive lock on this thread. That is a real
 * self-deadlock, not a hypothetical one; it is why this function does not use
 * that path. See drm/PURGE-NOTES.md for the full derivation.
 *
 * What is still not covered: the OBJT_MGTDEVICE object (if this GEM object
 * has ever been mmap()ed) keeps considering its now-unmapped pages resident
 * until its own last real reference goes away -- this function deliberately
 * never touches that object's refcount, for the reason above. That is inert
 * bookkeeping, not a live hole: shmem->madv is set to -1 and the mmap offset
 * is freed below, so drm_gem_shmem_fault() refuses new faults (SIGBUS)
 * without ever looking at a page again, and no new mapping can be created
 * either.
 *
 * As of this writing nothing in this tree calls drm_gem_shmem_purge() at all
 * (Lima wires up neither a madvise ioctl nor a shrinker, matching upstream
 * Lima -- only panfrost's shrinker calls this function upstream), so this
 * path is latent rather than active. NOT verified on hardware.
 */
void
drm_gem_shmem_purge(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	struct drm_device *dev = obj->dev;

	dma_resv_assert_held(shmem->base.resv);

	drm_WARN_ON(dev, !drm_gem_shmem_is_purgeable(shmem));

	drm_gem_shmem_zap_ptes(shmem);

	dma_unmap_sgtable(dev->dev, shmem->sgt, DMA_BIDIRECTIONAL, 0);
	sg_free_table(shmem->sgt);
	kfree(shmem->sgt);
	shmem->sgt = NULL;

	drm_gem_shmem_put_pages(shmem);

	shmem->madv = -1;

	drm_gem_free_mmap_offset(obj);

	/*
	 * Tell the swap object to drop everything it is holding, now: we are
	 * typically called from a shrinker.
	 */
	shmem_truncate_range(obj->filp->f_shmem, 0, (loff_t)-1);
	invalidate_mapping_pages(obj->filp->f_shmem, 0, (loff_t)-1);
}

/* ── userspace mapping ────────────────────────────────────────────────────── */

/*
 * purpose: &vm_operations_struct.fault handler — resolve one faulting page
 * input:   vmf — the fault, with vmf->vma->vm_private_data pointing at the GEM
 *          object (set by drm_gem_mmap_obj())
 * output:  VM_FAULT_NOPAGE on success, VM_FAULT_SIGBUS/OOM on failure
 * effects: inserts the page into the mapping's vm_object
 *
 * FreeBSD contract (sys/compat/linuxkpi/common/src/linux_compat.c,
 * linux_cdev_pager_populate): the handler is called with vma->vm_obj set and
 * that object *unlocked*, must busy the pages it resolves and report them
 * through vma->vm_pfn_first / vma->vm_pfn_count, then return VM_FAULT_NOPAGE.
 * lkpi_vmf_insert_pfn_prot_locked() does all of that, provided we hold the
 * vm_object write lock across it — which is why this looks different from the
 * Linux original's single vmf_insert_pfn() call.
 */
static int
drm_gem_shmem_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);
	struct page *page;
	unsigned long idx;
	int ret;

	dma_resv_lock(shmem->base.resv, NULL);

	/*
	 * vmf->pgoff carries the fake DRM mmap offset, not a page index into
	 * the object, so the index has to come from the address.
	 */
	if (!drm_gem_shmem_fault_index(vmf->address, vma->vm_start,
	    drm_gem_shmem_npages(obj->size), &idx) ||
	    drm_WARN_ON_ONCE(obj->dev, shmem->pages == NULL) ||
	    shmem->madv < 0) {
		dma_resv_unlock(shmem->base.resv);
		return (VM_FAULT_SIGBUS);
	}

	page = shmem->pages[idx];

	VM_OBJECT_WLOCK(vma->vm_obj);
	ret = lkpi_vmf_insert_pfn_prot_locked(vma, vmf->address,
	    page_to_pfn(page), vma->vm_page_prot);
	VM_OBJECT_WUNLOCK(vma->vm_obj);

	dma_resv_unlock(shmem->base.resv);

	return (ret);
}

/*
 * purpose: &vm_operations_struct.open handler — a new mapping of an existing
 *          vma (fork, mremap, partial unmap)
 * input:   vma — the new mapping
 * output:  none
 * effects: takes one more pages reference and one more GEM reference
 */
static void
drm_gem_shmem_vm_open(struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	drm_WARN_ON(obj->dev, obj->import_attach);

	dma_resv_lock(shmem->base.resv, NULL);

	/*
	 * The pages were pinned when the buffer was first mmap'd; this only
	 * accounts for the extra mapping.
	 */
	if (!drm_WARN_ON_ONCE(obj->dev, !shmem->pages_use_count))
		shmem->pages_use_count++;

	dma_resv_unlock(shmem->base.resv);

	drm_gem_vm_open(vma);
}

/*
 * purpose: &vm_operations_struct.close handler
 * input:   vma — the mapping going away
 * output:  none
 * effects: drops the pages reference and the GEM reference
 */
static void
drm_gem_shmem_vm_close(struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	dma_resv_lock(shmem->base.resv, NULL);
	drm_gem_shmem_put_pages(shmem);
	dma_resv_unlock(shmem->base.resv);

	drm_gem_vm_close(vma);
}

const struct vm_operations_struct drm_gem_shmem_vm_ops = {
	.fault = drm_gem_shmem_fault,
	.open = drm_gem_shmem_vm_open,
	.close = drm_gem_shmem_vm_close,
};

/*
 * purpose: &drm_gem_object_funcs.mmap implementation
 * input:   shmem — the object, vma — mapping being established
 * output:  0 or negative errno
 * effects: pins the pages for the lifetime of the mapping and selects the
 *          cache mode; the pages themselves are inserted lazily by
 *          drm_gem_shmem_fault()
 */
int
drm_gem_shmem_mmap(struct drm_gem_shmem_object *shmem,
    struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret;

	if (obj->import_attach) {
		/*
		 * Reset both vm_ops and vm_private_data so we don't end up
		 * with vm_ops pointing to our own implementation if the
		 * dma-buf backend doesn't set those fields (matches upstream
		 * Linux drm_gem_shmem_mmap()).
		 */
		vma->vm_private_data = NULL;
		vma->vm_ops = NULL;

		/*
		 * bzdOS 2026-08-11: drm-kmod now implements dma_buf_mmap()
		 * (patches/drm-kmod-dma-buf-mmap.patch) -- this branch used
		 * to be an unconditional -EOPNOTSUPP because that symbol did
		 * not exist anywhere, not because mapping an imported buffer
		 * is inherently unsafe. The call below reaches
		 * drm_gem_dmabuf_mmap() -> drm_gem_prime_mmap()
		 * (drivers/gpu/drm/drm_prime.c, already shipped in drm-kmod
		 * and already wired into drm_gem_prime_dmabuf_ops.mmap),
		 * which forwards into the *exporting* object's own
		 * obj->funcs->mmap -- for a self-exported shmem object, that
		 * is this same function's non-import branch below, i.e. the
		 * real, already-exercised page-mapping path, not new logic.
		 *
		 * obj->dma_buf is populated by drm_gem_prime_fd_to_handle()
		 * (drm_prime.c) before a freshly-imported handle can ever
		 * reach an mmap() call, so it is non-NULL here for the
		 * ordinary PRIME_FD_TO_HANDLE-then-mmap sequence.
		 *
		 * NOT verified on hardware. drm-kmod's dma_buf_mmap() also
		 * knowingly omits one piece of upstream's reference
		 * bookkeeping (vma_set_file() does not typecheck in this
		 * port) -- see that function's comment in drm-kmod's
		 * dma-buf.c and patches/README.md for exactly what that does
		 * and does not affect.
		 */
		ret = dma_buf_mmap(obj->dma_buf, vma, 0);

		/*
		 * Drop the reference drm_gem_mmap_obj() took on `obj` before
		 * calling us. On success, drm_gem_prime_mmap() just
		 * repointed vma->vm_private_data/vm_ops at the *exporter's*
		 * object instead of `obj`, so the vm_ops->close() that
		 * eventually undoes drm_gem_mmap_obj()'s reference will
		 * decrement the exporter's refcount, never this (importing)
		 * obj's. Without this put, obj would leak one reference per
		 * successful mmap. Matches upstream Linux
		 * drm_gem_shmem_mmap().
		 */
		if (ret == 0)
			drm_gem_object_put(obj);

		return (ret);
	}

	dma_resv_lock(shmem->base.resv, NULL);
	ret = drm_gem_shmem_get_pages(shmem);
	dma_resv_unlock(shmem->base.resv);

	if (ret)
		return (ret);

	vm_flags_set(vma, VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	if (shmem->map_wc)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	return (0);
}

/* ── debug / sg tables ────────────────────────────────────────────────────── */

/*
 * purpose: print the object's shmem state for debugfs
 * input:   shmem — the object, p — printer, indent — tab depth
 * output:  none
 * effects: writes to @p
 */
void
drm_gem_shmem_print_info(const struct drm_gem_shmem_object *shmem,
    struct drm_printer *p, unsigned int indent)
{
	if (shmem->base.import_attach)
		return;

	drm_printf_indent(p, indent, "pages_use_count=%u\n",
	    shmem->pages_use_count);
	drm_printf_indent(p, indent, "vmap_use_count=%u\n",
	    shmem->vmap_use_count);
	drm_printf_indent(p, indent, "vaddr=%p\n", shmem->vaddr);
}

/*
 * purpose: build a fresh sg_table describing the already-pinned pages
 * input:   shmem — the object, pages pinned, not imported
 * output:  sg_table (not DMA-mapped) or ERR_PTR
 * effects: allocates the table; drivers should call
 *          drm_gem_shmem_get_pages_sgt() instead of this
 */
struct sg_table *
drm_gem_shmem_get_sg_table(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;

	drm_WARN_ON(obj->dev, obj->import_attach);

	return (drm_prime_pages_to_sg(obj->dev, shmem->pages,
	    drm_gem_shmem_npages(obj->size)));
}

/*
 * purpose: pin the pages, DMA-map them and cache the resulting sg_table
 * input:   shmem — the object; caller holds shmem->base.resv
 * output:  the cached sg_table, or ERR_PTR
 * effects: on first call, populates shmem->sgt
 */
static struct sg_table *
drm_gem_shmem_get_pages_sgt_locked(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	struct sg_table *sgt;
	int ret;

	if (shmem->sgt)
		return (shmem->sgt);

	drm_WARN_ON(obj->dev, obj->import_attach);

	ret = drm_gem_shmem_get_pages(shmem);
	if (ret)
		return (ERR_PTR(ret));

	sgt = drm_gem_shmem_get_sg_table(shmem);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_put_pages;
	}

	/* Map the pages for use by the hardware. */
	ret = dma_map_sgtable(obj->dev->dev, sgt, DMA_BIDIRECTIONAL, 0);
	if (ret)
		goto err_free_sgt;

	shmem->sgt = sgt;

	return (sgt);

err_free_sgt:
	sg_free_table(sgt);
	kfree(sgt);
err_put_pages:
	drm_gem_shmem_put_pages(shmem);

	return (ERR_PTR(ret));
}

/*
 * purpose: main entry point for drivers needing the object's backing storage
 * input:   shmem — the object
 * output:  DMA-mapped sg_table, or ERR_PTR
 * effects: pins and maps on first use; subsequent calls return the cached table
 */
struct sg_table *
drm_gem_shmem_get_pages_sgt(struct drm_gem_shmem_object *shmem)
{
	struct sg_table *sgt;
	int ret;

	ret = dma_resv_lock_interruptible(shmem->base.resv, NULL);
	if (ret)
		return (ERR_PTR(ret));
	sgt = drm_gem_shmem_get_pages_sgt_locked(shmem);
	dma_resv_unlock(shmem->base.resv);

	return (sgt);
}

/*
 * purpose: &drm_driver.gem_prime_import_sg_table implementation
 * input:   dev — importing device, attach — dma-buf attachment, sgt — the
 *          exporter's scatter/gather table
 * output:  the new GEM object, or ERR_PTR
 * effects: creates a private (externally backed) shmem object owning @sgt
 */
struct drm_gem_object *
drm_gem_shmem_prime_import_sg_table(struct drm_device *dev,
    struct dma_buf_attachment *attach, struct sg_table *sgt)
{
	struct drm_gem_shmem_object *shmem;
	size_t size;

	size = drm_gem_shmem_align_size(attach->dmabuf->size);

	shmem = __drm_gem_shmem_create(dev, size, true);
	if (IS_ERR(shmem))
		return (ERR_CAST(shmem));

	shmem->sgt = sgt;

	drm_dbg_prime(dev, "size = %zu\n", size);

	return (&shmem->base);
}
