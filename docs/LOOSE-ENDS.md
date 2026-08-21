# hal/lima, hal/bzfb, hal/bzkms — loose ends

Compiled 2026-08-20 from the tree, not from memory. Nothing here was measured on
the board during this pass: a live hardware session held the serial/EMAC channel,
so every number quoted is either read out of a committed source comment or commit
message (attributed), or measured on the Linux host (patch dry-runs, package
contents, file state). Anything that needs the board to settle says so.

State this document is written against: lima renders on real hardware
(`tests/limabench.c` — sampled textures, 2420 draw calls, depth, blend, ~80 fps,
zero GPU MMU faults, commit `9f4088f`); Mesa 26.2 with the lima gallium driver is
hand-built into the guest's `/usr/local`; zero-copy presentation works through
`hal/bzfb` (1030.7 fps at 1120x276, commit `afe2591`); `hal/bzkms` is a real
DRM/KMS device on `/dev/dri/card1` doing PRIME import and page flips at 59.7 fps
paced by `DRM_EVENT_FLIP_COMPLETE` (commit `62fd5fd`).

Ordering is (risk x cheapness-to-fix): the things that can bite hardest for the
least work first. "Blocks extraction" means: blocks shipping `hal/lima` (+ the
`drm/` and `linux/` shims) as a standalone FreeBSD/lima deliverable with no bzdOS
dependency.

---

## 1. The `nonpci-busid` patch cannot be applied — `patch` rejects the file

> **RESOLVED.** The patch was regenerated as a real unified diff and now
> applies cleanly to a pristine `drm_sysctl_freebsd.c` — re-verified 2026-08-21
> after it was extended (the fallback busid must vary per device or Mesa cannot
> tell two DRM devices apart). Checked by reverse-applying to recover the
> original file and then re-applying the pair in order: the result is identical
> to the working tree byte-for-byte.

**Claim.** The fix for a drm-kmod defect that has already panicked one driver in
this tree is quarantined behind a file that `patch(1)` refuses to read.

**Evidence.** `patches/drm-kmod/drm-kmod-nonpci-busid.patch` has unified-diff
bodies but its hunk headers are bare `@@` with no line/count fields (two of them,
at the `---`/`+++` pair for `drivers/gpu/drm/drm_sysctl_freebsd.c`). Measured on
this host:

```
$ cd /opt/bzdos/drm-kmod-src && patch -p1 --dry-run < .../drm-kmod-nonpci-busid.patch
patch: **** Only garbage was found in the patch input.
```

Reverse dry-run fails identically. The other two in the same directory are fine —
both report "Reversed (or previously applied) patch detected", i.e. already
applied to `/opt/bzdos/drm-kmod-src`, and both reverse-apply cleanly. So
`infra/scripts/build-drm-kmod-arm64.sh`'s idempotent apply loop (`PATCHDIR` at
`build-drm-kmod-arm64.sh:67`, applied at `:107`) will hard-fail or skip on this
one — its own guard "hard-fails on a tree whose state it cannot determine"
(`patches/README.md`).

The defect it fixes is real and observed: `drm_add_busid_modesetting()`
(`/opt/bzdos/drm-kmod-src/drivers/gpu/drm/drm_sysctl_freebsd.c:174`) does
`to_pci_dev(dev->dev)` then `pdev->bus->number` for every DRM device, PCI or not.
`hal/bzkms` panicked there at `far = 0x8, esr = 0x96000004` (commit `d7c6653`,
and `bzkms.c:144-164` carries the stack). `hal/lima` survives only because its
parent sits inside a heap-allocated `platform_device`, so the backwards walk
lands in readable adjacent memory and the busid string is silently garbage.

**Unproven.** Whether the deployed guest `drm.ko` carries the fix (it cannot —
the patch has never applied, and `/opt/bzdos/drm-kmod-src/drivers/gpu/drm/drm_sysctl_freebsd.c:174`
is still the unguarded `to_pci_dev`). Whether lima's garbage busid string is ever
read by anything — nothing in this tree reads `hw.dri.0.busid`, but libdrm's
`drmGetBusid`/`drmParseSubsystemType` path is untested here.

**Next step.** Regenerate the patch with real hunk headers (`diff -u` against a
scratch copy of the file, or `git diff` in a throwaway repo), re-run the
dry-run/forward/reverse round trip, rebuild `drm.ko`, and then delete the
fake-PCI workaround in `bzkms.c:165-167` (`static struct pci_bus bzkms_pbus;
static struct pci_dev bzkms_ppdev;`) which exists solely to keep the bad cast
well-defined — `bzkms.c:162-164` says so explicitly.

**Extraction.** Blocks it. A standalone lima deliverable for FreeBSD inherits the
same latent landmine, and this is the patch that removes it.

---

## 2. The single fix the whole port depends on exists only as an uncommitted edit

> **RESOLVED.** The `ccu_a64.c` `AW_CLK_HAS_GATE` fix is now both a commit
> (`f38f90d1e`) and a patch file
> (`patches/freebsd-src/freebsd-ccu-a64-pll-gpu.patch`, 2399 bytes), listed in
> `patches/UPSTREAM-INDEX.md` as #3 with its write-up. It is no longer reachable
> only as a working-tree edit that a fetch could erase.

**Claim.** `ccu_a64.c`'s `AW_CLK_HAS_GATE` on `pll_gpu_clk` — the fix without
which the GPU cannot be clocked at all — is a dirty working-tree edit with no
commit and no patch file. One `git checkout` destroys it.

**Evidence.** In `/opt/bzdos/freebsd-src-earlyboot-wt`:

```
$ git status --short sys/dev/clk/allwinner/
 M sys/dev/clk/allwinner/aw_clk_frac.c
 M sys/dev/clk/allwinner/aw_clk_m.c
 M sys/dev/clk/allwinner/aw_clk_mipi.c
 M sys/dev/clk/allwinner/aw_clk_nkmp.c
 M sys/dev/clk/allwinner/aw_clk_nm.c
 M sys/dev/clk/allwinner/aw_clk_nmm.c
 M sys/dev/clk/allwinner/aw_clk_np.c
 M sys/dev/clk/allwinner/ccu_a64.c
```

`git diff 9263fb9ba HEAD` on branch `linuxkpi-dma-fix` touches only
`sys/compat/linuxkpi/common/src/linux_pci.c` and
`sys/dev/clk/allwinner/ccu_a83t.c` — `ccu_a64.c` is not in any commit. The edit
itself is at `sys/dev/clk/allwinner/ccu_a64.c:384-420` (a 25-line comment dated
`bzdOS 2026-08-11` plus `AW_CLK_HAS_LOCK | AW_CLK_HAS_GATE` on line 417).
`patches/freebsd-src/` contains only `freebsd-allwinner-clk-get-gate.patch` and
`freebsd-ccu-a83t-cpux-gate-lock.patch` — there is **no**
`freebsd-ccu-a64-pll-gpu.patch`, even though the 23 KB upstream write-up
`patches/UPSTREAM-freebsd-ccu-a64-pll-gpu.md` exists.

The seven `aw_clk_*.c` edits are the `get_gate` patch and *are* backed by
`patches/freebsd-src/freebsd-allwinner-clk-get-gate.patch` (byte-identical to the
tree state, verified by the dry-run being a clean reverse), but they are also
uncommitted.

**Unproven.** Nothing. This is a file-state fact.

**Next step.**
`git diff -- sys/dev/clk/allwinner/ccu_a64.c > hal/lima/patches/freebsd-src/freebsd-ccu-a64-pll-gpu.patch`,
then commit all eight files on `linuxkpi-dma-fix` so the branch reproduces the
working kernel from a clean checkout. Cross-check the resulting patch applies to
a pristine `releng/15.1`.

**Extraction.** Blocks it. This is a `freebsd-src` prerequisite for the port on
any A64 board; without a patch file, the deliverable is not reproducible.

---

## 3. A Mali PTE is 32 bits wide and nothing bounds the pages that go into it

> **RESOLVED 2026-08-20.** All four sites now refuse instead of truncating:
> `lima_vm_map_page()` rejects a leaf page above 4 GiB (`-ERANGE`), the
> page-directory path rejects page tables it cannot address (and frees them
> again), `drm_gem_shmem_freebsd_try_contig()` bounds
> `vm_page_alloc_contig()` at `UINT32_MAX` so the failure lands where the
> caller already has a per-page fallback, and bzkms bounds its own
> `contigmalloc()`, rejects an imported dma-buf crossing 4 GiB, and refuses
> to write an out-of-range address to the 32-bit scanout doorbell.
> Hardware-verified on the 1 GiB board: `limabench` passes, `limakms` runs
> 3599 frames at 60.0 fps with 0 refused, `lima_contig_alloc_pages` keeps
> climbing (so the bound did not silently push everything onto the slow
> path), and none of the new refusals fires — which is the correct outcome
> where no memory exists above 4 GiB.

**Claim.** `lima_vm_map_page()` truncates a 64-bit physical address into a 32-bit
PTE with no check, and the allocator that produces those pages is given the whole
physical address space as its upper bound.

**Evidence.**
- `lima_vm.h:70` — `u32 *cpu;` (the page-table array).
- `lima_vm.c:118` — `vm->bts[pbe].cpu[bte] = pa | LIMA_VM_FLAGS_CACHE;` where `pa`
  is `dma_addr_t` (64-bit). Silent narrowing, no `WARN`, no `-EINVAL`.
- `drm/drm_gem_shmem_helper.c:220-222` — the contiguous backing path calls
  `vm_page_alloc_contig(vm_obj, 0, ..., npages, 0, ~(vm_paddr_t)0, PAGE_SIZE, 0,
  VM_MEMATTR_DEFAULT)`. High bound `~(vm_paddr_t)0`, not 4 GiB. The per-page
  fallback (`shmem_read_mapping_page()`) has no bound either — this is
  `drm/drm_gem_shmem_helper.c` header deviation #2 (`:43-47`), still open, and now
  open in two places rather than one.
- The 32-bit constraint *is* declared where LinuxKPI can see it:
  `lima_device.c:777` `dma_set_coherent_mask(ldev->dev, DMA_BIT_MASK(32))` and
  `linux/platform_device.c:186,189` `linux_dma_tag_init{,_coherent}(dev,
  DMA_BIT_MASK(32))`. Those govern `dma_alloc_coherent()` (the page-directory
  allocation, `lima_vm.c:104`) and `dma_map_sg()` — not the shmem BO pages that
  end up in PTEs.
- Same class of bug in `hal/bzkms`: `bzkms.c:194-195` writes
  `(uint32_t)pa` into the flip doorbell with no range check, and
  `bzkms.c:523` allocates dumb buffers with `contigmalloc(..., 0, ~0UL, ...)`.

**Unproven / mitigating.** Unobservable on this board: the guest has 1 GiB of
DRAM, so no page can be above 4 GiB. For bzkms specifically EL2 validates the
extent before reprogramming DE2, and commit `afe2591`/`README-zerocopy.md` (bzdk-side, not in this repository) record
153471 accepted addresses with zero rejected — so a truncated address would
surface as an EL2 rejection (black screen) rather than as scanout of the wrong
memory. Neither mitigation is a guarantee, and neither produces a guest-side
diagnostic.

**Next step.** Two one-liners plus a bound: (a) in `lima_vm_map_page()`, reject
`pa >= (1ULL << 32)` with a `DRM_ERROR` naming the address; (b) change the
`vm_page_alloc_contig()` high bound to `BUS_SPACE_MAXADDR_32BIT`; (c) in
`bzkms_program()`, refuse `pa >= (1ULL << 32)` rather than truncating. This is
`PLAN-mesa-lima.md:448-466`'s §5.3, still exactly as it was written.

**Extraction.** Yes for (a)/(b) — a standalone lima deliverable will be run on
boards with more than 4 GiB. (c) is bzdOS-only.

---

## 4. `patches/` has three byte-identical duplicates, and the Makefile's own recipe reintroduces the bug they caused

> **RESOLVED 2026-08-20.** The three flat duplicates are deleted, the
> `Makefile` recipe now globs `patches/drm-kmod/*.patch` with a comment
> saying why the subdirectory is load-bearing, and all seven references that
> pointed at the flat paths were rewritten to the real ones. The related
> two-trees hazard in the same item is closed too: `bzkms/Makefile` and
> `bzfb/Makefile` defaulted to `/opt/bzdos/drm-kmod-src` while `drm.ko`
> itself is built from `/opt/bzdos/drm-kmod` — an ABI mismatch that would
> have presented as a guest panic inside DRM core, not as a build error.
> Both now default to the same tree `drm.ko` comes from; both modules were
> rebuilt against it and the full stack re-verified on hardware (60 s,
> 3598 flips, 0 refused).

**Claim.** The flat top-level copies of three patches are stale duplicates; the
build recipe in `Makefile`'s header globs exactly that flat directory, which is
the failure `patches/README.md` was written to prevent.

**Evidence.**
- `diff` says all three top-level copies are IDENTICAL to their subdirectory
  originals: `drm-kmod-dev-alias-lifecycle.patch`, `drm-kmod-dma-buf-mmap.patch`,
  `freebsd-allwinner-clk-get-gate.patch`. All six are tracked in git
  (`git ls-files hal/lima/patches/`).
- The top-level set is *missing* `drm-kmod-nonpci-busid.patch` — so it is not
  merely redundant, it is a partial, silently-out-of-date view.
- `Makefile:13` — `(cd /opt/bzdos/drm-kmod-src && for p in
  ../bsdOS/hal/lima/patches/*.patch; do patch -p1 -i $p; done)`. That glob picks
  up `freebsd-allwinner-clk-get-gate.patch`, a **freebsd-src** patch, and tries to
  apply it to drm-kmod. `patches/README.md` documents that this exact
  configuration made the build refuse to start, and says "Do not flatten it
  again."
- Four live references still point at the flat paths and would need updating:
  `drm/drm_gem_shmem_helper.c:49`, `drm/drm_gem_shmem_helper.c:1076`,
  `MALI-STATUS.md:128`, `MALI-STATUS.md:208`, plus
  `patches/UPSTREAM-drm-kmod-dev-alias.md:286,302` and
  `patches/UPSTREAM-freebsd-ccu-a64-pll-gpu.md:382`.
- `patches/README.md`'s own table says `drm-kmod/` targets `$DRM_KMOD_SRC`
  (`/opt/bzdos/drm-kmod`), while `Makefile:19` and `bzkms/Makefile:28` default to
  `/opt/bzdos/drm-kmod-src`, and `build-drm-kmod-arm64.sh:62` defaults to
  `/opt/bzdos/drm-kmod`. Both trees exist. `diff -rq` says they are identical
  today (only `.git`/`.github`/`.gitignore` differ), but neither is a git
  checkout, so future drift would be invisible.
- `patches/README.md` does not mention `drm-kmod-nonpci-busid.patch` at all.

**Unproven.** Nothing.

**Next step.** `git rm` the three top-level `.patch` files; repoint the seven
references at `patches/drm-kmod/` / `patches/freebsd-src/`; fix `Makefile:13` to
`patches/drm-kmod/*.patch`; pick one `DRM_KMOD_SRC` default and use it in all
three places; add a `nonpci-busid` section to `patches/README.md`.

**Extraction.** Yes — the patch set *is* part of the deliverable, and shipping two
inconsistent copies of it plus a recipe that misapplies them is a defect in the
deliverable itself.

---

## 5. The port's five design documents are frozen at 2026-08-11/12 and now state the opposite of the truth

> **RESOLVED 2026-08-21 — and the first attempt at this was not enough.**
> Banners were added at the top of the affected documents earlier, but the false
> statements stayed in the bodies, so a reader landing mid-document still got
> them. Corrected in place, at the sentence:
>
> - `README-arm64.md` — "no submit path has been exercised and no Mesa/lima
>   userland exists on the board", and the imported-PRIME-cannot-be-mmaped gap
>   (fixed by a patch that is now a build prerequisite).
> - `MALI-STATUS.md` — the "Nothing has *rendered* yet" to-do is struck rather
>   than deleted, and the heading "The GPU's interrupts are registered and have
>   never fired" is superseded in place: they fire on every job.
> - `PLAN-mesa-lima.md` — its premise ("Nothing has rendered ... no Mesa/lima
>   userland exists anywhere in this project") is struck, and its verdict "Mesa
>   is not a realistic near-term milestone" is annotated as the most expensive
>   error in the document: Mesa was cross-built and rendering eight days later.
> - `patches/README.md` — a whole section headed "**What has NOT been tested, at
>   all:**" claimed no kernel build, `dma_buf_mmap()` never executed, nothing
>   loaded. That patch is applied on every build, is in the running `drm.ko`,
>   and carries the zero-copy path at 1030 fps. Replaced with the measured
>   state; the one line still accurate (the `fork(2)`-inherited-vma case) is
>   kept.
> - `lima_sched.c` — "UNTESTED ON HARDWARE" on the teardown path, which was
>   verified live the same day, including forcing the recovery branch.

**Claim.** The documents a newcomer reads first say the GPU has never rendered.
This is not a cosmetic staleness — the load-bearing verdicts are inverted.

**Evidence** (last commit touching each, and the worst line in each):

| doc | frozen at | the sentence that is now false |
|---|---|---|
| `MALI-STATUS.md` | `bbb7a86`, 2026-08-11 23:33 | `:36` "Still no GPU *job* has run."; `:139` heading "The GPU's interrupts are registered and have never fired"; `:161-163` "Nothing has *rendered* yet ... no Mesa/lima userland is present" |
| `PLAN-mesa-lima.md` | `2198572`, 2026-08-11 20:45 | `:25-27` "**Nothing has rendered.** ... no Mesa/lima userland exists anywhere in this project"; `:288-299` "Mesa is not a realistic near-term milestone" |
| `SCANOUT-IMPORT.md` (bzdk-side, not in this repository) | `c5885a0`, 2026-08-11 23:52 | `:10-13` recommends the import route that was later measured not to work; `:30-34` "The single largest blocker is **not guest-side at all**" (the shipped design never needed that window); `:194-210` quotes `lima_l2_cache_flush()` as `{ return 0; }` |
| `mesa-feasibility.md` | **never committed** | `:14-18` "**nobody has ever done it**"; `:250-267` "blocked on a missing userspace sysroot" |
| `mesa-build-log.md` | `44f0677`, 2026-08-12 | `:306` heading "Final status ... dependency staging still in flight, build not started"; the file never records the outcome it exists to record |
| `drm/PURGE-NOTES.md` | `92315d4`, 2026-08-11 | `:15-16` "**Nothing here has run on hardware, or in a kernel build.**" — contradicted by the same directory's `drm_gem_shmem_helper.c:130` "measured on hardware 2026-08-20" |
| `tests/README.md` | `9f4088f`, 2026-08-19 | `:3-5` "this board has no FreeBSD display driver" — `hal/bzfb` and `hal/bzkms` both exist and work |

Two internal self-contradictions worth naming because they will mislead
independently of the staleness:
- `MALI-STATUS.md:3,48` say the GPU is up; `MALI-STATUS.md:305-307` still reads
  "**Status: built clean, not verified past the build.** It does not lift the
  stall" about the PMU, presented as live status rather than history. The file's
  `:170` "the history below" fence fails at exactly that one **Status:** line.
- `PLAN-mesa-lima.md:120-133` says the PP-clear opcodes *were* derived and
  cross-sourced; `:535-538` says the author "did not derive or verify" them. Same
  document, same day.
- `bzfb.c:55-56` — the file's own header says "DELIBERATELY NOT DONE HERE: no
  fb_info/fbd registration, so this creates no /dev/fb0" while `bzfb.c:146-174`
  and `:486-515` implement exactly that behind `hw.bzfb.fbd` (commit `afe2591`).
- `bzfb.c:48-53` argues export-not-import because "let lima allocate the BO and
  point the mixer at it -- founders on physical contiguity". That alternative is
  precisely what shipped, contiguity having been solved in
  `drm/drm_gem_shmem_helper.c:113-260`.

Roughly a hundred `file:line` citations inside these documents have also drifted
(e.g. every reference to `lima_vm.c:87` now means `lima_vm.c:118`;
`drm_gem_shmem_helper.c:772-782` is now `:1064-1117`).

**Unproven.** Nothing.

**Next step.** Cheapest useful action is not a rewrite: put a dated
`SUPERSEDED — see LOOSE-ENDS.md and the commits since <hash>` banner at the top of
each of the five pre-first-light documents, correct the six or so inverted
headline sentences listed above, and `git add hal/lima/mesa/FEASIBILITY.md`
(currently untracked, so none of its content is in history at all). `bzfb.c`'s
header contradiction is a two-line edit.

**Extraction.** Yes, weakly — the docs are the deliverable's explanation of
itself. `SCANOUT-IMPORT.md` (bzdk-side, not in this repository) and `hvfb/` do not travel at all (item 12).

---

## 6. `lima_l2_cache_flush()`'s return value is ignored by both callers

> **HALF DONE, deliberately (2026-08-21).** The observable half is in: the
> timeout counter was a function-local static, invisible from userland, and is
> now `sysctl compat.linuxkpi.lima_l2_flush_timeouts` — sticky for the module's
> lifetime, in the style of the counters in `drm/drm_gem_shmem_helper.c`. So
> "did any flush get abandoned during that run?" is now answerable after the
> fact instead of only by grepping dmesg for a message printed on powers of two.
> Measured since: **0**.
>
> The other half — propagating the error out of `lima_sched_run_job()` as a job
> failure — is **not** done, on purpose. It is a behaviour change on the hot
> path that turns a silent risk into a visible job failure, this item's own
> "next step" says it needs a soak first, and with the counter at 0 there is
> currently nothing to soak against. Revisit if the counter ever moves.

**Claim.** An abandoned L2 flush is indistinguishable from a successful one at
both call sites, and its only symptom is wrong pixels.

**Evidence.** `lima_l2_cache.c:78` returns `0` or `-ETIMEDOUT`.
`lima_sched.c:506` (`lima_sched_run_job`, before the MMU page-table switch) and
`lima_sched.c:828` (`lima_sched_recover_work`) both call it as a void statement.
This is faithful to upstream — `/opt/bzdos/linux-work/linux-6.12/drivers/gpu/drm/lima/lima_l2_cache.c`
does the same — so it is an inherited design, not a porting slip.

The deadline was raised from upstream's `1000us` to `20000us` on 2026-08-20
(`lima_l2_cache.c:101`, commit `afe2591`). Measured, per that commit message:
**41 timeouts across six hours of presenting runs** before, **0** after. The
reason the old bound was meaningless is worth keeping: LinuxKPI's `ktime_get()`
is `getnanouptime()`, the coarse clock, which only advances on a tick — so at
hz=1000 upstream's "1000 us" was really one to two milliseconds and no sub-tick
deadline expressed that way means anything.

**Unproven.** Whether an abandoned flush ever actually produced a visibly wrong
frame. Nobody looked for stale pixels while timeouts were happening; the 41
occurrences are counted, their consequences are not. And 0-of-anything is not
proof the race is gone — it is proof the deadline is no longer the binding
constraint.

**Next step.** Make the failure observable before making it fatal: a sticky
counter (`sysctl compat.linuxkpi.lima_l2_flush_timeouts`, in the style of the
counters at `drm/drm_gem_shmem_helper.c:178-186`) so a run can be checked after
the fact, and propagate the error out of `lima_sched_run_job()` as a job failure
rather than silently continuing. Both are small; the second is a behaviour change
that needs a soak before it can be trusted.

**Extraction.** No. It is upstream-parity behaviour; fixing it is an improvement
to offer upstream, not a prerequisite.

---

## 7. Every BO is mapped `LIMA_VM_FLAGS_CACHE`; `LIMA_VM_FLAGS_UNCACHE` is dead

> **CLOSED AS A DECISION, not as code (2026-08-21).** This item's own analysis
> already reached the right answer and it is worth making that explicit rather
> than leaving it looking open: **do not change the default.** It is upstream
> parity (Linux 6.12 has the same unconditional `pa | LIMA_VM_FLAGS_CACHE` and
> an equally unused `UNCACHE`), the failure mode it predicts was actively looked
> for and not found, and changing it would be exactly the kind of divergence
> from upstream an extraction should avoid. The cheap decisive experiment is
> recorded below if it is ever revisited.

**Claim.** There is no per-BO cache policy. A scanout buffer written by the PP and
read by the display engine gets the same cacheable Mali PTE as a texture.

**Evidence.** `lima_vm.c:118` — unconditional `pa | LIMA_VM_FLAGS_CACHE`.
`lima_regs.h:409` defines `LIMA_VM_FLAGS_CACHE`, `lima_regs.h:428` defines
`LIMA_VM_FLAGS_UNCACHE`, and nothing anywhere selects the latter (grep over the
whole port: one definition, zero uses). `lima_l2_cache.c:34-40` documents this as
deliberately deferred; `SCANOUT-IMPORT.md:177-191` and `:283-301` name the narrow
fix (thread a policy argument through `lima_vm_map_page()`).

**Unproven, and the important half.** *Whether it matters.* This is upstream
parity: Linux 6.12's `lima_vm.c:71` is the same unconditional
`pa | LIMA_VM_FLAGS_CACHE`, and its `LIMA_VM_FLAGS_UNCACHE` is equally unused
(`/opt/bzdos/linux-work/linux-6.12/drivers/gpu/drm/lima/lima_regs.h:281`). More
importantly, the failure mode the note predicts — stale pixels in a scanned-out
buffer — has been actively looked for and not found: EL2 scanned the presented
buffer from outside the guest and found the fragment shader's own colours in 20 of
35 sample points (`bzfb/tests/README-zerocopy.md` (bzdk-side, not in this repository), commit `ae6b548`), and
`limabench` reports pixel-exact results 4/4 runs. So the risk is theoretical
today, and the real flush (`lima_l2_cache.c`, since `c5885a0`) is what makes it
so.

**Next step.** Do not change the default. If it is ever revisited, the decisive
experiment is cheap and does not need a redesign: map *only* the presented
`gbm_surface` BO `UNCACHE` behind a sysctl and compare EL2-side scans of the
buffer over a long `limaflip` run against the cacheable default. Without a
measured difference this should stay as upstream has it.

**Extraction.** No. Changing it would be a *divergence* from upstream, which an
extraction should avoid.

---

## 8. The hand-built Mesa in `/usr/local` has no protection from `pkg`

> **ADDRESSED AT THE ROOT 2026-08-20** (not closed — the patch is written,
> not merged). The durable fix is not to protect a hand-built Mesa from
> `pkg`; it is to make `pkg` able to build the right one.
> `patches/freebsd-ports/mesa-dri-lima-gallium-option.patch` adds a `lima`
> option to `graphics/mesa-dri` following the `panfrost` pattern — three
> lines in the `Makefile` plus one `pkg-plist` entry, and deliberately no
> `libclc`/LLVM dependency, because lima's gpir/ppir compilers need
> neither. Verified to apply to a pristine ports checkout. Until it is
> merged, everything below still describes the real exposure.
>
> **AND THE EXPOSURE NOW HAS A NAME (2026-08-21).** `pkg check -s` on the guest
> reports exactly two checksum mismatches in the whole of `/usr/local`:
> `libexpat.so` and `libexpat.so.1`. Both are symlinks pointing at
> `libexpat.so.1.8.10`, hand-installed 2026-08-18 and owned by **no package**
> (`pkg which` -> "not found in the database"), shadowing the packaged
> `libexpat.so.1.12.2`. The hand-built Mesa resolves through that symlink
> (`ldd libgbm.so` -> `/usr/local/lib/libexpat.so.1`). So the concrete thing
> that breaks GL on this guest is not a vague "pkg upgrade of mesa-libs" -- it
> is anything that reinstalls **expat**, which would rewrite that symlink to
> 1.12.2 and relink Mesa onto a different library. Nothing else in `/usr/local`
> diverges from the package database (the rest of pkg's complaints are missing
> `__pycache__/*.pyc`, which is benign).

**Claim.** Any `pkg install` that pulls in `mesa-libs` or `mesa-dri` replaces the
guest's lima-capable Mesa with one that cannot drive this GPU. The threat is real;
the reason usually given for it is wrong.

**Evidence, measured on this host against `pkg.freebsd.org/FreeBSD:15:aarch64/latest`.**

The claim "FreeBSD's packaged `mesa-dri` contains no `*_dri.so` at all" is
**false**. `mesa-dri-26.1.7.pkg` ships 59 files, 49 of them `*_dri.so` under
`/usr/local/lib/dri/` — including `sun4i-drm_dri.so` and `panfrost_dri.so`. All 49
are symlinks to one 110416-byte `libdril_dri.so` loader shim.

The real gap is one level down, and is decisive:

```
$ strings libgallium-26.1.7.so | grep 'driver missing'
...
lima: driver missing
...
$ strings libgallium-26.1.7.so | grep -cE 'LIMA_DEBUG|gpir|ppir|Mali-400'
0
```

`lima` is in the loader's device table but the gallium driver is **not compiled
in** (contrast `panfrost`, which is present and does *not* appear in the
"driver missing" list). There is also no `dri/lima_dri.so` symlink. So the
packaged Mesa can enumerate this GPU and then refuse it.

The collision is exact — same prefix, same filenames. `mesa-libs-26.1.7` installs
`/usr/local/lib/libgbm.so{,.1,.1.0.0}`, `/usr/local/lib/libEGL_mesa.so{,.0,.0.0.0}`,
`/usr/local/lib/libGLX_mesa.so{,.0,.0.0.0}`, `/usr/local/lib/gbm/dri_gbm.so`,
`/usr/local/lib/libgallium-26.1.7.so`, `/usr/local/etc/libmap.d/mesa.conf`,
`/usr/local/share/glvnd/egl_vendor.d/50_mesa.json`. Our build is 26.2.0, so
`libgallium-26.2.0.so` would survive by soname — but `libgbm.so.1.0.0`,
`libEGL_mesa.so.0.0.0`, `gbm/dri_gbm.so`, `mesa.conf` and `50_mesa.json` are
byte-for-byte path collisions, and those are the files that decide which driver
gets loaded. `mesa-dri` is a dependency of `mesa-libs`' typical consumers, and
`xorg-server`/`xf86-video-*` pull `mesa-libs` in.

**Unproven.** What `pkg` actually *does* to a file that exists on disk but is
owned by no package — overwrite, or refuse. `pkg`'s conflict detection is between
packages; an unowned file is not a package. Untested here, and untestable without
the guest. Also unproven: the suggested mitigation. `pkg lock` locks an
**installed package**; our Mesa is not a package, so there is nothing to lock —
`pkg lock mesa-libs` cannot protect a hand-built tree, and saying otherwise would
be a false sense of safety.

**Next step, in order of durability.** (a) Record the manifest now: a
`find /usr/local -newer <marker>` listing of the hand-built Mesa, committed to
this repo, so it can be identified and restored. (b) Rebuild Mesa with
`--prefix=/opt/mesa-lima` and drive it with `LD_LIBRARY_PATH` /
`__EGL_VENDOR_LIBRARY_FILENAMES`, leaving `/usr/local` to `pkg` — this is the
only mitigation that survives an unattended `pkg upgrade`. (c) Failing that,
install a placeholder `mesa-libs`/`mesa-dri` and `pkg lock` those, so `pkg` has
something to refuse to touch. (a) is free and should happen regardless.

**Extraction.** No, but it is the first thing a downstream FreeBSD user hits, so
it belongs in the deliverable's README as a known packaging conflict: the FreeBSD
port of Mesa does not enable the lima gallium driver.

---

## 9. `glReadPixels` on an imported dma-buf FBO wedges the GPU past module unload

> **RESOLVED 2026-08-21 — it was not the GPU.**
> The half of this item that said "the wedged state survives `kldunload` of
> lima and bzfb — only a guest reboot clears it" was a misattribution. What
> survived the unload was a **dangling sysctl node**: `drm_sysctl_init()` put
> the shared `hw.dri` node into the per-device context, `sysctl_ctx_free()`
> then failed with EBUSY (that node's other children are drm.ko's module
> parameters, owned elsewhere) and removed nothing, and
> `drm_sysctl_cleanup()` freed the struct anyway. Mesa reads
> `hw.dri.<N>.busid`, so after one reload every GL program failed with
> "MESA-LOADER: failed to retrieve device information" — indistinguishable
> from a wedged GPU, and equally only curable by a reboot.
>
> Two worse consequences of the same defect were found on the way: an
> **unprivileged `sysctl -a` panicked the kernel** (`vm_fault failed`, with a
> faulting address that is recycled ASCII text), reproduced twice; and the
> per-device subtree leaked 6 nodes per load, cumulatively.
>
> Fixed in `patches/drm-kmod/drm-kmod-dri-sysctl-lifecycle.patch`. Verified
> over 5 load/GL/`sysctl -a`/unload cycles: GL works every cycle, `sysctl -a`
> returns 0 every cycle, 0 leaked nodes every cycle.
>
> **The remaining half is now settled too: it does not reproduce.**
> `hal/bzfb/tests/limaread.c` (new) does exactly the thing warned against --
> `SCANOUT|RENDERING` bo, exported, re-imported as an `EGLImage`, attached to an
> FBO, cleared, then `glReadPixels`. It completes with `0 of 16384 pixels wrong`
> on three consecutive runs, after which `limabench` still passes and
> `kldunload lima` still takes 0.06 s. The warning in
> `bzfb/tests/README-zerocopy.md` (bzdk-side, not in this repository) has been retracted there with the evidence.
> **This item is closed.**

**Claim.** A single ordinary GL call from unprivileged userspace puts the GPU in a
state that survives `kldunload` of both lima and bzfb and needs a guest reboot.
Nothing guards it.

**Evidence.** `bzfb/tests/README-zerocopy.md` (bzdk-side, not in this repository) "Do not do this": "`glReadPixels`
on an FBO whose colour attachment is an imported dma-buf **hangs the GPU on the
first frame**, and the wedged state survives `kldunload` of lima and bzfb — only a
guest reboot clears it." Same document records that this was found while trying to
provoke the tile resolve, and commit `261ef05` had to bound pipe teardown
specifically because `kldunload lima` then hung forever, followed by
`kldunload drm`.

**Unproven.** Where the wedge actually lives. "Survives module unload" points at
GPU-internal state or a stuck AXI transaction rather than driver state, but that
was inferred from the symptom, not localised. Whether the PMU reset path
(`lima_pmu.c`) is capable of clearing it has not been tried in that state.
Whether the wedge is specific to *imported* dma-buf attachments or to any
`glReadPixels` on a foreign-allocated target.

**Next step.** Two separable things. Diagnosis: reproduce with the GP/PP status
registers dumped from CPU1 (the debug core) rather than from the wedged guest, and
try a full PMU power-cycle of the GPU domain as a recovery. Containment, which is
cheaper and independent: on `kldload`, issue the same reset sequence
`lima_sched_pipe_fini`'s timeout branch uses, so a fresh load recovers a GPU the
previous load left wedged.

**Extraction.** Yes if unfixed — a userspace-triggerable unrecoverable GPU hang
is a shipping-blocker-class defect for a DRM driver, whoever's dma-buf it is.

---

## 10. The wedge-recovery branch that fix added has never executed

> **CORRECTED 2026-08-21 — the opposite was true: it was the ONLY branch ever
> taken.** The poll loop tested `pipe->current_task != NULL`, which is not a
> test for "a task is in flight": upstream lima sets that pointer when a job
> goes to the hardware and clears it only in the timeout/error path, so after
> any successful render it stays set forever. Every `kldunload lima` therefore
> waited the full 2000 ms per pipe, printed
> `[drm ERROR] pp: pipe still busy 2000ms into teardown`, and force-reset both
> pipes — while the hardware reported `int_state=0 status=0`, i.e. idle.
> Measured: **4.09 s** per unload, two false DRM_ERRORs, a needless GPU reset
> every time.
>
> Fixed by testing the task's **fence** instead (`pipe_task_in_flight()`), which
> is signalled exactly when the hardware is done and stays unsignalled for a
> genuine wedge. Unload is now **0.06 s** with no error line and no reset, and
> the clean-teardown path drops the VM reference the always-taken branch used
> to drop.
>
> The branch is now genuinely never taken on healthy hardware, so this item's
> original request stands satisfied a different way: a new
> `sysctl compat.linuxkpi.lima_fake_wedge=1` forces it deterministically. Run
> live, it does what it claims: bounded at 2000 ms per pipe, prints the
> DRM_ERROR, calls `task_error()` on both pipes, the unload **completes**, no
> UMA "not empty" warning, no panic from force-signalling with `-ENODEV`, and
> a subsequent `kldload` attaches and renders. Every "Unproven" claim above is
> now measured.

**Claim.** Commit `261ef05` bounded pipe teardown so a wedged GPU cannot hang
`kldunload` forever. The reordering half runs on every unload; the timeout half
has never run, on hardware or anywhere.

**Evidence.** `lima_sched.c:889` `#define LIMA_SCHED_PIPE_FINI_TIMEOUT_MS 2000`,
and the branch it guards in `lima_sched_pipe_fini()` (`lima_sched.c:949`+):
`DRM_ERROR`, force-reset through `pipe->task_error()`, force-signal the stuck
fence with `-ENODEV`, then tear down anyway. Commit `261ef05`'s own first line:
"UNTESTED ON HARDWARE. Written and reviewed from source inspection only ... could
not be verified live because the board/serial port were in use by another
debugging session." The reorder half (calling `lima_sched_pipe_fini()` before
`lima_{gp,pp}_pipe_fini()` destroys the task slab) does run on every unload, and
its observable symptom — `Freed UMA keg (lima_pp_task) was not empty (2 items)` —
is the thing to check for its regression.

**Unproven.** Every claim about the timeout branch: that 2 s is long enough for
the scheduler's own 500 ms job timeout to act first; that
`pipe->task_error()` can reset a genuinely wedged Mali; that force-signalling with
`-ENODEV` releases every `dma_resv`/ioctl waiter rather than tripping an assertion
in `drm_sched_stop()`.

**Next step.** It has a natural trigger — item 9. Run the `glReadPixels`-on-
imported-dma-buf case to wedge the GPU deliberately, then `kldunload lima`, and
check for: the `DRM_ERROR` line, an unload that completes, no UMA "not empty"
warning, and a subsequent `kldload` that attaches. Failing that (or first, since
it needs no board), a fault-injection knob — `sysctl
compat.linuxkpi.lima_fake_wedge=1` making `pipe->current_task` never clear —
exercises the branch deterministically off a real hang. That is the cheaper and
more repeatable route and is what the code deserves before it is relied on.

**Extraction.** No, but the timeout branch should carry its "never executed"
status in a comment until it has.

---

## 11. Eight upstream fixes are written and none is submitted; three have no write-up

> **UPDATE 2026-08-21 — TEN, and the bookkeeping this item was about is now
> obsolete.** `patches/UPSTREAM-INDEX.md` is the submission index: all ten
> fixes across three foreign trees, each with its target tree, patch file,
> where its write-up lives, apply order, and whether this board depends on it.
>
> The two that had "none — only the commit message" now have both: they were
> extracted from commits `134a4b503` / `39090bd6f` into a proper two-patch
> series (`freebsd-linuxkpi-01-dma-map-sg-multipage.patch`, then
> `-02-dma-alloc-coherent-memattr.patch` — verified to reproduce the committed
> tree byte-for-byte when applied in that order) and written up in
> `../patches/UPSTREAM-freebsd-linuxkpi-dma.md`.
>
> Four fixes carry their reasoning as a header comment inside the patch file
> rather than a separate `UPSTREAM-*.md`. That is deliberate, not a gap: a
> patch that travels with its own justification is harder to mis-send. Only #6
> (`dma-buf-mmap`) is genuinely thin, with prose in `README.md` alone.
>
> **The index also names which one to send first**, which the old count did
> not: the `hw.dri` sysctl lifecycle fix (#9) is a local denial of service any
> user with a shell can trigger on any FreeBSD machine running a DRM driver
> without debugfs — no lima, no Mali and no bzdOS needed to reproduce.
> Everything else is a porting fix.
>
> Submission still needs the author's own accounts, so the count of *submitted*
> stays zero.

**Claim.** The port's third-party-defect haul is eight distinct fixes across two
foreign trees. Five are documented well enough to send; three are not documented
at all. None has been submitted, and submission needs the author's own accounts.

**The haul, as it stands in the tree.**

| # | fix | tree / version | where it lives | write-up | still needed locally? |
|---|---|---|---|---|---|
| 1 | `dma_alloc_coherent()` returned CACHEABLE memory on arm64 (`VM_MEMATTR_DEFAULT` is write-back; now `VM_MEMATTR_UNCACHEABLE` on aarch64/arm/riscv) | freebsd-src 15.1, `sys/compat/linuxkpi/common/src/linux_pci.c` | commit `39090bd6f` on branch `linuxkpi-dma-fix` | **none** — only the commit message | yes; this is the fix that produced the first frame |
| 2 | `dma_map_sg()` cannot map multi-page lists on non-coherent arm64 (`nsegments = 1` + accumulating `sync_count` -> `EFBIG`) | freebsd-src 15.1, same file | commit `134a4b503` | **none** | yes |
| 3 | `aw_ccung` transposed gate/lock args on A83T CPUX PLLs | freebsd-src 15.1, `sys/dev/clk/allwinner/ccu_a83t.c` | commit `68fe3114f` **and** `patches/freebsd-src/freebsd-ccu-a83t-cpux-gate-lock.patch` (duplicated) | `patches/UPSTREAM-freebsd-ccu-a83t-cpux-gate-lock.md` (`:3` "**not submitted**") | no — A83T, not this board; carried for upstream only |
| 4 | `FRAC_CLK`/`NKMP_CLK`/`M_CLK`... clknodes had no `get_gate` method at all (7 files) | freebsd-src 15.1, `sys/dev/clk/allwinner/aw_clk_*.c` | `patches/freebsd-src/freebsd-allwinner-clk-get-gate.patch`, applied uncommitted in the worktree | `patches/freebsd-allwinner-clk-gate-audit.md` | yes — the diagnostic that found #5 |
| 5 | `ccu_a64.c` `pll_gpu_clk` declares a gate bit but omits `AW_CLK_HAS_GATE`, so `clk_enable()` silently no-ops | freebsd-src 15.1, `sys/dev/clk/allwinner/ccu_a64.c` | **uncommitted working-tree edit only** (item 2) | `patches/UPSTREAM-freebsd-ccu-a64-pll-gpu.md` (23 KB) | yes — without it the GPU cannot be clocked |
| 6 | `drm_dev_alias()` leaks its `/dev/dri` cdev; the next alloc of the same minor panics in `make_dev_alias_v()` | drm-kmod `drm_v6.6.25_13` (`11252e8b90`) | `patches/drm-kmod/drm-kmod-dev-alias-lifecycle.patch`, applied to both drm-kmod trees | `patches/UPSTREAM-drm-kmod-dev-alias.md` | yes |
| 7 | drm-kmod's dmabuf had no `dma_buf_mmap()`, so imported PRIME buffers could not be `mmap()`ed | drm-kmod, same tag | `patches/drm-kmod/drm-kmod-dma-buf-mmap.patch`, applied | prose section in `patches/README.md`, not an `UPSTREAM-*.md` | yes to build (`Makefile:33`: without it `drm_gem_shmem_helper.c` will not compile) |
| 8 | `drm_add_busid_modesetting()` assumes every DRM device is PCI | drm-kmod, same tag | `patches/drm-kmod/drm-kmod-nonpci-busid.patch` — **unusable**, item 1 | **none** | yes |

**Unproven.** Whether #3 is correct: its own write-up says "**Hardware tested:**
none. ... the evidence is the macro signature plus the file's own internal
inconsistency, not a measurement." There is no A83T board here. #1 and #2 are
hardware-verified but only on one SoC.

**Next step.** Cheapest first: write the two missing `UPSTREAM-*.md` reports for
#1 and #2 (they are the most valuable of the eight — they affect every
non-coherent-DMA arm64 FreeBSD board with a LinuxKPI driver, not just Allwinner),
and one for #8. Then submit: #1, #2, #4, #5 to FreeBSD Bugzilla/Phabricator; #3
flagged as analysis-only; #6, #7, #8 to the drm-kmod GitHub. `patches/README.md`
notes this project has no drm-kmod contribution path set up — that is the actual
blocker for three of them.

**Extraction.** Yes, all of #4-#8 — they are hard prerequisites for the port
building and running anywhere, and a standalone deliverable has to ship them or
name the upstream revisions that contain them.

---

## 12. `hvfb/` is dead code, and `SCANOUT-IMPORT.md` (bzdk-side, not in this repository) documents a route that was measured not to work

> **RESOLVED 2026-08-21.** The boundary is now visible in the tree rather than
> only in prose: both artefacts moved to `bzdos/` (with a `bzdos/README.md` (bzdk-side, not in this repository)
> saying why they are kept and that nothing depends on them), and
> `bzdos/SCANOUT-IMPORT.md` (bzdk-side, not in this repository) carries a status note at its head stating that its
> recommended route was measured not to work and that its geometry arithmetic is
> stale. New `EXTRACTION.md` is the manifest this item asked for: a TRAVELS list,
> a DOES NOT TRAVEL list, the judgement call about `lima_ccu_debug.c` spelled
> out, and the instruction to REWORD rather than delete the 19 `bzdOS` comments
> in travelling sources — each records a real finding. Re-verified by grep, not
> assumed: nothing outside `bzdos/` references `hvfb`, and lima still builds
> after the move.

**Claim.** Two bzdOS-specific artefacts should be marked as non-travelling; one of
them is also dead.

**Evidence.**
- `hvfb/lima_hvfb.c` (23835 bytes) + `hvfb/lima_hvfb_uapi.h` + `hvfb/Makefile`.
  Nothing outside `hvfb/` references it: grep over the whole of `/opt/bzdos/bsdOS`
  for `lima_hvfb|hvfb` in `*.sh`, `Makefile`, `*.mk` returns only `hvfb/Makefile`
  itself. `SCANOUT-IMPORT.md:36-40` says it "has never been built or run" — still
  literally true. Its function was taken over by `hal/bzfb` (commit `08e8a07`
  onward) and then `hal/bzkms` (`d7c6653`). It carries 9 of the port's 10 `bzdOS`
  source references.
- `SCANOUT-IMPORT.md` (bzdk-side, not in this repository) (48982 bytes) recommends at `:10-13` the import route that
  `bzfb/tests/README-zerocopy.md` (bzdk-side, not in this repository) later measured as writing the imported dma-buf
  exactly once and never again, at ~400 fps, after eliminating nine hypotheses one
  by one. Its geometry arithmetic at `:338-345` is for `HDMI_FB_BASE 0x4D000000`,
  1280 wide, stride 5120; the live window is 1120x276 stride 4480 at
  `0x4b000000`.

**Extraction, and the good news.** Everything else travels as-is. All `bzdOS`
references in the port's tracked `.c`/`.h` files are in **comments only** —
verified exhaustively: `lima_mmu.c:324`, `lima_device.c:521`,
`drm/drm_gem_shmem_helper.c:49`, `drm/drm_gem_shmem_helper.c:1075`,
`tests/lima_ioctl_smoke.c:85`. (`bus_if.h`, `device_if.h`, `pci_if.h` also match
but are generated build artefacts, not sources.) So extraction needs **no code
changes** — only a manifest saying what ships, and the cross-references in item 4
repointed.

**Unproven.** Whether `hvfb/` has any residual value as a reference for a future
non-bzdOS scanout importer. It is 24 KB of never-compiled code; the judgement is
that git history preserves it adequately.

**Next step.** Add an `EXTRACTION.md` (or a section in `README-arm64.md`) with two
lists: TRAVELS (`lima_*.c/h`, `drm/`, `linux/`, `Makefile`, `patches/`, `tests/`
minus the pre-Mesa ones) and DOES NOT TRAVEL (`hvfb/`, `SCANOUT-IMPORT.md` (bzdk-side, not in this repository),
`lima_ccu_debug.c` — Allwinner-CCU debug scaffolding — and the `bzdOS` comment
references, which should be reworded rather than deleted since they explain real
findings). Then delete `hvfb/` or move it under a `bzdos/` subdirectory so the
boundary is visible in the tree rather than only in prose.

---

## 13. bzkms's vblank is a callout, and its rate cannot express the mode

> **NOT PART OF THIS EXTRACTION.** `bzkms` is the bzdk-side DRM/KMS device (see
> `EXTRACTION.md`) and does not ship here. Resolved on that side 2026-08-21 —
> the vblank is now observed from the real panel rather than generated by a
> local timer — but nothing in this repository depends on it.

## 14. Smaller, cheap, low-risk

> **WORKED THROUGH 2026-08-21.** Done: `bzkms/.gitignore` written from
> `bzfb/`'s template and the three generated headers added to
> `lima/.gitignore` (`bus_if.h` needed an exact-line check — it is a substring
> of the `ofw_bus_if.h` already there, which is how it had been missed);
> `mesa-feasibility.md` is tracked; `contigfree(9)` replaced by `free()` with
> the deprecation quoted; **"PinePhone Pro" corrected in all 11 places** (it is
> RK3399/Mali-T860 — a different GPU family — while this is a Banana Pi M64,
> A64/Mali-400 MP2; `lima_pmu.c`'s existing explanation of the error is kept
> deliberately); the `Makefile`'s "REBOOT THE GUEST rather than
> kldunload/kldload" note replaced, because item 9's fix made reloading work;
> the contiguous-BO path written up as **deviation 5** in
> `drm/drm_gem_shmem_helper.c` with the DE2/no-IOMMU reason and the
> extraction-reviewer question named; that file's "NOTHING IN THIS FILE HAS RUN
> ON HARDWARE YET" banner corrected (it has, extensively — what has still never
> run is `drm_gem_shmem_purge()`, and that half of the warning is kept);
> `tests/README.md` extended from 2 of 8 files to all 8, including the two
> host-side unit tests and the `gmake test-layout` / `test-shmem` targets that
> reach them.
>
> **One item was refuted rather than fixed.** "`lima_pp_task_validate()`
> validates almost nothing ... contained today only by stage-2 isolation and a
> single-user guest" overstates the risk. Those fields are GPU **virtual**
> addresses, and every PP fetch goes through that PP's own MMU
> (`ppmmu0..ppmmu5`) programmed with the submitting context's page tables — so
> a bogus value either hits the context's own mappings or raises a GPU page
> fault that `lima_mmu_page_fault_resume()` already handles. It cannot name host
> memory or another context's buffers. The per-PP MMU *is* the containment, and
> it is the same mechanism upstream relies on. Written up in the function's own
> contract, including why walking the page tables per submit is deliberately not
> done.
>
> **A new bug fell out of the work**, worth recording because it was
> self-inflicted: the non-PCI busid fallback had been made a fixed
> `pci:0000:00:00.0`, which is fine with one DRM device and breaks with two —
> lima and bzkms advertised the same busid and Mesa's loader could not tell them
> apart ("failed to retrieve device information"). Only visible once a second
> test (`limaread`) exercised gbm on a card node with both loaded. The fallback
> now varies by sysctl node index; measured `pci:0000:00:00.0` and
> `pci:0000:00:01.0`, with all three GL tests passing with both devices present.

- **`bzkms/` has no `.gitignore`.** Ten generated files (`bzkms.o`, `.ko`, `.kld`,
  `export_syms`, `machine`, and five generated `*_if.h`) show as untracked noise
  in `git status`. `bzfb/.gitignore` is the template. `lima/.gitignore` is missing
  `device_if.h`, `bus_if.h`, `pci_if.h` for the same reason — those three appear
  as `??` today.
- **`mesa-feasibility.md` is untracked** (35223 bytes, never committed). Either
  commit it or delete it; leaving 35 KB of superseded analysis outside git is the
  worst of both.
- **`contigfree(9)` is deprecated.** `bzkms.c:421`; the declaration in
  `/opt/bzdos/freebsd-src-earlyboot-wt/sys/sys/malloc.h:181` is literally
  preceded by `/* contigfree(9) is deprecated. */`. Builds clean on 15.1, will
  not on a future release. Replace with `free(bo->kva, M_DEVBUF)`.
- **"PinePhone Pro" is the wrong board, named in 9 places.** `Makefile:45`,
  `lima_regs.h:38`, `lima_drv.c:14,34,609`, `lima_pp.h:44`, `lima_device.h:164`,
  `lima_vm.c:99`. `lima_pmu.c:16-17` already documents that it is wrong
  ("PinePhone *Pro* is RK3399/Mali-T860, not A64; plain PinePhone and this board
  (Banana Pi M64) are A64/Mali-400"). Also `drm/lima_drm.h:3` still says
  "Porcupine v0.3, PinePhone Pro Mali-400". A deliverable that claims support for
  a board with a different GPU family is a factual defect, not a typo.
- **Reload requires a guest reboot.** `Makefile:36-37`: "REBOOT THE GUEST rather
  than kldunload/kldload -- reloading leaves drm's sysctls behind ('can't re-use a
  leaf') and breaks eglInitialize." That is a ninth drm-kmod defect with no
  investigation and no patch. Both `lima/Makefile:174` and `bzkms/Makefile:44`
  set `DRM_SYSCTL_PARAM_PREFIX=_dri`, so the two drivers share that sysctl tree —
  worth checking whether loading both makes it worse.
- **`lima_pp_task_validate()` validates almost nothing.** `lima_pp.c:576` checks
  `num_pp` range and padding; whatever userspace puts in
  `plbu_array_address[]`/`wb[]` goes to hardware
  (`PLAN-mesa-lima.md:468-472`, still exactly true). Contained today only by
  stage-2 isolation and a single-user guest. A render node is nominally
  unprivileged.
- **`drm_gem_shmem_purge()` has never been called.** Deviation #4 in
  `drm/drm_gem_shmem_helper.c:56-68`; `drm_gem_shmem_zap_ptes()` at `:804-860`
  with `pmap_remove_all()` at `:855`, reached from `:921`. Grep over `hal/` finds
  no caller — no madvise ioctl (`lima_drv.c:354-358` exposes GET_PARAM /
  GEM_CREATE / GEM_INFO / GEM_SUBMIT / GEM_WAIT only) and no shrinker. Latent, not
  active; it becomes active the moment a shrinker is added.
- **The contiguous-BO path is an undocumented divergence.**
  `drm/drm_gem_shmem_helper.c:113-260`, `CONTIG_MAX_PAGES 4096` at `:145`,
  sysctls at `:178-186`, switch at `:195`. Upstream Linux never asks for
  contiguity. It is absent from the header's own deviation list (`:37-72`) and
  from `drm/PURGE-NOTES.md`. Add it as deviation #5 — an extraction reviewer will
  ask why it is there.
- **`tests/README.md` documents 2 of 8 files.** Undocumented:
  `lima_ioctl_smoke.c`, `lima_pp_clear.c`, `../tests/PP-CLEAR-FRAME.md` (all pre-Mesa, no
  recorded run since 2026-08-11), `test_lima_math.c` and `test_shmem_logic.c`
  (host-side unit tests, reachable only via `Makefile:190-194`
  `gmake test-layout` / `test-shmem`, which the README never mentions). The GL and
  display tests actually in use live in an undocumented second directory,
  `hal/bzfb/tests/` (`limashow.c`, `limaflip.c`, `limacube.c`, `limakms.c`), and
  the cross-reference exists in only one direction.
- **The bzfb/bzkms milestones are recorded only in commit messages and
  `bzfb/tests/README-*.md`.** There is no `hal/bzfb/README` or `hal/bzkms/README`.
  `MALI-STATUS.md`, which is where a reader looks, predates both.

---

## NOT loose ends — measured, and fine

Recorded so the next person does not spend a day on them.

- **The L2 flush timeout itself.** Looks alarming in `dmesg`
  (`lima_platform_driver0: l2 cache flush timed out`) and is listed as "Still
  open" in `bzfb/tests/README-zerocopy.md` (bzdk-side, not in this repository). It was the *deadline*, not the
  hardware: LinuxKPI's `ktime_get()` is the coarse clock, so upstream's 1000 us
  never meant 1000 us. 41 occurrences in six hours became 0 with a 20 ms bound
  (`lima_l2_cache.c:101`, commit `afe2591`). The L2 register base was verified
  correct at the same time — `init` reads back a sensible 64K/4-way/64-byte-line
  geometry through the same `iomem`. What remains open is only the ignored return
  value (item 6), not the timeout.
- **The contiguous BO path does not leak.** Measured around the module rather than
  the workload (`bzfb/tests/README-zerocopy.md` (bzdk-side, not in this repository), commit `a641ad7`): no modules
  128744 wired pages; loading both +449; one presenting run +1745; unloading both
  returns all 2190, net **+4**; six further runs add 53. An earlier commit
  (`95200e6`) claimed a leak and `95a672d` retracted it. Do not re-open this
  without a measurement around the module.
- **The flip is atomic against scanout.** The obvious worry — "no vsync, so a
  frame can be repointed mid-scanout" — is wrong for the flip itself:
  `DE_GLB_DBUFF` latches the layer address at the next vertical blank
  (commit `afe2591`). The tearing that *was* seen came from releasing the
  presented buffer back to gbm immediately, so the next frame was drawn into
  memory the display was still reading; fixed by holding the buffer until its
  successor is on screen, and confirmed by the presented addresses alternating
  (`0x734a2000` / `0x6deb0000`) instead of repeating.
- **Isolation held throughout the zero-copy work.** EL2 scanned guest buffers
  from outside the guest; the HUD's own framebuffer was sampled at the same time
  and was untouched. 153471 accepted doorbell addresses over a session, zero
  rejected.
- **`lima.ko` and `bzkms.ko` do not fight over symbols.** Both build with an
  empty `export_syms` (0 bytes in each directory), so lima's private copies of
  `drm_gem_shmem_*`, `drm_timeout_abs_to_jiffies()` and the platform-device bridge
  are not visible to bzkms and cannot collide. The `LIMA_PROVIDE_*` knobs
  (`Makefile:81-110`) exist for the day drm-kmod exports them.
- **"lima writes an imported dma-buf exactly once."** Fully characterised, nine
  hypotheses eliminated one at a time by experiment
  (`bzfb/tests/README-zerocopy.md` (bzdk-side, not in this repository)), and then routed around: the working answer is
  that lima allocates the buffer (`gbm_surface`) and the display is repointed at
  it, not the reverse. This is not a bug waiting to be fixed; it is a closed
  investigation with a shipped alternative. Read the table before re-deriving it.
- **The two `drm-kmod` source trees are in sync.** `/opt/bzdos/drm-kmod` and
  `/opt/bzdos/drm-kmod-src` differ only in `.git`/`.github`/`.gitignore`
  (`diff -rq`), and both carry the dev-alias and `dma_buf_mmap` patches. The
  inconsistent defaults are worth fixing (item 4) but there is no live divergence
  today.
- **`test_lima_math.c` is not stale.** Its constants (`test_lima_math.c:18-35`)
  still match `lima_vm.h:32-46` and `lima_vm.c:29-40` after the 2026-08-18
  rewrite. It is undocumented, not broken.
- **`dma_alloc_coherent()` on arm64.** Was the decisive defect (cacheable memory
  handed to a device whose contract is "no cache maintenance required"), and it is
  fixed and committed (`39090bd6f`). `lima_vm.c:96-99`'s comment about using
  `dma_alloc_coherent` instead of `dma_alloc_wc` is correct and current.
- **`lima_pmu.c`.** `MALI-STATUS.md:305-307` still reads "**Status: built clean,
  not verified past the build.** It does not lift the stall". That text predates
  `c5885a0`; the PMU sequence runs and attach proceeds past it. The line is stale
  documentation (item 5), not an open defect.

---

## What could not be settled without the board

Listed explicitly so nobody re-derives them from source and thinks they are done.

1. Whether the deployed guest `drm.ko` carries the `nonpci-busid` fix. It cannot
   (item 1), but that is inference from the patch being unusable, not a read of
   the running module.
2. Whether an abandoned L2 flush ever produced a visibly wrong frame (item 6).
3. Whether the `lima_sched_pipe_fini()` timeout branch works (item 10).
4. Where the `glReadPixels`-on-imported-dma-buf wedge lives, and whether a PMU
   power-cycle clears it (item 9).
5. What `pkg` does to the hand-built `/usr/local` Mesa, and what is currently
   installed in the guest (item 8). The package *contents* were measured on this
   host; the guest's state was not.
6. Where 59.7 fps comes from given a 62.5 Hz callout (item 13).
7. Whether loading both `lima.ko` and `bzkms.ko` makes the `_dri` sysctl
   "can't re-use a leaf" reload problem worse (item 14).
8. A clean-from-scratch cross-build. `bmake` with the header recipe reports
   nothing to do — every object in `hal/lima` is current against its `.depend`,
   and `lima.ko` (110552 bytes, 2026-08-20 19:45) is newer than every source —
   but a full rebuild was deliberately not run, because it writes into the same
   in-tree object files the live session is using.
