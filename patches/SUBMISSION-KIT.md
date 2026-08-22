# Submission kit — ready to send

Ten patches, three destinations. This file is the *sending* document: titles and
bodies to paste, in the order to send them, with the reasoning already written.
`UPSTREAM-INDEX.md` is the inventory; this is the outbox.

Attach the patch file itself to every report. Do not paste diffs into a comment
box — Bugzilla and GitHub both mangle them.

**Send #9 first, alone, and do not bundle it.** It is a local denial of service
in shipped code. Everything else is a porting fix and can wait a week.

---

## 1. drm-kmod — the one that is not a porting fix

**Where:** <https://github.com/freebsd/drm-kmod> — open an issue, then a PR
referencing it. This repo does not go through Bugzilla.

**Attach:** `drm-kmod/drm-kmod-dri-sysctl-lifecycle.patch`

**Title:**

    Unprivileged `sysctl -a` panics the kernel: hw.dri OIDs outlive the device they point into

**Body:**

> On a FreeBSD 15.1 aarch64 machine running a DRM driver where debugfs is
> unavailable, an unprivileged `sysctl -a` reliably panics the kernel:
>
>     panic: vm_fault failed: 0xffff0000005374b8 error 1
>
> with `far=0x667830223d6669e8` — a faulting "address" that decodes as ASCII
> (`if="0xf`), which is what walking a freed-and-reused sysctl node looks like.
> Reproduced twice on the same hardware.
>
> **Cause.** `drm_sysctl_init()` puts the **shared** `hw.dri` node into the
> *per-device* sysctl context. `hw.dri` is not per-device: drm.ko's own module
> parameters (`hw.dri.debug`, `hw.dri.poll`, `hw.dri.vblank_offdelay`, …) hang
> off it and are owned elsewhere. `sysctl_ctx_free()` is all-or-nothing, so
> removing `hw.dri` fails with EBUSY and the call removes **nothing** — not even
> that device's own `hw.dri.<N>` subtree. `drm_sysctl_cleanup()` then `free()`s
> the `drm_sysctl_info` those surviving OIDs point into.
>
> There is a second, related asymmetry: `drm_debugfs_init()` creates the sysctl
> tree unconditionally, while `drm_debugfs_cleanup()` returns early at
> `if (!minor->debugfs_root)` — so when debugfs is absent nothing frees it at all.
>
> **Two more symptoms of the same defect**, which is how it was found:
> `hw.dri.<N>` and all its children survive `kldunload` (measured: 6 nodes
> before, 6 after), and because Mesa reads `hw.dri.<N>.busid`, **all GL breaks
> after one driver reload** with `MESA-LOADER: failed to retrieve device
> information` / `eglInitialize 0x3001` until the machine is rebooted.
>
> **The patch** gives `hw.dri` and the three shared leaves under it a NULL
> context so they outlive any single device, keeps the per-device subtree in the
> context (where it now frees cleanly), makes `drm_sysctl_cleanup()` refuse to
> free the struct when `sysctl_ctx_free()` fails — a bounded leak is strictly
> better than a use-after-free any user can trigger — and moves the sysctl
> cleanup above `drm_debugfs_cleanup()`'s early return so it mirrors init.
>
> **Verified** over 5 consecutive load / GL / `sysctl -a` / unload cycles: GL
> works every cycle, `sysctl -a` returns 0 every cycle, 0 leaked nodes every
> cycle.
>
> Found while bringing up an out-of-tree Mali-400 driver, but nothing about the
> bug requires it: any DRM driver on a machine without debugfs will do.

---

## 2. drm-kmod — the three porting fixes

**Where:** same repo, one PR each (or one PR with three commits; ask the
maintainers' preference in the issue above).

**Attach / order:** `drm-kmod-dma-buf-mmap.patch`,
`drm-kmod-dev-alias-lifecycle.patch`, then `drm-kmod-nonpci-busid.patch`
(this one touches the same function as #1, so it goes after it).

**Titles and one-line summaries:**

- `dmabuf: add dma_buf_mmap()` — imported PRIME buffers cannot be `mmap()`ed
  without it. It is upstream Linux's own function minus one FreeBSD-specific
  line (`vma_set_file()`, whose `struct file *` vs `struct linux_file *`
  mismatch is demonstrated in the patch header). Write-up: prose section in
  `README.md`.
- `drm_dev_alias(): fix cdev lifecycle` — the `/dev/dri` alias cdev is leaked, so
  the next allocation of the same minor panics in `make_dev_alias_v()`.
  Write-up: `UPSTREAM-drm-kmod-dev-alias.md`.
- `drm_add_busid_modesetting(): do not dereference pdev->bus on non-PCI devices`
  — panics with `far=0x8` on **any** platform DRM driver, because it reads
  `pdev->bus->number` through a struct that is not a `pci_dev`. The patch guards
  with `dev_is_pci()` and synthesises a PCI-shaped busid otherwise, **varying per
  device** — a constant breaks Mesa's loader as soon as two DRM devices exist,
  which we hit with two. Note honestly in the PR that the synthesised string is a
  compatibility lie: the correct fix is teaching libdrm/Mesa about platform
  devices, which is a userspace change.

---

## 3. freebsd-src — LinuxKPI DMA, arm64-only

**Where:** Bugzilla <https://bugs.freebsd.org>, product **Base System**,
component **kern**. Cc `freebsd-x11@FreeBSD.org` (graphics/LinuxKPI territory).
Consider Phabricator <https://reviews.freebsd.org> for review first.

**Attach, in order:** `freebsd-linuxkpi-01-dma-map-sg-multipage.patch`, then
`freebsd-linuxkpi-02-dma-alloc-coherent-memattr.patch`. Applied in that order to
`134a4b503^` they reproduce the committed tree byte-for-byte.

**Title:**

    linuxkpi: dma_alloc_coherent() returns cacheable memory on arm64, and dma_map_sg() cannot map multi-page lists

**Body:** use `UPSTREAM-freebsd-linuxkpi-dma.md` verbatim — it is written for
this audience. The two points a maintainer will check first:

1. **`dma_alloc_coherent()` hands out write-back memory on arm64.**
   `kmem_alloc_contig(..., VM_MEMATTR_DEFAULT)` is write-back there, so the one
   allocator whose contract is "no cache maintenance required" gives a device the
   physical address of cacheable memory. Fix: `VM_MEMATTR_UNCACHEABLE` on
   aarch64/arm/riscv, matching `bus_dmamem_alloc(BUS_DMA_COHERENT)`.
2. **`nsegments = 1` makes a multi-page `dma_map_sg()` impossible.** busdma
   bounds the per-map *sync list* by `dmat->common.nsegments` and `sync_count`
   accumulates across calls on one map, so entry 0 takes the single slot and
   entry 1 returns EFBIG. This coupling is visible in
   `sys/arm64/arm64/busdma_bounce.c` and needs no hardware to confirm. Measured:
   `load_phys failed err=27 i=1/64`, every run. Plus: three error paths leave
   `sgl->dma_map` stale and `linux_dma_unmap_sg_attrs()` validates nothing, so a
   caller that unmaps after a failed map dereferences it (`far 0x1`,
   `bounce_bus_dmamap_sync+0x34c`).

Say plainly that the reproducer is an out-of-tree driver, so it cannot be handed
over — but that (2) is checkable by reading busdma, and the stale-`dma_map`
NULL-deref needs no hardware at all.

---

## 4. freebsd-src — Allwinner clocks

**Where:** Bugzilla, product **Base System**, component **arm**. Cc
`freebsd-arm@FreeBSD.org`.

**Attach:** `freebsd-ccu-a64-pll-gpu.patch`, `freebsd-allwinner-clk-get-gate.patch`,
`freebsd-ccu-a83t-cpux-gate-lock.patch` — independent of each other; may be one
report or three.

**Titles:**

- `ccu_a64: pll_gpu declares a gate bit but omits AW_CLK_HAS_GATE, so clk_enable() silently no-ops`
  — the GPU cannot be clocked at all without this. Write-up:
  `UPSTREAM-freebsd-ccu-a64-pll-gpu.md`.
- `aw_clk: FRAC_CLK / NKMP_CLK / M_CLK clknodes have no get_gate method` (7 files)
  — the diagnostic that found the one above. Write-up:
  `freebsd-allwinner-clk-gate-audit.md`.
- `ccu_a83t: transposed gate/lock arguments on the CPUX PLLs` — A83T only, so
  say up front that we do not have that board and it is offered for review rather
  than as a tested fix. Write-up:
  `UPSTREAM-freebsd-ccu-a83t-cpux-gate-lock.md`.

---

## 5. ports — graphics/mesa-dri

**Where:** Bugzilla, product **Ports & Packages**, component
**Individual Port(s)**. Read the port's `MAINTAINER` line and Cc them first.

**Attach:** `freebsd-ports/mesa-dri-lima-gallium-option.patch` (apply with `-p0`
from the ports root).

**Title:**

    graphics/mesa-dri: add a lima option (Mali-400/450, Utgard)

**Body:**

> `graphics/mesa-dri` builds every other gallium driver behind an option and
> already ships the display-only half of the Allwinner pairing —
> `sun4i-drm_dri.so` is in `pkg-plist` unconditionally. What it has never had is
> `lima_dri.so`, the render driver. So on an Allwinner A64 board
> `pkg install mesa-dri` installs 49 `*_dri.so` files, the one for that SoC's GPU
> is not among them, and libgallium reports `lima: driver missing`.
>
> Three lines in the `Makefile` plus one `pkg-plist` entry, following the
> `panfrost` pattern. **No libclc/LLVM dependency**, unlike panfrost: lima's
> compilers (gpir for vertex, ppir for fragment) are self-contained. Measured,
> not assumed — Mesa 26.2 with `-Dgallium-drivers=lima -Dllvm=disabled`
> cross-builds all 997 targets for FreeBSD/aarch64 and produces a real aarch64
> `lima_dri.so`.
>
> Utgard shipped on both 32-bit and 64-bit Allwinner parts (A20/H3 armv7,
> A64/H5 aarch64), so — unlike panfrost, which this port excludes on armv7 — the
> option is offered on both and excluded elsewhere.
>
> **The honest caveat, stated up front:** this option is only useful to someone
> running an out-of-tree `lima(4)`, because FreeBSD has no in-tree one. It becomes
> useful to everybody if the drm-kmod patches above land and the driver is
> upstreamed. If the maintainers would rather wait for that, this can sit in the
> PR until then.

---

## Things worth saying in every report

- **Every claim has a number behind it.** These were all diagnosed from measured
  register values and before/after counts on real hardware, and the patch headers
  carry them. A hardware claim without a number is unreviewable.
- **Say where it was found.** An out-of-tree driver on a Banana Pi M64 is an
  unusual context and hiding it wastes a reviewer's time.
- **Several were only visible with a second consumer** — two DRM devices instead
  of one, a second test exercising a different path. If a maintainer says "works
  for me", that is often why, not disagreement.
- **Do not claim more than one board.** Nothing here has been tested on a second
  unit.
