# Upstream submission index

Ten distinct third-party defects were found while porting lima to
FreeBSD/arm64. Every one is fixed, every fix has a patch file, and **none has
been submitted** — submission needs the author's own accounts (FreeBSD
Bugzilla/Phabricator, the drm-kmod GitHub repo, the ports tree).

This file exists so that sending them is an afternoon of clerical work rather
than a re-derivation. **`SUBMISSION-KIT.md` is the outbox**: the titles and
bodies to paste, per destination, in the order to send them. Order within a tree is the order they must be applied.

## `freebsd-src` — 5 fixes

| # | Fix | Patch | Write-up | Needed for this board? |
|---|---|---|---|---|
| 1 | `dma_map_sg()` cannot map a multi-page list on non-coherent arm64 (`nsegments=1` vs busdma's accumulating `sync_count`), **plus** stale `sgl->dma_map` on three error paths that unmap then dereferences | `freebsd-src/freebsd-linuxkpi-01-dma-map-sg-multipage.patch` | `UPSTREAM-freebsd-linuxkpi-dma.md` | **yes** |
| 2 | `dma_alloc_coherent()` returned CACHEABLE memory on arm64 — the one allocator that promises no cache maintenance | `freebsd-src/freebsd-linuxkpi-02-dma-alloc-coherent-memattr.patch` | same doc | **yes** — this is the fix that produced the first frame |
| 3 | `ccu_a64.c`'s `pll_gpu_clk` declares a gate bit but omits `AW_CLK_HAS_GATE`, so `clk_enable()` silently no-ops | `freebsd-src/freebsd-ccu-a64-pll-gpu.patch` | `UPSTREAM-freebsd-ccu-a64-pll-gpu.md` | **yes** — without it the GPU cannot be clocked at all |
| 4 | `FRAC_CLK`/`NKMP_CLK`/`M_CLK` clknodes had no `get_gate` method at all (7 files) | `freebsd-src/freebsd-allwinner-clk-get-gate.patch` | `freebsd-allwinner-clk-gate-audit.md` | yes — it is the diagnostic that found #3 |
| 5 | `aw_ccung` transposed gate/lock args on A83T CPUX PLLs | `freebsd-src/freebsd-ccu-a83t-cpux-gate-lock.patch` | `UPSTREAM-freebsd-ccu-a83t-cpux-gate-lock.md` | no — A83T is a different SoC; carried for upstream only |

Apply order: 1 → 2 (verified as a series). 3, 4, 5 are independent of those two
and of each other.

## `drm-kmod` — 4 fixes

| # | Fix | Patch | Write-up | Needed for this board? |
|---|---|---|---|---|
| 6 | dmabuf had no `dma_buf_mmap()`, so imported PRIME buffers could not be `mmap()`ed | `drm-kmod/drm-kmod-dma-buf-mmap.patch` | prose section in `README.md` | **yes** — a build prerequisite: without it `drm_gem_shmem_helper.c` does not compile |
| 7 | `drm_dev_alias()` leaks its `/dev/dri` cdev; the next alloc of the same minor panics in `make_dev_alias_v()` | `drm-kmod/drm-kmod-dev-alias-lifecycle.patch` | `UPSTREAM-drm-kmod-dev-alias.md` | **yes** |
| 8 | `drm_add_busid_modesetting()` dereferences `pdev->bus->number` through a struct that is not a `pci_dev` — panics (`far=0x8`) on **any** platform DRM driver. Fallback busid also has to be unique per device or Mesa's loader cannot tell two DRM devices apart | `drm-kmod/drm-kmod-nonpci-busid.patch` | 50-line header in the patch | **yes** |
| 9 | `hw.dri` sysctl lifecycle: the shared node was put in a per-device context, so `sysctl_ctx_free()` failed with EBUSY and removed nothing while cleanup freed the struct those OIDs point into. Consequences: GL broken after any driver reload until reboot, **and an unprivileged `sysctl -a` panics the kernel** | `drm-kmod/drm-kmod-dri-sysctl-lifecycle.patch` | 60-line header in the patch | **yes** |

**#9 is the one to send first.** It is a local denial of service reachable by
any user with a shell, on any FreeBSD machine running a DRM driver where
debugfs is unavailable — it needs no lima, no Mali and no bzdOS to reproduce.
Everything else here is a porting fix; that one is a security-relevant bug in
shipped code.

Apply order: 6, 7 independent; 9 before 8 (they touch the same function and
were verified in that order — applied to a pristine `drm_sysctl_freebsd.c` they
reproduce the working tree byte-for-byte).

## `freebsd-ports` — 1 fix

| # | Fix | Patch | Write-up | Needed for this board? |
|---|---|---|---|---|
| 10 | `graphics/mesa-dri` has no `lima` option, so Mali-400 users get no OpenGL driver from the package system — while the port already ships the display-only half (`sun4i-drm_dri.so`) | `freebsd-ports/mesa-dri-lima-gallium-option.patch` | 40-line header in the patch | no — but it is what makes this port usable by anyone else |

Note the honest caveat in its header: the option is only useful to someone
running an out-of-tree lima(4), because FreeBSD has no in-tree one. It becomes
useful to everybody if #6–#9 land and the driver itself is upstreamed.

## Where the write-ups live, and why some are in the patch

Three of the ten have a dedicated `UPSTREAM-*.md` because they needed a long
argument about hardware behaviour. Four carry their reasoning as a header
comment inside the patch file instead — deliberately, because a patch that
travels with its own justification is harder to mis-send than one whose
rationale lives in a separate file in a different repository. Two share
`UPSTREAM-freebsd-linuxkpi-dma.md`. One (#6) has only prose in `README.md` and
is the weakest-documented of the set.

## Verification status

Every patch in this index applies cleanly to a pristine copy of its target
tree — checked by reverse-applying or by reconstructing the baseline, not
assumed. Nine of the ten are load-bearing on live hardware right now: the board
boots, the GPU renders (textures, 2420 draws, depth, blend) and presents at
60 fps through DRM/KMS page flips paced by the real panel vblank. #5 is the
exception and is carried for upstream only.

## Where each of these actually goes

The channels, so nobody has to work it out at sending time. Confirm the current
maintainer before sending — these are the standard routes, not a promise about
who answers.

| Target | Route |
|---|---|
| `freebsd-src` (#1–#5) | FreeBSD Bugzilla — <https://bugs.freebsd.org>, product *Base System*. For a code review first, Phabricator — <https://reviews.freebsd.org>. The LinuxKPI/DRM ones (#1, #2) belong with the graphics people: the `freebsd-x11@FreeBSD.org` list. The Allwinner clock ones (#3–#5) are arm/SoC territory: `freebsd-arm@FreeBSD.org`. |
| `drm-kmod` (#6–#9) | <https://github.com/freebsd/drm-kmod> — issues and pull requests. This is a separate repository from FreeBSD base and does not go through Bugzilla. |
| `graphics/mesa-dri` (#10) | FreeBSD Bugzilla, product *Ports & Packages*, component *Individual Port(s)*. Read the port's own `MAINTAINER` line first and Cc them; graphics ports are usually maintained by the x11 team. |

**Send #9 first and separately.** It is a local denial of service — an
unprivileged `sysctl -a` panics the kernel — and it stands alone: it needs no
lima, no Mali and no explanation of this project. Bundling it with nine porting
fixes buries the one thing a maintainer must act on quickly.

Two practical notes learned the hard way here. First, every patch in this
directory carries its evidence — the failing register values, the measured
before/after — because a claim about hardware behaviour without a number in it
is unreviewable. Second, several of these were found only because a *second*
consumer appeared (two DRM devices instead of one, a second test exercising a
different path); if a maintainer says "works for me", that is often the reason,
not disagreement.
