/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * linux/platform_device.c — FreeBSD newbus/FDT <-> LinuxKPI platform_device
 *
 * See linux/platform_device.h for why this layer exists and what it promises.
 * This file is the implementation: newbus probe/attach/detach that builds a
 * struct platform_device from a simplebus (FDT) child, resource and IRQ
 * plumbing, and the DMA-tag setup linuxkpi only does for PCI devices.
 *
 * Nothing here is Lima-specific.  It lives under hal/lima only because that is
 * the module that first needed it; the intended destination is linuxkpi
 * proper, at which point the -I${.CURDIR} shadow and this file both disappear.
 *
 * NOT PROVEN: as of the commit that introduced it, this code compiles under
 * -Werror and links into an aarch64 lima.ko.  It has never been loaded and
 * never attached to anything.  Every claim below about runtime behaviour is a
 * claim about intent, not about an observation.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/pctrie.h>
#include <sys/proc.h>
#include <sys/rman.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <linux/kernel.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/slab.h>

#include <linux/platform_device.h>
#include <linux/interrupt.h>

/* ── Global registry of live platform devices ────────────────────────── */

/*
 * Only needed so that a bare request_irq(irq, ...) — which carries no
 * struct device * — can be resolved back to the device that minted the token.
 * Kept deliberately tiny; a list is right for the handful of DT devices a
 * single module ever owns.
 */
static struct mtx lkpi_platform_mtx;
MTX_SYSINIT(lkpi_platform_mtx, &lkpi_platform_mtx, "lkpi platform devices",
    MTX_DEF);
static LINUX_LIST_HEAD(lkpi_platform_devices);

/*
 * purpose:     Publish a freshly attached platform device for token lookup.
 * input:       pdev — attached device, fully initialised.
 * output:      none
 * sideEffects: links pdev onto the module-global registry.
 */
static void
lkpi_platform_publish(struct platform_device *pdev)
{
	mtx_lock(&lkpi_platform_mtx);
	list_add(&pdev->lkpi_link, &lkpi_platform_devices);
	mtx_unlock(&lkpi_platform_mtx);
}

/*
 * purpose:     Withdraw a detaching platform device from the registry.
 * input:       pdev — device being detached.
 * output:      none
 * sideEffects: unlinks pdev; safe to call on a device never published.
 */
static void
lkpi_platform_unpublish(struct platform_device *pdev)
{
	mtx_lock(&lkpi_platform_mtx);
	if (pdev->lkpi_link.next != NULL)
		list_del_init(&pdev->lkpi_link);
	mtx_unlock(&lkpi_platform_mtx);
}

/*
 * purpose:     Resolve an IRQ token to the platform device that minted it.
 * input:       irq — token from platform_get_irq(); idxp — optional out param
 *              receiving the resource index encoded in the token.
 * output:      owning struct platform_device *, or NULL.
 * sideEffects: none
 */
struct platform_device *
lkpi_platform_irq_lookup(unsigned int irq, int *idxp)
{
	struct platform_device *pdev, *found;
	int idx;

	if (!LKPI_PLATFORM_IRQ_IS_TOKEN(irq))
		return (NULL);
	idx = LKPI_PLATFORM_IRQ_INDEX(irq);
	found = NULL;

	mtx_lock(&lkpi_platform_mtx);
	list_for_each_entry(pdev, &lkpi_platform_devices, lkpi_link) {
		if (LKPI_PLATFORM_IRQ_TOKEN(pdev->id, idx) == irq) {
			found = pdev;
			break;
		}
	}
	mtx_unlock(&lkpi_platform_mtx);

	if (found != NULL && idxp != NULL)
		*idxp = idx;
	return (found);
}

/* ── DMA tag setup for a non-PCI struct device ───────────────────────── */

/*
 * linuxkpi's linux_dma_tag_init()/linux_dma_tag_init_coherent() are exported
 * and work on any struct device with a valid ->bsddev, but they expect
 * dev->dma_priv to already point at an allocated struct linux_dma_priv — and
 * that struct is private to sys/compat/linuxkpi/common/src/linux_pci.c, with no
 * exported allocator.  A platform device therefore has no DMA tags at all
 * unless this layer provides them, which means dma_map_sgtable(),
 * dma_alloc_coherent() and every GEM binding path NULL-deref.
 *
 * The layout below mirrors that private struct.  Mirroring a private layout is
 * normally unacceptable, so it is done under a signature check that makes a
 * mismatch loud and harmless instead of silent and fatal:
 *
 *   1. the block is allocated M_ZERO and four times oversized, so even a
 *      badly wrong guess writes only inside our own allocation;
 *   2. NOTHING is written through the mirror before validation — in particular
 *      the mutex is initialised only afterwards, so a layout mismatch cannot
 *      scribble on a field linuxkpi cares about;
 *   3. after calling both tag initialisers we read back all four fields they
 *      set.  linux_dma_tag_init() assigns priv->dma_mask = dma_mask and
 *      priv->dmat = <new tag>; if both of those, and their coherent
 *      counterparts, read correctly at the mirrored offsets, the prefix layout
 *      is confirmed and ->lock therefore sits where linuxkpi expects it.
 *
 * The upstream fix is a one-line export — linux_dma_priv_init(struct device *)
 * — in linux_pci.c.  Until then this is the only way an out-of-tree module can
 * give a platform device working DMA without patching the kernel.
 */
struct lkpi_dma_priv_mirror {
	uint64_t	dma_mask;
	bus_dma_tag_t	dmat;
	uint64_t	dma_coherent_mask;
	bus_dma_tag_t	dmat_coherent;
	struct mtx	lock;
	struct pctrie	ptree;
};

/*
 * purpose:     Give a platform device the DMA tags linuxkpi's dma-mapping API
 *              needs, validating the mirrored private layout before trusting
 *              it.
 * input:       pdev — attached platform device with dev.bsddev set.
 * output:      0 on success; negative errno if tags could not be created or
 *              the layout check failed.  A failure is not fatal to attach:
 *              dev->dma_priv is left NULL and the DMA API keeps reporting
 *              -EIO the way it does for any device without tags.
 * sideEffects: allocates pdev->dev.dma_priv; creates two busdma tags;
 *              initialises the embedded mutex only after validation.
 */
static int
lkpi_platform_dma_init(struct platform_device *pdev)
{
	struct lkpi_dma_priv_mirror *priv;
	struct device *dev;
	int error;

	dev = &pdev->dev;
	priv = malloc(4 * sizeof(*priv), M_DEVBUF, M_WAITOK | M_ZERO);
	dev->dma_priv = priv;

	error = linux_dma_tag_init(dev, DMA_BIT_MASK(32));
	if (error != 0)
		goto fail;
	error = linux_dma_tag_init_coherent(dev, DMA_BIT_MASK(32));
	if (error != 0)
		goto fail;

	if (priv->dma_mask != DMA_BIT_MASK(32) || priv->dmat == NULL ||
	    priv->dma_coherent_mask != DMA_BIT_MASK(32) ||
	    priv->dmat_coherent == NULL) {
		device_printf(dev->bsddev,
		    "linuxkpi struct linux_dma_priv layout has changed; "
		    "refusing to use the mirrored definition. DMA disabled.\n");
		error = -ENXIO;
		/*
		 * Deliberately leak the two busdma tags rather than destroy
		 * them through pointers we have just proven we cannot locate.
		 */
		dev->dma_priv = NULL;
		free(priv, M_DEVBUF);
		return (error);
	}

	/* Layout confirmed: now it is safe to write through the mirror. */
	mtx_init(&priv->lock, "lkpi-priv-dma", NULL, MTX_DEF);
	pctrie_init(&priv->ptree);
	return (0);

fail:
	dev->dma_priv = NULL;
	free(priv, M_DEVBUF);
	return (error);
}

/*
 * purpose:     Release the DMA tags and private block created above.
 * input:       pdev — device being detached.
 * output:      none
 * sideEffects: destroys both busdma tags and the mutex; clears dma_priv.
 */
static void
lkpi_platform_dma_fini(struct platform_device *pdev)
{
	struct lkpi_dma_priv_mirror *priv;

	priv = pdev->dev.dma_priv;
	if (priv == NULL)
		return;
	pdev->dev.dma_priv = NULL;
	if (priv->dmat != NULL)
		bus_dma_tag_destroy(priv->dmat);
	if (priv->dmat_coherent != NULL)
		bus_dma_tag_destroy(priv->dmat_coherent);
	mtx_destroy(&priv->lock);
	free(priv, M_DEVBUF);
}

/* ── FDT matching ────────────────────────────────────────────────────── */

/*
 * purpose:     Walk a Linux of_device_id table against a newbus device's FDT
 *              compatible property.
 * input:       dev — candidate device_t; tbl — NULL-terminated match table.
 * output:      matching entry, or NULL.
 * sideEffects: none
 */
static bool
lkpi_platform_compat_cb(void *ctx, const char *compatible)
{

	return (ofw_bus_is_compatible((device_t)ctx, compatible) != 0);
}

static const struct of_device_id *
lkpi_platform_of_match(device_t dev, const struct of_device_id *tbl)
{

	return (lkpi_platform_of_match_scan(tbl, lkpi_platform_compat_cb, dev));
}

/* ── Resources ───────────────────────────────────────────────────────── */

struct resource *
lkpi_platform_get_resource(struct platform_device *pdev, int type,
    unsigned int num)
{

	if (pdev == NULL)
		return (NULL);
	switch (type) {
	case SYS_RES_MEMORY:
		if (!lkpi_platform_res_index_ok(pdev->nmem, num))
			return (NULL);
		return (pdev->mem[num]);
	case SYS_RES_IRQ:
		if (!lkpi_platform_res_index_ok(pdev->nirq, num))
			return (NULL);
		return (pdev->irq[num]);
	default:
		return (NULL);
	}
}

int
lkpi_platform_get_irq(struct platform_device *pdev, unsigned int num)
{

	if (pdev == NULL || !lkpi_platform_res_index_ok(pdev->nirq, num) ||
	    pdev->irq[num] == NULL)
		return (-ENXIO);
	return ((int)LKPI_PLATFORM_IRQ_TOKEN(pdev->id, num));
}

int
lkpi_platform_get_irq_byname(struct platform_device *pdev, const char *name)
{
	int error, idx;

	if (pdev == NULL || pdev->node == 0 || name == NULL)
		return (-ENXIO);
	error = ofw_bus_find_string_index((phandle_t)pdev->node,
	    "interrupt-names", name, &idx);
	if (error != 0)
		return (-ENXIO);
	return (lkpi_platform_get_irq(pdev, (unsigned int)idx));
}

void *
lkpi_platform_ioremap_resource(struct platform_device *pdev,
    unsigned int index)
{

	if (pdev == NULL || index >= (unsigned int)pdev->nmem ||
	    pdev->mem[index] == NULL)
		return (ERR_PTR(-ENXIO));
	/*
	 * On every FreeBSD platform whose bus_space handle is a kernel virtual
	 * address (all of arm64's nexus/simplebus hierarchy), the handle is
	 * directly what linuxkpi's readl()/writel() dereference.  This is the
	 * same assumption linuxkpi itself makes for PCI BARs in
	 * lkpi_set_pcim_iomap_devres().
	 */
	return ((void *)rman_get_bushandle(pdev->mem[index]));
}

/* ── Interrupts ──────────────────────────────────────────────────────── */

struct lkpi_platform_irqe {
	struct platform_device	*pdev;
	irq_handler_t		handler;
	irq_handler_t		thread_handler;
	void			*arg;
	unsigned int		irq;
	int			idx;
	bool			devres;
};

/*
 * purpose:     newbus ithread trampoline: establish a Linux task context, run
 *              the driver's hardirq handler, then its threaded handler if the
 *              hardirq asked for one.
 * input:       ent — struct lkpi_platform_irqe * registered at setup time.
 * output:      none
 * sideEffects: whatever the driver's handler does.  Mirrors linuxkpi's own
 *              lkpi_irq_handler() exactly, including the M_NOWAIT bail-out.
 */
static void
lkpi_platform_irq_trampoline(void *ent)
{
	struct lkpi_platform_irqe *irqe;

	if (linux_set_current_flags(curthread, M_NOWAIT))
		return;

	irqe = ent;
	if (irqe->handler(irqe->irq, irqe->arg) == IRQ_WAKE_THREAD &&
	    irqe->thread_handler != NULL) {
		THREAD_SLEEPING_OK();
		irqe->thread_handler(irqe->irq, irqe->arg);
		THREAD_NO_SLEEPING();
	}
}

/*
 * purpose:     Tear down one established interrupt.
 * input:       irqe — descriptor whose tag is live.
 * output:      none
 * sideEffects: bus_teardown_intr(); clears the device's tag/ent slots.  Does
 *              NOT release the SYS_RES_IRQ resource: that was allocated at
 *              attach and is released at detach.
 */
static void
lkpi_platform_irq_teardown(struct lkpi_platform_irqe *irqe)
{
	struct platform_device *pdev;
	int idx;

	pdev = irqe->pdev;
	idx = irqe->idx;
	if (pdev->irq_tag[idx] != NULL) {
		bus_teardown_intr(pdev->dev.bsddev, pdev->irq[idx],
		    pdev->irq_tag[idx]);
		pdev->irq_tag[idx] = NULL;
	}
	pdev->irq_ent[idx] = NULL;
}

/*
 * purpose:     devres callback so devm_request_irq() really is automatic.
 * input:       dev — owning device; p — the irqe.
 * output:      none
 * sideEffects: as lkpi_platform_irq_teardown().
 */
static void
lkpi_platform_devm_irq_release(struct device *dev __unused, void *p)
{

	if (p != NULL)
		lkpi_platform_irq_teardown(p);
}

int
lkpi_platform_request_irq(struct device *xdev, unsigned int irq,
    irq_handler_t handler, irq_handler_t thread_handler, unsigned long flags,
    const char *name, void *arg)
{
	struct lkpi_platform_irqe *irqe;
	struct platform_device *pdev;
	int error, idx;

	if (handler == NULL)
		return (-EINVAL);
	if (!LKPI_PLATFORM_IRQ_IS_TOKEN(irq))
		return (-ENXIO);

	pdev = (xdev != NULL) ? to_platform_device(xdev) : NULL;
	if (pdev == NULL)
		pdev = lkpi_platform_irq_lookup(irq, NULL);
	if (pdev == NULL)
		return (-ENXIO);

	idx = LKPI_PLATFORM_IRQ_INDEX(irq);
	if (idx >= pdev->nirq || pdev->irq[idx] == NULL)
		return (-ENXIO);
	/*
	 * One handler per FDT interrupt entry.  Linux' IRQF_SHARED chains
	 * several handlers onto one line; on FreeBSD two IPs that genuinely
	 * share a line get two SYS_RES_IRQ entries and therefore two indices,
	 * so chaining is not needed to express that.
	 */
	if (pdev->irq_tag[idx] != NULL)
		return (-EBUSY);

	if (xdev != NULL)
		irqe = lkpi_devres_alloc(lkpi_platform_devm_irq_release,
		    sizeof(*irqe), GFP_KERNEL | __GFP_ZERO);
	else
		irqe = kzalloc(sizeof(*irqe), GFP_KERNEL);
	if (irqe == NULL)
		return (-ENOMEM);

	irqe->pdev = pdev;
	irqe->handler = handler;
	irqe->thread_handler = thread_handler;
	irqe->arg = arg;
	irqe->irq = irq;
	irqe->idx = idx;
	irqe->devres = (xdev != NULL);

	error = bus_setup_intr(pdev->dev.bsddev, pdev->irq[idx],
	    INTR_TYPE_MISC | INTR_MPSAFE, NULL, lkpi_platform_irq_trampoline,
	    irqe, &pdev->irq_tag[idx]);
	if (error != 0) {
		pdev->irq_tag[idx] = NULL;
		if (irqe->devres)
			lkpi_devres_free(irqe);
		else
			kfree(irqe);
		return (-error);
	}

	pdev->irq_ent[idx] = irqe;
	if (name != NULL)
		bus_describe_intr(pdev->dev.bsddev, pdev->irq[idx],
		    pdev->irq_tag[idx], "%s", name);
	if (irqe->devres)
		lkpi_devres_add(xdev, irqe);

	/*
	 * Linux keeps the "most recent" IRQ on struct device; some helpers read
	 * it back.  Harmless to maintain, and it keeps dev->irq out of its
	 * LINUX_IRQ_INVALID poison state once an interrupt really is live.
	 */
	pdev->dev.irq = irq;
	(void)flags;
	return (0);
}

void
lkpi_platform_free_irq(struct device *xdev, unsigned int irq, void *arg __unused)
{
	struct lkpi_platform_irqe *irqe;
	struct platform_device *pdev;
	int idx;

	if (!LKPI_PLATFORM_IRQ_IS_TOKEN(irq))
		return;
	pdev = (xdev != NULL) ? to_platform_device(xdev) : NULL;
	if (pdev == NULL)
		pdev = lkpi_platform_irq_lookup(irq, NULL);
	if (pdev == NULL)
		return;

	idx = LKPI_PLATFORM_IRQ_INDEX(irq);
	if (idx >= pdev->nirq)
		return;
	irqe = pdev->irq_ent[idx];
	if (irqe == NULL)
		return;

	lkpi_platform_irq_teardown(irqe);
	if (irqe->devres) {
		/*
		 * The block stays on the device's devres list and is freed when
		 * that list is run at detach.  linuxkpi has no devres_release()
		 * (only devres_destroy(), which cannot tell two entries with the
		 * same release function apart, and this driver class registers
		 * several), and lkpi_platform_irq_teardown() is idempotent, so
		 * letting devres free it later is both correct and cheaper than
		 * a match-function dance.
		 */
		return;
	}
	kfree(irqe);
}

/* ── newbus device methods ───────────────────────────────────────────── */

/*
 * purpose:     linuxkpi struct device teardown hook (kobject release path).
 * input:       dev — the platform device's struct device.
 * output:      none
 * sideEffects: runs any remaining devres entries and destroys devres_lock.
 *              Deliberately does NOT free the containing platform_device:
 *              that memory is the newbus softc and is owned by newbus.
 *              Mirrors linuxkpi's lkpi_pci_dev_release().
 */
static void
lkpi_platform_dev_release(struct device *dev)
{

	lkpi_devres_release_free_list(dev);
	spin_lock_destroy(&dev->devres_lock);
}

int
lkpi_platform_bus_probe(device_t dev, struct platform_driver *pdrv)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (lkpi_platform_of_match(dev, pdrv->driver.of_match_table) == NULL)
		return (ENXIO);
	device_set_desc(dev, pdrv->driver.name != NULL ? pdrv->driver.name :
	    device_get_name(dev));
	return (BUS_PROBE_DEFAULT);
}

int
lkpi_platform_bus_attach(device_t dev, struct platform_driver *pdrv)
{
	struct platform_device *pdev;
	int error, i, rid;

	if (pdrv->probe == NULL)
		return (ENXIO);

	pdev = device_get_softc(dev);
	bzero(pdev, sizeof(*pdev));

	/*
	 * Every linuxkpi entry point below (kzalloc, devres, drm_dev_alloc via
	 * the driver's probe) expects a Linux task context on curthread.
	 */
	linux_set_current(curthread);

	pdev->lkpi_magic = LKPI_PLATFORM_DEVICE_MAGIC;
	pdev->name = pdrv->driver.name != NULL ? pdrv->driver.name :
	    device_get_name(dev);
	pdev->id = device_get_unit(dev);
	pdev->node = (uint32_t)ofw_bus_get_node(dev);
	pdev->of_id = lkpi_platform_of_match(dev, pdrv->driver.of_match_table);
	pdev->of_driver = &pdrv->driver;
	INIT_LIST_HEAD(&pdev->lkpi_link);

	/*
	 * MMIO and interrupt resources come from the FDT "reg" and
	 * "interrupts" properties, which simplebus has already turned into
	 * consecutive rids starting at 0.  Allocate greedily and stop at the
	 * first gap: that is what determines nmem/nirq, and therefore what
	 * platform_get_resource()/platform_get_irq() can hand out.
	 */
	for (i = 0; i < LKPI_PLATFORM_MAX_MEM; i++) {
		rid = i;
		pdev->mem[i] = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
		    &rid, RF_ACTIVE);
		if (pdev->mem[i] == NULL)
			break;
		pdev->nmem = i + 1;
	}
	for (i = 0; i < LKPI_PLATFORM_MAX_IRQ; i++) {
		rid = i;
		pdev->irq[i] = bus_alloc_resource_any(dev, SYS_RES_IRQ,
		    &rid, RF_ACTIVE | RF_SHAREABLE);
		if (pdev->irq[i] == NULL)
			break;
		pdev->nirq = i + 1;
	}

	error = kobject_init_and_add(&pdev->dev.kobj, &linux_dev_ktype,
	    &linux_root_device.kobj, device_get_nameunit(dev));
	if (error != 0) {
		device_printf(dev, "kobject_init_and_add failed: %d\n", error);
		error = ENOMEM;
		goto fail_resources;
	}

	pdev->dev.bsddev = dev;
	pdev->dev.parent = &linux_root_device;
	/*
	 * Prefix-compatible by construction; the _Static_asserts in
	 * linux/platform_device.h are what make this cast safe.
	 */
	pdev->dev.driver = (struct device_driver *)&pdrv->driver;
	pdev->dev.release = lkpi_platform_dev_release;
	pdev->dev.irq = LINUX_IRQ_INVALID;
	/*
	 * bsddev_attached_here stays false (bzero above): this device_t belongs
	 * to newbus, and linuxkpi must never try to delete or detach it.
	 */
	spin_lock_init(&pdev->dev.devres_lock);
	INIT_LIST_HEAD(&pdev->dev.devres_head);
	INIT_LIST_HEAD(&pdev->dev.irqents);

	/*
	 * FATAL, and it used to be `(void)`-discarded with a comment claiming a
	 * device with no DMA tags "still works for everything that does not map
	 * memory for the engine".
	 *
	 * That claim is true right up to the first DMA map, and false from then
	 * on -- with one specific and nasty consequence: on failure
	 * lkpi_platform_dma_init() sets dev->dma_priv = NULL, and linuxkpi's
	 * dma-mapping API does NOT null-check it. dma_map_sgtable() takes
	 * DMA_PRIV_LOCK(priv) on a NULL priv, and linux_dma_unmap_sg_attrs()
	 * reaches through priv->dmat. So the documented "graceful -EIO
	 * degradation" is in fact a kernel panic, deferred to whenever something
	 * first tries to map memory for the engine -- i.e. to the first render,
	 * a long way from the attach that caused it, in a stack that names
	 * neither this file nor this decision.
	 *
	 * A device that cannot do DMA cannot drive this GPU, so refusing to
	 * attach is both honest and far cheaper to diagnose than a panic three
	 * layers away. Cleanup mirrors the probe-failure path below.
	 */
	error = lkpi_platform_dma_init(pdev);
	if (error != 0) {
		device_printf(dev, "DMA tag setup failed: %d -- refusing to "
		    "attach rather than leaving a device that panics on its "
		    "first DMA map\n", error);
		lkpi_platform_dma_fini(pdev);
		put_device(&pdev->dev);
		error = (error < 0) ? -error : error;
		goto fail_resources;
	}

	lkpi_platform_publish(pdev);

	error = pdrv->probe(pdev);
	if (error != 0) {
		device_printf(dev, "probe failed: %d\n", error);
		lkpi_platform_unpublish(pdev);
		lkpi_platform_dma_fini(pdev);
		put_device(&pdev->dev);
		error = (error < 0) ? -error : error;
		goto fail_resources;
	}

	return (0);

fail_resources:
	for (i = 0; i < pdev->nirq; i++)
		bus_release_resource(dev, SYS_RES_IRQ,
		    rman_get_rid(pdev->irq[i]), pdev->irq[i]);
	for (i = 0; i < pdev->nmem; i++)
		bus_release_resource(dev, SYS_RES_MEMORY,
		    rman_get_rid(pdev->mem[i]), pdev->mem[i]);
	pdev->lkpi_magic = 0;
	return (error);
}

int
lkpi_platform_bus_detach(device_t dev, struct platform_driver *pdrv)
{
	struct platform_device *pdev;
	int i;

	pdev = device_get_softc(dev);
	if (pdev->lkpi_magic != LKPI_PLATFORM_DEVICE_MAGIC)
		return (0);

	linux_set_current(curthread);

	if (pdrv->remove != NULL)
		pdrv->remove(pdev);

	lkpi_platform_unpublish(pdev);

	/*
	 * Anything the driver established with request_irq() rather than
	 * devm_request_irq() is still live here; devres entries have already
	 * been run by the driver's own teardown or will be by put_device().
	 */
	for (i = 0; i < pdev->nirq; i++) {
		if (pdev->irq_tag[i] != NULL) {
			bus_teardown_intr(dev, pdev->irq[i], pdev->irq_tag[i]);
			pdev->irq_tag[i] = NULL;
		}
	}

	lkpi_platform_dma_fini(pdev);

	/* Drops the last kobject reference -> lkpi_platform_dev_release(). */
	put_device(&pdev->dev);

	for (i = 0; i < pdev->nirq; i++)
		bus_release_resource(dev, SYS_RES_IRQ,
		    rman_get_rid(pdev->irq[i]), pdev->irq[i]);
	for (i = 0; i < pdev->nmem; i++)
		bus_release_resource(dev, SYS_RES_MEMORY,
		    rman_get_rid(pdev->mem[i]), pdev->mem[i]);

	pdev->lkpi_magic = 0;
	return (0);
}

int
lkpi_platform_bus_suspend(device_t dev, struct platform_driver *pdrv)
{
	struct platform_device *pdev;
	int error;

	pdev = device_get_softc(dev);
	if (pdev->lkpi_magic != LKPI_PLATFORM_DEVICE_MAGIC ||
	    pdrv->suspend == NULL)
		return (0);
	linux_set_current(curthread);
	error = pdrv->suspend(pdev);
	return (error < 0 ? -error : error);
}

int
lkpi_platform_bus_resume(device_t dev, struct platform_driver *pdrv)
{
	struct platform_device *pdev;
	int error;

	pdev = device_get_softc(dev);
	if (pdev->lkpi_magic != LKPI_PLATFORM_DEVICE_MAGIC ||
	    pdrv->resume == NULL)
		return (0);
	linux_set_current(curthread);
	error = pdrv->resume(pdev);
	return (error < 0 ? -error : error);
}

/* ── Driver registration ─────────────────────────────────────────────── */

/*
 * purpose:     Attach a Linux platform_driver to a FreeBSD bus devclass, so
 *              newbus discovers matching FDT nodes and calls the probe path
 *              generated by LINUXKPI_PLATFORM_DRIVER_BUS().
 * input:       pdrv — the driver; bsddriver — its generated driver_t;
 *              busname — parent bus devclass, e.g. "simplebus".
 * output:      0 on success, negative errno otherwise.
 * sideEffects: devclass_add_driver() under bus_topo_lock(), which may probe
 *              and attach devices synchronously.  Complains on the console
 *              rather than silently failing, because the caller is a
 *              module_init() whose return value FreeBSD discards.
 */
int
lkpi_platform_driver_register(struct platform_driver *pdrv,
    driver_t *bsddriver, const char *busname)
{
	devclass_t dc;
	int error;

	if (pdrv->probe == NULL) {
		printf("lkpi platform: driver '%s' has no .probe\n",
		    bsddriver->name);
		return (-EINVAL);
	}
	if (pdrv->driver.of_match_table == NULL) {
		printf("lkpi platform: driver '%s' has no .of_match_table\n",
		    bsddriver->name);
		return (-EINVAL);
	}

	dc = devclass_find(busname);
	if (dc == NULL) {
		printf("lkpi platform: no '%s' devclass; '%s' not registered\n",
		    busname, bsddriver->name);
		return (-ENXIO);
	}

	pdrv->bsddriver = bsddriver;
	pdrv->bsdbus = busname;

	bus_topo_lock();
	error = devclass_add_driver(dc, bsddriver, BUS_PASS_DEFAULT,
	    &pdrv->bsdclass);
	bus_topo_unlock();
	if (error != 0) {
		printf("lkpi platform: devclass_add_driver('%s') failed: %d\n",
		    bsddriver->name, error);
		return (-error);
	}

	pdrv->registered = true;
	return (0);
}

/*
 * purpose:     Undo lkpi_platform_driver_register() at module unload.
 * input:       pdrv — previously registered driver.
 * output:      none
 * sideEffects: devclass_delete_driver() under bus_topo_lock(), which detaches
 *              any still-attached devices.
 */
void
lkpi_platform_driver_unregister(struct platform_driver *pdrv)
{
	devclass_t dc;

	if (!pdrv->registered)
		return;
	dc = devclass_find(pdrv->bsdbus);
	if (dc != NULL) {
		bus_topo_lock();
		devclass_delete_driver(dc, pdrv->bsddriver);
		bus_topo_unlock();
	}
	pdrv->registered = false;
}
