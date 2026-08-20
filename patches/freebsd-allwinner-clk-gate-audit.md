# Audit: silently un-enableable Allwinner clocks in FreeBSD's `dev/clk/allwinner`

Scope: every `ccu_*.c` and every `aw_clk_*.c` clknode class under
`/opt/bzdos/freebsd-src-earlyboot-wt/sys/dev/clk/allwinner/` (11 `ccu_*.c` files,
9 clknode classes). Read-only analysis — this repo, `freebsd-src-earlyboot-wt`,
was **not modified** to produce this document. `ccu_a64.c` in particular was
read but not edited (a kernel build is running against it as of 2026-08-11).

All line numbers below were confirmed by direct reading of the cited file, not
recalled or guessed. Where I could not verify something, I say so instead of
inferring past the evidence.

## 1. How the bug works, precisely

Each of the `NKMP_CLK`/`NM_CLK`/`NMM_CLK`/`NP_CLK`/`M_CLK`/`FRAC_CLK` macros in
`aw_clk.h` takes a `gate_shift` (or `_gate`) argument **and** a separate
`_flags` argument. Confirmed by reading the macro bodies in
`aw_clk.h:181-585`: `gate_shift` is always its own struct field
(`.gate_shift = _gate_shift`), independent of whatever is passed as `_flags`.
Declaring a nonzero `gate_shift` says "here is the register bit"; it does
**not** imply the class will use it. Each class's `*_set_gate()` guards on the
flag, not the shift value:

```c
/* aw_clk_frac.c:106-107, and the same shape in aw_clk_m.c, aw_clk_nkmp.c,
 * aw_clk_nm.c, aw_clk_nmm.c, aw_clk_np.c */
if ((sc->flags & AW_CLK_HAS_GATE) == 0)
        return (0);
```

So a clock declared with a real gate bit but without `AW_CLK_HAS_GATE` in its
flags word reports `clk_enable() == 0` ("success") while never touching the
register. `clknode_enable()` (`sys/dev/clk/clk.c:1091`) does walk to parents
first, so the call *does* reach the affected clknode — it just no-ops there.

One clknode class, `aw_clk_mipi.c`, does **not** have this guard at all — its
`set_gate` unconditionally touches the gate bit (`aw_clk_mipi.c:97-105`). That
matters for what its `get_gate` must look like (§3).

## 2. Clocks that declare a gate bit but omit `AW_CLK_HAS_GATE`

"Declares a gate bit" below means: the macro invocation passes a **nonzero**
`gate_shift`/`_gate` argument, positionally confirmed against the macro
definition in `aw_clk.h` (not inferred from the source comment next to it —
see the A83T finding in §5, where the comment and the actual argument position
disagree). This only happens with the `FRAC_CLK` macro; I found zero instances
on `NKMP_CLK`, `NM_CLK`, `NMM_CLK`, `NP_CLK`, or `M_CLK` anywhere in the 11
files — every one of those already pairs a nonzero gate bit with
`AW_CLK_HAS_GATE` correctly (full per-file breakdown in §4).

| # | File | Clock | Line(s) | gate bit | flags as declared | Status |
|---|------|-------|---------|----------|--------------------|--------|
| 1 | `ccu_a31.c` | `pll_video0_clk` | 291-301 (gate at 297) | 31 | `AW_CLK_HAS_LOCK` | affected |
| 2 | `ccu_a31.c` | `pll_ve_clk` | 312-322 (gate at 318) | 31 | `AW_CLK_HAS_LOCK` | affected |
| 3 | `ccu_a31.c` | `pll_video1_clk` | 359-369 (gate at 365) | 31 | `AW_CLK_HAS_LOCK` | affected |
| 4 | `ccu_a31.c` | `pll_gpu_clk` | 381-391 (gate at 387) | 31 | `AW_CLK_HAS_LOCK` | affected — feeds a Mali-400MP2 |
| 5 | `ccu_a31.c` | `pll9_clk` | 406-416 (gate at 412) | 31 | `AW_CLK_HAS_LOCK` | affected |
| 6 | `ccu_a31.c` | `pll10_clk` | 418-428 (gate at 424) | 31 | `AW_CLK_HAS_LOCK` | affected |
| 7 | `ccu_a64.c` | `pll_video0_clk` | 278-288 (gate at 284) | 31 | `AW_CLK_HAS_LOCK` | affected |
| 8 | `ccu_a64.c` | `pll_ve_clk` | 300-310 (gate at 306) | 31 | `AW_CLK_HAS_LOCK` | affected |
| 9 | `ccu_a64.c` | `pll_video1_clk` | 371-381 (gate at 377) | 31 | `AW_CLK_HAS_LOCK` | affected |
| — | `ccu_a64.c` | `pll_gpu_clk` | 409-419 (gate at 415) | 31 | `AW_CLK_HAS_LOCK \| AW_CLK_HAS_GATE` | **already fixed** — see note below, not counted |
| 10 | `ccu_a64.c` | `pll_hsic_clk` | 432-442 (gate at 438) | 31 | `AW_CLK_HAS_LOCK` | affected |
| 11 | `ccu_a64.c` | `pll_de_clk` | 445-455 (gate at 451) | 31 | `AW_CLK_HAS_LOCK` | affected |
| 12 | `ccu_h3.c` | `pll_video_clk` | 295-305 (gate at 301) | 31 | `AW_CLK_HAS_LOCK` | affected |
| 13 | `ccu_h3.c` | `pll_ve_clk` | 308-318 (gate at 314) | 31 | `AW_CLK_HAS_LOCK` | affected |
| 14 | `ccu_h3.c` | `pll_gpu_clk` | 357-367 (gate at 363) | 31 | `AW_CLK_HAS_LOCK` | affected — feeds a Mali-400 |
| 15 | `ccu_h3.c` | `pll_de_clk` | 383-393 (gate at 389) | 31 | `AW_CLK_HAS_LOCK` | affected |

**15 currently-affected clocks** across 3 files. A 16th instance
(`ccu_a64.c`'s `pll_gpu_clk`) has already been fixed in the working tree as of
this reading (`AW_CLK_HAS_GATE` added at line 416, with a comment dated
2026-08-11 explaining exactly this bug) — I did not touch that file and am not
counting it as still-affected.

`ccu_a64.c`'s own new comment at that clock (lines 384-408) independently
lists the same five other `ccu_a64.c` FRAC_CLKs in row 7-11 above, plus "and so
does `ccu_h3.c`" — this audit's row 12-15 for `ccu_h3.c` confirms that claim by
direct reading; I did not simply take the comment's word for it.

### Files checked and found **not** to have this bug

`FRAC_CLK` is the only macro where this omission occurs anywhere in the tree.
Files that use it correctly, or don't use it at all:

- `ccu_a10.c`: uses `FRAC_CLK` twice (`pll_video0_clk` line 210,
  `pll_video1_clk` line 230) — both correctly carry `AW_CLK_HAS_GATE`
  (lines 217, 237).
- `ccu_h6.c`: has the same family of PLLs (`pll_ddr0`, `pll_gpu`, `pll_video0_4x`,
  `pll_video1_4x`, `pll_ve`, `pll_de`, `pll_hsic`) but built with `NMM_CLK`
  instead of `FRAC_CLK`, and every one of them correctly carries
  `AW_CLK_HAS_GATE | AW_CLK_HAS_LOCK` (e.g. line 264 for `pll_gpu_clk`). H6 is
  **not** affected by this bug at all, despite superficially resembling A64.
- `ccu_a83t.c`, `ccu_a13.c`, `ccu_d1.c`, `ccu_de2.c`, `ccu_h6_r.c`,
  `ccu_sun8i_r.c`: no `FRAC_CLK` usage at all. Every `NKMP_CLK`/`NM_CLK` clock
  in these files that declares a nonzero gate bit correctly carries
  `AW_CLK_HAS_GATE` (verified individually, not sampled) — including the raw
  (non-macro) `struct aw_clk_nkmp_def`/`struct aw_clk_nm_def` literals in
  `ccu_a13.c` (`pll_core`, `pll_audio`, `pll_ddr_base`, `pll_periph`, `nand_clk`,
  `mmc0_clk`, `mmc1_clk`, `mmc2_clk`, `ss_clk`, `spi0_clk`, `spi1_clk`,
  `spi2_clk`, `ir_clk`) and `ccu_sun8i_r.c` (`a83t_ir_clk`).

A number of `NM_CLK`/`M_CLK` clocks across every file declare `gate_shift = 0`
with no `AW_CLK_HAS_GATE` (e.g. `apb2_clk` in `ccu_a64.c:519-527`, `dram_clk` in
`ccu_a64.c:657-663`, `dram_clk` in `ccu_h3.c:571-578`, all the `pll_*_2x`/`_4x`
divider-stage `M_CLK`s in `ccu_d1.c`). I am **not** counting these: bit 0 is
also the struct's zero-initialized default, so a macro invocation with
`gate_shift = 0` and no flag is indistinguishable, from the source alone, from
"this clock genuinely has no gate register." I can't rule out that one of
these is a real bit-0 gate the author forgot to flag — that would require the
SoC's clock-register reference manual, which is outside what I can verify by
reading this source tree. I only counted rows where the declared bit is
non-zero, matching the exact shape of the bug already found in `pll_gpu_clk`
(gate bit filled in with a specific position, comment says `/* gate */`, flag
absent).

## 3. Clknode classes with `set_gate` but no `get_gate`

Confirmed by reading `clknode_if.m:67-81`: `get_gate` is a distinct KOBJ method
(`METHOD int get_gate { struct clknode *clk; bool *enabled; }`) with its own
contract — 0 on success, `ENOENT` if the hardware has no readable gate,
`ENXIO` if unimplemented. It is **not** synthesized from `set_gate`. Confirmed
in `clk.c`: the base `clknode_class` (`clk.c:75-84`) registers `set_gate` but
not `get_gate`, so any subclass that only overrides `set_gate` inherits the
KOBJ default for `get_gate`, which resolves to `kobj_error_method` → `ENXIO`.
The `hw.clock.<name>.gate` sysctl (`clk.c:1701-1711`) turns that `ENXIO`
straight into the string `"unimplemented"` — which is what actively misled the
investigation into thinking `pll_gpu` had no gate at all, when it did (bit 31)
and the driver was simply never asking the hardware.

| Class (file) | `set_gate` | `set_gate` guarded by `AW_CLK_HAS_GATE`? | `get_gate` |
|---|---|---|---|
| `aw_frac_clknode` (`aw_clk_frac.c:99-120`, method table line 337) | yes | yes | **missing** |
| `aw_m_clknode` (`aw_clk_m.c:90-110`, method table line 244) | yes | yes | **missing** |
| `aw_mipi_clknode` (`aw_clk_mipi.c:87-110`, method table line 232) | yes | **no** — unconditional | **missing** |
| `aw_nkmp_clknode` (`aw_clk_nkmp.c:95-115`, method table line 347) | yes | yes | **missing** |
| `aw_nm_clknode` (`aw_clk_nm.c:91-111`, method table line 291) | yes | yes | **missing** |
| `aw_nmm_clknode` (`aw_clk_nmm.c:78-98`, method table line 226) | yes | yes | **missing** |
| `aw_np_clknode` (`aw_clk_np.c:77-97`, method table line 214) | yes | yes | **missing** |
| `aw_prediv_mux_clknode` (`aw_clk_prediv_mux.c`) | **no such method at all** | n/a | n/a — no gate concept in this class, correctly |
| `clknode_gate` (`clk_gate.c:62-68`) — generic, not Allwinner-specific | yes | n/a (always a real gate) | **present** (`clk_gate.c:100-116`) — reference implementation |

**7 of 9 classes are missing `get_gate`.** The one non-Allwinner class in this
neighborhood, `clknode_gate` (`clk_gate.c`), already implements it correctly —
that's the class used for every `CCU_GATE(...)`-table bus/module gate (e.g.
`bus-gpu`, `bus-mmc0`; registered via `aw_ccung_register_gates()`,
`aw_ccung.c:180-203`, using `clknode_gate_register()`). Those simple bus gates
were never affected by either bug in this audit — only the PLL-formula
classes (`frac`/`m`/`mipi`/`nkmp`/`nm`/`nmm`/`np`) are.

`aw_clk_mipi.c` is a special case worth flagging on its own: its `set_gate`
has no `AW_CLK_HAS_GATE` guard (confirmed: no such check in
`aw_clk_mipi.c:87-110`), and the `MIPI_CLK` macro (`aw_clk.h:529-553`) doesn't
even have a `_flags` parameter to set one — every `MIPI_CLK`-declared clock's
`.flags` is implicitly 0. So this class was never at risk of the §2 bug (its
gate is unconditional), but a `get_gate` for it must **not** add a flag guard
the class's own `set_gate` doesn't have, or it would misreport every MIPI
clock as gate-less. This class is used exactly once in the whole tree —
`pll_mipi_clk` in `ccu_a64.c:422-429`.

## 4. Reachability

For each affected clock: does anything in this tree actually call
`clk_enable()`/`clknode_enable()` on it (directly, or on a child clock whose
enable recurses to it), and is it plausible that U-Boot/BROM leaves it running
regardless?

**Verified reachable, with a real consumer:**

- **`pll_gpu` (A64).** This is the clock the bug was originally found on, and I
  can trace the exact path in this checkout:
  - `sys/contrib/device-tree/src/arm64/allwinner/sun50i-a64.dtsi:1140-1159`
    — the `mali:` node (`compatible = "allwinner,sun50i-a64-mali",
    "arm,mali-400"`) has `clocks = <&ccu CLK_BUS_GPU>, <&ccu CLK_GPU>;
    clock-names = "bus", "core";`.
  - `/opt/bzdos/bsdOS/hal/lima/lima_device.c:338-358` (`lima_clk_init`) does
    `devm_clk_get(dev->dev, "bus")` and `devm_clk_get(dev->dev, "core")`.
  - `lima_device.c:235-303` (`lima_clk_enable`) calls
    `clk_prepare_enable(dev->clk_gpu)` — the "core" clock, i.e. `ccu_a64.c`'s
    `gpu_clk` (`M_CLK`, parent `"pll_gpu"`, `ccu_a64.c:745-751`).
  - `clknode_enable()` (`clk.c:1091`) walks to the parent, reaching
    `pll_gpu_clk` and calling `aw_clk_frac_set_gate()` on it — which, before
    the in-flight fix, no-ops.
  - This project's own `/opt/bzdos/bsdOS/hal/lima/lima_ccu_debug.c:120-157`
    independently diagnosed this exact bug (same file, same flag, same fix)
    before I started, and ships a stopgap (`lima_ccu_force_pll_gpu()`, opt-in
    via `hw.lima.force_pll_gpu=1`) specifically because `clk(9)` couldn't do
    it. That comment ends: *"THE REAL FIX is one word in ccu_a64.c — add
    AW_CLK_HAS_GATE to pll_gpu_clk's flags"* — which is exactly what the
    concurrent edit to `ccu_a64.c` does.
  - I found no display/TCON/HDMI/DE driver anywhere in
    `sys/arm/allwinner` or `sys/arm64` (checked by name and by
    `clk_get_by_ofw_name` call sites) — GPU is the only in-tree-adjacent
    consumer for any of this SoC's fractional PLLs.

- **`pll_de` (A64).** Reachable directly inside the driver itself, no external
  consumer needed: `ccu_a64.c:808-814`'s own `a64_init_clks[]` table has
  `{"pll_de", NULL, 432000000, true}` and `{"de", "pll_de", 0, true}` — both
  `.enable = true`. `aw_ccung_init_clocks()` (`aw_ccung.c:205-260`) calls
  `clknode_enable()` on both, unconditionally, every time this driver attaches
  — i.e. on every A64 board. This call is being made, and silently no-opping,
  on real hardware today.

**Plausible by structural analogy, not directly observed here:**

- **`pll_gpu` (H3).** `sys/contrib/device-tree/src/arm/allwinner/sun8i-h3.dtsi:208-227`
  has the identical `mali:` node shape (`clocks = <&ccu CLK_BUS_GPU>, <&ccu
  CLK_GPU>; clock-names = "bus", "core";`), and H3's `CLK_GPU` (`gpu_clk`,
  `NM_CLK`, `ccu_h3.c:670-678`) also parents on the broken `pll_gpu_clk`
  (`ccu_h3.c:357-367`). Same mechanism, same result — but I found nothing in
  this repo currently wiring `hal/lima` to an H3 board (bsdOS's documented
  targets are A64-based), so this is "the same latent bug would fire the same
  way," not something I observed being exercised.

**Not established — I looked and found no consumer:**

- **`pll_gpu` (A31).** `sys/contrib/device-tree/src/arm/allwinner/sun6i-a31.dtsi`
  has no `gpu`/`mali` node at all in this tree (checked by grep for both
  words). A31 does have a Mali-400MP2 in real hardware, but I found nothing in
  this FreeBSD checkout that would call `clk_enable` toward it. Also worth
  noting: on A31, the GPU's own module-level dividers (`gpu_core_clk`,
  `gpu_memory_clk`, `gpu_hyd_clk`, `ccu_a31.c:836-861`) are built with
  `PREDIV_CLK`, a class with **no gate concept at all** (§3) — unlike A64/H3
  where the module stage (`gpu_clk`) has its own correctly-flagged gate. So on
  A31 there isn't even a second gate stage to mask the missing PLL gate with;
  if something did enable the GPU there, it would be exposed to
  `pll_gpu`'s state immediately. I couldn't determine whether that makes it
  more or less likely to have been noticed — I have no evidence either way.
- **`pll_video0`, `pll_ve`, `pll_video1`, `pll_hsic`** (A64); **`pll_video`,
  `pll_ve`** (H3, beyond the already-covered `pll_gpu`/`pll_de`);
  **`pll_video0`, `pll_ve`, `pll_video1`, `pll9`, `pll10`** (A31). I found no
  in-tree FreeBSD driver calling `clk_enable`/`clk_get_by_ofw_name` toward any
  of these by name, and no display/DE/TCON/HDMI driver exists in this tree at
  all for any Allwinner SoC (checked by filename and by grep across
  `sys/arm/allwinner` and `sys/arm64`). `ccu_a64.c`'s own comment
  (lines 384-408) claims *"U-Boot leaves the video/de PLLs running"* as the
  reason this hasn't been noticed — **I have not verified that claim against
  any actual U-Boot source**; no U-Boot tree was in scope for this task, and
  I'm stating explicitly that this part is inference carried over from that
  comment, not something I independently confirmed. It is a plausible
  explanation (bootloaders commonly enable display PLLs for splash/console
  output, never GPU or USB-HSIC PLLs), but that's as far as I can take it from
  this repo alone.

## 5. Bonus finding: a real wrong-bit gate bug, found while checking argument
   positions (`ccu_a83t.c`)

This is not the bug the task asked me to catalogue (a missing flag) — it's a
different bug in the same family (a PLL that silently doesn't gate right),
found because the task asked me to verify macro-argument positions rather than
trust the source comments next to them.

`ccu_a83t.c:206-216` (`pll_c0cpux_clk`) and `ccu_a83t.c:217-227`
(`pll_c1cpux_clk`, identical shape) read, as source text:

```c
NKMP_CLK(pll_c0cpux_clk, ...
    0, 0, 1, AW_CLK_FACTOR_FIXED,		/* p factor (fake) */
    0, 0,					/* lock */
    31,						/* gate */
    AW_CLK_HAS_GATE | AW_CLK_SCALE_CHANGE);	/* flags */
```

`NKMP_CLK`'s actual parameter order (`aw_clk.h:195-232`) after the four factor
blocks is `_gate, _lock, _lock_retries, _flags` — gate **first**, then the two
lock values. Here the source has the "lock" line *before* the "gate" line, so
positionally: `_gate = 0`, `_lock = 0`, `_lock_retries = 31`, and only
`_flags` lands correctly. Every *other* `NKMP_CLK` in this same file —
`pll_audio_clk` right below it, `pll_video0_clk`, `pll_ve_clk`, `pll_ddr_clk`,
`pll_periph_clk`, `pll_gpu_clk`, `pll_hsic_clk`, `pll_de_clk`,
`pll_video1_clk` — has "gate" *before* "lock", matching the macro, and all of
them correctly end up with `gate_shift = 31`.

I verified this is a real compiled effect, not a misreading on my part, by
building the exact argument list against the genuine `aw_clk.h` (only the
struct/type declarations it depends on were stubbed; the macro itself is the
real one from this tree):

```
pll_c0cpux_clk: gate_shift=0 lock_shift=0 lock_retries=31 flags=0x11 (HAS_GATE=1 HAS_LOCK=0)
pll_audio_clk (control): gate_shift=31 lock_shift=0 lock_retries=0 flags=0x1 (HAS_GATE=1 HAS_LOCK=0)
```

So `AW_CLK_HAS_GATE` **is** set for `pll_c0cpux_clk`/`pll_c1cpux_clk` (this is
not an instance of the §2 bug), but `aw_clk_nkmp_set_gate()` will toggle bit 0
of `PLL_C0CPUX_CTRL`/`PLL_C1CPUX_CTRL` (offsets `0x00`/`0x04`) instead of bit
31. Every other `PLLx_CTRL` register in all 11 files in this tree uses bit 31
for enable and bit 28 for lock, without exception, which is what makes me
confident 31 was intended here too — but I have not checked the A83T's
register reference manual, so I can't rule out that bit 0 happens to be
something else specific to this register. What I *can* say from the code
alone: bit 31 (the bit every sibling PLL uses, and the bit the source comment
here claims is being set) is never touched by this call, on either of the
A83T's two CPU cluster PLLs. This is out of scope for the accompanying patch
(it's not a `get_gate` gap, and fixing it means changing argument order in
`ccu_a83t.c`, which the patch is not supposed to touch) — flagging it here so
it isn't lost.

## 6. Summary of what's verified vs inferred

**Verified by reading code / compiling against the real headers** (not
guessed): every row in the §2 and §3 tables; the `clknode_if.m` / `clk.c`
get_gate contract and its `ENXIO` → `"unimplemented"` sysctl path; the
`pll_gpu` (A64) and `pll_de` (A64) reachability chains, including the DT node
contents and the `hal/lima` source that already hit this bug; the `ccu_a83t.c`
gate/lock argument-order bug (confirmed by compiling the real macro).

**Inference, explicitly not independently verified here:** the claim that
U-Boot leaves the video/DE PLLs running (carried over from `ccu_a64.c`'s own
comment — I did not check U-Boot source); that H3's identical DT shape means
the GPU bug would fire the same way there if `hal/lima` were pointed at an H3
board (structurally sound, but not something I saw exercised); that A83T's PLL
register bit 0 is inert or harmless rather than a meaningful field (I don't
have the register manual).

**Could not determine:** whether any of the `gate_shift = 0` /
no-`AW_CLK_HAS_GATE` clocks (listed in §2) are a real missed gate rather than
a deliberately gate-less clock — needs the per-SoC register manual, not just
this source tree. Whether `pll_gpu` on A31 is reachable through anything at
all — I found no consumer in this repo, but that only means I didn't find one,
not that none could exist on real A31 hardware/firmware.
