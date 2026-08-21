# Mesa's lima Gallium driver on FreeBSD/aarch64 — feasibility

> **SUPERSEDED IN PART 2026-08-20 — read this first.** The analysis here is
> sound and worth reading, but its central open questions have since been
> ANSWERED on hardware, so do not take its cautions as current status. In
> particular: the interrupt-driven fence-signal path it flags as "has never
> fired on this hardware" now does fire, jobs complete, and
> `drmSyncobjWait()`'s path is exercised on every frame — `tests/limabench.c`
> passes 4/4 (sampled texture, 2420 draw calls, depth, blending, zero MMU
> faults) and Mesa 26.2's lima driver runs unmodified on top. Treat this file as
> the map that got there. Current open items: `LOOSE-ENDS.md`.


**Written:** 2026-08-11. Research-only pass: no board access, no kernel/world build,
no run of `build-lima-arm64.sh`/`build-drm-kmod-arm64.sh`. Everything below was
either read directly from a pinned Mesa/libdrm/drm-kmod source snapshot, read
directly from this repo's own kernel-side files, or produced by an actual
(bounded, documented) `meson setup` run. Every claim below is one of those three;
where I reasoned instead of reading, it says so.

## Verdict, in one sentence

Nothing in Mesa's build system, in the kernel↔userspace UAPI contract, or in
FreeBSD's own Mesa packaging practice blocks building the `lima` Gallium driver
for FreeBSD/aarch64 — every specific, checkable thing checks out — but **nobody
has ever done it**, so this is "should build, and I have unusually strong
evidence for that," not "builds, verified"; the real remaining unknowns are
runtime ones (an interrupt-driven fence-signal path that has never fired on this
hardware) that only an actual build-and-run can resolve.

---

## What I actually executed

- **Source acquisition.** `gitlab.freedesktop.org` and `cgit.freedesktop.org` were
  confirmed blocked/dead from this host (Anubis redirect and a TLS reset,
  respectively — consistent with the brief). A full tarball of the mirror
  `github.com/chaotic-cx/mesa-mirror` (a live, 5-minute-updated read-only mirror
  of `gitlab.freedesktop.org/mesa/mesa`, confirmed via its own repo description)
  measured **~11.8 KB/s** through this host's proxy (2.1 MB in 180 s before I
  gave up on that approach) — a full checkout (hundreds of MB) would have taken
  hours. Instead I resolved tag `mesa-26.2.0` to commit
  `9f0a761020bca92f2b07156a0621e5360cb8eca5` via the GitHub API and fetched only
  the specific files needed, individually, over `raw.githubusercontent.com` —
  each small file arrived in 1–3 s, so dozens of parallel fetches were cheap.
  This is the same technique `infra/scripts/build-drm-kmod-arm64.sh`'s own header
  comment documents for drm-kmod, applied to Mesa. `libdrm-2.4.134.tar.xz` (the
  exact version FreeBSD ports ships) was fetched directly from
  `dri.freedesktop.org` (reachable, unlike the other freedesktop hosts) and its
  SHA-256 matched the ports `distinfo` exactly.
- **Static reads**, file:line-cited throughout: Mesa's root `meson.build` (2673
  lines, fetched in full) and `meson.options` (the project renamed
  `meson_options.txt`), `src/gallium/drivers/lima/{meson.build,*.c,*.h}` in full,
  `src/util/{libsync.h,os_misc.c}`, `include/drm-uapi/lima_drm.h`,
  `src/panfrost/meson.build`, libdrm's `xf86drm.c` (extracted from the real
  tarball), and this repo's own `hal/lima/{lima_drv.c,drm/lima_drm.h,drm/drm_gem_shmem_helper.c}`.
  Also fetched fresh (not reused from memory/prior docs): FreeBSD's
  `graphics/libdrm/Makefile`, `graphics/mesa-dri/Makefile`,
  `graphics/mesa-libs/Makefile`, and their `files/patch-*` series, from
  `cgit.freebsd.org` (reachable).
- **A real, bounded `meson setup` run.** No `meson` was installed on this host;
  I installed it (version 1.12.0) into a throwaway venv under the scratch
  directory via `pip`. There is no FreeBSD/aarch64 **userspace** cross-toolchain
  anywhere on this host (see §4) — only a bare kernel cross-compiler exists,
  bootstrapped for `hal/lima`'s own kernel-module build, which has no target
  libc/headers and cannot link a hosted binary. So I used a documented,
  explicitly-labeled synthetic cross-file (`[binaries] c = 'cc'` — this host's
  real, working Linux gcc — with `[host_machine] system = 'freebsd', cpu_family
  = 'aarch64'`). This does **not** test "does the C source compile against real
  FreeBSD headers" (it cannot; no such headers exist here) — it tests exactly
  what §1 below needs: does Mesa's build-system *logic*
  (`host_machine.system()`-conditioned branches, option validation, dependency
  graph construction) accept or reject the freebsd+aarch64+lima combination.
  I fed it real files (root `meson.build`/`meson.options`, `include/`, `bin/`)
  as I went, and two intentionally-fake `pkg-config` stubs (`libdrm.pc`,
  `expat.pc` — clearly labeled as fake in-tree) once I confirmed this sandbox
  has no local `libdrm.pc` at all (an environment gap, unrelated to FreeBSD/lima).
  The exact command, the 7-step error progression, and the final result are in
  the Appendix.

Total network cost of the whole investigation: on the order of a few MB of
individually-fetched files plus the 437 KB libdrm tarball — nothing close to
"tens of minutes," as instructed.

---

## 1. Does Mesa support FreeBSD on aarch64 with the lima driver?

### 1.1 Mesa-on-FreeBSD in general: proven and extensive. Lima specifically: never attempted, and never excluded.

FreeBSD's own ports (`graphics/mesa-dri/Makefile`, `graphics/mesa-libs/Makefile`,
fetched fresh from `cgit.freebsd.org` today) build and ship Mesa's Gallium
infrastructure, `virgl`, `zink`, `llvmpipe`, `svga`, and — on aarch64
specifically — **`panfrost`** (`mesa-dri/Makefile`: `OPTIONS_EXCLUDE+=
${ARCH:Naarch64:C/.+/panfrost/}`, i.e. panfrost is *included* exactly when
`ARCH == aarch64`). `lima` appears in **neither** port's `GALLIUM_DRIVERS`
default list nor its `OPTIONS_GROUP_GALLIUM`/`OPTIONS_GROUP` — it is simply
absent, not excluded. Nobody has packaged it; that is different from Mesa
refusing to build it.

### 1.2 The build-system logic that actually decides this — read directly, not inferred

Root `meson.build` (mesa-26.2.0):

- **Line 162**: `system_has_kms_drm = ['openbsd', 'netbsd', 'freebsd',
  'gnu/kfreebsd', 'dragonfly', 'linux', 'sunos', 'android',
  'managarm'].contains(host_machine.system())` — FreeBSD is in the same
  allow-list as Linux for "this OS has KMS/DRM."
- **Lines 174–179**: when `gallium-drivers=auto` and `system_has_kms_drm` and
  `cpu_family == 'aarch64'`, the **default driver set Mesa picks for you
  already includes `'lima'`**, alongside `v3d, vc4, freedreno, etnaviv,
  nouveau, svga, tegra, virgl, panfrost, llvmpipe, softpipe, iris, zink,
  asahi` — gated purely on `system_has_kms_drm` (line 162: true for FreeBSD)
  and CPU family, never on `host_machine.system() == 'linux'` specifically.
- **Line 233**: `with_gallium_lima = gallium_drivers.contains('lima')` — a
  plain membership check, no OS condition attached at all.
- **Lines 260–268**: `with_gallium_drm = system_has_kms_drm and [...,
  with_gallium_lima, ...].contains(true)` — lima is one of the drivers that
  turns on the generic "this build talks to a DRM device" flag, gated only on
  `system_has_kms_drm`.
- **Line 343**: `with_gallium_kmsro = with_gallium_drm or (...)` — the generic
  ARM "kernel-mode-setting, render-only" winsys wrapper that lima (like
  panfrost/v3d/vc4/etnaviv) uses turns on the same way regardless of OS.
- **Lines 363–370**: `with_dri` (the frontend lima actually needs) is enabled
  when `with_gallium and (system_has_kms_drm or ...)` and either
  `glx == 'dri'` or `egl.enabled()` — FreeBSD qualifies via `system_has_kms_drm`
  exactly like Linux.

**I searched for, and did not find, any line anywhere in `meson.build` or
`meson.options` that special-cases lima against a specific OS** (no
`error('lima requires linux')`-shaped code exists; contrast with e.g. line 809's
explicit `.require(host_machine.system() == 'windows', ...)` for
`mediafoundation`, which shows Mesa's authors *do* write that pattern when a
driver genuinely is OS-locked — they didn't write it for lima).

Mesa's core does have real, deliberate FreeBSD accommodations elsewhere in the
same file — evidence this isn't merely "nobody thought about it":
line 1153 (`# Support systems without ETIME (e.g. FreeBSD)`), lines 1379
(`# FreeBSD annotated <pthread.h> but Mesa isn't ready`), lines 1607–1611
(`sys/sysctl.h` needs `<sys/types.h>` first "on FreeBSD and OpenBSD"), and the
`BSD qsort_r` argument-order check at line 1657–1671. None of these touch lima;
they're all in the shared, OS-portability part of the file that every driver
gets for free.

### 1.3 Empirical confirmation: a real (bounded) `meson setup` run

I ran `meson setup` against the real root `meson.build`/`meson.options` with
`--cross-file` declaring `system = 'freebsd', cpu_family = 'aarch64'` and
`-Dgallium-drivers=lima` (full command in the Appendix). It progressed through
**2536 lines** of real configure-time logic — every branch quoted in §1.2 above
actually executed, under `host_machine.system() == 'freebsd'`, without error —
resolved real dependencies (`zlib`, `zstd`, `threads`, `libelf`, `bison`,
`flex`, and — once I pointed `PKG_CONFIG_PATH` at a clearly-labeled fake
`libdrm.pc`/`expat.pc`, see the Appendix for why — `libdrm`/`expat`), correctly
**skipped LLVM entirely** (`Dependency llvm(...) for host machine skipped:
feature llvm disabled` — confirms lima truly doesn't need it, see §3), and
finally stopped at:

```
mesa-configtest/meson.build:2537:0: ERROR: Nonexistent build file 'src/meson.build'
```

Line 2537 is `subdir('src')` — the **first** line in the whole 2673-line file
that needs the actual multi-thousand-file source tree I deliberately did not
fetch. Everything before it — all of the FreeBSD/lima-relevant logic — ran
clean. This is the honest, expected boundary of a configure-only probe without
downloading gigabytes; it is not a rejection.

### 1.4 FreeBSD's own Mesa patch series carries essentially zero relevant patches

`graphics/mesa-dri/files/` (fetched from `cgit.freebsd.org`) has exactly 8
source patches. Skimmed all of them: one is PowerPC64 intrinsic-naming
(irrelevant to aarch64), one is an Intel i915/anv userptr retry quirk
(irrelevant to lima), one is an Intel Xe patch, one is an AMD `amdgpu` winsys
patch, one is a Vulkan WSI/Wayland patch, one is a Vulkan layer patch, one is a
`llvmpipe`-specific rasterizer patch, and the remaining generic one
(`patch-src_util_u__memory.h`) is a two-line macro *rename*
(`CACHE_LINE_SIZE` → `MESA_CACHE_LINE_SIZE`, to dodge a name collision) that
already exists, is trivial, and applies equally to every driver including any
future lima build. **None of the 8 patches touch `src/gallium/drivers/lima/`,
`src/gallium/drivers/panfrost/`, `src/panfrost/`, `src/gallium/winsys/kmsro/`,
`src/loader/`, `src/egl/`, or `src/gbm/`.** FreeBSD's own packaging experience
is direct evidence that getting a new ARM/DRM Gallium driver working on
aarch64 needs essentially no source patches beyond what's already applied for
everyone.

---

## 2. What lima's Gallium driver requires from the kernel, mapped against this port

Read directly from `src/gallium/drivers/lima/{lima_screen,lima_bo,lima_fence,lima_job}.c`
(mesa-26.2.0) against `hal/lima/{lima_drv.c,drm/lima_drm.h,drm/drm_gem_shmem_helper.c}`.

| Mesa call site | ioctl / mechanism | Kernel-side status |
|---|---|---|
| `lima_screen_query_info()`, `lima_screen.c:399–420` | `DRM_IOCTL_LIMA_GET_PARAM` ×2 (`GPU_ID`, `NUM_PP`) | **Present.** `hal/lima/lima_drv.c:87–125` (`lima_ioctl_get_param`), wired in `lima_drm_driver_ioctls[]` |
| `lima_bo_create()`, `lima_bo.c:291–303` | `DRM_IOCTL_LIMA_GEM_CREATE` | **Present.** `lima_drv.c:127–148` |
| `lima_bo_get_info()`, `lima_bo.c:129–141` | `DRM_IOCTL_LIMA_GEM_INFO` | **Present.** `lima_drv.c:150–164` |
| `lima_bo_map()`, `lima_bo.c:341–351` (native BOs) | `mmap()` on the render-node fd at `bo->offset` | **Present, non-import path only.** `drm_gem_shmem_mmap()`'s non-import branch, `drm_gem_shmem_helper.c` (kernel repo) |
| `lima_bo_import()` (FD path) → later `lima_bo_map()`, `lima_bo.c:424–445,496–499` | `drmPrimeFDToHandle` (imports the dma-buf as a native-looking handle on lima's own fd) **then the same `mmap()` path above, now on an `import_attach != NULL` object** | **This is exactly the "mmap-on-import" gap.** As of today this kernel now has a real `dma_buf_mmap()` (`hal/lima/drm/drm_gem_shmem_helper.c:787–812`, dated 2026-08-11, same day as this document) instead of the old unconditional `-EOPNOTSUPP` — but its own comment at line 805 says **"NOT verified on hardware."** Not needed for the minimal self-contained render/read-back milestone (no import happens); would matter for any dma-buf-import/EGL image test |
| `lima_bo_create()`/`lima_bo_free()`, `lima_bo.c:85–91,302` | `DRM_IOCTL_GEM_CLOSE` (generic DRM core, not lima-specific) | **Present** — core drm-kmod ioctl, compiled per `drm/Makefile:44` (`drm_ioctl.c`) |
| `lima_bo_export()`/`lima_bo_import()`, `lima_bo.c:375,398,429,487` | `DRM_IOCTL_GEM_FLINK`, `DRM_IOCTL_GEM_OPEN`, `DRM_IOCTL_PRIME_HANDLE_TO_FD`/`FD_TO_HANDLE` (all generic DRM core) | **Present**, same as above |
| `lima_bo_wait()`, `lima_bo.c:523–542` (used by the BO cache's own idle check, not the main completion path — see below) | `DRM_IOCTL_LIMA_GEM_WAIT` | **Present.** `lima_drv.c:246–262` |
| **Context init**, `lima_job.c:1130–1131` (unconditional, every context) | `drmSyncobjCreate(..., DRM_SYNCOBJ_CREATE_SIGNALED, ...)` ×2 (`in_sync`, `out_sync` per pipe) — `DRM_IOCTL_SYNCOBJ_CREATE` | **Present.** Generic drm-kmod core: `drm/Makefile:65` compiles `drm_syncobj.c`; `drm_ioctl.c:704` registers `DRM_IOCTL_SYNCOBJ_CREATE`; gated on `drm_core_check_feature(dev, DRIVER_SYNCOBJ)` which `hal/lima/lima_drv.c:370` sets (`DRIVER_RENDER \| DRIVER_GEM \| DRIVER_SYNCOBJ`) |
| **Every submit**, `lima_job.c:253,267` | `req.out_sync = ctx->out_sync[pipe]` inside `DRM_IOCTL_LIMA_GEM_SUBMIT` | **Present.** `lima_drv.c:166–244`; the kernel's own `struct drm_lima_gem_submit.out_sync` field is the exact same struct Mesa fills |
| **The actual completion wait for every job**, `lima_job.c:284` | `drmSyncobjWait(job->fd, ctx->out_sync + pipe, ...)` — `DRM_IOCTL_SYNCOBJ_WAIT` | **Present in source** (`drm_ioctl.c:714`; the whole IOCTL-level plumbing is compiled and DRIVER_SYNCOBJ-gated correctly), **but this is a different kernel code path than the plain `LIMA_GEM_WAIT` this project's existing `PLAN-mesa-lima.md` (§1.4, signal 1) tests, and it has never fired on real hardware.** See the callout below — this is the single most important correction/addition this document makes to the existing plan. |
| Native-fence export, `lima_job.c:1089` | `drmSyncobjExportSyncFile` (`DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE`) | **Present**, `drm_syncobj.c:889–892` implements the export path |
| Native-fence import (cross-API interop only — `lima_fence_server_sync()`, `lima_fence.c:53–62`; **not exercised by a self-contained test**) | `drmSyncobjImportSyncFile` | **Present**, `drm_syncobj.c:913–916` |
| `lima_fence_finish()`, `lima_fence.c:110–115` (glFinish-style wait via `pipe_screen::fence_finish`) | `poll()` on the fence fd, via Mesa's own `src/util/libsync.h` `sync_wait()` — a plain POSIX syscall, not a lima- or DRM-specific ioctl | Depends on the exported fd behaving as a pollable `sync_file`; see `sync_file.c` note below |
| `lima_fence_server_sync()`, `lima_fence.c:61` (cross-API interop only) | `ioctl(fd, SYNC_IOC_MERGE, ...)` — Mesa's `src/util/libsync.h` **vendors its own copy** of the `SYNC_IOC_MERGE` struct/macro rather than including a kernel header, specifically so it never needs `<linux/sync_file.h>` to exist at Mesa's own build time | drm-kmod's `dmabuf/Makefile` compiles `sync_file.c` (confirmed: `SRCS` list includes it) which implements `sync_file_ioctl()`'s `case SYNC_IOC_MERGE` and `sync_file_poll()`. Not exercised by a self-contained render/read-back test (only used for cross-API fence import) |
| `lima_screen_set_plb_max_blk()`, `lima_screen.c:366–379` (an H5-SoC quirk workaround; **irrelevant to this exact board's Mali-400 MP2 r1p1** and its return value is discarded by its only caller, `lima_screen.c:422`) | `drmGetDevice2()` checking `bustype == DRM_BUS_PLATFORM` | **libdrm's FreeBSD port hard-codes the wrong answer here**: `xf86drm.c:3641–3643`, `drmParseSubsystemType()`'s FreeBSD branch is `return DRM_BUS_PCI;` unconditionally — it can never report `DRM_BUS_PLATFORM` for our platform/simplebus GPU. **Confirmed non-fatal for this call site** (return value ignored) but a real, generic libdrm limitation worth knowing about if anything else ever depends on platform-bus detection |

### The one genuinely new finding here: Mesa's real completion-wait path is not the one this project's plan document tested

`PLAN-mesa-lima.md` §1.4 proposes `LIMA_GEM_WAIT` on a BO handle
(`lima_gem_wait()` → `drm_gem_dma_resv_wait()`) as the signal that proves "the
whole IRQ chain fired." That is a real, valid, and already-implemented kernel
path — but reading Mesa's actual driver shows **it is not the path Mesa itself
uses for normal job completion.** Mesa creates a `DRM_SYNCOBJ` per pipe at
context creation (`lima_job.c:1130–1131`), threads it through every submit as
`out_sync` (`:253,267`), and waits on it with `drmSyncobjWait` →
`DRM_IOCTL_SYNCOBJ_WAIT` (`:284`) — a separate ioctl family, with its own
fence-callback/wait-queue wiring inside `drm_syncobj.c`, distinct from
`lima_gem_wait()`'s direct `dma_resv` wait. Both are present and
`DRIVER_SYNCOBJ`-gated correctly in this kernel (verified above), and neither
has ever been exercised by a real submitted job on this hardware
(`MALI-STATUS.md`'s own finding that the GPU's IRQs have never fired stands for
both). **This doesn't change the verdict, but it does mean: passing the
existing plan's M1 (`LIMA_GEM_WAIT`-based) would not, by itself, prove the
signal-path Mesa actually depends on works — that needs `DRM_IOCTL_SYNCOBJ_WAIT`
specifically exercised end-to-end.** Tier 0/1 in the existing plan remain useful
for exactly what they were built to test (ioctl/VM/ctx plumbing and
hardware/frame-content, respectively); this is additive, not a correction of
an error.

---

## 3. The dependency chain

| Dependency | Needed for | FreeBSD availability | How I know |
|---|---|---|---|
| **libdrm** (core only — no `libdrm-lima`/`libdrm_lima` submodule) | every ioctl in §2 | **Ships today**, `graphics/libdrm` port, version 2.4.134, no `ARCH` exclusion for aarch64 | Fetched the real port `Makefile` + `distinfo` from `cgit.freebsd.org`; fetched and extracted the real 2.4.134 tarball and grepped its `meson.build`/`meson_options.txt` for "lima" — **zero hits**, confirming lima needs nothing beyond generic libdrm (matches Mesa's own `lima/meson.build:93`: `dependencies : [dep_libdrm, idep_nir_headers, idep_mesautil, idep_lima_pack]` — no `dep_libdrm_lima`) |
| **`libpanfrost_shared`** (Mesa vendors this; lima links against it — `lima_screen.c:48` `#include "pan_props.h"`, `meson.build:98,115`) | an ARM-Mali-generic device-properties helper, **not** the full panfrost Gallium driver | N/A (it's part of Mesa's own source, not a separate port) | Read `src/panfrost/meson.build`: `subdir('shared')` and `subdir('model')` (and, unconditionally, `subdir('compiler')`) run regardless of driver selection; `subdir('lib')`/`subdir('libpan')`/`subdir('perf')` (the actual panfrost driver internals) are gated behind `with_gallium_panfrost or with_panfrost_vk or with_tools.contains('panfrost')` and are **not** needed for a lima-only build |
| **LLVM** | nothing, for lima | N/A — not needed | `lima/meson.build:93`'s dependency list omits `dep_llvm`; the root `meson.build:247–251` block that force-enables LLVM lists `i915`, `llvmpipe`, `r300`(+draw), `radeonsi` — never lima. My `meson setup` run confirms it empirically: `Dependency llvm(...) skipped: feature llvm disabled`, no error |
| **expat** (driconf/`xmlconfig.h`, `lima_screen.c:30`) | build | Ships (`textproc/expat2`), and is already a `mesa-libs` dependency | Standard FreeBSD base/ports package; not independently re-verified beyond knowing `mesa-libs`/`mesa-dri` already depend on equivalent XML parsing |
| **zlib / zstd** (disk shader cache, `lima_disk_cache.c`) | build/runtime | zlib is in FreeBSD base; zstd ships (`archivers/zstd`), already a `mesa-dri`/`mesa-libs` `ZSTD` option | Read both ports' Makefiles directly |
| **python3 + mako + PyYAML + ply** | build-time codegen (NIR tables, lima's own `ir/lima_nir_algebraic.py` at `lima/meson.build:73–82`) | `mesa-dri/Makefile` already lists `${PYTHON_PKGNAMEPREFIX}ply>0:devel/py-ply` as a `BUILD_DEPENDS`; mako/PyYAML are unconditional Mesa build requirements (root `meson.build:1136–1147`: hard `error()` if missing) so any working FreeBSD Mesa build already has them | Directly read the ports Makefile for `py-ply`; mako/PyYAML existing as FreeBSD ports is high-confidence but **not independently fetched/verified in this session** — flagged as inferred, not read |
| **bison / flex** | GLSL preprocessor (`glcpp`) and other generated parsers | Standard FreeBSD ports (`devel/bison`, `devel/flex`) | Not independently fetched; inferred from ubiquity + my local `meson setup` run finding both on a generic Linux dev host without incident |
| **pkgconf, glslang, libdisplay-info** | glslang is `mesa-dri`-only (SPIR-V/Vulkan path lima doesn't touch); libdisplay-info is an optional `mesa-libs` `LIB_DEPENDS` | pkgconf ships everywhere; both others ship in ports but my `meson setup` run showed `libdisplay-info found: NO` did **not** block configuration for this reduced option set, i.e. it isn't hard-required here | Read `mesa-dri`/`mesa-libs` Makefiles; observed directly in the meson run |

**Everything in the first four rows (libdrm, panfrost-shared scope, no-LLVM,
expat/zlib/zstd) is directly confirmed by reading real source/ports files in
this session.** The python/bison/flex row is standard-and-ubiquitous but not
independently re-verified against FreeBSD's specific package index in this
session — treat it as high-confidence, not confirmed.

---

## 4. Build route: cross-build vs. native-on-guest

### Cross-build on this Linux host — blocked on a missing userspace sysroot, today

`infra/scripts/build-lima-arm64.sh`'s own header and prerequisite check
(`XCC="$MAKEOBJDIRPREFIX$FREEBSD_SRC/arm64.aarch64/tmp/usr/bin/cc"`) shows
exactly what exists: a **kernel** cross-toolchain, produced by FreeBSD's
`kernel-toolchain` bmake target. I checked the actual objdirs
(`/opt/bzdos/fbsd-obj/{lima-arm64,drm-kmod-arm64,mali-uio-arm64,...}`) and the
source worktree (`/opt/bzdos/freebsd-src-earlyboot-wt`, confirmed a real git
worktree of `/opt/bzdos/freebsd-src`) — there is no aarch64 **userspace**
sysroot (no target libc, no target headers, no crt objects) anywhere in this
project. `kernel-toolchain` deliberately does not build one; only `buildworld`
(or a large slice of it) does, and nothing in this repo's infra has ever run
that far for aarch64. Building Mesa needs a hosted userspace compile (it links
against `libc`, `libm`, `libpthread`, `libdrm.so`, etc.), not a freestanding
kernel-module compile — the existing cross-toolchain genuinely cannot do this
today, and standing up a real one means running the exact kind of
`buildworld`-class step this task is forbidden from running, as new,
unproven infrastructure on top of that.

### Native build on the guest — recommended

The guest is **already** a complete, self-consistent, real FreeBSD/aarch64
userspace build host (`/usr/bin/cc` is a real clang against a real sysroot,
per this project's own operating notes) — this sidesteps the entire
cross-sysroot problem for free, which is the dominant risk in the other route.
Trade-offs, honestly stated:

- **Single vCPU** (per this project's documented core layout: the FreeBSD
  guest gets one physical core). Ninja's parallelism is useless here; expect a
  long, single-threaded compile.
- **Disk**: ~4.7 GB free on the guest (per project notes). A `-Dbuildtype=release`
  build of the reduced dependency set in §3 (no LLVM, no Vulkan, no other
  Gallium drivers, no video codecs, no X11/Wayland) should fit comfortably —
  Mesa's *source* is a few hundred MB and the object/ninja directory for this
  reduced target set should be well under a GB — but this is a real
  constraint to watch, not a proven-safe margin; clean the build directory
  after producing the final `.so` if space gets tight.
- **Outbound internet from the guest is unverified.** LAN reachability
  (SSH) is proven in project memory; whether the guest can reach
  `pkg.freebsd.org` itself was not checked here (checking it would mean
  touching the board, which is off-limits for this task). The clean mitigation,
  and the one that matches this project's own established pattern ("push files
  guest-listens/host-connects, never fetch," per project notes): fetch the
  Mesa source tarball **and** the `.pkg` files for the packages in §3 from
  this Linux host — `pkg.freebsd.org` is reachable from here (confirmed,
  HTTP 200) — and push them to the guest, then `pkg add` locally with no
  guest-side internet dependency at all.
- **No LLVM requirement** (§3) removes what would otherwise be the single
  largest and slowest Mesa build dependency — this makes "slow single-core
  native build" a much smaller problem than it would be for, say, `radeonsi`
  or `llvmpipe`.

**Rough cost estimate (explicitly an estimate — nothing here was measured):**
the reduced dependency closure (`util`, `compiler/nir`, a GLSL-preprocessor
slice, `mesa/main`+`st/mesa`, `gallium/auxiliary`, `gallium/frontends/dri`,
`gallium/winsys/kmsro`, `loader`, `egl`, `gbm`, `src/panfrost/{shared,model,compiler}`,
and `lima` itself) is on the order of 800–1200 translation units. On a single
Cortex-A53-class core, a from-scratch build is plausibly **3–8 hours**
wall-clock once packages are staged — a real but bounded, overnight-able cost,
not a multi-day one. Treat this as a planning number, not a promise.

---

## 5. Build recipe (native, on the guest)

This is the concrete, step-by-step recipe the favorable verdict above earns.
None of it was executed — executing it means touching the board, which this
task forbids.

**On this Linux host** (or any host with real internet access):

```sh
# 1. Fetch a real, full Mesa source snapshot (not the selective per-file
#    fetch used for research above — an actual build needs the real tree).
#    Pin the exact commit used for this feasibility pass:
MESA_SHA=9f0a761020bca92f2b07156a0621e5360cb8eca5   # tag mesa-26.2.0
curl -L -o mesa-26.2.0.tar.gz \
  "https://codeload.github.com/chaotic-cx/mesa-mirror/tar.gz/$MESA_SHA"
#    (through this host's proxy this ran at ~11.8 KB/s when tried — budget
#    hours for a full fetch, or run it as a background task; consider trying
#    it from a different network path if one is available, since that
#    bandwidth ceiling is this proxy's, not Mesa's or the mirror's.)

# 2. Stage the FreeBSD/aarch64 .pkg files this build needs (see §3), fetched
#    from pkg.freebsd.org's aarch64 repo, so the guest needs no outbound
#    internet at all:
#      meson ninja pkgconf py3*-mako py3*-pyyaml py3*-ply libdrm expat2
#      zstd bison flex glslang(optional, unused by lima but harmless)

# 3. Push the tarball + .pkg files to the guest using this project's existing
#    listen/connect pattern (guest listens, host connects — do not fetch
#    directly onto the guest unless its outbound internet is separately
#    confirmed).
```

**On the guest** (FreeBSD 15.1 aarch64 — commands only, not run here):

```sh
# 4. Install the staged packages locally (no network needed if step 2/3 done):
pkg add /path/to/staged/*.pkg

# 5. Unpack Mesa and configure it for the smallest viable headless/offscreen
#    GLES target — this exact option set is the one whose *acceptance* was
#    verified in the meson-setup experiment in §1.3/Appendix (not its full
#    compile, which needs the real source tree this probe deliberately
#    didn't fetch):
tar xf mesa-26.2.0.tar.gz
cd mesa-mirror-<sha>
meson setup build \
  -Dgallium-drivers=lima \
  -Dplatforms= \
  -Dvulkan-drivers= \
  -Dglx=disabled \
  -Degl=enabled \
  -Dgbm=enabled \
  -Dllvm=disabled \
  -Dopengl=true \
  -Dgles1=disabled \
  -Dgles2=enabled \
  -Dvideo-codecs= \
  -Dbuild-tests=false \
  -Dtools= \
  --buildtype=release

# 6. Build. On one vCPU, -j has no benefit; run it in the background and
#    watch the log rather than waiting on the foreground:
ninja -C build 2>&1 | tee build.log &

# 7. On success, the result is libgallium-26.2.0.so (or the version-specific
#    name) containing the lima driver, plus libEGL/libGBM. Point a test
#    client at it with:
LIBGL_DRIVERS_PATH=build/src/gallium/targets/dri \
GBM_BACKENDS_PATH=build/src/gbm/backends \
EGL_PLATFORM=surfaceless \
  ./your_minimal_egl_clear_and_readpixels_test
```

**Notes on the option set** (each one earned by a real error message during
the meson-setup experiment, not guessed — see the Appendix for the exact
progression): `-Dplatforms=surfaceless` is **not** a valid value (surfaceless
is always available whenever EGL is enabled; `platforms` only lists windowing
*integrations* — `auto, x11, wayland, haiku, android, windows, macos` — so pass
`-Dplatforms=` empty). `-Ddri3` and `-Dosmesa` are **removed** options in this
Mesa version — do not pass them (a tutorial written against an older Mesa will
have them; this one doesn't). `-Dshared-glapi` is deprecated (accepted with a
warning, so just omit it).

If system `libdrm.pc` isn't found for any reason, Mesa's own `meson.build:1917,1924`
supports `-Dallow-fallback-for=libdrm` to build it as a wrapped subproject
instead — a documented escape hatch, not something I needed to use once a real
`libdrm.pc` (or the fake stub, for the probe) was on `PKG_CONFIG_PATH`.

---

## 6. What I could not verify

- **Whether the actual `.c` sources compile cleanly against real FreeBSD
  aarch64 headers.** The meson-setup experiment tests build-system *logic*
  acceptance, not a real compile — no FreeBSD userspace toolchain exists
  anywhere in this project (§4), so this remains genuinely untested. Given
  §1.4's patch-series evidence (essentially zero non-x86/non-Vulkan patches
  needed anywhere in Mesa today), I'd be surprised by major compile failures,
  but "surprised" is a prediction, not a result.
- **`DRM_IOCTL_SYNCOBJ_WAIT`'s actual signal/wakeup chain on this hardware.**
  Compiled and `DRIVER_SYNCOBJ`-gated correctly (verified by reading
  `drm_ioctl.c`/`drm_syncobj.c` directly), but — like every other GPU-IRQ-driven
  path in this project — it has never been exercised by a real submitted job.
  This is the load-bearing completion path for ordinary Mesa rendering (§2's
  callout), and it is a different, so-far-completely-unexercised code path from
  the plain `LIMA_GEM_WAIT` this project's existing plan tests.
- **The now-closed `dma_buf_mmap()` gap, on real hardware.** The kernel repo's
  own comment on it (dated today) says exactly this: implemented, not
  hardware-verified. Not needed for the minimal self-contained milestone.
- **Whether the ioctl-encoding equivalence between FreeBSD's and Linux's
  `_IOWR` macros holds for every struct Mesa uses this way** (I hand-verified
  it for `SYNC_IOC_MERGE`'s 48-byte `sync_merge_data` specifically — both
  OSes' bit layouts agree for a struct this size — but did not exhaustively
  check every other such macro-computed ioctl number Mesa or libdrm might use).
- **python3-mako/PyYAML/bison/flex as installable FreeBSD aarch64 packages** —
  treated as high-confidence (standard, ubiquitous, already required
  transitively by every existing FreeBSD Mesa build) but not independently
  fetched/confirmed against FreeBSD's package index in this session, unlike
  libdrm/expat/zstd which were.
- **Guest outbound internet reachability** — not checked (checking it means
  touching the board). The recipe in §5 is written to not depend on the answer
  either way.
- **Actual build time and actual disk footprint on the guest.** §4's "3–8
  hours" and "well under a GB" are estimates from file-count/size reasoning,
  not measurements.

---

## Appendix: the meson-setup experiment, in order

Cross-file used (`freebsd-aarch64-lima.ini`, kept in the scratch directory this
research ran from, not in this repo):

```ini
[binaries]
c = 'cc'
cpp = 'c++'
ar = 'ar'
strip = 'strip'
pkgconfig = 'pkg-config'

[host_machine]
system = 'freebsd'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true
```

Base command (constant across all iterations below):

```sh
meson setup build-probe mesa-configtest --cross-file=freebsd-aarch64-lima.ini \
  -Dgallium-drivers=lima -Dplatforms=<...> -Dvulkan-drivers= -Dglx=disabled \
  -Degl=enabled -Dgbm=enabled -Dllvm=disabled -Dopengl=true -Dgles1=disabled \
  -Dgles2=enabled -Dvideo-codecs= -Dbuild-tests=false [...]
```

Progression (each row = one real `meson setup` invocation; only the delta from
the previous row is described):

| # | What was missing / wrong | Exact error | Fix applied |
|---|---|---|---|
| 1 | `VERSION` file not fetched | `meson.build:7:12: ERROR: File VERSION does not exist.` | fetched `VERSION` (`26.2.0`) |
| 2 | guessed `-Dplatforms=surfaceless` | `Value "surfaceless" for option "platforms" is not in allowed choices: "auto, x11, wayland, haiku, android, windows, macos"` | `platforms` only lists window-system integrations; surfaceless needs no entry — used `-Dplatforms=` |
| 3 | guessed `-Ddri3=disabled` | `ERROR: Unknown option: "dri3".` | option removed in this Mesa version; dropped it |
| 4 | guessed `-Dosmesa=false` | `ERROR: Unknown option: "osmesa".` | option removed; dropped it |
| 5 | no local `libdrm.pc`/`expat.pc` on this research host at all | `Run-time dependency libdrm found: NO ... Dependency "libdrm" not found (tried pkg-config)` | added clearly-labeled **fake** `libdrm.pc`(2.4.134)/`expat.pc`(2.6.0) stubs to `PKG_CONFIG_PATH` — this step is an environment workaround for this sandbox, unrelated to FreeBSD/lima, and is **not** part of the real recipe in §5 |
| 6 | `include/meson.build`'s `include_directories('winddk')` | `Include dir winddk does not exist.` | created the (empty, Windows-only, irrelevant) directory |
| 7 | `install_headers()`/`files()` calls in `include/meson.build` for GL/GLES/EGL/KHR/CL headers | `File EGL/eglext_angle.h does not exist.` (then similarly for others, one at a time) | fetched the real headers from the pinned commit (confirms empirically that Mesa's `files()`/`install_headers()` **do** check existence eagerly at configure time, even for a variable — `opencl_headers` — that this option set never ends up consuming) |
| 8 | `bin/meson.build`'s `find_program()`/`configure_file()` for local helper scripts | (would have been `Program ... not found` / missing input file) | fetched the 5 small scripts it references |
| 9 | **final** | `meson.build:2537:0: ERROR: Nonexistent build file 'src/meson.build'` | none — this is `subdir('src')`, the deliberate, expected stopping point: the real multi-thousand-file source tree was never fetched, by design, per the task's "keep it bounded" instruction |

Every row up to and including the fixes is a real, reproducible meson
behavior against the real Mesa build system; row 9's stop is a scope decision,
not a build-system verdict.
