// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright 2018-2019 Qiang Yu <yuq825@gmail.com>
 * Copyright 2024 bsdOS Project (FreeBSD port)
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

// MODULE: hal/lima/lima_ctx.c
// PURPOSE: Manage per-process DRM rendering contexts (create, destroy, get/put refcount) for the Lima Mali-400 driver on FreeBSD 15.1
// PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_ctx.c

/*
 * FreeBSD porting notes (drm-66-kmod / LinuxKPI):
 *
 *   kzalloc / kfree
 *     LinuxKPI provides these via <linux/slab.h>; they map to
 *     malloc(M_DRM, M_WAITOK | M_ZERO) / free(M_DRM) internally.
 *     No change needed in calling code.
 *
 *   kref_init / kref_get / kref_put
 *     Provided by LinuxKPI <linux/kref.h>.  Semantics identical to Linux.
 *
 *   xa_alloc / xa_erase / xa_load / xa_init_flags / xa_destroy / xa_for_each
 *     XArray is part of LinuxKPI in drm-66-kmod.  All XA_FLAGS_ALLOC,
 *     xa_limit_32b, and the iteration macro xa_for_each work as-is.
 *
 *   mutex_init / mutex_lock / mutex_unlock / mutex_destroy
 *     LinuxKPI mutex shims over FreeBSD sx(9) locks.  No change needed.
 *
 *   task_pid_nr(current) / get_task_comm(buf, current)
 *     LinuxKPI provides current (curthread wrapper), task_pid_nr(), and
 *     get_task_comm().  On FreeBSD, task_pid_nr() returns curthread->td_proc->p_pid
 *     and get_task_comm() copies curthread->td_proc->p_comm into the buffer.
 *     The TASK_COMM_LEN-sized pname field in struct lima_ctx accommodates this.
 *
 *   container_of
 *     Standard C macro; provided by LinuxKPI <linux/kernel.h>.
 *
 *   lima_sched_context_init / lima_sched_context_fini
 *     Declared in lima_sched.h; implemented in lima_sched.c — no porting
 *     changes required at the call sites in this file.
 *
 * Target: FreeBSD 15.1 aarch64, drm-66-kmod LinuxKPI layer.
 * Tested hardware (planned): Allwinner A64 Mali-400 MP2
 *   (PinePhone Pro — Porcupine v0.3, MMIO 0x01C40000, see ../mali_uio.h).
 */

#include <linux/slab.h>

#include "lima_device.h"
#include "lima_ctx.h"

/*
 * lima_ctx_create
 *
 * purpose:  Allocate and initialise a new per-process Lima rendering context:
 *           zero-fill the context structure, attach it to the device, initialise
 *           a refcount of 1, set up one lima_sched_context per scheduler pipe
 *           (GP and PP), insert the context into the manager's XArray handle
 *           table, and record the calling process's PID and command name.
 * input:    dev — the Lima device this context belongs to
 *           mgr — the per-file lima_ctx_mgr that owns the XArray handle table
 *           id  — output: assigned handle index (xa_limit_32b, 32-bit)
 * output:   0 on success, *id populated with the new handle
 *           -ENOMEM if kzalloc fails or if xa_alloc cannot obtain a slot
 *           negative errno from lima_sched_context_init on pipe failure
 * sideEffects:
 *           allocates heap memory for struct lima_ctx (freed on final kref_put)
 *           inserts ctx into mgr->handles XArray (removed on lima_ctx_free)
 *           calls lima_sched_context_init for each pipe (undone on error or fini)
 *           records current->pid and comm in ctx->pid / ctx->pname
 */
int lima_ctx_create(struct lima_device *dev, struct lima_ctx_mgr *mgr, u32 *id)
{
	struct lima_ctx *ctx;
	int i, err;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->dev = dev;
	kref_init(&ctx->refcnt);

	for (i = 0; i < lima_pipe_num; i++) {
		err = lima_sched_context_init(dev->pipe + i, ctx->context + i, &ctx->guilty);
		if (err)
			goto err_out0;
	}

	err = xa_alloc(&mgr->handles, id, ctx, xa_limit_32b, GFP_KERNEL);
	if (err < 0)
		goto err_out0;

	ctx->pid = task_pid_nr(current);
	get_task_comm(ctx->pname, current);

	return 0;

err_out0:
	for (i--; i >= 0; i--)
		lima_sched_context_fini(dev->pipe + i, ctx->context + i);
	kfree(ctx);
	return err;
}

/*
 * lima_ctx_do_release
 *
 * purpose:  Final destructor for a Lima context, invoked by kref_put when the
 *           refcount reaches zero.  Tears down each scheduler context in pipe
 *           order then frees the structure.
 * input:    ref — embedded kref pointer; recovered to lima_ctx via container_of
 * output:   void
 * sideEffects:
 *           calls lima_sched_context_fini for each pipe (lima_pipe_num iterations)
 *           frees the lima_ctx allocation via kfree
 *
 * FreeBSD/linuxkpi: container_of and kfree provided by LinuxKPI
 * (<linux/kernel.h> and <linux/slab.h> respectively).
 */
static void lima_ctx_do_release(struct kref *ref)
{
	struct lima_ctx *ctx = container_of(ref, struct lima_ctx, refcnt);
	int i;

	for (i = 0; i < lima_pipe_num; i++)
		lima_sched_context_fini(ctx->dev->pipe + i, ctx->context + i);
	kfree(ctx);
}

/*
 * lima_ctx_free
 *
 * purpose:  Remove a context from the manager's handle table by ID and drop
 *           the handle-table reference (kref_put).  If the handle does not
 *           exist, return -EINVAL.  The context is freed only when all other
 *           held references (e.g. in-flight GPU jobs) are also released.
 * input:    mgr — the lima_ctx_mgr whose XArray table is to be updated
 *           id  — handle index previously returned by lima_ctx_create
 * output:   0 on success
 *           -EINVAL if id is not in mgr->handles
 * sideEffects:
 *           acquires and releases mgr->lock (LinuxKPI mutex)
 *           erases the XArray entry; may call lima_ctx_do_release if refcount
 *           drops to zero (which frees all scheduler contexts and the struct)
 */
int lima_ctx_free(struct lima_ctx_mgr *mgr, u32 id)
{
	struct lima_ctx *ctx;
	int ret = 0;

	mutex_lock(&mgr->lock);
	ctx = xa_erase(&mgr->handles, id);
	if (ctx)
		kref_put(&ctx->refcnt, lima_ctx_do_release);
	else
		ret = -EINVAL;
	mutex_unlock(&mgr->lock);
	return ret;
}

/*
 * lima_ctx_get
 *
 * purpose:  Look up a context by handle ID and increment its refcount.
 *           Returns NULL if the ID is not present in the manager's table.
 *           The caller must eventually call lima_ctx_put() to release the
 *           reference obtained here.
 * input:    mgr — the per-file lima_ctx_mgr that owns the XArray handle table
 *           id  — handle index to look up
 * output:   pointer to the lima_ctx with an elevated refcount on success
 *           NULL if the ID is not found
 * sideEffects:
 *           acquires and releases mgr->lock (LinuxKPI mutex)
 *           increments ctx->refcnt via kref_get if the context is found
 */
struct lima_ctx *lima_ctx_get(struct lima_ctx_mgr *mgr, u32 id)
{
	struct lima_ctx *ctx;

	mutex_lock(&mgr->lock);
	ctx = xa_load(&mgr->handles, id);
	if (ctx)
		kref_get(&ctx->refcnt);
	mutex_unlock(&mgr->lock);
	return ctx;
}

/*
 * lima_ctx_put
 *
 * purpose:  Release a reference to a Lima context obtained via lima_ctx_get
 *           or lima_ctx_create.  When the refcount reaches zero the context
 *           is torn down and freed by lima_ctx_do_release.
 * input:    ctx — non-NULL pointer to a lima_ctx with a held reference
 * output:   void
 * sideEffects:
 *           decrements ctx->refcnt; if it reaches zero, calls
 *           lima_ctx_do_release which frees all scheduler contexts and the
 *           lima_ctx allocation itself
 */
void lima_ctx_put(struct lima_ctx *ctx)
{
	kref_put(&ctx->refcnt, lima_ctx_do_release);
}

/*
 * lima_ctx_mgr_init
 *
 * purpose:  Initialise a Lima context manager: set up the protecting mutex and
 *           prepare the XArray handle table for allocation (XA_FLAGS_ALLOC
 *           enables xa_alloc() auto-ID assignment starting from index 0).
 * input:    mgr — uninitialised lima_ctx_mgr (typically embedded in
 *                 struct lima_drm_priv, zeroed at file-open time)
 * output:   void
 * sideEffects:
 *           initialises mgr->lock via mutex_init (LinuxKPI sx(9) wrapper)
 *           initialises mgr->handles via xa_init_flags with XA_FLAGS_ALLOC
 */
void lima_ctx_mgr_init(struct lima_ctx_mgr *mgr)
{
	mutex_init(&mgr->lock);
	xa_init_flags(&mgr->handles, XA_FLAGS_ALLOC);
}

/*
 * lima_ctx_mgr_fini
 *
 * purpose:  Tear down a Lima context manager at DRM file-close time.
 *           Iterates over any remaining handle-table entries (leaked contexts
 *           not freed by userspace) and drops their references, then destroys
 *           the XArray and the protecting mutex.
 * input:    mgr — fully initialised lima_ctx_mgr to be destroyed
 * output:   void
 * sideEffects:
 *           calls kref_put / lima_ctx_do_release for each surviving context
 *           destroys mgr->handles XArray (xa_destroy)
 *           destroys mgr->lock mutex (mutex_destroy)
 *
 * FreeBSD/linuxkpi: xa_for_each, xa_destroy, and mutex_destroy are all
 * provided by the LinuxKPI XArray and mutex shim layers in drm-66-kmod.
 * The unsigned long iteration index is cast-compatible with the 32-bit IDs
 * stored via xa_alloc with xa_limit_32b.
 */
void lima_ctx_mgr_fini(struct lima_ctx_mgr *mgr)
{
	struct lima_ctx *ctx;
	unsigned long id;

	xa_for_each(&mgr->handles, id, ctx) {
		kref_put(&ctx->refcnt, lima_ctx_do_release);
	}

	xa_destroy(&mgr->handles);
	mutex_destroy(&mgr->lock);
}
