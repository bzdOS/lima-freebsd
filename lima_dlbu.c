// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * lima_dlbu.c — Mali-450 Dynamic Load Balancing Unit stub (FreeBSD 15.1)
 *
 * SEMA CONTRACT
 *   purpose:     Initialise and program the Mali-450 DLBU for task dispatch.
 *   input:       ip — lima_ip for the DLBU block.
 *   output:      0 on success.
 *   sideEffects: no-op stub; real impl writes DLBU register set.
 */

#include <linux/device.h>
#include "lima_device.h"
#include "lima_dlbu.h"
#include "lima_sched.h"

int lima_dlbu_init(struct lima_ip *ip)
{
	dev_dbg(ip->dev->dev, "lima_dlbu_init (stub)\n");
	return 0;
}

void lima_dlbu_fini(struct lima_ip *ip)
{
}

int lima_dlbu_resume(struct lima_ip *ip)
{
	return lima_dlbu_init(ip);
}

void lima_dlbu_suspend(struct lima_ip *ip)
{
}

void lima_dlbu_setup(struct lima_ip *ip, struct lima_sched_task *task)
{
}

void lima_dlbu_enable(struct lima_device *dev, int num_pp)
{
}

void lima_dlbu_disable(struct lima_device *dev)
{
}

void lima_dlbu_set_reg(struct lima_ip *ip, u32 *regs)
{
}
