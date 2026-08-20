/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * lima_freebsd_compat.h — LinuxKPI gaps for Lima on FreeBSD 15.1 amd64
 *
 * drm-66-kmod targets PCI/amd64 only.  Lima is an ARM/DT driver, so several
 * subsystems are absent from FreeBSD's linuxkpi:
 *
 *   - Open Firmware / Device Tree (struct of_device_id, OF match)
 *   - platform_get/set_drvdata  (wrapper around dev_get/set_drvdata)
 *   - module_platform_driver    (init/exit via platform bus)
 *   - SET_SYSTEM_SLEEP_PM_OPS / SET_RUNTIME_PM_OPS (CONFIG_PM_SLEEP off)
 *   - pm_runtime_force_suspend / pm_runtime_force_resume
 *
 * Include this header in lima_drv.c AFTER the linuxkpi system headers so
 * these definitions shadow the would-be missing ones.
 *
 * On a real aarch64 port with full DT linuxkpi these stubs would be replaced
 * by proper implementations.  For now they let lima.ko compile and load on
 * the dev VM (amd64) to validate the port structure.
 */

#ifndef _LIMA_FREEBSD_COMPAT_H_
#define _LIMA_FREEBSD_COMPAT_H_

#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/pm.h>

/* ── Open Firmware / Device Tree ─────────────────────────────────────── */

/*
 * struct of_device_id, platform_get/set_drvdata, module_platform_driver,
 * platform_get_irq*, of_device_get_match_data and
 * devm_platform_ioremap_resource all used to be faked here.  They are now real
 * and live in hal/lima/linux/platform_device.h (which shadows linuxkpi's stub
 * via -I${.CURDIR}) plus hal/lima/linux/platform_device.c.  The only thing
 * still needed on this side is MODULE_DEVICE_TABLE's per-bus hook, which
 * linuxkpi's module.h expects a definition of but never uses meaningfully for
 * the "of" bus.
 */
#define MODULE_DEVICE_TABLE_BUS_of(bus, tbl)	/* handled by the FDT bridge */

/* ── PM ops macros (guarded by CONFIG_PM_SLEEP which is off on FreeBSD) */

#ifndef SET_SYSTEM_SLEEP_PM_OPS
#define SET_SYSTEM_SLEEP_PM_OPS(_sus, _res)	\
	.suspend  = (_sus),			\
	.resume   = (_res),			\
	.freeze   = (_sus),			\
	.thaw     = (_res),			\
	.poweroff = (_sus),			\
	.restore  = (_res),
#endif

#ifndef SET_RUNTIME_PM_OPS
#define SET_RUNTIME_PM_OPS(_sus, _res, _idle)	\
	.runtime_suspend = (_sus),		\
	.runtime_resume  = (_res),		\
	.runtime_idle    = (_idle),
#endif

/* ── pm_runtime_force helpers (not in linuxkpi) ──────────────────────── */

/*
 * On Linux these iterate child devices and call the driver's own suspend/
 * resume callbacks in the right order.  We forward-declare them here so
 * the compiler is happy; lima_drv.c replaces the uses with the driver's
 * own callbacks via the #define below.
 */
int lima_device_suspend(struct device *dev);
int lima_device_resume(struct device *dev);

#ifndef pm_runtime_force_suspend
#define pm_runtime_force_suspend	lima_device_suspend
#define pm_runtime_force_resume		lima_device_resume
#endif

/* ── DMA write-combining helpers (not in linuxkpi) ───────────────────── */

#include <linux/dma-mapping.h>

static inline void *
dma_alloc_wc(struct device *dev, size_t size, dma_addr_t *dma_addr,
	     gfp_t gfp)
{
	return dma_alloc_coherent(dev, size, dma_addr, gfp);
}

static inline void
dma_free_wc(struct device *dev, size_t size, void *cpu_addr,
	    dma_addr_t dma_addr)
{
	dma_free_coherent(dev, size, cpu_addr, dma_addr);
}

/* ── pm_runtime_resume_and_get (not in linuxkpi pm_runtime.h) ───────── */

#include <linux/pm_runtime.h>

static inline int
pm_runtime_resume_and_get(struct device *dev)
{
	return pm_runtime_get_sync(dev);
}

/* ── kmem_cache_create_usercopy (not in linuxkpi) ────────────────────── */

#include <linux/slab.h>

/*
 * Linux 5.x added kmem_cache_create_usercopy for hardened user-copy.
 * FreeBSD linuxkpi has only kmem_cache_create; ignore the usercopy bounds.
 */
#ifndef kmem_cache_create_usercopy
static inline struct linux_kmem_cache *
kmem_cache_create_usercopy(const char *name, unsigned int size,
			    unsigned int align, unsigned long flags,
			    unsigned int useroffset, unsigned int usersize,
			    void (*ctor)(void *))
{
	return kmem_cache_create(name, size, align, flags, ctor);
}
#endif

/* ── readl_poll_timeout (not in linuxkpi iopoll.h) ───────────────────── */

#include <linux/io.h>

/*
 * Spin-poll addr until cond is true or timeout_us microseconds elapse.
 * sleep_us is ignored (no sleep in kernel poll).
 * Returns 0 on success, -ETIMEDOUT on timeout.
 */
#ifndef readl_poll_timeout
#define readl_poll_timeout(addr, val, cond, sleep_us, timeout_us)	\
({									\
	int __pt_ret = -ETIMEDOUT;					\
	int __pt_us = (timeout_us);					\
	do {								\
		(val) = readl(addr);					\
		if (cond) { __pt_ret = 0; break; }			\
		DELAY(1);						\
	} while (--__pt_us > 0);					\
	__pt_ret;							\
})
#endif

#endif /* _LIMA_FREEBSD_COMPAT_H_ */
