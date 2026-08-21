# hal/lima on aarch64 — build status

Measured 2026-08-04 on the Linux build host, FreeBSD 15.1 (`__FreeBSD_version`
1501000), drm-kmod tag `drm_v6.6.25_13`.

## Result: it builds, and every symbol it references now exists somewhere.

**SUPERSEDED 2026-08-11 — it loads and attaches on real hardware now.** This
heading used to end "It has still never been loaded, and could not attach if it
were", which was true when written and is not any more:

```
lima_platform_driver0: gp/pp0/pp1 - mali400 version major 1 minor 1
[drm] Initialized lima 1.1.0 20191231 for lima_platform_driver0 on minor 0
```

on a Banana Pi M64 (Mali-400 MP2 r1p1), FreeBSD 15.1 guest under the bzdOS EL2
hypervisor, with `/dev/dri/card0` and `/dev/dri/renderD128` present. Getting
there needed three fixes outside this file's scope — a drm-kmod `/dev/dri` alias
leak that panicked any second probe, a real `lima_pmu.c`, and the discovery that
`ccu_a64.c` cannot enable `PLL_GPU` at all. See `MALI-STATUS.md`.

One thing that heading's successor must not overclaim: attach is **not**
rendering.

**That caveat has since been overtaken by events, and this line said so far too
long.** It used to end "no submit path has been exercised and no Mesa/lima
userland exists on the board", which was accurate on 2026-08-11 and false from
2026-08-19 onward. Measured since: Mesa 26.2 with `-Dgallium-drivers=lima`
cross-built and installed on the board, textured and depth-tested geometry
rendering (2420 draw calls per frame), zero-copy presentation at 1030 fps, and
3601 DRM/KMS page flips with 0 refused in a 60-second run paced by the real
panel vblank. See `MALI-STATUS.md` for the numbers and how each was taken.

Attach needed an opt-in `kenv hw.lima.force_pll_gpu=1` stopgap for the first few
hours. The guest kernel has since been rebuilt with the real clk(9) fix
(`AW_CLK_HAS_GATE` on `ccu_a64.c`'s `pll_gpu_clk`, see
`tftpboot/rebuild-2026-08-11/README.md` (bzdk-side, not in this repository)) and attach now works with that tunable
unset. The build-status measurements below are unchanged and still accurate.

```
$ DRM_KMOD_SRC=/path/to/drm-kmod ./infra/scripts/build-lima-arm64.sh
OK: /opt/bzdos/fbsd-obj/lima-arm64/lima.ko
lima.ko: ELF 64-bit LSB shared object, ARM aarch64, version 1 (SYSV)
```

| Metric | Value |
|---|---|
| `.c` files compiling clean for aarch64 | **15 / 15** (13 Lima + 2 DRM infrastructure) |
| Compile flags | full FreeBSD kmod set, **including `-Werror`** |
| Warnings | 2 (both `unused function`, see below) |
| Genuine port bugs found | **0** |
| `lima.ko` size | 72 088 bytes, 15 objects, links via `ld -Bshareable` |
| Undefined symbols implemented nowhere | **0** (was 11) |
| Kernel-free tests | 49 / 49 layout + 47 / 47 shmem logic |
| External headers required from drm-kmod | **31** (not the whole tree) |

The two warnings are the interesting part of the build, not the clean compile:
`lima_read_block` and **`lima_pdev_probe`** are unused. A driver whose probe
routine is dead code has no attach path.

## Both stated blockers were removable; neither was the real one

The old Makefile hardcoded
`KOBJ_DIR=/usr/obj/usr/src/amd64.amd64/sys/BSDOS-SQUIRREL-amd64`, which pinned
the build to amd64 *and* required a finished kernel build. Both problems had one
fix: this driver needs exactly six generated headers — `device_if.h`,
`bus_if.h`, `pci_if.h`, `vnode_if.h` (+ the two `vnode_if_*` that come with it),
and no `opt_*.h` beyond the automatic `opt_global.h`. Listing them in `SRCS`
makes `sys/conf/kmod.mk` generate them into the module objdir from `${SYSDIR}`
alone. That is what drm-kmod's own `drm/Makefile` does. The Makefile now names
no architecture at all, and no prebuilt kernel obj tree is needed.

Verified header set, not guessed: it was recovered from the `.depend.*.o` files
left by the earlier completed amd64 build.

## The three real blockers, in the order they would bite (1 of 3 now closed)

### 1. ~~11 symbols that nothing on FreeBSD implements~~ — CLOSED 2026-08-04

`lima.ko` had 101 undefined symbols, of which 11 `drm_gem_shmem_*` were
implemented nowhere: drm-kmod `drm_v6.6.25_13` ships no
`include/drm/drm_gem_shmem_helper.h`, no
`drivers/gpu/drm/drm_gem_shmem_helper.c`, and no mention of `shmem` anywhere in
its `drm/Makefile`. The port papered over the *header* with 55 non-comment
lines of pure declarations, which is exactly why the link succeeded and a
`kldload` could not.

That header is now a real header and `drm/drm_gem_shmem_helper.c` (~640 LOC) is
a real FreeBSD implementation: `shmem_file_setup()`'s OBJT_SWAP vm_object as the
backing store, `shmem_read_mapping_page()`/`put_page()` for page wiring,
`pmap_page_set_memattr()` for write-combining (which makes `map_wc` work on
arm64, where Linux' x86-only `set_pages_array_wc()` leaves it write-back), and
`lkpi_vmf_insert_pfn_prot_locked()` under `VM_OBJECT_WLOCK` in the fault path,
because linuxkpi's fault contract (busy the pages, report `vm_pfn_first` /
`vm_pfn_count`) is not Linux'. Locking is upstream 6.6's: the object's
`dma_resv` covers `pages`, `pages_use_count`, `vaddr`, `vmap_use_count`, `madv`.

Closing it exposed a **second** symbol nothing defines:
`drm_timeout_abs_to_jiffies()`. drm-kmod ships `include/drm/drm_utils.h` but
`drivers/gpu/drm/drm_utils.c` **does not exist in the repository** (404 at the
tag) and `drm/Makefile` never references it. `drm/drm_utils_freebsd.c` now
provides it. Any DRM driver using the absolute-timeout wait ioctl (lima,
panfrost, v3d, msm) hits the same wall.

Measured on the same build host, same flags:

| Undefined symbols in `lima.ko` | before | after |
|---|---:|---:|
| total | 101 | 123 |
| resolved by the arm64 `GENERIC` kernel | 50 | 70 |
| resolved by `drm.ko` / `dmabuf.ko` | 40 | 53 |
| **implemented nowhere** | **11** | **0** |

The total went *up* because a real implementation calls real functions
(`drm_gem_object_init`, `drm_prime_pages_to_sg`, `dma_buf_vmap`,
`shmem_read_mapping_page`, `pmap_page_set_memattr`, …). The only row that
matters is the last one.

Method, so the numbers can be re-derived: `llvm-nm -u lima.ko` for the total;
`llvm-nm --defined-only` on the locally built
`arm64.aarch64/sys/GENERIC/kernel` for the kernel column (58 390 symbols); the
remaining 53 checked one by one against the source files drm-kmod's `drm/Makefile`
and `dmabuf/Makefile` actually compile (`drm_gem.c`, `drm_mm.c`, `drm_prime.c`,
`drm_print.c`, `drm_syncobj.c`, `drm_os_freebsd.c`, `scheduler/sched_*.c`,
`dma-buf.c`, `dma-fence.c`, `dma-resv.c`), all of which build for aarch64
according to drm-kmod's own `SUPPORTED_ARCH`.

**Caveat on that 53: it is source-level evidence, not a link test.** No
`drm.ko`/`dmabuf.ko` for aarch64 exists on this host, so nothing has actually
been resolved by a loader.

**And the 123 is a floor, not the total.** `lima_pdev_probe` is dead code
(blocker 2), so the compiler discards `lima_drm_driver` with it and its
references never reach the object file. Forcing the probe path live in a scratch
copy of the tree raises the count to 139: the 18 extra symbols are
`drm_dev_alloc/put/register/unregister`, `drm_open/read/poll/release`,
`drm_ioctl`, `drm_gem_mmap`, `__this_linker_file` and six kernel `sysctl`/devres
helpers — all resolvable, none new blockers, but they are invisible until
blocker 2 is fixed.

#### What is NOT proven about the new code

Only that it compiles under `-Werror` and links. `tests/test_shmem_logic.c`
covers the reference-count state machine, the size arithmetic, the fault
address→index translation and the purge predicate (47 checks, and it includes
the shipped `drm/drm_gem_shmem_logic.h` rather than a copy of it) — but page
wiring, cache-attribute changes, fault insertion, `dma_map_sgtable()` on a
platform device and the `dma_resv` locking discipline are kernel-only and have
**never executed**. Two known functional gaps, both documented in the file
header: imported PRIME buffers cannot be `mmap()`ed (drm-kmod's dmabuf has no
`dma_buf_mmap()`), and `drm_gem_shmem_purge()` cannot invalidate live userspace
mappings (FreeBSD's device mapping is not reachable from the GEM object).

  **FIXED since this was written** — `patches/drm-kmod/drm-kmod-dma-buf-mmap.patch` adds the missing `dma_buf_mmap()`, and it is now a build prerequisite rather than an optional extra: without it `drm/drm_gem_shmem_helper.c` does not compile. The imported-PRIME mmap path is exercised on every frame of the zero-copy presentation route (1030 fps measured), and `tests/limaread` reads an imported dma-buf back through an FBO with 0 of 16384 pixels wrong.

### 2. There is no path from the FDT to `lima_pdev_probe`

FreeBSD's `sys/compat/linuxkpi/common/include/linux/platform_device.h` is a
stub:

```c
struct platform_driver {           /* no .probe, no .of_match_table */
        void (*remove)(struct platform_device *);
        struct device_driver driver;
};
#define dev_is_platform(dev)    (false)
#define to_platform_device(dev) (NULL)

static __inline int
platform_driver_register(struct platform_driver *pdrv)
{
        pr_debug("%s: TODO\n", __func__);
        return (-ENXIO);
}
```

`lima_drv.c` ends in `module_platform_driver(lima_platform_driver)`, which
`lima_freebsd_compat.h` expands to a `module_init` calling
`platform_driver_register` — so module init returns `-ENXIO` unconditionally.
Nothing ever reaches `lima_pdev_probe`; the compiler already says so.

#### Actionable plan, researched 2026-08-04

Scoped by reading what `hal/lima` actually calls and what linuxkpi actually
provides. Total ~450–700 LOC across three files, none of them requiring a
kernel patch (the same `-I${.CURDIR}` shadowing trick the clk/reset stubs
already use).

1. **`hal/lima/linux/platform_device.h`** (shadow header, ~150 LOC) — a real
   `struct platform_device` (linuxkpi `struct device` + `device_t` + arrays of
   `struct resource *` for MEM and IRQ) and a real `struct platform_driver`
   (`.probe`, `.remove`, `.driver.name`, `.driver.of_match_table`), plus
   `platform_get_resource()`, `platform_get_irq[_byname][_optional]()`,
   `devm_platform_ioremap_resource()`, `to_platform_device()`,
   `dev_is_platform()`, `of_device_get_match_data()`, `dev_of_node()`. This
   replaces the four `-ENXIO`/`NULL` stubs currently in
   `lima_freebsd_compat.h`.

2. **`hal/lima/linux/platform_device.c`** (~250 LOC) — the bridge itself.
   Register through newbus statically, not dynamically: a
   `LINUXKPI_PLATFORM_DRIVER_MODULE(name, pdrv, simplebus)` macro emitting a
   `driver_t` + `DRIVER_MODULE()` whose generic methods are shared. Probe walks
   `pdrv->driver.of_match_table` with `ofw_bus_is_compatible()`; attach does
   `bus_alloc_resource_any(SYS_RES_MEMORY/SYS_RES_IRQ, rid 0..n)`,
   `device_initialize(&pdev->dev)` (linuxkpi's own — it sets up `kobj`,
   `devres_lock` and `devres_head`, so `devm_*` works), `pdev->dev.bsddev = dev`,
   `pdev->dev.parent = &linux_root_device`, then calls `pdrv->probe(pdev)`.
   `platform_driver_register()` becomes a no-op returning 0, since newbus has
   already done the work by the time module init runs.

3. **`hal/lima/linux/interrupt.h`** (~120 LOC) — **the non-obvious part, and the
   reason a "perfect" bridge would still not deliver interrupts.**
   `lkpi_request_irq()` (`sys/compat/linuxkpi/common/src/linux_interrupt.c:124`)
   resolves a Linux IRQ number by calling `lkpi_pci_find_irq_dev()`, which walks
   the **global `pci_devices` list** (`linux_pci.c:1162`). A platform device is
   not and cannot be on that list, so `devm_request_irq()` — used by
   `lima_gp.c`, `lima_pp.c` and `lima_mmu.c` — returns `-ENXIO` no matter how
   correct the platform_device is. The module-local fix is to shadow
   `request_irq`/`devm_request_irq`/`free_irq` and go straight to
   `bus_setup_intr()` on the `device_t`, keeping the handler-wrapping and devres
   registration that linuxkpi does. The upstream fix is a platform-device lookup
   path in linuxkpi's `linux_interrupt.c`, which is a FreeBSD kernel patch.

Remaining unknown after all three: whether `dma_map_sgtable()` works against a
platform `struct device` whose bus tag comes from `linux_dma_tag_init_coherent()`
rather than from a PCI parent. `lima_device.c` already calls that initialiser, so
the plumbing exists, but it has never been exercised.

### 3. Clocks, resets and regulators are fake-success no-ops

`hal/lima/linux/{clk.h,reset.h,regulator/consumer.h}` are local stubs that must
shadow drm-kmod's empty ones (hence `-I${.CURDIR}` first). They return `NULL` /
`0` / `ERR_PTR(-ENODEV)` and do nothing. `devm_clk_get` returns `NULL` and
`clk_prepare_enable(NULL)` returns success. On real A64 hardware the Mali-400
would never be clocked, reset-deasserted or powered — the driver would attach
(once blocker 2 is fixed) and then talk to a dead block.

## Verdict on the spec's 6–12 month estimate

`docs/specs/SPEC_lima_freebsd.md` (bzdk-side, not in this repository) costed Phase B as ~5 kLOC and 6–12 months.
What is measurable now:

- The ~5.3 kLOC lima port itself is **further along than the spec implies** — it
  is C that compiles clean under `-Werror` on the target arch with zero port
  bugs. That part is closer to done than to started.
- But the spec's LOC table lists only `sun4i-drm`, `lima.ko` and a "drm-kmod
  version bump". It does not account for the GEM SHMEM helper or a
  `platform_device`/OF bridge, which are the actual blockers and are both
  missing FreeBSD *infrastructure* rather than Lima code. Neither is bought by
  bumping drm-kmod: 6.6.25_13 is already the newest and still lacks shmem.
- So the 6–12 month order of magnitude looks **right, for the wrong reasons**.
  The risk is not the 5 kLOC of Lima; it is three pieces of missing FreeBSD DRM
  plumbing, each of which lands upstream-of-Lima and benefits any future SoC
  DRM driver.

One of those three is now written (~980 LOC of header + implementation + tests
for the shmem helper, plus 40 LOC for `drm_timeout_abs_to_jiffies`), which
recalibrates the rest: blockers 2 and 3 together are 650–1150 LOC of comparable
infrastructure work — weeks, not months. The 6–12 months belongs almost entirely
to what comes after them: first `kldload`, first attach, and validating on real
silicon everything that was written blind. See the re-cost table in
`docs/specs/SPEC_lima_freebsd.md` (bzdk-side, not in this repository).

## The `sysctl___hw_dri` blocker — ROOT-CAUSED AND FIXED (2026-08-07)

**Verdict: real root cause found, real fix landed, verified in the same
board-free QEMU rig that reproduced the failure. `kldload lima.ko` now
succeeds.** Nothing has run on real hardware; nothing here implies it has —
this closes the *symbol-resolution* blocker only (see "still-open" list at the
very end of this file for what remains: FDT-probe, clocks/resets/regulators,
attach).

### Root cause (mechanism-level, verified against `sys/kern/kern_linker.c` and `sys/kern/link_elf.c`, not guessed)

`sysctl___hw_dri` genuinely exists in `drm.ko` as a `GLOBAL`/`DEFAULT`-visibility
data symbol, in *both* `.symtab` and `.dynsym` (checked directly with
`readelf -sW` / `readelf --dyn-syms` on the built `drm.ko` — this ruled out
every ELF-visibility hypothesis: `EXPORT_SYMS` semantics, `objcopy
--strip-debug`, compiler `-fvisibility`, all clean). The bug is not in the
`.ko` file at all. It is in how the FreeBSD kernel's own module loader decides
*which already-loaded klds it is allowed to search* when resolving a new
module's undefined references:

- `relocate_file1()` (`sys/kern/link_elf.c:1471`) calls `elf_lookup()`
  (`link_elf.c:1913`) for every undefined relocation, which calls
  `linker_file_lookup_symbol(lf, symbol, deps=1)`
  (`sys/kern/kern_linker.c:883`).
- `linker_file_lookup_symbol_internal()` (`kern_linker.c:897`) searches (a)
  the module itself, then (b) — **only if `deps` is set** — iterates
  `file->deps[i]` and recurses with **`deps=0`** (`kern_linker.c:940-949`).
  That `deps=0` is the second half of the trap: dependency search is exactly
  **one level deep**. A module does not inherit its dependencies'
  dependencies.
- `file->deps[]` is populated by `linker_load_dependencies()`
  (`kern_linker.c:2317`, called from `link_elf_load_file()` at
  `link_elf.c:1239`, **before** `relocate_file()` is called at
  `link_elf.c:1243` — so this is not a load-order race, the dependency list is
  fully built before any relocation is attempted). It does two things
  unconditionally: adds `linker_kernel_file` (line 2331-2334, which is why the
  ~70 kernel-resident symbols always resolved), and then walks the loading
  module's **own** `MDT_DEPEND` metadata (compiled in via `MODULE_DEPEND()`)
  to find already-loaded modules by their registered module *name* (via
  `modlist_lookup2()`, matched against `MODULE_VERSION()`/`DECLARE_MODULE()`
  strings — NOT `.ko` filenames) and add their linker_file as a dependency.

`hal/lima/*.c` had **zero** `MODULE_DEPEND()` / `MODULE_VERSION()` calls
anywhere. So `lima.ko`'s `deps[]` contained only the kernel. `drm.ko` was
loaded, resident, and its symbol was globally visible in its own file — but
`linker_file_lookup_symbol_internal()` never even looks at "every other
currently-loaded kld"; there is no such fallback. It only looks where the
loading module told it to look, and lima.ko never told it anything.

One more trap for anyone repeating this: drm-kmod's `drm.ko` registers its
*module name* (what `MODULE_DEPEND()` must reference) as **`drmn`**, not
`drm` — see `DECLARE_MODULE(drmn, drm_mod, ...)` /
`MODULE_VERSION(drmn, 2)` in `drivers/gpu/drm/drm_os_freebsd.c`. `dmabuf.ko`
registers as `dmabuf` (`MODULE_VERSION(dmabuf, 1)`). Depending on the
filename instead of the registered name silently does nothing (no build
error — `MODULE_DEPEND` just wouldn't reference a real name and
`modlist_lookup2()` would fail at load time with a *different*, more obvious
error, "cannot find dependency", not the confusing "already-present symbol
looks undefined" shape this blocker had).

### Why this wasn't a guess: empirical confirmation

`llvm-nm -u lima.ko` (185 undefined symbols) diffed against `llvm-nm
--defined-only` on the GENERIC arm64 kernel (58390 symbols), `drm.ko` and
`dmabuf.ko`: **54 resolve only via `drm.ko`, 9 only via `dmabuf.ko`, all the
rest via the kernel**, plus `__this_linker_file` which `kern_linker.c:918`
special-cases before any file/deps search runs at all — so the true "missing
from everywhere" count was and remains **zero**. `sysctl___hw_dri` is one of
the 54. This confirms the fix needs exactly two `MODULE_DEPEND` lines
(`drmn`, `dmabuf`) — no more, no less; `linuxkpi_video.ko` (drm.ko's own
extra dependency) is not referenced by any lima.ko symbol directly, so lima
does not need it as a direct dependency (the non-transitive, one-level-deep
lookup means this had to be checked explicitly, not assumed).

Every other real driver in drm-kmod already does exactly this — it is the
established, upstream convention, not a lima-specific workaround:

```
$ grep -rn 'MODULE_DEPEND(.*drmn\|MODULE_DEPEND(.*dmabuf' drm-kmod --include=*.c | grep -v drm_os_freebsd.c
drivers/dma-buf/dma-buf-kmod.c:MODULE_DEPEND(dmabuf, linuxkpi, 1, 1, 1);
drivers/gpu/drm/amd/amdgpu/amdgpu_freebsd.c:MODULE_DEPEND(amdgpu, drmn, 2, 2, 2);
drivers/gpu/drm/amd/amdgpu/amdgpu_freebsd.c:MODULE_DEPEND(amdgpu, dmabuf, 1, 1, 1);
drivers/gpu/drm/i915/i915_module.c:MODULE_DEPEND(i915kms, drmn, 2, 2, 2);
drivers/gpu/drm/i915/i915_module.c:MODULE_DEPEND(i915kms, dmabuf, 1, 1, 1);
drivers/gpu/drm/ttm/ttm_module.c:MODULE_DEPEND(ttm, drmn, 2, 2, 2);
drivers/gpu/drm/ttm/ttm_module.c:MODULE_DEPEND(ttm, dmabuf, 1, 1, 1);
drivers/gpu/drm/radeon/radeon_freebsd.c:MODULE_DEPEND(radeonkms, drmn, 2, 2, 2);
drivers/gpu/drm/radeon/radeon_freebsd.c:MODULE_DEPEND(radeonkms, dmabuf, 1, 1, 1);
dummygfx/dummygfx_drv.c:MODULE_DEPEND(dummygfx, drmn, 2, 2, 2);
```

`hal/lima` was the one driver missing this boilerplate. `lima_drv.c` never
declares its own module identity via `DECLARE_MODULE`/`MODULE_VERSION` either
(`module_platform_driver()` only wires `module_init`/`module_exit`, a SYSINIT
mechanism entirely independent of the `MODULE_DEPEND`/`modmetadata_set`
linker-set mechanism) — nothing about that needed to change; `MODULE_DEPEND`
only requires that *this* module state its own dependencies, not that it be
formally versioned itself.

### The fix

Two lines added to `lima_drv.c`, right after `module_platform_driver(lima_platform_driver)`:

```c
MODULE_DEPEND(lima, drmn, 2, 2, 2);
MODULE_DEPEND(lima, dmabuf, 1, 1, 1);
```

`<sys/module.h>` (where the real `MODULE_DEPEND` lives — distinct from the
Linux-style `MODULE_AUTHOR`/`MODULE_DESCRIPTION`/`MODULE_LICENSE` tags already
in the file) is already pulled in transitively via `<linux/module.h>` →
`<sys/module.h>` (`linuxkpi/bsd/include/linux/module.h:5`), so no new
`#include` was needed.

Verified, not just built:

1. `llvm-nm` on the rebuilt `lima.ko` shows the new `modmetadata_set` entries
   (`__set_modmetadata_set_sym__mod_metadata_md_lima_on_drmn`,
   `...md_lima_on_dmabuf`) and `strings lima.ko | grep -x 'drmn\|dmabuf'`
   finds both names.
2. `./infra/scripts/qemu-lima-load.sh` — same rig, same GENERIC kernel, same
   `drm.ko`/`dmabuf.ko` — now reports:
   ```
   LIMA-RIG: dmabuf.ko KLDLOAD-OK
   LIMA-RIG: drm.ko KLDLOAD-OK
   LIMA-RIG: lima.ko KLDLOAD-OK
   ```
   and `kldstat` in the guest lists all six modules (kernel, dmabuf, drm,
   linuxkpi_video, lindebugfs, lima) loaded and resident.
3. Kernel-free suites unaffected: 49/49 layout + 47/47 shmem logic still
   pass after the change (the fix touches only module metadata, no logic
   lima's own code exercises).

Also corrected while here: the previous revision of this section claimed
`hal/lima/Makefile` sets `EXPORT_SYMS=YES` "same as `drm/Makefile` does" —
false, checked directly (`grep EXPORT_SYMS hal/lima/Makefile` finds nothing;
`drm-kmod/drm/Makefile:12` does set it). `hal/lima/Makefile` gets
`kmod.mk`'s default `EXPORT_SYMS=NO`, which is why a from-scratch build prints
`:> export_syms` (an empty allow-list) rather than skipping the filter step.
This was never the bug — `EXPORT_SYMS` on lima's *own* build only controls
whether *other* future modules could resolve symbols *out of* lima.ko, which
nothing currently needs — but the claim itself was wrong and is corrected
here so nobody re-derives a false lead from it.

The `qemu-lima-load.sh` script comment block that used to describe this
blocker (a `module_param_named()`/shared-sysctl-node theory) was also a wrong
guess from an earlier pass; it has been replaced with the verified mechanism
above.

### What is explicitly NOT claimed

Symbol resolution succeeding is not attach succeeding. `qemu-system-aarch64
-M virt`'s DTB has no `arm,mali-400`-compatible node, so `lima_pdev_probe`
correctly never runs on this rig — "loaded and initialised, no device to
attach" is the expected and correct outcome here, not "attached". The
still-open items below (FDT-to-probe path already closed per the section
above it, but clocks/resets/regulators are still fake no-ops, and nothing has
executed on real Mali-400 silicon) are unchanged by this fix.

## The exact next blocker (2026-08-06 entry — SUPERSEDED, kept for history only)

Everything in this section (the `sysctl___hw_dri undefined` failure, and the
"kld symbol-export/visibility question" framing of it) is now stale. See
"The `sysctl___hw_dri` blocker — ROOT-CAUSED AND FIXED (2026-08-07)" above:
the real mechanism was `MODULE_DEPEND`-based dependency scoping in
`sys/kern/kern_linker.c`, not an ELF visibility/export question, and
`lima.ko`'s `kldload` now succeeds. Blocker 2 (the `platform_device` bridge)
and the `drm.ko`/`dmabuf.ko` aarch64 builds mentioned below are still
accurate and unaffected.

Blocker 2 (the `platform_device` bridge) is DONE: `hal/lima/linux/
platform_device.{h,c}` shadow linuxkpi's `-ENXIO` stub with a real
`.probe`/`.of_match_table`, registering with newbus under `simplebus`. Proof:
the "unused function lima_pdev_probe" warning is gone from a clean rebuild.

`drm.ko`/`dmabuf.ko` now exist for aarch64 too
(`infra/scripts/build-drm-kmod-arm64.sh`), closing what had been a
source-level-only symbol-resolution claim.

Not attached, not run on real hardware — that remains true; only the
symbol-resolution blocker above this line is closed.
