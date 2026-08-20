/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * linux/interrupt.h — route the request_irq() family for platform devices
 *
 * THE TRAP THIS FILE EXISTS TO CLOSE
 *
 * linuxkpi's lkpi_request_irq() (sys/compat/linuxkpi/common/src/
 * linux_interrupt.c) turns a Linux IRQ number into a newbus resource like this:
 *
 *	dev = lkpi_pci_find_irq_dev(irq);
 *	if (dev == NULL)
 *		return -ENXIO;
 *
 * and lkpi_pci_find_irq_dev() (linux_pci.c) walks the global `pci_devices`
 * list.  A platform device is not on that list and cannot be put on it, so
 * devm_request_irq() returns -ENXIO for a platform device *no matter how
 * correct the platform_device bridge is*.  A "perfect" bridge still delivers
 * no interrupts.  This is the one part of the platform-device problem that
 * is invisible until attach time, which is why it gets its own file.
 *
 * HOW IT IS CLOSED
 *
 * linuxkpi's entry points are static inline functions, so they cannot be
 * replaced — but they can be *shadowed by function-like macros defined after
 * them*, which is what happens below.  Each shadow inspects its arguments:
 *
 *   - a struct device * that belongs to a platform_device, or an IRQ number
 *     carrying LKPI_PLATFORM_IRQ_TAG  -> handled here, straight to
 *     bus_setup_intr() on the device_t, with the same handler wrapping and
 *     devres registration linuxkpi does;
 *   - anything else                    -> forwarded to lkpi_request_irq()
 *     unchanged, so PCI drivers in the same module are unaffected.
 *
 * The upstream fix is a platform-device lookup path inside linuxkpi's
 * linux_interrupt.c; that is a FreeBSD kernel patch and is not required for
 * an out-of-tree module to work.
 */

#ifndef _BZDOS_LKPI_INTERRUPT_H_
#define	_BZDOS_LKPI_INTERRUPT_H_

/*
 * Pull in the real linuxkpi header first: everything except the request_irq
 * family (tasklets, irqreturn_t, IRQF_*, irq_handler_t) is used as-is.
 */
#include_next <linux/interrupt.h>

#include <linux/platform_device.h>

/* ── Bridge entry points (linux/platform_device.c) ───────────────────── */

int	lkpi_platform_request_irq(struct device *dev, unsigned int irq,
	    irq_handler_t handler, irq_handler_t thread_handler,
	    unsigned long flags, const char *name, void *arg);
void	lkpi_platform_free_irq(struct device *dev, unsigned int irq,
	    void *arg);

/* ── Routing ─────────────────────────────────────────────────────────── */

/*
 * purpose:     Decide whether an interrupt request belongs to the platform
 *              bridge or to linuxkpi's PCI path.
 * input:       dev — struct device * or NULL; irq — Linux IRQ number/token.
 * output:      true if the platform bridge owns it.
 * sideEffects: none
 */
static inline bool
lkpi_platform_owns_irq(struct device *dev, unsigned int irq)
{
	if (LKPI_PLATFORM_IRQ_IS_TOKEN(irq))
		return (true);
	return (dev != NULL && dev_is_platform(dev));
}

/*
 * purpose:     Register an interrupt handler, dispatching to whichever of the
 *              two backends owns the IRQ.
 * input/output/sideEffects: as Linux' request_irq()/devm_request_irq().
 */
static inline int
lkpi_route_request_irq(struct device *dev, unsigned int irq,
    irq_handler_t handler, irq_handler_t thread_handler, unsigned long flags,
    const char *name, void *arg)
{
	if (lkpi_platform_owns_irq(dev, irq))
		return (lkpi_platform_request_irq(dev, irq, handler,
		    thread_handler, flags, name, arg));
	return (lkpi_request_irq(dev, irq, handler, thread_handler, flags,
	    name, arg));
}

/*
 * purpose:     Release an interrupt handler registered above.
 * input/output/sideEffects: as Linux' free_irq()/devm_free_irq().
 */
static inline void
lkpi_route_free_irq(struct device *dev, unsigned int irq, void *arg)
{
	if (lkpi_platform_owns_irq(dev, irq)) {
		lkpi_platform_free_irq(dev, irq, arg);
		return;
	}
	if (dev != NULL)
		lkpi_devm_free_irq(dev, irq, arg);
	else
		lkpi_free_irq(irq, arg);
}

/*
 * Shadow linuxkpi's static inlines.  These must be function-like macros:
 * the originals are functions and cannot be redefined, but a macro with the
 * same name defined afterwards wins at every call site.  The originals stay
 * in the translation unit, unused and harmless.
 */
#define	request_irq(_irq, _h, _f, _n, _a)				\
	lkpi_route_request_irq(NULL, (_irq), (_h), NULL, (_f), (_n), (_a))

#define	request_threaded_irq(_irq, _h, _th, _f, _n, _a)			\
	lkpi_route_request_irq(NULL, (_irq), (_h), (_th), (_f), (_n), (_a))

#define	devm_request_irq(_dev, _irq, _h, _f, _n, _a)			\
	lkpi_route_request_irq((_dev), (_irq), (_h), NULL, (_f), (_n), (_a))

#define	devm_request_threaded_irq(_dev, _irq, _h, _th, _f, _n, _a)	\
	lkpi_route_request_irq((_dev), (_irq), (_h), (_th), (_f), (_n), (_a))

#define	free_irq(_irq, _a)		lkpi_route_free_irq(NULL, (_irq), (_a))
#define	devm_free_irq(_dev, _irq, _a)	lkpi_route_free_irq((_dev), (_irq), (_a))

#endif	/* _BZDOS_LKPI_INTERRUPT_H_ */
