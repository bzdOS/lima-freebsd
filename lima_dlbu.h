/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * lima_dlbu.h — Mali-450 Dynamic Load Balancing Unit interface
 * Ported from Linux 6.6 drivers/gpu/drm/lima/lima_dlbu.h
 */

#ifndef __LIMA_DLBU_H__
#define __LIMA_DLBU_H__

struct lima_ip;
struct lima_sched_task;

int  lima_dlbu_init(struct lima_ip *ip);
void lima_dlbu_fini(struct lima_ip *ip);
int  lima_dlbu_resume(struct lima_ip *ip);
void lima_dlbu_suspend(struct lima_ip *ip);
void lima_dlbu_setup(struct lima_ip *ip, struct lima_sched_task *task);
void lima_dlbu_enable(struct lima_device *dev, int num_pp);
void lima_dlbu_disable(struct lima_device *dev);
void lima_dlbu_set_reg(struct lima_ip *ip, u32 *regs);

#endif /* __LIMA_DLBU_H__ */
