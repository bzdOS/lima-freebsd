// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2019 Hans de Goede <hdegoede@redhat.com> (original Linux code)
 * FreeBSD port: bsdOS Project, 2026
 */

// MODULE: hal/lima/drm/drm_utils_freebsd.c
// PURPOSE: The one function of Linux' drivers/gpu/drm/drm_utils.c that DRM
//          drivers need and drm-kmod does not build.
// PORTED_FROM: Linux 6.6 drivers/gpu/drm/drm_utils.c
//
// drm-kmod ships include/drm/drm_utils.h (so drivers compile) but has no
// drm_utils.c at all — the file does not exist in the repository at tag
// drm_v6.6.25_13, and drm/Makefile never mentions it. So
// drm_timeout_abs_to_jiffies() is a second symbol, besides the GEM SHMEM
// helpers, that any driver using the standard "wait for BO with absolute
// timeout" ioctl pattern (lima, panfrost, v3d, msm, …) references and that
// resolves nowhere on FreeBSD.
//
// Like the shmem helper next to it, this is driver-agnostic and belongs in
// drm.ko; it lives here only because drm.ko cannot currently be patched from
// this tree.

#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/sched.h>

#include <drm/drm_utils.h>

/*
 * purpose: convert an absolute CLOCK_MONOTONIC deadline in nanoseconds into a
 *          relative timeout in jiffies, as the DRM_IOCTL_*_WAIT ioctls use
 * input:   timeout_nsec — absolute deadline in ns; 0 means "poll, do not block"
 * output:  jiffies to wait: 0 if the deadline has already passed, otherwise at
 *          least 1, clamped to MAX_SCHEDULE_TIMEOUT - 1 so an absurd deadline
 *          cannot turn into an infinite wait
 * effects: none (reads the monotonic clock)
 */
signed long
drm_timeout_abs_to_jiffies(int64_t timeout_nsec)
{
	ktime_t abs_timeout, now;
	uint64_t timeout_ns, timeout_jiffies64;

	/* Save some cycles and wrapping time if the caller asked for 0. */
	if (timeout_nsec == 0)
		return (0);

	abs_timeout = ns_to_ktime(timeout_nsec);
	now = ktime_get();

	if (!ktime_after(abs_timeout, now))
		return (0);

	timeout_ns = ktime_to_ns(ktime_sub(abs_timeout, now));

	timeout_jiffies64 = nsecs_to_jiffies64(timeout_ns);

	/* Clamp the timeout so it can never be mistaken for "wait forever". */
	if (timeout_jiffies64 >= MAX_SCHEDULE_TIMEOUT - 1)
		return (MAX_SCHEDULE_TIMEOUT - 1);

	/*
	 * Round up: a sub-jiffy remainder must still block, otherwise a
	 * deadline "1 ns from now" would return immediately.
	 */
	return (timeout_jiffies64 + 1);
}
