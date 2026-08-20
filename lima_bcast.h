/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * lima_bcast.h — Mali-450 broadcast unit interface
 * Ported from Linux 6.6 drivers/gpu/drm/lima/lima_bcast.h
 */

#ifndef __LIMA_BCAST_H__
#define __LIMA_BCAST_H__

struct lima_ip;

int  lima_bcast_init(struct lima_ip *ip);
void lima_bcast_fini(struct lima_ip *ip);
int  lima_bcast_resume(struct lima_ip *ip);
void lima_bcast_suspend(struct lima_ip *ip);
void lima_bcast_enable(struct lima_device *dev, int num_pp);

#endif /* __LIMA_BCAST_H__ */
