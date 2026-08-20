/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright 2018-2019 Qiang Yu <yuq825@gmail.com> */
/* FreeBSD port: Copyright 2024 bsdOS contributors */
/* SPDX-License-Identifier: BSD-2-Clause (port additions) */

/*
 * MODULE:      hal/lima/lima_ctx.h
 * PURPOSE:     GPU rendering-context lifecycle — per-process handle table
 *              and scheduler-context binding for Lima Mali-400 under
 *              FreeBSD 15.1 drm-66-kmod.
 * PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_ctx.h
 *
 * NOTE: All Linux APIs here (kref, xarray, atomic_t, TASK_COMM_LEN,
 *       pid_t) are provided verbatim by drm-66-kmod LinuxKPI.
 *       No FreeBSD-specific substitutions are needed in this header.
 */

#ifndef __LIMA_CTX_H__
#define __LIMA_CTX_H__

/*
 * LinuxKPI provides <linux/xarray.h>, <linux/sched.h>, and
 * <linux/kref.h> under drm-66-kmod.  The include paths resolve
 * to ${SYSDIR}/contrib/drm-66kmod/include/linux/.
 */
#include <linux/xarray.h>      /* struct xarray, xa_init, xa_erase */
#include <linux/sched.h>       /* TASK_COMM_LEN                     */

#include "lima_device.h"       /* struct lima_device, lima_pipe_num */

/*
 * struct lima_ctx — per-process GPU rendering context
 *
 * purpose:     Tracks one userspace context's lifetime, the device it
 *              belongs to, its scheduler slots on each pipe, and a
 *              guilty flag set by the fault handler when a job hangs.
 * input:       Allocated by lima_ctx_create(); released when refcnt
 *              drops to zero via kref_put().
 * output:      Exposed through lima_ctx_mgr handles (u32 id).
 * sideEffects: None on construction; job submission and GPU faults
 *              mutate .guilty and .context[].
 */
struct lima_ctx {
	struct kref              refcnt;       /* LinuxKPI kref — unchanged */
	struct lima_device      *dev;          /* owning Lima device         */
	struct lima_sched_context context[lima_pipe_num];  /* GP + PP pipes */
	atomic_t                 guilty;       /* set by timeout/fault handler */

	/* debug info — preserved verbatim from Linux */
	char    pname[TASK_COMM_LEN];           /* comm name of creating task */
	pid_t   pid;                            /* PID of creating task       */
};

/*
 * struct lima_ctx_mgr — per-device context handle table
 *
 * purpose:     Maps userspace u32 handles to lima_ctx pointers,
 *              protected by a mutex.  One mgr per lima_device.
 * input:       Initialised by lima_ctx_mgr_init(); torn down by
 *              lima_ctx_mgr_fini() which frees any leaked contexts.
 * output:      Slot IDs returned by lima_ctx_create().
 * sideEffects: .handles xarray grows with each lima_ctx_create call.
 */
struct lima_ctx_mgr {
	struct mutex   lock;     /* LinuxKPI mutex — maps to FreeBSD sx under kmod */
	struct xarray  handles;  /* u32 → lima_ctx*, LinuxKPI xarray passthrough  */
};

/*
 * lima_ctx_create — allocate a new context and register it in mgr
 *
 * purpose:  Kernel entry point for DRM_IOCTL_LIMA_CTX_CREATE.  Allocates
 *           struct lima_ctx, initialises kref and scheduler slots,
 *           records comm/pid, and stores the ctx in mgr->handles.
 * input:    dev — owning device; mgr — per-device ctx table; id — out.
 * output:   0 on success; negative errno on failure.  *id set to the
 *           new handle.
 * sideEffects: allocates memory; holds mgr->lock briefly; may sleep.
 */
int lima_ctx_create(struct lima_device *dev,
                    struct lima_ctx_mgr *mgr,
                    u32 *id);

/*
 * lima_ctx_free — drop handle from mgr and release one ref
 *
 * purpose:  Kernel entry for DRM_IOCTL_LIMA_CTX_FREE.  Removes the
 *           handle from the xarray then calls lima_ctx_put() which
 *           destroys the ctx if no other refs remain.
 * input:    mgr — per-device ctx table; id — handle to free.
 * output:   0 on success; -EINVAL if id is not present.
 * sideEffects: holds mgr->lock briefly; may free memory.
 */
int lima_ctx_free(struct lima_ctx_mgr *mgr, u32 id);

/*
 * lima_ctx_get — look up handle and bump refcount
 *
 * purpose:  Safe borrow — increments kref before returning pointer so
 *           caller holds a stable reference across submit paths.
 * input:    mgr — per-device ctx table; id — handle to look up.
 * output:   Non-NULL lima_ctx* on success; NULL if id unknown.
 * sideEffects: increments ctx->refcnt; caller must call lima_ctx_put().
 */
struct lima_ctx *lima_ctx_get(struct lima_ctx_mgr *mgr, u32 id);

/*
 * lima_ctx_put — release one reference, destroy ctx when last ref drops
 *
 * purpose:  Paired with lima_ctx_get() and lima_ctx_create().  Calls
 *           kref_put() with an internal release callback that tears
 *           down scheduler contexts and frees the allocation.
 * input:    ctx — context to release; must not be NULL.
 * output:   void.
 * sideEffects: may free ctx and scheduler resources.
 */
void lima_ctx_put(struct lima_ctx *ctx);

/*
 * lima_ctx_mgr_init — zero-initialise handle table and mutex
 *
 * purpose:  Called from lima_device_init().  Sets up the xarray with
 *           XA_FLAGS_ALLOC so xa_alloc() auto-assigns u32 IDs.
 * input:    mgr — uninitialised ctx manager embedded in lima_device.
 * output:   void (cannot fail on FreeBSD; mutex_init never fails).
 * sideEffects: writes mgr->lock and mgr->handles.
 */
void lima_ctx_mgr_init(struct lima_ctx_mgr *mgr);

/*
 * lima_ctx_mgr_fini — destroy handle table, warn and free leaked contexts
 *
 * purpose:  Called from lima_device_fini().  Walks remaining xarray
 *           entries (leaked by userspace), emits DRM_ERROR for each,
 *           and force-releases them.
 * input:    mgr — active ctx manager.
 * output:   void.
 * sideEffects: destroys all remaining lima_ctx objects; invalidates mgr.
 */
void lima_ctx_mgr_fini(struct lima_ctx_mgr *mgr);

#endif /* __LIMA_CTX_H__ */
