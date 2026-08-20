/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright 2018-2019 Qiang Yu <yuq825@gmail.com>
 * Copyright 2024 bsdOS contributors
 *
 * FreeBSD port of Linux 6.6 drivers/gpu/drm/lima/lima_device.h
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the conditions of the
 * BSD 2-Clause License are met.
 */

// MODULE:      hal/lima/lima_device.h
// PURPOSE:     Lima GPU device descriptor — per-device state, IP block
//              registry, and pipe scheduler binding for Mali-400/450
// PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_device.h
// TARGET:      FreeBSD 15.1 · drm-66-kmod · LinuxKPI 6.6 compat layer
// MAINTAINER:  bsdOS project <a.bodrov@nextop.inc>
// NOTE:        spinlock_t, ktime_t, usleep_range, __iomem all satisfied
//              by linuxkpi; no FreeBSD-native sx/mtx replacements needed.
//              lima_device.ddev->dev_private is deprecated upstream;
//              callers should migrate to drmm_* managed allocation and
//              drm_dev_get_priv() once drm-66-kmod exposes that API.

#ifndef __LIMA_DEVICE_H__
#define __LIMA_DEVICE_H__

/*
 * LinuxKPI-provided headers — same include paths as Linux.
 * drm-66-kmod installs these under ${SYSDIR}/contrib/drm-66kmod/include.
 */
#include <drm/drm_device.h>
#include <linux/delay.h>      /* usleep_range, might_sleep_if → linuxkpi shim */
#include <linux/list.h>       /* list_head                   → linuxkpi shim */
#include <linux/mutex.h>      /* mutex                       → linuxkpi (sx)  */

#include "lima_sched.h"
#include "lima_dump.h"
#include "lima_devfreq.h"

/* ── GPU variant selector ─────────────────────────────────────────────────── */

enum lima_gpu_id {
	lima_gpu_mali400 = 0,
	lima_gpu_mali450,
	lima_gpu_num,
};

/* ── IP block identifiers ─────────────────────────────────────────────────── */
/*
 * Order must match the hardware register map.  All 27 IDs are present on
 * Mali-450; Mali-400 has fewer PP + ppmmu units (num_pp < 8).
 */

enum lima_ip_id {
	lima_ip_pmu,
	lima_ip_gpmmu,
	lima_ip_ppmmu0,
	lima_ip_ppmmu1,
	lima_ip_ppmmu2,
	lima_ip_ppmmu3,
	lima_ip_ppmmu4,
	lima_ip_ppmmu5,
	lima_ip_ppmmu6,
	lima_ip_ppmmu7,
	lima_ip_gp,
	lima_ip_pp0,
	lima_ip_pp1,
	lima_ip_pp2,
	lima_ip_pp3,
	lima_ip_pp4,
	lima_ip_pp5,
	lima_ip_pp6,
	lima_ip_pp7,
	lima_ip_l2_cache0,
	lima_ip_l2_cache1,
	lima_ip_l2_cache2,
	lima_ip_dlbu,
	lima_ip_bcast,
	lima_ip_pp_bcast,
	lima_ip_ppmmu_bcast,
	lima_ip_num,
};

/* ── Forward declaration ──────────────────────────────────────────────────── */

struct lima_device;

/* ── lima_ip — per-IP-block descriptor ───────────────────────────────────── */

struct lima_ip {
	struct lima_device  *dev;
	enum   lima_ip_id    id;
	bool                 present;

	/*
	 * MMIO window mapped via devm_ioremap_resource() (LinuxKPI).
	 * __iomem qualifier is retained; linuxkpi provides the sparse-
	 * compatible annotation.  A64 GPU MMIO: base 0x01C40000 size 0x10000.
	 */
	void __iomem        *iomem;
	int                  irq;

	union {
		/* gp / pp: set when a soft-reset is in flight */
		bool          async_reset;

		/*
		 * l2 cache: serialises flush/invalidate.
		 * spinlock_t → LinuxKPI spinlock wrapper (FreeBSD mtx);
		 * include path <linux/spinlock.h> via linuxkpi.
		 * No source-level change required.
		 */
		spinlock_t    lock;

		/* pmu / bcast: power-domain enable mask */
		u32           mask;
	} data;
};

/* ── Pipe identifier ──────────────────────────────────────────────────────── */

enum lima_pipe_id {
	lima_pipe_gp,
	lima_pipe_pp,
	lima_pipe_num,
};

/* ── lima_device — top-level GPU device state ────────────────────────────── */

struct lima_device {
	struct device        *dev;    /* LinuxKPI platform device              */
	struct drm_device    *ddev;   /* DRM device (drm-66-kmod)              */

	enum lima_gpu_id      id;
	u32                   gp_version;
	u32                   pp_version;
	int                   num_pp;

	/*
	 * Whole-device MMIO window.
	 * A64: base 0x01C40000, size 0x10000 (see mali_uio.h in this repo).
	 * Mapped via devm_ioremap_resource(); LinuxKPI handles the
	 * bus_space(9) mapping under the hood.
	 */
	void __iomem         *iomem;

	/*
	 * Clock handles.
	 * LinuxKPI clk_* shims wrap FreeBSD clk(9) behind a Linux-compatible
	 * struct clk *. On A64 the relevant clock is clk_a64_gpu.
	 */
	struct clk           *clk_bus;
	struct clk           *clk_gpu;

	/*
	 * Reset control.
	 * LinuxKPI reset_control_* wraps the Allwinner CCU reset line
	 * described by the FDT "resets" property.
	 */
	struct reset_control *reset;

	/*
	 * Voltage regulator.
	 * LinuxKPI regulator_* shim.  On PinePhone Pro / A64 the GPU VDD
	 * is a fixed-factor regulator; devm_regulator_get() returns a
	 * non-NULL stub that satisfies enable/disable calls.
	 */
	struct regulator     *regulator;

	/* Per-IP-block state, indexed by enum lima_ip_id */
	struct lima_ip        ip[lima_ip_num];

	/* GP and PP scheduler pipes */
	struct lima_sched_pipe pipe[lima_pipe_num];

	/*
	 * Empty VM used as a placeholder before application VMs are
	 * allocated.  va_start/va_end bound the GPU-visible address space.
	 */
	struct lima_vm       *empty_vm;
	uint64_t              va_start;
	uint64_t              va_end;

	/*
	 * Dynamic Load Balancing Unit descriptor page.
	 * Allocated via dma_alloc_coherent() — LinuxKPI dma_* shim over
	 * FreeBSD bus_dma(9).
	 */
	u32                  *dlbu_cpu;
	dma_addr_t            dlbu_dma;

	/* Dynamic frequency scaling (devfreq subsystem) */
	struct lima_devfreq   devfreq;

	/* ── Debug / error capture ────────────────────────────────────────── */
	struct lima_dump_head  dump;
	struct list_head       error_task_list;
	struct mutex           error_task_list_lock;
};

/* ── to_lima_dev() — accessor ─────────────────────────────────────────────── */

/*
 * to_lima_dev - retrieve lima_device pointer from a drm_device pointer.
 *
 * PURPOSE:      Accessor shim: dereferences drm_device.dev_private to
 *               recover the enclosing lima_device.
 * INPUT:        dev  — non-NULL drm_device *
 * OUTPUT:       lima_device * (never NULL after lima_device_init succeeds)
 * SIDE-EFFECTS: none
 *
 * DEPRECATION: drm_device.dev_private is deprecated in DRM >= 5.18.
 * The field still exists in drm-66-kmod but will be removed in a future
 * release.  Callers must migrate to drmm_alloc() + container_of() once
 * drm_dev_get_priv() is available in drm-66-kmod (>= 6.7).
 */
static inline struct lima_device *
to_lima_dev(struct drm_device *dev)
{
	return dev->dev_private;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int         lima_device_init(struct lima_device *ldev);
void        lima_device_fini(struct lima_device *ldev);

const char *lima_ip_name(struct lima_ip *ip);

/* ── lima_poll_timeout() — hardware register polling helper ──────────────── */

/*
 * lima_poll_func_t - predicate type for lima_poll_timeout().
 *
 * PURPOSE:      Function pointer type for a register-ready predicate.
 * INPUT:        ip — IP block to inspect
 * OUTPUT:       non-zero when the condition is satisfied
 * SIDE-EFFECTS: none (read-only register access expected)
 */
typedef int (*lima_poll_func_t)(struct lima_ip *);

/*
 * lima_poll_timeout - busy-poll an IP block until predicate returns true.
 *
 * PURPOSE:      Spin-wait with optional sleep on a hardware register
 *               condition, bounded by a microsecond deadline.
 * INPUT:        ip         — target IP block
 *               func       — predicate; non-zero means done
 *               sleep_us   — sleep interval in µs between polls (0=spin)
 *               timeout_us — total deadline in µs (0=no timeout)
 * OUTPUT:       0 on success, -ETIMEDOUT if deadline exceeded
 * SIDE-EFFECTS: may sleep when sleep_us > 0
 *
 * All primitives used here (ktime_add_us, ktime_get, ktime_compare,
 * usleep_range, might_sleep_if) resolve to LinuxKPI shims on FreeBSD.
 * No FreeBSD-native replacements are required.
 */
static inline int
lima_poll_timeout(struct lima_ip *ip, lima_poll_func_t func,
		  int sleep_us, int timeout_us)
{
	ktime_t timeout = ktime_add_us(ktime_get(), timeout_us);

	might_sleep_if(sleep_us);
	while (1) {
		if (func(ip))
			return 0;

		if (timeout_us && ktime_compare(ktime_get(), timeout) > 0)
			return -ETIMEDOUT;

		if (sleep_us)
			usleep_range((sleep_us >> 2) + 1, sleep_us);
	}
	return 0;
}

/* ── Power management hooks ───────────────────────────────────────────────── */

/*
 * Registered via LinuxKPI dev_pm_ops in lima_drv.c.
 * pm_runtime_* calls underneath resolve to linuxkpi shims backed by
 * FreeBSD device_suspend(9) / device_resume(9) where available.
 */
int lima_device_suspend(struct device *dev);
int lima_device_resume(struct device *dev);

#endif /* __LIMA_DEVICE_H__ */
