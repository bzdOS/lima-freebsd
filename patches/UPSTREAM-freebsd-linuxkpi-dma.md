# Upstream: two LinuxKPI DMA defects on non-coherent arm64

**Status: NOT SUBMITTED.** Written up here so submission is a send, not a
research project. Submitting needs the author's own FreeBSD accounts.

| | |
|---|---|
| Tree | `freebsd-src`, `releng/15.1` (reproduced on 15.1-RELEASE) |
| File | `sys/compat/linuxkpi/common/src/linux_pci.c` |
| Patches | `freebsd-src/freebsd-linuxkpi-01-dma-map-sg-multipage.patch`, then `freebsd-src/freebsd-linuxkpi-02-dma-alloc-coherent-memattr.patch` — **in that order** (verified: applied in sequence to `134a4b503^` they reproduce the committed tree byte-for-byte) |
| Commits | `134a4b503`, `39090bd6f` on branch `linuxkpi-dma-fix` |
| Hardware | Banana Pi M64 (Allwinner A64, Cortex-A53), Mali-400 MP2 via drm-kmod + Mesa/lima |
| Blast radius | Every non-coherent-DMA platform — i.e. essentially every arm64 SoC GPU. x86 cannot observe either bug. |

Both are **arm64-only by construction**, which is why they survived: the paths
are exercised constantly on amd64, where DMA is coherent by architecture and
neither defect has an observable effect.

## 1. `dma_alloc_coherent()` returned CACHEABLE memory

`kmem_alloc_contig(..., VM_MEMATTR_DEFAULT)` is write-back on arm64. So the one
allocator whose entire contract is *"no cache maintenance required"* handed out
cacheable memory and passed its physical address to a device. The CPU's stores
sit in cache, the device reads DRAM and sees stale contents, and nobody flushes
anything — because a caller of this API is entitled to assume it need not.

**Fix:** `VM_MEMATTR_UNCACHEABLE` on aarch64/arm/riscv, matching what
`bus_dmamem_alloc(BUS_DMA_COHERENT)` already does in the same situation.
Unchanged elsewhere, so no platform gives up write-back caching for nothing.

**How it was found — worth repeating, because the wrong answer was available.**
lima allocates its Mali GPU page tables through this path, and the GPU MMU
faulted on an address whose PTE the CPU could read back perfectly well:

    mmu page fault at 0x286000 ... of type read on gpmmu
      -> PTE = 0x649371df: the page IS mapped

The mapping code, the TLB-zap implementation and both of its call sites were
each verified byte-identical to upstream Linux **before** reaching for a
coherency explanation. The tables were correct and simply not visible to the
device.

**Result:** first frame. 5/5 runs, zero GPU MMU faults.

## 2. `dma_map_sg()` cannot map a multi-page list

Two independent defects on one path.

**(a) `nsegments = 1` makes a multi-page `dma_map_sg()` impossible.** The
coupling is invisible from `linux_dma_tag_init()`: busdma bounds the per-map
*sync list* by `dmat->common.nsegments`, and `sync_count` **accumulates across
calls on the same map**:

    } else if ((map->flags & DMAMAP_COHERENT) == 0) {
            if (map->sync_count == 0 || curaddr != sl_end) {
                    if (++map->sync_count > dmat->common.nsegments)
                            break;      -> buflen != 0 -> EFBIG

`linux_dma_map_sg_attrs()` loads every S/G entry into one shared map, so entry 0
takes the single slot and entry 1 fails. Measured, with a diagnostic on the
failure path:

    lkpi dma_map_sg: load_phys failed err=27 i=1/64 phys=0x54c02000 len=4096

Failure on the **second** entry of a 64-entry list, every run. A coherent map
never touches that counter — hence arm64-only. Raised to 1024; `nsegments` also
sizes the map allocation (`sizeof(*map) + sizeof(struct sync_list) * nsegments`),
which is affordable because the sync list only grows on a **discontiguity**.

**(b) The failure paths leave `sgl->dma_map` stale, and unmap validates
nothing.** `linux_dma_unmap_sg_attrs()` goes straight to
`bus_dmamap_sync(priv->dmat, sgl->dma_map, ...)`. The map function returns 0 on
failure while leaving that field bad three different ways:
`bounce_bus_dmamap_create()` returns ENOMEM from its `dmat->segments`
allocation **without writing `*mapp` at all**; `alloc_dmamap()` failure leaves
NULL; and the `load_phys` path destroys the map but leaves the pointer
reachable. A caller whose teardown then unmaps — which drm/lima does —
dereferences it:

    far 0x1  esr 0x96000004      elr bounce_bus_dmamap_sync+0x34c
                                 lr  linux_dma_unmap_sg_attrs+0xac

This one is a NULL/stale-pointer dereference reachable from an ordinary error
path, so it is worth submitting even to maintainers who consider (a) a tuning
question.

## Note for a reviewer

These were found while bringing up an out-of-tree Mali-400 DRM driver, so the
reproducer is not something a maintainer can run directly. What *is* directly
checkable is the reasoning in (2a) — the `nsegments`/`sync_count` coupling is
plain in `sys/arm64/arm64/busdma_bounce.c` — and (2b), which needs no hardware
at all to read as a bug.
