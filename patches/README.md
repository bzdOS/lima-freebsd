# Patches against other people's trees

`hal/lima` builds against a drm-kmod source tree (`DRM_KMOD_SRC`, tag
`drm_v6.6.25_13`, commit `11252e8b9074218848abe3195601acad655a2e26`). That tree
is **not** a git checkout here and `infra/scripts/build-drm-kmod-arm64.sh`
re-fetches its files from the GitHub API — so an edit made directly in it is an
edit that silently disappears on the next fetch. Anything we need changed there
lives here instead.

**Patches are grouped by which tree they target, and that split matters:**

| directory | target tree | applied by |
|---|---|---|
| `drm-kmod/` | `$DRM_KMOD_SRC` (`/opt/bzdos/drm-kmod`) | `build-drm-kmod-arm64.sh`, automatically |
| `freebsd-src/` | the FreeBSD source tree (`/opt/bzdos/freebsd-src-earlyboot-wt`) | by hand, before `buildkernel` |
| `freebsd-ports/` | a FreeBSD **ports** checkout, at its root, with `-p0` | by hand; not part of any build here |

They are separate because the build script applies **every** `*.patch` in its
patch directory to drm-kmod. When both kinds shared one flat directory the build
refused to start, correctly: its guard reported a freebsd-src patch as "neither
applies nor is applied" to drm-kmod. Do not flatten it again, and put a new patch
in the directory for the tree it patches.

Apply by hand with:

```sh
cd "$DRM_KMOD_SRC" && patch -p1 < .../patches/drm-kmod/<name>.patch
cd /opt/bzdos/freebsd-src-earlyboot-wt && patch -p1 < .../patches/freebsd-src/<name>.patch
cd /usr/ports && patch -p0 < .../patches/freebsd-ports/<name>.patch   # note -p0
```

The ports one takes `-p0`, not `-p1`: its paths are already relative to the
ports root (`graphics/mesa-dri/Makefile`), the form a ports patch is normally
submitted in.

For drm-kmod, `build-drm-kmod-arm64.sh` does it itself, idempotently (dry-runs
each patch first, skips ones already applied, hard-fails on a tree whose state it
cannot determine), so a plain build is enough.

---

## `drm-kmod-dev-alias-lifecycle.patch`

**A failed DRM probe leaked its `/dev/dri` node, and the retry panicked the
kernel.**

`drm_dev_alias()` (`drivers/gpu/drm/drm_os_freebsd.c`) is FreeBSD-specific glue
that gives a DRM minor its `/dev/dri/cardN` / `/dev/dri/renderDN` node, because
FreeBSD does not create it automatically the way Linux does. It called
`make_dev_alias()` and threw the returned `cdev` away. Nothing anywhere
destroyed it — not `drm_dev_put()`, not `drm_dev_unregister()`, not module
unload; `grep drm_dev_alias` found a create with no counterpart.

On its own that is a leak. What made it fatal is the FreeBSD API: plain
`make_dev_alias()` is `make_dev_alias_v(MAKEDEV_WAITOK, ...)`, and without
`MAKEDEV_CHECKNAME` `kern_conf.c` **panics** on a name that already exists
instead of returning `EEXIST`. So the *second* `drm_dev_alloc()` for a given
minor index in one boot took the machine down:

```
panic: make_dev_alias_v: bad si_name (error=17, si_name=dri/renderD128)
#3  make_dev_alias+0x39c
#5  drm_dev_alias+0x1f8
#6  drm_sysfs_minor_alloc+0x1bc
#7  drm_minor_alloc+0xec
#8  drm_dev_init+0x244
#9  drm_dev_alloc+0x38
#10 lima_pdev_probe+0x90
```

The alias is created inside `drm_dev_alloc()`, well before
`drm_dev_register()` — so *every* driver that calls `drm_dev_alloc()` and then
fails for any later reason arms this. And "probe fails, fix something, load
again" is not an edge case, it is what bringing a new DRM driver up consists of.
It cost this project a day: the panic was misread as "making lima's clock
enables real crashes the board", because the first load of the rebuilt lima.ko
came after an earlier load had already failed in the same boot, so it panicked
in `drm_dev_alloc()` before `lima_clk_enable()` was ever reached. See
`hal/lima/MALI-STATUS.md`.

The fix is the missing half of the pair:

- keep the alias in a new `drm_minor::bsd_alias` (FreeBSD-only field);
- create it with `make_dev_alias_p(MAKEDEV_CHECKNAME, ...)` so a collision is a
  returned error that `drm_sysfs_minor_alloc()`'s existing `goto err` unwinds,
  not a panic;
- destroy it in `drm_minor_alloc_release()`, the `drmm_` action that already
  runs on every `drm_dev_put()` — including the one on a failed probe's error
  path, which is the case that was broken.

**Verified on real hardware** (Banana Pi M64, FreeBSD 15.1 guest under the bzdOS
EL2 hypervisor): three consecutive `kldload`/`kldunload` cycles of a lima.ko
whose probe fails at the MMU self-test, with no `make_dev_alias` panic, where
the second one previously panicked every time. `/dev/dri/` is absent after
unload, i.e. the alias really is destroyed rather than merely not colliding.

Not upstreamed. It is a genuine upstream bug and worth sending, but this project
has no drm-kmod contribution path set up and the fix is needed here now.

---

## `drm-kmod-dma-buf-mmap.patch`

**Imported PRIME buffers could not be `mmap()`ed: drm-kmod's dmabuf had no
`dma_buf_mmap()`.**

`drm_gem_shmem_mmap()` (`hal/lima/drm/drm_gem_shmem_helper.c`) is this
project's own port of Linux's GEM SHMEM helper — drm-kmod ships neither the
header nor the `.c` for it, see the file's own header comment. Its
`import_attach` branch is the code path a `mmap()` of an *imported* PRIME
buffer runs through. Before this patch it read (`drm_gem_shmem_helper.c:772-782`
at the time this was found):

```c
if (obj->import_attach) {
	/*
	 * Linux forwards this to dma_buf_mmap(). drm-kmod's dmabuf
	 * does not implement it (only dma_buf_vmap/vunmap exist), so
	 * refuse rather than map the wrong pages. ...
	 */
	vma->vm_private_data = NULL;
	vma->vm_ops = NULL;
	return (-EOPNOTSUPP);
}
```

Upstream Linux (`drivers/gpu/drm/drm_gem_shmem_helper.c`) calls
`dma_buf_mmap(obj->dma_buf, vma, 0)` there instead. drm-kmod's
`linux/dma-buf.h` (`linuxkpi/gplv2/include/linux/dma-buf.h`) declares
`dma_buf_vmap()`/`dma_buf_vunmap()` and defines both in
`drivers/dma-buf/dma-buf.c` — but never declared or defined `dma_buf_mmap()`.
Grepping the whole tree for `dma_buf_mmap` before this patch turns up only
the unrelated, already-existing `dma_buf_mmap_fileops` (the FreeBSD-native
`fo_mmap` hook for `mmap()`ing a dma-buf fd directly) — nothing implementing
the Linux in-kernel entry point a driver's own GEM mmap handler calls to hand
an already-set-up `vma` to the dma-buf backing an *imported* GEM object.
Every such mmap failed before `dma_buf_ops.mmap` was ever consulted,
regardless of what the exporter implemented.

**This turned out to be a real gap, not a deeper missing-machinery problem.**
Everything `dma_buf_mmap()` needs to call into already exists and is already
wired up in this tree:

- `drivers/gpu/drm/drm_prime.c` already has `drm_gem_dmabuf_mmap()` ->
  `drm_gem_prime_mmap()`, and `drm_gem_prime_dmabuf_ops.mmap` already points
  at the former — this is the exporter-side callback chain; nothing needed
  to be added there.
- `drm_gem_prime_fd_to_handle()` (same file) already sets `obj->dma_buf` on
  a freshly-imported GEM object, matching upstream, so it is populated by
  the time any mmap() of that handle can happen.
- `vma_pages()` and `vma_set_file()` (`linux/mm.h`) already exist in the base
  tree's linuxkpi and are already used elsewhere in drm-kmod
  (`drm_gem.c`, `ttm/ttm_bo_vm.c`, i915).

The fix is `dma_buf_mmap()` itself — a small bounds-checking dispatcher, added
to `linux/dma-buf.h` (prototype) and `drivers/dma-buf/dma-buf.c`
(implementation), ported from upstream Linux 6.6/6.12's version of the same
function, with **one deliberate, documented omission**: real Linux additionally
does

```c
vma_set_file(vma, dmabuf->file);
```

so the vma holds its own reference on the dma-buf's file. That line does not
typecheck in this port: `struct dma_buf`'s `file` field (see the "Native
struct file, not struct linux_file" comment already on that struct) is a
*native* FreeBSD `struct file *` — this port's dma-buf fds are real fd-table
entries created by `falloc_noinstall()`/`finit()`, not linuxkpi char-device
shims — while `vma_set_file()` takes a `struct linux_file *`, which is what
`vma->vm_file` actually is. The two `struct file` types are unrelated in
linuxkpi; there is no `struct linux_file` for a dma-buf's own fd to hand it.
Faking one would mean inventing lifetime machinery this tree does not have,
not porting Linux's — exactly the kind of shim this project is trying to stop
doing. The patch's comment on `dma_buf_mmap()` spells out what still holds
without that line (the importing GEM object's own reference, taken by
`drm_gem_mmap_obj()` and dropped by the caller-side fix below on success,
transitively keeps the dma-buf alive for the ordinary map/use/unmap
lifetime) and what does not (a vma that outlives the importing handle by some
other path, e.g. inherited across `fork(2)`, is not proven to keep the
dma-buf pinned here).

**Also changed, directly (not a patch — this is bsdOS's own file, not
drm-kmod's):** `hal/lima/drm/drm_gem_shmem_helper.c`'s `import_attach` branch
now calls the new `dma_buf_mmap(obj->dma_buf, vma, 0)` instead of returning
`-EOPNOTSUPP`, and — matching upstream — drops the reference
`drm_gem_mmap_obj()` took on the importing object when the call succeeds
(needed because `drm_gem_prime_mmap()` repoints `vma->vm_private_data`/
`vm_ops` at the *exporter's* object on success, so the importer's own
reference would otherwise never be released). The file's header comment
(deviation #3) is updated to match. This is the part that actually makes the
drm-kmod fix effective for a caller; without it, `dma_buf_mmap()` would exist
but nothing in this tree would call it.

**What has been tested, and how:**

- The patch applies cleanly with `patch -p1 --dry-run` against the current
  (unpatched) `$DRM_KMOD_SRC` tree, and a real (non-dry-run) forward-apply
  followed by a real reverse-apply, done against an isolated scratch copy,
  reproduces the intended patched content and then the original byte-for-byte
  — a full round trip, not just a one-directional dry-run.
- `diff` confirms the patch touches exactly two files:
  `linuxkpi/gplv2/include/linux/dma-buf.h` and `drivers/dma-buf/dma-buf.c`.
- The new `dma_buf_mmap()` body (copied verbatim, not paraphrased) was
  compiled — with both `gcc` and `clang`, `-std=c99`/`gnu99 -Wall -Wextra
  -Werror -pedantic` — against a hand-transcribed set of type stubs
  (`struct dma_buf`, `struct dma_buf_ops`, `struct vm_area_struct`,
  `vma_pages()`, `vma_set_file()`) whose field names and field *types* were
  copied from the real headers, including the `#undef file` / `#define file
  linux_file` macro trick that makes the `vma_set_file()` omission necessary.
  It compiles clean with zero warnings.
- The type-mismatch claim was checked empirically, not just asserted from
  reading: a second variant with the literal upstream
  `vma_set_file(vma, dmabuf->file);` line added back fails to compile against
  the same stubs, on both compilers, with exactly the predicted diagnostic
  (`incompatible pointer types ... 'struct file *' ... 'struct linux_file *'`).
- The caller-side shape now used in `drm_gem_shmem_helper.c` (`ret =
  dma_buf_mmap(obj->dma_buf, vma, 0); if (ret == 0) drm_gem_object_put(obj);`)
  was separately checked against stubs for `struct drm_gem_object` and
  `drm_gem_object_put()` transcribed from `drm-kmod/include/drm/drm_gem.h`;
  it also compiles clean.

**STATUS 2026-08-21 — the section that used to sit here is deleted, because it
had become false.**

It said, in a public repository, that this patch had "NOT been tested, at all":
no kernel build, `dma_buf_mmap()` never executed, nothing loaded, no aarch64
`drm.ko`/`dmabuf.ko` build on the host, and nothing in `hal/lima` reaching the
code at runtime. Every one of those statements was true when written and none of
them is true now, and leaving it up told readers a load-bearing patch was
untested guesswork.

What is actually the case:

- `build-drm-kmod-arm64.sh` applies this patch on **every** build and refuses to
  build a tree whose patch state it cannot verify. The resulting `drm.ko` is the
  one running on the board.
- `dma_buf_mmap()` executes constantly. It is on the zero-copy presentation
  path: a client renders into a `gbm_surface`, exports the front buffer's
  dma-buf, and it is imported and mapped. Measured **1030 fps** through that
  path, and **3601 DRM/KMS page flips with 0 refused** in a 60-second run.
- PRIME import of a foreign dma-buf works, including the case that had been
  written off as dangerous: `tests/limaread` attaches an imported dma-buf to an
  FBO and reads it back — `0 of 16384 pixels wrong`.
- The `fork(2)`-inherited-vma edge case the omitted `vma_set_file()` would have
  covered is **still** not proven either way. That one line of the old section
  survives, because it is the only part still accurate.
- The other known gap — `drm_gem_shmem_purge()` cannot invalidate live userspace
  mappings, deviation 4 in `drm/drm_gem_shmem_helper.c` — is untouched by this
  patch and **has still never executed**. Nothing calls it: lima has no madvise
  ioctl and no shrinker, matching upstream.

The type/syntax evidence above it still stands on its own merits and is left
as-is; it simply is no longer the *only* evidence.

Not upstreamed, for the same reason as the alias-lifecycle fix above: this
project has no drm-kmod contribution path set up. Unlike that fix, this one
*is* essentially a straight port of upstream's own function minus one
FreeBSD-specific line, so it would need the least adaptation of anything here
if that path ever exists.
