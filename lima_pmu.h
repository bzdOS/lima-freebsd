/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * lima_pmu.h — Mali-400 Power Management Unit (PMU) interface
 *
 * Ported from Linux 6.6 drivers/gpu/drm/lima/lima_pmu.h
 * The PMU powers domains on/off independently on multi-PP Mali-450.
 * On Mali-400 (single PP) power is managed at the device level.
 */

#ifndef __LIMA_PMU_H__
#define __LIMA_PMU_H__

struct lima_ip;

int  lima_pmu_init(struct lima_ip *ip);
void lima_pmu_fini(struct lima_ip *ip);
int  lima_pmu_resume(struct lima_ip *ip);
void lima_pmu_suspend(struct lima_ip *ip);

#endif /* __LIMA_PMU_H__ */
