/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * linux/clk.h — real clk(9) consumer binding for Lima on FreeBSD 15.1
 *
 * HISTORY / WHY THIS FILE WAS REWRITTEN (2026-08-11)
 *
 * Until 2026-08-11 every function below was a no-op stub: devm_clk_get()
 * returned NULL, clk_prepare_enable() returned 0 without touching hardware,
 * clk_get_rate()/clk_set_rate() returned 0.  The header's own comment claimed
 * "the actual clock enable/disable happens via FDT overlays and the real CCU
 * driver" — i.e. something else, elsewhere, would turn the clock on before
 * lima.ko needed it.  That claim was false.  FreeBSD's clk(9) framework
 * (sys/dev/clk/clk.h, backed here by the Allwinner CCU driver) requires an
 * explicit consumer call — clk_get_by_ofw_name() + clk_enable() — exactly like
 * Linux's clk_prepare_enable().  Nothing enables a clock on a driver's behalf.
 *
 * The cost of the stub returning fake success: lima_clk_enable() in
 * lima_device.c believed both the bus clock and the GPU core clock were
 * running, proceeded to lima_mmu.c's directory-table-entry write/read-back
 * self-test, and failed it — logged on real hardware as:
 *
 *     lima_platform_driver0: mmu gpmmu dte write test fail
 *     lima_platform_driver0: probe failed: -5
 *
 * with `hw.clock.bus-gpu.enable_cnt: 0` confirming the bus clock had in fact
 * never been enabled by anyone.  A future reader must NOT reintroduce the
 * "something else enables it" assumption: on this platform, if this file
 * does not call clk_enable(), the clock stays gated, full stop.
 *
 * WHAT THIS FILE ACTUALLY DOES NOW
 *
 *   - devm_clk_get(dev, id) resolves the named clock ("bus"/"core" per the
 *     A64 Mali-400 DT binding's clock-names) against the consumer's FDT node
 *     via clk_get_by_ofw_name(), exactly the real API a native FreeBSD FDT
 *     driver would call.
 *   - The returned "struct clk *" IS the real clk_t (clk_t is itself a
 *     typedef for struct clk *, see sys/dev/clk/clk.h) — no separate wrapper
 *     object, no extra allocation.  This header's own forward declaration of
 *     "struct clk" is never given a body, so it stays compatible with the
 *     kernel's own opaque type in this translation unit.
 *   - clk_prepare_enable()/clk_disable_unprepare() are clk_enable()/
 *     clk_disable() by another name.  FreeBSD's clknode framework already
 *     keeps a per-clock enable refcount (visible as the enable_cnt sysctl
 *     used to diagnose this bug), so no separate prepare/enable-count
 *     bookkeeping belongs in this shim.
 *   - Failure is not swallowed: clk_get_by_ofw_name()/clk_enable() failures
 *     are converted to a negative errno and returned through IS_ERR()/
 *     PTR_ERR(), which lima_clk_init()/lima_clk_enable() already check and
 *     log.  A missing or unenableable clock now aborts probe instead of
 *     silently reporting success.
 *
 * devm_* LIFETIME
 *
 * Linux's devm_clk_get() ties the clk handle to the struct device's devres
 * list, freed automatically on device removal, and lima_device.c relies on
 * exactly that contract: lima_clk_fini() only gates the clocks
 * (clk_disable_unprepare()) and never calls clk_put().  The struct device
 * embedded in our struct platform_device (linux/platform_device.h) is a real,
 * unmodified LinuxKPI struct device — its ->devres_head/->devres_lock are
 * initialized by lkpi_platform_bus_attach() and run by
 * lkpi_platform_dev_release() (via lkpi_devres_release_free_list()) when the
 * device is detached.  That means devm_add_action(), already provided by
 * LinuxKPI's real linux/device.h, gives this shim genuine devm_ semantics:
 * devm_clk_get() registers a release action that calls clk_release() when
 * the device goes away, so the caller truly does not need to free anything.
 * clk_put() is provided for drivers that call it explicitly (lima_device.c
 * does not); it runs the same release immediately instead of waiting for
 * detach, which is safe here because FreeBSD's devm_add_action() has no
 * "cancel this specific action" call — an explicit early clk_put() plus the
 * deferred devm action means clk_release() would run twice, using a freed
 * handle the second time. To avoid that, clk_put() below does not call
 * clk_release() itself; it is a documented no-op that defers to the devm
 * action, matching what lima_device.c already assumes.
 *
 * clk_get_rate() / clk_set_rate()
 *
 * Both are real: clk_get_rate() calls clk_get_freq() and clk_set_rate() calls
 * clk_set_freq().  clk_get_rate() returning 0 on failure or on a NULL clk is
 * not a new instance of the stub bug — it is Linux's own documented contract
 * for clk_get_rate() (0 means "unknown"), and lima_device.c only uses it for
 * an informational dev_info() rate print, never for a correctness decision.
 * clk_set_rate() is unused anywhere in this port today (no OPP/DVFS wiring
 * yet), but is implemented for real rather than left inert, since Lima's
 * upstream OPP support calls it.
 */

#ifndef _LIMA_LINUX_CLK_H_
#define _LIMA_LINUX_CLK_H_

#include <linux/compiler.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/device.h>

#include <sys/types.h>
#include <dev/ofw/openfirm.h>
#include <dev/clk/clk.h>

#include "platform_device.h"	/* to_platform_device(), struct device.bsddev */

/*
 * Never given a full definition in this translation unit: the real storage
 * is the kernel's private "struct clk" in sys/dev/clk/clk.c.  clk_t (see
 * dev/clk/clk.h) is "typedef struct clk *clk_t", so a "struct clk *" here and
 * a clk_t there are the same bits; devm_clk_get() returns the clk_t handle
 * straight through with no wrapper allocation.
 */
struct clk;
struct device;

/*
 * purpose:     Release action run by devm when the owning device is detached
 *              (or immediately, see clk_put() below): releases the clk(9)
 *              handle obtained by devm_clk_get()/devm_clk_get_optional().
 * input:       data — the clk_t handle, passed back by devm_add_action().
 * output:      none
 * sideEffects: calls clk_release(), which clk(9) documents as safe to call
 *              only once per successful clk_get_by_ofw_name().
 */
static inline void
lima_clk_release_action(void *data)
{
	clk_t clk = (clk_t)data;

	if (clk != NULL)
		clk_release(clk);
}

/*
 * purpose:     Resolve a named clock from the consumer device's FDT node and
 *              tie its lifetime to the device (devm_ semantics).
 * input:       dev — struct device of a platform_device built by
 *              linux/platform_device.h (its ->node is the FDT phandle);
 *              id — clock-names entry to resolve ("bus", "core", ...).
 *              optional — if true, "clock not present in the DT node" (as
 *              opposed to any other failure) yields NULL rather than an
 *              ERR_PTR(), matching devm_clk_get_optional()'s contract.
 * output:       real clk_t handle on success; ERR_PTR(-errno) on failure;
 *              NULL if optional and the clock-names entry is simply absent.
 * sideEffects: registers a devm release action on dev; no hardware state
 *              changes until clk_prepare_enable() is called.
 */
static inline struct clk *
lima_devm_clk_get_common(struct device *dev, const char *id, bool optional)
{
	struct platform_device *pdev;
	clk_t clk;
	int error;

	if (dev == NULL)
		return (ERR_PTR(-EINVAL));

	pdev = to_platform_device(dev);
	if (pdev == NULL || dev->bsddev == NULL)
		return (ERR_PTR(-ENODEV));

	error = clk_get_by_ofw_name(dev->bsddev, (phandle_t)pdev->node, id,
	    &clk);
	if (error != 0) {
		if (optional && error == ENOENT)
			return (NULL);
		return (ERR_PTR(-error));
	}

	error = devm_add_action(dev, lima_clk_release_action, clk);
	if (error != 0) {
		clk_release(clk);
		return (ERR_PTR(-error));
	}

	return ((struct clk *)clk);
}

static inline struct clk *
devm_clk_get(struct device *dev, const char *id)
{
	return (lima_devm_clk_get_common(dev, id, false));
}

static inline struct clk *
devm_clk_get_optional(struct device *dev, const char *id)
{
	return (lima_devm_clk_get_common(dev, id, true));
}

/*
 * purpose:     Enable a clock obtained via devm_clk_get(); FreeBSD's clk(9)
 *              has no separate prepare/enable split, so this is clk_enable().
 * input:       clk — handle from devm_clk_get(), or NULL (a no-op success,
 *              matching Linux's own "clk may be NULL/optional" convention).
 * output:      0 on success; negative errno from clk_enable() on failure.
 * sideEffects: increments the clknode's enable refcount; on the GPU bus
 *              clock this is the write that was missing before 2026-08-11.
 */
static inline int
clk_prepare_enable(struct clk *clk)
{
	int error;

	if (clk == NULL)
		return (0);

	error = clk_enable((clk_t)clk);
	if (error != 0)
		return (-error);
	return (0);
}

/*
 * purpose:     Disable a clock enabled via clk_prepare_enable().
 * input:       clk — handle from devm_clk_get(), or NULL (no-op).
 * output:      none
 * sideEffects: decrements the clknode's enable refcount; gates the clock
 *              once the count reaches zero.
 */
static inline void
clk_disable_unprepare(struct clk *clk)
{
	if (clk != NULL)
		clk_disable((clk_t)clk);
}

/*
 * purpose:     Read back a clock's current frequency.
 * input:       clk — handle from devm_clk_get(), or NULL.
 * output:      frequency in Hz, or 0 if clk is NULL or clk_get_freq() fails.
 *              0-on-failure is Linux's own documented clk_get_rate() contract
 *              (see header comment), not a stub returning fake success.
 * sideEffects: none
 */
static inline unsigned long
clk_get_rate(struct clk *clk)
{
	uint64_t freq;

	if (clk == NULL)
		return (0);
	if (clk_get_freq((clk_t)clk, &freq) != 0)
		return (0);
	return ((unsigned long)freq);
}

/*
 * purpose:     Request a new frequency for a clock.
 * input:       clk — handle from devm_clk_get(); rate — desired Hz.
 * output:      0 on success; negative errno on failure, including -EINVAL if
 *              clk is NULL (Lima does not currently call this; unused today
 *              because this port has no OPP/DVFS table wired up yet, but it
 *              is implemented for real since upstream Lima's OPP support
 *              does call it).
 * sideEffects: reprograms the backing clknode (PLL/divider) if supported.
 */
static inline int
clk_set_rate(struct clk *clk, unsigned long rate)
{
	int error;

	if (clk == NULL)
		return (-EINVAL);

	error = clk_set_freq((clk_t)clk, (uint64_t)rate, CLK_SET_ROUND_ANY);
	if (error != 0)
		return (-error);
	return (0);
}

/*
 * purpose:     Explicit release, for drivers that call clk_put() instead of
 *              relying on devm.  lima_device.c does not call this.
 * input:       clk — handle from devm_clk_get(), or NULL.
 * output:      none
 * sideEffects: intentionally none beyond documentation: devm_clk_get()
 *              already registered a devm release action that will call
 *              clk_release() at device-detach time. FreeBSD's devm_add_action
 *              has no "cancel" call, so releasing here too would double-free
 *              the same clk_t handle when detach runs its devm list. Keeping
 *              this a no-op is deliberate, not the old stub bug: the clock
 *              handle is still released exactly once, just at detach instead
 *              of at clk_put() time.
 */
static inline void
clk_put(struct clk *clk)
{
	(void)clk;
}

#endif /* _LIMA_LINUX_CLK_H_ */
