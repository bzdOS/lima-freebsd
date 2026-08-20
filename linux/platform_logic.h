/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * linux/platform_logic.h — the kernel-free parts of the platform_device bridge
 *
 * Everything in linux/platform_device.{h,c} that is pure arithmetic or pure
 * table-walking lives here, so it can be compiled and tested from userspace
 * without a kernel.  linux/platform_device.h includes this file; the host test
 * hal/lima/tests/test_platform_logic.c includes this same shipped file rather
 * than a copy, so a regression in the IRQ-token encoding or the match-table
 * scan fails the test instead of failing silently at attach time.
 *
 * Rule for what belongs here: no newbus, no busdma, no OFW, no linuxkpi.  If a
 * function needs a device_t it belongs in platform_device.c.
 */

#ifndef _BZDOS_LKPI_PLATFORM_LOGIC_H_
#define	_BZDOS_LKPI_PLATFORM_LOGIC_H_

#ifndef _KERNEL
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

/*
 * Resource fan-out limits.  Both are per platform_device and only size the
 * arrays in struct platform_device; raising them costs a few pointers each.
 * The largest real consumer today is Mali-450 MP8 (one MMIO window, twenty-odd
 * interrupts).
 */
#define	LKPI_PLATFORM_MAX_MEM	8
#define	LKPI_PLATFORM_MAX_IRQ	32

/* ── Open Firmware match table ───────────────────────────────────────── */

/*
 * Linux' struct of_device_id.  Only .compatible and .data are meaningful on
 * FreeBSD; .name/.type are kept so upstream driver tables compile unchanged.
 * The table terminator is an all-zero entry, i.e. compatible[0] == '\0'.
 */
struct of_device_id {
	char		name[32];
	char		type[32];
	char		compatible[128];
	const void	*data;
};

/*
 * Predicate supplied by the caller so this scan needs no OFW: in the kernel it
 * wraps ofw_bus_is_compatible(dev, s), in the tests it compares against a
 * fixture string.
 */
typedef bool lkpi_platform_compat_fn(void *ctx, const char *compatible);

/*
 * purpose:     Find the first entry of a Linux of_device_id table whose
 *              compatible string the caller's predicate accepts.
 * input:       tbl — NULL-terminated match table, may be NULL;
 *              is_compatible — predicate, may be NULL;
 *              ctx — opaque, passed through to the predicate.
 * output:      pointer to the winning entry, or NULL when nothing matched.
 * sideEffects: none
 */
static inline const struct of_device_id *
lkpi_platform_of_match_scan(const struct of_device_id *tbl,
    lkpi_platform_compat_fn *is_compatible, void *ctx)
{

	if (tbl == NULL || is_compatible == NULL)
		return (NULL);
	for (; tbl->compatible[0] != '\0'; tbl++) {
		if (is_compatible(ctx, tbl->compatible))
			return (tbl);
	}
	return (NULL);
}

/* ── IRQ tokens ──────────────────────────────────────────────────────── */

/*
 * A platform IRQ "number" is a token, not an interrupt vector.  Layout:
 *
 *	bit 30 .. 20   LKPI_PLATFORM_IRQ_TAG   (marks the token as ours)
 *	bit 19 ..  8   newbus unit of the owning device
 *	bit  7 ..  0   resource index within that device
 *
 * The tag is what lets linux/interrupt.h route a bare request_irq(irq, ...)
 * — which carries no struct device * — to this layer rather than to linuxkpi's
 * PCI-only lkpi_pci_find_irq_dev() path, and lets a collision with a real PCI
 * MSI vector be detected rather than silently mishandled.
 *
 * Bit 31 is deliberately left clear.  Linux drivers test platform_get_irq()
 * for failure with `if (ret < 0)` on a signed int; a token with the sign bit
 * set would be read as an error by every caller.
 */
#define	LKPI_PLATFORM_IRQ_TAG		0x71d00000u
#define	LKPI_PLATFORM_IRQ_TAG_MASK	0xfff00000u
#define	LKPI_PLATFORM_IRQ_TOKEN(unit, idx)				\
	(LKPI_PLATFORM_IRQ_TAG | (((unsigned int)(unit) & 0xfffu) << 8) |\
	    ((unsigned int)(idx) & 0xffu))
#define	LKPI_PLATFORM_IRQ_IS_TOKEN(irq)					\
	((((unsigned int)(irq)) & LKPI_PLATFORM_IRQ_TAG_MASK) ==	\
	    LKPI_PLATFORM_IRQ_TAG)
#define	LKPI_PLATFORM_IRQ_INDEX(irq)	(((unsigned int)(irq)) & 0xffu)
#define	LKPI_PLATFORM_IRQ_UNIT(irq)					\
	((((unsigned int)(irq)) >> 8) & 0xfffu)

/* ── Resource index bounds ───────────────────────────────────────────── */

/*
 * purpose:     Single place that decides whether a caller-supplied resource
 *              index is servable, so platform_get_resource(),
 *              platform_get_irq() and ioremap() cannot drift apart.
 * input:       navail — number of resources the device actually acquired;
 *              idx — index the caller asked for.
 * output:      true iff idx addresses an acquired resource.
 * sideEffects: none
 */
static inline bool
lkpi_platform_res_index_ok(int navail, unsigned int idx)
{

	if (navail <= 0)
		return (false);
	return (idx < (unsigned int)navail);
}

#endif	/* _BZDOS_LKPI_PLATFORM_LOGIC_H_ */
