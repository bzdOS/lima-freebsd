# tests/PP-CLEAR-FRAME.md — a minimal Mali-400 PP "clear only, no geometry" job

**Written:** 2026-08-11. Answers the gap `docs/PLAN-mesa-lima.md` §1.3 flagged explicitly:
*"Getting the actual bytes of that tile-command list right is real Utgard-format
work — I have not derived or verified exact opcode values here."* This document
derives them, states exactly where each value comes from, and states plainly the
one thing that is still a guess.

## 0. Bottom line, before the detail

- The **struct layout** of `drm_lima_m400_pp_frame` is settled beyond doubt (it's
  a byte-for-byte-identical UAPI header in four independent places — this repo,
  upstream Linux, this host's installed kernel headers, and it matches what a
  currently-shipping Mesa driver fills in).
- The **per-word meaning and the value each word needs for a minimal clear** are
  now backed by 2–4 independent, mutually-corroborating external sources for
  **21 of 23** frame words and all 12 write-back words actually used. Two words
  have a genuine, named, low-stakes disagreement between an old source and the
  current one (§3, words 10/11 and 16/17) — resolved by preferring the current
  Mesa driver, with the older value given as a fallback.
- The **tile-descriptor array** that `plbu_array_address` points to — the thing
  PLAN.md called unverified — has an exact, byte-identical format confirmed by
  **two independently-authored, hardware-verified drivers twelve years apart**
  (§5). This is no longer a guess.
- **One real gap remains, precisely scoped, not hand-waved:** whether the small
  per-tile *polygon-list* memory that the tile-descriptor array's addresses point
  *into* is safe to leave zero-filled when no GP job ever runs. No source found —
  not Limare, not Mesa, not ARM's own GPL driver — ever exercises a PP submission
  with **no preceding GP job at all**. Every real driver, including current Mesa,
  runs a GP pass first, even for a zero-draw clear (§6). This is the one place an
  honest "unverified guess" belongs, and it is named exactly, not vaguely.
- The kernel **validates none of the content** described here (§7). A malformed
  frame goes straight to hardware.

## 1. Provenance key

Every value below is tagged:

| Tag | Meaning |
|---|---|
| **[K]** | Verified by reading this repo's own kernel-side source |
| **[U]** | Verified against upstream Linux kernel source, fetched fresh this session |
| **[H]** | Verified against this host's installed `kernel-headers` package (independent copy) |
| **[A]** | Verified against ARM's own GPL "Utgard" kernel driver source (`DX910-SW-99002-r3p2-01rel2`) |
| **[L]** | Verified against the Limare project's source (Luc Verhaegen, 2011–2012) |
| **[M]** | Verified against Mesa's current `lima` Gallium driver source |
| **[G]** | My own synthesis, combining two or more tagged sources — derivation shown |
| **[X]** | Unverified guess — flagged explicitly, never silently assumed |

## 2. Sources actually fetched, and the routes that didn't work

Per the task's instruction to report routes tried:

- **`gitlab.freedesktop.org` — blocked.** Every attempt (direct `WebFetch`, and a
  direct `curl` from this sandbox) hits GitLab's Anubis anti-bot challenge or a
  connection-level failure. Confirmed again this session, matching what
  `docs/PLAN-mesa-lima.md` §2.1/§6 already reported.
- **`cgit.freedesktop.org` — also blocked**, but differently: `curl` gets a
  TLS-level `unexpected eof while reading` (connection dropped during the
  handshake), not an HTTP error. Tried fresh this session; not previously tried
  in `docs/PLAN-mesa-lima.md`.
- **GitHub mirrors — worked, for everything.** `raw.githubusercontent.com` and
  `api.github.com` (for directory listings via the git-trees/contents API) were
  reachable by plain `curl` from this sandbox with no proxy tricks needed. Used:
  - `github.com/torvalds/linux` — upstream Linux kernel, exact byte-for-byte
    cross-check of `lima_regs.h` and `lima_pp.c` against this repo's port.
  - `github.com/allwinner-zh/linux-3.4-sunxi` — an Allwinner vendor kernel tree
    that vendors ARM's own GPL "Mali Utgard" kernel driver in full
    (`modules/mali/DX910-SW-99002-r3p2-01rel2/`) — this is ARM's own driver, not
    a third-party reimplementation, fitting doubly well since our board's SoC
    vendor *is* Allwinner.
  - `github.com/limadriver/lima` — the Limare project's own home, no anti-bot at
    all, still up and fetchable in full.
  - `github.com/chaotic-cx/mesa-mirror` (and independently cross-checked against
    `github.com/mirror/mesa`, identical content) — a read-only GitHub mirror of
    `gitlab.freedesktop.org/mesa/mesa`. **This is the one PLAN.md's §2.1/§6
    explicitly said it could not reach** ("I could not fetch Mesa's actual
    `src/gallium/drivers/lima/{lima_screen,lima_bo}.c`... including a
    GitHub-mirror guess (404)"). The specific name `mesa3d/mesa` PLAN.md
    presumably guessed does 404; `chaotic-cx/mesa-mirror` and `mirror/mesa` do
    not, and both have `src/gallium/drivers/lima/` present and current.
  - A local find: this host's `kernel-headers-7.1.3-100.fc43.x86_64` RPM package
    ships `/usr/include/drm/lima_drm.h` — an independent, non-repo, non-fetched
    copy of the same UAPI header, useful as a zero-network cross-check.
- **`WebSearch`** was used to find the exact repo/file names above before
  fetching (e.g. it correctly pointed at `cwabbott0/mali-isa-docs`, which turned
  out to document the fragment-shader ALU ISA, not the job/frame format — a dead
  end reported honestly rather than stretched to fit).

All fetched files are saved under this session's scratchpad
(`/tmp/claude-0/-opt-bzdos/e178afc8-58a9-428c-a28c-a3907b800482/scratchpad/research/`)
for anyone who wants to check the primary text directly; that directory is not
part of this repo and will not persist.

## 3. `struct drm_lima_m400_pp_frame` — top-level fields

```c
struct drm_lima_m400_pp_frame {
        __u32 frame[LIMA_PP_FRAME_REG_NUM];      /* 23 words, §4 below */
        __u32 num_pp;
        __u32 wb[3 * LIMA_PP_WB_REG_NUM];        /* 3 x 12 words, §5 below */
        __u32 plbu_array_address[4];
        __u32 fragment_stack_address[4];
};
```
**[K]** `hal/lima/drm/lima_drm.h:62-71`. Byte-identical **[U]** to
`github.com/torvalds/linux/blob/master/drivers/gpu/drm/lima/lima_drm.h` and
**[H]** to `/usr/include/drm/lima_drm.h` (from `kernel-headers`) — three
independent copies agree exactly, so there is no struct-layout ambiguity at all.
Total size: 4 × (23 + 1 + 36 + 4 + 4) = **272 bytes**, checked at compile time in
`lima_pp_clear.c` via `_Static_assert`.

| Field | GPU VA or immediate? | What it is | Minimal-clear value |
|---|---|---|---|
| `frame[23]` | mixed, see §4 | Register block written verbatim to PP MMIO offsets `0x00..0x58` | §4 table |
| `num_pp` | immediate | How many PP cores this job uses | `1` — deliberately not `2`, see §8.3 |
| `wb[36]` | mixed, see §5 | 3 write-back unit register blocks | §5 table (unit 0 only; 1 and 2 all-zero) |
| `plbu_array_address[4]` | **GPU VA**, one per active PP core | Address of the per-core tile-descriptor array (§6) | `plbu_array_address[0]` = VA of that array; `[1..3]` unused (`num_pp=1`) |
| `fragment_stack_address[4]` | GPU VA (unused here) | Per-core fragment-shader stack | all `0` — **[L]** Limare's `pp.c` comment: *"not needed for drawing a simple triangle"*; with zero primitives there is no fragment-shader invocation at all, so this is even more clearly true here |

Both `plbu_array_address[i]` and `fragment_stack_address[i]` get copied by the
kernel into `frame[0]` and `frame[12]` respectively, immediately before upload —
**[K]** `hal/lima/lima_pp.c:654-661`:
```c
frame->frame[LIMA_PP_FRAME >> 2]  = frame->plbu_array_address[i];
frame->frame[LIMA_PP_STACK >> 2]  = frame->fragment_stack_address[i];
```
So whatever userspace puts in `frame[0]` and `frame[12]` **before** submission is
overwritten and never reaches hardware. This document sets them to `0` there for
clarity, but it does not matter.

## 4. `frame[23]` — word by word

Offsets are byte offsets from the PP core's own MMIO base (`LIMA_PP_FRAME =
0x0000`); word index = offset / 4. This repo's own `lima_regs.h` names only 5 of
the 23 words (`FRAME`, `RSW`, `STACK`, `STACK_SIZE`, `ORIGIN_OFFSET_X`) — **this
is not a trimmed-down copy**; ARM's own GPL driver's register header
(`regs/mali_200_regs.h`, **[A]**) names the *exact same 5* and no more. The
kernel driver (this repo's and upstream's) never needs to interpret the other 18
symbolically because it uploads the whole block as an opaque blob — see §7. The
remaining names below come from Limare and Mesa, external to any kernel driver.

| # | Byte off. | Name(s) | Value | Provenance |
|---|---|---|---|---|
| 0 | 0x00 | `LIMA_PP_FRAME` / "PLBU Array Address" | *(irrelevant — clobbered, §3)* | **[K][U][A]** name; **[M]** modern field name |
| 1 | 0x04 | `LIMA_PP_RSW` / "Render Address" | VA of a 64-byte, 64-byte-aligned, all-zero **render state** block (§4.1) | **[K][U][A]** register name+offset; **[L][M]** it's a pointer, confirmed by both; content choice is this doc's own reasoning |
| 2 | 0x08 | "Vertex Address" (Mesa) / `unused_0` (Limare) | `0` | **[L]** calls it unused; **[M]**'s own `lima_pack_pp_frame_reg()` never sets this field either, for any job — 0 is what a real, current, working driver produces |
| 3 | 0x0C | "flags": `FP16 Tilebuffer`(bit0) `Early Z`(bit1) ... (Mesa) / `flags`: `16BITS`(bit0) `ACTIVE`(bit1) `ONSCREEN`(bit5) (Limare) | `0x00000002` | **[G]** — Limare sets bit1 unconditionally ("always set", its name for it: `ACTIVE`); Mesa's `lima_pack_pp_frame_reg()` sets `frame.early_z = true` **unconditionally**, same bit. Two sources, 12 years apart, disagree on the bit's *name* but agree exactly on the *value*. Bit 0 (16-bit tilebuffer) and bit 5 (Mesa: "origin lower left"; Limare: "onscreen") are both left `0` — neither source sets them for an off-screen 8-bit target |
| 4 | 0x10 | `Clear Value Depth` | `0x00FFFFFF` | **[L][M]** identical position and identical constant in both; Limare sets it unconditionally even though this job has no depth write-back |
| 5 | 0x14 | `Clear Value Stencil` | `0x00000000` | **[L][M]** agree |
| 6 | 0x18 | `Clear Value 8bpc Color 0` — packed `R\|G<<8\|B<<16\|A<<24` | the clear colour, packed this way | **[M]**'s `genxml/common.xml` names the exact bit packing (`Red` bits0-7, `Green` 8-15, `Blue` 16-23, `Alpha` 24-31); **[L]** confirms word position and that it holds "rgba" |
| 7,8,9 | 0x1C,0x20,0x24 | `Clear Value 8bpc Color 1/2/3` | same packed value, 3 more copies | **[L][M]** both write all 4 copies identically regardless of MRT count |
| 10 | 0x28 | `Bounding Box Right` (Mesa, stored value = width−1) / `width` (Limare: "width−1, only if not 16-aligned, else 0") | `width−1` = **`0x3F`** for a 64-px-wide target | **[M]**: `lima_pack_pp_frame_reg()` sets `frame.bounding_box_right = fb->width` **unconditionally** (packer applies −1); this **contradicts** Limare's "only if not 16-aligned" rule for a 64×64 target. Preferring **[M]** as current/authoritative. `Bounding Box Left` (bits16-19) stays `0` — Mesa never sets it |
| 11 | 0x2C | `Bounding Box Bottom` | `0x3F` (height−1, same reasoning) | same as above |
| 12 | 0x30 | `LIMA_PP_STACK` | *(irrelevant — clobbered, §3)* | **[K][U][A]** |
| 13 | 0x34 | `LIMA_PP_STACK_SIZE` / `Fragment Stack Size`+`Pointer Initial Value` | `0x00000000` | **[K][U][A]** name+offset; **[L][M]** both zero it for a no-shading job (`job->pp_max_stack_size = 0`) |
| 14 | 0x38 | unused | `0` | **[L]** calls it unused; **[M]**'s struct packer never emits a field at this word at all — 2-source agreement that it's genuinely dead |
| 15 | 0x3C | unused | `0` | same as above |
| 16 | 0x40 | `LIMA_PP_ORIGIN_OFFSET_X` (kernel name) / `Origin X` (Mesa) / "one, always 1" (Limare) | `0x00000001` | **[K][U][A]** register *name*; **[L]** empirical value; **[M]**'s code: `frame.origin_x = 1;` **unconditionally** — this is not a coordinate that happens to be 1, current Mesa hard-codes the literal constant `1` for every job. Fully resolved, high confidence |
| 17 | 0x44 | `Origin Y` (Mesa, stored value = logical−1) / `supersampled_height` (Limare, =1 for non-supersampled) | `0x0000007F` (127 = 2×64−1) | **[M]**: `frame.origin_y = fb->height * 2;` unconditionally, packer applies −1. For height=64: (64×2)−1 = 127. This **disagrees with Limare's model** (which would give 1, not 127) — Limare's 2011 understanding of this specific word looks superseded; using Mesa's current formula as authoritative |
| 18 | 0x48 | "Subpixel Specifier" (Mesa) / `dubya` (Limare, "0x77, meaning unknown") | `0x00000077` | **[L]**: `job->frame.dubya = 0x77;` unconditional; **[M]**: `frame.subpixel_specifier = 0x77;` unconditional. Same constant, 12 years apart, now with a real name from Mesa even though neither source explains *why* 119 |
| 19 | 0x4C | "Tiebreak Mode" (Mesa) / `onscreen` (Limare, 0 for FBOs) | `0x00000001` | **[M]**: `frame.tiebreak_mode = 1;` unconditionally, no on/off-screen distinction visible at this word in the current driver. Following Mesa |
| 20 | 0x50 | "Polygon Tile Amount X/Y" + "Polygon Tile Size" (Mesa) / `blocking` (Limare: `(max_blocking<<28)\|(shift_h<<16)\|shift_w`) | `0x00000000` | **[G]** — both models agree on the bit layout; both reduce to all-zero when the tile grid needs no merging. For a 64×64 target: base tile grid is 4×4 = 16 tiles (16-px tiles, **[L]** `plb_create()`: `width = ALIGN(w,16)>>4`), and Limare only merges tiles when the *total* count exceeds 320 — 16 is nowhere close, so `shift_w=shift_h=0`, hence `blocking=0` under either model |
| 21 | 0x54 | "Scale/Flip ..." bits (Mesa) / `scale` (Limare, "always 0x10C without supersampling") | `0x00000E0C` | **[M]**: `frame.scale_fragcoord = frame.scale_derivatives = frame.flip_dithering_matrix = frame.flip_fragcoord = frame.flip_derivatives = true;` unconditionally → bits 2,3,9,10,11 set → `0xE0C`. **Note: this genuinely differs from Limare's `0x10C`** (bits 2,3,8 in Limare's model) — Limare's "flip" bits (8,9,10,11 in its scheme) don't line up 1:1 with Mesa's field boundaries, so the two "scale" values are not directly comparable bit-for-bit; treating Mesa's `0xE0C` as authoritative since it's the current, named, field-level source |
| 22 | 0x58 | "Tilebuffer Channel Layout" (Mesa: 4×4-bit nibbles) / `foureight` (Limare, "always 0x8888") | `0x00008888` | **[M]**: `genxml/common.xml`'s struct has 4-bit Red/Green/Blue/Alpha sub-fields; `lima_format_get_channel_layout()` returns `{8,8,8,8}` for **every 8-bit-per-channel pixel format**, packed as nibbles = `0x8888`. **[L]**'s `foureight = 0x8888` (empirical, name unexplained) is now fully explained: it's the per-channel bit-width descriptor, not a mystery constant. Full resolution, high confidence, two sources 12 years apart |

### 4.1 The render-state block that `RSW` (word 1) points to

64 bytes, must sit at a 64-byte-aligned GPU VA (word 1's packing is
`address>>6` — **[M]** `genxml/common.xml`, `struct "Render State" size="64"`).
Named fields exist (`shader_address`, `uniforms_address`, `textures_address`,
`varyings_address`, blend/depth/stencil state, etc. — **[L]** `render_state.h`,
**[M]**'s much larger `"Render State"` struct in `common.xml`) but **none of
them can matter for a job with zero primitives**: they are all consumed by
fragment-shader dispatch, which never happens if no primitive is ever rasterized
(§6). This document zero-fills the whole 64 bytes rather than leaving it
unmapped or garbage — cheap insurance, not a verified requirement — see §6 for
why zero-fill in general is the one real open question in this whole document.

## 5. `wb[36]` — the 3 write-back units

`LIMA_PP_WB(i) = 0x0100 * (i+1)` — **[K][U][A]** register block base offsets
(`0x100`, `0x200`, `0x300`). Only unit 0 is used (a single ARGB8888 render
target); units 1 and 2 are left entirely zero, which sets their `type` field to
`LIMA_PP_WB_TYPE_DISABLED` (`0`) — **[L]** `enum lima_pp_wb_type`.

| # | Field | Value (unit 0) | Provenance |
|---|---|---|---|
| 0 | `type` | `0x00000002` (`COLOR`) | **[K][U][A]** name `LIMA_PP_WB_SOURCE_SELECT` @ offset 0; **[L][M]** both call the field `type`, both use enum value `2` for a colour target |
| 1 | `address` | GPU VA of the render-target BO | **[K][U][A]** name `LIMA_PP_WB_SOURCE_ADDR` @ offset 4 (this one extra name beyond ARM's own header — Lima's kernel authors evidently confirmed it independently); **[L][M]** agree it's the target address |
| 2 | `pixel_format` | `0x00000003` | **[M]** `lima_format.c`: `PIPE_FORMAT_B8G8R8A8_UNORM` → hardware code `LIMA_PIXEL_FORMAT_B8G8R8A8 = 0x03`. See §5.1 for why this specific format is the one that means "ARGB8888" |
| 3 | `downsample_factor` | `0` | **[L][M]** agree, no MSAA |
| 4 | `pixel_layout` | `0` (linear) | **[M]** `lima_pack_wb_cbuf_reg()`: `pixel_layout = 0x0` for a non-tiled resource — an explicitly linear target must ask for linear, not tiled (`0x2`) |
| 5 | `pitch` | `32` | **[M]**/**[L]** both: `pitch = stride/8`. For 64px × 4 bytes/px = 256-byte stride: 256/8 = 32 |
| 6 | `flags` (Mesa) / `mrt_bits` (Limare, same word position) | `0x00000000` | **[M]** `lima_pack_wb_cbuf_reg()`: `wb.flags = swap_channels ? 0x4 : 0x0;` where `swap_channels = lima_format_get_pixel_swap_rb(format)`. For `PIPE_FORMAT_B8G8R8A8_UNORM`, `lima_pixel_formats[]` sets `swap_r_b = false` — **no swap needed**. This *disagrees* with what Limare's own demo code does at this word (it sets `4`, commented "RGBA instead of BGRA") — but Limare's demo target format is not the same one this document targets; see §5.1 |
| 7 | `mrt_bits` (Mesa) | `0` | **[M]** only set when `nr_samples>1` (MSAA); irrelevant here |
| 8 | `mrt_pitch` | `0` | **[M]**/**[L]** agree, single target |
| 9,10,11 | `unused0/1/2` | `0` | **[L]**/**[M]** agree |

### 5.1 Resolving "ARGB8888" precisely — byte order, not just a format name

This is the one place a wrong-but-plausible-looking value was a real risk, so the
reasoning is spelled out rather than just asserted.

- DRM's `DRM_FORMAT_ARGB8888` names channels MSB-to-LSB of a little-endian 32-bit
  read, i.e. **memory byte order (low to high address) is B, G, R, A** — this is
  DRM's own universal convention, not specific to this driver.
- Mesa/Gallium's `PIPE_FORMAT_B8G8R8A8_UNORM` names channels **in memory byte
  order directly** (byte0=B, byte1=G, byte2=R, byte3=A) — same convention,
  different naming scheme, same result: **`PIPE_FORMAT_B8G8R8A8_UNORM` and
  `DRM_FORMAT_ARGB8888` describe the identical in-memory byte layout.**
- **[M]** `lima_format.c` maps `PIPE_FORMAT_B8G8R8A8_UNORM` → hardware
  `pixel_format=0x03`, `swap_r_b=false`. It maps `PIPE_FORMAT_R8G8B8A8_UNORM`
  (a *different* memory layout, R first) to the *same* hardware code `0x03`
  but with `swap_r_b=true`.
- Conclusion **[G]**: hardware `pixel_format=0x03` with no swap (`flags=0`) is
  the encoding for "memory order B,G,R,A", which is exactly `ARGB8888`. This is
  a two-hop inference (DRM convention → Gallium convention → Mesa's own lookup
  table), not a single source stating the equivalence outright, so it is marked
  **[G]**, not **[M]** alone — high confidence, but say so plainly.
- **Because this is the one place a subtle mistake produces wrong colours
  instead of a hang**, `lima_pp_clear.c` deliberately clears to a colour with
  four *different* byte values (`R=0x11,G=0x22,B=0x33,A=0xFF`) rather than a
  colour like solid red or grey, specifically so a channel swap shows up
  immediately in the byte-by-byte read-back rather than being invisible.

## 6. The tile-descriptor array (`plbu_array_address` target) — what "no primitives" actually means

This is the part `docs/PLAN-mesa-lima.md` flagged as not derived. It is now derived,
with an important scope correction.

### 6.1 The array format itself — verified, not guessed

**[L]** Limare's `limare/lib/plb.c`, function `plb_pp_stream_create()`, and
**[M]** Mesa's current `lima_job.c`, function `lima_generate_pp_stream()` —
**two independently-written, hardware-verified drivers, 12+ years apart — use
byte-identical magic opcodes** for this array. Per tile:

```
word0 = 0x00000000
word1 = 0xB8000000 | tile_x | (tile_y << 8)                       /* set tile position */
word2 = 0xE0000002 | ((tile_polygon_list_va >> 3) & ~0xE0000003)   /* pointer into that tile's polygon-list scratch, §6.2 */
word3 = 0xB0000000
```
followed by one terminator record after the last tile — **[M]**'s version (used
here, current/authoritative) is 4 words, 2 more than **[L]**'s 2-word version:
```
0x00000000, 0xBC000000, 0x00000000, 0x00000000
```
This makes every record in the array a fixed 4 words, including the terminator —
tidier than Limare's shorter terminator and worth preferring since it's current.

For a 64×64 target: 16-pixel base tiles (**[L]** `plb_create()`:
`width_tiles = ALIGN(w,16)>>4`) give a 4×4 = 16-tile grid, so the array is
16 × 4 words (tile records) + 4 words (terminator) = **272 bytes** — which is
exactly **[L]**'s own formula `pp_size = 16 * (width*height + 1)` for
`width=height=4`: `16*(16+1) = 272`. Two independent derivations agree exactly.

With `num_pp=1` (§8.3), all 16 tiles go into this single array; the address of
this array (as returned by `GEM_INFO` on the BO that holds it) is
`plbu_array_address[0]`.

### 6.2 What's genuinely unverified: the per-tile polygon-list content

Word 2 of each tile record points at a small (512-byte, **[L]** `block_size =
0x200`) scratch region *per tile* — this is where the GP's PLBU hardware unit
writes each tile's actual list of triangle references, for a **real** job with
real geometry. This document's job has zero geometry, so nothing ever writes
real content there — the question is whether that memory being **left
zero-filled** (which is what a freshly-allocated GEM BO already gives, for free)
is read by the PP hardware as "zero primitives in this tile" or as something
else, including something that hangs.

**No source found — not Limare, not Mesa, not ARM's own driver — ever answers
this**, for a structural reason, not a research gap that more searching would
close: in every real pipeline, this memory is a hardware **output** of the GP's
PLBU unit, never a software **input** that any CPU code constructs by hand.
There is no reference implementation of "an empty polygon-list record" to copy,
because no software anywhere ever writes one on purpose — the GP always does,
even when told to process zero vertices.

**And that "even when" is the scope correction PLAN.md needs:** reading Mesa's
current `lima_do_job()` (`lima_job.c:874-1017`) shows that **Mesa's own driver
never actually submits a PP job without a preceding GP job — not even for a
draw-nothing clear.** `lima_job_has_draw_pending()` exists and can be `false`
(`plbu_cmd_array.size == 0`), but `lima_do_job()` has no branch that skips the GP
submission in that case — it always builds a `drm_lima_gp_frame`, always calls
`lima_job_start(job, LIMA_PIPE_GP, ...)`, and only then builds and submits the PP
frame. The GP job just becomes trivial (`vs_cmd_size=0`; the PLBU command list is
only the 2-word `END` terminator `lima_finish_plbu_cmd()` appends — `lima_job.c:
734-744`), but it still runs, and it is presumably what leaves the per-tile
polygon-list scratch in a state the PP can read as "empty" — **whatever that
state actually is is exactly the thing this document cannot verify**, because
it's produced by real GP hardware executing a real (if trivial) command list,
not by anything zero-filled in advance by software.

**Net effect on this document's actual deliverable (a PP-only submission, per
`docs/PLAN-mesa-lima.md` §1.3's own choice to skip the GP):** the tile-descriptor
*array* is exact and sourced; the *content it points into* is the one place this
document, `lima_pp_clear.c`, and — as far as could be established — every
existing open-source Mali-400 driver, have nothing to say. Zero-filling that
scratch region (what this document's test program does, because a fresh
`GEM_CREATE` BO already gives zero pages for free) is a reasonable default, not
a verified one. It is the single **[X]** in this whole document, and it is named
here precisely rather than left as a vague caveat.

One thing worth putting on the record for whoever revisits this: since Mesa
itself always pairs GP+PP even for a no-op clear, an alternative to the PP-only
path would be to submit that same trivial GP job first (6-word `drm_lima_gp_frame`,
`vs_cmd_size=0`, `plbu_cmd` = just the 2-word `END`) before the PP job — this
would remove the **[X]** entirely by exactly reproducing what a real, working
driver does, at the cost of one more small BO (a GP tile-heap) and one more
ioctl round-trip. That is a real, buildable alternative; it is not what was
asked for here (`docs/PLAN-mesa-lima.md` §1.3 explicitly chose PP-only), so it is
recorded as a finding for the plan's owner, not implemented.

## 7. What the kernel validates — plainly, and the hang risk that follows

Read directly, not inferred:

- **`lima_ioctl_gem_submit`** (`hal/lima/lima_drv.c:166-244`) checks: `pipe` is
  a valid pipe index and `nr_bos != 0` (line 188); `flags` has no unknown bits
  (190); **`frame_size` equals `pipe->frame_size` exactly** (194-195, i.e. it must
  be exactly `sizeof(struct drm_lima_m400_pp_frame)` = 272 bytes, checked purely
  as a byte count); then it `copy_from_user()`s the BO list and the frame
  **verbatim**, and calls `pipe->task_validate()`.
- **`lima_pp_task_validate`** (`hal/lima/lima_pp.c:576-598`) — the *only* other
  check — reads `num_pp` and confirms `0 < num_pp <= pipe->num_processor`; for
  the Mali-400 (non-broadcast) path shown here, that is genuinely **the entire
  check**. No field of `frame[]`, no field of `wb[]`, no `plbu_array_address`,
  no `fragment_stack_address` is inspected for validity, range, or alignment.
- **`lima_pp_task_run`** (`hal/lima/lima_pp.c:648-668`) then writes every one of
  those unchecked words straight into PP MMIO via `lima_pp_write_frame()`
  (`lima_pp.c:299-310`, a flat loop, no interpretation) and writes
  `LIMA_PP_CTRL_START_RENDERING` — the hardware runs whatever was uploaded.

**Say this plainly, as asked:** the kernel performs **no content validation
whatsoever** of the frame this document describes. A malformed `plbu_array_address`
(pointing at unmapped or non-existent GPU VA), a bad pitch/pixel_format
combination that makes the write-back unit compute an out-of-bounds address, or —
per §6.2 — a per-tile polygon-list region that the PP's tile-traversal logic
does not actually treat as "empty" the way this document assumes, all go
straight to real Mali-400 silicon with nothing in software positioned to catch
them first. `docs/PLAN-mesa-lima.md` §5.4 argues the *most likely* outcome of a bad
frame is a bounded, reported failure (job timeout + the existing
hang-detection IRQs + scheduler recovery + `hw.lima_error`), not a board-wide
stall of the EHCI-storm kind this project has already lost hours to — but that
argument has itself never been tested against a real hang on this hardware, and
is not a substitute for treating every field here as something that can reach
the GPU exactly as written. This is precisely why `lima_pp_clear.c` refuses to
run without an explicit, informed flag (§9 of that file's own design).

## 8. BO layout used by `lima_pp_clear.c`

Two BOs, both allocated via plain `GEM_CREATE` (no `LIMA_BO_FLAG_HEAP` — that
path is stubbed to `-ENOSYS`, **[K]** `hal/lima/lima_gem.c:28-37`, already noted
in `docs/PLAN-mesa-lima.md` §1.2).

### 8.1 Render target BO — 16384 bytes (64×64×4)

Written by the PP's write-back unit; read back by the CPU afterwards. Must be
listed in `drm_lima_gem_submit.bos[]` with `LIMA_SUBMIT_BO_WRITE` — **[K]**
`hal/lima/lima_gem.c:356-361`: the submit path attaches the completion fence to
every BO in `bos[]`, tagged by that BO's read/write flag; `LIMA_GEM_WAIT` on this
handle is how the test program detects completion (`LIMA_GEM_WAIT` needs a fence
on *this* BO specifically to have anything to wait for).

### 8.2 Scratch BO — 0x3000 bytes (12288), sub-divided by fixed byte offset

| Offset | Size | Contents |
|---|---|---|
| `0x000` | 64 | Render-state block (§4.1), all zero |
| `0x100` | 272 (ends at `0x210`) | Tile-descriptor array (§6.1): 16 × 16-byte tile records + 1 × 16-byte terminator |
| `0x400` | 16 × 512 = 8192 (ends at `0x2400`) | Per-tile polygon-list scratch (§6.2), zero-filled, **[X]** |

The gap between `0x210` (end of the tile array) and `0x400` (start of the
per-tile scratch) is deliberate slack, not an oversight — the tile array is
272 bytes, so anything placed before `0x210` would overlap it; rounding up to
the next `0x100` boundary keeps the arithmetic easy to re-check by eye instead
of packing tight. (An earlier draft of this document and of `lima_pp_clear.c`
placed the per-tile scratch at `0x200`, which *does* overlap the tile array's
last 16 bytes — caught and fixed during a final correctness pass, before
either file was ever run, precisely because nothing here is exercised on real
hardware to catch it for us.)

All three regions start at offsets that are multiples of 64 (in fact of 256),
satisfying every alignment requirement found in §4 (`RSW`: 64-byte; tile array
pointer: 8-byte; per-tile polygon-list pointer: 8-byte) with room to spare.
Listed in `bos[]` with `LIMA_SUBMIT_BO_READ` (the PP only reads it). Total
scratch BO usage is `0x2400` (9216) bytes; the BO itself is allocated at
`0x3000` (12288) for margin.

**Why every address-shaped field above is written as a plain, unshifted byte
address, despite `genxml/common.xml` showing `shr(3)`/`shr(6)` packing
modifiers:** those modifiers are an artifact of Mesa's own bit-packing helper
library, which lets its driver code write a real address and has the packer
shift it for storage. The *raw hardware word*, before and after that packing, is
numerically identical to the plain address whenever the address's low bits are
already zero — which page-aligned (4096-byte) GEM allocations, and 64-byte
sub-offsets within them, both guarantee. **[K]**'s own kernel code confirms this
independently: `lima_pp.c:657-658` assigns `frame->plbu_array_address[i]`
straight into `frame[0]` with no shift at all.

### 8.3 Why `num_pp = 1`, not `2`

`GET_PARAM(NUM_PP)` on this board reports `2` (`docs/MALI-STATUS.md`, `pp0`+`pp1`
both attach). `lima_pp_task_validate` accepts any `num_pp` from `1` up to that
count (§7). Using `num_pp=2` would require splitting the 16 tiles across two
separate tile-descriptor arrays — **[M]** `lima_generate_pp_stream()` does this
with a Hilbert-curve interleave for cache locality (`round-robin: pp = index %
num_pp`) — which is a real, sourced algorithm but pure complexity for a minimal
test with no performance goal. `num_pp=1` runs on real hardware, exercises the
real completion IRQ path, and is valid per the kernel's own check; the second PP
core simply never receives a `START_RENDERING` write (`hal/lima/lima_pp.c:
654-666`, the per-core loop only iterates `frame->num_pp` times) and stays idle.

## 9. Summary table: how many of the 23 + 12 fields are guesses

| Category | Count | Which |
|---|---|---|
| Verified, ≥2 independent sources agree on value | 18 of 23 frame words + 9 of 12 wb words | see §4/§5 tables |
| Verified structure, value is this document's own reasoned synthesis (**[G]**) | 3 frame words (3, 20, 21 — bit-level combination) + `pixel_format`/`flags` pairing (§5.1) | shown with derivation |
| Verified name/position, but two sources disagree on value and Mesa (current) was preferred | 4 frame words (10, 11, 16→ resolved fully, 17 partially, 21 partially) | called out individually in §4 |
| Structure verified from 2 independent hardware-tested drivers; **content is an unverified guess** | 1 region: the per-tile polygon-list scratch, §6.2 | the one real **[X]** |

That is a much smaller gap than `docs/PLAN-mesa-lima.md` §1.3/§6 had reason to expect
going in, and it is a *different, more specific* gap than "the opcodes are
unknown" — the opcodes are known; what a from-scratch, GP-skipped submission
does to hardware that no existing driver has ever fed that exact input is not.

## 10. Files read in this repo while writing this document

`hal/lima/drm/lima_drm.h`, `hal/lima/lima_regs.h`, `hal/lima/lima_pp.c`,
`hal/lima/lima_pp.h`, `hal/lima/lima_drv.c`, `hal/lima/lima_gem.c`,
`hal/lima/lima_gem.h`, `hal/lima/lima_vm.c`, `hal/lima/lima_vm.h`,
`hal/lima/lima_sched.h`, `hal/lima/lima_device.h`, `hal/lima/lima_device.c`
(lines 735-810), `docs/MALI-STATUS.md`, `docs/README-arm64.md`,
`docs/PLAN-mesa-lima.md`, `hal/lima/tests/test_lima_math.c` (style
reference only).
