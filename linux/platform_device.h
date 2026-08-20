/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * linux/platform_device.h — a real LinuxKPI platform_device on FreeBSD newbus/FDT
 *
 * WHY THIS FILE EXISTS
 *
 * FreeBSD's sys/compat/linuxkpi/common/include/linux/platform_device.h is a
 * stub: struct platform_driver has no .probe, and platform_driver_register()
 * is literally
 *
 *	pr_debug("%s: TODO\n", __func__);
 *	return (-ENXIO);
 *
 * so every Linux driver ending in module_platform_driver() fails module init
 * unconditionally and its probe routine is unreachable dead code.  That is
 * true of *any* SoC/DT driver, not just Lima: it is missing FreeBSD
 * infrastructure that sits upstream of the driver being ported.
 *
 * This header (plus linux/platform_device.c and linux/interrupt.h next to it)
 * is that infrastructure.  It shadows the linuxkpi stub via the Makefile's
 * -I${.CURDIR}, the same mechanism the local clk/reset/regulator shims already
 * use, so no FreeBSD kernel patch is needed.  Nothing in it is Lima-specific.
 *
 * MODEL
 *
 * A Linux platform_device is "a device the platform knows about by name/DT
 * node, with MMIO and IRQ resources".  On FreeBSD that is exactly a newbus
 * device_t whose parent bus is simplebus (an FDT node), so the bridge is:
 *
 *	simplebus child (device_t) ── newbus probe/attach ──> struct platform_device
 *	                                                        └─ struct device
 *	                                                             (linuxkpi's own,
 *	                                                              unmodified)
 *
 * Registration is done the way linuxkpi does it for PCI: the driver's
 * module_init() calls into lkpi_platform_driver_register(), which hands a
 * generated driver_t to devclass_add_driver().  Doing it at module_init time
 * rather than with a file-scope DRIVER_MODULE() matters: module_init runs at
 * SI_SUB_OFED_MODINIT (== SI_SUB_ROOT_CONF - 1) whereas DRIVER_MODULE runs at
 * SI_SUB_DRIVERS, so a static DRIVER_MODULE would attach — and call
 * drm_dev_alloc() — *before* drm.ko's own module_init had run when both are
 * loaded in one kldload transaction.
 *
 * WHAT IS DELIBERATELY NOT LINUX-SHAPED
 *
 *   - platform_get_resource() returns FreeBSD's struct resource *, not Linux's
 *     struct resource *.  There is no meaningful way to fake the latter, and
 *     every consumer on FreeBSD wants the former.
 *   - IRQ "numbers" are opaque tokens minted by this layer (see
 *     LKPI_PLATFORM_IRQ_* below), not global interrupt vectors.  They are only
 *     ever passed back to the request_irq() family, which linux/interrupt.h
 *     shadows so the tokens resolve here instead of in linuxkpi's PCI-only
 *     lkpi_pci_find_irq_dev() path.
 *
 * SPDX note: this file is original BSD-2-Clause work; it copies no Linux code.
 */

#ifndef _BZDOS_LKPI_PLATFORM_DEVICE_H_
#define	_BZDOS_LKPI_PLATFORM_DEVICE_H_

/*
 * Claim linuxkpi's own guard as well.  If any include path ever reaches the
 * stub after us it must expand to nothing rather than redefine these structs.
 */
#define	_LINUXKPI_LINUX_PLATFORM_DEVICE_H

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>

#include <sys/bus.h>
#include <sys/rman.h>
#include <machine/resource.h>

/*
 * The kernel-free half: LKPI_PLATFORM_MAX_*, struct of_device_id, the IRQ-token
 * arithmetic and the match-table scan.  Separate file so hal/lima/tests can
 * compile and check exactly the code that ships.
 */
#include "platform_logic.h"

/*
 * Tell hal/lima/lima_freebsd_compat.h (and anything else using the same
 * convention) that struct of_device_id is already defined.
 */
#define	_LINUX_OF_DEVICE_H

/* ── struct device_driver, extended ──────────────────────────────────── */

/*
 * FreeBSD's linuxkpi struct device_driver has no .of_match_table, but every
 * DT driver writes its match table there:
 *
 *	.driver = { .name = "lima", .of_match_table = dt_match },
 *
 * Rather than patch linuxkpi's device.h (out of tree, and it would change a
 * struct the whole kernel shares) we define a *prefix-compatible* superset and
 * static_assert the shared prefix.  A struct lkpi_device_driver * may therefore
 * be handed to anything expecting a struct device_driver *, including
 * struct device's .driver field.  If linuxkpi ever reorders device_driver the
 * assertions below break the build instead of silently mis-aliasing.
 */
struct lkpi_device_driver {
	/* --- must mirror struct device_driver exactly, in order --- */
	const char			*name;
	const struct dev_pm_ops		*pm;
	void				(*shutdown)(struct device *);
	void				(*coredump)(struct device *);
	/* --- FreeBSD extension --- */
	const struct of_device_id	*of_match_table;
};

_Static_assert(offsetof(struct lkpi_device_driver, name) ==
    offsetof(struct device_driver, name),
    "lkpi_device_driver.name must alias device_driver.name");
_Static_assert(offsetof(struct lkpi_device_driver, pm) ==
    offsetof(struct device_driver, pm),
    "lkpi_device_driver.pm must alias device_driver.pm");
_Static_assert(offsetof(struct lkpi_device_driver, shutdown) ==
    offsetof(struct device_driver, shutdown),
    "lkpi_device_driver.shutdown must alias device_driver.shutdown");
_Static_assert(offsetof(struct lkpi_device_driver, coredump) ==
    offsetof(struct device_driver, coredump),
    "lkpi_device_driver.coredump must alias device_driver.coredump");
_Static_assert(sizeof(struct lkpi_device_driver) >=
    sizeof(struct device_driver),
    "lkpi_device_driver must be a superset of device_driver");

/* ── struct platform_device ──────────────────────────────────────────── */

/*
 * The linuxkpi stub's struct platform_device is never touched by the kernel or
 * by drm.ko (nothing outside a driver dereferences one), so this layout is
 * free.  Only the embedded struct device must stay linuxkpi's, because that is
 * what gets handed to drm_dev_alloc(), devres, dma-mapping and friends.
 */
struct platform_device {
	/*
	 * struct device must stay first: newbus allocates a platform_device as
	 * the device softc, and linuxkpi's convention (struct pci_dev does the
	 * same) is that device_get_softc(bsddev) is usable as a
	 * struct device *.  Linux puts name/id first; nothing on FreeBSD cares.
	 */
	struct device		dev;

	const char		*name;
	int			id;
	bool			id_auto;

	/* ---- FreeBSD/newbus back end (no Linux counterpart) ---- */

	/*
	 * Identifies this struct device as the .dev of a platform_device, so
	 * dev_is_platform()/to_platform_device() can be honest instead of
	 * returning false/NULL like the stub does.
	 */
	uint32_t		lkpi_magic;
#define	LKPI_PLATFORM_DEVICE_MAGIC	0x706c6466u	/* "pldf" */

	uint32_t		node;		/* phandle_t of the FDT node */
	const struct of_device_id *of_id;	/* winning match table entry */
	struct lkpi_device_driver *of_driver;	/* driver that claimed us */

	int			nmem;
	int			nirq;
	struct resource		*mem[LKPI_PLATFORM_MAX_MEM];
	struct resource		*irq[LKPI_PLATFORM_MAX_IRQ];
	void			*irq_tag[LKPI_PLATFORM_MAX_IRQ];
	void			*irq_ent[LKPI_PLATFORM_MAX_IRQ];

	struct list_head	lkpi_link;	/* global registry, see .c */
};

/* IRQ tokens: see linux/platform_logic.h for the layout and its rationale. */

/* ── struct platform_driver ──────────────────────────────────────────── */

struct platform_driver {
	int	(*probe)(struct platform_device *);
	void	(*remove)(struct platform_device *);
	int	(*suspend)(struct platform_device *);
	int	(*resume)(struct platform_device *);
	void	(*shutdown)(struct platform_device *);
	struct lkpi_device_driver	driver;

	/* ---- FreeBSD private, filled in by the registration macro ---- */
	driver_t	*bsddriver;
	devclass_t	bsdclass;
	const char	*bsdbus;
	bool		registered;
};

/*
 * Linux 6.11 renamed .remove_new back to .remove.  Accept both spellings so
 * drivers pinned to either kernel vintage build unchanged.
 */
#define	remove_new	remove

/* ── Bridge entry points (linux/platform_device.c) ───────────────────── */

int	lkpi_platform_driver_register(struct platform_driver *pdrv,
	    driver_t *bsddriver, const char *busname);
void	lkpi_platform_driver_unregister(struct platform_driver *pdrv);

int	lkpi_platform_bus_probe(device_t dev, struct platform_driver *pdrv);
int	lkpi_platform_bus_attach(device_t dev, struct platform_driver *pdrv);
int	lkpi_platform_bus_detach(device_t dev, struct platform_driver *pdrv);
int	lkpi_platform_bus_suspend(device_t dev, struct platform_driver *pdrv);
int	lkpi_platform_bus_resume(device_t dev, struct platform_driver *pdrv);

struct resource *lkpi_platform_get_resource(struct platform_device *pdev,
	    int type, unsigned int num);
int	lkpi_platform_get_irq(struct platform_device *pdev, unsigned int num);
int	lkpi_platform_get_irq_byname(struct platform_device *pdev,
	    const char *name);
void	*lkpi_platform_ioremap_resource(struct platform_device *pdev,
	    unsigned int index);

/* Resolve an IRQ token back to its device; used by linux/interrupt.h. */
struct platform_device *lkpi_platform_irq_lookup(unsigned int irq, int *idxp);

/* ── Linux-facing API ────────────────────────────────────────────────── */

/*
 * purpose:     Recover the platform_device owning a linuxkpi struct device.
 * input:       dev — any struct device *, possibly NULL, possibly not ours.
 * output:      owning struct platform_device *, or NULL if dev is not the .dev
 *              of a platform_device built by this bridge.
 * sideEffects: none
 */
static inline struct platform_device *
to_platform_device(struct device *dev)
{
	struct platform_device *pdev;

	if (dev == NULL)
		return (NULL);
	pdev = container_of(dev, struct platform_device, dev);
	if (pdev->lkpi_magic != LKPI_PLATFORM_DEVICE_MAGIC)
		return (NULL);
	return (pdev);
}

/*
 * purpose:     Test whether a struct device is a platform device.
 * input:       dev — any struct device *, possibly NULL.
 * output:      true iff this bridge built it.
 * sideEffects: none
 */
static inline bool
dev_is_platform(struct device *dev)
{
	return (to_platform_device(dev) != NULL);
}

static inline void *
platform_get_drvdata(const struct platform_device *pdev)
{
	return (dev_get_drvdata(&pdev->dev));
}

static inline void
platform_set_drvdata(struct platform_device *pdev, void *data)
{
	dev_set_drvdata(&pdev->dev, data);
}

static inline void *
dev_get_platdata(struct device *dev __unused)
{
	return (NULL);
}

/*
 * purpose:     Fetch the n-th resource of a given type (SYS_RES_MEMORY or
 *              SYS_RES_IRQ) belonging to a platform device.
 * input:       pdev, type — SYS_RES_MEMORY / SYS_RES_IRQ, num — index.
 * output:      FreeBSD struct resource *, or NULL.  NB: NOT Linux' struct
 *              resource; see the header comment.
 * sideEffects: none
 */
static inline struct resource *
platform_get_resource(struct platform_device *pdev, int type, unsigned int num)
{
	return (lkpi_platform_get_resource(pdev, type, num));
}

/*
 * purpose:     Return an IRQ token for the num-th interrupt of pdev.
 * input:       pdev, num — index into the FDT "interrupts" property.
 * output:      opaque non-negative token, or -ENXIO if absent.
 * sideEffects: none
 */
static inline int
platform_get_irq(struct platform_device *pdev, unsigned int num)
{
	return (lkpi_platform_get_irq(pdev, num));
}

static inline int
platform_get_irq_optional(struct platform_device *pdev, unsigned int num)
{
	return (lkpi_platform_get_irq(pdev, num));
}

/*
 * purpose:     Return an IRQ token by FDT "interrupt-names" entry.
 * input:       pdev, name — string from the node's interrupt-names property.
 * output:      opaque non-negative token, or -ENXIO if the name is absent.
 * sideEffects: none
 */
static inline int
platform_get_irq_byname(struct platform_device *pdev, const char *name)
{
	return (lkpi_platform_get_irq_byname(pdev, name));
}

static inline int
platform_get_irq_byname_optional(struct platform_device *pdev,
    const char *name)
{
	return (lkpi_platform_get_irq_byname(pdev, name));
}

/*
 * purpose:     Map the index-th MMIO window of pdev for CPU access.
 * input:       pdev, index — MEM resource index.
 * output:      __iomem cookie usable with readl/writel, or an ERR_PTR().
 * sideEffects: none beyond what bus_alloc_resource_any() already did at
 *              attach; the mapping is released on detach.
 */
static inline void __iomem *
devm_platform_ioremap_resource(struct platform_device *pdev,
    unsigned int index)
{
	return ((void __iomem *)lkpi_platform_ioremap_resource(pdev, index));
}

static inline void __iomem *
devm_platform_get_and_ioremap_resource(struct platform_device *pdev,
    unsigned int index, struct resource **resp)
{
	if (resp != NULL)
		*resp = lkpi_platform_get_resource(pdev, SYS_RES_MEMORY, index);
	return ((void __iomem *)lkpi_platform_ioremap_resource(pdev, index));
}

/*
 * purpose:     Return the .data pointer of the of_device_id entry that matched
 *              this device, which DT drivers use to carry a per-variant tag.
 * input:       dev — the platform device's struct device.
 * output:      match data, or NULL if dev is not a platform device or the
 *              matching entry carried no data.
 * sideEffects: none
 */
static inline const void *
of_device_get_match_data(const struct device *dev)
{
	struct platform_device *pdev;

	pdev = to_platform_device(__DECONST(struct device *, dev));
	if (pdev == NULL || pdev->of_id == NULL)
		return (NULL);
	return (pdev->of_id->data);
}

/*
 * purpose:     Expose the backing FDT node of a device.
 * input:       dev — the platform device's struct device.
 * output:      phandle of the node, or 0 when there is none.
 * sideEffects: none
 */
static inline uint32_t
dev_of_node(struct device *dev)
{
	struct platform_device *pdev;

	pdev = to_platform_device(dev);
	return (pdev == NULL ? 0 : pdev->node);
}

/*
 * Registration.  newbus has already done the discovery work by the time a
 * Linux driver's module_init() runs, but drivers still call this, and it is
 * what returns -ENXIO in the stub.  Here it is a no-op success: the real work
 * happened in lkpi_platform_driver_register() below.
 */
static inline int
platform_driver_register(struct platform_driver *pdrv __unused)
{
	return (0);
}

static inline void
platform_driver_unregister(struct platform_driver *pdrv __unused)
{
}

static inline int
platform_device_register(struct platform_device *pdev __unused)
{
	return (0);
}

static inline void
platform_device_unregister(struct platform_device *pdev __unused)
{
}

/* ── module_platform_driver() ────────────────────────────────────────── */

/*
 * Emits, for one struct platform_driver:
 *   - five thin newbus methods that pass the pdrv explicitly, so the bridge
 *     needs no driver_t -> platform_driver lookup table;
 *   - the driver_t itself, sized so newbus allocates our platform_device as
 *     the device softc;
 *   - module_init/module_exit hooks that add the driver to _bus's devclass.
 *
 * _bus is a devclass name, e.g. "simplebus" for /soc FDT children.
 */
#define	LINUXKPI_PLATFORM_DRIVER_BUS(_name, _pdrv, _bus)		\
									\
static int								\
_name##_lkpi_bus_probe(device_t _dev)					\
{									\
	return (lkpi_platform_bus_probe(_dev, &(_pdrv)));		\
}									\
									\
static int								\
_name##_lkpi_bus_attach(device_t _dev)					\
{									\
	return (lkpi_platform_bus_attach(_dev, &(_pdrv)));		\
}									\
									\
static int								\
_name##_lkpi_bus_detach(device_t _dev)					\
{									\
	return (lkpi_platform_bus_detach(_dev, &(_pdrv)));		\
}									\
									\
static int								\
_name##_lkpi_bus_suspend(device_t _dev)					\
{									\
	return (lkpi_platform_bus_suspend(_dev, &(_pdrv)));		\
}									\
									\
static int								\
_name##_lkpi_bus_resume(device_t _dev)					\
{									\
	return (lkpi_platform_bus_resume(_dev, &(_pdrv)));		\
}									\
									\
static device_method_t _name##_lkpi_bus_methods[] = {			\
	DEVMETHOD(device_probe,		_name##_lkpi_bus_probe),	\
	DEVMETHOD(device_attach,	_name##_lkpi_bus_attach),	\
	DEVMETHOD(device_detach,	_name##_lkpi_bus_detach),	\
	DEVMETHOD(device_suspend,	_name##_lkpi_bus_suspend),	\
	DEVMETHOD(device_resume,	_name##_lkpi_bus_resume),	\
	DEVMETHOD_END							\
};									\
									\
static driver_t _name##_lkpi_bus_driver = {				\
	#_name,								\
	_name##_lkpi_bus_methods,					\
	sizeof(struct platform_device)					\
};									\
									\
static int __init							\
_name##_lkpi_modinit(void)						\
{									\
	return (lkpi_platform_driver_register(&(_pdrv),			\
	    &_name##_lkpi_bus_driver, (_bus)));				\
}									\
									\
static void __exit							\
_name##_lkpi_modexit(void)						\
{									\
	lkpi_platform_driver_unregister(&(_pdrv));			\
}									\
									\
module_init(_name##_lkpi_modinit);					\
module_exit(_name##_lkpi_modexit)

#define	module_platform_driver(_pdrv)					\
	LINUXKPI_PLATFORM_DRIVER_BUS(_pdrv, _pdrv, "simplebus")

#endif	/* _BZDOS_LKPI_PLATFORM_DEVICE_H_ */
