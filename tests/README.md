# Mali-400 / lima on-hardware tests

Off-screen only. lima is render-only (`DRIVER_RENDER`, no `MODESET`), so both
tests render into an FBO and read the pixels back — nothing appears on a monitor
by this path, and that is deliberate: it makes them usable on any board with no
display bring-up at all.

(Updated 2026-08-20: "this board has no FreeBSD display driver" is no longer
true in the project these tests came from — output does reach a monitor there,
through a separate KMS driver that is not part of this repository. The tests
below are unaffected and still the right first thing to run, because they answer
"is the GPU rasterising" without depending on any display path.)

| test | what it proves |
|---|---|
| `limatri.c` | the GPU executes a job at all: one clear (PP), one untextured triangle (GP builds a tile list, PP rasterises), 64x64. The first-frame milestone. |
| `limabench.c` | that GL actually works: a sampled texture, 2420 draw calls per run, depth testing, alpha blending, 512x512 with a depth buffer, over 10 frames. |

`limatri` passing is not evidence that `limabench` will: a single triangle
exercises neither the texture unit, nor tile-list growth (the heap BO), nor depth,
nor blending. That gap is why the second test exists.

## Cross-compile on the Linux host

Needs the Mesa cross-build staging tree (headers + libs) and a FreeBSD sysroot:

    clang --target=aarch64-unknown-freebsd15.1 -fuse-ld=lld \
      --sysroot=<sysroot> -I<stage>/usr/local/include \
      -O2 -o limabench limabench.c \
      -L<stage>/usr/local/lib -lEGL -lGLESv2 -lgbm

`-fuse-ld=lld` is not optional: without it clang picks the host's GNU ld, which
rejects the aarch64 `crt1.o` with "relocations in generic ELF (EM: 183)".

## Running

    kldload /boot/modules/drm.ko
    kldload /boot/modules/lima.ko
    LD_LIBRARY_PATH=/usr/local/lib ./limabench

No `LIMA_DEBUG` flag is needed. It used to require `LIMA_DEBUG=nogrowheap`
because `lima_heap_alloc()` returned `-ENOSYS`; that is implemented now.

## Reading the results honestly

`limabench` checks pixels, not just absence of a crash: a known texel colour in
two different texture quadrants, that depth did not reject the nearer quad, and
that blending produced a mix. Its first version reported a driver failure that was
its own arithmetic — it sampled 40 px from the quad's centre when the quad is only
38.4 px across, landing outside it. Sample points are inside the geometry now, and
the lesson is in the comment: a failing on-hardware test is a claim about the
driver, so the test's own geometry has to be checked before the claim is believed.

Measured 2026-08-19, Banana Pi M64 (A64, Mali-400 MP2), FreeBSD 15.1 aarch64 guest
under the bzdOS EL2 hypervisor: 2420 draw calls in ~125 ms (~80 frames/s), 4/4
runs, zero GPU MMU faults.
