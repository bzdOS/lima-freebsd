# Mesa `lima` Gallium driver — native build log on the Chimp guest

Guest: `root@192.168.88.82` (Banana Pi M64, FreeBSD 15.1-RC3 aarch64, single vCPU).
Host doing the fetching/orchestration: this Linux host (`/opt/bzdos/bsdOS`).
Baseline free disk on guest at start: **4.6-4.7 GB on `/` (`/dev/vtbd0p3`, 6.0G total)**.

This log is written incrementally, in the order things actually happened, including
the mistakes. See `FEASIBILITY.md` for the pre-build research this continues, and
`MALI-STATUS.md` for the kernel-side state this assumes as given.

## 0. Pre-flight

Guest state confirmed live (2026-08-11 evening):

```
$ uname -a
FreeBSD bsdos-chimp 15.1-RC3 FreeBSD 15.1-RC3 #2 9263fb9bab26-dirty: ... arm64
$ df -h /
/dev/vtbd0p3    6.0G    838M    4.7G    15%    /
cc: present (/usr/bin/cc)
pkg: present (/usr/sbin/pkg)
python3: ABSENT
meson: ABSENT
ninja: ABSENT
git: ABSENT
fetch: present (/usr/bin/fetch), curl: ABSENT
```

Note: the guest has no `/bin/ls` at all (minimal `bpi-headless`-derived image) —
not needed for anything below, just flagged so it isn't mistaken for a broken PATH.

## 1. Source acquisition — done on the host, not the guest

Per the task brief, tried the pinned mirror first, then found a better source.

- **This host's shell proxy (`http_proxy`/`https_proxy` -> `168.81.67.134:8000`)
  measured slower than a direct connection to GitHub for both tiny and large
  fetches** (tiny raw file: 1.75s proxied vs 0.55s direct; 20s-bounded tarball
  fetch: 0.78 MB/s proxied vs 1.82 MB/s direct). All fetches below used
  `env -u http_proxy -u https_proxy` (direct).
- First attempt: `github.com/chaotic-cx/mesa-mirror` tag `mesa-26.2.0`
  (`9f0a761020bca92f2b07156a0621e5360cb8eca5`) via
  `codeload.github.com/chaotic-cx/mesa-mirror/tar.gz/<sha>`. This is a **raw git
  snapshot tar.gz**, not a release tarball. A first 20s-bounded probe got
  36.4 MB at 1.82 MB/s (healthy); the real unbounded fetch attempt, however,
  **stalled hard** — 300s wall-clock produced only 19.6 MB total (avg 65 KB/s),
  i.e. it fell off a cliff after the initial burst. Abandoned this source rather
  than fight an inconsistent connection.
- Switched to Mesa's own official release mirror, **`archive.mesa3d.org`**,
  which is not blocked and is a plain static Apache file server (`Accept-Ranges:
  bytes` confirmed, so resumable):
  ```
  env -u http_proxy -u https_proxy curl -L -C - --retry 15 --retry-delay 5 \
    --connect-timeout 15 --max-time 1200 -o mesa-26.2.0.tar.xz \
    https://archive.mesa3d.org/mesa-26.2.0.tar.xz
  ```
  `Content-Length: 68461648` (~65 MiB, `.tar.xz` — much smaller than the raw
  git-snapshot `.tar.gz` would have been). Completed cleanly: **68,461,648
  bytes, 76.8s, avg 891 KB/s**. This is the **official release tarball for
  tag mesa-26.2.0** — same tag/version the feasibility research pinned, from
  Mesa's own publication host rather than a third-party mirror.
  - `sha256sum`: `efd4bb08cdb7c365a812cd4e6c9202ab55b2f22cdcd13c7d6c4f9647b799a4ef`
  - `xz -t`: integrity OK.
  - Extracted locally to verify before spending guest disk/CPU on it:
    **387 MB, 12,803 files**, `VERSION` file reads `26.2.0`, no `.git` (as
    expected for a release tarball), `src/gallium/drivers/lima/` present and
    populated, root `meson.build` is 2673 lines / `meson.options` 917 lines —
    both match the line counts `FEASIBILITY.md` cites, and the exact lines it
    quotes (line 162 `system_has_kms_drm`, the aarch64-default driver list
    including `'lima'`) are byte-identical in this real tree. `dri3`/`osmesa`
    confirmed absent from `meson.options` (removed options, as the feasibility
    appendix found); `shared-glapi` still present but deprecated.
  - **Decision: fetched on the host, will `scp` the compressed 68 MB `.tar.xz`
    to the guest and extract there**, rather than fetching on the guest
    directly. Rationale: the guest's own outbound-internet speed to arbitrary
    hosts was never verified (only `pkg.freebsd.org` was, per the task brief),
    the host's connection is known-fast once the proxy is bypassed, and 68 MB
    compressed is a much smaller transfer than 387 MB uncompressed — worth the
    guest's single core spending a few seconds on `xz` decompression instead.

## 2. An operational mistake, and its cleanup — read this before trusting timing numbers below

Early on, three separate `ssh root@192.168.88.82 '<cmd>'` invocations (a `pkg
search` batch and two `pkg update` attempts) were wrapped in a host-side shell
`timeout N` and got killed client-side when they ran long. **This did not stop
the remote commands.** Killing the local `ssh` client with `SIGKILL` (what
`timeout` does after its grace period) abandons the TCP connection without a
clean disconnect, so the remote `sh`/`pkg` processes kept running, undetected,
on the guest. Three generations of `pkg search`/`pkg update` piled up
concurrently, all contending for the same repo-catalog lock, on a
**single-core** guest — load average hit **1.92/2.04/1.05**, and a plain `ssh
... echo CONNECTED` timed out twice in a row ("Connection timed out during
banner exchange") because `sshd` itself couldn't get scheduled promptly.

No forbidden action was taken (no reboot, no boot-path write, no kldload/unload,
no `/etc` edit) and disk was never at risk (this was pure CPU/memory
contention), but it did make the guest transiently unresponsive over SSH for a
few minutes, and it wasted guest CPU cycles redundantly re-processing the same
package catalog three-to-four times over. Recovered by polling until SSH
answered again, then `kill`ing the specific orphaned PIDs (verified by exact
PID from `ps aux`, leaving the one legitimate in-flight `pkg update` alone).
After cleanup, that legitimate `pkg update` completed normally
(`All repositories are up to date.`, exit 0).

**Lesson applied for the rest of this session: no more host-side `timeout`
wrapping a remote `ssh` call that might run long.** Long-running guest
operations below use the harness's own backgrounding instead (which does not
SIGKILL the local ssh client), so the remote side always gets a clean
disconnect if anything needs to stop.

## 3. Build dependencies installed via `pkg`

**Session boundary: picked up cold on 2026-08-12.** The prior agent session ended
mid-`pkg install`, without a single package confirmed finished, and without
reporting that clearly (see the task brief this session started from). Everything
above this point is the prior session's own work, kept verbatim; everything below
is new.

State found at hand-off (first check, this session):

- `pkg install -y meson ninja pkgconf libdrm` (10 packages, 239 MiB) already
  running on the guest — PIDs 4170 (`sh` wrapper)/4171/4173. Confirmed alive
  but **slow**: at the 22m40s mark, `meson`/`ninja`/`python3`/`pkgconf` were
  *all* still absent, and `/` had grown only 996M -> 1.0G used in that window.
  No pkg lock-file contention (`/var/db/pkg/lock*` absent) — this reads as the
  same "guest's own link is slow for large transfers" pattern the task brief
  warned about (156 MB not finishing in 5 min), not a deadlock. Left it
  running rather than killing/restarting it: killing a live `pkg install`
  risks a half-updated package database, and §2 above is exactly this class
  of intervention backfiring on this same guest.
- Guest disk: `/dev/vtbd0p3 6.0G, 1.0G used, 4.5G avail, 18%` — healthy, far
  above the 1.0 GB floor.
- `cc` present (FreeBSD clang 19.1.7), `pkg` present, `flex` present (base).
  `bison`/`curl`/`python3`/`meson`/`ninja`/`pkgconf` all still absent, pending
  the install above.
- `/dev/dri/card0` and `/dev/dri/renderD128` present (symlinks into `../drm/{0,128}`),
  `drm.ko`/`lima.ko` both loaded — matches the task brief, not re-verified beyond
  confirming the links/modules are there.
- `/bin/ls`, `/sbin/shutdown`, `/sbin/poweroff` all present, plausible sizes
  (35352/15696/15696 bytes), timestamped Aug 12 09:31 — consistent with the
  brief's note that these were just restored from base.txz.

## 4. Source re-fetch, this session

The prior session's own `mesa-26.2.0.tar.xz` (verified good there, sha256
`efd4bb08cdb7c365a812cd4e6c9202ab55b2f22cdcd13c7d6c4f9647b799a4ef`) did not
survive into this session — agent scratch directories are per-session, and the
file lived in one, not in this repo. Re-fetched rather than hunting for it:
re-downloading a already-proven 65 MB source is cheap next to the risk of
trusting a stale copy.

- Re-confirmed §1's proxy finding independently: a fresh 12s-bounded sample of
  `codeload.github.com/chaotic-cx/mesa-mirror` got 400 KB/s through the proxy
  vs. 6.56 MB/s with `env -u http_proxy -u https_proxy` (direct) — over 16x.
  Also independently reconfirmed that endpoint's own burst-then-decay
  behavior: a live, unbounded fetch of that same URL (started before this
  comparison, proxied) decayed from a ~20 KB/s peak down to ~10 KB/s sustained
  over 98 s and was still falling — consistent with the prior session's report
  of the same source stalling to 65 KB/s after an initial burst. Killed that
  download and did not return to this source.
- Verified `archive.mesa3d.org` reachability explicitly, this session
  (`curl -v` HEAD, proxy bypassed): clean TLS handshake, valid Let's Encrypt
  cert (expires 2026-10-27), `HTTP/1.1 200`, `Content-Length: 68461648` —
  byte-for-byte the same size the prior session recorded for this file,
  strong evidence it's the identical artifact.
- Fetch command (proxy bypassed, resumable):
  ```
  cd <scratchpad>/mesa-build && env -u http_proxy -u https_proxy curl -L -C - \
    --retry 15 --retry-delay 5 --connect-timeout 15 --max-time 300 \
    -o mesa-26.2.0.tar.xz https://archive.mesa3d.org/mesa-26.2.0.tar.xz \
    -w "HTTP:%{http_code} size:%{size_download} time:%{time_total}s speed:%{speed_download}Bps\n"
  ```
  First attempt (same command, preceded by a `kill` of the abandoned codeload
  download) exited 1 with **zero captured output** — not even the first
  `echo`. Not diagnosed further since an immediate, unmodified rerun of just
  the connectivity check succeeded cleanly; flagged here in case the failure
  mode recurs. Second attempt (the connectivity check, `curl -v HEAD`) worked
  first try (see above). Third attempt (the real download) ran past the 300 s
  foreground window and was moved to the harness's own background runner —
  outcome recorded in §5 once checked.

## 5. Fetch result, and a repeat of the prior session's own logged mistake

The backgrounded fetch **succeeded**: it hit the same 300s wall clog partway
(`curl: (28) Operation timed out after 300000 milliseconds with 13847963 out
of 68461648 bytes received`, decaying to a ~45 KB/s crawl, same shape as the
codeload throttle in §4) but `--retry`+`-C -` resumed it, and the resumed leg
ran at a healthy 4.4-5.4 MB/s and finished the remaining ~55 MB in 15s.
Final: **68,461,648 bytes**, `sha256 efd4bb08cdb7c365a812cd4e6c9202ab55b2f22cdcd13c7d6c4f9647b799a4ef`
— **byte- and hash-identical to the prior session's own fetch** — `xz -t` OK.
Confirms this is the same, correct artifact via two independent fetches.

Then a repeat mistake, caught immediately: copying the tarball to the guest
with `timeout 180 scp ... ` (a host-side shell `timeout` wrapping a remote
transfer) hit the 180s ceiling and got **SIGKILLed by `timeout` itself** —
exactly the failure mode §2 above already diagnosed and warned against
("no more host-side `timeout` wrapping a remote `ssh` call that might run
long"), just applied to `scp` instead of `ssh` this time. The guest's LAN
link to this board is evidently also slow (not just this host's outbound
internet) — a 65 MB same-subnet `scp` did not finish in 3 minutes. Corrected
by relaunching via host-side `nohup ... &` (survives no external kill) instead
of a shell `timeout`, per the task brief's own guidance. No forbidden action
resulted; the guest was not touched beyond the ordinary (harmless) partial
file `/root/mesa-26.2.0.tar.xz` left mid-write, which the retried `scp`
overwrites by default.

## 6. Guest `pkg install`, still in progress — the guest's link is genuinely slow

At the ~35 minute mark: `meson`/`ninja`/`python3`/`pkgconf` still all absent;
`/var/cache/pkg` held **41 MiB of the expected 239 MiB** (~17%); the `pkg
install` process itself had accumulated only **20.6s of actual CPU time**
against 35 minutes of wall clock (`ps -o time`) — i.e. it is genuinely
I/O-bound waiting on the network, not spinning or deadlocked. Implied average
throughput to `pkg.freebsd.org`: roughly 20 KB/s. At that rate the remaining
~198 MiB is another ~2.5-3 hours. Left it running untouched (per §3's
reasoning); this is the dominant real-world cost of this whole task, not the
build itself.

## 7. Tarball transfer completed and verified

The `nohup`-relaunched `scp` (§5) finished cleanly on its own, no further
intervention needed: guest-side size **68,461,648 bytes** (exact match), and
`sha256` computed independently **on the guest** —
`efd4bb08cdb7c365a812cd4e6c9202ab55b2f22cdcd13c7d6c4f9647b799a4ef` — matches
both the host's own copy and the prior session's originally-recorded hash,
three-way. The guest's LAN link is not fast (this 65 MB took roughly half an
hour end-to-end across both the killed and resumed attempts) but it is not
broken; it just needs to not be killed mid-flight (see next).

## 8. Repeated the §2 mistake myself, twice, before it stuck

Despite having just read and written up §2/§5's warning against wrapping a
remote command in a host-side shell `timeout`, I did it again for the first
`tar xf` extraction attempt (`timeout 90 ssh ... 'tar xf ...'`). Consequence,
smaller-scale than §2's but the same shape: the local `timeout` SIGKILLed the
`ssh` client at 90s, the remote `tar` died with it (no `nohup`), and it left
a **partial, 135-file extraction** of what should be a ~12,800-file tree. The
very next command's `rm -rf mesa-26.2.0` then failed outright
(`rm: mesa-26.2.0: Directory not empty` — a mid-write tarball extraction can
leave a directory entry created before its would-be contents, or a permission
bit `tar` hadn't gotten around to relaxing yet, either of which trips plain
`rm`) — a small, self-inflicted cleanup problem, not a guest or Mesa problem.

**Corrected for real this time**: every guest command from here on either has
no shell-level `timeout` at all (relying on the harness's own behavior of
*backgrounding* a slow foreground command rather than killing it — observed
directly and confirmed safe with the §5 tarball fetch) or uses guest-side
`nohup ... &` for anything meant to keep running past the calling command's
own return. Diagnosed and re-attempted the extraction on that basis
(`chflags -R 0` to clear any lingering mode bits, then `rm -rf`, then a clean
`tar xf`) — outcome in §9.

## 9. The real cause of the "Directory not empty": an orphaned `tar`, not permissions

The diagnosis command itself (`find`/`ls -lao`) showed the partial tree's
directory mtimes advancing (`10:23`, `10:24`, `10:25`, seconds apart) *during*
a command that was only supposed to be listing it — the killed §8 `tar` was
never actually stopped. Killing the local `ssh` client with `timeout` had
detached from, but not terminated, the remote `tar xf`, which kept extracting
in the background the entire time, unsupervised, fighting every subsequent
`rm -rf`/re-`tar xf` for the same files. The next attempt to fix this (a
fresh `ssh` that itself ran a second, un-coordinated `tar xf` into the same
directory while the first was presumably still alive) made it worse: that
whole command's *SSH connection itself* died mid-command (exit 255), matching
§2's exact prior finding almost exactly (a single-core guest, multiple heavy
processes -- the orphaned `tar`, `pkg install`, a second `tar`, `sshd` -- goes
unresponsive enough that even the transport layer can't be serviced promptly).

Fixed properly: `pkill -9 -f "tar xf mesa-26.2.0.tar.xz"` first, a pause,
*then* `chflags`/`rm -rf`/one single `tar xf`, all as one `nohup`-free (this
one genuinely needs to run to completion, not survive past the call)
foreground command with no shell-level `timeout` -- relying entirely on the
harness's own background-not-kill behavior if it runs long. Outcome below.

## 10. Independently verified every planned `meson` flag against the real 26.2.0 tarball

While the guest was busy, cross-checked every option this log's planned
`meson setup` invocation touches against `meson.options` in the **actual
fetched-and-verified tree** (not just FEASIBILITY.md's git-snapshot reading —
the two should agree, since a tagged release and its snapshot share the same
option set, but this closes the gap between "should" and "checked"):

| option | type (real 26.2.0 `meson.options`) | plan passes | valid? |
|---|---|---|---|
| `gallium-drivers` | array, choices include `'lima'` | `lima` | yes |
| `platforms` | array, choices are window-system integrations only (`auto, x11, wayland, haiku, android, windows, macos`) — no `surfaceless` entry exists, confirming it's always-on and not selected via this option | `` (empty) | yes |
| `vulkan-drivers` | array | `` (empty) | yes |
| `glx` | combo, choices include `disabled` | `disabled` | yes |
| `egl` | feature | `enabled` | yes |
| `gbm` | feature | `enabled` | yes |
| `llvm` | feature | `disabled` | yes |
| `opengl` | boolean, default `true` | `true` | yes |
| `gles1` | feature | `disabled` | yes |
| `gles2` | feature | `enabled` | yes |
| `video-codecs` | array, default `['all_free']` | `` (empty, overrides default) | yes |
| `build-tests` | boolean, default `false` | `false` | yes (matches default) |
| `tools` | array, default `[]` (`'lima'` is a valid *tool* choice too -- a separate standalone utility, not the driver -- irrelevant here since default is already empty) | `` (empty, matches default) | yes |
| `shared-glapi` | feature, **`deprecated: true`** | not passed | correct to omit |
| `dri3`, `osmesa` | **no `option()` block exists for either name anywhere in `meson.options`** | not passed | confirms these are fully removed, not just deprecated |

Every flag in the planned command is confirmed valid against the exact bytes
that will run on the guest. This was a purely local, zero-guest-load check
(the tarball extracted a second time on the host, into scratch, for reading
only) — nothing here touched the board.

## 11. Final status as of this write-up — dependency staging still in flight, build not started

Being explicit about exactly where this stands, per the task's own guidance
that "ran out of budget mid-setup" is a legitimate result to report precisely
rather than round up:

- **Mesa 26.2.0 source: fully fetched, verified, and staged on the guest.**
  `/root/mesa-26.2.0.tar.xz`, sha256 `efd4bb08cdb7c365a812cd4e6c9202ab55b2f22cdcd13c7d6c4f9647b799a4ef`,
  confirmed three ways (host, guest, and the prior session's independent
  fetch). Not yet cleanly extracted at the time of this write-up -- see next.
- **Extraction: in progress, not confirmed complete.** After the §8/§9
  orphaned-`tar` incident was cleaned up (both stray `tar` processes killed,
  partial tree removed), a single clean `tar xf mesa-26.2.0.tar.xz` was
  launched (no shell timeout, harness-backgrounded) and was still running at
  last check, with no output yet (expected -- plain `tar xf` is silent until
  it finishes or errors). The one data point available on how long this
  genuinely takes on this CPU: the first (orphaned, contended) attempt burned
  **3:47 of actual CPU time** and had only gotten as far as creating
  `src/amd/` -- alphabetically the *first* entry under `src/`, so this is not
  a reliable "close to done" signal, just a lower bound on cost. A clean
  extraction, still sharing the one core with the `pkg install` below, should
  finish; there is no reason to expect it to fail, but it had not reported
  back as of this log entry.
- **`pkg install -y meson ninja pkgconf libdrm`: still running, ~1h elapsed.**
  `pkgconf` is the only one of the four confirmed installed so far (`meson`,
  `ninja`, `python3` all still absent at last check). `/var/cache/pkg` was at
  42 MiB of the expected 239 MiB for most of the session -- it may simply not
  reflect an in-progress package's cache entry until that package's fetch
  fully lands, so "unchanged" is not the same as "stalled"; the process's own
  CPU-time (climbing steadily: 20.6s -> 44.6s -> 1:17.8 -> 1:28.8 across the
  session) confirms it is doing real, if slow, work throughout, consistent
  with the same slow-guest-link pattern documented for the tarball transfer.
- **Not yet run, and blocked on the above finishing cleanly:** the follow-up
  `pkg install -y bison expat2 zstd <pyver>-mako <pyver>-yaml <pyver>-ply`
  (deliberately not started concurrently with the still-running install, to
  avoid repeating §2's exact concurrency mistake), `meson setup`, and `ninja`.
- **Prepared and ready, not yet transferred to the guest** (held back
  specifically to avoid adding any load to the guest's single core while the
  extraction/install above are running):
  - `build_lima.sh` -- a single script that does the follow-up `pkg install`,
    checksum-reverifies the tarball, extracts, runs the exact `meson setup`
    from §10 with output captured to `/root/meson-setup.log`, and -- only if
    `build/build.ninja` actually exists afterward -- launches
    `nohup ninja -C build > /root/ninja-build.log 2>&1 &` in the background.
    Syntax-checked (`sh -n`) clean.
  - `lima_smoke.c` -- a minimal headless EGL/GLES2 smoke test: opens
    `/dev/dri/renderD128` directly via `gbm`, creates a GBM-platform EGL
    display, binds a GLES2 context on a 4x4 pbuffer, clears to solid red,
    `glFinish()`s (the real completion-wait path, per FEASIBILITY.md's
    `drmSyncobjWait` callout), reads the pixels back, and checks they are
    uniformly red. Flags a non-uniform result as a possible symptom of the
    known `lima_vm.c:87` cacheable-mapping gap rather than "nothing rendered."
    Not yet compiled (needs the Mesa build's own `libEGL`/`libGLESv2`/`libgbm`,
    which do not exist yet).
- **Disk, both sides, healthy:**
  - Guest `/`: `6.0G` total, last confirmed reading **`1.2G` used / `4.3G`
    avail (22%)** -- comfortably above the 1.0 GB floor throughout, and the
    remaining Mesa build (source + objects) was independently estimated at
    "well under 1 GB more" (`FEASIBILITY.md` §4), leaving margin.
  - Host: `/` at 76G free / 930G, `/tmp` (tmpfs) at 6.7G free / 7.8G -- no
    concern.
- **No forbidden action taken.** No reboot/reset/power/kldload/`/etc` edit/
  tftpboot write on the guest at any point. The two self-inflicted incidents
  (§5's killed `scp`, §8/§9's killed `tar` + its orphan) were host-side
  process-management mistakes with a guest-side side effect of leaving inert
  partial files/processes -- never a forbidden category of action, and both
  were fully diagnosed and cleaned up rather than worked around.

**What actually blocks "it builds" / "it renders" right now is wall-clock,
not a technical unknown**: this guest's link is measured at roughly
20-45 KB/s for both its own internet egress (`pkg install`) and, intermittently,
its LAN throughput under CPU contention (the `scp`/`tar` incidents above) --
on a single physical core that a ~239 MiB dependency install and a
~387 MB/12,803-file source extraction both have to share before the actual
multi-hour `ninja` build can even begin. Every step attempted so far has
succeeded once given enough wall-clock and once the host-side "don't kill a
slow remote command" discipline was actually followed; nothing here points to
a FreeBSD-, lima-, or Mesa-specific defect.
