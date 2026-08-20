/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * lima_devfreq.h — devfreq stub for FreeBSD Lima port
 *
 * FreeBSD has no Linux devfreq subsystem.  All entry points are no-ops
 * so the driver compiles and loads; GPU runs at fixed clock frequency.
 * DVFS can be wired up later via FreeBSD sysctls / OPP tables if needed.
 */

#ifndef __LIMA_DEVFREQ_H__
#define __LIMA_DEVFREQ_H__

struct lima_devfreq {
	/* placeholder — keeps sizeof(lima_device) stable */
	unsigned int	dummy;
};

struct lima_device;

static inline int lima_devfreq_init(struct lima_device *ldev)
{
	return 0;
}

static inline void lima_devfreq_fini(struct lima_device *ldev)
{
}

static inline int lima_devfreq_resume(struct lima_devfreq *devfreq)
{
	return 0;
}

static inline int lima_devfreq_suspend(struct lima_devfreq *devfreq)
{
	return 0;
}

static inline void lima_devfreq_record_busy(struct lima_devfreq *devfreq)
{
}

static inline void lima_devfreq_record_idle(struct lima_devfreq *devfreq)
{
}

#endif /* __LIMA_DEVFREQ_H__ */
