# drm_dev_alias() leaks its /dev/dri alias cdev; the next allocation of the same minor panics in make_dev_alias_v() instead of failing cleanly

## Summary (suggested title)

FreeBSD: `drm_dev_alias()` never keeps or destroys the `/dev/dri/cardN`/`renderDN`
alias it creates, so a second `drm_dev_alloc()` for the same minor index in one
boot — the normal result of a failed probe being retried — panics in
`make_dev_alias_v()` with `bad si_name (error=17 EEXIST)` instead of failing the
probe cleanly.

## Affected

- **drm-kmod**, tag `drm_v6.6.25_13`, commit `11252e8b9074218848abe3195601acad655a2e26`.
  Verified against the actual GitHub ref, not assumed: the annotated tag
  `drm_v6.6.25_13` (tag object `fb631d392658f56b3c17d27fe74ff63739ce8cf2`)
  resolves to commit `11252e8b9074218848abe3195601acad655a2e26`, tagged by
  Jean-Sébastien Pédron on 2026-06-01 (tag object's tagger date; the commit's
  own author was not separately checked).
- Files (all under this commit):
  - `drivers/gpu/drm/drm_os_freebsd.c` — `drm_dev_alias()`
  - `drivers/gpu/drm/drm_sysfs.c` — `drm_sysfs_minor_alloc()`, the caller
  - `drivers/gpu/drm/drm_drv.c` — `drm_minor_alloc()`, `drm_minor_alloc_release()`,
    `drm_dev_init()`, `drm_dev_alloc()`, `drm_dev_put()`
  - `include/drm/drm_file.h` — `struct drm_minor`
- FreeBSD side of the mechanism (`make_dev_alias()`/`make_dev_alias_v()`):
  `sys/kern/kern_conf.c`, `sys/sys/conf.h`, verified against FreeBSD 15.1
  (`__FreeBSD_version` 1501000, confirmed in `sys/sys/param.h`). This devfs API
  is old and stable; it was only independently re-checked against the 15.1 tree
  in this pass, not against other branches.
- Reproduced on: Banana Pi M64 (Allwinner A64), FreeBSD 15.1 guest, loading an
  out-of-tree lima (Mali-400) KMS driver built against this drm-kmod tag.

Permalinks (pinned to the exact commit, so they show this content even after
the tree moves on):
- `https://github.com/freebsd/drm-kmod/blob/11252e8b9074218848abe3195601acad655a2e26/drivers/gpu/drm/drm_os_freebsd.c#L117-L152`
- `https://github.com/freebsd/drm-kmod/blob/11252e8b9074218848abe3195601acad655a2e26/drivers/gpu/drm/drm_sysfs.c#L170-L218`
- `https://github.com/freebsd/drm-kmod/blob/11252e8b9074218848abe3195601acad655a2e26/drivers/gpu/drm/drm_drv.c#L101-L161`
- `https://github.com/freebsd/drm-kmod/blob/11252e8b9074218848abe3195601acad655a2e26/include/drm/drm_file.h#L75-L89`

## Mechanism

`drm_dev_alias()` is FreeBSD-only glue: FreeBSD doesn't create `/dev/dri/*`
device nodes automatically the way Linux's sysfs/udev does, so this function
finds the "real" cdev `drm_drv.c` registered for a minor and gives it a
human-readable alias. `drm_os_freebsd.c:116-152`:

```c
int
drm_dev_alias(struct device *ldev, struct drm_minor *minor, const char *minor_str)
{
	...
	snprintf(buf, sizeof(buf), "dri/%s", minor_str);
	cdevp = linux_find_cdev("drm", DRM_MAJOR, minor->index);
	MPASS(cdevp != NULL);
	if (cdevp == NULL)
		return (-ENXIO);
	minor->bsd_device = cdevp->cdev;
	make_dev_alias(cdevp->cdev, buf, minor->index);
	reset_debug_log();
	return (0);
}
```

Line 149's `make_dev_alias()` return value (a `struct cdev *`) is discarded —
the statement isn't even an assignment. `struct drm_minor` (`drm_file.h:75-89`)
has no field for it:

```c
struct drm_minor {
	...
#ifdef __FreeBSD__
	struct cdev *bsd_device; 	/* Device number for mknod */
#endif
	struct dentry *debugfs_root;
	...
};
```

`bsd_device` (line 83) is the *original* cdev `drm_drv.c` made via
`register_chrdev` — not the alias. Grepping `bsd_device` across
`drm_os_freebsd.c`, `drm_drv.c`, `drm_sysfs.c` and `drm_file.h` at this commit
turns up exactly two hits: its declaration (`drm_file.h:83`) and this one
assignment (`drm_os_freebsd.c:148`). `destroy_dev()` does not appear in any of
those four files. `drm_minor_alloc_release()` — the FreeBSD-relevant teardown
for a minor, registered via `drmm_add_action_or_reset(dev,
drm_minor_alloc_release, minor)` (`drm_drv.c:151`), so it runs on every
`drm_dev_put()` once the device's refcount reaches zero
(`drm_dev_put()`→`drm_dev_release()`→`drm_managed_release()`, `drm_drv.c:795-838`)
— touches neither field:

```c
static void drm_minor_alloc_release(struct drm_device *dev, void *data)
{
	struct drm_minor *minor = data;
	unsigned long flags;

	WARN_ON(dev != minor->dev);

	put_device(minor->kdev);

	if (minor->type == DRM_MINOR_ACCEL) {
		accel_minor_remove(minor->index);
	} else {
		spin_lock_irqsave(&drm_minor_lock, flags);
		idr_remove(&drm_minors_idr, minor->index);
		spin_unlock_irqrestore(&drm_minor_lock, flags);
	}
}
```
(`drm_drv.c:101-117`)

So nothing — not `drm_dev_put()`, not `drm_dev_unregister()`, not a module
unload — ever destroys the alias. That alone is a leak (`/dev/dri/renderD128`
etc. stay in devfs forever once a driver has loaded once). What makes it fatal
is what happens on the *next* attempt to allocate the same minor:

`idr_remove()` above hands the minor index back to the allocator.
`drm_minor_alloc()`'s `idr_alloc(&drm_minors_idr, NULL, 64 * type, 64 * (type
+ 1), GFP_NOWAIT)` (`drm_drv.c:137-141`) allocates the lowest free id in a
64-wide range per type (`DRM_MINOR_PRIMARY = 0`, `DRM_MINOR_RENDER = 2`,
`DRM_MINOR_ACCEL = 32`, `drm_file.h:59-63` — `64 * 2 = 128`, which is exactly
why the panic below says `renderD128`). On a board with one GPU and nothing
else contending for minors, the next `drm_minor_alloc(dev, DRM_MINOR_RENDER)`
— i.e. the next probe attempt in the same boot — gets index 128 again,
deterministically.

`drm_dev_alias()` recomputes the same name (`buf` built from `minor_str` +
`minor->index`, line 143) and calls `make_dev_alias()` again. FreeBSD's
`prep_devname()` (`sys/kern/kern_conf.c:707-761`) is where a duplicate name is
actually detected:

```c
	if (devfs_dev_exists(dev->si_name) != 0)
		return (EEXIST);
```
(`kern_conf.c:757-758`; `EEXIST` is 17 — `sys/sys/errno.h:67`)

— it returns `EEXIST` because the leaked alias from the previous attempt is
still there. That error reaches `make_dev_alias_v()`:

```c
static int
make_dev_alias_v(int flags, struct cdev **cdev, struct cdev *pdev,
    const char *fmt, va_list ap)
{
	...
	error = prep_devname(dev, fmt, ap);
	if (error != 0) {
		if ((flags & MAKEDEV_CHECKNAME) == 0) {
			panic("make_dev_alias_v: bad si_name "
			    "(error=%d, si_name=%s)", error, dev->si_name);
		}
		dev_unlock();
		devfs_free(dev);
		return (error);
	}
	...
}
```
(`kern_conf.c:958-997`, panic at 979-982)

and the plain `make_dev_alias()` that `drm_dev_alias()` calls is:

```c
struct cdev *
make_dev_alias(struct cdev *pdev, const char *fmt, ...)
{
	...
	res = make_dev_alias_v(MAKEDEV_WAITOK, &dev, pdev, fmt, ap);
	...
	KASSERT(res == 0 && dev != NULL,
	    ("make_dev_alias: failed make_dev_alias_v (error=%d)", res));
	return (dev);
}
```
(`kern_conf.c:999-1013`)

`MAKEDEV_WAITOK` only — no `MAKEDEV_CHECKNAME` (`sys/sys/conf.h:254-259` for
the flag values). So the only way `make_dev_alias()` can react to a duplicate
name is a hard `panic()`; there's a `KASSERT` as a second-line backstop, but
the first line always fires. Nothing about this is a race — it is
deterministic given the same minor index twice in a boot, which is deterministic
given a failed-then-retried probe on an idle board.

Because `make_dev_alias_v()` panics rather than returning, control never gets
back to `drm_dev_alias()`'s own (irrelevant, `cdevp == NULL`-only) error path,
nor to its caller `drm_sysfs_minor_alloc()`'s existing, perfectly adequate
unwind:

```c
	rv = drm_dev_alias(kdev, minor, minor_str);
	if (rv < 0)
		goto err;
	return kdev;

err:
	put_device(kdev);
	return ERR_PTR(rv);
```
(`drm_sysfs.c:210-217`)

That `goto err` would have turned a name collision into an ordinary
`drm_minor_alloc()` failure. It's simply never reached — the failure happens
three stack frames further down, as a panic, not as a returned error code.

## Reproduction

1. `kldload` a DRM driver whose `..._probe()` calls `drm_dev_alloc()` and then
   fails for *any* reason afterward (a self-test, a missing resource — the
   specific reason doesn't matter, only that it's after `drm_dev_alloc()` and
   ends in `drm_dev_put()`).
2. Fix whatever made it fail, or don't — `kldunload`/`kldload` again (or let
   the bus retry the probe).
3. The second `drm_minor_alloc()` for that minor type gets the same index the
   first one got (nothing else is allocating from that range), `drm_dev_alias()`
   computes the same alias name, and it already exists → panic.

### Why this is not normally hit

`drm_dev_alias()` runs inside `drm_dev_alloc()`/`drm_dev_init()`, well *before*
`drm_dev_register()` — i.e., before a driver has done anything a user would
call "working". A mature driver on FreeBSD (`amdgpu`, `i915`) reaches this code
once per boot and doesn't fail after it, so the leak never gets a chance to
collide with itself. The failure mode that triggers this is specifically
"probe fails, then is retried in the same boot" — the ordinary rhythm of
*bringing up a new driver that FreeBSD doesn't support yet* (this was found
bringing up `lima`/Mali-400, which has no in-tree FreeBSD equivalent to have
exercised this path before), not something that happens to an already-working
driver's normal users. It also requires reaching `drm_dev_alloc()` at all —
any earlier probe failure (e.g. an FDT match miss) never touches this code and
looks unrelated.

## Observed symptom (verbatim)

```
panic: make_dev_alias_v: bad si_name (error=17, si_name=dri/renderD128)
  make_dev_alias <- drm_dev_alias <- drm_sysfs_minor_alloc
  <- drm_minor_alloc <- drm_dev_init <- drm_dev_alloc <- lima_pdev_probe+0x90
```

Recorded on real hardware (Banana Pi M64, FreeBSD 15.1 guest) by this project;
not re-triggered during this verification pass (no board access — see "What
was verified vs. taken on trust" below). The call chain in the trace matches the actual static
call graph exactly: `lima_pdev_probe()` calls `drm_dev_alloc()`
(`hal/lima/lima_drv.c:537`), which calls `drm_dev_init()`
(`drm_drv.c:783`/`624`), which calls `drm_minor_alloc()`
(`drm_drv.c:680`/`685`/`675`), which calls `drm_sysfs_minor_alloc()`
(`drm_drv.c:155`), which calls `drm_dev_alias()` (`drm_sysfs.c:210`), which
calls `make_dev_alias()` (`drm_os_freebsd.c:149`). `renderD128` is exactly what
`DRM_MINOR_RENDER == 2` (`drm_file.h:62`) times the 64-wide `idr_alloc()` range
predicts for the first render minor.

## Proposed fix

Give the alias somewhere to live, and destroy it on the same path that already
runs on a failed probe's unwind:

1. Add `struct cdev *bsd_alias;` to `struct drm_minor` (`drm_file.h`), next to
   `bsd_device`.
2. In `drm_dev_alias()`, replace `make_dev_alias()` with
   `make_dev_alias_p(MAKEDEV_CHECKNAME, &minor->bsd_alias, cdevp->cdev, buf,
   minor->index)`; on a nonzero return, clear `bsd_alias`/`bsd_device` and
   propagate `-error` instead of unconditionally returning 0.
   `MAKEDEV_CHECKNAME` is the flag that turns the `prep_devname()` failure
   path in `make_dev_alias_v()` (quoted above) from a `panic()` into a normal
   `return (error)` — it is the one-flag difference between `make_dev_alias()`
   and `make_dev_alias_p()`.
3. Add `drm_dev_alias_free(minor)` (idempotent: `destroy_dev()` the alias if
   non-NULL, then clear the field) and call it from `drm_minor_alloc_release()`
   under `#ifdef __FreeBSD__` — the same function quoted above, which already
   runs unconditionally on `drm_dev_put()`, including on a failed probe's error
   path.

### Risk

Low, and entirely FreeBSD-only (`#ifdef __FreeBSD__`); no Linux code path
changes. `destroy_dev()` on a field that is NULL-checked and owned solely by
this minor is the standard FreeBSD idiom used one field over
(`minor->bsd_device` itself is torn down the same way elsewhere in this
file). The only behavior change on the *success* path is that unloading a
driver now actually removes its `/dev/dri` alias — arguably an independent,
smaller pre-existing bug (stale dev nodes surviving unload) fixed as a side
effect.

This exact change (implemented in
`hal/lima/patches/drm-kmod/drm-kmod-dev-alias-lifecycle.patch` in this repository) was
verified on the hardware above: three consecutive `kldload`/`kldunload` cycles
of a driver whose probe fails every time, no `make_dev_alias` panic where the
second cycle previously panicked every time, and `/dev/dri/` confirmed absent
after each unload.

## What was verified vs. taken on trust

**Verified by reading source in this pass**, against the pristine, freshly
network-fetched blob content of drm-kmod at commit
`11252e8b9074218848abe3195601acad655a2e26` (independently confirmed to be what
tag `drm_v6.6.25_13` resolves to, via GitHub's tag/ref API — not assumed from
this project's own README) and against this project's FreeBSD 15.1 source
tree:
- Every file:line citation and quoted code block above.
- That the project's existing patch
  (`hal/lima/patches/drm-kmod/drm-kmod-dev-alias-lifecycle.patch`) applies cleanly
  (`patch -p1 --dry-run`) against these exact pristine pre-patch files —
  i.e. the patch's own line references are correct for this commit.
- `__FreeBSD_version` 1501000 in this project's FreeBSD tree
  (`sys/sys/param.h:77`).
- `DRM_MINOR_RENDER == 2` and the `idr_alloc()` range math that explains
  `renderD128`.
- That `lima_pdev_probe()` (this project's out-of-tree driver, not drm-kmod)
  really does call `drm_dev_alloc()` once and `drm_dev_put()` on two separate
  error-unwind paths (`hal/lima/lima_drv.c:537,572,602`).
- That `bsd_device`/`destroy_dev` do not otherwise appear in the four drm-kmod
  files most directly involved (`drm_os_freebsd.c`, `drm_drv.c`, `drm_sysfs.c`,
  `drm_os_freebsd.h`) — this was a targeted grep across those four files, not
  an exhaustive search of the whole drm-kmod tree for some unrelated file that
  might also touch the alias.

**Not independently re-verified in this pass** (taken from this project's own
prior records, since board access was explicitly out of scope for this task):
- The exact hex frame offsets in the fuller backtrace recorded in
  `README.md` (`make_dev_alias+0x39c`, etc.) — the *function
  names and call order* in that trace were checked against the real call
  graph above; the offsets were not.
- The "three kldload/kldunload cycles, no panic, /dev/dri absent" hardware
  confirmation of the fix — this is this project's own previously-recorded
  hardware run, not something re-run during this task.

No factual error was found in how this bug was described to me; everything
checked against the actual source exactly as stated.
