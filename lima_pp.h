/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright 2017-2019 Qiang Yu <yuq825@gmail.com>
 * Copyright 2024 bsdOS contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

// MODULE: hal/lima/lima_pp.h
// PURPOSE: Declare pixel-processor (PP) init/suspend/resume lifecycle and broadcast-group interface for the Lima Mali-400 DRM driver on FreeBSD 15.1
// PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_pp.h

/*
 * purpose:   Public interface for Lima PP (pixel processor) lifecycle management.
 *            Covers per-PP init/fini/suspend/resume, broadcast-group operations
 *            for PP arrays, and the higher-level pipeline init that wires PP cores
 *            into the DRM scheduler.
 * input:     struct lima_ip * — per-IP-block descriptor (PP or PP_BCAST);
 *            struct lima_device * — parent Lima device for pipeline-level ops.
 * output:    0 on success; negative errno on failure for int-returning functions.
 * sideEffects: modifies hardware register state (power, IRQ mask) and allocates
 *              DRM scheduler resources on lima_pp_pipe_init.
 *
 * Target:    FreeBSD 15.1 aarch64, drm-66-kmod LinuxKPI layer.
 *            Tested hardware: Allwinner A64 Mali-400 MP2 (PinePhone Pro — Porcupine v0.3).
 *            MMIO base 0x01C40000, size 0x10000 (see hal/mali_uio.h).
 */

#ifndef __LIMA_PP_H__
#define __LIMA_PP_H__

/*
 * Forward declarations — full definitions live in lima_device.h / lima_ip.h.
 * Under drm-66-kmod these translate to the LinuxKPI struct wrappers; no
 * FreeBSD-specific aliases are needed here because the Lima C files include
 * the full Linux-compatible headers via <drm/drm_drv.h> et al.
 */
struct lima_ip;
struct lima_device;

/* ---------------------------------------------------------------------------
 * Per-PP (pixel processor) lifecycle
 *
 * Each Mali-400 PP block (PP0..PP3) is treated as an independent lima_ip.
 * init()    — map MMIO, probe version registers, install IRQ handler, register
 *             with the DRM scheduler engine.
 * fini()    — undo init(); called on driver detach or fatal error.
 * resume()  — restore register state after runtime-PM suspend or system sleep.
 * suspend() — save volatile register state; gate clocks via pm_runtime_*.
 * ------------------------------------------------------------------------- */

/*
 * lima_pp_resume
 *
 * purpose:  Restore a single PP block from runtime-PM or system-suspend state.
 *           Re-enables the PP IRQ and resets the hardware state machine so the
 *           block is ready to accept rendering jobs.
 * input:    ip — pointer to the lima_ip descriptor for this PP core.
 * output:   0 on success; -EIO if the PP does not respond within the timeout.
 * sideEffects: writes PP_STATUS and PP_INT_MASK MMIO registers; may call
 *              pm_runtime_get_sync() on the parent device.
 */
int lima_pp_resume(struct lima_ip *ip);

/*
 * lima_pp_suspend
 *
 * purpose:  Gate a single PP block for runtime-PM or system sleep.
 *           Masks the PP IRQ and signals pm_runtime_put_autosuspend() so the
 *           power domain can be removed after the autosuspend delay.
 * input:    ip — pointer to the lima_ip descriptor for this PP core.
 * output:   void (errors are logged but not propagated; suspend must succeed).
 * sideEffects: clears PP_INT_MASK; calls pm_runtime_put_autosuspend().
 */
void lima_pp_suspend(struct lima_ip *ip);

/*
 * lima_pp_init
 *
 * purpose:  Probe and initialise one PP core: map MMIO window, read version
 *           register, install IRQ handler, and register a drm_sched_entity.
 * input:    ip — pre-allocated lima_ip with resource IDs filled by the bus layer.
 * output:   0 on success; negative errno (ENOMEM, EIO, EINVAL) on failure.
 * sideEffects: allocates IRQ (bus_setup_intr equivalent via LinuxKPI request_irq),
 *              creates a drm_sched_entity; both are released in lima_pp_fini().
 */
int lima_pp_init(struct lima_ip *ip);

/*
 * lima_pp_fini
 *
 * purpose:  Release all resources acquired by lima_pp_init() for one PP core.
 * input:    ip — initialised lima_ip descriptor.
 * output:   void.
 * sideEffects: frees IRQ, destroys drm_sched_entity, unmaps MMIO.
 */
void lima_pp_fini(struct lima_ip *ip);

/* ---------------------------------------------------------------------------
 * PP broadcast group (PP_BCAST)
 *
 * Mali-400 MP2+ exposes a single broadcast register window that fans writes
 * out to all PP cores simultaneously, used for parallel job dispatch.
 * The bcast variant mirrors the per-PP API but targets the PP_BCAST ip block.
 * ------------------------------------------------------------------------- */

/*
 * lima_pp_bcast_resume
 *
 * purpose:  Resume the PP broadcast block after runtime-PM or system sleep.
 *           Mirrors lima_pp_resume() for the broadcast IP.
 * input:    ip — lima_ip descriptor for the PP_BCAST block.
 * output:   0 on success; -EIO on hardware timeout.
 * sideEffects: writes PP_BCAST MMIO; calls pm_runtime_get_sync().
 */
int lima_pp_bcast_resume(struct lima_ip *ip);

/*
 * lima_pp_bcast_suspend
 *
 * purpose:  Suspend the PP broadcast block; mirrors lima_pp_suspend().
 * input:    ip — lima_ip descriptor for the PP_BCAST block.
 * output:   void.
 * sideEffects: clears PP_BCAST IRQ mask; calls pm_runtime_put_autosuspend().
 */
void lima_pp_bcast_suspend(struct lima_ip *ip);

/*
 * lima_pp_bcast_init
 *
 * purpose:  Initialise the PP broadcast block: MMIO map + version check only;
 *           no per-core IRQ is registered (individual PP IRQs handle completion).
 * input:    ip — lima_ip with broadcast resource IDs filled by the bus layer.
 * output:   0 on success; negative errno on failure.
 * sideEffects: maps PP_BCAST MMIO window; released in lima_pp_bcast_fini().
 */
int lima_pp_bcast_init(struct lima_ip *ip);

/*
 * lima_pp_bcast_fini
 *
 * purpose:  Release resources allocated by lima_pp_bcast_init().
 * input:    ip — initialised PP_BCAST lima_ip descriptor.
 * output:   void.
 * sideEffects: unmaps PP_BCAST MMIO window.
 */
void lima_pp_bcast_fini(struct lima_ip *ip);

/* ---------------------------------------------------------------------------
 * Pipeline-level PP management
 *
 * lima_pp_pipe_init / lima_pp_pipe_fini operate on the full lima_device and
 * initialise the DRM GPU scheduler pipe that services all PP cores together.
 * They must be called after all individual lima_pp_init() calls succeed.
 * ------------------------------------------------------------------------- */

/*
 * lima_pp_pipe_init
 *
 * purpose:  Create the DRM scheduler pipe for the PP subsystem: allocates a
 *           drm_gpu_scheduler, links all PP drm_sched_entity objects into it,
 *           and registers the run-queue with the Lima scheduler backend.
 * input:    dev — fully probed lima_device with all PP cores initialised.
 * output:   0 on success; -ENOMEM if scheduler allocation fails.
 * sideEffects: allocates drm_gpu_scheduler (heap); registered entities reference
 *              it — lima_pp_pipe_fini() must be called before lima_pp_fini().
 */
int lima_pp_pipe_init(struct lima_device *dev);

/*
 * lima_pp_pipe_fini
 *
 * purpose:  Tear down the DRM scheduler pipe created by lima_pp_pipe_init().
 *           Flushes any pending jobs, stops the scheduler thread, and frees
 *           the drm_gpu_scheduler.
 * input:    dev — lima_device whose PP pipe is to be destroyed.
 * output:   void.
 * sideEffects: drm_sched_fini(); all PP drm_sched_entity objects become invalid
 *              after this call — must be called before lima_pp_fini() on each core.
 */
void lima_pp_pipe_fini(struct lima_device *dev);

#endif /* __LIMA_PP_H__ */
