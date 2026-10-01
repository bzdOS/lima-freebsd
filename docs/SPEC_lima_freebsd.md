# SPEC_lima_freebsd.md — Lima GPU Path on FreeBSD

**Created:** 2026-06-21
**Status:** Active specification
**Target hardware:** PinePhone (Allwinner A64, Mali-400) — Woodpecker v0.3 (**oBzdOS/OpenBSD**); QEMU virtio-gpu — the board v0.1.x / the board v0.2 (FreeBSD)

> **Lima применимость:** Mali-400 (наш A64-таргет) — это GPU, который драйвит Lima. Реальный PinePhone **Pro** (Rockchip RK3399S, Mali-T860) — **НЕ наш таргет** и использует другой драйвер (Panfrost, не Lima); упомянут здесь только для контраста.
> **TODO:** Этот spec написан под FreeBSD 15.1; для oBzdOS/OpenBSD нужен отдельный аудит Lima/weston availability.
**FreeBSD target:** 15.1 aarch64 (the board/the board only; Woodpecker = OpenBSD, see TODO above)

**See also:**
- `docs/specs/SPEC_woodpecker_hal.md §5` — display pipeline phase overview + UIO vs DRM evaluation (primary reference, not duplicated here)
- `PLAN-gpu-bringup.md` — bring-up roadmap (archived; superseded by this spec)
- `docs/specs/SPEC_squirrel_rootfs.md` — QEMU display setup (QXL/SPICE for amd64)

---

## Status / Phase map

| Phase | Name | Driver / mechanism | Blocker | Target release |
|---|---|---|---|---|
| A | fbdev MVP | weston fbdev-backend.so + Mesa softpipe | none — implemented | Woodpecker v0.3 |
| B | Lima DRM | sun4i-drm port + lima.ko in drm-kmod | ~5 kLOC kernel port, 6–12 mo | 2027 stretch |
| C | virgl (QEMU) | QEMU virtio-gpu + Mesa virgl | none — virtio-gpu exists | the board v0.2 |

---

## Phase A: fbdev + weston (MVP)

**State:** implemented. All files shipped; rc.d service wires up at boot.

### What was implemented

**HAL layer (`hal/gpu.zig`):**
- `GpuBackend` enum with `Fbdev` (active) and `Lima` (compile-time stub)
- `openFb(path)` — opens `/dev/fb0` via `posix.open` (no heap)
- `getFbInfo(fd)` — issues `FBIOGET_VSCREENINFO` ioctl, returns `FbInfo{width, height, depth, stride}`
- `closeFb(fd)` — best-effort close on teardown
- `limaOpen()` — comptime-gated stub; `@compileError` unless `active_backend == .Lima`
- FreeBSD-only guard: `comptime { if (builtin.os.tag != .freebsd) @compileError(...) }`
- All structs `extern`, 16-byte `FbInfo` verified at comptime; no `unsafe`, no `unwrap`

**rc.d service (`infra/rc.d/bsdos_display`):**
- Starts Weston under `daemon(8)` with `fbdev-backend.so`
- Injects env: `LIBSEAT_BACKEND=noop` (no logind on FreeBSD), `LIBGL_ALWAYS_SOFTWARE=1`, `MESA_GALLIUM_DRIVER=softpipe`
- Cleans stale Wayland sockets before each start
- `BEFORE: bsdos_core bsdos_core_server` ordering in rc.conf
- rc.conf knobs: `bsdos_display_backend`, `bsdos_display_fb`, `bsdos_display_ini`, `bsdos_display_runtime`, `bsdos_display_log`

**Weston config (`infra/config/weston-woodpecker.ini`):**
- `[core]` backend=fbdev-backend.so, idle-time=0, repaint-window=16ms
- `[shell]` locking=false, animation=none (softpipe budget ~30 fps)
- `[output]` name=LVDS1 (fbdev backend name), mode=720x1440, transform=normal
- `[input]` disable-touch=false, keyboard-layout=us
- `[libinput]` enable-tap=true, tap-and-drag=true (weston >= 9)

**Rendering pipeline at runtime:**
```
/dev/fb0 (U-Boot framebuffer passthrough)
    └─ weston fbdev-backend.so
         └─ Mesa softpipe (LIBGL_ALWAYS_SOFTWARE=1)
              └─ Qt6 / app via Wayland socket $XDG_RUNTIME_DIR/wayland-0
```

**Performance envelope:** ~30 fps software-rendered at 720×1440. Adequate for shell UI; not suitable for video playback or 3D. Battery impact: CPU rendering at ~300–500mW vs ~50mW with Lima.

**Deploy:**
```sh
install -m 755 infra/rc.d/bsdos_display /usr/local/etc/rc.d/
install -m 644 infra/config/weston-woodpecker.ini /usr/local/etc/weston-bsdos.ini
# rc.conf: bsdos_display_enable="YES"
service bsdos_display start
```

---

## Phase B: Lima DRM (proper GPU acceleration)

**State:** blocked on FreeBSD kernel work. Not in 2026 scope.

### What Lima is

Lima is the open-source Mesa Gallium driver for Mali-400/450 GPUs. It requires a DRM (Direct Rendering Manager) kernel interface to map GPU registers, manage command submission, and handle memory objects. On Linux, `sun4i-drm` + `lima.ko` provide this. FreeBSD has partial DRM support via `drm-kmod` (currently mirroring Linux DRM ~5.15), but the Allwinner-specific `sun4i-drm` panel/display controller driver has not been ported.

### Kernel work required

| Component | LOC estimate | Notes |
|---|---|---|
| `sun4i-drm` panel + TCON driver | ~3 kLOC C | Display controller, MIPI-DSI sequencer |
| `lima.ko` FreeBSD DRM module | ~2 kLOC C | Adapts Mesa Lima uAPI to FreeBSD `drm-kmod` |
| `drm-kmod` version bump | existing infra | May need bump to 6.1+ for Lima uAPI stability |

**Total:** ~5 kLOC new kernel code + integration testing. Estimated effort: 6–12 months for a single experienced FreeBSD/DRM developer. This is a 2027 stretch target.

### UIO bypass (evaluated, not chosen for 2026)

An intermediate path exists: a minimal `mali_uio.ko` (~250 LOC) exposes Mali-400 MMIO as `/dev/uio0`, allowing Mesa Lima's UIO backend to drive the GPU from userspace without a full DRM stack. This was evaluated in `PLAN-mali-uio.md`:

- **Pro:** 4-week implementation, no kernel DRM work
- **Con:** No KMS, no hotplug, no Vulkan path, memory safety weaker than DRM
- **Decision:** Phase A (fbdev softpipe) is good enough for 2026; skip UIO and go directly to Lima DRM in 2027 when the kernel work is done

### What Lima DRM unlocks

- OpenGL ES 2.0 hardware acceleration on Mali-400 (full Lima feature set)
- KMS/DRM pageflip → tear-free compositing
- ~10× CPU rendering reduction → battery life improvement
- Foundation for Vulkan (via Zink on top of OpenGL ES, or future Panfrost/Turnip if A64 gets an upgraded Mali)

---

## Phase C: virgl in QEMU the board

**State:** path is unblocked; implementation deferred until the board 2-stream demo is stable.

### What virgl is

VirtIO-GPU with virgl (VirGL = Virtual OpenGL) passes OpenGL ES calls from the QEMU guest through to the host GPU via the `virtio-gpu` device. Mesa inside the guest uses the `virgl` Gallium driver; the host renders via its own Mesa stack.

### Current QEMU setup (the board)

the board amd64 currently uses QXL + SPICE for display (see `docs/specs/SPEC_squirrel_rootfs.md`). This is a 2D-only path adequate for stream relay but not for GPU-accelerated compositing tests.

### virgl upgrade path

```
QEMU cmdline:
  -device virtio-gpu-gl,virgl=on
  -display sdl,gl=on          # or egl-headless for CI
  -vga none

Guest (FreeBSD 15.1):
  - drm-kmod pkg: provides virtio_gpu.ko
  - Mesa virgl Gallium driver (pkg mesa-dri)
  - weston --backend=drm-backend.so  # uses /dev/dri/card0

Host requirements:
  - QEMU >= 8.0 (virtio-gpu-gl stable)
  - Host Mesa with EGL (nvidia/amd/Intel on Linux host)
  - KVM enabled (TCG too slow for virgl)
```

**When to enable:** after the board 2-stream demo ships and `appCowork` electron42 build is unblocked. virgl enables GPU-accelerated compositor testing on amd64 before real PinePhone hardware is available.

**aarch64 QEMU (architectural target):** same virgl path applies; virtio-gpu-gl is architecture-agnostic in QEMU.

---

## Files created in this workflow

| File | Purpose |
|---|---|
| `hal/gpu.zig` | Zig GPU/display HAL — fbdev open/info/close + Lima stub |
| `infra/rc.d/bsdos_display` | FreeBSD rc.d service — starts Weston with fbdev backend |
| `infra/config/weston-woodpecker.ini` | Weston config for PinePhone (Allwinner A64, 720×1440, touch, no lockscreen) |
| `docs/specs/SPEC_lima_freebsd.md` | This spec — implementation plan and phase map |

---

## Open questions

1. **`video_info.vi_stride`:** Does FreeBSD `FBIOGET_VSCREENINFO` expose stride directly in `video_info`, or must it be derived as `width × bpp/8`? `gpu.zig` currently derives it; verify against PinePhone U-Boot framebuffer on first hardware bring-up.
2. **fbdev node on PinePhone (A64):** Is the U-Boot framebuffer exposed as `/dev/fb0` immediately after FreeBSD 15.1 boots, or does a `efifb` or `vt(4)` driver claim it first? May need `loader.conf` tuning (`hw.vga.textmode=0`).
3. **weston fbdev-backend package:** Is `graphics/weston` in FreeBSD ports built with fbdev backend enabled by default, or does it require a port option (`WITH_FBDEV=yes`)?
4. **UIO revisit trigger:** If Lima DRM port slips past 2027, re-evaluate `mali_uio.ko` as a 2027 H1 fallback.
5. **virgl on aarch64 host:** Hetzner CAX (Ampere A1) ARM64 — does the host Mesa stack support EGL for virgl host-side rendering? Needed for CI on CAX machines.
