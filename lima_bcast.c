// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * lima_bcast.c — Mali-450 broadcast unit stub (FreeBSD 15.1)
 *
 * SEMA CONTRACT
 *   purpose:     Control the Mali-450 broadcast unit that fans out
 *                frame-descriptor writes to all pixel processors.
 *   input:       ip — lima_ip for the bcast block; num_pp — active PP count.
 *   output:      0 on success.
 *   sideEffects: no-op stub; real impl enables the bcast mask register.
 */

#include <linux/device.h>
#include "lima_device.h"
#include "lima_bcast.h"

int lima_bcast_init(struct lima_ip *ip)
{
	dev_dbg(ip->dev->dev, "lima_bcast_init (stub)\n");
	return 0;
}

void lima_bcast_fini(struct lima_ip *ip)
{
}

int lima_bcast_resume(struct lima_ip *ip)
{
	return lima_bcast_init(ip);
}

void lima_bcast_suspend(struct lima_ip *ip)
{
}

void lima_bcast_enable(struct lima_device *dev, int num_pp)
{
}
