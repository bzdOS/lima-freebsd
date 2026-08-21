# ccu_a64.c/ccu_h3.c: FRAC_CLK PLLs declare a gate bit but omit AW_CLK_HAS_GATE, so clk_enable() silently no-ops (PLL_GPU: an unpowered GPU MMIO read wedges the SoC)

## Summary (suggested title)

`sys/dev/clk/allwinner/ccu_a64.c`: `pll_gpu_clk` (and five sibling `FRAC_CLK`
PLLs in the same file, plus four in `ccu_h3.c`) declare a gate bit in their
register layout but never set `AW_CLK_HAS_GATE` in their flags, so
`clk_enable()` on any consumer returns success without ever touching the PLL's
enable bit. For `PLL_GPU` specifically this leaves a GPU's core clock and AHB
gate open onto a PLL that is off and unlocked; the first MMIO read the GPU
IP needs to complete never returns, and the failure has no panic and no
backtrace — it looks exactly like a hardware fault.

## Affected

- **FreeBSD 15.1**, `__FreeBSD_version` 1501000. Verified in
  `sys/sys/param.h:77` of this project's FreeBSD source tree (a git worktree
  at commit `9263fb9bab261980c57c38d55bd4d8f2e1746992`, commit message "15.1:
  Update to RC2", 2026-06-06). `param.h` was not among this tree's
  locally-modified files, so this is the committed, pinned value, not a local
  edit.
- Independently confirmed against the *canonical* FreeBSD release, not just
  this project's checkout: `sys/dev/clk/allwinner/ccu_a64.c`,
  `aw_clk_frac.c`, `aw_clk_frac.h`, `aw_clk_m.c`, `ccu_h3.c`, `sys/dev/clk/clk.c`
  and `clk.h` are **byte-for-byte identical** between that RC2 commit and the
  official tag `release/15.1.0` (commit `96841ea08dcfa84b954a32dc5ae1a26c28966cf4`,
  tagged 2026-06-12 by Colin Percival) — diffed directly, zero drift in any of
  these files between RC2 and the final release.
- Files: `sys/dev/clk/allwinner/ccu_a64.c` (`pll_gpu_clk` and five sibling
  `FRAC_CLK`s), `sys/dev/clk/allwinner/ccu_h3.c` (four more `FRAC_CLK`s, same
  pattern), `sys/dev/clk/allwinner/aw_clk_frac.c` (`aw_clk_frac_set_gate`),
  `sys/dev/clk/allwinner/aw_clk.h` (flag values, `FRAC_CLK` macro),
  `sys/dev/clk/allwinner/aw_clk_m.c` (shares the `get_gate` gap, see below),
  `sys/dev/clk/clk.c` (`clknode_enable`/`clk_enable`, generic and correct).
- Hardware: Banana Pi M64 (Allwinner A64), Mali-400 MP2 r1p1, FreeBSD 15.1
  guest, out-of-tree `lima` KMS driver (drm-kmod).

Permalinks (pinned to `release/15.1.0`, proven identical to what was actually
run):
- `https://github.com/freebsd/freebsd-src/blob/96841ea08dcfa84b954a32dc5ae1a26c28966cf4/sys/dev/clk/allwinner/ccu_a64.c#L383-L394`
- `https://github.com/freebsd/freebsd-src/blob/96841ea08dcfa84b954a32dc5ae1a26c28966cf4/sys/dev/clk/allwinner/aw_clk_frac.c#L98-L120`
- `https://github.com/freebsd/freebsd-src/blob/96841ea08dcfa84b954a32dc5ae1a26c28966cf4/sys/dev/clk/clk.c#L1073-L1100`
- `https://github.com/freebsd/freebsd-src/blob/96841ea08dcfa84b954a32dc5ae1a26c28966cf4/sys/dev/clk/allwinner/aw_clk.h#L318-L352`

## Mechanism

`FRAC_CLK()` (`sys/dev/clk/allwinner/aw_clk.h:318-352`) maps its positional
arguments straight into `struct aw_clk_frac_def`
(`sys/dev/clk/allwinner/aw_clk_frac.h:31-49`):

```c
#define FRAC_CLK(_clkname, _id, _name, _pnames,	\
     _offset,						\
     _nshift, _nwidth, _nvalue, _nflags,		\
     _mshift, _mwidth, _mvalue, _mflags,		\
     _gate_shift, _lock_shift,_lock_retries,		\
    _flags, _freq0, _freq1, _mode_sel, _freq_sel,	\
    _min_freq, _max_freq)				\
	static struct aw_clk_frac_def _clkname = {	\
		...					\
		.gate_shift = _gate_shift,		\
		.lock_shift = _lock_shift,		\
		.lock_retries = _lock_retries,		\
		.flags = _flags,			\
		...
```
(`aw_clk.h:318-345`)

`ccu_a64.c` declares `pll_gpu_clk` with this macro, `sys/dev/clk/allwinner/ccu_a64.c:383-394`:

```c
static const char *pll_gpu_parents[] = {"osc24M"};
FRAC_CLK(pll_gpu_clk,
    CLK_PLL_GPU,				/* id */
    "pll_gpu", pll_gpu_parents,			/* name, parents */
    0x38,					/* offset */
    8, 7, 0, 0,					/* n factor */
    0, 4, 0, 0,					/* m factor */
    31, 28, 1000,				/* gate, lock, lock retries */
    AW_CLK_HAS_LOCK,				/* flags */
    270000000, 297000000,			/* freq0, freq1 */
    24, 25,					/* mode sel, freq sel */
    192000000, 600000000);			/* min freq, max freq */
```

`gate_shift = 31` **is** supplied — the comment on that line even labels it —
but `flags` (line 391) is `AW_CLK_HAS_LOCK` alone. `AW_CLK_HAS_GATE` (value
`0x0001`) and `AW_CLK_HAS_LOCK` (value `0x0002`) are separate bits
(`aw_clk.h:59-60`); only the lock bit is requested.

`aw_clk_frac_set_gate()` is the `clknode_set_gate` method for every `FRAC_CLK`
(registered at `aw_clk_frac.c:334-341`, `DEFINE_CLASS_1(aw_frac_clknode, ...)`
at line 344). It opens (`sys/dev/clk/allwinner/aw_clk_frac.c:98-107`):

```c
static int
aw_clk_frac_set_gate(struct clknode *clk, bool enable)
{
	struct aw_clk_frac_sc *sc;
	uint32_t val;

	sc = clknode_get_softc(clk);

	if ((sc->flags & AW_CLK_HAS_GATE) == 0)
		return (0);
	...
```

— returns success without reading or writing any register, for *either*
`enable` value, before the rest of the function (the actual
read-modify-write) is reached. This is not a race or a corner case; it is the
first thing the function does, on every call, for as long as the flag is
absent.

clk(9)'s generic enable path does the right thing on its own end.
`clknode_enable()` (`sys/dev/clk/clk.c:1073-1100`) recurses to the parent
*before* gating the node itself:

```c
int
clknode_enable(struct clknode *clknode)
{
	int rv;

	CLK_TOPO_ASSERT();

	/* Enable clock for each node in chain, starting from source. */
	if (clknode->parent_cnt > 0) {
		rv = clknode_enable(clknode->parent);
		if (rv != 0) {
			return (rv);
		}
	}

	/* Handle this node */
	CLKNODE_XLOCK(clknode);
	if (clknode->enable_cnt == 0) {
		rv = CLKNODE_SET_GATE(clknode, 1);
		if (rv != 0) {
			CLKNODE_UNLOCK(clknode);
			return (rv);
		}
	}
	clknode->enable_cnt++;
	CLKNODE_UNLOCK(clknode);
	return (0);
}
```

So enabling the GPU's core clock ("gpu", a correctly-flagged `M_CLK` at
`ccu_a64.c:720-726`, offset `0x1A0`, `AW_CLK_HAS_GATE` set) really does walk up
to its parent `pll_gpu` and call `CLKNODE_SET_GATE()` on it (line 1091) — the
call happens exactly as designed. It's `aw_clk_frac_set_gate()`'s own early
return (above) that turns that call into a no-op, and `clk_enable()`
(`clk.c:1271-1286`) just forwards the resulting `0` upward with no indication
anything was skipped.

**Hardware consequence.** With the core clock's own gate correctly opened
(`M_CLK` at offset `0x1A0`), the AHB bus gate correctly opened (a plain
`CCU_GATE(CLK_BUS_GPU, "bus-gpu", "ahb1", 0x64, 20)` — `ccu_a64.c:167`, a
different, unaffected gate mechanism), and the GPU out of reset
(`CCU_RESET(RST_BUS_GPU, 0x2c4, 20)` — `ccu_a64.c:115`), everything *looks*
correctly clocked from clk(9)'s bookkeeping — but the PLL feeding all of that
never actually turned on. Register addresses below are the CCU's own base
(`0x01c20000`, confirmed in `sys/contrib/device-tree/src/arm64/allwinner/sun50i-a64.dtsi:699`)
plus each clock's declared `offset`:

| register | address (base + offset) | measured | meaning |
|---|---|---|---|
| `PLL_GPU_CTRL` | `0x01c20000 + 0x38` | `0x03006207`, bit31(en)=0, bit28(lock)=0 | PLL off, unlocked |
| `GPU_CLK_REG` | `0x01c20000 + 0x1A0` | `0x80000000`, bit31=1 | core-clock gate open |
| `BUS_CLK_GATING_REG1` | `0x01c20000 + 0x64` | bit20=1 | AHB "bus-gpu" gate open |
| `BUS_SOFT_RST_REG1` | `0x01c20000 + 0x2c4` | bit20=1 | GPU reset deasserted |

A write into the Mali IP's MMIO window posts and retires without needing a
response, so it "succeeds" either way; a read has to wait for one. Out of
reset, with a real bus path open to a block whose own clock never started,
the read blocks forever, and — measured on this Banana Pi M64 — takes the
whole SoC interconnect down with it: no panic, no backtrace (the CPU cannot
retire past the stalled load to reach a trap handler), console output stops
mid-line, and the only recovery is an external/watchdog reset. (On this
project's own rig that reset is delivered by a separate always-alive core
petting a hardware watchdog; on plain bare-metal FreeBSD without that
infrastructure the expected symptom is simply an unresponsive board recoverable
only by an external power-cycle or a configured watchdog. I have not
reproduced the bare-metal form myself — see "What was verified vs. taken on
trust" below.)

**Scope of the omission.** Every `FRAC_CLK`-declared clock in both files has
this exact gap, and — checked explicitly, this is not incidental — every
*other*-macro clock (`NKMP_CLK`, `NKMP_CLK_WITH_UPDATE`, `M_CLK`, `NM_CLK`,
`FIXED_CLK`) that declares a gate sets `AW_CLK_HAS_GATE` correctly:

- `ccu_a64.c`: `pll_video0` (278-285), `pll_ve` (300-307), `pll_video1`
  (371-378), `pll_gpu` (384-391), `pll_hsic` (407-414), `pll_de` (420-427) —
  6 `FRAC_CLK`s, 6 missing the flag. `pll_cpux`, `pll_audio`, `pll_ddr0`,
  `pll_periph0_2x`, `pll_periph1_2x`, `pll_ddr1` all use `NKMP_CLK`/
  `NKMP_CLK_WITH_UPDATE` instead and all correctly carry `AW_CLK_HAS_GATE`.
- `ccu_h3.c`: `pll_video` (295-302), `pll_ve` (308-315), `pll_gpu` (357-364),
  `pll_de` (383-390) — 4 `FRAC_CLK`s, 4 missing the flag; `pll_cpux`,
  `pll_audio`, `pll_ddr`, `pll_periph0`, `pll_periph1` (all `NKMP_CLK`-family)
  again correctly carry it.

**The omission is not limited to these two files.** A separate, more
exhaustive audit of every `ccu_*.c`/`aw_clk_*.c` file under
`sys/dev/clk/allwinner/` in this same tree
(`freebsd-allwinner-clk-gate-audit.md` in this repository)
found the identical pattern in a third file, `ccu_a31.c`: six more `FRAC_CLK`s
missing `AW_CLK_HAS_GATE`, including A31's own `pll_gpu_clk`
(`ccu_a31.c:381-391`) — the A31 also ships a real Mali-400MP2. I spot-checked
this myself rather than taking it on trust, since it directly bears on this
report's scope: `ccu_a31.c` is unmodified in this tree (confirmed via `git
status`, so this is the pristine 15.1 declaration, not a local edit) and reads,
at 381-391:

```c
FRAC_CLK(pll_gpu_clk,
    CLK_PLL_GPU,				/* id */
    "pll_gpu", pll_parents,		/* name, parents */
    0x38,					/* offset */
    8, 7, 0, 0,					/* n factor */
    0, 4, 0, 0,					/* m factor */
    31, 28, 1000,				/* gate, lock, lock retries */
    AW_CLK_HAS_LOCK,				/* flags */
    270000000, 297000000,			/* freq0, freq1 */
    24, 25,					/* mode sel, freq sel */
    30000000, 600000000);			/* min freq, max freq */
```

— byte-for-byte the same shape as `ccu_a64.c`'s. I also confirmed that
tree's other claim that makes this latent rather than already-reported: A31's
device-tree source in this checkout
(`sys/contrib/device-tree/src/arm/allwinner/sun6i-a31.dtsi`) has no `mali` or
`gpu` node at all (`grep -i` for both words: no match) — so on A31, unlike A64,
there isn't even a DT-level path to a GPU clock consumer in this tree, which is
consistent with, and slightly stronger than, this report's "no in-tree
consumer" reasoning below. I did not re-verify that audit's remaining claims
(H6/A83T/etc.) myself; they are not needed for this report's scope and are
called out here only because they change "how many places have this bug",
which matters for anyone deciding how broadly to fix it upstream.

## Reproduction

No in-tree FreeBSD driver currently looks up `pll_gpu`/`gpu` at all — grepping
all of `sys/` outside the Allwinner clock drivers and the device-tree source
themselves finds zero hits; the `gpu@...` node in
`sun50i-a64.dtsi`/`sun50i-h5.dtsi`/`sun50i-h6.dtsi` exists in the DT but
nothing in FreeBSD's tree binds a driver to it. So the sequence that hits this
needs a driver FreeBSD doesn't ship:

1. On an Allwinner A64 or H3 board, attach any driver — in this case an
   out-of-tree `lima` (Mali-400) build — that calls `clk_enable()` on a clock
   whose parent chain includes `pll_gpu`, then issues an MMIO read to hardware
   clocked exclusively by that PLL, while the PLL was not already running.
2. `clk_enable()` returns 0. The read wedges the SoC as described above.

### Why this is not normally hit

FreeBSD has no in-tree GPU driver for these SoCs (no Mali/Lima support
upstream), so `pll_gpu`/`gpu` are the only clk(9) nodes in either file with
*zero* in-tree consumers, full stop — this was checked, not assumed. The other
five affected `FRAC_CLK`s on A64 (`pll_video0`, `pll_ve`, `pll_video1`,
`pll_hsic`, `pll_de`) and three on H3 (`pll_video`, `pll_ve`, `pll_de`) have
the identical code-level bug but don't surface it, for a reason this report
did **not** independently verify against U-Boot source (no U-Boot tree was
available in this pass; this is carried over from this project's own prior
analysis): U-Boot is understood to leave the display/video PLLs already
running before FreeBSD boots, so FreeBSD's `clk_enable()` on them is called on
an already-locked PLL — a real no-op is indistinguishable from the buggy one
— or a consuming driver never calls `clk_enable()` on them at all if it only
reads frequency. `PLL_GPU` is the one FRAC_CLK PLL on these SoCs that nothing
starts before FreeBSD and that only a GPU driver would ever ask clk(9) to
start from cold — which is also exactly the class of driver FreeBSD doesn't
ship yet.

## Observed symptom

Measured, from inside the driver at the moment it matters (this project's own
hardware run; not re-measured in this pass — no board access):

```
after-core: PLL_GPU=0x03006207 en=0 lock=0    <- PLL off, not locked
            GPU_CLK=0x80000000  gate=1        <- core-clock gate open
            BUS_GATE1=0x00100001 bus_gpu=1    <- AHB gate open
```

Effect: two different MMIO reads into the Mali IP window were observed to
hang this way — the PMU block at `+0x2000` and the GP MMU block at `+0x3000`
(both inside the `gpu@1c40000` window). No panic, no backtrace; console output
stops mid-line (e.g. `mmu gpmmu: DTE write returned, reading back` with
nothing after); the board becomes fully unresponsive until an external
watchdog reset.

**Confirmation, not just a consistent story:** setting bit 31 of `PLL_GPU_CTRL`
by hand locks the PLL in ~230 µs (23 polls) and the identical driver load then
attaches cleanly on the same board — recorded the same day as this report in
this project's own status notes (`../docs/MALI-STATUS.md`, "RESOLVED: the GPU
is up"), including a full successful Mali-400 MP2 r1p1 attach
(`[drm] Initialized lima 1.1.0 20191231 for lima_platform_driver0 on minor 0`)
gated behind an opt-in `kenv hw.lima.force_pll_gpu=1` workaround pending the
real kernel fix. This is this project's result, not something re-run during
this verification pass.

## Proposed fix

Add `AW_CLK_HAS_GATE` to `pll_gpu_clk`'s flags in `ccu_a64.c` only:

```c
-    AW_CLK_HAS_LOCK,				/* flags */
+    AW_CLK_HAS_LOCK | AW_CLK_HAS_GATE,		/* flags */
```

(The identical `pll_gpu_clk` declaration in `ccu_h3.c` has the identical bug
by inspection, but was not exercised on real H3 hardware in this
investigation, so this report scopes its verified recommendation to
`ccu_a64.c`; the H3 case should get the same fix once someone can validate it
there.)

**Explicitly not proposed:** adding `AW_CLK_HAS_GATE` to the other five
affected `FRAC_CLK`s in `ccu_a64.c` (`pll_video0`, `pll_ve`, `pll_video1`,
`pll_hsic`, `pll_de`) or the other three in `ccu_h3.c` (`pll_video`, `pll_ve`,
`pll_de`), even though they have the byte-identical omission. This is a
deliberate scoping decision, not an oversight, and the reasoning is structural,
not just cautious: `AW_CLK_HAS_GATE` is checked once, near the top of
`aw_clk_frac_set_gate(struct clknode *clk, bool enable)`, *before* the
function branches on `enable` — the same flag gates both `clk_enable()` and
`clk_disable()` for a given node. Today, `clk_disable()` on every one of these
PLLs is exactly as much of a no-op as `clk_enable()` is — silently, for the
same reason. Adding the flag makes both directions live at once. `pll_video0`
and `pll_de` feed the display pipeline; something that calls
`clk_disable(pll_video0)` or `clk_disable(pll_de)` today, believing it does
nothing (because today it truly does nothing), would newly and actually cut a
clock the display path depends on. `pll_gpu` is safe to change alone precisely
because — established above — nothing in FreeBSD's tree currently calls
`clk_enable()` *or* `clk_disable()` on it at all: there is no existing call
site for the fix to newly activate and break.

### Risk

Very low for the scoped fix: it changes behavior only for a clock node with no
current in-tree consumer. The only code that will ever observe the
difference is a future (or out-of-tree, as here) GPU driver — exactly the
caller that needs `clk_enable()` to be truthful. No existing driver, in-tree
or not, is known to reference `pll_gpu` or `gpu` today.

## Related, worth fixing separately (lower priority, same root-cause class)

`aw_clk_frac.c` and `aw_clk_m.c` implement `clknode_set_gate` but neither
registers a `clknode_get_gate` method — checked directly in both
`clknode_method_t[]` tables (`aw_clk_frac.c:334-341`; the equivalent table in
`aw_clk_m.c:241-248`), neither has a `CLKNODEMETHOD(clknode_get_gate, ...)`
entry. The generic clk(9) sysctl handler (`sys/dev/clk/clk.c:1701-1706`) does:

```c
	case CLKNODE_SYSCTL_GATE:
		ret = CLKNODE_GET_GATE(clknode, &enable);
		if (ret == 0)
			sbuf_printf(sb, enable ? "enabled": "disabled");
		else if (ret == ENXIO)
			sbuf_printf(sb, "unimplemented");
```

and kobj's own default for any method a class doesn't override
(`sys/kern/subr_kobj.c:90-95`, `kobj_error_method()`) returns exactly `ENXIO`.
So `sysctl hw.clock.<name>.gate` reads `unimplemented` for *every* clock of
either class — including `pll_gpu` itself, both before and after the proposed
fix — regardless of whether `AW_CLK_HAS_GATE` is set. This is a real,
separate, fully independent bug (other clk(9) backends, e.g.
`clk_gate.c` and both Tegra PLL drivers, do implement `get_gate`), and it
actively misleads: "unimplemented" reads as "this clock class has no gate
concept," when the true meaning is "this class cannot read its gate state
back." That misreading cost real debugging time during this project's own
investigation before the register-level read settled it.

The same audit referenced above found this `get_gate` gap in five more
Allwinner clknode classes beyond `aw_clk_frac`/`aw_clk_m`
(`aw_clk_mipi`, `aw_clk_nkmp`, `aw_clk_nm`, `aw_clk_nmm`, `aw_clk_np` — seven
classes total; two other classes in the same directory, `aw_clk_prediv_mux`
and the generic non-Allwinner `clknode_gate`, are correctly unaffected: the
former has no gate concept at all, the latter already implements `get_gate`)
and drafted a patch for all seven,
`hal/lima/patches/freebsd-src/freebsd-allwinner-clk-get-gate.patch` in this repository. I
read that patch; its shape for the two classes this report already traced
(`aw_clk_frac`, `aw_clk_m`) matches exactly what tracing `aw_clk_frac_set_gate()`
and the `clknode_if.m`/`kobj_error_method` chain above would predict a correct
`get_gate` should look like (guard on `AW_CLK_HAS_GATE`, return `ENOENT` if
absent — the contract documented at `clknode_if.m:73-77` for "hardware doesn't
support reading gate enable" — otherwise read the same register `set_gate`
writes and report the bit). I did not independently re-verify its other five
classes' register layouts line by line.

## What was verified vs. taken on trust

**Verified by reading source in this pass**, against this project's FreeBSD
tree and cross-checked byte-for-byte against the official `release/15.1.0`
tag fetched fresh from `github.com/freebsd/freebsd-src`:
- Every file:line citation and quoted code block above, including the full
  macro-argument-to-struct-field mapping for `FRAC_CLK`, the exact deciding
  lines in `aw_clk_frac_set_gate()`, the parent-recursion in
  `clknode_enable()`, and the complete `get_gate`→`ENXIO`→`"unimplemented"`
  chain down to kobj's default method.
- That `ccu_a64.c`/`aw_clk_frac.c`/`aw_clk_frac.h`/`aw_clk_m.c`/`ccu_h3.c`/`clk.c`/`clk.h`
  are byte-identical between this project's pinned RC2 commit and the
  official `release/15.1.0` tag (direct diff of fetched blobs).
- The complete classification of every `FRAC_CLK`/`NKMP_CLK`/`M_CLK`/`NM_CLK`/`FIXED_CLK`
  invocation in both `ccu_a64.c` and `ccu_h3.c` by which macro declares it and
  whether `AW_CLK_HAS_GATE` is set — this is what turned "every FRAC_CLK...
  shares the omission" from a claim that looked like it might be an
  overstatement into one that is exactly, literally true (100% of `FRAC_CLK`
  invocations in both files lack the flag; 100% of gate-declaring invocations
  of every *other* macro in the same two files have it).
- The register offsets and bit positions in the table above, against
  `ccu_a64.c`'s own `FRAC_CLK`/`M_CLK`/`CCU_GATE`/`CCU_RESET` declarations and
  the CCU's base address in `sun50i-a64.dtsi`.
- That no in-tree FreeBSD driver references a `pll_gpu` or plain `"gpu"` clock
  anywhere outside the Allwinner clock drivers and device-tree sources
  themselves (targeted grep of `sys/`, not necessarily exhaustive of every
  possible spelling).
- That this project's local tree has an *uncommitted* one-line diff at
  `ccu_a64.c:391` that is precisely the proposed fix — i.e. it is already
  drafted and (per the project's own same-day status notes) hardware-tested
  here, just not yet built into a committed change or sent upstream.
- Spot-checked (not blindly relayed): the two most consequential claims from
  the separate whole-tree audit cited above — that `ccu_a31.c:381-391` has the
  byte-identical `pll_gpu_clk` omission, and that this tree's A31 device tree
  has no `mali`/`gpu` node at all. Both confirmed directly. I did not
  independently re-run that audit's full sweep of all 11 `ccu_*.c` files and 9
  clknode classes, or its `ccu_a83t.c` wrong-argument-position finding (§5 of
  that document, a different bug in the same family) — I read it and it is
  internally consistent with everything I verified myself, but I have not
  personally re-derived it.

**Not independently verified in this pass** (relied on this project's own
prior analysis/measurements, since board access and U-Boot source were both
out of scope for this task):
- U-Boot's boot-time PLL state (the claim that it leaves
  video/de PLLs running) — I have no U-Boot source in this environment and
  did not check it.
- The register values under "Observed symptom" and the "attaches cleanly
  after setting bit 31 by hand" result — consistent with, and fully explained
  by, the source-level mechanism traced above, but not re-measured on
  hardware by me.
- Whether the exact same MMIO-read hang manifests identically on plain
  bare-metal FreeBSD (no hypervisor, no separate watchdog-petting core) —
  the underlying bus-stall mechanism is a hardware property independent of
  this project's specific rig, but I have not seen it happen outside that rig.

No factual error was found in how this bug was described to me. The one claim
that looked at first pass like it might be an overstatement — "every FRAC_CLK
in ccu_a64.c and ccu_h3.c shares the omission" — turned out, once every macro
invocation in both files was individually classified, to be exactly correct.

The one respect in which the description was *incomplete* rather than wrong:
it named only `ccu_a64.c` and `ccu_h3.c`. The bug also exists, independently
confirmed above, in `ccu_a31.c` — a third SoC family with a real Mali-400MP2
GPU in hardware, currently unexercised in this tree only because its own
device tree has no GPU node at all. Anyone taking this upstream should decide
up front whether to file it as one A64-scoped report (matching what was
hardware-verified) or a broader one covering all three files (matching the
full extent of the bug) — this report is written as the former.
