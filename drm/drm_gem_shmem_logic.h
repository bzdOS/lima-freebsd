/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * drm/drm_gem_shmem_logic.h — pure, kernel-free logic of the GEM SHMEM helper.
 *
 * MODULE: hal/lima/drm/drm_gem_shmem_logic.h
 * PURPOSE: The arithmetic and reference-count state machine used by
 *          drm_gem_shmem_helper.c, factored out so it can be unit-tested on
 *          the build host without a kernel (see tests/test_shmem_logic.c).
 *
 * Nothing in here touches the VM, a lock, or a struct with kernel types: every
 * function is a total function of its scalar arguments. The kernel file is
 * responsible for holding the right lock while calling them; these helpers
 * only encode *what* the counters must do, not *when*.
 *
 * PAGE_SHIFT/PAGE_SIZE are expected from the including environment (the kernel
 * provides them; the host test defines them to the same values).
 */

#ifndef __DRM_GEM_SHMEM_LOGIC_H__
#define __DRM_GEM_SHMEM_LOGIC_H__

#ifndef PAGE_SHIFT
#error "include a header defining PAGE_SHIFT before drm_gem_shmem_logic.h"
#endif

/*
 * Outcome of dropping a reference on a counted resource (the pages array or
 * the kernel vmap). Kept as an enum rather than a bare int so a caller cannot
 * silently confuse "still referenced" with "release now".
 */
enum drm_gem_shmem_unref {
	DRM_GEM_SHMEM_UNREF_UNDERFLOW = -1,	/* count was already 0: bug */
	DRM_GEM_SHMEM_UNREF_RELEASE = 0,	/* last reference: free it */
	DRM_GEM_SHMEM_UNREF_KEEP = 1,		/* references remain */
};

/*
 * purpose: number of pages backing an object of the given byte size
 * input:   size — object size in bytes; must already be page-aligned
 * output:  page count
 * effects: none
 */
static inline unsigned long
drm_gem_shmem_npages(unsigned long size)
{
	return (size >> PAGE_SHIFT);
}

/*
 * purpose: round an allocation request up to a whole number of pages
 * input:   size — requested size in bytes
 * output:  page-aligned size; 0 stays 0
 * effects: none
 */
static inline unsigned long
drm_gem_shmem_align_size(unsigned long size)
{
	return ((size + PAGE_SIZE - 1) & ~((unsigned long)PAGE_SIZE - 1));
}

/*
 * purpose: take a reference on a counted resource
 * input:   use_count — pointer to the counter
 * output:  1 if this is the first reference (caller must acquire the
 *          resource), 0 if it was already held
 * effects: increments *use_count
 */
static inline int
drm_gem_shmem_ref_first(unsigned int *use_count)
{
	return ((*use_count)++ == 0);
}

/*
 * purpose: drop a reference on a counted resource
 * input:   use_count — pointer to the counter
 * output:  see enum drm_gem_shmem_unref
 * effects: decrements *use_count unless it was already 0 (in which case it is
 *          left at 0 — an underflow is a caller bug and must not wrap around)
 */
static inline enum drm_gem_shmem_unref
drm_gem_shmem_unref(unsigned int *use_count)
{
	if (*use_count == 0)
		return (DRM_GEM_SHMEM_UNREF_UNDERFLOW);
	if (--(*use_count) > 0)
		return (DRM_GEM_SHMEM_UNREF_KEEP);
	return (DRM_GEM_SHMEM_UNREF_RELEASE);
}

/*
 * purpose: translate a fault address into an index into the pages array
 * input:   address  — faulting address as handed to the fault handler,
 *          vm_start — start of the mapping,
 *          npages   — number of pages in the object,
 *          out      — receives the page index when the fault is in range
 * output:  1 if in range (*out valid), 0 if the fault must become SIGBUS
 * effects: none
 *
 * NOTE: the DRM fake mmap offset lives in vm_pgoff and must NOT be used here;
 * the mapping always starts at page 0 of the object.
 */
static inline int
drm_gem_shmem_fault_index(unsigned long address, unsigned long vm_start,
    unsigned long npages, unsigned long *out)
{
	unsigned long idx;

	if (address < vm_start)
		return (0);
	idx = (address - vm_start) >> PAGE_SHIFT;
	if (idx >= npages)
		return (0);
	*out = idx;
	return (1);
}

/*
 * purpose: decide whether an object may be reclaimed under memory pressure
 * input:   madv            — madvise state (<0 purged, 0 in use, >0 driver's),
 *          vmap_use_count  — outstanding kernel mappings,
 *          have_sgt        — object has a mapped sg table,
 *          exported        — object is exported as a dma-buf,
 *          imported        — object was imported from a dma-buf
 * output:  1 if purgeable, 0 otherwise
 * effects: none
 */
static inline int
drm_gem_shmem_purgeable(int madv, unsigned int vmap_use_count, int have_sgt,
    int exported, int imported)
{
	return (madv > 0 && vmap_use_count == 0 && have_sgt != 0 &&
	    exported == 0 && imported == 0);
}

/*
 * purpose: apply an madvise request
 * input:   madv     — pointer to the object's madvise state,
 *          new_madv — requested state
 * output:  1 if the object is still usable, 0 if it has been purged
 * effects: sets *madv to new_madv unless the object was already purged
 */
static inline int
drm_gem_shmem_madvise_apply(int *madv, int new_madv)
{
	if (*madv >= 0)
		*madv = new_madv;
	return (*madv >= 0);
}

#endif /* __DRM_GEM_SHMEM_LOGIC_H__ */
