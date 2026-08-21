/*
 * lima_pp_clear.c — Mali-400 PP "clear only, no geometry" job, submitted
 * through lima's real ioctls.
 *
 * Self-contained the same way the Tier-0 ioctl smoke test is: no libdrm, no
 * drm-kmod headers. Every struct and ioctl number below is redefined locally
 * from hal/lima/drm/lima_drm.h and cross-checked with _Static_assert. Do not
 * add a libdrm/drm-kmod #include here — the point is this file compiles and
 * links against nothing but the base system.
 *
 * Frame-field values are documented word-by-word, with provenance, in
 * hal/lima/tests/PP-CLEAR-FRAME.md. Read that file before touching the
 * constants below — in particular section 6.2, which names the ONE value in
 * this whole program that is an unverified guess rather than a sourced fact:
 * the per-tile polygon-list scratch memory is left zero-filled, and no known
 * driver (old Limare, current Mesa, ARM's own GPL driver) has ever exercised
 * a PP submission with no preceding GP job, which is exactly what this
 * program does. The kernel performs *no* content validation of any of this
 * (tests/PP-CLEAR-FRAME.md section 7) — a bad value here goes straight to real
 * Mali-400 silicon.
 *
 * THIS PROGRAM HAS NEVER BEEN RUN. It has been read for syntax/type
 * correctness only (see the build note at the bottom of this file). Do not
 * treat a clean compile as evidence it works.
 *
 * Build (syntax/type check only, on a non-FreeBSD host):
 *   gcc -std=c11 -Wall -Wextra -Werror -o /dev/null -c lima_pp_clear.c
 *
 * Real use (never done in this session):
 *   ./lima_pp_clear --i-know-this-can-hang-the-gpu
 */

/* Requested before any system header: under strict -std=c11, glibc hides
 * clock_gettime()/CLOCK_MONOTONIC unless POSIX.1-2008 visibility is asked
 * for explicitly (discovered by the compile-check this file's own build
 * note asks for — see the bottom of this file). Harmless on FreeBSD, which
 * honours the same macro. */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

/* ===========================================================================
 * 1. UAPI, redefined locally from hal/lima/drm/lima_drm.h.
 *    Field names/order/types match that header exactly (verified against it,
 *    plus an independent copy at /usr/include/drm/lima_drm.h on this host,
 *    plus upstream github.com/torvalds/linux — all three agree byte-for-byte,
 *    see tests/PP-CLEAR-FRAME.md section 3). __u32/__u64/__s64 -> uint32_t/
 *    uint64_t/int64_t: identical ABI, different spelling only.
 * ======================================================================== */

enum drm_lima_param {
	DRM_LIMA_PARAM_GPU_ID,
	DRM_LIMA_PARAM_NUM_PP,
	DRM_LIMA_PARAM_GP_VERSION,
	DRM_LIMA_PARAM_PP_VERSION,
};
#define DRM_LIMA_PARAM_GPU_ID_MALI400 1u

struct drm_lima_get_param {
	uint32_t param;
	uint32_t pad;
	uint64_t value;
};
_Static_assert(sizeof(struct drm_lima_get_param) == 16,
    "drm_lima_get_param must match the kernel UAPI size exactly");

#define LIMA_BO_FLAG_HEAP (1u << 0)

struct drm_lima_gem_create {
	uint32_t size;
	uint32_t flags;
	uint32_t handle;
	uint32_t pad;
};
_Static_assert(sizeof(struct drm_lima_gem_create) == 16,
    "drm_lima_gem_create must match the kernel UAPI size exactly");

struct drm_lima_gem_info {
	uint32_t handle;
	uint32_t va;
	uint64_t offset;
};
_Static_assert(sizeof(struct drm_lima_gem_info) == 16,
    "drm_lima_gem_info must match the kernel UAPI size exactly");

#define LIMA_SUBMIT_BO_READ  0x01u
#define LIMA_SUBMIT_BO_WRITE 0x02u

struct drm_lima_gem_submit_bo {
	uint32_t handle;
	uint32_t flags;
};
_Static_assert(sizeof(struct drm_lima_gem_submit_bo) == 8,
    "drm_lima_gem_submit_bo must match the kernel UAPI size exactly");

#define LIMA_PP_FRAME_REG_NUM 23
#define LIMA_PP_WB_REG_NUM    12

/* Mali-400 PP frame — see tests/PP-CLEAR-FRAME.md sections 3-5 for every field. */
struct drm_lima_m400_pp_frame {
	uint32_t frame[LIMA_PP_FRAME_REG_NUM];
	uint32_t num_pp;
	uint32_t wb[3 * LIMA_PP_WB_REG_NUM];
	uint32_t plbu_array_address[4];
	uint32_t fragment_stack_address[4];
};
_Static_assert(sizeof(struct drm_lima_m400_pp_frame) == 272,
    "drm_lima_m400_pp_frame must be exactly 272 bytes "
    "(23 + 1 + 36 + 4 + 4 = 68 words) or lima_ioctl_gem_submit's "
    "frame_size check (hal/lima/lima_drv.c) will reject this frame");

#define LIMA_PIPE_GP 0x00u
#define LIMA_PIPE_PP 0x01u

#define LIMA_SUBMIT_FLAG_EXPLICIT_FENCE (1u << 0)

struct drm_lima_gem_submit {
	uint32_t ctx;
	uint32_t pipe;
	uint32_t nr_bos;
	uint32_t frame_size;
	uint64_t bos;
	uint64_t frame;
	uint32_t flags;
	uint32_t out_sync;
	uint32_t in_sync[2];
};
_Static_assert(sizeof(struct drm_lima_gem_submit) == 48,
    "drm_lima_gem_submit must match the kernel UAPI size exactly");

#define LIMA_GEM_WAIT_READ  0x01u
#define LIMA_GEM_WAIT_WRITE 0x02u

struct drm_lima_gem_wait {
	uint32_t handle;
	uint32_t op;
	int64_t  timeout_ns;
};
_Static_assert(sizeof(struct drm_lima_gem_wait) == 16,
    "drm_lima_gem_wait must match the kernel UAPI size exactly");

struct drm_lima_ctx_create {
	uint32_t id;
	uint32_t _pad;
};
_Static_assert(sizeof(struct drm_lima_ctx_create) == 8,
    "drm_lima_ctx_create must match the kernel UAPI size exactly");

/* Same shape as drm_lima_ctx_create, but the real UAPI header
 * (hal/lima/drm/lima_drm.h:117-120) gives it a distinct name; mirrored here
 * rather than reusing drm_lima_ctx_create, even though the ioctl-number
 * macro below only cares about sizeof(). */
struct drm_lima_ctx_free {
	uint32_t id;
	uint32_t _pad;
};
_Static_assert(sizeof(struct drm_lima_ctx_free) == 8,
    "drm_lima_ctx_free must match the kernel UAPI size exactly");

/* ---------------------------------------------------------------------------
 * ioctl numbers. Hand-rolled from the same standard Linux ioctl-number
 * encoding drm-kmod ports faithfully (dir<<30 | size<<16 | type<<8 | nr,
 * type='d', DRM_COMMAND_BASE=0x40 for driver-private ioctls) — confirmed
 * against a real installed copy of <drm/drm.h> on this host
 * (/usr/include/drm/drm.h: DRM_IOCTL_BASE='d', DRM_COMMAND_BASE=0x40,
 * DRM_IOR/IOW/IOWR expand to exactly this). Deliberately not #include-ing
 * that header, or any drm-kmod header, per this file's own self-containment
 * rule.
 * ------------------------------------------------------------------------- */

#define LIMA_IOCTL_TYPE 'd'
#define LIMA_IOC_NONE   0u
#define LIMA_IOC_WRITE  1u
#define LIMA_IOC_READ   2u

#define LIMA_IOC(dir, nr, size) \
	((unsigned long)( \
	    (((unsigned long)(dir)) << 30) | \
	    ((((unsigned long)(size)) & 0x1FFFUL) << 16) | \
	    (((unsigned long)(LIMA_IOCTL_TYPE)) << 8) | \
	    ((unsigned long)(nr))))

#define LIMA_IOR(nr, type)  LIMA_IOC(LIMA_IOC_READ, nr, sizeof(type))
#define LIMA_IOW(nr, type)  LIMA_IOC(LIMA_IOC_WRITE, nr, sizeof(type))
#define LIMA_IOWR(nr, type) LIMA_IOC(LIMA_IOC_READ | LIMA_IOC_WRITE, nr, sizeof(type))

#define DRM_COMMAND_BASE 0x40u

#define DRM_LIMA_GET_PARAM  0x00u
#define DRM_LIMA_GEM_CREATE 0x01u
#define DRM_LIMA_GEM_INFO   0x02u
#define DRM_LIMA_GEM_SUBMIT 0x03u
#define DRM_LIMA_GEM_WAIT   0x04u
#define DRM_LIMA_CTX_CREATE 0x05u
#define DRM_LIMA_CTX_FREE   0x06u

#define DRM_IOCTL_LIMA_GET_PARAM  LIMA_IOWR(DRM_COMMAND_BASE + DRM_LIMA_GET_PARAM,  struct drm_lima_get_param)
#define DRM_IOCTL_LIMA_GEM_CREATE LIMA_IOWR(DRM_COMMAND_BASE + DRM_LIMA_GEM_CREATE, struct drm_lima_gem_create)
#define DRM_IOCTL_LIMA_GEM_INFO   LIMA_IOWR(DRM_COMMAND_BASE + DRM_LIMA_GEM_INFO,   struct drm_lima_gem_info)
#define DRM_IOCTL_LIMA_GEM_SUBMIT LIMA_IOW (DRM_COMMAND_BASE + DRM_LIMA_GEM_SUBMIT, struct drm_lima_gem_submit)
#define DRM_IOCTL_LIMA_GEM_WAIT   LIMA_IOW (DRM_COMMAND_BASE + DRM_LIMA_GEM_WAIT,   struct drm_lima_gem_wait)
#define DRM_IOCTL_LIMA_CTX_CREATE LIMA_IOR (DRM_COMMAND_BASE + DRM_LIMA_CTX_CREATE, struct drm_lima_ctx_create)
#define DRM_IOCTL_LIMA_CTX_FREE   LIMA_IOW (DRM_COMMAND_BASE + DRM_LIMA_CTX_FREE,   struct drm_lima_ctx_free)

/* ===========================================================================
 * 2. Frame-content constants. Every value here is explained, with
 *    provenance, in tests/PP-CLEAR-FRAME.md sections 4/5/6. Do not change a value
 *    here without updating that document, and vice versa.
 * ======================================================================== */

#define TARGET_WIDTH  64u
#define TARGET_HEIGHT 64u
#define TARGET_BPP    4u
#define TARGET_BO_SIZE (TARGET_WIDTH * TARGET_HEIGHT * TARGET_BPP)  /* 16384 */

/* 16px-per-tile hardware tile grid (tests/PP-CLEAR-FRAME.md section 6.1). */
#define TILE_PX       16u
#define TILE_GRID_W   (TARGET_WIDTH  / TILE_PX)   /* 4 */
#define TILE_GRID_H   (TARGET_HEIGHT / TILE_PX)   /* 4 */
#define NUM_TILES     (TILE_GRID_W * TILE_GRID_H) /* 16 */
#define TILE_POLY_SCRATCH_SIZE 0x200u              /* per tile, section 6.2 */

/* Scratch BO layout (tests/PP-CLEAR-FRAME.md section 8.2). Each region must not
 * overlap the next: render_state [0x000,0x040), tile array [0x100,0x210)
 * (NUM_TILES*16 + 16 bytes = 272 = 0x110), so poly scratch cannot start
 * before 0x210 -- rounded up to 0x400 for a clean, generously separated
 * boundary rather than packing tight. */
#define SCRATCH_RENDER_STATE_OFF 0x000u  /* 64 bytes, all zero */
#define SCRATCH_TILE_ARRAY_OFF   0x100u  /* ends at 0x210, NUM_TILES*16 + 16 bytes */
#define SCRATCH_POLY_SCRATCH_OFF 0x400u  /* NUM_TILES * TILE_POLY_SCRATCH_SIZE, ends at 0x2400 */
#define SCRATCH_BO_SIZE          0x3000u /* 12288, comfortably covers all three (need 0x2400) */

/* Clear colour: four DISTINCT byte values on purpose (tests/PP-CLEAR-FRAME.md
 * section 5.1) so a channel swap is visible in the read-back instead of
 * hiding behind a colour like solid red or grey. Packed as
 * R | G<<8 | B<<16 | A<<24 per Mesa's "Clear Value 8bpc Color" bit layout. */
#define CLEAR_R 0x11u
#define CLEAR_G 0x22u
#define CLEAR_B 0x33u
#define CLEAR_A 0xFFu
#define CLEAR_COLOR_PACKED \
	(CLEAR_R | (CLEAR_G << 8) | (CLEAR_B << 16) | (CLEAR_A << 24))

/* Sentinel the render target is pre-filled with before submission, so a
 * post-wait read-back that still shows the sentinel unambiguously means
 * "the GPU never wrote here" rather than "it wrote the wrong thing". */
#define SENTINEL_BYTE 0xAAu

/* frame[] word indices — names/positions from tests/PP-CLEAR-FRAME.md section 4. */
enum {
	FRAME_W_PLBU_ARRAY_ADDR = 0,  /* clobbered by the kernel, lima_pp.c */
	FRAME_W_RSW             = 1,
	FRAME_W_VERTEX_ADDR     = 2,
	FRAME_W_FLAGS           = 3,
	FRAME_W_CLEAR_DEPTH     = 4,
	FRAME_W_CLEAR_STENCIL   = 5,
	FRAME_W_CLEAR_COLOR0    = 6,
	FRAME_W_CLEAR_COLOR1    = 7,
	FRAME_W_CLEAR_COLOR2    = 8,
	FRAME_W_CLEAR_COLOR3    = 9,
	FRAME_W_BBOX_RIGHT      = 10,
	FRAME_W_BBOX_BOTTOM     = 11,
	FRAME_W_STACK_ADDR      = 12, /* clobbered by the kernel, lima_pp.c */
	FRAME_W_STACK_SIZE      = 13,
	FRAME_W_UNUSED14        = 14,
	FRAME_W_UNUSED15        = 15,
	FRAME_W_ORIGIN_X        = 16,
	FRAME_W_ORIGIN_Y        = 17,
	FRAME_W_SUBPIXEL        = 18,
	FRAME_W_TIEBREAK        = 19,
	FRAME_W_POLY_TILE       = 20,
	FRAME_W_SCALE_FLIP      = 21,
	FRAME_W_CHANNEL_LAYOUT  = 22,
};

/* wb[] word indices, one write-back unit (tests/PP-CLEAR-FRAME.md section 5). */
enum {
	WB_W_TYPE         = 0,
	WB_W_ADDRESS      = 1,
	WB_W_PIXEL_FORMAT = 2,
	WB_W_DOWNSAMPLE   = 3,
	WB_W_PIXEL_LAYOUT = 4,
	WB_W_PITCH        = 5,
	WB_W_FLAGS        = 6,
	WB_W_MRT_BITS     = 7,
	WB_W_MRT_PITCH    = 8,
	WB_W_UNUSED0      = 9,
	WB_W_UNUSED1      = 10,
	WB_W_UNUSED2      = 11,
};

#define LIMA_PP_WB_TYPE_COLOR    0x00000002u
#define LIMA_PIXEL_FORMAT_B8G8R8A8 0x00000003u /* == DRM ARGB8888 byte order, section 5.1 */

/* ===========================================================================
 * 3. Small helpers.
 * ======================================================================== */

static int g_fail_count = 0;

static void
mark_fail(const char *what)
{
	fprintf(stderr, "FAIL: %s\n", what);
	g_fail_count++;
}

static int
xioctl(int fd, unsigned long req, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, req, arg);
	} while (ret == -1 && errno == EINTR);

	return ret;
}

/* Dies immediately — used only for ioctls this program cannot meaningfully
 * continue past (e.g. failing to open the device at all). Every ioctl that
 * is itself part of what we are trying to observe uses xioctl() directly and
 * is checked explicitly instead, so a failure there is reported, not fatal.
 */
static void
must_ioctl(int fd, unsigned long req, void *arg, const char *name)
{
	if (xioctl(fd, req, arg) == -1) {
		fprintf(stderr, "FATAL: ioctl(%s) failed: %s (errno=%d)\n",
		    name, strerror(errno), errno);
		exit(1);
	}
	printf("ok:   ioctl(%s)\n", name);
}

static int64_t
monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		fprintf(stderr, "FATAL: clock_gettime(CLOCK_MONOTONIC) failed: %s\n",
		    strerror(errno));
		exit(1);
	}
	return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/* ===========================================================================
 * 4. Tile-descriptor array (tests/PP-CLEAR-FRAME.md section 6.1). Verified opcodes,
 *    cross-checked against both the 2011-2012 Limare project and the current
 *    Mesa lima Gallium driver, which agree byte-for-byte.
 * ======================================================================== */

static void
build_tile_array(uint8_t *scratch, uint32_t scratch_va)
{
	uint32_t *arr = (uint32_t *)(void *)(scratch + SCRATCH_TILE_ARRAY_OFF);
	unsigned tx, ty;
	unsigned idx = 0;

	for (ty = 0; ty < TILE_GRID_H; ty++) {
		for (tx = 0; tx < TILE_GRID_W; tx++) {
			unsigned tile_index = ty * TILE_GRID_W + tx;
			uint32_t plb_va = scratch_va + SCRATCH_POLY_SCRATCH_OFF +
			    tile_index * TILE_POLY_SCRATCH_SIZE;

			arr[idx++] = 0x00000000u;
			arr[idx++] = 0xB8000000u | tx | (ty << 8);
			arr[idx++] = 0xE0000002u | ((plb_va >> 3) & ~0xE0000003u);
			arr[idx++] = 0xB0000000u;
		}
	}

	/* Terminator record — Mesa's 4-word form (lima_generate_pp_stream),
	 * preferred over Limare's older 2-word form. See section 6.1. */
	arr[idx++] = 0x00000000u;
	arr[idx++] = 0xBC000000u;
	arr[idx++] = 0x00000000u;
	arr[idx++] = 0x00000000u;

	printf("info: tile array built, %u tiles, %u bytes\n",
	    (unsigned)NUM_TILES, (unsigned)(idx * 4));
}

/* ===========================================================================
 * 5. Frame construction (tests/PP-CLEAR-FRAME.md sections 4/5).
 * ======================================================================== */

static void
build_pp_frame(struct drm_lima_m400_pp_frame *f,
    uint32_t target_va, uint32_t render_state_va, uint32_t tile_array_va)
{
	memset(f, 0, sizeof(*f));

	f->frame[FRAME_W_PLBU_ARRAY_ADDR] = 0; /* clobbered, section 3 */
	f->frame[FRAME_W_RSW]             = render_state_va;
	f->frame[FRAME_W_VERTEX_ADDR]     = 0;
	f->frame[FRAME_W_FLAGS]           = 0x00000002u; /* Early-Z / "ACTIVE" */
	f->frame[FRAME_W_CLEAR_DEPTH]     = 0x00FFFFFFu;
	f->frame[FRAME_W_CLEAR_STENCIL]   = 0x00000000u;
	f->frame[FRAME_W_CLEAR_COLOR0]    = CLEAR_COLOR_PACKED;
	f->frame[FRAME_W_CLEAR_COLOR1]    = CLEAR_COLOR_PACKED;
	f->frame[FRAME_W_CLEAR_COLOR2]    = CLEAR_COLOR_PACKED;
	f->frame[FRAME_W_CLEAR_COLOR3]    = CLEAR_COLOR_PACKED;
	f->frame[FRAME_W_BBOX_RIGHT]      = TARGET_WIDTH  - 1u;
	f->frame[FRAME_W_BBOX_BOTTOM]     = TARGET_HEIGHT - 1u;
	f->frame[FRAME_W_STACK_ADDR]      = 0; /* clobbered, section 3 */
	f->frame[FRAME_W_STACK_SIZE]      = 0x00000000u;
	f->frame[FRAME_W_UNUSED14]        = 0;
	f->frame[FRAME_W_UNUSED15]        = 0;
	f->frame[FRAME_W_ORIGIN_X]        = 0x00000001u;
	f->frame[FRAME_W_ORIGIN_Y]        = TARGET_HEIGHT * 2u - 1u;
	f->frame[FRAME_W_SUBPIXEL]        = 0x00000077u;
	f->frame[FRAME_W_TIEBREAK]        = 0x00000001u;
	f->frame[FRAME_W_POLY_TILE]       = 0x00000000u;
	f->frame[FRAME_W_SCALE_FLIP]      = 0x00000E0Cu;
	f->frame[FRAME_W_CHANNEL_LAYOUT]  = 0x00008888u;

	/* Write-back unit 0: the one ARGB8888 linear colour target. */
	f->wb[WB_W_TYPE]         = LIMA_PP_WB_TYPE_COLOR;
	f->wb[WB_W_ADDRESS]      = target_va;
	f->wb[WB_W_PIXEL_FORMAT] = LIMA_PIXEL_FORMAT_B8G8R8A8;
	f->wb[WB_W_DOWNSAMPLE]   = 0;
	f->wb[WB_W_PIXEL_LAYOUT] = 0; /* linear, not tiled */
	f->wb[WB_W_PITCH]        = (TARGET_WIDTH * TARGET_BPP) / 8u;
	f->wb[WB_W_FLAGS]        = 0; /* no R/B swap: B8G8R8A8 == ARGB8888 */
	f->wb[WB_W_MRT_BITS]     = 0;
	f->wb[WB_W_MRT_PITCH]    = 0;
	f->wb[WB_W_UNUSED0]      = 0;
	f->wb[WB_W_UNUSED1]      = 0;
	f->wb[WB_W_UNUSED2]      = 0;
	/* wb[12..35] (units 1 and 2) already zero from memset -> type=DISABLED */

	f->num_pp = 1; /* deliberately not 2 — section 8.3 */
	f->plbu_array_address[0] = tile_array_va;
	/* plbu_array_address[1..3] and fragment_stack_address[0..3] stay 0 */
}

/* ===========================================================================
 * 6. main
 * ======================================================================== */

static void
print_gate_message(void)
{
	fprintf(stderr,
	    "lima_pp_clear: refusing to run.\n"
	    "\n"
	    "This program submits a hand-built Mali-400 PP frame directly to\n"
	    "/dev/dri/renderD128. The kernel driver performs NO content\n"
	    "validation of that frame (hal/lima/lima_pp.c:lima_pp_task_validate\n"
	    "checks only num_pp; hal/lima/lima_drv.c:lima_ioctl_gem_submit checks\n"
	    "only sizes) -- whatever is in it goes straight to real Mali-400\n"
	    "silicon on a board that is in active use.\n"
	    "\n"
	    "One value in this frame is a genuinely UNVERIFIED GUESS, not a\n"
	    "sourced fact: the per-tile polygon-list scratch memory (the small\n"
	    "buffers each tile-descriptor entry points into, see\n"
	    "tests/PP-CLEAR-FRAME.md section 6.2) is left zero-filled. No known\n"
	    "working driver -- not the 2011-2012 Limare project, not the current\n"
	    "Mesa lima Gallium driver, not ARM's own GPL Utgard driver -- has\n"
	    "ever submitted a PP job with no preceding GP job, which is exactly\n"
	    "what this program does. Whether real hardware treats an all-zero\n"
	    "polygon list as \"zero primitives\" or as something else is not\n"
	    "established anywhere this session could reach.\n"
	    "\n"
	    "Read hal/lima/tests/PP-CLEAR-FRAME.md in full before running this.\n"
	    "\n"
	    "If you understand the above and want to proceed anyway, re-run with:\n"
	    "  %s --i-know-this-can-hang-the-gpu\n",
	    "lima_pp_clear");
}

int
main(int argc, char **argv)
{
	int gate_ok = 0;
	int i;
	int fd;
	struct drm_lima_get_param gp;
	struct drm_lima_ctx_create ctx;
	struct drm_lima_gem_create target_create, scratch_create;
	struct drm_lima_gem_info target_info, scratch_info;
	struct drm_lima_gem_submit_bo submit_bos[2];
	struct drm_lima_gem_submit submit;
	struct drm_lima_m400_pp_frame frame;
	struct drm_lima_gem_wait wait_args;
	uint8_t *target_map, *scratch_map;
	uint32_t target_va, scratch_va, render_state_va, tile_array_va;
	int wait_ret;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--i-know-this-can-hang-the-gpu") == 0)
			gate_ok = 1;
	}
	if (!gate_ok) {
		print_gate_message();
		return 1;
	}

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "FATAL: open(/dev/dri/renderD128) failed: %s\n",
		    strerror(errno));
		return 1;
	}
	printf("ok:   open(/dev/dri/renderD128) -> fd=%d\n", fd);

	/* -- Pre-flight sanity: is this even the hardware we think it is? -- */
	memset(&gp, 0, sizeof(gp));
	gp.param = DRM_LIMA_PARAM_GPU_ID;
	must_ioctl(fd, DRM_IOCTL_LIMA_GET_PARAM, &gp, "GET_PARAM(GPU_ID)");
	printf("info: GPU_ID = %llu\n", (unsigned long long)gp.value);
	if (gp.value != DRM_LIMA_PARAM_GPU_ID_MALI400) {
		fprintf(stderr,
		    "FATAL: GPU_ID=%llu, expected %u (MALI400) -- this frame is "
		    "Mali-400-specific, refusing to submit it to different hardware\n",
		    (unsigned long long)gp.value, DRM_LIMA_PARAM_GPU_ID_MALI400);
		close(fd);
		return 1;
	}

	memset(&gp, 0, sizeof(gp));
	gp.param = DRM_LIMA_PARAM_NUM_PP;
	must_ioctl(fd, DRM_IOCTL_LIMA_GET_PARAM, &gp, "GET_PARAM(NUM_PP)");
	printf("info: NUM_PP = %llu\n", (unsigned long long)gp.value);
	if (gp.value < 1) {
		fprintf(stderr, "FATAL: NUM_PP=0, no PP core to submit to\n");
		close(fd);
		return 1;
	}

	memset(&gp, 0, sizeof(gp));
	gp.param = DRM_LIMA_PARAM_GP_VERSION;
	must_ioctl(fd, DRM_IOCTL_LIMA_GET_PARAM, &gp, "GET_PARAM(GP_VERSION)");
	printf("info: GP_VERSION  = 0x%08llx\n", (unsigned long long)gp.value);

	memset(&gp, 0, sizeof(gp));
	gp.param = DRM_LIMA_PARAM_PP_VERSION;
	must_ioctl(fd, DRM_IOCTL_LIMA_GET_PARAM, &gp, "GET_PARAM(PP_VERSION)");
	printf("info: PP_VERSION  = 0x%08llx\n", (unsigned long long)gp.value);

	/* -- Context -- */
	memset(&ctx, 0, sizeof(ctx));
	must_ioctl(fd, DRM_IOCTL_LIMA_CTX_CREATE, &ctx, "CTX_CREATE");
	printf("info: ctx id = %u\n", ctx.id);

	/* -- BOs -- */
	memset(&target_create, 0, sizeof(target_create));
	target_create.size = TARGET_BO_SIZE;
	must_ioctl(fd, DRM_IOCTL_LIMA_GEM_CREATE, &target_create, "GEM_CREATE(target)");
	printf("info: target handle = %u, size = %u\n",
	    target_create.handle, (unsigned)TARGET_BO_SIZE);

	memset(&scratch_create, 0, sizeof(scratch_create));
	scratch_create.size = SCRATCH_BO_SIZE;
	must_ioctl(fd, DRM_IOCTL_LIMA_GEM_CREATE, &scratch_create, "GEM_CREATE(scratch)");
	printf("info: scratch handle = %u, size = %u\n",
	    scratch_create.handle, (unsigned)SCRATCH_BO_SIZE);

	memset(&target_info, 0, sizeof(target_info));
	target_info.handle = target_create.handle;
	must_ioctl(fd, DRM_IOCTL_LIMA_GEM_INFO, &target_info, "GEM_INFO(target)");
	target_va = target_info.va;
	printf("info: target va = 0x%08x, mmap offset = 0x%llx\n",
	    target_va, (unsigned long long)target_info.offset);

	memset(&scratch_info, 0, sizeof(scratch_info));
	scratch_info.handle = scratch_create.handle;
	must_ioctl(fd, DRM_IOCTL_LIMA_GEM_INFO, &scratch_info, "GEM_INFO(scratch)");
	scratch_va = scratch_info.va;
	printf("info: scratch va = 0x%08x, mmap offset = 0x%llx\n",
	    scratch_va, (unsigned long long)scratch_info.offset);

	render_state_va = scratch_va + SCRATCH_RENDER_STATE_OFF;
	tile_array_va   = scratch_va + SCRATCH_TILE_ARRAY_OFF;

	/* -- CPU-side setup: sentinel-fill the target, build the scratch BO -- */
	target_map = mmap(NULL, TARGET_BO_SIZE, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, (off_t)target_info.offset);
	if (target_map == MAP_FAILED) {
		fprintf(stderr, "FATAL: mmap(target) failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	memset(target_map, SENTINEL_BYTE, TARGET_BO_SIZE);
	printf("info: target BO sentinel-filled with 0x%02x\n", SENTINEL_BYTE);

	scratch_map = mmap(NULL, SCRATCH_BO_SIZE, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, (off_t)scratch_info.offset);
	if (scratch_map == MAP_FAILED) {
		fprintf(stderr, "FATAL: mmap(scratch) failed: %s\n", strerror(errno));
		munmap(target_map, TARGET_BO_SIZE);
		close(fd);
		return 1;
	}
	memset(scratch_map, 0, SCRATCH_BO_SIZE);
	/* render_state block: all zero (tests/PP-CLEAR-FRAME.md section 4.1) --
	 * already zero from the memset above, nothing more to write. */
	build_tile_array(scratch_map, scratch_va);
	/* Per-tile polygon-list scratch (offset SCRATCH_POLY_SCRATCH_OFF..end):
	 * left zero from the memset above. THIS IS THE UNVERIFIED GUESS --
	 * see tests/PP-CLEAR-FRAME.md section 6.2 and the gate message above. */

	/* -- Build the frame -- */
	build_pp_frame(&frame, target_va, render_state_va, tile_array_va);
	printf("info: frame built: num_pp=%u plbu_array_address[0]=0x%08x "
	    "wb[0].address=0x%08x wb[0].pitch=%u clear=0x%08x\n",
	    frame.num_pp, frame.plbu_array_address[0],
	    frame.wb[WB_W_ADDRESS], frame.wb[WB_W_PITCH],
	    (unsigned)CLEAR_COLOR_PACKED);

	/* -- Submit -- */
	submit_bos[0].handle = target_create.handle;
	submit_bos[0].flags  = LIMA_SUBMIT_BO_WRITE; /* PP writes the clear here */
	submit_bos[1].handle = scratch_create.handle;
	submit_bos[1].flags  = LIMA_SUBMIT_BO_READ;  /* PP only reads tile data */

	memset(&submit, 0, sizeof(submit));
	submit.ctx        = ctx.id;
	submit.pipe       = LIMA_PIPE_PP;
	submit.nr_bos     = 2;
	submit.frame_size = sizeof(frame);
	submit.bos        = (uint64_t)(uintptr_t)submit_bos;
	submit.frame      = (uint64_t)(uintptr_t)&frame;
	submit.flags      = 0;
	submit.out_sync   = 0;
	submit.in_sync[0] = 0;
	submit.in_sync[1] = 0;

	if (xioctl(fd, DRM_IOCTL_LIMA_GEM_SUBMIT, &submit) == -1) {
		fprintf(stderr, "FATAL: GEM_SUBMIT failed: %s (errno=%d)\n",
		    strerror(errno), errno);
		munmap(scratch_map, SCRATCH_BO_SIZE);
		munmap(target_map, TARGET_BO_SIZE);
		close(fd);
		return 1;
	}
	printf("ok:   GEM_SUBMIT(pipe=PP, nr_bos=2, frame_size=%zu)\n", sizeof(frame));

	/* -- Wait, bounded. timeout_ns is an ABSOLUTE CLOCK_MONOTONIC
	 * deadline (hal/lima/lima_gem.c:lima_gem_wait ->
	 * drm_timeout_abs_to_jiffies), not a relative duration -- passing a
	 * raw relative value here would be interpreted as a deadline in the
	 * distant past and return -ETIMEDOUT immediately regardless of the
	 * GPU. */
	memset(&wait_args, 0, sizeof(wait_args));
	wait_args.handle     = target_create.handle;
	wait_args.op         = LIMA_GEM_WAIT_READ;
	wait_args.timeout_ns = monotonic_ns() + 3LL * 1000000000LL; /* +3s, bounded */

	wait_ret = xioctl(fd, DRM_IOCTL_LIMA_GEM_WAIT, &wait_args);
	if (wait_ret == -1) {
		if (errno == ETIMEDOUT || errno == EBUSY) {
			mark_fail("GEM_WAIT did not signal completion within 3s "
			    "(signal 1 of docs/PLAN-mesa-lima.md section 1.4 failed -- "
			    "suspect the IRQ bridge, or a hang on real hardware)");
		} else {
			fprintf(stderr, "FATAL: GEM_WAIT failed: %s (errno=%d)\n",
			    strerror(errno), errno);
			munmap(scratch_map, SCRATCH_BO_SIZE);
			munmap(target_map, TARGET_BO_SIZE);
			close(fd);
			return 1;
		}
	} else {
		printf("ok:   GEM_WAIT signalled completion\n");
	}

	/* -- Read back and compare -- */
	{
		uint32_t px;
		uint32_t mismatches = 0;
		uint32_t still_sentinel = 0;
		uint32_t printed = 0;
		uint8_t expect[4];

		/* Memory byte order for B8G8R8A8_UNORM == ARGB8888 (section 5.1):
		 * low->high address = B, G, R, A. */
		expect[0] = (uint8_t)CLEAR_B;
		expect[1] = (uint8_t)CLEAR_G;
		expect[2] = (uint8_t)CLEAR_R;
		expect[3] = (uint8_t)CLEAR_A;

		for (px = 0; px < TARGET_WIDTH * TARGET_HEIGHT; px++) {
			uint8_t *p = target_map + (size_t)px * TARGET_BPP;
			int is_sentinel = (p[0] == SENTINEL_BYTE &&
			    p[1] == SENTINEL_BYTE && p[2] == SENTINEL_BYTE &&
			    p[3] == SENTINEL_BYTE);
			int matches = (p[0] == expect[0] && p[1] == expect[1] &&
			    p[2] == expect[2] && p[3] == expect[3]);

			if (is_sentinel)
				still_sentinel++;
			if (!matches) {
				mismatches++;
				if (printed < 8) {
					fprintf(stderr,
					    "  pixel %u: got %02x%02x%02x%02x want %02x%02x%02x%02x\n",
					    px, p[0], p[1], p[2], p[3],
					    expect[0], expect[1], expect[2], expect[3]);
					printed++;
				}
			}
		}

		printf("info: read-back: %u/%u pixels matched expected colour, "
		    "%u still show the sentinel unchanged\n",
		    (unsigned)(TARGET_WIDTH * TARGET_HEIGHT - mismatches),
		    (unsigned)(TARGET_WIDTH * TARGET_HEIGHT),
		    (unsigned)still_sentinel);

		if (mismatches == TARGET_WIDTH * TARGET_HEIGHT && still_sentinel == mismatches) {
			mark_fail("every pixel is still the sentinel -- the GPU never "
			    "wrote to the target at all (signal 2 of "
			    "docs/PLAN-mesa-lima.md section 1.4 failed)");
		} else if (mismatches > 0) {
			mark_fail("target does not hold the expected clear colour -- "
			    "frame-content bug (the kernel would not have caught this, "
			    "section 7); if bytes are a permutation of the expected "
			    "ones, re-check section 5.1's channel-order reasoning "
			    "before assuming the frame layout itself is wrong");
		} else {
			printf("ok:   render target matches the expected clear "
			    "colour exactly\n");
		}
	}

	printf("note: this program does not check `sysctl hw.lima_error` "
	    "(docs/PLAN-mesa-lima.md section 1.4, signal 3) -- that is FreeBSD-"
	    "specific and outside what a portable ioctl-only test can do; "
	    "check it by hand after running this.\n");

	munmap(scratch_map, SCRATCH_BO_SIZE);
	munmap(target_map, TARGET_BO_SIZE);

	{
		struct drm_lima_ctx_free free_ctx;

		memset(&free_ctx, 0, sizeof(free_ctx));
		free_ctx.id = ctx.id;
		if (xioctl(fd, DRM_IOCTL_LIMA_CTX_FREE, &free_ctx) == -1)
			mark_fail("CTX_FREE failed");
		else
			printf("ok:   CTX_FREE(%u)\n", ctx.id);
	}

	close(fd);

	if (g_fail_count > 0) {
		fprintf(stderr, "\n=== FAIL: %d check(s) failed ===\n", g_fail_count);
		return 1;
	}
	printf("\n=== PASS ===\n");
	return 0;
}

/*
 * Build note (this session, gcc on Linux, no FreeBSD headers available):
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -o /dev/null -c lima_pp_clear.c
 *
 * Nothing had to be worked around: this file intentionally uses only
 * <stdint.h>/<stddef.h>/<stdio.h>/<stdlib.h>/<string.h>/<errno.h>/<time.h>/
 * <fcntl.h>/<unistd.h>/<sys/ioctl.h>/<sys/mman.h>/<sys/types.h>, all of which
 * are standard on both Linux and FreeBSD with identical semantics for every
 * call used here (open/ioctl/mmap/munmap/close/clock_gettime). No FreeBSD-
 * specific header (e.g. <sys/sysctl.h>, needed only for the hw.lima_error
 * check this file deliberately omits, see the printed note above) is
 * referenced. The one thing that cannot be checked on this host is whether
 * /dev/dri/renderD128's ioctl numbers, as computed by the LIMA_IOC macros
 * above, actually match what drm-kmod's FreeBSD DRM core expects at the
 * syscall boundary -- that depends only on drm-kmod preserving Linux's ioctl
 * encoding for DRM_COMMAND_BASE ioctls, which is drm-kmod's whole design
 * point, but it is a fact about drm-kmod this session did not independently
 * re-verify beyond reading hal/lima/drm/lima_drm.h's own DRM_IOWR/DRM_IOW/
 * DRM_IOR usage, which encodes to the same values.
 */
