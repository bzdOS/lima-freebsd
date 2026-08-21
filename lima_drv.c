// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0
/* Copyright 2017-2019 Qiang Yu <yuq825@gmail.com>
 * Copyright 2024 bsdOS Project — FreeBSD 15.1 port
 *
 * MODULE:      hal/lima/lima_drv.c
 * PURPOSE:     Mali-400 DRM driver entry — probe, IOCTL dispatch, module init
 * PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_drv.c
 *
 * SEMA CONTRACT
 *   purpose:     Register Mali-400/450 as a DRM render node on FreeBSD 15.1
 *                via LinuxKPI + drm-66-kmod; dispatch userspace IOCTL requests
 *                to gem/vm/sched subsystems.
 *   input:       FDT node matching "arm,mali-400" or "arm,mali-400-mp2";
 *                MMIO base 0x01C40000 size 0x10000 (Allwinner A64).
 *   output:      /dev/dri/renderD* character device; DRM render node
 *                accessible to weston/mesa userspace.
 *   sideEffects: Allocates DRM device, initialises per-file VM + ctx manager,
 *                registers hw.lima_error sysctl node; enables pm_runtime
 *                autosuspend (200 ms).
 */

/*
 * PORTING NOTES (FreeBSD 15.1 / drm-66-kmod)
 * ============================================
 * 1. All Linux headers below are provided by LinuxKPI inside drm-66-kmod;
 *    include them unchanged.
 * 2. drm_gem_shmem_prime_import_sg_table -> drm_gem_dma_prime_import_sg_table
 *    drm-66-kmod on FreeBSD uses the DMA helper path, not the shmem helper.
 * 3. sysfs bin_attribute (lima_error_state_read/write) is replaced by a
 *    FreeBSD sysctl(9) PROC node at hw.lima_error.  The block-reader logic
 *    is identical.
 * 4. module_platform_driver() is a LinuxKPI macro that generates the correct
 *    DRIVER_MODULE + simplebus attachment for FreeBSD newbus/FDT.
 * 5. of_device_id table gains "arm,mali-400-mp2" for the Allwinner A64.
 * 6. kvcalloc, kvfree, kmem_cache_zalloc, copy_from_user, u64_to_user_ptr,
 *    devm_kzalloc, pm_runtime_* — all LinuxKPI-provided; no changes needed.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_drv.h>
#include <drm/drm_prime.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/lima_drm.h>

/* FreeBSD: sysctl(9) for error-state node (replaces Linux sysfs bin_attribute) */
#include <sys/sysctl.h>
#include <sys/kernel.h>

#include "lima_device.h"
#include "lima_drv.h"
#include "lima_gem.h"
#include "lima_vm.h"
#include "lima_freebsd_compat.h"

/* ── Module parameters ────────────────────────────────────────────────
 *
 * LinuxKPI module_param_named() maps these to FreeBSD sysctl nodes
 * under hw.lima.* rather than /sys/module/lima/parameters/.
 */

int  lima_sched_timeout_ms;
uint lima_heap_init_nr_pages = 8;
uint lima_max_error_tasks;
uint lima_job_hang_limit;

MODULE_PARM_DESC(sched_timeout_ms, "task run timeout in ms");
module_param_named(sched_timeout_ms, lima_sched_timeout_ms, int, 0444);

MODULE_PARM_DESC(heap_init_nr_pages, "heap buffer init number of pages");
module_param_named(heap_init_nr_pages, lima_heap_init_nr_pages, uint, 0444);

MODULE_PARM_DESC(max_error_tasks, "max number of error tasks to save");
module_param_named(max_error_tasks, lima_max_error_tasks, uint, 0644);

MODULE_PARM_DESC(job_hang_limit,
    "number of times to allow a job to hang before dropping it (default 0)");
module_param_named(job_hang_limit, lima_job_hang_limit, uint, 0444);

/* ── IOCTL handlers ──────────────────────────────────────────────────*/

static int
lima_ioctl_get_param(struct drm_device *dev, void *data,
                     struct drm_file *file)
{
	/*
	 * purpose:     Return a single GPU capability/version parameter to
	 *              userspace via drm_lima_get_param.
	 * input:       args->param — one of DRM_LIMA_PARAM_*
	 * output:      args->value set on success; -EINVAL for unknown param.
	 * sideEffects: none
	 */
	struct drm_lima_get_param *args = data;
	struct lima_device        *ldev = to_lima_dev(dev);

	if (args->pad)
		return -EINVAL;

	switch (args->param) {
	case DRM_LIMA_PARAM_GPU_ID:
		switch (ldev->id) {
		case lima_gpu_mali400:
			args->value = DRM_LIMA_PARAM_GPU_ID_MALI400; break;
		case lima_gpu_mali450:
			args->value = DRM_LIMA_PARAM_GPU_ID_MALI450; break;
		default:
			args->value = DRM_LIMA_PARAM_GPU_ID_UNKNOWN;  break;
		}
		break;
	case DRM_LIMA_PARAM_NUM_PP:
		args->value = ldev->pipe[lima_pipe_pp].num_processor; break;
	case DRM_LIMA_PARAM_GP_VERSION:
		args->value = ldev->gp_version; break;
	case DRM_LIMA_PARAM_PP_VERSION:
		args->value = ldev->pp_version; break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int
lima_ioctl_gem_create(struct drm_device *dev, void *data,
                      struct drm_file *file)
{
	/*
	 * purpose:     Allocate a GEM buffer object and return a userspace handle.
	 * input:       args->{size, flags} — size in bytes; flags: LIMA_BO_FLAG_HEAP
	 * output:      args->handle — GEM handle on success.
	 * sideEffects: Increments drm_device gem object count.
	 */
	struct drm_lima_gem_create *args = data;

	if (args->pad)
		return -EINVAL;
	if (args->flags & ~(LIMA_BO_FLAG_HEAP))
		return -EINVAL;
	if (args->size == 0)
		return -EINVAL;

	return lima_gem_create_handle(dev, file, args->size,
	                              args->flags, &args->handle);
}

static int
lima_ioctl_gem_info(struct drm_device *dev, void *data,
                    struct drm_file *file)
{
	/*
	 * purpose:     Return GPU VA and mmap offset for a GEM handle.
	 * input:       args->handle — valid GEM handle for this file.
	 * output:      args->{va, offset} set on success.
	 * sideEffects: none
	 */
	struct drm_lima_gem_info *args = data;

	return lima_gem_get_info(file, args->handle,
	                         &args->va, &args->offset);
}

static int
lima_ioctl_gem_submit(struct drm_device *dev, void *data,
                      struct drm_file *file)
{
	/*
	 * purpose:     Submit a GP or PP job to the hardware scheduler.
	 * input:       args — pipe id, BO list, frame data, sync fences.
	 * output:      out_sync fence signalled on job completion.
	 * sideEffects: Allocates task from slab; pins BOs; enqueues to
	 *              lima_sched_pipe; consumes in/out syncobjs.
	 */
	struct drm_lima_gem_submit   *args = data;
	struct lima_device            *ldev = to_lima_dev(dev);
	struct lima_drm_priv          *priv = file->driver_priv;
	struct drm_lima_gem_submit_bo *bos;
	struct lima_sched_pipe        *pipe;
	struct lima_sched_task        *task;
	struct lima_ctx               *ctx;
	struct lima_submit             submit = {0};
	size_t                         size;
	int                            err = 0;

	if (args->pipe >= lima_pipe_num || args->nr_bos == 0)
		return -EINVAL;
	if (args->flags & ~(LIMA_SUBMIT_FLAG_EXPLICIT_FENCE))
		return -EINVAL;

	pipe = ldev->pipe + args->pipe;
	if (args->frame_size != pipe->frame_size)
		return -EINVAL;

	bos = kvcalloc(args->nr_bos,
	               sizeof(*submit.bos) + sizeof(*submit.lbos),
	               GFP_KERNEL);
	if (!bos)
		return -ENOMEM;

	size = args->nr_bos * sizeof(*submit.bos);
	if (copy_from_user(bos, u64_to_user_ptr(args->bos), size)) {
		err = -EFAULT;
		goto out0;
	}

	task = kmem_cache_zalloc(pipe->task_slab, GFP_KERNEL);
	if (!task) { err = -ENOMEM; goto out0; }

	task->frame = task + 1;
	if (copy_from_user(task->frame, u64_to_user_ptr(args->frame),
	                   args->frame_size)) {
		err = -EFAULT; goto out1;
	}

	err = pipe->task_validate(pipe, task);
	if (err) goto out1;

	ctx = lima_ctx_get(&priv->ctx_mgr, args->ctx);
	if (!ctx) { err = -ENOENT; goto out1; }

	submit.pipe       = args->pipe;
	submit.bos        = bos;
	submit.lbos       = (struct lima_bo **)((uint8_t *)bos + size);
	submit.nr_bos     = args->nr_bos;
	submit.task       = task;
	submit.ctx        = ctx;
	submit.flags      = args->flags;
	submit.in_sync[0] = args->in_sync[0];
	submit.in_sync[1] = args->in_sync[1];
	submit.out_sync   = args->out_sync;

	err = lima_gem_submit(file, &submit);

	lima_ctx_put(ctx);
out1:
	if (err)
		kmem_cache_free(pipe->task_slab, task);
out0:
	kvfree(bos);
	return err;
}

static int
lima_ioctl_gem_wait(struct drm_device *dev, void *data,
                    struct drm_file *file)
{
	/*
	 * purpose:     Block until a GEM BO's last read or write fence signals.
	 * input:       args->{handle, op, timeout_ns}
	 * output:      0 on signal; -ETIME on timeout; -EINVAL on bad op.
	 * sideEffects: none
	 */
	struct drm_lima_gem_wait *args = data;

	if (args->op & ~(LIMA_GEM_WAIT_READ | LIMA_GEM_WAIT_WRITE))
		return -EINVAL;
	return lima_gem_wait(file, args->handle,
	                     args->op, args->timeout_ns);
}

static int
lima_ioctl_ctx_create(struct drm_device *dev, void *data,
                      struct drm_file *file)
{
	/*
	 * purpose:     Allocate a GPU scheduling context for this DRM file.
	 * input:       args->_pad must be zero.
	 * output:      args->id — opaque context handle.
	 * sideEffects: Inserts ctx into priv->ctx_mgr IDR.
	 */
	struct drm_lima_ctx_create *args = data;
	struct lima_drm_priv        *priv = file->driver_priv;
	struct lima_device           *ldev = to_lima_dev(dev);

	if (args->_pad) return -EINVAL;
	return lima_ctx_create(ldev, &priv->ctx_mgr, &args->id);
}

static int
lima_ioctl_ctx_free(struct drm_device *dev, void *data,
                    struct drm_file *file)
{
	/*
	 * purpose:     Release a GPU scheduling context previously created
	 *              via lima_ioctl_ctx_create.
	 * input:       args->id — context handle to free.
	 * output:      0 on success; -ENOENT if id not found.
	 * sideEffects: Removes ctx from IDR; waits for outstanding jobs.
	 */
	struct drm_lima_ctx_create *args = data;
	struct lima_drm_priv        *priv = file->driver_priv;

	if (args->_pad) return -EINVAL;
	return lima_ctx_free(&priv->ctx_mgr, args->id);
}

/* ── Per-file open / postclose ───────────────────────────────────────*/

static int
lima_drm_driver_open(struct drm_device *dev, struct drm_file *file)
{
	/*
	 * purpose:     Initialise per-DRM-file private state (VM + ctx manager).
	 * input:       dev — lima drm_device; file — newly opened drm_file.
	 * output:      file->driver_priv set to allocated lima_drm_priv.
	 * sideEffects: Allocates lima_vm and initialises lima_ctx_mgr.
	 */
	struct lima_drm_priv *priv;
	struct lima_device    *ldev = to_lima_dev(dev);
	int                    err;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->vm = lima_vm_create(ldev);
	if (!priv->vm) {
		err = -ENOMEM;
		goto err_out0;
	}

	lima_ctx_mgr_init(&priv->ctx_mgr);

	file->driver_priv = priv;
	return 0;

err_out0:
	kfree(priv);
	return err;
}

static void
lima_drm_driver_postclose(struct drm_device *dev, struct drm_file *file)
{
	/*
	 * purpose:     Tear down per-file state after last reference dropped.
	 * input:       file->driver_priv — lima_drm_priv allocated in open.
	 * output:      none
	 * sideEffects: Frees lima_vm; drains ctx_mgr; kfree priv.
	 */
	struct lima_drm_priv *priv = file->driver_priv;

	lima_ctx_mgr_fini(&priv->ctx_mgr);
	lima_vm_put(priv->vm);
	kfree(priv);
}

/* ── IOCTL table + drm_driver struct ────────────────────────────────*/

static const struct drm_ioctl_desc lima_drm_driver_ioctls[] = {
	DRM_IOCTL_DEF_DRV(LIMA_GET_PARAM,  lima_ioctl_get_param,   DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(LIMA_GEM_CREATE, lima_ioctl_gem_create,  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(LIMA_GEM_INFO,   lima_ioctl_gem_info,    DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(LIMA_GEM_SUBMIT, lima_ioctl_gem_submit,  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(LIMA_GEM_WAIT,   lima_ioctl_gem_wait,    DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(LIMA_CTX_CREATE, lima_ioctl_ctx_create,  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(LIMA_CTX_FREE,   lima_ioctl_ctx_free,    DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(lima_drm_driver_fops);

/*
 * Changelog:
 *   1.1.0 — heap buffer support
 */
static const struct drm_driver lima_drm_driver = {
	.driver_features     = DRIVER_RENDER | DRIVER_GEM | DRIVER_SYNCOBJ,
	.open                = lima_drm_driver_open,
	.postclose           = lima_drm_driver_postclose,
	.ioctls              = lima_drm_driver_ioctls,
	.num_ioctls          = ARRAY_SIZE(lima_drm_driver_ioctls),
	.fops                = &lima_drm_driver_fops,
	.name                = "lima",
	.desc                = "lima DRM",
	.date                = "20191231",
	.major               = 1,
	.minor               = 1,
	.patchlevel          = 0,

	.gem_create_object         = lima_gem_create_object,
	.gem_prime_import_sg_table = drm_gem_shmem_prime_import_sg_table,
};

/* ── Block reader (unchanged from Linux) ────────────────────────────*/

struct lima_block_reader {
	uint8_t *dst;
	size_t   base;
	size_t   count;
	size_t   off;
	ssize_t  read;
};

static bool
lima_read_block(struct lima_block_reader *reader,
               void *src, size_t src_size)
{
	/*
	 * purpose:     Copy a contiguous slice from a virtual "block" into
	 *              reader->dst, advancing reader state.
	 * input:       reader — current read cursor; src/src_size — source block.
	 * output:      true if bytes remain to be read.
	 * sideEffects: Advances reader->{dst,off,read,base,count}.
	 */
	size_t max_off = reader->base + src_size;

	if (reader->off < max_off) {
		size_t size = min_t(size_t, max_off - reader->off,
		                    reader->count);
		memcpy(reader->dst, (const uint8_t *)src + (reader->off - reader->base), size);
		reader->dst   += size;
		reader->off   += size;
		reader->read  += size;
		reader->count -= size;
	}
	reader->base = max_off;
	return !!reader->count;
}

/* ── Error-state: FreeBSD sysctl(9) port of Linux sysfs bin_attribute */

/*
 * purpose:     sysctl PROC handler for hw.lima_error — read returns the
 *              serialised error task dump; write clears the list.
 * input:       req — sysctl request (newptr==NULL -> read, else write).
 * output:      0 on success; errno on fault.
 * sideEffects: Read: copies dump + task data under error_task_list_lock.
 *              Write: drains error_task_list; resets dump.{size,num_tasks}.
 */
static int
lima_error_state_sysctl(struct sysctl_oid *oidp, void *arg1,
                        intmax_t arg2, struct sysctl_req *req)
{
	struct lima_device           *ldev = (struct lima_device *)arg1;
	struct lima_sched_error_task *et, *tmp;
	int                           error;

	if (req->newptr != NULL) {
		/* Write path: clear error task list */
		mutex_lock(&ldev->error_task_list_lock);
		list_for_each_entry_safe(et, tmp,
		                         &ldev->error_task_list, list) {
			list_del(&et->list);
			kvfree(et);
		}
		ldev->dump.size      = 0;
		ldev->dump.num_tasks = 0;
		mutex_unlock(&ldev->error_task_list_lock);
		return (0);
	}

	/* Read path: serialise dump into sysctl output buffer */
	mutex_lock(&ldev->error_task_list_lock);

	error = SYSCTL_OUT(req, &ldev->dump, sizeof(ldev->dump));
	if (!error) {
		list_for_each_entry(et, &ldev->error_task_list, list) {
			error = SYSCTL_OUT(req, et->data, et->size);
			if (error) break;
		}
	}

	mutex_unlock(&ldev->error_task_list_lock);
	return (error);
}

/* One sysctl OID per loaded driver instance */
static struct sysctl_oid *lima_error_oid;

static void
lima_error_sysctl_register(struct lima_device *ldev)
{
	/*
	 * purpose:     Register hw.lima_error sysctl node for this device.
	 * input:       ldev — fully initialised lima_device.
	 * output:      none (failures are silent; node simply absent).
	 * sideEffects: Allocates OID under _hw tree; stores in lima_error_oid.
	 */
	lima_error_oid =
	    SYSCTL_ADD_PROC(NULL,
	        SYSCTL_STATIC_CHILDREN(_hw),
	        OID_AUTO, "lima_error",
	        CTLTYPE_OPAQUE | CTLFLAG_RW | CTLFLAG_MPSAFE,
	        (void *)ldev, 0,
	        lima_error_state_sysctl,
	        "A", "Lima GPU error task dump (write to clear)");
}

static void
lima_error_sysctl_unregister(void)
{
	/*
	 * purpose:     Remove hw.lima_error sysctl node.
	 * input:       none
	 * output:      none
	 * sideEffects: Frees OID; sets lima_error_oid = NULL.
	 */
	if (lima_error_oid) {
		sysctl_remove_oid(lima_error_oid, 1, 0);
		lima_error_oid = NULL;
	}
}

/* ── Platform probe / remove ────────────────────────────────────────*/

static int
lima_pdev_probe(struct platform_device *pdev)
{
	/*
	 * purpose:     Bind Lima DRM driver to a Mali-400/450 platform device
	 *              discovered via FDT "arm,mali-400*" compatible string.
	 * input:       pdev — FDT-matched platform device (A64: MMIO 0x01C40000).
	 * output:      0 on success; negative errno on failure.
	 * sideEffects: Allocates lima_device + drm_device; initialises hw blocks;
	 *              enables pm_runtime autosuspend (200 ms); registers DRM node;
	 *              registers hw.lima_error sysctl.
	 */
	struct lima_device *ldev;
	struct drm_device   *ddev;
	int                  err;

	err = lima_sched_slab_init();
	if (err)
		return err;

	ldev = devm_kzalloc(&pdev->dev, sizeof(*ldev), GFP_KERNEL);
	if (!ldev) { err = -ENOMEM; goto err_out0; }

	ldev->dev = &pdev->dev;
	ldev->id  = (enum lima_gpu_id)(uintptr_t)of_device_get_match_data(&pdev->dev);

	platform_set_drvdata(pdev, ldev);

	ddev = drm_dev_alloc(&lima_drm_driver, &pdev->dev);
	if (IS_ERR(ddev)) { err = PTR_ERR(ddev); goto err_out0; }

	ddev->dev_private = ldev;
	ldev->ddev        = ddev;

	err = lima_device_init(ldev);
	if (err) goto err_out1;

	err = lima_devfreq_init(ldev);
	if (err) {
		dev_err(&pdev->dev, "Fatal error during devfreq init\n");
		goto err_out2;
	}

	pm_runtime_set_active(ldev->dev);
	pm_runtime_mark_last_busy(ldev->dev);
	pm_runtime_set_autosuspend_delay(ldev->dev, 200);
	pm_runtime_use_autosuspend(ldev->dev);
	pm_runtime_enable(ldev->dev);

	err = drm_dev_register(ddev, 0);
	if (err < 0) goto err_out3;

	/* FreeBSD: sysctl instead of sysfs bin_attribute */
	lima_error_sysctl_register(ldev);

	return 0;

err_out3:
	pm_runtime_disable(ldev->dev);
	lima_devfreq_fini(ldev);
err_out2:
	lima_device_fini(ldev);
err_out1:
	drm_dev_put(ddev);
err_out0:
	lima_sched_slab_fini();
	return err;
}

static void
lima_pdev_remove(struct platform_device *pdev)
{
	/*
	 * purpose:     Cleanly detach Lima DRM driver from the platform device.
	 * input:       pdev — platform device previously probed.
	 * output:      none
	 * sideEffects: Unregisters DRM node + sysctl; disables pm_runtime;
	 *              tears down hw blocks; frees DRM device + slab caches.
	 */
	struct lima_device *ldev = platform_get_drvdata(pdev);
	struct drm_device   *ddev = ldev->ddev;

	lima_error_sysctl_unregister();

	drm_dev_unregister(ddev);

	/* stop autosuspend to ensure device is in active state */
	pm_runtime_set_autosuspend_delay(ldev->dev, -1);
	pm_runtime_disable(ldev->dev);

	lima_devfreq_fini(ldev);
	lima_device_fini(ldev);

	drm_dev_put(ddev);
	lima_sched_slab_fini();
}

/* ── FDT match table ────────────────────────────────────────────────*/

static const struct of_device_id dt_match[] = {
	/* Allwinner A64: Mali-400 MP2 (2 pixel processors) */
	{ .compatible = "arm,mali-400-mp2", .data = (void *)lima_gpu_mali400 },
	/* Generic Mali-400 fallback */
	{ .compatible = "arm,mali-400",     .data = (void *)lima_gpu_mali400 },
	{ .compatible = "arm,mali-450",     .data = (void *)lima_gpu_mali450 },
	{}
};
MODULE_DEVICE_TABLE(of, dt_match);

/* ── Power management ops ───────────────────────────────────────────*/

static const struct dev_pm_ops lima_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
	                        pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(lima_device_suspend, lima_device_resume, NULL)
};

/* ── Platform driver + module registration ──────────────────────────*/

/*
 * Same shape as upstream Linux.  It works on FreeBSD because
 * hal/lima/linux/platform_device.h shadows linuxkpi's stub with a
 * platform_driver that really has .probe and a device_driver that really has
 * .of_match_table, and module_platform_driver() there registers this driver
 * with newbus (parent bus "simplebus", i.e. FDT /soc children) instead of
 * expanding to linuxkpi's platform_driver_register(), which returns -ENXIO
 * unconditionally.
 */
static struct platform_driver lima_platform_driver = {
	.probe  = lima_pdev_probe,
	.remove = lima_pdev_remove,
	.driver = {
		.name           = "lima",
		.pm             = &lima_pm_ops,
		.of_match_table = dt_match,
	},
};

module_platform_driver(lima_platform_driver);

/*
 * Real FreeBSD module dependencies (sys/module.h, pulled in transitively via
 * <linux/module.h> -> <sys/module.h>) — NOT the same thing as the
 * Linux-style MODULE_AUTHOR/DESCRIPTION/LICENSE tags below.
 *
 * Without these, kldload of this module fails with
 * "link_elf: symbol sysctl___hw_dri undefined" even though drm.ko is already
 * loaded and genuinely exports that symbol. Root cause (see
 * docs/README-arm64.md): sys/kern/kern_linker.c's linker_file_lookup_symbol_internal()
 * resolves a module's undefined references ONLY against itself, the kernel
 * proper (always implicit), and its own linker_file->deps[] — which is
 * populated exclusively from THIS module's own MODULE_DEPEND() metadata, one
 * level deep (the recursive lookup passes deps=0, so drm.ko's dependency on
 * dmabuf.ko is not inherited by lima.ko). There is no fallback that searches
 * "every other currently-loaded kld" the way a plain nm/readelf read of
 * drm.ko (which does show the symbol as global) might suggest. Every other
 * drm-kmod driver (amdgpu, i915, ttm, radeon, dummygfx) declares exactly this
 * pair — lima's port had simply never carried it over.
 */
MODULE_DEPEND(lima, drmn, 2, 2, 2);
MODULE_DEPEND(lima, dmabuf, 1, 1, 1);

MODULE_AUTHOR("Lima Project Developers");
MODULE_AUTHOR("bsdOS Project — FreeBSD 15.1 port");
MODULE_DESCRIPTION("Lima DRM Driver (FreeBSD 15.1 / drm-66-kmod)");
MODULE_LICENSE("Dual BSD/GPL");
