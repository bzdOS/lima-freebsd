# What travels, and what does not

This driver was written inside bzdOS, for a Mali-400 behind a bare-metal EL2
hypervisor. Most of it is not about that at all: it is a FreeBSD/arm64 port of
Linux's `lima`, and it is useful to anyone with Utgard silicon and no GPU driver.
This file is the boundary, so an extraction does not have to be an archaeology
exercise. It is deliberately a manifest, not a script — the split is a judgement
about *meaning*, and a reader should be able to disagree with it.

The public extraction lives at **https://github.com/bzdOS/lima-freebsd**.

## TRAVELS — the port proper

| What | Notes |
|---|---|
| `lima_*.c` / `lima_*.h` | The driver. `lima_ccu_debug.c/.h` is the one judgement call — see below. |
| `drm/` | The `drm_gem_shmem_helper.c` fork and `drm_utils_freebsd.c`. The fork is the substance of the port: FreeBSD has no `drm_gem_shmem` and Utgard needs one contiguous run per BO for DE2 scanout. |
| `linux/`, `platform_device.c` | The LinuxKPI gaps this port had to fill. |
| `Makefile` | Needs `DRM_KMOD_SRC`; see the header. |
| `patches/` | **Load-bearing, not optional.** Nine fixes across three foreign trees (drm-kmod, freebsd-src, freebsd-ports). Two of them are build prerequisites — without `drm-kmod-dma-buf-mmap.patch` the shmem helper does not compile, and without `freebsd-ccu-a64-pll-gpu.patch` the GPU cannot be clocked at all. `patches/README.md` explains why they are split by target tree, and `patches/*/UPSTREAM-*.md` are the write-ups. |
| `tests/` | `limabench.c` (the real workload: textures, 2420 draws, depth, blend), `lima_ioctl_smoke.c`, `test_lima_math.c`, `test_shmem_logic.c` (host-side, no board). `lima_pp_clear.c` / `limatri.c` predate Mesa and are only interesting as history. |
| `MALI-STATUS.md`, `LOOSE-ENDS.md`, `README-arm64.md` | The honest state of things, including what is unproven. Keep them; a port whose known weaknesses are written down is worth more than one that looks clean. |

**`bzdOS` mentions in travelling sources are comments only** — 19 of them, in
`lima_mmu.c`, `lima_device.c`, `drm/drm_gem_shmem_helper.c`,
`tests/lima_ioctl_smoke.c` and a few others. Every one records a real finding
(why a BO must be contiguous, why a teardown is ordered the way it is). **Reword
them, do not delete them.** Deleting them loses the reason and leaves code that
looks arbitrary.

`lima_ccu_debug.c` is Allwinner clock-controller debug scaffolding — not
bzdOS-specific, but not part of lima either. It travels because it is what found
the `ccu_a64.c` `AW_CLK_HAS_GATE` omission that made the GPU unclockable, and
anyone bringing this up on another Allwinner part will want it. Drop it if you
are on different silicon.

## DOES NOT TRAVEL — `bzdos/`

Everything bzdOS-specific now lives under `bzdos/`, so the boundary is visible
in the tree instead of only in prose:

| What | Why |
|---|---|
| `bzdos/hvfb/` | 24 KB of never-compiled code (`lima_hvfb.c`, its uapi header, its Makefile). It was the first attempt at handing a rendered buffer to the hypervisor's scanout; `hal/bzfb` took the job over, then `hal/bzkms`. Nothing outside the directory references it. Kept rather than deleted because it is the only written record of that first design — but it is not a dependency of anything. |
| `bzdos/SCANOUT-IMPORT.md` (bzdk-side, not in this repository) | 49 KB arguing for an import route that was later **measured not to work** (it wrote the imported dma-buf exactly once, at ~400 fps of nothing changing). Its geometry arithmetic is stale too. Kept for its findings; see the status note at its head. |

Also outside this directory and outside the port: `hal/bzfb` (the hypervisor
framebuffer/doorbell device) and `hal/bzkms` (the DRM/KMS device whose page flip
*is* the hypervisor doorbell). Those are bzdOS products, not lima.

## What an extraction still has to do

1. Repoint the cross-references. `patches/README.md`, `MALI-STATUS.md` and the
   `UPSTREAM-*.md` files name paths inside this tree.
2. Decide about `lima_ccu_debug.c` (above).
3. Reword, not delete, the `bzdOS` comments.
4. Nothing else. There are **no code changes** required — verified by grep, not
   assumed: every bzdOS reference in a travelling `.c`/`.h` is inside a comment.
