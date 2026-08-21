/*
 * lima_ioctl_smoke.c -- Tier 0 ioctl-plumbing smoke test for the lima DRM
 * driver (Mali-400 MP2, Banana Pi M64 / FreeBSD 15.1 guest).
 *
 * Implements the exact sequence from docs/PLAN-mesa-lima.md sec1.2 ("Tier 0 --
 * prove the ioctl plumbing, touch zero GPU hardware state"):
 *
 *   open("/dev/dri/renderD128", O_RDWR)
 *   GET_PARAM(GPU_ID)     -> expect DRM_LIMA_PARAM_GPU_ID_MALI400 (1)
 *   GET_PARAM(NUM_PP)     -> expect 2                    (pp0 + pp1)
 *   GET_PARAM(GP_VERSION) -> expect mali400, major 1, minor 1
 *   GET_PARAM(PP_VERSION) -> expect mali400, major 1, minor 1
 *   CTX_CREATE                                    -> ctx_id
 *   GEM_CREATE(size=4096, flags=0)  (NOT LIMA_BO_FLAG_HEAP)  -> handle
 *   GEM_INFO(handle)                              -> {va, offset}
 *   mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, offset) -> ptr
 *   write a known pattern through ptr, read it back, compare
 *   munmap(ptr, 4096)
 *   CTX_FREE(ctx_id)
 *   close(fd)
 *
 * Plus one extra, clearly-labelled NEGATIVE test inserted right after the
 * main GEM_CREATE: GEM_CREATE with LIMA_BO_FLAG_HEAP must fail -ENOSYS
 * (lima_heap_alloc() is a stub, hal/lima/lima_gem.c:28-37).  Never send that
 * flag in the main sequence above.
 *
 * This never programs a single GP/PP/MMU register (docs/PLAN-mesa-lima.md
 * sec1.2): a failure here means the bug is in ioctl plumbing, GEM, VM
 * (per-file page tables / ctx-mgr), or drm_gem_shmem_helper.c's mmap /
 * page-fault path -- not in a Mali command stream.  See tests/README.md for
 * exactly what each failing step implicates.
 *
 * NO libdrm, NO drm-kmod headers (neither ships on FreeBSD base, and
 * drm-kmod's <uapi/drm/drm.h> is fetched by pkg, not vendored in this repo
 * -- see hal/lima/Makefile's own comments).  Every struct/ioctl-number
 * below is hand-derived from the real UAPI header, hal/lima/drm/lima_drm.h
 * (this repo), with a comment at each definition naming the exact line(s)
 * it mirrors.  _Static_assert on every struct's size (and on the two
 * 64-bit-field offsets that padding could silently shift) turns a drift
 * between this file and lima_drm.h into a build failure instead of a
 * silent wrong-size ioctl.
 *
 * Deliberately NOT mirrored here: GEM_SUBMIT, GEM_WAIT, and the GP/PP frame
 * structs (lima_drm.h:51-101).  Tier 0 never calls them -- see
 * docs/PLAN-mesa-lima.md sec1.3 (Tier 1) for the job that will need them.
 *
 * Build (on the FreeBSD guest, using its own native cc -- see
 * tests/README.md for why this must not be cross-compiled):
 *   cc -Wall -Wextra -O0 -o lima_ioctl_smoke lima_ioctl_smoke.c
 * Run:
 *   ./lima_ioctl_smoke
 * (optional: ./lima_ioctl_smoke /dev/dri/renderD129  -- override the device
 * path; defaults to /dev/dri/renderD128.)
 *
 * Exit status: 0 iff every step passed; nonzero otherwise.  Always prints a
 * one-line "=== SUMMARY: ... ===" as the last line of output.  Safe to run
 * more than once in a row -- see tests/README.md, "repeatability".
 */

#include <sys/types.h>
#include <sys/mman.h>

#if defined(__FreeBSD__)
#include <sys/ioctl.h>   /* ioctl() prototype; also pulls in sys/ioccom.h */
#include <sys/ioccom.h>  /* _IOWR/_IOW/_IOR -- explicit per the task, harmless
                           * to repeat: FreeBSD's own sys/ioctl.h already
                           * #includes this, guarded by its header guard. */
#else
/*
 * Deliberately NOT #include <sys/ioctl.h> on a non-FreeBSD host: on Linux
 * it drags in <asm-generic/ioctl.h>, which defines _IOC/_IOR/_IOW/_IOWR/
 * IOC_IN/IOC_OUT/IOC_INOUT under these EXACT SAME names using LINUX's bit
 * layout (confirmed by trying it: gcc reports "IOC_OUT redefined ...
 * previous definition /usr/include/asm-generic/ioctl.h:100" et al, which
 * is the mismatch described in the long comment above DRM_IOCTL_BASE below
 * made concrete). #including both would be a straight redefinition error,
 * and #including only Linux's would silently give this file Linux's
 * ioctl-command encoding instead of the BSD one this program actually
 * needs -- worse than a build error. So: no <sys/ioctl.h> here, a bare
 * hand-written prototype for the one libc function this file calls
 * (ioctl() itself is not part of ISO C, so it is never declared by the
 * other, non-ioctl-related headers below), and a byte-for-bit
 * reproduction of FreeBSD 15's sys/sys/ioccom.h _IOC family (verified
 * against the vendored copy at
 * /opt/bzdos/build/freebsd-src/sys/sys/ioccom.h) under its real names.
 *
 * This whole branch exists ONLY so `gcc -fsyntax-only`/`-c` can typecheck
 * this file on a Linux host (see tests/README.md).  On the real FreeBSD
 * target it is skipped entirely and the genuine system headers are used.
 */
extern int ioctl(int fd, unsigned long request, ...);

#define IOCPARM_MASK 0x1fffUL
#define IOC_VOID     0x20000000UL
#define IOC_OUT      0x40000000UL
#define IOC_IN       0x80000000UL
#define IOC_INOUT    (IOC_IN | IOC_OUT)
#define _IOC(inout, group, num, len) \
    ((unsigned long)((inout) | (((long)(len) & IOCPARM_MASK) << 16) | \
        ((group) << 8) | (num)))
#define _IOR(g, n, t)  _IOC(IOC_OUT,   (g), (n), sizeof(t))
#define _IOW(g, n, t)  _IOC(IOC_IN,    (g), (n), sizeof(t))
#define _IOWR(g, n, t) _IOC(IOC_INOUT, (g), (n), sizeof(t))
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* =========================================================================
 * UAPI mirror of hal/lima/drm/lima_drm.h.
 *
 * If this ever disagrees with lima_drm.h, lima_drm.h is the real kernel
 * UAPI and is right by definition -- fix THIS file, not that one.
 * ========================================================================= */

/* lima_drm.h:14-18 */
enum drm_lima_param_gpu_id {
    DRM_LIMA_PARAM_GPU_ID_UNKNOWN = 0,
    DRM_LIMA_PARAM_GPU_ID_MALI400 = 1,
    DRM_LIMA_PARAM_GPU_ID_MALI450 = 2,
};

/* lima_drm.h:20-25 */
enum drm_lima_param {
    DRM_LIMA_PARAM_GPU_ID     = 0,
    DRM_LIMA_PARAM_NUM_PP     = 1,
    DRM_LIMA_PARAM_GP_VERSION = 2,
    DRM_LIMA_PARAM_PP_VERSION = 3,
};

/* lima_drm.h:27-31 */
struct drm_lima_get_param {
    uint32_t param;
    uint32_t pad;
    uint64_t value;
};
_Static_assert(sizeof(struct drm_lima_get_param) == 16,
    "drm_lima_get_param drifted from lima_drm.h:27-31 (expected 16 bytes)");
_Static_assert(offsetof(struct drm_lima_get_param, value) == 8,
    "drm_lima_get_param.value drifted from lima_drm.h:30 (expected offset 8)");

/* lima_drm.h:33 */
#define LIMA_BO_FLAG_HEAP (1U << 0)

/* lima_drm.h:35-40 */
struct drm_lima_gem_create {
    uint32_t size;
    uint32_t flags;
    uint32_t handle;
    uint32_t pad;
};
_Static_assert(sizeof(struct drm_lima_gem_create) == 16,
    "drm_lima_gem_create drifted from lima_drm.h:35-40 (expected 16 bytes)");

/* lima_drm.h:42-46 */
struct drm_lima_gem_info {
    uint32_t handle;
    uint32_t va;
    uint64_t offset;
};
_Static_assert(sizeof(struct drm_lima_gem_info) == 16,
    "drm_lima_gem_info drifted from lima_drm.h:42-46 (expected 16 bytes)");
_Static_assert(offsetof(struct drm_lima_gem_info, offset) == 8,
    "drm_lima_gem_info.offset drifted from lima_drm.h:45 (expected offset 8)");

/* lima_drm.h:112-115 */
struct drm_lima_ctx_create {
    uint32_t id;
    uint32_t _pad;
};
_Static_assert(sizeof(struct drm_lima_ctx_create) == 8,
    "drm_lima_ctx_create drifted from lima_drm.h:112-115 (expected 8 bytes)");

/* lima_drm.h:117-120 */
struct drm_lima_ctx_free {
    uint32_t id;
    uint32_t _pad;
};
_Static_assert(sizeof(struct drm_lima_ctx_free) == 8,
    "drm_lima_ctx_free drifted from lima_drm.h:117-120 (expected 8 bytes)");

/*
 * lima_drm.h:122-136 -- ioctl numbers.
 *
 * DRM_IOCTL_BASE ('d') and DRM_COMMAND_BASE (0x40) are the two constants
 * every DRM ioctl -- on Linux and on this FreeBSD port alike -- is built
 * from.
 *
 * IMPORTANT, and the reason this is not just Linux's asm-generic/ioctl.h
 * bit math copy-pasted: lima_drm.h's own DRM_IOWR/DRM_IOW/DRM_IOR macros
 * come from drm-kmod's <uapi/drm/drm.h>, which on FreeBSD defines them in
 * terms of THIS platform's native <sys/ioccom.h> _IOWR/_IOW/_IOR -- not
 * Linux's asm-generic ones.  That distinction is operational, not
 * cosmetic: FreeBSD's generic ioctl(2) syscall path decodes the copy-in /
 * copy-out direction and the argument size from the raw command word using
 * ioccom.h's bit positions BEFORE it ever calls into the driver. Linux's
 * dir-bit layout and BSD's do not agree bit-for-bit on the write-only
 * (DRM_IOW) or read-only (DRM_IOR) shapes -- only the read+write (DRM_IOWR)
 * shape happens to come out numerically identical either way, because
 * "both direction bits set" means the same 0xC0000000 pattern under both
 * schemes.  Building these constants with Linux's dir-bit formula instead
 * of ioccom.h's would silently miscompile CTX_FREE, a DRM_IOW ioctl: under
 * BSD's decode of a Linux-style "write" command word, only the "copy out"
 * bit would appear set, so the kernel would never copy the caller's `id`
 * field in, and the driver would always see id=0 (and would then copy a
 * zeroed struct back over the caller's memory afterwards, since "copy out"
 * looked set instead).  Using <sys/ioccom.h>'s own _IOWR/_IOW/_IOR -- the
 * real header on FreeBSD, a labelled reproduction above on any other host
 * for the syntax check only -- is what keeps every constant below
 * numerically identical to what lima_drm.h itself expands to when it is
 * compiled into lima.ko.
 */
#define DRM_IOCTL_BASE   'd'
#define DRM_COMMAND_BASE 0x40

#define DRM_LIMA_GET_PARAM  0x00  /* lima_drm.h:122 */
#define DRM_LIMA_GEM_CREATE 0x01  /* lima_drm.h:123 */
#define DRM_LIMA_GEM_INFO   0x02  /* lima_drm.h:124 */
#define DRM_LIMA_CTX_CREATE 0x05  /* lima_drm.h:127 */
#define DRM_LIMA_CTX_FREE   0x06  /* lima_drm.h:128 */

/* lima_drm.h:130 */
#define DRM_IOCTL_LIMA_GET_PARAM \
    _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + DRM_LIMA_GET_PARAM, \
        struct drm_lima_get_param)
/* lima_drm.h:131 */
#define DRM_IOCTL_LIMA_GEM_CREATE \
    _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + DRM_LIMA_GEM_CREATE, \
        struct drm_lima_gem_create)
/* lima_drm.h:132 */
#define DRM_IOCTL_LIMA_GEM_INFO \
    _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + DRM_LIMA_GEM_INFO, \
        struct drm_lima_gem_info)
/* lima_drm.h:135 */
#define DRM_IOCTL_LIMA_CTX_CREATE \
    _IOR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + DRM_LIMA_CTX_CREATE, \
        struct drm_lima_ctx_create)
/* lima_drm.h:136 */
#define DRM_IOCTL_LIMA_CTX_FREE \
    _IOW(DRM_IOCTL_BASE, DRM_COMMAND_BASE + DRM_LIMA_CTX_FREE, \
        struct drm_lima_ctx_free)

/* =========================================================================
 * Test harness
 * ========================================================================= */

#define SMOKE_BO_SIZE  4096u
#define SMOKE_BO_WORDS (SMOKE_BO_SIZE / 4u)

static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;
static int g_step = 0;

static void
step_header(const char *title)
{
    g_step++;
    printf("\n-- [%d] %s --\n", g_step, title);
    fflush(stdout);
}

static void
step_pass(const char *label)
{
    printf("PASS  %s\n", label);
    g_pass++;
}

static void
step_fail(const char *label, const char *why)
{
    printf("FAIL  %s -- %s\n", label, why);
    g_fail++;
}

static void
step_skip(const char *label, const char *why)
{
    printf("SKIP  %s -- %s\n", label, why);
    g_skip++;
}

static void
errno_why(char *buf, size_t buflen, const char *what)
{
    snprintf(buf, buflen, "%s failed: %s (errno=%d)", what, strerror(errno), errno);
}

/* sigsetjmp/siglongjmp target for the mmap write/read-back region below --
 * see the long comment at that call site for why this exists. */
static sigjmp_buf g_fault_jmp;
static volatile int g_fault_signum;

static void
fault_handler(int signum)
{
    g_fault_signum = signum;
    siglongjmp(g_fault_jmp, 1);
}

/*
 * Mali GP/PP "version" register decode.  Read directly from this repo's
 * kernel source, not guessed:
 *   - Register layout -- top 16 bits = product id, next 8 = major, low 8 =
 *     minor -- lima_gp.c:338-348 (lima_gp_print_version) and lima_pp.c:
 *     372-392 (lima_pp_print_version): same layout, two different
 *     product-id spaces (GP and PP are separate IP blocks on this SoC).
 *   - The raw register is stored verbatim into dev->gp_version /
 *     dev->pp_version (lima_gp.c:408; lima_pp.c:474) and returned as-is by
 *     GET_PARAM (lima_drv.c:117-120) -- so what GET_PARAM hands back is
 *     exactly what lima_{gp,pp}_print_version() already decoded once at
 *     attach time.
 *   - This board's attach banner already printed "gp - mali400 version
 *     major 1 minor 1" / "pp0 - mali400 version major 1 minor 1" / "pp1 -
 *     mali400 version major 1 minor 1" (docs/MALI-STATUS.md:9-11) --
 *     that is what fixes major=1, minor=1 as the expected values below,
 *     not a guess, and it is a hardware-identity register (Mali-400 r1p1's
 *     core revision), stable regardless of clock/PLL configuration.
 */
struct mali_version {
    uint32_t raw;
    uint32_t product_id;
    uint32_t major;
    uint32_t minor;
};

static struct mali_version
decode_mali_version(uint64_t reg64)
{
    struct mali_version v;
    uint32_t reg = (uint32_t)reg64;

    v.raw        = reg;
    v.product_id = reg >> 16;
    v.major      = (reg >> 8) & 0xFFu;
    v.minor      = reg & 0xFFu;
    return v;
}

/* lima_gp.c:342-348 */
static const char *
gp_product_name(uint32_t product_id)
{
    switch (product_id) {
    case 0xA07: return "mali200";
    case 0xC07: return "mali300";
    case 0xB07: return "mali400";
    case 0xD07: return "mali450";
    default:    return "unknown";
    }
}

/* lima_pp.c:376-392 */
static const char *
pp_product_name(uint32_t product_id)
{
    switch (product_id) {
    case 0xC807: return "mali200";
    case 0xCE07: return "mali300";
    case 0xCD07: return "mali400";
    case 0xCF07: return "mali450";
    default:     return "unknown";
    }
}

int
main(int argc, char **argv)
{
    const char *devpath = "/dev/dri/renderD128";
    int fd = -1;
    int have_ctx = 0;
    uint32_t ctx_id = 0;
    int have_handle = 0;
    uint32_t handle = 0;
    int have_offset = 0;
    uint64_t bo_offset = 0;
    void *map = MAP_FAILED;
    char why[192];
    int overall_ok;

    if (argc > 1)
        devpath = argv[1];

    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== lima_ioctl_smoke: Tier 0 ioctl-plumbing test (docs/PLAN-mesa-lima.md sec1.2) ===\n");
    printf("device: %s\n", devpath);

    /* ---- [1] open ------------------------------------------------------ */
    step_header("open(O_RDWR)");
    fd = open(devpath, O_RDWR);
    if (fd < 0) {
        errno_why(why, sizeof why, "open");
        step_fail("open", why);
        printf("\n=== SUMMARY: %d passed, %d failed, %d skipped -- FAIL (no fd, cannot continue) ===\n",
            g_pass, g_fail, g_skip);
        return 1;
    }
    step_pass("open");

    /* ---- [2-5] GET_PARAM x4 --------------------------------------------*/
    {
        struct drm_lima_get_param gp;

        step_header("GET_PARAM(GPU_ID)");
        memset(&gp, 0, sizeof gp);
        gp.param = DRM_LIMA_PARAM_GPU_ID;
        if (ioctl(fd, DRM_IOCTL_LIMA_GET_PARAM, &gp) < 0) {
            errno_why(why, sizeof why, "ioctl(GET_PARAM GPU_ID)");
            step_fail("GET_PARAM(GPU_ID)", why);
        } else {
            printf("   GPU_ID = %" PRIu64 "\n", (uint64_t)gp.value);
            if (gp.value == DRM_LIMA_PARAM_GPU_ID_MALI400) {
                step_pass("GET_PARAM(GPU_ID) == DRM_LIMA_PARAM_GPU_ID_MALI400 (1)");
            } else {
                snprintf(why, sizeof why, "got %" PRIu64 ", want %u (MALI400)",
                    (uint64_t)gp.value, (unsigned)DRM_LIMA_PARAM_GPU_ID_MALI400);
                step_fail("GET_PARAM(GPU_ID)", why);
            }
        }

        step_header("GET_PARAM(NUM_PP)");
        memset(&gp, 0, sizeof gp);
        gp.param = DRM_LIMA_PARAM_NUM_PP;
        if (ioctl(fd, DRM_IOCTL_LIMA_GET_PARAM, &gp) < 0) {
            errno_why(why, sizeof why, "ioctl(GET_PARAM NUM_PP)");
            step_fail("GET_PARAM(NUM_PP)", why);
        } else {
            printf("   NUM_PP = %" PRIu64 "\n", (uint64_t)gp.value);
            if (gp.value == 2) {
                step_pass("GET_PARAM(NUM_PP) == 2");
            } else {
                snprintf(why, sizeof why, "got %" PRIu64 ", want 2 (pp0+pp1)", (uint64_t)gp.value);
                step_fail("GET_PARAM(NUM_PP)", why);
            }
        }

        step_header("GET_PARAM(GP_VERSION)");
        memset(&gp, 0, sizeof gp);
        gp.param = DRM_LIMA_PARAM_GP_VERSION;
        if (ioctl(fd, DRM_IOCTL_LIMA_GET_PARAM, &gp) < 0) {
            errno_why(why, sizeof why, "ioctl(GET_PARAM GP_VERSION)");
            step_fail("GET_PARAM(GP_VERSION)", why);
        } else {
            struct mali_version v = decode_mali_version(gp.value);
            printf("   GP_VERSION raw=0x%08" PRIx32 " product=0x%03" PRIx32 " (%s) major=%" PRIu32 " minor=%" PRIu32 "\n",
                v.raw, v.product_id, gp_product_name(v.product_id), v.major, v.minor);
            if (v.product_id == 0xB07 && v.major == 1 && v.minor == 1) {
                step_pass("GET_PARAM(GP_VERSION) == mali400 major 1 minor 1");
            } else {
                snprintf(why, sizeof why,
                    "product=0x%03" PRIx32 " major=%" PRIu32 " minor=%" PRIu32 ", want product=0xb07 major=1 minor=1",
                    v.product_id, v.major, v.minor);
                step_fail("GET_PARAM(GP_VERSION)", why);
            }
        }

        step_header("GET_PARAM(PP_VERSION)");
        memset(&gp, 0, sizeof gp);
        gp.param = DRM_LIMA_PARAM_PP_VERSION;
        if (ioctl(fd, DRM_IOCTL_LIMA_GET_PARAM, &gp) < 0) {
            errno_why(why, sizeof why, "ioctl(GET_PARAM PP_VERSION)");
            step_fail("GET_PARAM(PP_VERSION)", why);
        } else {
            struct mali_version v = decode_mali_version(gp.value);
            printf("   PP_VERSION raw=0x%08" PRIx32 " product=0x%03" PRIx32 " (%s) major=%" PRIu32 " minor=%" PRIu32 "\n",
                v.raw, v.product_id, pp_product_name(v.product_id), v.major, v.minor);
            if (v.product_id == 0xCD07 && v.major == 1 && v.minor == 1) {
                step_pass("GET_PARAM(PP_VERSION) == mali400 major 1 minor 1");
            } else {
                snprintf(why, sizeof why,
                    "product=0x%03" PRIx32 " major=%" PRIu32 " minor=%" PRIu32 ", want product=0xcd07 major=1 minor=1",
                    v.product_id, v.major, v.minor);
                step_fail("GET_PARAM(PP_VERSION)", why);
            }
        }
    }

    /* ---- [6] CTX_CREATE --------------------------------------------- */
    step_header("CTX_CREATE");
    {
        struct drm_lima_ctx_create cc;
        memset(&cc, 0, sizeof cc);
        if (ioctl(fd, DRM_IOCTL_LIMA_CTX_CREATE, &cc) < 0) {
            errno_why(why, sizeof why, "ioctl(CTX_CREATE)");
            step_fail("CTX_CREATE", why);
        } else {
            ctx_id = cc.id;
            have_ctx = 1;
            printf("   ctx_id = %" PRIu32 "\n", ctx_id);
            step_pass("CTX_CREATE");
        }
    }

    /* ---- [7] GEM_CREATE(size=4096, flags=0) -------------------------- */
    step_header("GEM_CREATE(size=4096, flags=0)");
    {
        struct drm_lima_gem_create gc;
        memset(&gc, 0, sizeof gc);
        gc.size = SMOKE_BO_SIZE;
        gc.flags = 0;
        if (ioctl(fd, DRM_IOCTL_LIMA_GEM_CREATE, &gc) < 0) {
            errno_why(why, sizeof why, "ioctl(GEM_CREATE)");
            step_fail("GEM_CREATE", why);
        } else {
            printf("   handle = %" PRIu32 "\n", gc.handle);
            /*
             * General DRM-core convention, not verified by reading
             * drm-kmod's drm_gem.c in this sandbox (it is fetched by pkg,
             * not vendored in this repo -- see hal/lima/Makefile): handle 0
             * is always invalid, every driver's handle allocator starts at
             * 1. Kept as a real check because it is a decades-stable,
             * DRM-wide invariant, not a lima-specific guess.
             */
            if (gc.handle != 0) {
                handle = gc.handle;
                have_handle = 1;
                step_pass("GEM_CREATE");
            } else {
                step_fail("GEM_CREATE",
                    "ioctl succeeded but returned handle 0 (DRM-core convention: 0 is always invalid)");
            }
        }
    }

    /* ---- [NEGATIVE] GEM_CREATE(flags=LIMA_BO_FLAG_HEAP) -------------- */
    step_header("NEGATIVE: GEM_CREATE(flags=LIMA_BO_FLAG_HEAP) must fail ENOSYS (lima_gem.c:28-37)");
    {
        struct drm_lima_gem_create hc;
        memset(&hc, 0, sizeof hc);
        hc.size = SMOKE_BO_SIZE;
        hc.flags = LIMA_BO_FLAG_HEAP;
        if (ioctl(fd, DRM_IOCTL_LIMA_GEM_CREATE, &hc) == 0) {
            /*
             * Unexpected success: lima_heap_alloc() no longer stubbed to
             * -ENOSYS.  No lima-specific "free one GEM handle" ioctl exists
             * to release hc.handle here -- close(fd) at the end of this
             * program releases it via the DRM core's per-file handle-table
             * teardown, same as every other handle in this test.
             */
            snprintf(why, sizeof why,
                "unexpectedly SUCCEEDED (handle=%" PRIu32 ") -- lima_heap_alloc() no longer -ENOSYS?",
                hc.handle);
            step_fail("NEGATIVE GEM_CREATE(HEAP)", why);
        } else if (errno == ENOSYS) {
            step_pass("NEGATIVE GEM_CREATE(HEAP) correctly failed ENOSYS");
        } else {
            errno_why(why, sizeof why, "ioctl(GEM_CREATE HEAP)");
            step_fail("NEGATIVE GEM_CREATE(HEAP)", why);
        }
    }

    /* ---- [9] GEM_INFO(handle) (runtime step number; the negative test
     * above consumes step [8]) ------------------------------------------*/
    step_header("GEM_INFO(handle)");
    if (!have_handle) {
        step_skip("GEM_INFO", "no handle (GEM_CREATE step above did not produce one)");
    } else {
        struct drm_lima_gem_info gi;
        memset(&gi, 0, sizeof gi);
        gi.handle = handle;
        if (ioctl(fd, DRM_IOCTL_LIMA_GEM_INFO, &gi) < 0) {
            errno_why(why, sizeof why, "ioctl(GEM_INFO)");
            step_fail("GEM_INFO", why);
        } else {
            long pagesize = sysconf(_SC_PAGESIZE);

            printf("   va = 0x%08" PRIx32 "\n", gi.va);
            printf("   offset = 0x%" PRIx64 " (%" PRIu64 ")\n", gi.offset, gi.offset);
            if (pagesize > 0 && (gi.offset % (uint64_t)pagesize) != 0) {
                printf("   NOTE: offset is not a multiple of the page size (%ld) -- mmap() below will EINVAL\n",
                    pagesize);
            }
            bo_offset = gi.offset;
            have_offset = 1;
            step_pass("GEM_INFO");
        }
    }

    /* ---- [10] mmap + write pattern + read back + compare -------------*/
    step_header("mmap / write pattern / read back / compare");
    if (!have_offset) {
        step_skip("mmap write/read-back", "no offset (GEM_INFO step above did not produce one)");
    } else {
        map = mmap(NULL, SMOKE_BO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)bo_offset);
        if (map == MAP_FAILED) {
            errno_why(why, sizeof why, "mmap");
            step_fail("mmap", why);
        } else {
            struct sigaction sa, old_bus, old_segv;

            printf("   mmap ok: ptr=%p size=%u\n", map, SMOKE_BO_SIZE);

            /*
             * Guard the actual memory touch, not just the ioctls: if the
             * shmem-helper's page-fault path (drm_gem_shmem_vm_ops) is
             * broken, the FIRST access below can SIGBUS/SIGSEGV rather
             * than return an errno. Catching that here turns a silent
             * process-killed-by-signal (no summary line at all -- see
             * docs/PLAN-mesa-lima.md sec1.2, "if this hangs or crashes") into an
             * attributed FAIL with a one-line summary still printed.
             */
            memset(&sa, 0, sizeof sa);
            sa.sa_handler = fault_handler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;
            sigaction(SIGBUS, &sa, &old_bus);
            sigaction(SIGSEGV, &sa, &old_segv);

            if (sigsetjmp(g_fault_jmp, 1) == 0) {
                volatile uint32_t *words = (volatile uint32_t *)map;
                size_t i;
                size_t mismatches = 0;
                size_t first_bad = (size_t)-1;
                uint32_t first_got = 0;
                uint32_t first_want = 0;

                for (i = 0; i < SMOKE_BO_WORDS; i++)
                    words[i] = (uint32_t)(0xC0FFEE00u ^ (uint32_t)i);

                for (i = 0; i < SMOKE_BO_WORDS; i++) {
                    uint32_t want = (uint32_t)(0xC0FFEE00u ^ (uint32_t)i);
                    uint32_t got = words[i];
                    if (got != want) {
                        if (first_bad == (size_t)-1) {
                            first_bad = i;
                            first_got = got;
                            first_want = want;
                        }
                        mismatches++;
                    }
                }

                printf("   wrote %u bytes, read back %u bytes, compared %u words\n",
                    SMOKE_BO_SIZE, SMOKE_BO_SIZE, SMOKE_BO_WORDS);

                if (mismatches == 0) {
                    step_pass("mmap write/read-back byte-compare");
                } else {
                    snprintf(why, sizeof why,
                        "%zu/%u words mismatched; first at word %zu (byte %zu): got 0x%08" PRIx32 " want 0x%08" PRIx32,
                        mismatches, SMOKE_BO_WORDS, first_bad, first_bad * 4, first_got, first_want);
                    step_fail("mmap write/read-back byte-compare", why);
                }
            } else {
                snprintf(why, sizeof why,
                    "signal %d (%s) touching the mmap'd BO -- shmem-helper page-fault path, not an ioctl-argument bug",
                    g_fault_signum, g_fault_signum == SIGBUS ? "SIGBUS" : "SIGSEGV");
                step_fail("mmap write/read-back byte-compare", why);
            }

            sigaction(SIGBUS, &old_bus, NULL);
            sigaction(SIGSEGV, &old_segv, NULL);
        }
    }

    /* ---- [11] munmap ---------------------------------------------------*/
    step_header("munmap");
    if (map == MAP_FAILED) {
        step_skip("munmap", "no mapping to release (mmap step above failed or was skipped)");
    } else if (munmap(map, SMOKE_BO_SIZE) < 0) {
        errno_why(why, sizeof why, "munmap");
        step_fail("munmap", why);
    } else {
        step_pass("munmap");
    }

    /* ---- [12] CTX_FREE(ctx_id) ------------------------------------------*/
    step_header("CTX_FREE(ctx_id)");
    if (!have_ctx) {
        step_skip("CTX_FREE", "no ctx_id (CTX_CREATE step above did not produce one)");
    } else {
        struct drm_lima_ctx_free cf;
        memset(&cf, 0, sizeof cf);
        cf.id = ctx_id;
        if (ioctl(fd, DRM_IOCTL_LIMA_CTX_FREE, &cf) < 0) {
            errno_why(why, sizeof why, "ioctl(CTX_FREE)");
            step_fail("CTX_FREE", why);
        } else {
            step_pass("CTX_FREE");
        }
    }

    /* ---- [13] close(fd) --------------------------------------------------*/
    step_header("close(fd)");
    if (close(fd) < 0) {
        errno_why(why, sizeof why, "close");
        step_fail("close", why);
    } else {
        step_pass("close");
    }

    overall_ok = (g_fail == 0 && g_skip == 0);
    printf("\n=== SUMMARY: %d passed, %d failed, %d skipped -- %s ===\n",
        g_pass, g_fail, g_skip, overall_ok ? "PASS" : "FAIL");
    return overall_ok ? 0 : 1;
}
