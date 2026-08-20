/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * linux/reset.h — real hwreset(9) consumer binding for Lima on FreeBSD 15.1
 *
 * HISTORY / WHY THIS FILE WAS REWRITTEN (2026-08-11)
 *
 * Until 2026-08-11 every function below was a no-op stub returning success,
 * on the claim that "the CCU reset lines are toggled by the FDT driver at
 * probe time before lima.ko loads."  That claim was false: FreeBSD's
 * hwreset(9) framework (sys/dev/hwreset/hwreset.h) requires an explicit
 * consumer call — hwreset_get_by_ofw_name()/hwreset_array_get_ofw() plus
 * hwreset_deassert() — exactly like Linux's reset_control_deassert().
 * Nothing deasserts a reset line on a driver's behalf.
 *
 * This bug shipped alongside the linux/clk.h stub (see that file's header
 * for the full failure signature: `mmu gpmmu dte write test fail`,
 * `hw.clock.bus-gpu.enable_cnt: 0`).  For the specific Allwinner A64
 * Mali-400 GPU, the CCU's BUS_SOFT_RST register for the GPU bit defaults
 * de-asserted at reset on this SoC, so the missing reset_control_deassert()
 * call was not itself the failure that was observed live — the missing clock
 * enable was.  It is fixed here anyway, for real, because leaving it a stub
 * would recreate the exact same class of bug (silent fake success hiding a
 * gated IP block) the moment this driver runs on hardware where the GPU
 * reset line is NOT deasserted by default, or after a suspend/resume cycle
 * that re-asserts it.  A future reader must not reintroduce the "something
 * else resets it" assumption.
 *
 * WHAT THIS FILE ACTUALLY DOES NOW
 *
 *   - devm_reset_control_array_get_optional_shared(dev) resolves the
 *     consumer FDT node's "resets"/"reset-names" property list via
 *     hwreset_array_get_ofw() — the array form is what a DT node with
 *     multiple, possibly shared, reset lines uses, matching Linux's
 *     "_array_...shared" naming.
 *   - devm_reset_control_get_optional_shared(dev, id) resolves a single
 *     named reset line via hwreset_get_by_ofw_name(), for drivers (unlike
 *     the current lima_device.c) that ask for one reset by name.
 *   - "optional": if the FDT node simply has no "resets" property (or no
 *     entry with that name), that is not an error — it returns NULL, same as
 *     Linux's *_optional_* variants.  Any OTHER failure (malformed
 *     reference, xref resolution failure, etc.) is a real error and is
 *     returned as ERR_PTR(-errno), not papered over as "no reset controller
 *     present."
 *   - reset_control_deassert()/reset_control_assert() call
 *     hwreset_(de)assert() or hwreset_array_(de)assert() for real and
 *     propagate a negative errno on failure, so lima_clk_enable()'s existing
 *     `if (err) { dev_err(...); goto error_out1; }` path actually triggers
 *     on a real reset-controller failure instead of never firing.
 *
 * devm_* LIFETIME
 *
 * Same contract as linux/clk.h: the struct device embedded in this driver's
 * struct platform_device (linux/platform_device.h) is a real, unmodified
 * LinuxKPI struct device whose devres list runs automatically at detach
 * (lkpi_platform_bus_detach() -> put_device() -> lkpi_platform_dev_release()
 * -> lkpi_devres_release_free_list()).  The wrapper object below (needed
 * because a single "struct reset_control *" here must represent either the
 * scalar hwreset_t or the hwreset_array_t case) is allocated with the real
 * devm_kzalloc(), and a devm_add_action() registers the release of the
 * underlying hwreset(9) handle.  Because LinuxKPI's devres list is released
 * head-first and new entries are added at the head (see linux_devres.c
 * lkpi_devres_add()/lkpi_devres_release_free_list()), adding the release
 * action *after* the devm_kzalloc() guarantees the hwreset handle is
 * released before the wrapper memory backing it is freed. lima_clk_fini()
 * only calls reset_control_assert(); it never explicitly frees dev->reset,
 * relying on exactly this devm behavior.
 */

#ifndef _LIMA_LINUX_RESET_H_
#define _LIMA_LINUX_RESET_H_

#include <linux/compiler.h>
#include <linux/stddef.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/device.h>

#include <sys/types.h>
#include <dev/ofw/openfirm.h>
#include <dev/hwreset/hwreset.h>

#include "platform_device.h"	/* to_platform_device(), struct device.bsddev */

struct device;

/*
 * Real storage: exactly one of the two is populated, selected by is_array.
 * Allocated with devm_kzalloc(); see the devm_* LIFETIME note above.
 */
struct reset_control {
	bool		is_array;
	union {
		hwreset_t	single;
		hwreset_array_t	array;
	} u;
};

/*
 * purpose:     devm release action: releases whichever hwreset(9) handle a
 *              reset_control wraps.
 * input:       data — the struct reset_control *, passed back by
 *              devm_add_action().
 * output:      none
 * sideEffects: calls hwreset_release()/hwreset_array_release(); the wrapper
 *              memory itself is freed separately by devm_kzalloc()'s own
 *              devres entry (added before this action, so released after
 *              it — see the header comment).
 */
static inline void
lima_reset_release_action(void *data)
{
	struct reset_control *rc = data;

	if (rc == NULL)
		return;
	if (rc->is_array)
		hwreset_array_release(rc->u.array);
	else
		hwreset_release(rc->u.single);
}

/*
 * purpose:     Resolve the "resets"/"reset-names" property list of a
 *              consumer's FDT node into a shared, optional reset_control.
 * input:       dev — struct device of a platform_device built by
 *              linux/platform_device.h.
 * output:      reset_control * on success; NULL if the node has no "resets"
 *              property at all (legitimately optional); ERR_PTR(-errno) on
 *              any other failure (malformed property, allocation failure,
 *              devm registration failure).
 * sideEffects: registers a devm release action on dev.
 */
static inline struct reset_control *
devm_reset_control_array_get_optional_shared(struct device *dev)
{
	struct platform_device *pdev;
	struct reset_control *rc;
	int error;

	if (dev == NULL)
		return (ERR_PTR(-EINVAL));

	pdev = to_platform_device(dev);
	if (pdev == NULL || dev->bsddev == NULL)
		return (ERR_PTR(-ENODEV));

	rc = devm_kzalloc(dev, sizeof(*rc), GFP_KERNEL);
	if (rc == NULL)
		return (ERR_PTR(-ENOMEM));

	error = hwreset_array_get_ofw(dev->bsddev, (phandle_t)pdev->node,
	    &rc->u.array);
	if (error != 0) {
		/* devm_kzalloc's own devres entry frees rc at detach; nothing
		 * to release here since hwreset_array_get_ofw() failed before
		 * producing a handle. */
		if (error == ENOENT)
			return (NULL);
		return (ERR_PTR(-error));
	}
	rc->is_array = true;

	error = devm_add_action(dev, lima_reset_release_action, rc);
	if (error != 0) {
		hwreset_array_release(rc->u.array);
		return (ERR_PTR(-error));
	}

	return (rc);
}

/*
 * purpose:     Resolve a single named reset line ("reset-names" entry) into
 *              a shared, optional reset_control.
 * input:       dev — as above; id — reset-names entry to resolve.
 * output:      reset_control * on success; NULL if the named entry is simply
 *              absent; ERR_PTR(-errno) on any other failure.
 * sideEffects: registers a devm release action on dev.
 *
 * Unused by lima_device.c today (it only asks for the array form), provided
 * for API completeness/future drivers in this tree that name a single reset.
 */
static inline struct reset_control *
devm_reset_control_get_optional_shared(struct device *dev, const char *id)
{
	struct platform_device *pdev;
	struct reset_control *rc;
	int error;

	if (dev == NULL)
		return (ERR_PTR(-EINVAL));

	pdev = to_platform_device(dev);
	if (pdev == NULL || dev->bsddev == NULL)
		return (ERR_PTR(-ENODEV));

	rc = devm_kzalloc(dev, sizeof(*rc), GFP_KERNEL);
	if (rc == NULL)
		return (ERR_PTR(-ENOMEM));

	error = hwreset_get_by_ofw_name(dev->bsddev, (phandle_t)pdev->node,
	    __DECONST(char *, id), &rc->u.single);
	if (error != 0) {
		if (error == ENOENT)
			return (NULL);
		return (ERR_PTR(-error));
	}
	rc->is_array = false;

	error = devm_add_action(dev, lima_reset_release_action, rc);
	if (error != 0) {
		hwreset_release(rc->u.single);
		return (ERR_PTR(-error));
	}

	return (rc);
}

/*
 * purpose:     Deassert (release) a reset line, bringing the IP block out of
 *              reset.
 * input:       rc — handle from either devm_reset_control_*_get_optional_*
 *              call above, or NULL (no-op success, matching Linux's
 *              "optional reset may be NULL" convention).
 * output:      0 on success; negative errno on failure.
 * sideEffects: real hwreset_deassert()/hwreset_array_deassert() hardware
 *              write.
 */
static inline int
reset_control_deassert(struct reset_control *rc)
{
	int error;

	if (rc == NULL)
		return (0);

	error = rc->is_array ? hwreset_array_deassert(rc->u.array) :
	    hwreset_deassert(rc->u.single);
	if (error != 0)
		return (-error);
	return (0);
}

/*
 * purpose:     Assert a reset line, holding the IP block in reset.
 * input:       rc — as above, or NULL (no-op).
 * output:      0 on success; negative errno on failure.
 * sideEffects: real hwreset_assert()/hwreset_array_assert() hardware write.
 */
static inline int
reset_control_assert(struct reset_control *rc)
{
	int error;

	if (rc == NULL)
		return (0);

	error = rc->is_array ? hwreset_array_assert(rc->u.array) :
	    hwreset_assert(rc->u.single);
	if (error != 0)
		return (-error);
	return (0);
}

/*
 * purpose:     Pulse a reset line (assert then deassert).  hwreset(9) has no
 *              single "reset pulse" primitive, so this composes the two.
 * input:       rc — as above, or NULL (no-op).
 * output:      0 on success; negative errno from whichever half fails first
 *              (left deasserted only if the assert half itself failed).
 * sideEffects: two real hardware writes.
 *
 * Unused by lima_device.c today; provided for API completeness.
 */
static inline int
reset_control_reset(struct reset_control *rc)
{
	int error;

	if (rc == NULL)
		return (0);

	error = reset_control_assert(rc);
	if (error != 0)
		return (error);
	return (reset_control_deassert(rc));
}

#endif /* _LIMA_LINUX_RESET_H_ */
