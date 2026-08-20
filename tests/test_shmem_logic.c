/*
 * test_shmem_logic.c — userspace tests for the GEM SHMEM helper's logic
 *
 * Unlike test_lima_math.c, which mirrors constants, this file #includes the
 * *shipped* header drm/drm_gem_shmem_logic.h, so a change to the reference
 * counting or fault-index arithmetic that breaks an invariant fails here.
 *
 * What can and cannot be covered: the page-array/vmap reference-count state
 * machine, the size arithmetic, the fault address→page-index translation, the
 * purgeable predicate and the madvise transition are pure functions and are
 * covered. Everything that talks to a vm_object, a dma_resv or a pmap is
 * kernel-only and is NOT covered by any host test — see the WARNING at the end
 * of this file.
 *
 * Build: cc -Wall -Wextra -o test_shmem_logic test_shmem_logic.c && ./test_shmem_logic
 * Or via: gmake -C hal/lima test-shmem
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

/* The kernel supplies these; the helper only needs the two. */
#define PAGE_SHIFT 12
#define PAGE_SIZE  (1 << PAGE_SHIFT)

#include "../drm/drm_gem_shmem_logic.h"

/* ── Test harness (same shape as test_lima_math.c) ───────────────────────── */

static int pass_count = 0;
static int fail_count = 0;

#define CHECK_EQ(got, want, msg) do { \
    unsigned long long _g = (unsigned long long)(got); \
    unsigned long long _w = (unsigned long long)(want); \
    if (_g == _w) { \
        printf("ok   %s\n", (msg)); \
        pass_count++; \
    } else { \
        printf("FAIL %s  (got %llu, want %llu)\n", (msg), _g, _w); \
        fail_count++; \
    } \
} while (0)

#define CHECK(cond, msg) CHECK_EQ(!!(cond), 1u, (msg))

int main(void)
{
    printf("=== DRM GEM SHMEM helper logic tests ===\n\n");

    /* ── Size arithmetic ────────────────────────────────────────────────── */
    printf("-- Size arithmetic --\n");
    CHECK_EQ(drm_gem_shmem_align_size(0),          0u,     "align(0) == 0");
    CHECK_EQ(drm_gem_shmem_align_size(1),          4096u,  "align(1) == 4096");
    CHECK_EQ(drm_gem_shmem_align_size(4095),       4096u,  "align(4095) == 4096");
    CHECK_EQ(drm_gem_shmem_align_size(4096),       4096u,  "align(4096) is idempotent");
    CHECK_EQ(drm_gem_shmem_align_size(4097),       8192u,  "align(4097) == 8192");
    CHECK_EQ(drm_gem_shmem_align_size(1 << 20),    1u << 20, "align(1 MiB) is idempotent");
    CHECK_EQ(drm_gem_shmem_npages(4096),           1u,     "npages(4096) == 1");
    CHECK_EQ(drm_gem_shmem_npages(1 << 20),        256u,   "npages(1 MiB) == 256");
    CHECK_EQ(drm_gem_shmem_npages(drm_gem_shmem_align_size(4097)), 2u,
             "npages(align(4097)) == 2");

    /* ── Reference counting: pages / vmap ───────────────────────────────── */
    printf("\n-- Reference counting --\n");
    {
        unsigned int use = 0;

        CHECK_EQ(drm_gem_shmem_ref_first(&use), 1u, "first ref reports first");
        CHECK_EQ(use, 1u,                            "first ref leaves count 1");
        CHECK_EQ(drm_gem_shmem_ref_first(&use), 0u, "second ref reports not-first");
        CHECK_EQ(use, 2u,                            "second ref leaves count 2");
        CHECK_EQ(drm_gem_shmem_unref(&use), DRM_GEM_SHMEM_UNREF_KEEP,
                 "unref with refs left says KEEP");
        CHECK_EQ(use, 1u,                            "KEEP leaves count 1");
        CHECK_EQ(drm_gem_shmem_unref(&use), DRM_GEM_SHMEM_UNREF_RELEASE,
                 "last unref says RELEASE");
        CHECK_EQ(use, 0u,                            "RELEASE leaves count 0");
    }
    {
        /*
         * An underflow is a caller bug (upstream fires a WARN_ON_ONCE). What
         * must never happen is a wrap to UINT_MAX, which would keep the pages
         * pinned forever.
         */
        unsigned int use = 0;

        CHECK_EQ(drm_gem_shmem_unref(&use), DRM_GEM_SHMEM_UNREF_UNDERFLOW,
                 "unref at zero reports UNDERFLOW");
        CHECK_EQ(use, 0u, "underflow does not wrap the counter");
    }
    {
        /* A deep pin nest must release exactly once, at the bottom. */
        unsigned int use = 0;
        int i, first_count = 0, release_count = 0;

        for (i = 0; i < 8; i++)
            first_count += drm_gem_shmem_ref_first(&use);
        for (i = 0; i < 8; i++)
            release_count +=
                (drm_gem_shmem_unref(&use) == DRM_GEM_SHMEM_UNREF_RELEASE);
        CHECK_EQ(first_count,   1u, "8 nested refs report first exactly once");
        CHECK_EQ(release_count, 1u, "8 nested unrefs release exactly once");
        CHECK_EQ(use,           0u, "balanced nest ends at zero");
    }

    /* ── Fault address → page index ─────────────────────────────────────── */
    printf("\n-- Fault index translation --\n");
    {
        unsigned long idx = ~0UL;
        const unsigned long npages = 4;          /* a 16 KiB object */
        const unsigned long start = 0;           /* linuxkpi vma->vm_start */

        CHECK_EQ(drm_gem_shmem_fault_index(0, start, npages, &idx), 1u,
                 "fault at offset 0 is in range");
        CHECK_EQ(idx, 0u, "offset 0 maps to page 0");
        CHECK_EQ(drm_gem_shmem_fault_index(4095, start, npages, &idx), 1u,
                 "fault at 4095 is in range");
        CHECK_EQ(idx, 0u, "offset 4095 still maps to page 0");
        CHECK_EQ(drm_gem_shmem_fault_index(4096, start, npages, &idx), 1u,
                 "fault at 4096 is in range");
        CHECK_EQ(idx, 1u, "offset 4096 maps to page 1");
        CHECK_EQ(drm_gem_shmem_fault_index(3 * 4096, start, npages, &idx), 1u,
                 "last page is in range");
        CHECK_EQ(idx, 3u, "offset 3*4096 maps to page 3");
        CHECK_EQ(drm_gem_shmem_fault_index(4 * 4096, start, npages, &idx), 0u,
                 "one page past the end is rejected (SIGBUS)");
        CHECK_EQ(idx, 3u, "rejected fault does not touch the index");
    }
    {
        /* Non-zero vm_start: the index is relative to the mapping, not to 0. */
        unsigned long idx = ~0UL;
        const unsigned long start = 0x40000000UL;

        CHECK_EQ(drm_gem_shmem_fault_index(start + 8192, start, 4, &idx), 1u,
                 "offset within a non-zero-based vma is in range");
        CHECK_EQ(idx, 2u, "vm_start is subtracted before shifting");
        CHECK_EQ(drm_gem_shmem_fault_index(start - 4096, start, 4, &idx), 0u,
                 "address below vm_start is rejected, not wrapped");
    }

    /* ── madvise / purgeable ────────────────────────────────────────────── */
    printf("\n-- madvise and purge eligibility --\n");
    {
        int madv = 0;

        CHECK_EQ(drm_gem_shmem_madvise_apply(&madv, 1), 1u,
                 "madvise on a live object succeeds");
        CHECK_EQ(madv, 1u, "madvise stores the new state");
        CHECK_EQ(drm_gem_shmem_madvise_apply(&madv, -1), 0u,
                 "madvise(-1) marks the object purged");
        CHECK_EQ(drm_gem_shmem_madvise_apply(&madv, 1), 0u,
                 "a purged object cannot be revived");
        CHECK_EQ(madv, (unsigned long long)-1,
                 "a purged object stays purged");
    }
    /* madv, vmap_use_count, have_sgt, exported, imported */
    CHECK_EQ(drm_gem_shmem_purgeable(1, 0, 1, 0, 0), 1u,
             "idle, marked, mapped-for-GPU object is purgeable");
    CHECK_EQ(drm_gem_shmem_purgeable(0, 0, 1, 0, 0), 0u,
             "unmarked object is not purgeable");
    CHECK_EQ(drm_gem_shmem_purgeable(-1, 0, 1, 0, 0), 0u,
             "already-purged object is not purgeable again");
    CHECK_EQ(drm_gem_shmem_purgeable(1, 1, 1, 0, 0), 0u,
             "object with a live kernel mapping is not purgeable");
    CHECK_EQ(drm_gem_shmem_purgeable(1, 0, 0, 0, 0), 0u,
             "object with no sg table is not purgeable");
    CHECK_EQ(drm_gem_shmem_purgeable(1, 0, 1, 1, 0), 0u,
             "exported object is not purgeable");
    CHECK_EQ(drm_gem_shmem_purgeable(1, 0, 1, 0, 1), 0u,
             "imported object is not purgeable");

    /* ── Summary ────────────────────────────────────────────────────────── */
    printf("\n=== %s: %d passed, %d failed ===\n",
           fail_count ? "FAIL" : "PASS", pass_count, fail_count);

    /*
     * WARNING, so nobody mistakes a green run for a working driver: these
     * tests exercise arithmetic only. The parts of drm_gem_shmem_helper.c that
     * can actually corrupt memory — shmem_read_mapping_page() page wiring,
     * put_page() release, pmap_page_set_memattr() cache-mode changes,
     * lkpi_vmf_insert_pfn_prot_locked() fault insertion and the dma_resv
     * locking discipline — cannot be tested without a kernel and have never
     * been executed on hardware.
     */
    return fail_count ? 1 : 0;
}
