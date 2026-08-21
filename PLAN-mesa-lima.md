# PLAN-mesa-lima.md — from "attached" to "rendered"

> **SUPERSEDED 2026-08-20 — read this first.** This document is the plan from
> before any of it worked, kept for its reasoning, and its headline statements are
> now FALSE. Specifically: "Nothing has rendered. No GP or PP job has ever been
> submitted; no Mesa/lima userland exists anywhere in this project" (§ near the
> top) and "Mesa is not a realistic near-term milestone [**WRONG, and worth recording as such: Mesa was cross-built and rendering eight days later. The estimate was the most expensive error in this document.**]" (§ near the end) were
> both overtaken. The Mali-400 renders — `tests/limabench.c` passes 4/4 with a
> sampled texture, 2420 draw calls, depth testing and alpha blending, zero GPU
> MMU faults — and Mesa 26.2's lima gallium driver runs on this port unmodified,
> giving EGL 1.5 and GLES 2.0. What is still open is in `LOOSE-ENDS.md`, not here.
> The value left in this file is the route it mapped and the signals it defined,
> several of which are exactly what ended up proving the thing worked.


**Written:** 2026-08-11. **Author context:** planning-only pass, no builds, no board
access. Every path cited below was actually read while writing this; anything not
backed by a read is labelled **ASSUMPTION**.

## 0. Where this actually sits today

Verified state, from `hal/lima/MALI-STATUS.md` and `hal/lima/README-arm64.md` (both
dated 2026-08-11):

- `lima.ko` attaches on real hardware: Banana Pi M64 (Allwinner A64, Mali-400 MP2
  r1p1), FreeBSD 15.1 guest, `/dev/dri/card0` + `/dev/dri/renderD128` exist,
  `hw.dri.0.name` = `lima` (`MALI-STATUS.md:1-21`).
- **SUPERSEDED (same day, later): attach no longer requires any tunable.** The
  guest kernel was rebuilt with `AW_CLK_HAS_GATE` on `ccu_a64.c`'s `pll_gpu_clk`
  and redeployed, and attach was re-verified with `hw.lima.force_pll_gpu` unset
  entirely. Every mention of that tunable below is historical; ignore the
  instructions to set it. See MALI-STATUS.md's "Fixed at the root, and verified".
  The original text follows.
- Attach at the time of writing **required** `kenv hw.lima.force_pll_gpu=1` — without it,
  `clk_prepare_enable()` on the GPU core clock silently succeeds while `PLL_GPU`
  stays unlocked, and the first MMIO read into the Mali block never returns
  (`MALI-STATUS.md:23-64`, `hal/lima/lima_ccu_debug.c:158-207`).
- ~~**Nothing has rendered.** No GP or PP job has ever been submitted; no
  Mesa/lima userland exists anywhere in this project.~~ **This plan's premise
  expired on 2026-08-19.** Both halves are now false: Mesa 26.2 is built with
  `-Dgallium-drivers=lima` and installed on the board, and the GPU renders
  textured, depth-tested, alpha-blended geometry at 2420 draw calls per frame.
  The document is kept because its *reasoning* about what would be needed turned
  out to be largely right and is the record of how the route was chosen — but do
  not read its status statements as current. `MALI-STATUS.md` is.
- Two specific, already-documented functional gaps in the kernel port
  (`hal/lima/drm/drm_gem_shmem_helper.c:37-54`, confirmed by reading the code at
  `drm_gem_shmem_helper.c:766-782` and `:606-648`):
  1. Imported PRIME/dma-buf objects cannot be `mmap()`ed — `drm_gem_shmem_mmap()`
     returns `-EOPNOTSUPP` whenever `obj->import_attach != NULL`, because
     drm-kmod's `dma-buf.c` implements `dma_buf_vmap()`/`dma_buf_vunmap()` but no
     `dma_buf_mmap()`.
  2. `drm_gem_shmem_purge()` cannot invalidate a *live* userspace mapping, because
     FreeBSD's device-mapping object (an `OBJT_MGTDEVICE` object under linuxkpi's
     cdev pager) is not reachable from the GEM object the way Linux's
     `drm_vma_node_unmap()` reaches it.

### Scope/naming note worth flagging, not fixing

This repo's own planning docs have not caught up to the above. `ROADMAP.md:142`
says *"НЕ в scope Chimp bring-up: ... Lima/Mali (Woodpecker stretch)"*, `ROADMAP.md:238`
repeats "Lima/GLES = Woodpecker stretch" for Chimp, and `PLAN-gpu-bringup.md:9-11`
says *"No GPU work is on the Chimp bring-up critical path"* / "Lima/Mali-400 (Phase 2
below) = Woodpecker stretch." Woodpecker is **oBzdOS, an OpenBSD-based target**
(`/opt/bzdos/bsdOS/CLAUDE.md`'s codename table; `docs/specs/SPEC_lima_freebsd.md:8`
even flags "TODO: for oBzdOS/OpenBSD a separate Lima/weston audit is needed"). The
Mali work that actually landed did so on **Chimp** (Banana Pi M64, FreeBSD), not
Woodpecker. `hal/lima/drm/lima_drm.h:3`'s own header comment ("Porcupine v0.3,
PinePhone Pro Mali-400") is a fossil of that same drift. None of this blocks the
plan below — the hardware and OS in front of us right now are unambiguous — but
whoever reads this next should know the top-level roadmap doesn't reflect where
this actually happened, and that nothing here has been checked against OpenBSD.

**What "done" means for this document:** the smallest change that makes something
verifiably different appear in GPU-written memory, checked headlessly, with every
build/verify step expressed as "prepare off-board, hand off one command to run on
the board."

---

## 1. The smallest thing that proves the submit path works

### 1.1 The real UAPI, read from the tree

The kernel-side UAPI header is `hal/lima/drm/lima_drm.h` (not `drm_lima.h` — the
file's own name and every `DRM_IOCTL_LIMA_*` macro use the `lima_drm` order,
matching upstream). It is a straight, apparently-unmodified copy of upstream's
UAPI (`lima_drm.h:1-2` credits Qiang Yu, 2017-2018). Every ioctl userspace has to
use is defined at `lima_drm.h:122-136`:

| # | ioctl | struct | Wired to (`hal/lima/lima_drv.c`) |
|---|---|---|---|
| 0x00 | `DRM_IOCTL_LIMA_GET_PARAM` | `drm_lima_get_param{param, pad, value}` | `lima_ioctl_get_param` (`lima_drv.c:87-125`) |
| 0x01 | `DRM_IOCTL_LIMA_GEM_CREATE` | `drm_lima_gem_create{size, flags, handle, pad}` | `lima_ioctl_gem_create` (`:127-148`) |
| 0x02 | `DRM_IOCTL_LIMA_GEM_INFO` | `drm_lima_gem_info{handle, va, offset}` | `lima_ioctl_gem_info` (`:150-164`) |
| 0x03 | `DRM_IOCTL_LIMA_GEM_SUBMIT` | `drm_lima_gem_submit{ctx, pipe, nr_bos, frame_size, bos, frame, flags, out_sync, in_sync[2]}` | `lima_ioctl_gem_submit` (`:166-244`) |
| 0x04 | `DRM_IOCTL_LIMA_GEM_WAIT` | `drm_lima_gem_wait{handle, op, timeout_ns}` | `lima_ioctl_gem_wait` (`:246-262`) |
| 0x05 | `DRM_IOCTL_LIMA_CTX_CREATE` | `drm_lima_ctx_create{id, _pad}` | `lima_ioctl_ctx_create` (`:264-280`) |
| 0x06 | `DRM_IOCTL_LIMA_CTX_FREE` | (same struct) | `lima_ioctl_ctx_free` (`:282-298`) |

All seven are registered `DRM_RENDER_ALLOW` in `lima_drm_driver_ioctls[]`
(`lima_drv.c:353-361`) against a `drm_driver` with `DRIVER_RENDER | DRIVER_GEM |
DRIVER_SYNCOBJ` (`:369-385`). `GET_PARAM` supports exactly four params
(`lima_drm.h:20-25`, `lima_drv.c:104-123`): `GPU_ID`, `NUM_PP`, `GP_VERSION`,
`PP_VERSION`. None of this is aspirational — it is the code that compiles into the
`lima.ko` that is attached on the board right now.

### 1.2 Tier 0 — prove the ioctl plumbing, touch zero GPU hardware state

Before submitting anything to GP/PP, there is a smaller, strictly-safer test that
isolates userspace/VM/ctx-lifecycle bugs from hardware/frame-content bugs:

```
fd = open("/dev/dri/renderD128", O_RDWR)
GET_PARAM(GPU_ID) -> expect DRM_LIMA_PARAM_GPU_ID_MALI400 (0x1)
GET_PARAM(NUM_PP) -> expect 2                (matches "pp0/pp1" in the attach banner)
GET_PARAM(GP_VERSION), GET_PARAM(PP_VERSION) -> expect the "major 1 minor 1" already in dmesg
CTX_CREATE -> ctx_id
GEM_CREATE(size=4096, flags=0) -> handle       (NOT LIMA_BO_FLAG_HEAP — lima_heap_alloc()
                                                 is stubbed to -ENOSYS, see lima_gem.c:28-37)
GEM_INFO(handle) -> {va, offset}
mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, offset) -> ptr
write a known pattern through ptr, read it back
munmap(ptr, 4096)
CTX_FREE(ctx_id)
close(fd)
```

This never programs a single GP/PP/MMU register — `lima_gem_create_handle()`
(`lima_gem.c:91-135`) only calls `drm_gem_shmem_create()` +
`drm_gem_shmem_get_pages_sgt()`, and the mmap path is the *non*-import branch of
`drm_gem_shmem_mmap()` (`drm_gem_shmem_helper.c:765-797`) — the one branch that is
**not** one of the two known-missing gaps. If this hangs or crashes, the bug is in
GEM/VM/ctx plumbing or the shmem helper's page-wiring path, not in a Mali command
stream. Pass/fail is binary and needs no pixels: every call returns the errno the
pseudocode above says it should, and the mmap round-trip byte-compares equal.

### 1.3-PREMISE — CHALLENGED by later research (2026-08-11, same day)

Read this before acting on §1.3 below. The follow-up research that derived the
actual frame encoding (`tests/PP-CLEAR-FRAME.md`) found the opcodes are NOT the
hard part — they are exact and cross-sourced, byte-identical between the Limare
project (2011) and current Mesa, twelve years apart. What it could not find is a
precedent for §1.3's central shortcut:

**No open-source driver anywhere skips the GP job — not even for a zero-draw
clear.** The per-tile polygon-list scratch memory that `plbu_array_address` points
into is, in every real driver including current Mesa's `lima_do_job()`, a *GP
hardware output* and never a software input. So "PP job with no preceding GP job
at all" is not a lightly-travelled path; it is an untravelled one, and it is
structurally unverifiable from existing sources rather than merely undocumented.

That does not make §1.3 wrong — the original Lima bring-up really did work by
observing write-back DMA — but the risk is concentrated somewhere the plan did not
put it. Two consequences:

- The one genuine guess in the frame is that scratch region (left zero-filled).
  `tests/lima_pp_clear.c` names exactly that in its refusal message.
- If M1 fails, "the frame is wrong" and "PP-without-GP is not a thing on this
  hardware" are both live explanations, and the second cannot be excluded by more
  careful encoding. Deciding between them probably means doing the GP job after
  all, which reintroduces the shader-compiler dependency §1.3 exists to avoid.

And the safety framing is now measured, not assumed: `lima_ioctl_gem_submit`
checks only ioctl-level sizes and ranges, and `lima_pp_task_validate`
(`lima_pp.c`) checks only `num_pp` (plus `_pad` on m450). There is **zero**
content validation of `frame[]`, `wb[]` or any address in them — verified by
reading both. A malformed frame reaches hardware unexamined.

### 1.3 Tier 1 — one real job, and why PP-only, not GP

A GP (Geometry Processor) job's frame is six words (`LIMA_GP_FRAME_REG_NUM 6` at
`lima_drm.h:56`) that are just address ranges — confirmed against
`hal/lima/lima_regs.h:115-120`: `VSCL_START/END_ADDR`, `PLBUCL_START/END_ADDR`,
`PLBU_ALLOC_START/END_ADDR`. That means a GP job requires a **real, correctly
encoded vertex-shader command list and PLBU command list already sitting in GPU
memory** — i.e. it needs a working shader compiler (upstream Mesa's `gpir`) or
hand-derived Utgard machine code. That is not a small test; it is most of what a
real driver does.

A PP (Pixel Processor) job is smaller in the one way that matters: it is possible
to run the PP with **zero geometry** — a PLBU tile-command list that says "clear
to color X, no primitives" — which sidesteps the shader compiler entirely. This is
exactly the same shortcut the original (pre-Mesa) Lima bring-up work took: draw
nothing, just observe the write-back DMA happen (the "Limare" project's own
documented methodology was capture-and-replay of single-frame command streams for
this reason — general knowledge, not verified against Lima/Limare source in this
sandbox, see §6 caveats). Concretely, for Mali-400 the frame struct is
`drm_lima_m400_pp_frame` (`lima_drm.h:65-71`): 23 frame registers + `num_pp` + 36
write-back registers (`wb[3*12]`) + 4 `plbu_array_address` + 4
`fragment_stack_address`. The kernel does **no content validation** of any of
this — `lima_pp_task_validate()` (`lima_pp.c:566-598`) only checks `num_pp` is
in-range and padding is zero. Whatever address is in `plbu_array_address[i]` gets
written straight into the `LIMA_PP_FRAME` register and the hardware runs it
(`lima_pp.c:648-665`, `lima_regs.h:241` for the register offset). Getting the
actual bytes of that tile-command list right is real Utgard-format work — I have
not derived or verified exact opcode values here (**ASSUMPTION gap**, flagged
explicitly rather than invented) — but the *shape* of the minimal test is settled:

```
GEM_CREATE a small RGBA8 render target BO (e.g. 64x64x4 = 16384 bytes)
GEM_CREATE a small BO holding a hand-written "clear-only" PLBU tile list
Fill drm_lima_m400_pp_frame:
  num_pp = 1
  plbu_array_address[0] = the tile-list BO's GPU VA (from GEM_INFO)
  wb[0..11] = write-back unit 0 programmed to target the render-target BO,
              correct pitch/format, clear color baked into the tile list
GEM_SUBMIT(pipe=LIMA_PIPE_PP, ctx, nr_bos=2, frame=&frame, out_sync=0, in_sync={0,0})
GEM_WAIT(render_target_handle, op=LIMA_GEM_WAIT_READ, timeout_ns=~1e9)
mmap the render-target BO, compare every pixel to the expected clear color
```

`out_sync`/`in_sync` can stay 0 — `lima_gem_submit()` attaches the job's fence to
every submitted BO's `dma_resv` unconditionally (`lima_gem.c:356-361`), so
`LIMA_GEM_WAIT` on the render-target handle alone is sufficient; no
`DRM_IOCTL_SYNCOBJ_*` scaffolding is required for the minimal test (it is
available — `DRIVER_SYNCOBJ` is set and drm-kmod's `drm_syncobj.c` is one of the
sources its own `drm/Makefile` compiles, per the symbol audit in
`README-arm64.md:110-115` — but it is not the smallest path).

### 1.4 What "observes completion" means, concretely

Three independent signals, because any one alone is checking a different, weaker
thing:

1. **`LIMA_GEM_WAIT` returns 0, not `-ETIMEDOUT`/`-EBUSY`.** `lima_gem_wait()`
   (`lima_gem.c:395-410`) turns a `drm_gem_dma_resv_wait()` timeout into exactly
   those two errnos. A 0 here proves the whole IRQ chain fired at least once: GIC
   SPI → the platform-device interrupt bridge (`hal/lima/linux/interrupt.h`,
   never yet exercised for a real device IRQ — see §5) → `lima_pp_irq_handler` →
   `lima_sched`'s fence signal.
2. **The render target's bytes actually match the clear color.** (1) alone is not
   proof the GPU did real work: `lima_sched_timeout_ms`/`lima_job_hang_limit`
   (module params in `lima_drv.c:67-83`) mean a hung job can also end up
   fence-signalled (with an error) by the scheduler's own recovery path rather
   than by real hardware completion — reading the actual pixels is what tells
   these two apart.
3. **`sysctl hw.lima_error` reports zero saved tasks after the run.** This sysctl
   is real and already wired (`lima_drv.c:433-490`, registered at `:562`); a clean
   run should show `dump.num_tasks == 0`. A non-zero count means the scheduler's
   error/recovery path fired even if (1) and (2) happened to look fine.

All three are cheap to check from the same tiny test binary; none needs a display.

---

## 2. Mesa's lima driver on FreeBSD

### 2.1 Mesa-on-FreeBSD in general: proven. Lima specifically: never attempted.

FreeBSD's own `graphics/mesa-dri` port Makefile (fetched from
`cgit.freebsd.org/ports/plain/graphics/mesa-dri/Makefile`) sets
`GALLIUM_DRIVERS= virgl zink` by default and offers
`OPTIONS_GROUP_GALLIUM= crocus i915 iris llvmpipe panfrost r300 r600 radeonsi svga`
as build options, with panfrost gated to aarch64-only via
`OPTIONS_EXCLUDE+= ${ARCH:Naarch64:C/.+/panfrost/}`. **`lima` appears nowhere in
that file.** So: Mesa's core, Gallium infrastructure, and libdrm already build and
ship on FreeBSD (proven daily by that same port for radeonsi/panfrost/svga/etc.),
but nobody has packaged, built, or tested the `lima` Gallium driver against
FreeBSD. Attempting it would be a first, on top of a kernel driver that has also
never executed a real job. That combination of two firsts is the reason §2.4's
verdict is what it is.

I could not fetch Mesa's actual `src/gallium/drivers/lima/{lima_screen,lima_bo}.c`
in this sandbox to verify their exact ioctl call sites — `gitlab.freedesktop.org`
returned an anti-bot (Anubis) challenge to every fetch attempt, including through
a GitHub-mirror guess and a `cgit.freedesktop.org` mirror, both of which either
404'd or were also blocked. Everything below about what Mesa's driver *does* is
therefore reasoned from (a) this repo's own UAPI header being what a Mesa driver
would call against, and (b) the architecture Mesa's other DRM Gallium drivers
(panfrost, v3d, etnaviv) are known to share — **flagged here as an assumption, not
a read fact**, per the instruction to mark what I could not verify.

### 2.2 What Mesa needs from the kernel, mapped against what exists

| Mesa need (assumed, §2.1 caveat) | ioctl/param | Present in this repo? |
|---|---|---|
| Identify GPU at screen-create | `GET_PARAM` x4 | Yes — `lima_drv.c:87-125` |
| Per-context GPU state | `CTX_CREATE`/`CTX_FREE` | Yes — `:264-298` |
| Allocate buffers | `GEM_CREATE` | Yes — `:127-148` |
| CPU-map a buffer it allocated itself | `GEM_INFO` + `mmap()` | Yes, for **native** BOs — `drm_gem_shmem_helper.c:765-797` (non-import branch) |
| CPU-map a buffer imported via dma-buf/PRIME | `GEM_INFO` + `mmap()` on an imported handle | **No** — `drm_gem_shmem_helper.c:772-782`, `-EOPNOTSUPP` |
| Submit a batch | `GEM_SUBMIT` | Yes — `:166-244` |
| Sync / glFinish / fence wait | `GEM_WAIT` (or syncobj) | Yes — `:246-262`, and `drm_syncobj.c` is compiled into drm-kmod per `README-arm64.md:110-115` |
| Reclaim idle buffers under memory pressure | `drm_gem_shmem_purge()` from a shrinker | Present but cannot revoke a live userspace mapping — `drm_gem_shmem_helper.c:606-648` |

### 2.3 Which gap blocks which Mesa path, specifically

- **The mmap-on-import gap blocks:** `EGL_EXT_image_dma_buf_import` /
  `EGL_MESA_image_dma_buf_export`, GBM-based window-system integration, and any
  future zero-copy hand-off to a display driver (the planned but unwritten
  `sun4i-drm`, per `DESIGN-gpu-display-a64.md:41-47`) or a Wayland `wl_drm`-style
  compositor path. All of these are display/WSI concerns.
- **The mmap-on-import gap does NOT block:** a self-contained
  `EGL_PLATFORM_SURFACELESS_MESA` client that only ever touches BOs Lima allocated
  itself (renderbuffers, textures uploaded from host memory, `glReadPixels`
  targets) — none of those are import paths. This is exactly why headless-only is
  the right target for the first Mesa milestone, if one is ever attempted (§4,
  M4).
- **The purge gap blocks:** correctness under real memory pressure for any
  long-running Mesa client (a compositor, a game). It does not affect a short,
  single-shot smoke-test process that allocates a handful of small BOs and exits.

### 2.4 Verdict

Mesa is not a realistic near-term milestone. Concretely, attempting it today means
stacking: (1) a kernel driver that has never submitted a real job, on top of (2) a
platform/IRQ bridge that has never delivered a real device interrupt, on top of
(3) a Mesa+FreeBSD+lima combination nobody has ever built, with (4) zero
FreeBSD-specific prior art to compare wrong results against. If any of the four
layers misbehaves, there is no way to tell which one from a Mesa-level symptom
(a black screen, a crash inside `libGL`, a hang) — the debugging surface is far
too wide. The bare-ioctl tiers in §1 exist precisely to burn down (1) and (2)
first, cheaply and diagnosably, before (3)+(4) are even attempted. Recommendation:
defer Mesa entirely until Tier 1 (§1.3) is hardware-proven and repeatable (M2 in
§4); treat a first Mesa attempt (M4) as a stretch goal gated on that, not as this
milestone.

---

## 3. Build and delivery route

Three different things need building, and they do not have the same answer.

### 3.1 The kernel module (`lima.ko`) — keep the existing cross-build route

`infra/scripts/build-lima-arm64.sh` already does this: enter FreeBSD's own `make
buildenv` for `TARGET=arm64 TARGET_ARCH=aarch64` and run `bmake` over
`hal/lima/Makefile` there, entirely on the Linux host, no VM, no board
(`build-lima-arm64.sh:1-36`). Paired with `infra/scripts/build-drm-kmod-arm64.sh`
for `drm.ko`/`dmabuf.ko`. This is not a proposal — it is the pipeline that
produced the exact `lima.ko`/`drm.ko` running on the board today (per
`MALI-STATUS.md`'s attach banner). There is no reason to change it for kernel-side
work; every future kernel-side commit (a fix to a frame handler, a new debug
counter, eventually the real `AW_CLK_HAS_GATE` fix) goes through this same route.

### 3.2 The tiny Tier 0/Tier 1 userspace test program — build it natively on the guest

This is the one piece of new work where the route is *not* "the same as
`hal/lima`," and it is worth being explicit about why:

- `build-lima-arm64.sh` only ever bootstraps a **kernel** cross-toolchain — its
  own prerequisite check looks for
  `$MAKEOBJDIRPREFIX/$FREEBSD_SRC/arm64.aarch64/tmp/usr/bin/cc`
  (`build-lima-arm64.sh:75-80`), populated by FreeBSD's `kernel-toolchain` build
  target. That target does not stage a target **sysroot** (libc headers/libs) for
  linking a hosted userspace binary — only `buildworld` (or at least a much larger
  slice of it) does that, and nothing in this repo's infra has ever run that far
  for aarch64. Whether the existing cross-compiler could even find `<fcntl.h>`
  or link against a target `libc.so` today is genuinely unknown — I did not test
  it, and standing up a proper userspace cross-sysroot if it is missing would be
  new, unproven infra.
- The guest already has a working, matching-in-every-way native toolchain:
  `/opt/bzdos/microkernel/CLAUDE.md:87-88` states plainly that "the guest is also
  a build host: it has `/usr/bin/cc` and ~4.7 GB free, so a misbehaving base tool
  is usually faster to rebuild there than to debug" — the same reasoning applies
  to a ~150-400 line test program even more strongly, since there is no
  cross-sysroot question at all.
- The file has to get onto the guest either way, and the documented pattern
  (`microkernel/CLAUDE.md:89-91`) is the guest listens, the host connects (`nc -l`
  on the guest side) — inbound to the host is firewalled the other direction.

**Recommendation: write and compile the Tier 0/Tier 1 test source on the Linux
host (so it lands in this git repo, e.g. under `hal/lima/tests/` alongside the
existing host-runnable `test_lima_math.c`/`test_shmem_logic.c`), push the `.c`
file itself to the guest over the listen/connect pattern, and run `cc` natively
on the guest.** Rough cost: the file is small, so guest-side compile time is a
non-issue even on the single vCPU this guest gets (§5.7); the only real cost is
the push step, which is already a solved, documented pattern. This is a normal
source-iteration loop, not a live-disk hotpatch, so it does not run into
`WOW_FEATURES.md`'s rule against patching around systemic bugs on a live image —
the source of truth stays in the repo either way.

### 3.3 A future full Mesa build — undecided, and should stay that way until M2 passes

If/when Mesa is actually attempted (M4, §4), there are two routes and neither is
proven:

- **(a) Cross-build on the Linux host**, matching `hal/lima`'s own pattern. This
  needs a real aarch64-FreeBSD userspace sysroot (a `buildworld`-derived tree, or
  equivalent) plus cross-built dependencies (libdrm, expat, zlib, LLVM if any
  Gallium path pulls it in) and a Meson cross-file pointing at all of it. None of
  this exists in this repo today; building it is itself a multi-day undertaking
  with its own unknowns, separate from Mesa's own build.
- **(b) Build natively on the guest**, the same reasoning as §3.2 but at a much
  larger scale: Mesa is hundreds of thousands of lines, Meson+Ninja, and the guest
  has one vCPU (§5.7) and ~4.7 GB free. Whether the guest can even reach a FreeBSD
  package mirror to `pkg install meson ninja python3 libdrm-devel` is genuinely
  unverified in this sandbox (LAN connectivity is proven — SSH to the guest works
  per project memory — outbound-to-internet is not something I checked or should
  check by touching the board).

Given neither is proven and Mesa is explicitly not this milestone's target (§2.4),
defer the decision. When it is time to make it, the cheap first step is proving
out **one** trivial cross-compiled aarch64-FreeBSD *userspace* "hello world" from
the Linux host — that single experiment answers whether route (a)'s sysroot
question is a five-minute fix or a real project, before committing.

---

## 4. Ordered milestones

Every milestone below is "prepare and verify off-board, then hand off one
concrete action to run on hardware." None of them touch the board from here.

| # | Milestone | Built/verified off-board as | Hand-off | Pass | Fail (and what it tells you) |
|---|---|---|---|---|---|
| **M0** | Tier 0 ioctl smoke test (§1.2) | `.c` source in this repo, syntax/logic checked by reading, no cross-compile attempted (§3.2) | push source, `cc -Wall -Wextra -O0 -o lima_ioctl_smoke lima_ioctl_smoke.c && ./lima_ioctl_smoke` on the guest (no tunable needed since the kernel fix) | All 4 `GET_PARAM`s match the known-good banner; `GEM_CREATE`/`GEM_INFO`/mmap/write/read-back/`munmap`/`CTX_CREATE`/`CTX_FREE` all return the expected value; process exits 0 | Any ioctl returns an unexpected errno, or the mmap SIGBUSes → bug is in GEM/VM/ctx plumbing or the shmem helper, *before* any hardware job is involved |
| **M1** | PP-only clear job (§1.3) | `.c` source in this repo; PLBU tile-list content is the one piece with a documented open gap (§1.3, "I have not derived exact opcode values") — resolving that is part of doing M1, not before it | push source, run on guest (no tunable needed since the kernel fix), then check `sysctl hw.lima_error` | All three signals in §1.4 hold: `GEM_WAIT` returns 0, render-target bytes match the clear color, `hw.lima_error` shows 0 tasks | Timeout → suspect the IRQ bridge (§5.5) first, since it has never fired for a real device IRQ. Wrong pixels with no timeout → frame-content bug, kernel won't catch it (`lima_pp_task_validate` only checks `num_pp`, `lima_pp.c:566-598`). Non-zero `hw.lima_error` → scheduler recovery fired even if the other two looked fine |
| **M2** | Repeat M1 across several reloads and a few clear colors/BO sizes | same binary, parameterized | run N times across fresh boots | Deterministic — same pass every time | Flaky/one-shot-only failures are exactly the class of bug this project has hit before (the `drm_dev_alias` panic-on-second-probe in `MALI-STATUS.md:87-108`) — a repeat pass is what tells "attach works" apart from "attach worked once" |
| **M3** | One real GP job (vertex + PLBU command list) fed into an M1-style PP read-back | `.c` source, explicitly the hardest of the four — needs real Utgard vertex/PLBU encoding, not just a clear list | run on guest | Same three-signal check as M1, now covering the GP IRQ path (`LIMA_GP_IRQ_VS_END_CMD_LST`/`PLBU_END_CMD_LST`, `lima_regs.h:133-134`) too | This is plausibly where "stop and reassess" happens — if M3 turns out to need more Utgard-format reverse-engineering than expected, that is itself a valid, honest finding to report rather than push through |
| **M4** (stretch, not committed) | First Mesa+lima build attempt, gated on M1–M3 all passing | route decided per §3.3 | run a trivial `EGL_PLATFORM_SURFACELESS_MESA` clear+`glReadPixels` client | Links and runs without crashing (correct pixels is a bonus, not the bar) | Any crash inside Mesa itself, with M1–M3 already green, isolates the bug to the Mesa/FreeBSD/lima combination specifically (§2.4) |

M0 and M1 are the actual recommendation for "the next Mali milestone" the parent
task asked for. M2 is what turns M1 from "it worked once" into "it works." M3 and
M4 are named so the sequence is visible, not because they are committed work.

---

## 5. Risks specific to this platform

Each one: mechanism, whether it plausibly affects rendering, how to find out —
without touching the board myself.

### 5.1 `kenv hw.lima.force_pll_gpu=1` is not persistent by design

**Mechanism:** off by default "precisely so it cannot quietly become the fix"
(`lima_ccu_debug.c:120-157`). **Affects rendering:** certainly — without it,
attach itself fails (`MALI-STATUS.md:23-46`), so every milestone in §4 silently
fails at step 0 if this is forgotten. **How to find out / mitigate:** it isn't a
thing to discover, it's a thing to not forget — every hand-off instruction in §4
must restate it. The real fix (one flag in `sys/dev/clk/allwinner/ccu_a64.c`,
`MALI-STATUS.md:56-58`) is a kernel rebuild, explicitly out of scope here (hard
constraint: no kernel/world builds).

### 5.2 `HCR_EL2.IMO=0` / the hypervisor's relationship to the GIC

**Mechanism, read from the microkernel tree, not assumed:** the *current* board
build runs with `HCR_EL2.IMO=0` — physical IRQs routed **directly to the guest**,
not to EL2 (`/opt/bzdos/microkernel/PROGRESS.md:100`: "routing all physical
interrupts directly to the guest... lets the guest handle device interrupts... 
natively"). `microkernel/README.md`'s "known gaps" section confirms this is
paired with the GIC being otherwise unmediated: *"`HCR_EL2.IMO=0` gives the guest
unmediated access to the real GICv2"* — stage-2 identity-maps the whole low-1GiB
MMIO block including the real GIC distributor and CPU interface, with the *only*
carve-out being the one 4 KiB UART0 page (`stage2.h:63-85`, `stage2.c:278-314`).
The GICC→GICV virtualization redirect that would otherwise complicate this is
explicitly documented as `IMO=1`-only and **removed** under the current `IMO=0`
policy (`stage2.c:292-309`; project memory: "GICV redirect coupled to IMO... 
HARMFUL under IMO=0"). The Mali node's interrupts are ordinary SPIs, confirmed by
reading the deployed DTB directly
(`fdtget -t s /opt/bzdos/tftpboot/bananapi-min.dtb /soc/gpu@1c40000 status` → `okay`;
`bananapi-min.dts:1244-1255`: `interrupts = <0 0x61 4 0 0x62 4 0 0x63 4 0 0x64 4 0
0x66 4 0 0x67 4 0 0x65 4>` for gp/gpmmu/pp0/ppmmu0/pp1/ppmmu1/pmu — SPI numbers 97,
98, 99, 100, 102, 103, 101). **Affects rendering: plausibly low risk**, precisely
*because* `IMO=0` means there is no hypervisor mediation standing between the GPU's
SPI and FreeBSD's own GIC driver — this is closer to bare metal than a
type-1-hypervisor IRQ path would be. The real open question is downstream of the
GIC entirely: whether `hal/lima/linux/interrupt.h`'s `bus_setup_intr()` bridge
(README-arm64.md's blocker 2, item 3, `README-arm64.md:198-209`) — which has never
carried a real device interrupt, only been read about — actually works. **How to
find out:** M1 (§4) *is* the test; if `GEM_WAIT` times out despite correct pixel
data eventually appearing (or appearing after a manual re-read), suspect the IRQ
bridge specifically, not `IMO`.

### 5.3 Mali's 32-bit DMA address mask vs. FreeBSD's unconstrained `OBJT_SWAP` allocator

**Mechanism:** `lima_device_init()` calls `dma_set_coherent_mask(ldev->dev,
DMA_BIT_MASK(32))` because "the Mali MMU constrains addresses to 32-bit physical"
(`lima_device.c:750-763`), and `lima_gem.c:107-112` repeats the same fact for GEM
BOs. But FreeBSD's shmem helper has no equivalent of Linux's
`mapping_set_gfp_mask(__GFP_DMA32)` — "a FreeBSD `OBJT_SWAP` object has no
equivalent knob, so pages may live above 4 GiB" (`drm_gem_shmem_helper.c:43-47`).
**Affects rendering: low risk on this exact configuration.** The deployed DTB's
own `/memory` node is `0x40000000` base, `0x40000000` size — **1 GiB** total guest
RAM, confirmed by reading it directly
(`fdtget -t x /opt/bzdos/tftpboot/bananapi-min.dtb /memory reg` → `40000000
40000000`), matching `stage2.h:88`'s `STAGE2_DRAM_SIZE 0x40000000UL`. Every page a
1 GiB guest can hand out is already under the 4 GiB line; the missing enforcement
is currently unobservable, not merely unlikely. **How to find out (cheap, worth
doing once M1 exists, not before):** log each BO's `dma_addr_t` in
`lima_vm_map_page()` (`lima_vm.c:55-90`) and assert `< 0x100000000`; this becomes
a real bug only if guest RAM is ever grown, which is a decision this plan does not
make.

### 5.4 PP frame content is unvalidated, and hardware executes it as given

**Mechanism:** `lima_pp_task_validate()` checks only `num_pp` range and padding
(`lima_pp.c:566-598`); whatever is in `plbu_array_address[]`/`wb[]` is written
straight to hardware (`lima_pp.c:648-665`). **Affects rendering:** a wrong tile
list is the single most likely cause of M1 not working the first time. **Blast
radius, reasoned from what exists (not verified against a real hang):** this is a
qualitatively different risk than the EHCI-storm class of bug that has taken the
whole board down before (`MALI-STATUS.md:147-188`) — that was an unanswered MMIO
*read* stalling the interconnect. Mali-400 exposes explicit hang-detection IRQs
(`LIMA_GP_IRQ_HANG`/`LIMA_GP_IRQ_FORCE_HANG` at `lima_regs.h:138-139`, folded into
the whole `LIMA_GP_IRQ_MASK_ERROR` set at `lima_regs.h:210-221`), and the driver already
wires scheduler-level timeout/recovery (`lima_sched_timeout_ms`,
`lima_job_hang_limit` module params, `lima_drv.c:67-83`; error-task capture via
`hw.lima_error`, `lima_drv.c:433-490`). A malformed frame should therefore surface
as a *reported, bounded* failure (job timeout + a saved error task) rather than a
board-wide hang — but this recovery path has itself never fired against a real
hang, so this is a reasoned expectation, not a guarantee. **How to find out:** M1
finds out. If a run ever needs board recovery, that in itself is the signal that
this risk was underestimated, and the existing watchdog-reset recovery (proven
elsewhere in this project) is the backstop — for someone else to invoke on
hardware, not something this document arranges.

### 5.5 The platform-device interrupt bridge has never carried a real device IRQ

**Mechanism:** `hal/lima/linux/interrupt.h` shadows `devm_request_irq()` to go
through `bus_setup_intr()` on the underlying `device_t` instead of linuxkpi's
PCI-only IRQ lookup (`interrupt.h:1-37`, `:115-128`). This is real, compiled,
attached code — but per `README-arm64.md:130-141`, everything demonstrated on
hardware so far is MMIO reads/writes during clock/PMU bring-up; no GP/PP/MMU IRQ
has ever actually fired and been handled through this path. **Affects
rendering:** directly — this is the mechanism M1's completion signal depends on
(§1.4, signal 1). **How to find out:** identical to §5.2's answer — M1 is the
first real exercise of this path; a `GEM_WAIT` timeout with otherwise-correct
pixel data (visible only via a follow-up manual read, not the automated wait)
would specifically implicate this bridge over the GIC/`IMO` question in §5.2.

### 5.6 The two documented gaps, restated as risks against this specific plan

**Mmap-on-import (`drm_gem_shmem_helper.c:772-782`):** does not affect M0–M3 (none
of them import a dma-buf); would block any dma-buf-import step of M4 if one were
added — none is, by design (§2.3). **Purge-under-pressure
(`drm_gem_shmem_helper.c:606-648`):** irrelevant to every milestone here (all are
short, single-shot processes with no memory pressure induced); would need a real
fix before any long-running Mesa client milestone past M4.

### 5.7 Single vCPU guest

**Mechanism:** `microkernel/CLAUDE.md`'s core layout: "CPU0 — runs the guest,"
with CPU1-3 dedicated to the debug core / eMMC offload / idle
(`microkernel/CLAUDE.md:65-74`) — the FreeBSD guest gets one physical core.
**Affects rendering:** low direct risk for M0-M3 (short, single-purpose test
binaries with nothing else contending for CPU0), but it is the main cost driver in
§3.2/§3.3's build-route reasoning, and worth remembering if M2's "repeat across N
reloads" ever flakes only under load — that would point at scheduling/latency
contention for the completion IRQ, not at the frame content or IRQ bridge.

---

## 6. What I could not verify (consolidated)

- Mesa's actual `lima_screen.c`/`lima_bo.c` source — `gitlab.freedesktop.org`
  blocked every fetch attempt (Anubis anti-bot challenge) in this sandbox,
  including a GitHub-mirror guess (404) and a `cgit.freedesktop.org` mirror
  (403/connection reset). §2's description of what Mesa calls is reasoned from
  this repo's UAPI header plus the shared architecture of other DRM Gallium
  drivers, not read directly.
- The exact Mali-400 Utgard PLBU/tile-list byte encoding needed for M1's
  "clear-only" job (§1.3) — I found the *struct layout* it goes into
  (`lima_drm.h:65-71`) and confirmed the kernel does not validate its contents,
  but did not derive or verify actual opcode bytes.
- Whether the guest's FreeBSD has outbound internet/pkg-mirror reachability
  (relevant to §3.3's route (b)) — LAN reachability to the guest is
  well-established in project memory; outbound-from-guest was not something I
  checked, since checking it would mean touching the board.
- Whether the bootstrapped kernel cross-toolchain
  (`arm64.aarch64/tmp/usr/bin/cc`) can, as-is, link a hosted userspace binary at
  all (§3.2) — I read the build script's own prerequisite checks but did not
  attempt a compile.
- Runtime behavior of literally everything in §1.3/§1.4/§4 beyond M0 — by
  construction, since no builds or hardware access were in scope for this task.

---

## References (all read while writing this document)

`hal/lima/MALI-STATUS.md`, `hal/lima/README-arm64.md`, `hal/lima/drm/lima_drm.h`,
`hal/lima/lima_drv.c`, `hal/lima/lima_gem.c`, `hal/lima/lima_vm.c`,
`hal/lima/lima_pp.c`, `hal/lima/lima_gp.c` (frame-register cross-check only),
`hal/lima/lima_device.c`, `hal/lima/lima_regs.h`, `hal/lima/lima_ccu_debug.c`,
`hal/lima/linux/interrupt.h`, `hal/lima/drm/drm_gem_shmem_helper.c`,
`hal/lima/Makefile`, `hal/lima/tests/test_lima_math.c`,
`docs/specs/SPEC_lima_freebsd.md`,
`infra/scripts/build-lima-arm64.sh`, `infra/scripts/build-drm-kmod-arm64.sh`,
`DESIGN-gpu-display-a64.md`, `PLAN-gpu-bringup.md`, `ROADMAP.md`, `CLAUDE.md`
(bsdOS root); `/opt/bzdos/microkernel/CLAUDE.md`, `PROGRESS.md`, `README.md`,
`WOW_FEATURES.md`, `stage2.h`, `stage2.c`; the deployed
`/opt/bzdos/tftpboot/bananapi-min.dtb` and its source `build/bananapi-min.dts`
(read directly with `fdtget`/`dtc`, not assumed); FreeBSD's
`graphics/mesa-dri/Makefile` (`cgit.freebsd.org`).
