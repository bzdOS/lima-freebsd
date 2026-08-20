/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright 2017-2019 Qiang Yu <yuq825@gmail.com>
 * Copyright 2024 bsdOS Project (FreeBSD 15.1 port)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 */

// MODULE:      hal/lima/lima_gp.h
// PURPOSE:     Mali-400 Geometry Processor (GP) lifecycle and pipeline API
// PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_gp.h
// TARGET:      FreeBSD 15.1 aarch64, drm-66-kmod LinuxKPI
// SIDEEFFECTS: none — declarations only; implementation in lima_gp.c

#ifndef __LIMA_GP_H__
#define __LIMA_GP_H__

/*
 * FreeBSD note: lima_ip and lima_device are defined in lima_device.h.
 * Both structs are portable as-is; LinuxKPI in drm-66-kmod provides the
 * underlying drm_device and device_node fields they depend on.
 */
struct lima_ip;
struct lima_device;

int  lima_gp_resume(struct lima_ip *ip);
void lima_gp_suspend(struct lima_ip *ip);
int  lima_gp_init(struct lima_ip *ip);
void lima_gp_fini(struct lima_ip *ip);

int  lima_gp_pipe_init(struct lima_device *dev);
void lima_gp_pipe_fini(struct lima_device *dev);

#endif  /* __LIMA_GP_H__ */
