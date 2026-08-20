// MODULE: hal/lima/lima_sched.h
// PURPOSE: GPU job scheduler types and pipe interface for Lima Mali-400 under FreeBSD 15.1 drm-66-kmod
// PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_sched.h

/*
 * SPDX-License-Identifier: BSD-2-Clause
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
 *
 * Original copyright:
 *   Copyright 2017-2019 Qiang Yu <yuq825@gmail.com>
 */

#ifndef __LIMA_SCHED_H__
#define __LIMA_SCHED_H__

/*
 * FreeBSD 15.1 drm-66-kmod: all headers below are provided by the LinuxKPI
 * compatibility layer shipped with drm-66-kmod.  No Linux kernel source tree
 * is required; include paths are resolved via:
 *   -I${SYSDIR}/contrib/drm-66kmod/include
 *   -I${SYSDIR}/contrib/drm-66kmod/include/drm
 */
#include <drm/gpu_scheduler.h>    /* drm_gpu_scheduler, drm_sched_entity, drm_sched_job */
#include <linux/list.h>           /* struct list_head — LinuxKPI passthrough */
#include <linux/spinlock.h>       /* spinlock_t, spin_lock_irqsave */
#include <linux/atomic.h>         /* atomic_t */
#include <linux/slab.h>           /* struct kmem_cache */
#include <linux/workqueue.h>      /* struct work_struct */
#include <linux/dma-fence.h>      /* struct dma_fence */
#include <linux/types.h>          /* u32, u64 */

/*
 * NOTE: <linux/xarray.h> is intentionally omitted.  It was present in the
 * upstream Lima header but no field in this translation unit references an
 * xarray.  The include is dead weight here; lima_vm.c carries it independently
 * where the xa_alloc / xa_erase calls actually live.
 */

struct lima_device;
struct lima_vm;

/*
 * purpose:     Carry a failed task's register-capture payload for error recovery
 * input:       Populated by lima_sched_pipe when task_error fires
 * output:      Linked into lima_sched_pipe's error list; freed after recovery
 * sideEffects: Allocated from kmalloc; caller must kfree data
 */
struct lima_sched_error_task {
	struct list_head  list;
	void             *data;
	u32               size;
};

/*
 * purpose:     Single GPU work item: holds VM reference, frame descriptor,
 *              BO list, heap BO for growable jobs, and the completion fence
 * input:       Initialised via lima_sched_task_init(); submitted to a context
 * output:      fence is signalled on GPU completion or error
 * sideEffects: Increments lima_vm refcount; decrements on lima_sched_task_fini
 */
struct lima_sched_task {
	struct drm_sched_job  base;       /* drm-66-kmod scheduler job base */

	struct lima_vm       *vm;
	void                 *frame;

	struct lima_bo      **bos;
	int                   num_bos;

	bool                  recoverable;
	struct lima_bo       *heap;       /* growable heap BO for fragment shader */

	/* pipe fence — signalled when this task leaves the hardware pipeline */
	struct dma_fence     *fence;
};

/*
 * purpose:     Per-client submission queue; wraps drm_sched_entity
 * input:       One per file_priv / userspace context
 * output:      Tasks queued here are dispatched FIFO to the owning pipe
 * sideEffects: drm_sched_entity_init increments scheduler runqueue depth
 */
struct lima_sched_context {
	struct drm_sched_entity  base;
};

/* Pipe capacity constants — identical to Linux upstream */
#define LIMA_SCHED_PIPE_MAX_MMU        8
#define LIMA_SCHED_PIPE_MAX_L2_CACHE   2
#define LIMA_SCHED_PIPE_MAX_PROCESSOR  8

struct lima_ip;   /* IP block (GP, PP, MMU, L2$) — defined in lima_ip.h */

/*
 * purpose:     Represents one hardware pipeline (GP or PP array) — owns the
 *              drm_gpu_scheduler, fence context, IP block arrays, and the
 *              per-pipe operation vtable
 * input:       Initialised by lima_sched_pipe_init; op pointers set by
 *              lima_gp_pipe_init / lima_pp_pipe_init
 * output:      Drives task dispatch, MMU flush, L2 invalidation, and error recovery
 * sideEffects: Allocates task_slab (kmem_cache); spawns kthread via drm_sched_init
 */
struct lima_sched_pipe {
	struct drm_gpu_scheduler  base;   /* drm-66-kmod scheduler — unchanged */

	u64          fence_context;
	u32          fence_seqno;
	spinlock_t   fence_lock;          /* LinuxKPI spinlock_t */

	struct lima_device      *ldev;

	struct lima_sched_task  *current_task;
	struct lima_vm          *current_vm;

	/* MMU instances attached to this pipe (GP has 1, PP has up to 4) */
	struct lima_ip  *mmu[LIMA_SCHED_PIPE_MAX_MMU];
	int              num_mmu;

	/* L2 cache instances (shared between GP and PP on Mali-400 MP2) */
	struct lima_ip  *l2_cache[LIMA_SCHED_PIPE_MAX_L2_CACHE];
	int              num_l2_cache;

	/* Pixel-processor or geometry-processor instances */
	struct lima_ip  *processor[LIMA_SCHED_PIPE_MAX_PROCESSOR];
	int              num_processor;

	/* Broadcast targets used for multi-PP commands */
	struct lima_ip  *bcast_processor;
	struct lima_ip  *bcast_mmu;

	u32             done;    /* bitmask: which PPs reported IRQ_DONE */
	bool            error;   /* set by task_mmu_error; cleared on recovery */
	atomic_t        task;    /* in-flight task counter — LinuxKPI atomic_t */

	int                     frame_size;
	struct kmem_cache      *task_slab;  /* LinuxKPI kmem_cache → FreeBSD UMA zone */

	/*
	 * Per-pipe operation vtable — function pointers set at pipe init time.
	 *
	 * task_validate  — check user-supplied frame before queueing (return 0 or -errno)
	 * task_run       — program hardware registers and kick the job
	 * task_fini      — called on normal completion; read back results if needed
	 * task_error     — called on hardware fault; capture error state
	 * task_mmu_error — called on MMU page-fault interrupt
	 * task_recover   — attempt soft recovery (TLB flush + MMU reset); return 0 or -errno
	 */
	int  (*task_validate)  (struct lima_sched_pipe *pipe, struct lima_sched_task *task);
	void (*task_run)       (struct lima_sched_pipe *pipe, struct lima_sched_task *task);
	void (*task_fini)      (struct lima_sched_pipe *pipe);
	void (*task_error)     (struct lima_sched_pipe *pipe);
	void (*task_mmu_error) (struct lima_sched_pipe *pipe);
	int  (*task_recover)   (struct lima_sched_pipe *pipe);

	struct work_struct  recover_work;  /* deferred recovery via LinuxKPI workqueue */
};

/* ── Task lifetime ─────────────────────────────────────────────────────── */

/*
 * purpose:     Initialise a lima_sched_task and acquire BO / VM references
 * input:       task        — uninitialised task struct (from task_slab)
 *              context     — submission entity that will own this task
 *              bos/num_bos — buffer objects the GPU will access
 *              vm          — page-table set to activate on the pipe MMU
 * output:      Returns 0 on success; negative errno on failure
 * sideEffects: Calls drm_sched_job_init; increments BO refcounts
 */
int  lima_sched_task_init(struct lima_sched_task    *task,
                          struct lima_sched_context *context,
                          struct lima_bo           **bos,
                          int                        num_bos,
                          struct lima_vm            *vm);

/*
 * purpose:     Release all resources held by a task; called on normal or error path
 * input:       task — fully initialised task (init succeeded)
 * output:      void
 * sideEffects: Calls drm_sched_job_cleanup; decrements BO refcounts
 */
void lima_sched_task_fini(struct lima_sched_task *task);

/* ── Context lifecycle ─────────────────────────────────────────────────── */

/*
 * purpose:     Initialise a submission context (drm_sched_entity) on the given pipe
 * input:       pipe    — target hardware pipeline
 *              context — uninitialised context struct
 *              guilty  — per-context preemption fault counter (atomic_t)
 * output:      Returns 0 on success; negative errno on failure
 * sideEffects: Calls drm_sched_entity_init; context is runnable after this returns
 */
int  lima_sched_context_init(struct lima_sched_pipe    *pipe,
                             struct lima_sched_context *context,
                             atomic_t                  *guilty);

/*
 * purpose:     Drain and destroy a submission context
 * input:       pipe, context — must have been successfully init'd
 * output:      void
 * sideEffects: Calls drm_sched_entity_destroy; blocks until in-flight jobs drain
 */
void lima_sched_context_fini(struct lima_sched_pipe    *pipe,
                             struct lima_sched_context *context);

/*
 * purpose:     Submit a ready task to the scheduler and return its completion fence
 * input:       task — fully initialised; bos pinned; frame data written
 * output:      dma_fence* the caller can use to wait for GPU completion; or ERR_PTR
 * sideEffects: Calls drm_sched_entity_push_job; fence refcount is 1 on return
 */
struct dma_fence *lima_sched_context_queue_task(struct lima_sched_task *task);

/* ── Pipe lifecycle ────────────────────────────────────────────────────── */

/*
 * purpose:     Initialise a hardware pipe and its drm_gpu_scheduler
 * input:       pipe — struct with IP arrays and vtable pre-populated by caller
 *              name — human-readable name for scheduler thread (e.g. "lima-gp")
 * output:      Returns 0 on success; negative errno on failure
 * sideEffects: Calls drm_sched_init; spawns scheduler kthread; creates task_slab
 */
int  lima_sched_pipe_init(struct lima_sched_pipe *pipe, const char *name);

/*
 * purpose:     Tear down the pipe scheduler; inverse of lima_sched_pipe_init
 * input:       pipe — successfully initialised
 * output:      void
 * sideEffects: Calls drm_sched_fini; destroys task_slab; cancels recover_work
 */
void lima_sched_pipe_fini(struct lima_sched_pipe *pipe);

/*
 * purpose:     Signal task completion to the scheduler (called from IRQ handler)
 * input:       pipe — pipe whose current_task just finished
 * output:      void
 * sideEffects: Signals dma_fence; wakes drm_gpu_scheduler dispatch loop
 */
void lima_sched_pipe_task_done(struct lima_sched_pipe *pipe);

/* ── Inline helpers ────────────────────────────────────────────────────── */

/*
 * purpose:     Set pipe error flag and invoke the MMU fault handler atomically
 * input:       pipe — pipe that triggered an MMU page-fault interrupt
 * output:      void
 * sideEffects: Sets pipe->error = true; calls pipe->task_mmu_error(pipe)
 *              Must be called from IRQ context; task_mmu_error must be IRQ-safe
 */
static inline void
lima_sched_pipe_mmu_error(struct lima_sched_pipe *pipe)
{
	pipe->error = true;
	pipe->task_mmu_error(pipe);
}

/* ── Module-level slab management ──────────────────────────────────────── */

/*
 * purpose:     Create the global lima_sched_task kmem_cache (UMA zone on FreeBSD)
 * input:       none
 * output:      Returns 0 on success; -ENOMEM if zone creation fails
 * sideEffects: Allocates kernel memory; must pair with lima_sched_slab_fini
 */
int  lima_sched_slab_init(void);

/*
 * purpose:     Destroy the global lima_sched_task kmem_cache
 * input:       none
 * output:      void
 * sideEffects: Frees UMA zone; must be called after all pipes are torn down
 */
void lima_sched_slab_fini(void);

#endif /* __LIMA_SCHED_H__ */
