/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * lima_l2_cache.h — Mali-400 L2 cache IP interface
 * Ported from Linux 6.6 drivers/gpu/drm/lima/lima_l2_cache.h
 */

#ifndef __LIMA_L2_CACHE_H__
#define __LIMA_L2_CACHE_H__

struct lima_ip;

int  lima_l2_cache_init(struct lima_ip *ip);
void lima_l2_cache_fini(struct lima_ip *ip);
int  lima_l2_cache_resume(struct lima_ip *ip);
void lima_l2_cache_suspend(struct lima_ip *ip);
int  lima_l2_cache_flush(struct lima_ip *ip);

#endif /* __LIMA_L2_CACHE_H__ */
