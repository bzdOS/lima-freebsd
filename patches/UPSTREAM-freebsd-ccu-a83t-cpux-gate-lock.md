# Upstream report: ccu_a83t.c passes NKMP_CLK()'s gate/lock arguments transposed

**Status:** analysis complete, patch written, **not submitted**. Submission needs
the maintainer-facing account and should go out under the author's own name.

**Target:** `freebsd-src`, `sys/dev/clk/allwinner/ccu_a83t.c`
**Affects:** every Allwinner A83T board — the CPUX cluster PLLs
**Severity:** enabling or disabling either CPUX PLL writes the wrong register bit
**Hardware tested:** none. See "Confidence" below — the evidence is the macro
signature plus the file's own internal inconsistency, not a measurement.

## The defect

`NKMP_CLK()` (`sys/dev/clk/allwinner/aw_clk.h`) takes the gate shift *before* the
lock shift and lock retries:

```c
#define NKMP_CLK(_clkname, _id, _name, _pnames,		\
  _offset,						\
  _n_shift, _n_width, _n_value, _n_flags,		\
  _k_shift, _k_width, _k_value, _k_flags,		\
  _m_shift, _m_width, _m_value, _m_flags,		\
  _p_shift, _p_width, _p_value, _p_flags,		\
  _gate,						\
  _lock, _lock_retries,					\
  _flags)						\
	...
		.gate_shift = _gate,			\
		.lock_shift = _lock,			\
		.lock_retries = _lock_retries,		\
```

`pll_c0cpux_clk` and `pll_c1cpux_clk` passed them in the opposite order, matching
their own inline comments rather than the macro:

```c
    0, 0, 1, AW_CLK_FACTOR_FIXED,		/* p factor (fake) */
    0, 0,					/* lock */
    31,						/* gate */
    AW_CLK_HAS_GATE | AW_CLK_SCALE_CHANGE);	/* flags */
```

Positionally that is `_gate = 0`, `_lock = 0`, `_lock_retries = 31`. So:

- `gate_shift` becomes **0** instead of 31
- `lock_retries` becomes 31, which is meaningless here (no `AW_CLK_HAS_LOCK`)

`AW_CLK_HAS_GATE` **is** set, so `aw_clk_nkmp_set_gate()` does write a gate bit —
just bit 0 of `PLL_C0CPUX_CTRL_REG` / `PLL_C1CPUX_CTRL_REG` rather than bit 31.
The PLL enable is therefore never touched, and bit 0 is clobbered instead.

## Why this is unambiguous

The file contradicts itself. Every other `NKMP_CLK` entry in `ccu_a83t.c` already
passes gate before lock:

| line | order |
|---|---|
| 214–215 | `0, 0 /* lock */` then `31 /* gate */` — **wrong** (`pll_c0cpux`) |
| 225–226 | `0, 0 /* lock */` then `31 /* gate */` — **wrong** (`pll_c1cpux`) |
| 238–239 | `31 /* gate */` then `0, 0 /* lock */` — correct |
| 251–252 | `31 /* gate */` then `0, 0 /* lock */` — correct |
| 264–265 | `31 /* gate */` then `0, 0 /* lock */` — correct |
| 277–278 | `31 /* gate */` then `0, 0 /* lock */` — correct |

Two entries out of six disagree with the other four and with the macro. That is a
transcription slip, not a deliberate encoding.

## Patch

```diff
     0, 0, 1, AW_CLK_FACTOR_FIXED,		/* p factor (fake) */
-    0, 0,					/* lock */
-    31,						/* gate */
+    31,						/* gate */
+    0, 0,					/* lock */
     AW_CLK_HAS_GATE | AW_CLK_SCALE_CHANGE);	/* flags */
```

Applied to both `pll_c0cpux_clk` and `pll_c1cpux_clk`. All six entries then read
identically.

## Confidence, stated honestly

I have **no A83T hardware**, so this is not measured. What supports it:

1. The macro signature, quoted above from the same tree.
2. The four sibling entries in the same file using the opposite (correct) order.
3. Bit 31 being the PLL enable on these registers, consistent with every other
   Allwinner PLL definition in this directory.

What a maintainer with A83T hardware should check: read
`PLL_C0CPUX_CTRL_REG` (offset 0x00) before and after a
`clk_enable()`/`clk_disable()` on `pll_c0cpux`, and confirm bit 31 moves rather
than bit 0.

## How this was found

Chasing a *different* defect in the same directory: `ccu_a64.c` omits
`AW_CLK_HAS_GATE` on every `FRAC_CLK`, so `aw_clk_frac_set_gate()` early-returns
and `PLL_GPU` can never be enabled — which makes the Mali-400 on A64 boards
impossible to power up. That one **is** hardware-measured (see
`UPSTREAM-freebsd-ccu-a64-pll-gpu.md`). Auditing the neighbouring files for the
same class of gate-handling mistake surfaced this transposition, plus 15 further
clocks across a31/a64/h3 that omit `AW_CLK_HAS_GATE` (catalogued in
`freebsd-allwinner-clk-gate-audit.md`, deliberately untouched — those need
per-clock hardware verification that only their board owners can do).

Same family of bug, two different shapes: in `ccu_a64.c` the gate is never
written at all; here it is written to the wrong bit.

## Local commit

`68fe3114f` in the `freebsd-src-earlyboot-wt` worktree, message
"aw_ccung: fix transposed gate/lock args on A83T CPUX PLLs".
