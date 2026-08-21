# Mali-400 / Lima on Chimp — status 2026-08-11 (evening)

> **SUPERSEDED 2026-08-20. Read [`LOOSE-ENDS.md`](LOOSE-ENDS.md) first.**
>
> This file was last touched by commit `bbb7a86` on 2026-08-11 and roughly
> twenty-five commits have landed since. Its headline claims are now inverted by
> events, and several of them read as live status rather than history:
>
> - `:36` "Still no GPU *job* has run" — `tests/limabench.c` runs 2420 draw
>   calls with textures, depth and blending at ~80 fps, 4/4 runs, zero GPU MMU
>   faults (commit `9f4088f`).
> - `:139` "The GPU's interrupts are registered and have never fired" — GP, PP
>   and MMU interrupts fire on every job.
> - `:161-163` "Nothing has *rendered* yet ... no Mesa/lima userland exists" —
>   Mesa 26.2 with the lima gallium driver is built into the guest's
>   `/usr/local`; zero-copy presentation works through `hal/bzfb` (1030.7 fps);
>   `hal/bzkms` is a real DRM/KMS device on `/dev/dri/card1` doing PRIME import
>   and page flips at 59.7 fps paced by `DRM_EVENT_FLIP_COMPLETE`.
> - `:305-307` "**Status:** built clean, not verified past the build" about the
>   PMU — that predates commit `c5885a0`; the PMU sequence runs and attach
>   proceeds past it. It is the one place this file's `:170` "the history below"
>   fence fails, because it is written as a live status line.
> - `:15` "GEM_CREATE(HEAP) -> ENOSYS, as required" — heap BOs are implemented
>   (commit `0ab14a5`).
>
> Most `file:line` citations below have also drifted (every `lima_vm.c:87` now
> means `lima_vm.c:118`). What is still open, what is proven, and what would
> settle each item is in `LOOSE-ENDS.md`, which also lists the things that look
> suspicious here and were measured to be fine.

## USERSPACE TALKS TO THE GPU — Tier 0 PASSED on hardware (2026-08-11)

`hal/lima/tests/lima_ioctl_smoke.c`, compiled on the guest with its own `cc` and
run against `/dev/dri/renderD128`: **13 passed, 0 failed, 0 skipped, exit 0.**

```
GET_PARAM(GPU_ID)     -> MALI400
GET_PARAM(NUM_PP)     -> 2
GET_PARAM(GP_VERSION) -> raw=0x0b070101 product=0xb07  (mali400) major=1 minor=1
GET_PARAM(PP_VERSION) -> raw=0xcd070101 product=0xcd07 (mali400) major=1 minor=1
CTX_CREATE            -> ctx_id 0
GEM_CREATE(4096)      -> handle 1
GEM_CREATE(HEAP)      -> ENOSYS, as required (lima_gem.c:28-37)
GEM_INFO              -> va=0x00000000 offset=0x100000000
mmap/write/read-back  -> 4096 bytes written, 1024 words compared equal
munmap / CTX_FREE / close -> clean
```

Both version words match the encoding predicted from `lima_gp.c:338-348` and
`lima_pp.c:372-392` exactly, including the two different product-ID spaces for the
same chip. The mmap round-trip is the load-bearing one: it exercises
`drm_gem_shmem_create()` + `drm_gem_shmem_get_pages_sgt()` and the **non-import**
branch of `drm_gem_shmem_mmap()` on real hardware, which is the whole GEM/VM/shmem
page-wiring path.

`va = 0x00000000` is correct, not a missing mapping: `lima_gem.c:154` adds the BO to
the per-file VM at *creation* time (`lima_vm_bo_add(vm, bo, true)`), and
`lima_device.c` sets `ldev->va_start = 0`, so the first BO in a fresh VM legitimately
lands at GPU VA 0. Checked because `lima_vm_get_va()` dereferences
`lima_vm_bo_find()`'s result without a NULL check (same as upstream) — that is
unreachable here only because `drm_gem_object_lookup(file, handle)` guarantees the
BO belongs to this file's VM.

Still no GPU *job* has run. This proves the plumbing, not the hardware pipeline.

### Correction to the plan from the Mesa feasibility work

`PLAN-mesa-lima.md` treats `LIMA_GEM_WAIT` as the completion path that matters.
Mesa does not use it: `lima_job.c` waits via `drmSyncobjWait` /
`DRM_IOCTL_SYNCOBJ_WAIT`. That path exists here — the driver declares
`DRIVER_SYNCOBJ` (`lima_drv.c:370`) and drm-kmod ships a real 1683-line
`drm_syncobj.c` with `DRM_IOCTL_SYNCOBJ_WAIT` registered (`drm_ioctl.c:714`),
both verified — but it has never been exercised. So the first untested thing on
the way to Mesa is syncobj, not `GEM_WAIT`.

## RESOLVED: the GPU is up

```
lima_platform_driver0: mmu gpmmu:  DTE read back 0xcafeb000 (want 0xCAFEB000)
lima_platform_driver0: mmu ppmmu0: DTE read back 0xcafeb000
lima_platform_driver0: mmu ppmmu1: DTE read back 0xcafeb000
lima_platform_driver0: gp  - mali400 version major 1 minor 1
lima_platform_driver0: pp0 - mali400 version major 1 minor 1
lima_platform_driver0: pp1 - mali400 version major 1 minor 1
lima_platform_driver0: bus rate = 300000000
lima_platform_driver0: mod rate = 297000000
[drm] Initialized lima 1.1.0 20191231 for lima_platform_driver0 on minor 0
```

`/dev/dri/card0` and `/dev/dri/renderD128` exist, `hw.dri.0.name` is `lima`,
`pll_gpu`/`gpu`/`bus-gpu` all hold `enable_cnt: 1`, the full drm+lima stack
unloads and reloads cleanly, and the board has been up over an hour continuously.
Mali-400 MP2 r1p1 is detected and the DRM device is registered — the first time
in this project.

**Root cause: `PLL_GPU` was never enabled, and clk(9) could not enable it.**

`sys/dev/clk/allwinner/ccu_a64.c` declares `pll_gpu_clk` with its gate bit (31)
and lock bit (28) filled in, but with `flags = AW_CLK_HAS_LOCK` — **no
`AW_CLK_HAS_GATE`**. `aw_clk_frac_set_gate()` opens with
`if ((sc->flags & AW_CLK_HAS_GATE) == 0) return (0);`, so it returns success
without touching the register.

clk(9) is not at fault for failing to try: `clknode_enable()` walks parents
first, so `pll_gpu`'s `set_gate` really was called. Measured, from inside the
driver at the moment it matters:

```
after-core: PLL_GPU=0x03006207 en=0 lock=0    <- PLL off, not locked
            GPU_CLK=0x80000000  gate=1        <- core-clock gate open
            BUS_GATE1=0x00100001 bus_gpu=1    <- AHB gate open
```

So the GPU had its bus clock and a core-clock gate **open onto a dead PLL**. In
reset that reads back garbage, which is why stages 0–2 failed cleanly. Out of
reset the GPU accepts an MMIO transaction, needs its core clock to complete it,
has none — the read never returns, the interconnect stalls, every core goes with
it, and the watchdog resets the board. Enabling `PLL_GPU` (it locks in ~230 µs,
23 polls) makes the identical stage-3 load attach cleanly.

The omission is shared by **every** `FRAC_CLK` in `ccu_a64.c` — `pll_video0`,
`pll_ve`, `pll_video1`, `pll_gpu`, `pll_hsic`, `pll_de` — and by `ccu_h3.c`. No
fractional PLL on these SoCs can be enabled through clk(9). It goes unnoticed
because U-Boot leaves the video/de PLLs running; `PLL_GPU` is the one nobody
turns on before FreeBSD, because only a GPU driver wants it.

### Fixed at the root, and verified

`AW_CLK_HAS_GATE` added to `pll_gpu_clk`'s flags in `ccu_a64.c`; guest kernel
rebuilt and deployed (`tftpboot/rebuild-2026-08-11/`, rollback in
`known-good-2026-07-30/`). Verified on hardware with the stopgap tunable
**completely unset**:

```
force_pll_gpu = kenv: unable to get hw.lima.force_pll_gpu
lima ccu[after-core]: PLL_GPU=0x93006207 en=1 lock=1
lima ccu[after-core]: VERDICT pll=1 lock=1 core=1 bus=1 rel=0
[drm] Initialized lima 1.1.0 20191231 for lima_platform_driver0 on minor 0
```

clk(9) now enables the PLL by itself. Only `pll_gpu` was changed — the other five
FRAC_CLKs share the omission and were left alone deliberately, because the flag
also lets `clk_disable()` gate them and the display path depends on
`pll_video0`/`pll_de`. See that README for the full delta (it also drops the four
`BZDOS-RACE` printfs; checked first that no tooling greps for them).

`lima_ccu_force_pll_gpu()` is kept, still opt-in and now unnecessary on this
kernel: it is the only way to run against a stock/unfixed one.

### `clknode_get_gate` — also done, and it corroborates the fix independently

`hw.clock.*.gate` used to read `unimplemented` for `pll_gpu`/`gpu` because
`aw_clk_frac`/`aw_clk_m` implement `set_gate` but not `get_gate`. That reading is
what nearly made this investigation conclude "the CCU has no gate for pll_gpu",
which was wrong. Implemented for all seven affected Allwinner clknode classes
(`patches/freebsd-src/freebsd-allwinner-clk-get-gate.patch`), compiled into the deployed
kernel, and measured:

```
before kldload lima:  pll_gpu.gate: disabled   gpu.gate: disabled   bus-gpu.gate: disabled
after  kldload lima:  pll_gpu.gate: enabled    gpu.gate: enabled    bus-gpu.gate: enabled
```

Worth more than the tidiness: this observes the clock enable through a completely
different path from the in-driver CCU dump that diagnosed it, and agrees.

### The GPU's interrupts are registered and have never fired

`vmstat -ia` (note the `-a` — plain `vmstat -i` hides zero-count sources,
`usr.bin/vmstat/vmstat.c:1229`, and reading its absence as "not registered" is a
mistake made and corrected during this pass):

```
gic0,s97 .. gic0,s103    all present, all count 0
```

SPI 97–103 are exactly the GPU's seven DT interrupts. Six carry handlers
(`gp`, `gpmmu`, `pp0`, `ppmmu0`, `pp1`, `ppmmu1`); `s101` is `pmu`, allocated with
no handler, which matches upstream — `lima_pmu_init()` does not request one.

So the platform-device IRQ bridge does register with newbus, and no GPU interrupt
has ever been delivered — because nothing has ever been submitted. That makes the
bridge the largest *untested* piece rather than a known-broken one, and
`LIMA_GEM_WAIT` in the first submit test is what will exercise it. See
`PLAN-mesa-lima.md`.

### What still needs doing

- Nothing has *rendered* yet. Attach is not a working GPU: no submit path has
  been exercised, no Mesa/lima userland is present. `PLAN-mesa-lima.md` has the
  sequenced plan; its first milestone is a native-ioctl PP clear job, not Mesa.
- 15 further Allwinner clocks share the missing-flag bug (a31/a64/h3) and A83T's
  two CPU-cluster PLLs have their gate/lock macro arguments transposed so
  `set_gate` writes bit 0 instead of 31. Neither affects this board; both are
  documented in `patches/freebsd-allwinner-clk-gate-audit.md` and deliberately
  not changed.

The history below is kept because the wrong turns in it are the reusable part.

## RETRACTION (superseded by the section above)

An earlier version of this file, written the same day, said:

> **Loading `lima.ko` takes the entire board down to U-Boot.** Reproduced twice.
> […] it is consistent with the shims now genuinely doing something: enabling the
> GPU bus/core clocks and deasserting the GPU reset really does power up Mali, and
> something about that kills the machine.

**That was wrong.** The clocks are fine, and two of the three crashes it was based
on were a different bug entirely. It was written with the crash text uncaptured —
the file said so — and the inference filled the gap. The correction below came from
fixing the observability first and then bisecting, which is what that version's own
"next steps" recommended.

## What is now established, by direct measurement

### 1. Repeated probe attempts panicked the kernel — a drm-kmod bug, not ours

```
panic: make_dev_alias_v: bad si_name (error=17, si_name=dri/renderD128)
  make_dev_alias <- drm_dev_alias <- drm_sysfs_minor_alloc
  <- drm_minor_alloc <- drm_dev_init <- drm_dev_alloc <- lima_pdev_probe+0x90
```

`drm_dev_alias()` created the `/dev/dri/renderDN` node and discarded the cdev;
nothing destroyed it, and plain `make_dev_alias()` **panics** on a duplicate name
instead of returning `EEXIST`. The alias is created inside `drm_dev_alloc()`, long
before `drm_dev_register()`, so *any* driver that allocates a DRM device and then
fails for any later reason arms this — and the second load takes the machine down.

This is what actually happened "twice" in the retracted claim: the first load of
the rebuilt `lima.ko` followed an earlier failed load in the same boot, so it
panicked in `drm_dev_alloc()` **before `lima_clk_enable()` ever ran**. The real
clock shims were never exercised.

Fixed in `patches/drm-kmod/drm-kmod-dev-alias-lifecycle.patch` (see `patches/README.md`).
Verified: three `kldload`/`kldunload` cycles of a failing lima, no panic,
`/dev/dri` absent after unload.

### 2. The clocks and the reset are correct

`hw.lima.clk_stage` (see `lima_device.c`) splits `lima_clk_enable()`'s three
operations so they can be run one at a time. On a fresh boot, one probe each:

| stage | what runs | result |
|---|---|---|
| 0 | nothing (reproduces the old stubs) | `dte write test fail`, `probe failed: -5`, **board fine** |
| 1 | bus clock | as stage 0, **board fine** |
| 2 | bus + core clock | both report ON, then `dte write test fail: -5`, **board fine** |
| 3 | bus + core + reset deassert | all three report success, then **board dies** |

So the clock enables are **safe** — stage 2 has both clocks on, fails cleanly, and
leaves guest and hypervisor alive. What the retracted claim called "the clocks crash
the board" is really "the clocks are harmless, and the reset deassert reaches
something else".

**Be precise about what this does NOT show.** Stages 0 and 2 fail *identically*
(same message, same errno), so nothing here demonstrates that enabling the clocks
had any observable effect at all. `clk_enable()` returning 0 is not proof: this port
has already been burned three times by a call that reported success and did nothing.
The A64 CCU driver does define real gates for both (`pll_gpu` bit 31 @0x38,
`gpu` bit 31 @0x1A0) and `aw_clk_frac`/`aw_clk_m` do implement `clknode_set_gate`,
so enabling *should* work — but the confirming read-back is missing, because neither
class implements `clknode_get_gate` and so `hw.clock.gpu.gate` reports
`unimplemented` rather than a value. (`hw.clock.*.enable_cnt` read after a failed
probe is also uninformative: the error path has already released the clocks.)

That leaves two live possibilities for the stall, and they are not distinguished
yet:

- **(a)** the clocks really are on, and the GPU still does not answer;
- **(b)** the core clock is not actually on, and a GPU out of reset with dead
  internal logic accepts the bus transaction and never completes it.

(b) fits the evidence just as well as (a) and is cheaper to be wrong about.

### 3. The real failure: no MMIO **read** to the Mali window ever returns

With the GPU clocked and out of reset, tracing each step (`init_ip[N]`, and the
DTE self-test bracketed write/read) gives:

```
clk_enable: bus clock ON / core clock ON / reset DEASSERTED
device_init: clocks+regulator done (regulator absent)
device_init: iomem mapped, entering per-IP init
init_ip[0] pmu off=0x2000
pmu: powering up domains, mask=0x00000003
<nothing, ever>
```

and, before the PMU was implemented, at the next block along:

```
init_ip[1] gpmmu off=0x3000
mmu gpmmu: DTE write 0xCAFEBABE
mmu gpmmu: DTE write returned, reading back
<nothing, ever>
```

Two different blocks (PMU at +0x2000, GP MMU at +0x3000), same shape: **the write
completes, the read never returns.** A write to Device memory is posted and retires
without a bus response; a read must wait for data. So this is an MMIO read that
gets no response — the CPU stalls on it, and because the stall is at the
interconnect it takes every core with it:

- no panic, no backtrace — the CPU cannot execute the panic path;
- console output stops **mid-line** (`mmu gpmmu dte write te` / `st fail.`);
- all four cores gone, so EL2 and the EMAC debug channel go too;
- the hardware watchdog eventually resets the board to U-Boot, because CPU1
  stopped petting it.

"Whole board down to U-Boot" was that, not a hypervisor crash.

Note the asymmetry that explains why this only appeared now: with the clock
**gated** (stages 0–2) the same read returns garbage instead of hanging — the
gated bus interface completes the transaction itself. Ungated, the transaction is
forwarded to a GPU that never answers.

### 4. `lima_pmu.c` was a stub, and that is a genuine root cause

36 lines: `dev_dbg(...); return 0;`. Its comment claimed "on PinePhone Pro (A64)
the PMU is controlled by the Allwinner CCU" — wrong about the SoC (PinePhone *Pro*
is RK3399/Mali-T860; this is A64/Mali-400) and wrong about the hardware: the CCU
gates the GPU's clocks and holds its reset, which is not the same thing as the
power domains **inside** the GPU. Those are switched only by this block.

So the GP/L2/PP/MMU domains were never powered up, and reading an unpowered block
is exactly the no-response stall above. Implemented for real (upstream's sequence:
mask the command interrupt, set `PMU_SW_DELAY`, read `PMU_STATUS` for which
domains are off, power those up, poll `INT_RAWSTAT`, clear).

**Status: built clean, not verified past the build.** It does not lift the stall —
`PMU_STATUS` is itself a read, so the new code hangs on its own first read, one
step earlier than before. It is still the right implementation and a real bug
fixed; it is just not sufficient.

### 5. Three stubs, one shape

`clk.h`, `regulator/consumer.h` and `lima_pmu.c` were each no-ops whose comments
asserted that something else, elsewhere, did the work — "FDT overlays and the real
CCU driver", "managed by the PMIC driver at boot", "controlled by the Allwinner
CCU". Two are now proven false and the third is the leading suspect below. A stub
that returns success is indistinguishable from a working implementation until the
hardware disagrees, and here the hardware's way of disagreeing was to take the
board down with no diagnostics at all.

## How (a) vs (b) got settled — it was (b)

The plan below was to read the CCU from outside. That turned out to be the wrong
place: a failing probe runs `lima_clk_disable()` on the way out, so by the time an
external reader looks the bits are clear again regardless of what happened; and a
*succeeding* one stalls the bus and takes the debug core with it. The window is
only observable from inside the driver, which is what `lima_ccu_dump()` does — the
console survives both cases now.

For reference, the registers (all confirmed correct against `ccu_a64.c`, and a
baseline read over EMAC on an idle board matched: everything off, GPU in reset):

| register | address | bit | meaning |
|---|---|---|---|
| `PLL_GPU_CTRL` | `0x01c20038` | 31 | PLL enable |
| `GPU_CLK_REG` | `0x01c201a0` | 31 | core clock gate |
| `BUS_CLK_GATING_REG1` | `0x01c20064` | 20 | `bus-gpu` gate |
| `BUS_SOFT_RST_REG1` | `0x01c202c4` | 20 | GPU reset (1 = deasserted) |

Result: `bus_gpu` and the core-clock gate were set, `PLL_GPU` was not — so (b),
and the bug was in the clk(9) path, not in Mali.

### The power hypothesis, which was wrong — kept because it was nearly acted on

- `linux/regulator/consumer.h` is also a **stub**: `devm_regulator_get()` returns
  `ERR_PTR(-ENODEV)` and `regulator_enable()` returns 0 without acting. Its comment
  is about PinePhone Pro, i.e. a different board.
- The deployed DTB's `gpu@1c40000` has **no `mali-supply`**, so even a real
  regulator implementation would find nothing to enable.
- This project already knows FreeBSD's PMIC driver switches off rails nobody asked
  it to (`dldo1`, the HDMI PHY supply, which the hypervisor reclaims RSB to
  re-enable).

All of that is true and none of it was the cause. The counter-evidence was already
there and is why it was not acted on first: on A64 the Mali sits in the `vdd-sys`
domain, and the guest's own PMIC log shows
`axp8xx_pmu0: Setting vdd-sys (dcdc6) to 1100000<->1100000` — a rail that powers
much of the SoC and cannot be off while the board runs. Three no-op stubs in a row
make "another stub is the cause" an attractive story; the register read settled it
in one attempt instead.

The regulator shim is still a stub and the DTB still has no `mali-supply`. Neither
blocks attach, so both are now ordinary follow-up work rather than suspects.

## Observability, which is what made all of the above possible

The reason this pass produced a chain of measurements where the previous one
produced a guess: a crash that takes the whole board down destroys its own
evidence, because the only way to get the debug channel back is to reload the
hypervisor, which resets the console ring, and the FreeBSD boot that follows
refills it.

`vconsole_init()` now copies the previous run's ring aside before resetting it
(microkernel `hv_addrmap.h` VCPM lane; read with
`bzdctl.py console --postmortem`). Every trace quoted above was recovered that way,
after the board was already gone. Hardware-verified: a marker echoed to the guest
console before a reload was read back after it.

The other half is that the traces are `dev_info`, one per step, placed so that
consecutive lines bracket a single hardware access. Each console byte is a
synchronous stage-2 fault into EL2, captured before the next instruction retires,
so the last line in the ring is exactly the last thing that executed. That is what
turned "dies somewhere in attach" into "dies on this read".

Both are deliberately permanent. The traces cost a dozen lines once per attach.

## What is cleared, by direct evidence

lima's own driver logic, the `platform_device` bridge (its IRQ "numbers" are
deliberate tokens that `linux/interrupt.h` shadows `request_irq`/`devm_request_irq`
to decode — they look like garbage and are not), the DTB, `kldload`, the clock
shims, the reset shim, and the hypervisor.
