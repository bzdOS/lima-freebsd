# `drm_gem_shmem_purge()` and live userspace mappings

**Outcome: (a), implemented.** `drm_gem_shmem_purge()` (`drm_gem_shmem_helper.c`)
now invalidates a live userspace mapping of the buffer it is purging, closing
deviation #4 in that file's header. It does **not** do this by porting
Linux's `drm_vma_node_unmap()` call — that call has no compilable FreeBSD
target in this drm-kmod tree (see below). It does it by calling
`pmap_remove_all()` directly on each backing page, in a new static helper,
`drm_gem_shmem_zap_ptes()` (`drm_gem_shmem_helper.c:634-689`), called from
`drm_gem_shmem_purge()` (`:741`) before anything else runs.

This document is the derivation: what upstream does, what FreeBSD actually
offers, the specific hazard found and rejected along the way, why the chosen
mechanism avoids it, what it guarantees, what it does not, and what has and
has not been verified. **Nothing here has run on hardware, or in a kernel
build.**

## 1. What upstream does

Linux 6.6/6.12 `drivers/gpu/drm/drm_gem_shmem_helper.c`, `drm_gem_shmem_purge()`
(read directly from a local Linux 6.12 checkout,
`/opt/bzdos/linux-work/linux-6.12/drivers/gpu/drm/drm_gem_shmem_helper.c:443-473`;
6.6 — this file's `PORTED_FROM` version — matches):

```c
void drm_gem_shmem_purge(struct drm_gem_shmem_object *shmem)
{
	...
	dma_unmap_sgtable(dev->dev, shmem->sgt, DMA_BIDIRECTIONAL, 0);
	sg_free_table(shmem->sgt);
	kfree(shmem->sgt);
	shmem->sgt = NULL;

	drm_gem_shmem_put_pages(shmem);

	shmem->madv = -1;

	drm_vma_node_unmap(&obj->vma_node, dev->anon_inode->i_mapping);   /* <-- */
	drm_gem_free_mmap_offset(obj);

	shmem_truncate_range(file_inode(obj->filp), 0, (loff_t)-1);
	invalidate_mapping_pages(file_inode(obj->filp)->i_mapping, 0, (loff_t)-1);
}
```

`drm_vma_node_unmap()` calls `unmap_mapping_range()` on the DRM device's own
internal "anon_inode" address space, at the offset/size of this object's mmap
node. On Linux, every `mmap()` of this object's fake DRM offset is a VMA in
*that* address space, so this one call finds and zaps every PTE, in every
process, that maps this object — regardless of whether the driver kept track
of any of them. That is the entire trick: Linux's mm subsystem tracks
mappings by *address space* (the `struct address_space` behind an inode), and
already knows how to walk "every VMA mapping this address space" without the
caller enumerating them.

## 2. What FreeBSD/drm-kmod actually offers for that call

`dev->anon_inode` is the value upstream passes. It does not exist as a field
on FreeBSD:

```c
/* drm-kmod/include/drm/drm_device.h:145-148 */
/** @anon_inode: inode for private address-space */
#ifdef __linux__
struct inode *anon_inode;
#endif
```

Its allocation (`drm_drv.c:674-682`, `drm_fs_inode_new()`/`drm_fs_inode_free()`
at `:552-576`) is the same way, entirely `#ifdef __linux__`. drm-kmod's own
`drm_dev_unplug()` — which on Linux does exactly
`unmap_mapping_range(dev->anon_inode->i_mapping, 0, 0, 1)` to clear every CPU
mapping of the device — spells out, in the FreeBSD branch of that same
function, that this was never done:

```c
/* drm-kmod/drivers/gpu/drm/drm_drv.c:505-514 */
#ifdef __linux__
	unmap_mapping_range(dev->anon_inode->i_mapping, 0, 0, 1);
#elif defined(__FreeBSD__)
	/* FreeBSD TODO */
#endif
```

So a mechanical, upstream-faithful port of `drm_gem_shmem_purge()`'s
`drm_vma_node_unmap(&obj->vma_node, dev->anon_inode->i_mapping)` line is not a
design choice away from this codebase — it is a compile error against this
drm-kmod tree, and drm-kmod's own maintainers already knew and flagged it
elsewhere. This is not specific to Lima or to this port.

## 3. The primitive that looks like the fix, and why it is a trap

`drm_vma_manager.h` **does** have an already-ported FreeBSD overload of
`drm_vma_node_unmap()` that takes something other than an `address_space *`:

```c
/* drm-kmod/include/drm/drm_vma_manager.h:220-235 */
static inline void drm_vma_node_unmap(struct drm_vma_offset_node *node,
#ifdef __linux__
				      struct address_space *file_mapping)
#elif defined(__FreeBSD__)
				      void *obj)
#endif
{
	if (drm_mm_node_allocated(&node->vm_node))
#ifdef __linux__
		unmap_mapping_range(file_mapping,
#elif defined(__FreeBSD__)
		unmap_mapping_range(obj,
#endif
				    drm_vma_node_offset_addr(node),
				    drm_vma_node_size(node) << PAGE_SHIFT, 1);
}
```

`unmap_mapping_range` on FreeBSD is a linuxkpi macro for
`lkpi_unmap_mapping_range()` (`linux/mm.h:429-430`,
`linux_page.c:638-649`), whose own doc comment says exactly what `obj` must
be:

```c
/*
 * ... @obj should match to vm_private_data field of vm_area_struct returned
 * by mmap file operation handler, see linux_file_mmap_single() sources ...
 */
void
lkpi_unmap_mapping_range(void *obj, loff_t const holebegin __unused,
    loff_t const holelen __unused, int even_cows __unused)
{
	vm_object_t devobj;

	devobj = cdev_pager_lookup(obj);
	if (devobj != NULL) {
		cdev_mgtdev_pager_free_pages(devobj);
		vm_object_deallocate(devobj);
	}
}
```

`vm_private_data` is set to our own `struct drm_gem_object *` by
`drm_gem_mmap_obj()` (`drm-kmod/drivers/gpu/drm/drm_gem.c:1055`,
`vma->vm_private_data = obj;`), and that is also the `handle` that
`linux_file_mmap_single()` registers the object's `OBJT_MGTDEVICE` vm_object
under (`linux_compat.c:1341-1384`: `vm_private_data = vmap->vm_private_data;`
then `cdev_pager_allocate(vm_private_data, OBJT_MGTDEVICE,
&linux_cdev_pager_ops[0], ...)`). So `unmap_mapping_range(&shmem->base, 0, 0,
1)` type-checks, resolves the right object, and reads as *the* obvious,
almost-verbatim port of upstream's call. **It is still not safe to call from
inside `drm_gem_shmem_purge()`, and the reason is a real, traceable
deadlock, not a style objection:**

1. `cdev_pager_lookup(obj)` (`vm_pager.h:302`, impl `device_pager.c:117-133`)
   finds the `OBJT_MGTDEVICE` object by handle and — via
   `vm_pager_object_lookup()` (`vm_pager.c:379-396`) — **takes a new reference
   on it** before returning it.
2. `cdev_mgtdev_pager_free_pages()` (`device_pager.c:299-318`) busies and
   unmaps every resident page of that object (`pmap_remove_all()` +
   `vm_page_iter_remove()` per page, via `cdev_mgtdev_pager_free_page()`,
   `device_pager.c:292-297`) — this part is fine and is in fact the same
   underlying primitive this fix ends up using.
3. `vm_object_deallocate(devobj)` (`vm_object.c:616-688`) drops the reference
   taken in step 1. If that reference happens to be the *last* one — i.e. no
   process currently has this GEM object mapped, or the only process that did
   is racing a concurrent `munmap(2)`/exit against this exact call — then
   `vm_object_deallocate()` (still inside the same call, same thread) marks
   the object `OBJ_DEAD` and calls `vm_object_terminate()` (`:684-685`), which
   for a `OBJT_MGTDEVICE` object runs `dev_pager_dealloc()`
   (`device_pager.c:333-359`):
   ```c
   static void
   dev_pager_dealloc(vm_object_t object)
   {
   	VM_OBJECT_WUNLOCK(object);
   	object->un_pager.devp.ops->cdev_pg_dtor(object->un_pager.devp.handle);
   	...
   ```
4. For our object, `cdev_pg_dtor` is `linux_cdev_pager_dtor()`
   (`linux_compat.c:612-634`), which calls `vmap->vm_ops->close(vmap)`. `vm_ops`
   was set by `drm_gem_mmap_obj()` to `obj->funcs->vm_ops`
   (`drm_gem.c:1056`), which for a shmem object is
   `&drm_gem_shmem_vm_ops` (`drm_gem_shmem_helper.c:97`), whose `.close` is
   **`drm_gem_shmem_vm_close()`** (`drm_gem_shmem_helper.c:860`):
   ```c
   drm_gem_shmem_vm_close(struct vm_area_struct *vma)
   {
   	...
   	dma_resv_lock(shmem->base.resv, NULL);   /* :865 */
   	drm_gem_shmem_put_pages(shmem);
   	dma_resv_unlock(shmem->base.resv);
   	...
   ```

Step 4 tries to take `shmem->base.resv` — a non-recursive `ww_mutex` — on the
**same thread that is currently inside `drm_gem_shmem_purge()`**, which took
that exact lock before calling any of this
(`dma_resv_assert_held(shmem->base.resv)` at entry, never released until
`drm_gem_shmem_purge()` returns). `dma_resv_lock(..., NULL)` passes no
acquire context, so there is no reentrancy detection possible even in
principle. **The result is an unconditional self-deadlock**, reachable
whenever `unmap_mapping_range()`/`drm_vma_node_unmap()` is called from a purge
path on an object whose only live mapper is concurrently unmapping — an
ordinary race with an unprivileged `munmap(2)` on another thread, not an
exotic condition.

This chain is real and shipping (it is what linuxkpi *actually gives drivers*
for this operation on FreeBSD, and is the closest thing to a sanctioned
"port" of `drm_vma_node_unmap()` in this tree) — which is exactly why it was
worth tracing all the way through instead of stopping at "it type-checks."
Using it here would have looked like the correct upstream port while
shipping a latent, rare, hard-to-reproduce kernel deadlock — precisely the
failure shape this task was scoped to avoid.

## 4. What the fix actually does

FreeBSD's page-fault path for one of these mmaps does something Linux's does
not: `lkpi_vmf_insert_pfn_prot_locked()` (`linux_page.c:508-572`), called from
this file's own `drm_gem_shmem_fault()` (`:789`), does not just insert a PFN
into the page tables — it **moves the `vm_page_t` between vm_objects**. On
first fault, it takes the page that `shmem_read_mapping_page()` put in our
`OBJT_SWAP` backing object (`obj->filp->f_shmem`), calls `vm_page_remove()` to
detach it from that object, and `vm_page_iter_insert()` to attach the *same*
page to the per-GEM-object `OBJT_MGTDEVICE` object that
`linux_file_mmap_single()` created (or reused — see below) for this mmap. The
page's identity (the `vm_page_t` pointer) never changes; which `vm_object`
currently owns it does.

Two things about that object are worth knowing and are what make the fix
possible:

- **There is only ever one, system-wide, per GEM object.**
  `cdev_pager_allocate()` (`device_pager.c:135-256`) dedupes by `handle`, and
  the handle is our own `struct drm_gem_object *` (§3). A second `mmap()` of
  the same GEM object — by the same or a different process — reuses the
  existing `OBJT_MGTDEVICE` object and its single, shared `struct
  vm_area_struct` (`linux_compat.c:1341-1385`). This is also why
  `drm_gem_shmem_vm_open()`/`_vm_close()` model "one more/one fewer mapping"
  as a `pages_use_count` increment/decrement rather than per-mapping state.
- **The rename only ever happens while `shmem->base.resv` is held.** The
  entire `lkpi_vmf_insert_pfn_prot_locked()` call in `drm_gem_shmem_fault()`
  is bracketed by that function's own `dma_resv_lock()`/`dma_resv_unlock()`
  (`:798`, and the unlock further down). Since `drm_gem_shmem_purge()` also
  requires `shmem->base.resv` held for its entire body, and a `ww_mutex` has
  one owner at a time, **no page belonging to this GEM object can change
  which `vm_object` owns it while purge is running.** `pg->object` is
  quiescent for the whole of `drm_gem_shmem_zap_ptes()`, for a reason
  specific to this driver's own locking, not a general FreeBSD guarantee.

Given that, `drm_gem_shmem_zap_ptes()` (`drm_gem_shmem_helper.c:634-689`),
called from `drm_gem_shmem_purge()` right after the purgeability check and
before anything else, does for each of `shmem->pages[i]`:

1. Read `pg->object` (safe per the quiescence argument above; not raced).
2. `VM_OBJECT_WLOCK()` that object, re-check `pg->object` did not change
   (defensive — matches the re-check `lkpi_vmf_insert_pfn_prot_locked()`
   itself does after a lock handoff, `linux_page.c:535-561` — not load-bearing
   given point 2 above, but cheap and consistent with this file's own style
   elsewhere).
3. `vm_page_busy_acquire(pg, VM_ALLOC_WAITFAIL)` — the same busy-then-act
   pattern `vm_object_page_remove()` (`vm/vm_object.c:2032-2037`) and
   `cdev_mgtdev_pager_free_pages()` (`device_pager.c:310-315`) use.
4. `pmap_remove_all(pg)` (`vm/pmap.h:161`, arm64 body
   `sys/arm64/arm64/pmap.c:4393-...`) — **this is the actual invalidation**:
   it strips this page from every pmap that currently maps it, in every
   process, unconditionally. It is the same primitive both of the functions
   named in step 3 use internally for a wired, managed page — the same
   primitive `vm_object_page_remove()` calls for exactly our situation
   (`p->wired` true, `object->ref_count != 0`, `vm_object.c:2042-2046`).
5. Unbusy, unlock, move to the next page.

Deliberately, this **never calls `cdev_pager_lookup()` or
`vm_object_deallocate()`** — it never touches the `OBJT_MGTDEVICE` object's
reference count at all, which is precisely what made §3's path unsafe. There
is no object to look up if the GEM object has never been mmap()ed
(`pg->object` is just the swap object, `pmap_remove_all()` on an unmapped page
is a correctly-inert no-op), and no refcount to race if it has.

### Lock order

The only new lock nesting this introduces is **`shmem->base.resv` (outer,
already held by `drm_gem_shmem_purge()`'s own contract) → `VM_OBJECT_WLOCK`
(middle) → the pmap layer's internal PV-list/per-pmap locks (inner, taken
inside `pmap_remove_all()`)**.

- `dma_resv` outer, `VM_OBJECT_WLOCK` inner is the exact order this file's own
  `drm_gem_shmem_fault()` already uses (`:798` then `:814`). It is also the
  order `linux_cdev_pager_populate()` (`linux_compat.c:494-561`) enforces from
  the other side: it is entered with the object's `VM_OBJECT_WLOCK` already
  held (the generic `vm_fault()` calling convention for `.pgo_populate`), and
  **explicitly drops it** (`:506`, `VM_OBJECT_WUNLOCK(vm_obj)`) *before*
  calling into `->fault()`, which is what takes `dma_resv`. That is a
  deliberate reversal by the linuxkpi authors to keep `dma_resv` outermost
  whenever both locks are live — the same direction this fix uses.
- `VM_OBJECT_WLOCK` outer, pmap-internal locks inner is the FreeBSD VM
  subsystem's own standing convention, used by `vm_object_page_remove()` and
  `cdev_mgtdev_pager_free_pages()` themselves (both cited above) —
  not something introduced here.

No code path was found, in this file or in the linuxkpi/drm-kmod source read
for this task, that takes a `VM_OBJECT_WLOCK` of either of this object's two
possible owning objects and *then* tries to acquire `shmem->base.resv` — the
inversion that would make this unsafe. That is a statement about what was
read, not an exhaustive proof over the whole kernel.

## 5. What this guarantees, and what it does not

**Guarantees:** once `drm_gem_shmem_purge()` returns, no process has a valid
PTE mapping any page that belonged to this GEM object at the time purge ran.
Any pre-existing mapping (mapped and already faulted-in, or mapped but never
faulted) either has no PTE at all, or had one and just lost it. Any later
access through that old mapping takes a fresh page fault, which
`drm_gem_shmem_fault()` turns into `VM_FAULT_SIGBUS` because `shmem->madv` is
already `-1` by the time that fault could run (set immediately after
`drm_gem_shmem_zap_ptes()` returns, before the mmap offset is freed). No new
mapping can be created either, because `drm_gem_free_mmap_offset()` runs in
the same function.

**Does not guarantee / does not do:**

- It does not detach the pages from the `OBJT_MGTDEVICE` object's own
  resident-page tracking the way `cdev_mgtdev_pager_free_pages()` would (that
  function both unmaps *and* calls `vm_page_iter_remove()`). This driver
  deliberately stops at the unmap. The `OBJT_MGTDEVICE` object, if one exists
  for this GEM object, keeps considering these now-unmapped pages resident
  until its own last real reference goes away (ordinary `munmap()`/process
  exit, whenever that happens next). That is inert bookkeeping — the pages
  are unreachable (no PTE, `madv < 0`, no mmap offset) — not a live hole, but
  it does mean this is not a byte-for-byte replica of what
  `cdev_mgtdev_pager_free_pages()`/upstream's truncate would leave behind.
- The `pg->object`-is-quiescent argument (§4) is specific to this driver: it
  relies on `drm_gem_shmem_fault()` being the *only* code path in this tree
  that ever moves one of these pages between objects, and on that path always
  taking `shmem->base.resv` first. It is not a general FreeBSD or linuxkpi
  guarantee, and would need re-checking if this pattern is copied into a
  driver whose fault handler does not follow the same locking.
  `drm_gem_shmem_zap_ptes()`'s defensive re-check after taking
  `VM_OBJECT_WLOCK` is there so that a future change violating this
  assumption fails safe (retries against the object's current state) rather
  than acting on a stale pointer — but it does not by itself make the
  function correct if that assumption stops holding.
- It does not reclaim any memory. Exactly as before this change, memory
  reclamation is `drm_gem_shmem_put_pages()` (unwire) and
  `shmem_truncate_range()`/`invalidate_mapping_pages()` (drop from the swap
  object), unchanged, still running right after. This function only removes
  the mappings that would otherwise let userspace keep reading/writing pages
  after those two calls run.
- It does not help a *concurrent* mapper who is mid-fault when purge runs —
  that cannot happen either, because both paths serialize on
  `shmem->base.resv`, but it is worth being explicit that this fix relies on
  that serialization rather than on anything in `drm_gem_shmem_zap_ptes()`
  itself.

## 6. Does anything call purge today?

**No.** Checked directly, not assumed:

- `grep -rn drm_gem_shmem_purge` / `drm_gem_shmem_madvise` across
  `/opt/bzdos/bsdOS/hal/lima` and `/opt/bzdos/drm-kmod`: the only hits are the
  definitions/declarations in `drm_gem_shmem_helper.{c,h}` themselves, the
  purgeable-predicate tests in `hal/lima/tests/test_shmem_logic.c` (which test
  the pure `drm_gem_shmem_purgeable()`/`_madvise_apply()` logic functions in
  `drm_gem_shmem_logic.h`, not `drm_gem_shmem_purge()` itself), and mentions in
  `docs/PLAN-mesa-lima.md` and `docs/README-arm64.md` describing the same gap this
  document is about.
- Lima's own ported driver (`hal/lima/lima_gem.c`, `lima_drv.c`) never
  mentions `purge`, `madvise`, or `shrinker`.
- **Upstream matches**: a local Linux 6.12 checkout's
  `drivers/gpu/drm/lima/lima_gem.c`/`lima_drv.c` also has no `madvise` ioctl
  and no shrinker — `lima_drv.c`'s ioctl table
  (`lima_drv.c:247-254`) has `GET_PARAM`, `GEM_CREATE`, `GEM_INFO`,
  `GEM_SUBMIT`, `GEM_WAIT`, `CTX_CREATE`, `CTX_FREE`; there is no
  `lima_gem_shrinker.c` or equivalent file. Upstream, the *only* caller of
  `drm_gem_shmem_purge()` in the whole `drivers/gpu/drm` tree is
  `panfrost_gem_shrinker.c` — Lima has simply never wired this up, on either
  OS.

So this gap — before and after this change — is **latent, not active**: no
ioctl, no shrinker, no code path anywhere in this tree reaches
`drm_gem_shmem_purge()` at runtime. That does not make the fix pointless
(this is exactly the kind of correctness gap that should be closed before,
not after, someone adds a madvise ioctl or a shrinker for Lima — both are
plausible future work per `docs/PLAN-mesa-lima.md`), but it does mean nothing
observable on this platform changes today, and it is why this was a
same-day, non-urgent, doc-plus-fix task rather than a hotfix.

## 7. Verification performed

Two host-side, kernel-build-free checks, both bounded exactly as described:

**A. Isolated type/syntax check of the new code**, in the spirit of (and
patterned after) the `drm-kmod-dma-buf-mmap.patch` check in
`patches/README.md`:

- `drm_gem_shmem_zap_ptes()` was copied verbatim into a standalone `.c` file
  and compiled against a hand-written header of type/macro/prototype stubs,
  with `gcc` and `clang`, `-std=c99 -Wall -Wextra -Werror -pedantic`, `-c`
  (compile only, no link). Both compile clean, zero warnings.
- Every stub with a real FreeBSD/drm-kmod counterpart was transcribed from
  the header actually read for this task, with its own file:line noted in the
  stub file's comments: `struct vm_page`'s `object` field (`vm/vm_page.h`),
  `#define page vm_page` (`linux/page.h:51`), `VM_OBJECT_WLOCK`/`WUNLOCK`
  (`vm/vm_object.h:269-274`, macro bodies reproduced verbatim), `vm_page_t`/
  `vm_object_t` typedefs (`vm/vm.h:109-110,136`), `vm_page_busy_acquire()`
  (`vm/vm_page.h:576`), `VM_ALLOC_WAITFAIL` (`vm/vm_page.h:492`),
  `pmap_remove_all()` (`vm/pmap.h:161`), and the `drm_gem_object`/
  `drm_gem_shmem_object` fields this code touches
  (`drm-kmod/include/drm/drm_gem.h:302,322,373`; this file's own
  `drm_gem_shmem_helper.h:63-107`). A few leaves with no real call site in
  this new code (`rw_wlock`/`rw_wunlock`/`rw_wowned`'s own signatures,
  `vm_page_xunbusy_hard`, `dma_resv_assert_held`) are simplified stand-ins,
  clearly marked as such in the stub file, not transcribed from a directly
  read header.
- The stub setup was checked for false confidence, not just used
  optimistically: a deliberately broken variant (`pmap_remove_all(pobj)`
  instead of `pmap_remove_all(pg)` — passing a `vm_object_t` where
  `pmap_remove_all()` wants a `vm_page_t`) was compiled against the same
  stubs with both compilers and **fails** on both, with exactly the expected
  diagnostic (`incompatible pointer types ... 'vm_object_t' ...
  'vm_page_t'`). This confirms the stubs are not simply accepting anything
  passed to them.

**B. Whole-file preprocessor sanity pass.** The actual, edited
`drm_gem_shmem_helper.c` was run through `gcc -E` (preprocess only) against
empty stand-in headers for all of its `#include`s (real relative
`drm_gem_shmem_logic.h` picked up unmodified from its real location, needing
only a `PAGE_SHIFT` macro predefined). This is a different, complementary
check from (A): it does not verify types at all, but it does verify that the
whole file — not just the new function in isolation — lexes as valid C:
comments and string/char literals all balance and every preprocessor
directive is well-formed. This check exists because it caught a real mistake
made while writing this: a doc comment that quoted the literal text
`"/* FreeBSD TODO */"` inside a `/* ... */` block comment, whose embedded
`*/` would have closed that comment early and left a stray unterminated
string as live code. First pass failed with exactly that symptom; the wording
was changed to describe the marker instead of quoting its comment syntax, and
the re-run preprocesses clean (`gcc -E`, exit 0, no diagnostics), with the new
function and its call site both present, intact, in the preprocessed output.

**What neither check proves:** no kernel build of any kind was run or
attempted (`build-drm-kmod-arm64.sh` and `build-lima-arm64.sh` are explicitly
off-limits for this task — those objdirs are in active use elsewhere).
Nothing here exercises a real linuxkpi macro expansion, real FreeBSD header
ordering, real `dma_resv`/`ww_mutex`/`VM_OBJECT`/pmap implementations, or the
kernel's actual diagnostic set. Nothing links. Nothing was loaded onto any
board or into any VM. The lock-order argument in §4 is a structural
derivation from reading the actual FreeBSD VM/linuxkpi/drm-kmod source in
this tree (every claim above is backed by a specific file and line, not
general FreeBSD knowledge) — it is not something a compiler or a host-side
check can confirm, and it has not been exercised under real concurrency on
real hardware. The board is explicitly off-limits for this task; this fix has
not been tried there and there is no aarch64 build of it to try even
off-board.
