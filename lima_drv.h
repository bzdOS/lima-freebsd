// MODULE: hal/lima/lima_drv.h
// PURPOSE: DRM private state for Lima (Mali-400) file handles and submit objects
// PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_drv.h

/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright 2017-2019 Qiang Yu <yuq825@gmail.com>
 * Copyright 2024 bsdOS contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.
 */

#ifndef __LIMA_DRV_H__
#define __LIMA_DRV_H__

/*
 * purpose:   Per-file DRM private state and job-submit descriptor for
 *            Lima Mali-400 under FreeBSD 15.1 drm-66-kmod LinuxKPI.
 * input:     included by lima_drv.c, lima_gem.c, lima_sched.c
 * output:    struct lima_drm_priv, struct lima_submit, to_lima_drm_priv()
 * sideEffects: none — header only
 */

#include <drm/drm_file.h>    /* LinuxKPI drm-66-kmod: struct drm_file */

#include "lima_ctx.h"         /* struct lima_ctx, struct lima_ctx_mgr */

/*
 * Scheduler / heap tuning knobs — defined in lima_drv.c,
 * exposed here so other translation units can read them.
 *
 * Identical to Linux: no FreeBSD-specific changes needed because
 * drm-66-kmod LinuxKPI provides module_param_named() equivalent
 * via the same extern int / extern uint pattern.
 */
extern int  lima_sched_timeout_ms;    /* job timeout before GPU reset   */
extern uint lima_heap_init_nr_pages;  /* initial heap pages per context  */
extern uint lima_max_error_tasks;     /* max tasks kept on error list    */
extern uint lima_job_hang_limit;      /* consecutive hangs before reset  */

/* Forward declarations — definitions in their respective headers. */
struct lima_vm;
struct lima_bo;
struct lima_sched_task;
struct drm_lima_gem_submit_bo;        /* uAPI: per-BO submit entry       */

/*
 * struct lima_drm_priv
 *
 * purpose:   Driver-private state allocated per open DRM file handle.
 *            Stored in drm_file::driver_priv; freed on file release.
 * input:     allocated by lima_drv_open(), zero-initialised
 * output:    vm      — per-process GPU virtual address space
 *            ctx_mgr — pool of scheduler contexts for this fd
 * sideEffects: vm and ctx_mgr hold references; must be released on close
 *
 * FreeBSD note: drm_file::driver_priv is available unchanged through
 * LinuxKPI — no adaptation required.
 */
struct lima_drm_priv {
	struct lima_vm      *vm;       /* GPU MMU page tables for this fd  */
	struct lima_ctx_mgr  ctx_mgr;  /* scheduler context manager        */
};

/*
 * struct lima_submit
 *
 * purpose:   Transient descriptor built from the userspace ioctl payload
 *            during DRM_IOCTL_LIMA_GEM_SUBMIT; discarded after enqueue.
 * input:     populated by lima_gem_submit_ioctl()
 * output:    task — scheduler task handed to drm_sched_entity_push_job()
 * sideEffects: holds references to ctx, bos[], lbos[] until task completes
 *
 * Field sizes and layout are identical to Linux; u32 is provided by
 * LinuxKPI <linux/types.h> which drm-66-kmod pulls in automatically.
 */
struct lima_submit {
	struct lima_ctx              *ctx;      /* owning scheduler context        */
	int                           pipe;     /* 0 = GP, 1 = PP                  */
	u32                           flags;    /* DRM_LIMA_SUBMIT_FLAG_*           */

	struct drm_lima_gem_submit_bo *bos;     /* uAPI BO array (kernel copy)     */
	struct lima_bo              **lbos;     /* resolved lima_bo pointers        */
	u32                           nr_bos;  /* element count of bos[]/lbos[]   */

	u32                           in_sync[2]; /* wait fences: [0]=GP, [1]=PP   */
	u32                           out_sync;   /* signal fence fd                */

	struct lima_sched_task       *task;     /* scheduler task (owns BOs)       */
};

/*
 * to_lima_drm_priv()
 *
 * purpose:   Recover lima_drm_priv from a drm_file pointer.
 * input:     file — open DRM file handle, driver_priv already set
 * output:    pointer to lima_drm_priv; never NULL after open succeeds
 * sideEffects: none
 *
 * Identical to Linux — LinuxKPI preserves drm_file::driver_priv semantics.
 */
static inline struct lima_drm_priv *
to_lima_drm_priv(struct drm_file *file)
{
	return file->driver_priv;
}

#endif /* __LIMA_DRV_H__ */
