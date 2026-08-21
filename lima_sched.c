// SPDX-License-Identifier: GPL-2.0 OR MIT
// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright 2017-2019 Qiang Yu <yuq825@gmail.com>
 * Copyright 2024 bsdOS Project (FreeBSD port)
 *
 * MODULE: hal/lima/lima_sched.c
 * PURPOSE: GPU job scheduler for Mali-400: fence management, job submission, timeout recovery
 * PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_sched.c
 *
 * // sema:start
 * // purpose:    Implements the DRM GPU scheduler back-end for the Lima (Mali-400)
 * //             driver: per-pipe fence creation/release via slab, task init/fini,
 * //             scheduler context lifecycle, job run/timeout/free callbacks,
 * //             power-management busy/idle accounting, error-state capture, and
 * //             pipe bring-up/tear-down.
 * // input:      lima_sched_pipe (hardware pipe descriptor), lima_sched_task (GPU job),
 * //             lima_sched_context (per-fd entity), lima_device (device state)
 * // output:     dma_fence* returned to callers on submission; error task list updated
 * //             on GPU timeout; PM reference counts maintained
 * // sideEffects: allocates/frees kmem_cache entries; signals dma_fences; schedules
 * //             recover_work; calls drm_sched_fault on unrecoverable errors;
 * //             flushes L2 cache and MMU TLB; transitions PM runtime state
 * // sema:end
 *
 * Porting notes (FreeBSD 15.1 + drm-66-kmod LinuxKPI):
 *   - All drm_sched_*, dma_fence_*, kmem_cache_* APIs are provided by LinuxKPI
 *     (sys/contrib/drm-kmod) without change — no shims required here.
 *   - kvmalloc/kvfree are LinuxKPI-provided; fall back to kmalloc if needed.
 *   - iosys_map / drm_gem_vmap_unlocked provided by drm-66-kmod drm_gem.h.
 *   - pm_runtime_* provided by LinuxKPI power/runtime.h.
 *   - vmap/vunmap: LinuxKPI provides these for contiguous vmalloc mappings;
 *     for heap BOs on FreeBSD the pages array comes from drm_gem_dma pages.
 *   - pgprot_writecombine: LinuxKPI asm-generic/pgtable-nopmd.h equivalent used.
 *   - INIT_WORK / schedule_work / work_struct: LinuxKPI linux/workqueue.h.
 *   - RCU (call_rcu): LinuxKPI linux/rcupdate.h.
 *   - spin_lock_init / atomic_t: LinuxKPI linux/spinlock.h / linux/atomic.h.
 *   - list_add / LIST_HEAD: LinuxKPI linux/list.h.
 *   - container_of: LinuxKPI linux/kernel.h.
 */

#include <linux/delay.h>
#include <linux/iosys-map.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/pm_runtime.h>

#include "lima_devfreq.h"
#include "lima_drv.h"
#include "lima_sched.h"
#include "lima_vm.h"
#include "lima_mmu.h"
#include "lima_l2_cache.h"
#include "lima_gem.h"
#include "lima_trace.h"
#include "lima_freebsd_compat.h"
#include <sys/sysctl.h>

/* --------------------------------------------------------------------------
 * Lima fence — thin wrapper around dma_fence allocated from a slab cache.
 * --------------------------------------------------------------------------
 *
 * sema:start
 * purpose:   Represent a single GPU job completion event as a DRM sync fence.
 * input:     lima_sched_pipe* stored in fence so get_timeline_name can return
 *            the scheduler's name without an extra lookup.
 * output:    struct lima_fence embedded dma_fence base usable by DRM core.
 * sideEffects: slab allocation on create; RCU-deferred slab free on release.
 * sema:end
 */

struct lima_fence {
	struct dma_fence base;
	struct lima_sched_pipe *pipe;
};

static struct kmem_cache *lima_fence_slab;
static int lima_fence_slab_refcnt;

/*
 * lima_sched_slab_init — create (or reuse) the lima_fence slab cache.
 *
 * sema:start
 * purpose:  Lazily create a SLAB_HWCACHE_ALIGN slab for struct lima_fence.
 *           Reference-counted so multiple pipes share one cache.
 * input:    none
 * output:   0 on success, -ENOMEM if kmem_cache_create fails
 * sideEffects: may allocate a new kmem_cache; increments lima_fence_slab_refcnt
 * sema:end
 */
int lima_sched_slab_init(void)
{
	if (!lima_fence_slab) {
		lima_fence_slab = kmem_cache_create(
			"lima_fence", sizeof(struct lima_fence), 0,
			SLAB_HWCACHE_ALIGN, NULL);
		if (!lima_fence_slab)
			return -ENOMEM;
	}

	lima_fence_slab_refcnt++;
	return 0;
}

/*
 * lima_sched_slab_fini — release the lima_fence slab cache when last user gone.
 *
 * sema:start
 * purpose:  Decrement refcnt; destroy slab when it reaches zero.
 * input:    none
 * output:   none
 * sideEffects: may call kmem_cache_destroy and set lima_fence_slab = NULL
 * sema:end
 */
void lima_sched_slab_fini(void)
{
	if (!--lima_fence_slab_refcnt) {
		kmem_cache_destroy(lima_fence_slab);
		lima_fence_slab = NULL;
	}
}

/* --------------------------------------------------------------------------
 * dma_fence ops
 * -------------------------------------------------------------------------- */

static inline struct lima_fence *to_lima_fence(struct dma_fence *fence)
{
	return container_of(fence, struct lima_fence, base);
}

/*
 * lima_fence_get_driver_name — return the driver name string for this fence.
 *
 * sema:start
 * purpose:  Satisfy dma_fence_ops.get_driver_name; used in debug output.
 * input:    fence — dma_fence pointer (ignored, name is constant)
 * output:   "lima"
 * sideEffects: none
 * sema:end
 */
static const char *lima_fence_get_driver_name(struct dma_fence *fence)
{
	return "lima";
}

/*
 * lima_fence_get_timeline_name — return the scheduler pipe name for this fence.
 *
 * sema:start
 * purpose:  Satisfy dma_fence_ops.get_timeline_name for per-timeline debug info.
 * input:    fence — dma_fence* cast to lima_fence to retrieve pipe name
 * output:   pipe->base.name string (set during drm_sched_init)
 * sideEffects: none
 * sema:end
 */
static const char *lima_fence_get_timeline_name(struct dma_fence *fence)
{
	struct lima_fence *f = to_lima_fence(fence);

	return f->pipe->base.name;
}

/*
 * lima_fence_release_rcu — RCU callback to free the fence back to the slab.
 *
 * sema:start
 * purpose:  Called from RCU grace period after all readers are done.
 *           Frees the lima_fence allocation safely after dma_fence refcount hits 0.
 * input:    rcu — embedded rcu_head inside struct dma_fence
 * output:   none
 * sideEffects: kmem_cache_free on lima_fence_slab
 * sema:end
 */
static void lima_fence_release_rcu(struct rcu_head *rcu)
{
	struct dma_fence *f = container_of(rcu, struct dma_fence, rcu);
	struct lima_fence *fence = to_lima_fence(f);

	kmem_cache_free(lima_fence_slab, fence);
}

/*
 * lima_fence_release — dma_fence_ops.release: schedule RCU-deferred free.
 *
 * sema:start
 * purpose:  Called when dma_fence refcount drops to zero; defers actual free
 *           to RCU callback to avoid use-after-free in signalling paths.
 * input:    fence — dma_fence being released
 * output:   none
 * sideEffects: queues lima_fence_release_rcu via call_rcu
 * sema:end
 */
static void lima_fence_release(struct dma_fence *fence)
{
	struct lima_fence *f = to_lima_fence(fence);

	call_rcu(&f->base.rcu, lima_fence_release_rcu);
}

static const struct dma_fence_ops lima_fence_ops = {
	.get_driver_name   = lima_fence_get_driver_name,
	.get_timeline_name = lima_fence_get_timeline_name,
	.release           = lima_fence_release,
};

/*
 * lima_fence_create — allocate and initialise a new lima_fence for a pipe.
 *
 * sema:start
 * purpose:  Allocate a lima_fence from the slab, associate it with pipe, and
 *           initialise the embedded dma_fence with the pipe's lock/context/seqno.
 * input:    pipe — scheduler pipe that will signal this fence on job completion
 * output:   pointer to initialised lima_fence, or NULL on allocation failure
 * sideEffects: kmem_cache_zalloc; increments pipe->fence_seqno
 * sema:end
 */
static struct lima_fence *lima_fence_create(struct lima_sched_pipe *pipe)
{
	struct lima_fence *fence;

	fence = kmem_cache_zalloc(lima_fence_slab, GFP_KERNEL);
	if (!fence)
		return NULL;

	fence->pipe = pipe;
	dma_fence_init(&fence->base, &lima_fence_ops, &pipe->fence_lock,
		       pipe->fence_context, ++pipe->fence_seqno);

	return fence;
}

/* --------------------------------------------------------------------------
 * Container-of helpers
 * -------------------------------------------------------------------------- */

static inline struct lima_sched_task *to_lima_task(struct drm_sched_job *job)
{
	return container_of(job, struct lima_sched_task, base);
}

static inline struct lima_sched_pipe *to_lima_pipe(struct drm_gpu_scheduler *sched)
{
	return container_of(sched, struct lima_sched_pipe, base);
}

/* --------------------------------------------------------------------------
 * Task lifecycle
 * -------------------------------------------------------------------------- */

/*
 * lima_sched_task_init — initialise a scheduler task (GPU job).
 *
 * sema:start
 * purpose:  Deep-copy the BO array, take GEM object references, initialise the
 *           drm_sched_job, arm it for submission, and retain the VM.
 * input:    task    — pre-allocated task struct (from pipe->task_slab)
 *           context — scheduler entity the job belongs to
 *           bos     — array of lima_bo* referenced by this job
 *           num_bos — length of bos[]
 *           vm      — address space for this job
 * output:   0 on success; -ENOMEM or drm_sched_job_init error on failure.
 *           On failure the task struct is left in a state safe for kfree.
 * sideEffects: kmemdup for bos; drm_gem_object_get per BO; drm_sched_job_init;
 *             drm_sched_job_arm; lima_vm_get
 * sema:end
 */
int lima_sched_task_init(struct lima_sched_task *task,
			 struct lima_sched_context *context,
			 struct lima_bo **bos, int num_bos,
			 struct lima_vm *vm)
{
	int err, i;

	task->bos = kmemdup(bos, sizeof(*bos) * num_bos, GFP_KERNEL);
	if (!task->bos)
		return -ENOMEM;

	for (i = 0; i < num_bos; i++)
		drm_gem_object_get(&bos[i]->base.base);

	err = drm_sched_job_init(&task->base, &context->base, vm);
	if (err) {
		kfree(task->bos);
		return err;
	}

	drm_sched_job_arm(&task->base);

	task->num_bos = num_bos;
	task->vm = lima_vm_get(vm);

	return 0;
}

/*
 * lima_sched_task_fini — tear down a scheduler task and release resources.
 *
 * sema:start
 * purpose:  Reverse of lima_sched_task_init: cleanup drm_sched_job, drop GEM
 *           object refs, free BO array, release VM.
 * input:    task — task to finalise (must have been initialised successfully)
 * output:   none
 * sideEffects: drm_sched_job_cleanup; drm_gem_object_put per BO; kfree(bos);
 *             lima_vm_put
 * sema:end
 */
void lima_sched_task_fini(struct lima_sched_task *task)
{
	int i;

	drm_sched_job_cleanup(&task->base);

	if (task->bos) {
		for (i = 0; i < task->num_bos; i++)
			drm_gem_object_put(&task->bos[i]->base.base);
		kfree(task->bos);
	}

	lima_vm_put(task->vm);
}

/* --------------------------------------------------------------------------
 * Scheduler context (per-fd entity) lifecycle
 * -------------------------------------------------------------------------- */

/*
 * lima_sched_context_init — initialise a DRM scheduler entity for one file.
 *
 * sema:start
 * purpose:  Wrap drm_sched_entity_init for a single pipe at NORMAL priority.
 * input:    pipe    — hardware pipe this context submits to
 *           context — context struct to initialise
 *           guilty  — atomic flag set when job is declared guilty (for throttling)
 * output:   0 on success, negative errno on failure
 * sideEffects: drm_sched_entity_init
 * sema:end
 */
int lima_sched_context_init(struct lima_sched_pipe *pipe,
			    struct lima_sched_context *context,
			    atomic_t *guilty)
{
	struct drm_gpu_scheduler *sched = &pipe->base;

	return drm_sched_entity_init(&context->base, DRM_SCHED_PRIORITY_NORMAL,
				     &sched, 1, guilty);
}

/*
 * lima_sched_context_fini — destroy a scheduler entity.
 *
 * sema:start
 * purpose:  Drain pending jobs and destroy the drm_sched_entity.
 * input:    pipe    — hardware pipe (unused here, kept for API symmetry)
 *           context — context to destroy
 * output:   none
 * sideEffects: drm_sched_entity_destroy (may sleep waiting for pending jobs)
 * sema:end
 */
void lima_sched_context_fini(struct lima_sched_pipe *pipe,
			     struct lima_sched_context *context)
{
	drm_sched_entity_destroy(&context->base);
}

/*
 * lima_sched_context_queue_task — push a task onto the scheduler entity queue.
 *
 * sema:start
 * purpose:  Obtain a reference to the job's finished fence, submit the job to
 *           the DRM scheduler queue, and return the fence to the caller.
 * input:    task — armed task (lima_sched_task_init + drm_sched_job_arm called)
 * output:   dma_fence* for the job's completion (caller owns the reference)
 * sideEffects: dma_fence_get on task->base.s_fence->finished;
 *             drm_sched_entity_push_job; emits lima_task_submit trace event
 * sema:end
 */
struct dma_fence *lima_sched_context_queue_task(struct lima_sched_task *task)
{
	struct dma_fence *fence = dma_fence_get(&task->base.s_fence->finished);

	trace_lima_task_submit(task);
	drm_sched_entity_push_job(&task->base);
	return fence;
}

/* --------------------------------------------------------------------------
 * Power management helpers
 * -------------------------------------------------------------------------- */

/*
 * lima_pm_busy — resume GPU via runtime PM and record devfreq busy state.
 *
 * sema:start
 * purpose:  Called before dispatching a job to ensure the GPU is powered on and
 *           to account for active utilisation in the devfreq governor.
 * input:    ldev — lima_device whose underlying struct device drives PM
 * output:   0 on success; negative errno if pm_runtime_resume_and_get fails
 * sideEffects: increments PM usage counter; marks devfreq busy
 * sema:end
 *
 * FreeBSD note: pm_runtime_resume_and_get maps to LinuxKPI pm_runtime.h which
 * calls the driver's runtime_resume callback (lima_device_runtime_resume).
 */
static int lima_pm_busy(struct lima_device *ldev)
{
	int ret;

	/* Resume GPU if it has been suspended by runtime PM. */
	ret = pm_runtime_resume_and_get(ldev->dev);
	if (ret < 0)
		return ret;

	lima_devfreq_record_busy(&ldev->devfreq);
	return 0;
}

/*
 * lima_pm_idle — record devfreq idle and allow GPU auto-suspend.
 *
 * sema:start
 * purpose:  Called after a job completes (or times out) to mark the GPU as idle
 *           for devfreq accounting and enable runtime auto-suspend.
 * input:    ldev — lima_device
 * output:   none
 * sideEffects: marks devfreq idle; calls pm_runtime_mark_last_busy +
 *             pm_runtime_put_autosuspend
 * sema:end
 */
static void lima_pm_idle(struct lima_device *ldev)
{
	lima_devfreq_record_idle(&ldev->devfreq);

	/* GPU can now auto-suspend after the configured delay. */
	pm_runtime_mark_last_busy(ldev->dev);
	pm_runtime_put_autosuspend(ldev->dev);
}

/* --------------------------------------------------------------------------
 * DRM scheduler back-end operations
 * -------------------------------------------------------------------------- */

/*
 * lima_sched_run_job — drm_sched_backend_ops.run_job: dispatch one GPU job.
 *
 * sema:start
 * purpose:  Called by the DRM scheduler worker to start a job on hardware.
 *           Creates the completion fence, resumes PM, flushes L2 cache,
 *           switches the MMU to the job's address space, and calls the
 *           pipe-specific task_run callback (GP or PP).
 * input:    job — drm_sched_job to run (contains the task embedded struct)
 * output:   dma_fence* for job completion that the scheduler will signal,
 *           or NULL on error (scheduler will retry / fault)
 * sideEffects: lima_fence_create; lima_pm_busy; L2 cache flush; MMU switch;
 *             updates pipe->current_task / pipe->current_vm / pipe->error;
 *             emits lima_task_run trace event; calls pipe->task_run
 * sema:end
 *
 * Note: dma_fence_get(task->fence) is called before task_run so that the
 * caller's reference is established before the IRQ completion handler can
 * signal and potentially free the fence.
 */
static struct dma_fence *lima_sched_run_job(struct drm_sched_job *job)
{
	struct lima_sched_task *task = to_lima_task(job);
	struct lima_sched_pipe *pipe = to_lima_pipe(job->sched);
	struct lima_device *ldev = pipe->ldev;
	struct lima_fence *fence;
	int i, err;

	/* After GPU reset the finished fence carries a negative error code;
	 * skip hardware dispatch and let the scheduler handle resubmission. */
	if (job->s_fence->finished.error < 0)
		return NULL;

	fence = lima_fence_create(pipe);
	if (!fence)
		return NULL;

	err = lima_pm_busy(ldev);
	if (err < 0) {
		dma_fence_put(&fence->base);
		return NULL;
	}

	task->fence = &fence->base;

	/* Grab an extra reference for the caller; the IRQ handler may signal
	 * (and release) the fence before we return it upwards. */
	dma_fence_get(task->fence);

	pipe->current_task = task;

	/*
	 * Flush L2 cache before switching the MMU page tables.
	 * This is needed for the MMU to work correctly on Mali-400: without
	 * the flush, GP/PP may hang or page-fault after running for a while.
	 *
	 * TODO (upstream tracking):
	 *   1. Is this a TLB coherency issue?
	 *   2. Performance impact measurement (all GP/PP share one L2).
	 *   3. Can we defer to task_fini to overlap with next job setup?
	 *   4. When GP/PP use separate L2 caches, PP must wait for GP flush.
	 */
	for (i = 0; i < pipe->num_l2_cache; i++)
		lima_l2_cache_flush(pipe->l2_cache[i]);

	lima_vm_put(pipe->current_vm);
	pipe->current_vm = lima_vm_get(task->vm);

	if (pipe->bcast_mmu)
		lima_mmu_switch_vm(pipe->bcast_mmu, pipe->current_vm);
	else {
		for (i = 0; i < pipe->num_mmu; i++)
			lima_mmu_switch_vm(pipe->mmu[i], pipe->current_vm);
	}

	trace_lima_task_run(task);

	pipe->error = false;
	pipe->task_run(pipe, task);

	return task->fence;
}

/* --------------------------------------------------------------------------
 * Error task capture
 * -------------------------------------------------------------------------- */

/*
 * lima_sched_build_error_task_list — snapshot GPU job state for post-mortem.
 *
 * sema:start
 * purpose:  On job timeout, capture the frame data, process name/PID, and all
 *           GPU buffer object contents into a lima_sched_error_task entry and
 *           append it to dev->error_task_list for userspace retrieval.
 * input:    task — timed-out task whose state should be captured
 * output:   none (best-effort; logs and returns on allocation failure)
 * sideEffects: mutex_lock/unlock on dev->error_task_list_lock;
 *             kvmalloc for error task buffer; vmap/vunmap for heap BOs;
 *             drm_gem_vmap_unlocked/vunmap_unlocked for regular BOs;
 *             list_add to dev->error_task_list; updates dev->dump counters
 * sema:end
 *
 * FreeBSD porting note:
 *   vmap() — LinuxKPI provides this for PAGE_SIZE-aligned physical page arrays.
 *   pgprot_writecombine() — LinuxKPI asm/pgtable.h maps to VM_MEMATTR_WRITE_COMBINING
 *   on aarch64 FreeBSD.
 *   drm_gem_vmap_unlocked / drm_gem_vunmap_unlocked — drm-66-kmod drm_gem.h.
 *   iosys_map — drm-66-kmod linux/iosys-map.h.
 */
static void lima_sched_build_error_task_list(struct lima_sched_task *task)
{
	struct lima_sched_error_task *et;
	struct lima_sched_pipe *pipe = to_lima_pipe(task->base.sched);
	struct lima_ip *ip = pipe->processor[0];
	int pipe_id = ip->id == lima_ip_gp ? lima_pipe_gp : lima_pipe_pp;
	struct lima_device *dev = ip->dev;
	struct lima_sched_context *sched_ctx =
		container_of(task->base.entity,
			     struct lima_sched_context, base);
	struct lima_ctx *ctx =
		container_of(sched_ctx, struct lima_ctx, context[pipe_id]);
	struct lima_dump_task *dt;
	struct lima_dump_chunk *chunk;
	struct lima_dump_chunk_pid *pid_chunk;
	struct lima_dump_chunk_buffer *buffer_chunk;
	u32 size, task_size, mem_size;
	int i;
	struct iosys_map map;
	int ret;

	mutex_lock(&dev->error_task_list_lock);

	if (dev->dump.num_tasks >= lima_max_error_tasks) {
		dev_info(dev->dev, "fail to save task state from %s pid %d: "
			 "error task list is full\n", ctx->pname, ctx->pid);
		goto out;
	}

	/* Compute total serialised size. */

	/* frame chunk */
	size = sizeof(struct lima_dump_chunk) + pipe->frame_size;
	/* process name chunk */
	size += sizeof(struct lima_dump_chunk) + sizeof(ctx->pname);
	/* pid chunk */
	size += sizeof(struct lima_dump_chunk);
	/* buffer chunks — one per BO */
	for (i = 0; i < task->num_bos; i++) {
		struct lima_bo *bo = task->bos[i];

		size += sizeof(struct lima_dump_chunk);
		size += bo->heap_size ? bo->heap_size : lima_bo_size(bo);
	}

	task_size = size + sizeof(struct lima_dump_task);
	mem_size  = task_size + sizeof(*et);
	et = kvmalloc(mem_size, GFP_KERNEL);
	if (!et) {
		dev_err(dev->dev,
			"fail to alloc task dump buffer of size %x\n", mem_size);
		goto out;
	}

	et->data = et + 1;
	et->size = task_size;

	/* Populate the dump task header. */
	dt = et->data;
	memset(dt, 0, sizeof(*dt));
	dt->id   = pipe_id;
	dt->size = size;

	/* --- Frame chunk --- */
	chunk = (struct lima_dump_chunk *)(dt + 1);
	memset(chunk, 0, sizeof(*chunk));
	chunk->id   = LIMA_DUMP_CHUNK_FRAME;
	chunk->size = pipe->frame_size;
	memcpy(chunk + 1, task->frame, pipe->frame_size);
	dt->num_chunks++;

	/* --- Process name chunk --- */
	chunk = (void *)(chunk + 1) + chunk->size;
	memset(chunk, 0, sizeof(*chunk));
	chunk->id   = LIMA_DUMP_CHUNK_PROCESS_NAME;
	chunk->size = sizeof(ctx->pname);
	memcpy(chunk + 1, ctx->pname, sizeof(ctx->pname));
	dt->num_chunks++;

	/* --- PID chunk --- */
	pid_chunk = (void *)(chunk + 1) + chunk->size;
	memset(pid_chunk, 0, sizeof(*pid_chunk));
	pid_chunk->id  = LIMA_DUMP_CHUNK_PROCESS_ID;
	pid_chunk->pid = ctx->pid;
	dt->num_chunks++;

	/* --- Buffer chunks --- */
	buffer_chunk = (void *)(pid_chunk + 1) + pid_chunk->size;
	for (i = 0; i < task->num_bos; i++) {
		struct lima_bo *bo = task->bos[i];
		void *data;

		memset(buffer_chunk, 0, sizeof(*buffer_chunk));
		buffer_chunk->id = LIMA_DUMP_CHUNK_BUFFER;
		buffer_chunk->va = lima_vm_get_va(task->vm, bo);

		if (bo->heap_size) {
			/*
			 * Heap BO: pages are physically discontiguous; use
			 * vmap() to create a temporary kernel mapping.
			 *
			 * FreeBSD/LinuxKPI note: vmap() expects a struct page**
			 * array and count; pgprot_writecombine is mapped to
			 * write-combining attributes by LinuxKPI on aarch64.
			 */
			buffer_chunk->size = bo->heap_size;

			data = vmap(bo->base.pages,
				    bo->heap_size >> PAGE_SHIFT,
				    VM_MAP,
				    pgprot_writecombine(PAGE_KERNEL));
			if (!data) {
				kvfree(et);
				goto out;
			}

			memcpy(buffer_chunk + 1, data, buffer_chunk->size);

			vunmap(data);
		} else {
			/*
			 * Regular GEM DMA BO: use drm_gem_vmap_unlocked for a
			 * safe kernel mapping through the iosys_map abstraction.
			 */
			buffer_chunk->size = lima_bo_size(bo);

			ret = drm_gem_vmap_unlocked(&bo->base.base, &map);
			if (ret) {
				kvfree(et);
				goto out;
			}

			memcpy(buffer_chunk + 1, map.vaddr, buffer_chunk->size);

			drm_gem_vunmap_unlocked(&bo->base.base, &map);
		}

		buffer_chunk = (void *)(buffer_chunk + 1) + buffer_chunk->size;
		dt->num_chunks++;
	}

	list_add(&et->list, &dev->error_task_list);
	dev->dump.size      += et->size;
	dev->dump.num_tasks++;

	dev_info(dev->dev, "save error task state success\n");

out:
	mutex_unlock(&dev->error_task_list_lock);
}

/* --------------------------------------------------------------------------
 * Timeout handler
 * -------------------------------------------------------------------------- */

/*
 * lima_sched_timedout_job — drm_sched_backend_ops.timedout_job.
 *
 * sema:start
 * purpose:  Handle a GPU job that did not complete within the timeout window.
 *           Stops the scheduler, captures error state, triggers hardware error
 *           recovery, resumes the MMU from page-fault stall, and restarts
 *           the scheduler to resubmit pending jobs.
 * input:    job — drm_sched_job that timed out
 * output:   DRM_GPU_SCHED_STAT_NOMINAL (scheduler should continue)
 * sideEffects: drm_sched_stop; drm_sched_increase_karma; lima_sched_build_error_task_list
 *             (conditional); pipe->task_error; lima_mmu_page_fault_resume;
 *             lima_vm_put; lima_pm_idle; drm_sched_resubmit_jobs; drm_sched_start
 * sema:end
 */
static enum drm_gpu_sched_stat lima_sched_timedout_job(struct drm_sched_job *job)
{
	struct lima_sched_pipe *pipe = to_lima_pipe(job->sched);
	struct lima_sched_task *task = to_lima_task(job);
	struct lima_device *ldev = pipe->ldev;

	if (!pipe->error)
		DRM_ERROR("lima job timeout\n");

	drm_sched_stop(&pipe->base, &task->base);

	drm_sched_increase_karma(&task->base);

	if (lima_max_error_tasks)
		lima_sched_build_error_task_list(task);

	pipe->task_error(pipe);

	if (pipe->bcast_mmu)
		lima_mmu_page_fault_resume(pipe->bcast_mmu);
	else {
		int i;

		for (i = 0; i < pipe->num_mmu; i++)
			lima_mmu_page_fault_resume(pipe->mmu[i]);
	}

	lima_vm_put(pipe->current_vm);
	pipe->current_vm  = NULL;
	pipe->current_task = NULL;

	lima_pm_idle(ldev);

	drm_sched_resubmit_jobs(&pipe->base);
	drm_sched_start(&pipe->base, true);

	return DRM_GPU_SCHED_STAT_NOMINAL;
}

/* --------------------------------------------------------------------------
 * Job free
 * -------------------------------------------------------------------------- */

/*
 * lima_sched_free_job — drm_sched_backend_ops.free_job: release job resources.
 *
 * sema:start
 * purpose:  Called by the scheduler after a job has either completed or been
 *           cleaned up after timeout.  Drops the fence reference, removes BOs
 *           from the VM mapping, and returns the task to the pipe's slab.
 * input:    job — completed/cleaned drm_sched_job
 * output:   none
 * sideEffects: dma_fence_put on task->fence; lima_vm_bo_del per BO;
 *             lima_sched_task_fini; kmem_cache_free to pipe->task_slab
 * sema:end
 */
static void lima_sched_free_job(struct drm_sched_job *job)
{
	struct lima_sched_task *task = to_lima_task(job);
	struct lima_sched_pipe *pipe = to_lima_pipe(job->sched);
	struct lima_vm *vm = task->vm;
	struct lima_bo **bos = task->bos;
	int i;

	dma_fence_put(task->fence);

	for (i = 0; i < task->num_bos; i++)
		lima_vm_bo_del(vm, bos[i]);

	lima_sched_task_fini(task);
	kmem_cache_free(pipe->task_slab, task);
}

static const struct drm_sched_backend_ops lima_sched_ops = {
	.run_job      = lima_sched_run_job,
	.timedout_job = lima_sched_timedout_job,
	.free_job     = lima_sched_free_job,
};

/* --------------------------------------------------------------------------
 * Hardware recovery work item
 * -------------------------------------------------------------------------- */

/*
 * lima_sched_recover_work — work_struct handler: attempt hardware recovery.
 *
 * sema:start
 * purpose:  Flush L2 cache and MMU TLBs, then call the pipe-specific
 *           task_recover callback.  If recovery fails, signal a hard fault
 *           to the scheduler via drm_sched_fault.
 * input:    work — embedded work_struct in struct lima_sched_pipe
 * output:   none
 * sideEffects: lima_l2_cache_flush per cache; lima_mmu_flush_tlb per MMU;
 *             pipe->task_recover; drm_sched_fault on failure
 * sema:end
 *
 * FreeBSD note: INIT_WORK / schedule_work use LinuxKPI linux/workqueue.h which
 * maps to FreeBSD taskqueue(9) internally.
 */
static void lima_sched_recover_work(struct work_struct *work)
{
	struct lima_sched_pipe *pipe =
		container_of(work, struct lima_sched_pipe, recover_work);
	int i;

	for (i = 0; i < pipe->num_l2_cache; i++)
		lima_l2_cache_flush(pipe->l2_cache[i]);

	if (pipe->bcast_mmu) {
		lima_mmu_flush_tlb(pipe->bcast_mmu);
	} else {
		for (i = 0; i < pipe->num_mmu; i++)
			lima_mmu_flush_tlb(pipe->mmu[i]);
	}

	if (pipe->task_recover(pipe))
		drm_sched_fault(&pipe->base);
}

/* --------------------------------------------------------------------------
 * Pipe lifecycle
 * -------------------------------------------------------------------------- */

/*
 * lima_sched_pipe_init — initialise a scheduler pipe (GP or PP).
 *
 * sema:start
 * purpose:  Allocate a fence context, initialise the fence spinlock and the
 *           recovery work item, then bring up the DRM GPU scheduler for this
 *           pipe with a configurable timeout.
 * input:    pipe — pipe to initialise (ldev must already be set by caller)
 *           name — human-readable name string (e.g. "lima_gp", "lima_pp")
 * output:   0 on success, negative errno on drm_sched_init failure
 * sideEffects: dma_fence_context_alloc; spin_lock_init; INIT_WORK; drm_sched_init
 * sema:end
 *
 * Timeout: lima_sched_timeout_ms module parameter; defaults to 500 ms if not
 * set (value <= 0).
 *
 * drm_sched_init signature in drm-66-kmod (Linux 6.6 API):
 *   drm_sched_init(sched, ops, hw_submission, hang_limit,
 *                  timeout, timeout_wq, score, name, dev)
 * The NULL timeout_wq causes the scheduler to use its own internal work queue.
 */
int lima_sched_pipe_init(struct lima_sched_pipe *pipe, const char *name)
{
	unsigned int timeout = lima_sched_timeout_ms > 0 ?
			       lima_sched_timeout_ms : 500;

	pipe->fence_context = dma_fence_context_alloc(1);
	spin_lock_init(&pipe->fence_lock);

	INIT_WORK(&pipe->recover_work, lima_sched_recover_work);

	return drm_sched_init(&pipe->base, &lima_sched_ops, 1,
			      lima_job_hang_limit,
			      msecs_to_jiffies(timeout), NULL,
			      NULL, name, pipe->ldev->dev);
}

/*
 * Bound on how long lima_sched_pipe_fini() will wait for a hardware task
 * that is still in flight when teardown starts.  500ms is the default GPU
 * job timeout (lima_sched_pipe_init() above); 2s gives the scheduler's own
 * timedout_job()/resubmit path multiple chances to clear pipe->current_task
 * on its own before this code takes the wedge into its own hands.
 */
#define LIMA_SCHED_PIPE_FINI_TIMEOUT_MS  2000
#define LIMA_SCHED_PIPE_FINI_POLL_MS     10

/*
 * lima_sched_pipe_fini — tear down a scheduler pipe.
 *
 * sema:start
 * purpose:  Drain and destroy the DRM GPU scheduler for this pipe, without
 *           ever blocking the calling thread (module unload / device
 *           detach) indefinitely, even if the GPU hardware is wedged and
 *           will never again raise a completion or error interrupt for the
 *           task currently on the pipe.
 * input:    pipe — pipe to tear down
 * output:   none
 * sideEffects: may force-signal pipe->current_task->fence with -ENODEV and
 *             call pipe->task_error() if the pipe is still busy after
 *             LIMA_SCHED_PIPE_FINI_TIMEOUT_MS; calls drm_sched_stop() to
 *             reap any job whose hardware fence already signalled but that
 *             the scheduler kthread had not yet freed; calls drm_sched_fini
 *             (parks/stops the worker thread; kthread_stop can no longer
 *             block on real hardware once the steps above have run)
 * sema:end
 *
 * VERIFIED ON HARDWARE 2026-08-21. This used to say "UNTESTED ON HARDWARE
 * ... written and reviewed from source inspection only", which was true when
 * written and is not any more:
 *
 *   - The reordering half runs on every unload. `kldunload lima` after a
 *     render takes 0.06 s, prints nothing, and leaves no UMA "keg not empty"
 *     warning -- the symptom whose absence is the regression test.
 *   - The bounded-teardown half was forced deliberately, via the new
 *     sysctl compat.linuxkpi.lima_fake_wedge, because with the predicate
 *     fixed (see pipe_task_in_flight) it is correctly never taken on healthy
 *     hardware. Forced, it does exactly what it claims: bounded at
 *     LIMA_SCHED_PIPE_FINI_TIMEOUT_MS per pipe, logs the DRM_ERROR, calls
 *     task_error() on both pipes, the unload COMPLETES rather than hanging,
 *     no panic from force-signalling with -ENODEV, and a subsequent kldload
 *     attaches and renders.
 *
 * Root cause this addresses (see commit message for the full trace):
 *
 *   1. drm_sched_main() in drm-kmod-src/drivers/gpu/drm/scheduler/
 *      sched_main.c checks `while (!kthread_should_stop())` *before*
 *      re-entering its body, so drm_sched_fini()'s kthread_stop() can win a
 *      race against a just-signalled job's cleanup and leave it in
 *      sched->pending_list forever, never freed via ops->free_job(). Both
 *      lima_fini_gp_pipe() and lima_fini_pp_pipe() in lima_device.c
 *      additionally called lima_{gp,pp}_pipe_fini() — which destroys the
 *      task_slab kmem_cache — *before* lima_sched_pipe_fini() ever stopped
 *      the scheduler kthread, so a job freed by that kthread mid-race was
 *      calling kmem_cache_free() into a cache concurrently being destroyed.
 *      This is the direct cause of the "Freed UMA keg (lima_pp_task) was
 *      not empty" warning on a clean unload.  Fixed by reordering
 *      lima_fini_gp_pipe()/lima_fini_pp_pipe() (lima_device.c) to call
 *      lima_sched_pipe_fini() first, and by this function now calling
 *      drm_sched_stop(&pipe->base, NULL) — which walks pending_list and
 *      frees every job whose hardware fence already fired — before
 *      drm_sched_fini().
 *
 *   2. If a task is genuinely wedged (hardware will never again post the
 *      completion/error IRQ that lima_sched_pipe_task_done() is waiting
 *      for), pipe->current_task never clears on its own.  Nothing in the
 *      original teardown path bounded how long that could take: eventually
 *      the scheduler's own 500ms job timeout (lima_sched_timedout_job())
 *      should clear it, but that is not guaranteed (e.g. if the tdr work
 *      races module unload, or a future change disables/raises
 *      lima_job_hang_limit).  The poll loop below puts an explicit, logged
 *      ceiling on that wait and forces the pipe idle itself on timeout
 *      instead of relying on that recovery having already run.
 */
/*
 * pipe_task_in_flight — is there really a task the hardware still owes us?
 *
 * NOT the same question as `pipe->current_task != NULL`, which is what the
 * first version of the teardown below tested. Upstream lima sets current_task
 * when a job is handed to the hardware (lima_sched_run_task) and clears it only
 * in the timeout/error path -- lima_sched_pipe_task_done() signals the fence on
 * normal completion and deliberately leaves current_task pointing at the last
 * task submitted. So after any successful render, current_task stays non-NULL
 * forever.
 *
 * Testing it directly therefore made the "GPU is wedged" branch fire on EVERY
 * unload. Measured on hardware 2026-08-21: `kldunload lima` after one limabench
 * run took 4.09 s (2000 ms per pipe), printed
 *   [drm ERROR] pp: pipe still busy 2000ms into teardown
 * for both pipes, and force-reset each one -- while the hardware itself
 * reported `int_state=0 status=0`, i.e. perfectly idle with nothing in flight.
 * A false alarm that also power-cycled a healthy GPU on every unload, and it
 * masked the branch's real purpose: LOOSE-ENDS.md recorded the timeout path as
 * "has never executed" when in fact it was the only path ever taken.
 *
 * The fence is the honest test: it is signalled exactly when the hardware is
 * done with the task (or when recovery completes it), and it stays unsignalled
 * for a genuine wedge -- which is the case this teardown exists to survive.
 */
/*
 * Deterministic exerciser for the wedge branch below. With the predicate fixed,
 * that branch is (correctly) never taken on healthy hardware -- so the only way
 * to know it works is to force it, and the only honest alternative was wedging
 * a real GPU with the glReadPixels-on-imported-dma-buf case and needing a guest
 * reboot afterwards. Set it, run one job, unload: the teardown then behaves
 * exactly as it would against a Mali that never posts another interrupt.
 */
static int fake_wedge;
SYSCTL_INT(_compat_linuxkpi, OID_AUTO, lima_fake_wedge, CTLFLAG_RWTUN,
    &fake_wedge, 0,
    "pretend the pipe's task never completes, to exercise the bounded "
    "teardown/recovery path in lima_sched_pipe_fini()");

static bool pipe_task_in_flight(struct lima_sched_pipe *pipe)
{
	struct lima_sched_task *task = pipe->current_task;

	if (task == NULL || task->fence == NULL)
		return false;
	if (fake_wedge)
		return true;
	return !dma_fence_is_signaled(task->fence);
}

void lima_sched_pipe_fini(struct lima_sched_pipe *pipe)
{
	struct lima_sched_task *task;
	unsigned int waited = 0;

	/*
	 * Bounded wait for any in-flight task to clear, either through normal
	 * completion (lima_sched_pipe_task_done) or the scheduler's own
	 * timeout recovery (lima_sched_timedout_job).  This must never spin
	 * forever: a job wedged on real hardware may simply never raise
	 * another interrupt.
	 */
	while (pipe_task_in_flight(pipe) &&
	       waited < LIMA_SCHED_PIPE_FINI_TIMEOUT_MS) {
		msleep(LIMA_SCHED_PIPE_FINI_POLL_MS);
		waited += LIMA_SCHED_PIPE_FINI_POLL_MS;
	}

	task = pipe_task_in_flight(pipe) ? pipe->current_task : NULL;
	if (task) {
		DRM_ERROR("%s: pipe still busy %ums into teardown — GPU task "
			  "did not complete or time out on its own; forcing "
			  "recovery instead of blocking module unload\n",
			  pipe->base.name, waited);

		/*
		 * Best-effort hardware reset.  Bounded internally: GP/PP
		 * hard_reset() poll for reset-complete via
		 * lima_poll_timeout() (lima_gp.c / lima_pp.c), they do not
		 * spin unconditionally.
		 */
		pipe->error = true;
		pipe->task_error(pipe);

		/*
		 * Force-complete the stuck fence so anything still waiting
		 * on it (a GEM wait ioctl, a dma_resv wait, or a later
		 * dma_fence_wait() inside drm_sched_stop()/drm_sched_fini()
		 * below) is released instead of hanging.  dma_fence_signal()
		 * is a no-op on an already-signalled fence, so this is safe
		 * even if the scheduler's own recovery raced us and already
		 * completed it.
		 */
		if (task->fence) {
			dma_fence_set_error(task->fence, -ENODEV);
			dma_fence_signal(task->fence);
		}

		lima_vm_put(pipe->current_vm);
		pipe->current_vm  = NULL;
		pipe->current_task = NULL;

	} else if (pipe->current_task != NULL) {
		/*
		 * Clean teardown with a completed task still recorded (the
		 * normal case -- see pipe_task_in_flight above). Drop the VM
		 * reference and the pointer here, because it used to happen
		 * inside the wedge branch that always ran; without this the
		 * reference would now leak on every unload.
		 */
		lima_vm_put(pipe->current_vm);
		pipe->current_vm  = NULL;
		pipe->current_task = NULL;
	}

	/*
	 * Park the scheduler thread and reap every job whose hardware fence
	 * already signalled (see root-cause note #1 above) before the final
	 * drm_sched_fini().  Passing bad=NULL: we are not recovering from a
	 * specific timed-out job here, just draining before teardown.
	 */
	drm_sched_stop(&pipe->base, NULL);

	drm_sched_fini(&pipe->base);
}

/*
 * lima_sched_pipe_task_done — IRQ/completion path: signal fence or handle error.
 *
 * sema:start
 * purpose:  Called from the pipe's interrupt handler (GP/PP done IRQ) to
 *           complete the current task.  On success: call task_fini, signal
 *           the completion fence, and release the PM busy reference.
 *           On error with a recoverable task: schedule recover_work.
 *           On unrecoverable error: signal drm_sched_fault.
 * input:    pipe — hardware pipe reporting completion
 * output:   none
 * sideEffects: conditional schedule_work; drm_sched_fault; pipe->task_fini;
 *             dma_fence_signal; lima_pm_idle
 * sema:end
 *
 * Called from IRQ context (or a tasklet / kernel thread depending on FreeBSD
 * LinuxKPI IRQ threading model); pipe->error set by the IP-specific IRQ
 * handler before calling this function.
 */
void lima_sched_pipe_task_done(struct lima_sched_pipe *pipe)
{
	struct lima_sched_task *task = pipe->current_task;
	struct lima_device *ldev = pipe->ldev;

	if (pipe->error) {
		if (task && task->recoverable)
			schedule_work(&pipe->recover_work);
		else
			drm_sched_fault(&pipe->base);
	} else {
		pipe->task_fini(pipe);
		dma_fence_signal(task->fence);

		lima_pm_idle(ldev);
	}
}
