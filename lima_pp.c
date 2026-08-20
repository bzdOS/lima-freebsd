// SPDX-License-Identifier: BSD-2-Clause OR MIT
/*
 * Copyright 2017-2019 Qiang Yu <yuq825@gmail.com>
 * Copyright 2024 bsdOS project (FreeBSD port)
 *
 * MODULE: hal/lima/lima_pp.c
 * PURPOSE: Pixel Processor (PP) init, IRQ handling, task dispatch and reset for Mali-400/450
 * PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_pp.c
 *
 * sema contract:
 *   purpose:     Manage the Mali-400/450 Pixel Processor array — hardware
 *                initialisation, IRQ registration, frame submission, soft/hard
 *                reset, and scheduler pipe wiring.
 *   input:       struct lima_ip pointers for individual PP cores and the PP
 *                broadcast IP; struct lima_device for the parent GPU device.
 *   output:      Initialised PP hardware ready to accept rendering tasks;
 *                scheduler pipe callbacks populated in pipe->task_*.
 *   sideEffects: Registers interrupt handlers via devm_request_irq (LinuxKPI);
 *                modifies MMIO registers on the Mali-400/450 hardware;
 *                allocates a per-device kmem_cache for PP task frames.
 *
 * FreeBSD porting notes:
 *   - drm-66-kmod provides the DRM subsystem through the LinuxKPI layer.
 *     All Linux kernel primitives used here (atomic_*, dev_err/dev_info,
 *     devm_request_irq, kmem_cache_*, IRQ_HANDLED, etc.) are provided by
 *     LinuxKPI headers shipped with drm-66-kmod and the base system.
 *   - No FreeBSD-native kernel headers are needed for this file because
 *     LinuxKPI wraps <linux/interrupt.h>, <linux/io.h>, <linux/device.h>
 *     and <linux/slab.h> with FreeBSD equivalents.
 *   - The writel/readl MMIO accessors map to bus_space_write_4 /
 *     bus_space_read_4 through the LinuxKPI iomap shim.
 *   - lima_poll_timeout() is defined in lima_device.h using
 *     readl_poll_timeout from LinuxKPI.
 *   - IRQF_SHARED maps to RF_SHAREABLE in the LinuxKPI IRQ layer.
 *   - kmem_cache_create_usercopy: LinuxKPI provides kmem_cache_create;
 *     the _usercopy variant is aliased to the plain version because
 *     FreeBSD's slab allocator does not need the copy-region hint.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/slab.h>

#include <drm/lima_drm.h>

#include "lima_device.h"
#include "lima_pp.h"
#include "lima_dlbu.h"
#include "lima_bcast.h"
#include "lima_vm.h"
#include "lima_regs.h"

/* ---------------------------------------------------------------------------
 * MMIO convenience accessors
 *
 * purpose:     Wrap raw writel/readl with the per-IP MMIO base so call sites
 *              only need to name the register offset.
 * input:       ip — pointer to the lima_ip whose iomem base to use
 * sideEffects: pp_write triggers a 32-bit MMIO store; pp_read triggers a load.
 * ------------------------------------------------------------------------- */
#define pp_write(reg, data) writel(data, ip->iomem + (reg))
#define pp_read(reg)        readl(ip->iomem + (reg))

/* ---------------------------------------------------------------------------
 * Internal: lima_pp_handle_irq
 *
 * purpose:     Process a single PP interrupt state word — log errors, mark
 *              the scheduler pipe faulted when an error interrupt fires, mask
 *              further interrupts on error, then acknowledge all pending bits.
 * input:       ip    — the PP core that raised the interrupt
 *              state — value read from LIMA_PP_INT_STATUS
 * output:      none (void)
 * sideEffects: Writes LIMA_PP_INT_MASK (masks interrupts on error) and
 *              LIMA_PP_INT_CLEAR (acknowledges interrupt); sets pipe->error.
 * ------------------------------------------------------------------------- */
static void lima_pp_handle_irq(struct lima_ip *ip, u32 state)
{
	struct lima_device *dev = ip->dev;
	struct lima_sched_pipe *pipe = dev->pipe + lima_pipe_pp;

	if (state & LIMA_PP_IRQ_MASK_ERROR) {
		u32 status = pp_read(LIMA_PP_STATUS);

		dev_err(dev->dev, "pp error irq state=%x status=%x\n",
			state, status);

		pipe->error = true;

		/* mask all interrupts before hard reset */
		pp_write(LIMA_PP_INT_MASK, 0);
	}

	pp_write(LIMA_PP_INT_CLEAR, state);
}

/* ---------------------------------------------------------------------------
 * lima_pp_irq_handler — per-core PP interrupt handler
 *
 * purpose:     Service an interrupt from one PP core: read interrupt status,
 *              delegate to lima_pp_handle_irq, then decrement the pipe task
 *              counter and signal completion when all PP cores are done.
 * input:       irq  — interrupt line number (unused, for shared IRQ probe)
 *              data — pointer to struct lima_ip for this core
 * output:      IRQ_NONE if interrupt register is zero (shared IRQ); else
 *              IRQ_HANDLED.
 * sideEffects: May call lima_sched_pipe_task_done() when all cores complete;
 *              modifies MMIO via lima_pp_handle_irq.
 * ------------------------------------------------------------------------- */
static irqreturn_t lima_pp_irq_handler(int irq, void *data)
{
	struct lima_ip *ip = data;
	struct lima_device *dev = ip->dev;
	struct lima_sched_pipe *pipe = dev->pipe + lima_pipe_pp;
	u32 state = pp_read(LIMA_PP_INT_STATUS);

	/* for shared irq case */
	if (!state)
		return IRQ_NONE;

	lima_pp_handle_irq(ip, state);

	if (atomic_dec_and_test(&pipe->task))
		lima_sched_pipe_task_done(pipe);

	return IRQ_HANDLED;
}

/* ---------------------------------------------------------------------------
 * lima_pp_bcast_irq_handler — broadcast PP interrupt handler (Mali-450)
 *
 * purpose:     Service the broadcast interrupt for a Mali-450 multi-PP setup.
 *              Polls each active PP core for pending interrupts or rendering
 *              completion, handles each, and signals the scheduler when all
 *              cores have finished.
 * input:       irq      — interrupt line (unused for shared IRQ probe guard)
 *              data     — pointer to struct lima_ip for the PP broadcast IP
 * output:      IRQ_NONE if no task is active; IRQ_HANDLED if at least one
 *              core was serviced.
 * sideEffects: Calls lima_pp_handle_irq per active core; may call
 *              lima_sched_pipe_task_done; updates pipe->done bitmask.
 * ------------------------------------------------------------------------- */
static irqreturn_t lima_pp_bcast_irq_handler(int irq, void *data)
{
	int i;
	irqreturn_t ret = IRQ_NONE;
	struct lima_ip *pp_bcast = data;
	struct lima_device *dev = pp_bcast->dev;
	struct lima_sched_pipe *pipe = dev->pipe + lima_pipe_pp;
	struct drm_lima_m450_pp_frame *frame;

	/* for shared irq case */
	if (!pipe->current_task)
		return IRQ_NONE;

	frame = pipe->current_task->frame;

	for (i = 0; i < frame->num_pp; i++) {
		struct lima_ip *ip = pipe->processor[i];
		u32 status, state;

		if (pipe->done & (1 << i))
			continue;

		/*
		 * Read status before int_status to avoid a race where the
		 * rendering active bit clears between the two reads and we
		 * would miss the final interrupt.
		 */
		status = pp_read(LIMA_PP_STATUS);
		state  = pp_read(LIMA_PP_INT_STATUS);

		if (state) {
			lima_pp_handle_irq(ip, state);
			ret = IRQ_HANDLED;
		} else {
			if (status & LIMA_PP_STATUS_RENDERING_ACTIVE)
				continue;
		}

		pipe->done |= (1 << i);
		if (atomic_dec_and_test(&pipe->task))
			lima_sched_pipe_task_done(pipe);
	}

	return ret;
}

/* ---------------------------------------------------------------------------
 * lima_pp_soft_reset_async — begin a non-blocking soft reset
 *
 * purpose:     Initiate a soft reset of the PP core by masking interrupts,
 *              clearing raw interrupt status and asserting SOFT_RESET.  The
 *              reset completion must be polled later with
 *              lima_pp_soft_reset_async_wait_one().
 * input:       ip — the PP core to reset
 * output:      none (void); idempotent if ip->data.async_reset already set.
 * sideEffects: Writes LIMA_PP_INT_MASK, LIMA_PP_INT_RAWSTAT, LIMA_PP_CTRL;
 *              sets ip->data.async_reset = true.
 * ------------------------------------------------------------------------- */
static void lima_pp_soft_reset_async(struct lima_ip *ip)
{
	if (ip->data.async_reset)
		return;

	pp_write(LIMA_PP_INT_MASK, 0);
	pp_write(LIMA_PP_INT_RAWSTAT, LIMA_PP_IRQ_MASK_ALL);
	pp_write(LIMA_PP_CTRL, LIMA_PP_CTRL_SOFT_RESET);
	ip->data.async_reset = true;
}

/* ---------------------------------------------------------------------------
 * lima_pp_soft_reset_poll — predicate for reset-completion polling
 *
 * purpose:     Return non-zero when the PP soft reset has completed — i.e.
 *              rendering is no longer active and the reset-completed raw
 *              interrupt is asserted.
 * input:       ip — the PP core being polled
 * output:      non-zero (true) when reset complete; 0 while still resetting.
 * sideEffects: Two MMIO reads; no writes.
 * ------------------------------------------------------------------------- */
static int lima_pp_soft_reset_poll(struct lima_ip *ip)
{
	return !(pp_read(LIMA_PP_STATUS) & LIMA_PP_STATUS_RENDERING_ACTIVE) &&
		pp_read(LIMA_PP_INT_RAWSTAT) == LIMA_PP_IRQ_RESET_COMPLETED;
}

/* ---------------------------------------------------------------------------
 * lima_pp_soft_reset_async_wait_one — wait for one PP soft reset to complete
 *
 * purpose:     Block (with timeout) until the PP soft reset initiated by
 *              lima_pp_soft_reset_async() finishes, then re-arm interrupts.
 * input:       ip — the PP core whose reset to await
 * output:      0 on success; -ETIMEDOUT if reset does not complete within
 *              100 us.
 * sideEffects: Writes LIMA_PP_INT_CLEAR and LIMA_PP_INT_MASK on success.
 *              Logs error on timeout via dev_err.
 * ------------------------------------------------------------------------- */
static int lima_pp_soft_reset_async_wait_one(struct lima_ip *ip)
{
	struct lima_device *dev = ip->dev;
	int ret;

	ret = lima_poll_timeout(ip, lima_pp_soft_reset_poll, 0, 100);
	if (ret) {
		dev_err(dev->dev, "pp %s reset time out\n", lima_ip_name(ip));
		return ret;
	}

	pp_write(LIMA_PP_INT_CLEAR, LIMA_PP_IRQ_MASK_ALL);
	pp_write(LIMA_PP_INT_MASK, LIMA_PP_IRQ_MASK_USED);
	return 0;
}

/* ---------------------------------------------------------------------------
 * lima_pp_soft_reset_async_wait — wait for all pending PP soft resets
 *
 * purpose:     Wait for the previously started async soft reset on a single
 *              PP core or, for the broadcast IP, on all active PP cores in
 *              the current frame.  Clears ip->data.async_reset on return.
 * input:       ip — the PP core (or broadcast IP) whose reset to await
 * output:      0 if all cores reset successfully; non-zero on any timeout.
 * sideEffects: Calls lima_pp_soft_reset_async_wait_one per core; clears
 *              ip->data.async_reset.
 * ------------------------------------------------------------------------- */
static int lima_pp_soft_reset_async_wait(struct lima_ip *ip)
{
	int i, err = 0;

	if (!ip->data.async_reset)
		return 0;

	if (ip->id == lima_ip_pp_bcast) {
		struct lima_device *dev = ip->dev;
		struct lima_sched_pipe *pipe = dev->pipe + lima_pipe_pp;
		struct drm_lima_m450_pp_frame *frame = pipe->current_task->frame;

		for (i = 0; i < frame->num_pp; i++)
			err |= lima_pp_soft_reset_async_wait_one(pipe->processor[i]);
	} else {
		err = lima_pp_soft_reset_async_wait_one(ip);
	}

	ip->data.async_reset = false;
	return err;
}

/* ---------------------------------------------------------------------------
 * lima_pp_write_frame — upload frame and write-back descriptors to PP MMIO
 *
 * purpose:     Bulk-write the frame register array (LIMA_PP_FRAME region) and
 *              all three write-back object register sets into the PP MMIO.
 * input:       ip    — the PP core to program
 *              frame — pointer to LIMA_PP_FRAME_REG_NUM u32 frame words
 *              wb    — pointer to 3 x LIMA_PP_WB_REG_NUM u32 wb words
 * output:      none (void)
 * sideEffects: (LIMA_PP_FRAME_REG_NUM + 3xLIMA_PP_WB_REG_NUM) MMIO writes.
 * ------------------------------------------------------------------------- */
static void lima_pp_write_frame(struct lima_ip *ip, u32 *frame, u32 *wb)
{
	int i, j, n = 0;

	for (i = 0; i < LIMA_PP_FRAME_REG_NUM; i++)
		writel(frame[i], ip->iomem + LIMA_PP_FRAME + i * 4);

	for (i = 0; i < 3; i++) {
		for (j = 0; j < LIMA_PP_WB_REG_NUM; j++)
			writel(wb[n++], ip->iomem + LIMA_PP_WB(i) + j * 4);
	}
}

/* ---------------------------------------------------------------------------
 * lima_pp_hard_reset_poll — predicate for hard-reset completion
 *
 * purpose:     Write a sentinel value to LIMA_PP_PERF_CNT_0_LIMIT and read
 *              it back; a successful readback means the PP bus is alive after
 *              hard reset.
 * input:       ip — the PP core being polled
 * output:      non-zero (true) when sentinel readback matches; 0 otherwise.
 * sideEffects: One MMIO write and one MMIO read to LIMA_PP_PERF_CNT_0_LIMIT.
 * ------------------------------------------------------------------------- */
static int lima_pp_hard_reset_poll(struct lima_ip *ip)
{
	pp_write(LIMA_PP_PERF_CNT_0_LIMIT, 0xC01A0000);
	return pp_read(LIMA_PP_PERF_CNT_0_LIMIT) == 0xC01A0000;
}

/* ---------------------------------------------------------------------------
 * lima_pp_hard_reset — force-reset a PP core
 *
 * purpose:     Assert LIMA_PP_CTRL_FORCE_RESET and poll until the PP hardware
 *              acknowledges via the sentinel readback mechanism; then clear
 *              interrupts and restore the interrupt mask.
 * input:       ip — the PP core to hard-reset
 * output:      0 on success; -ETIMEDOUT if the reset poll exceeds 100 us.
 * sideEffects: Multiple MMIO writes; dev_err on timeout.
 * ------------------------------------------------------------------------- */
static int lima_pp_hard_reset(struct lima_ip *ip)
{
	struct lima_device *dev = ip->dev;
	int ret;

	pp_write(LIMA_PP_PERF_CNT_0_LIMIT, 0xC0FFE000);
	pp_write(LIMA_PP_INT_MASK, 0);
	pp_write(LIMA_PP_CTRL, LIMA_PP_CTRL_FORCE_RESET);
	ret = lima_poll_timeout(ip, lima_pp_hard_reset_poll, 10, 100);
	if (ret) {
		dev_err(dev->dev, "pp hard reset timeout\n");
		return ret;
	}

	pp_write(LIMA_PP_PERF_CNT_0_LIMIT, 0);
	pp_write(LIMA_PP_INT_CLEAR, LIMA_PP_IRQ_MASK_ALL);
	pp_write(LIMA_PP_INT_MASK, LIMA_PP_IRQ_MASK_USED);
	return 0;
}

/* ---------------------------------------------------------------------------
 * lima_pp_print_version — log PP hardware version to dmesg
 *
 * purpose:     Read LIMA_PP_VERSION register, decode the Mali model name and
 *              major/minor revision, and emit a dev_info message.
 * input:       ip — the PP core whose version to read
 * output:      none (void); side-output to dmesg.
 * sideEffects: One MMIO read; one dev_info call.
 * ------------------------------------------------------------------------- */
static void lima_pp_print_version(struct lima_ip *ip)
{
	u32 version, major, minor;
	const char *name;

	version = pp_read(LIMA_PP_VERSION);
	major   = (version >> 8) & 0xFF;
	minor   = version & 0xFF;

	switch (version >> 16) {
	case 0xC807:
		name = "mali200";
		break;
	case 0xCE07:
		name = "mali300";
		break;
	case 0xCD07:
		name = "mali400";
		break;
	case 0xCF07:
		name = "mali450";
		break;
	default:
		name = "unknown";
		break;
	}

	dev_info(ip->dev->dev, "%s - %s version major %d minor %d\n",
		 lima_ip_name(ip), name, major, minor);
}

/* ---------------------------------------------------------------------------
 * lima_pp_hw_init — software-initialise PP hardware after power-on or resume
 *
 * purpose:     Clear the async_reset flag (hardware is not yet in reset),
 *              kick off an async soft reset, and wait for it to complete.
 * input:       ip — the PP core to initialise
 * output:      0 on success; non-zero on reset timeout.
 * sideEffects: Modifies ip->data.async_reset; writes PP MMIO via
 *              lima_pp_soft_reset_async / lima_pp_soft_reset_async_wait.
 * ------------------------------------------------------------------------- */
static int lima_pp_hw_init(struct lima_ip *ip)
{
	ip->data.async_reset = false;
	lima_pp_soft_reset_async(ip);
	return lima_pp_soft_reset_async_wait(ip);
}

/* ---------------------------------------------------------------------------
 * Public: lima_pp_resume
 *
 * purpose:     Re-initialise PP hardware after a power-management suspend.
 * input:       ip — the PP core to resume
 * output:      0 on success; non-zero on reset timeout.
 * sideEffects: See lima_pp_hw_init.
 * ------------------------------------------------------------------------- */
int lima_pp_resume(struct lima_ip *ip)
{
	return lima_pp_hw_init(ip);
}

/* ---------------------------------------------------------------------------
 * Public: lima_pp_suspend
 *
 * purpose:     Prepare PP hardware for power-management suspend.  Currently
 *              the PP does not require explicit quiescing beyond what the
 *              scheduler ensures before calling suspend.
 * input:       ip — the PP core to suspend
 * output:      none (void)
 * sideEffects: none
 * ------------------------------------------------------------------------- */
void lima_pp_suspend(struct lima_ip *ip)
{
	/* Nothing to do: scheduler ensures the PP is idle before suspend. */
}

/* ---------------------------------------------------------------------------
 * Public: lima_pp_init — initialise a single PP core
 *
 * purpose:     Print hardware version, run soft reset, and register the per-
 *              core IRQ handler.  Store the version register value in
 *              dev->pp_version for later use by upper layers.
 * input:       ip — the PP core to initialise
 * output:      0 on success; non-zero on HW init failure or IRQ registration
 *              failure.
 * sideEffects: Registers interrupt handler via devm_request_irq (LinuxKPI);
 *              writes dev->pp_version; emits dmesg via lima_pp_print_version.
 * ------------------------------------------------------------------------- */
int lima_pp_init(struct lima_ip *ip)
{
	struct lima_device *dev = ip->dev;
	int err;

	lima_pp_print_version(ip);

	err = lima_pp_hw_init(ip);
	if (err)
		return err;

	err = devm_request_irq(dev->dev, ip->irq, lima_pp_irq_handler,
			       IRQF_SHARED, lima_ip_name(ip), ip);
	if (err) {
		dev_err(dev->dev, "pp %s fail to request irq\n",
			lima_ip_name(ip));
		return err;
	}

	dev->pp_version = pp_read(LIMA_PP_VERSION);

	return 0;
}

/* ---------------------------------------------------------------------------
 * Public: lima_pp_fini — tear down a single PP core
 *
 * purpose:     Release per-core PP resources on driver unload.  IRQ is
 *              released automatically by devm_ bookkeeping.
 * input:       ip — the PP core to tear down
 * output:      none (void)
 * sideEffects: none (devm_ handles IRQ release)
 * ------------------------------------------------------------------------- */
void lima_pp_fini(struct lima_ip *ip)
{
	/* devm_request_irq cleanup is handled by the device managed resource
	 * framework when the parent device is released. */
}

/* ---------------------------------------------------------------------------
 * Public: lima_pp_bcast_resume — resume the PP broadcast IP (Mali-450)
 *
 * purpose:     Mark the broadcast IP's async_reset flag as cleared after
 *              resume — individual PP resumes have already reset the hardware.
 * input:       ip — the PP broadcast IP
 * output:      0 always
 * sideEffects: Clears ip->data.async_reset.
 * ------------------------------------------------------------------------- */
int lima_pp_bcast_resume(struct lima_ip *ip)
{
	/* PP has been reset by individual PP resume */
	ip->data.async_reset = false;
	return 0;
}

/* ---------------------------------------------------------------------------
 * Public: lima_pp_bcast_suspend — suspend the PP broadcast IP (Mali-450)
 *
 * purpose:     Prepare the PP broadcast IP for power-management suspend.
 *              No explicit action required beyond scheduler-level quiescing.
 * input:       ip — the PP broadcast IP
 * output:      none (void)
 * sideEffects: none
 * ------------------------------------------------------------------------- */
void lima_pp_bcast_suspend(struct lima_ip *ip)
{
	/* Nothing to do: scheduler ensures the PP is idle before suspend. */
}

/* ---------------------------------------------------------------------------
 * Public: lima_pp_bcast_init — initialise the PP broadcast IP (Mali-450)
 *
 * purpose:     Register the broadcast IRQ handler that services all PP cores
 *              via a single interrupt line on Mali-450 hardware.
 * input:       ip — the PP broadcast IP to initialise
 * output:      0 on success; non-zero on IRQ registration failure.
 * sideEffects: Registers interrupt handler via devm_request_irq (LinuxKPI).
 * ------------------------------------------------------------------------- */
int lima_pp_bcast_init(struct lima_ip *ip)
{
	struct lima_device *dev = ip->dev;
	int err;

	err = devm_request_irq(dev->dev, ip->irq, lima_pp_bcast_irq_handler,
			       IRQF_SHARED, lima_ip_name(ip), ip);
	if (err) {
		dev_err(dev->dev, "pp %s fail to request irq\n",
			lima_ip_name(ip));
		return err;
	}

	return 0;
}

/* ---------------------------------------------------------------------------
 * Public: lima_pp_bcast_fini — tear down the PP broadcast IP (Mali-450)
 *
 * purpose:     Release broadcast-IP resources on driver unload.
 * input:       ip — the PP broadcast IP
 * output:      none (void)
 * sideEffects: none (devm_ handles IRQ release)
 * ------------------------------------------------------------------------- */
void lima_pp_bcast_fini(struct lima_ip *ip)
{
	/* devm_request_irq cleanup is handled automatically. */
}

/* ---------------------------------------------------------------------------
 * Scheduler pipe callbacks
 * ------------------------------------------------------------------------- */

/* lima_pp_task_validate
 *
 * purpose:     Validate a submitted PP task frame before hardware submission —
 *              check that num_pp is non-zero and does not exceed the number of
 *              physical PP cores; reject frames with non-zero padding.
 * input:       pipe — the PP scheduler pipe
 *              task — the task whose frame to validate
 * output:      0 if valid; -EINVAL if frame is malformed.
 * sideEffects: none
 */
static int lima_pp_task_validate(struct lima_sched_pipe *pipe,
				 struct lima_sched_task *task)
{
	u32 num_pp;

	if (pipe->bcast_processor) {
		struct drm_lima_m450_pp_frame *f = task->frame;

		num_pp = f->num_pp;

		if (f->_pad)
			return -EINVAL;
	} else {
		struct drm_lima_m400_pp_frame *f = task->frame;

		num_pp = f->num_pp;
	}

	if (num_pp == 0 || num_pp > pipe->num_processor)
		return -EINVAL;

	return 0;
}

/* lima_pp_task_run
 *
 * purpose:     Submit a PP rendering task to hardware.  For Mali-450 with the
 *              broadcast IP, programs shared frame registers once and uses the
 *              DLBU or per-PP PLBU arrays; for Mali-400 programs each core
 *              individually.
 * input:       pipe — the PP scheduler pipe
 *              task — the task to submit (contains frame descriptor)
 * output:      none (void)
 * sideEffects: Writes extensive PP MMIO; enables/disables DLBU and bcast
 *              hardware; triggers rendering by writing LIMA_PP_CTRL_START_RENDERING.
 */
static void lima_pp_task_run(struct lima_sched_pipe *pipe,
			     struct lima_sched_task *task)
{
	if (pipe->bcast_processor) {
		struct drm_lima_m450_pp_frame *frame = task->frame;
		struct lima_device *dev = pipe->bcast_processor->dev;
		struct lima_ip *ip = pipe->bcast_processor;
		int i;

		pipe->done = 0;
		atomic_set(&pipe->task, frame->num_pp);

		if (frame->use_dlbu) {
			lima_dlbu_enable(dev, frame->num_pp);

			frame->frame[LIMA_PP_FRAME >> 2] = LIMA_VA_RESERVE_DLBU;
			lima_dlbu_set_reg(dev->ip + lima_ip_dlbu, frame->dlbu_regs);
		} else {
			lima_dlbu_disable(dev);
		}

		lima_bcast_enable(dev, frame->num_pp);

		lima_pp_soft_reset_async_wait(ip);

		lima_pp_write_frame(ip, frame->frame, frame->wb);

		for (i = 0; i < frame->num_pp; i++) {
			struct lima_ip *ip = pipe->processor[i];

			pp_write(LIMA_PP_STACK, frame->fragment_stack_address[i]);
			if (!frame->use_dlbu)
				pp_write(LIMA_PP_FRAME, frame->plbu_array_address[i]);
		}

		pp_write(LIMA_PP_CTRL, LIMA_PP_CTRL_START_RENDERING);
	} else {
		struct drm_lima_m400_pp_frame *frame = task->frame;
		int i;

		atomic_set(&pipe->task, frame->num_pp);

		for (i = 0; i < frame->num_pp; i++) {
			struct lima_ip *ip = pipe->processor[i];

			frame->frame[LIMA_PP_FRAME >> 2] =
				frame->plbu_array_address[i];
			frame->frame[LIMA_PP_STACK >> 2] =
				frame->fragment_stack_address[i];

			lima_pp_soft_reset_async_wait(ip);

			lima_pp_write_frame(ip, frame->frame, frame->wb);

			pp_write(LIMA_PP_CTRL, LIMA_PP_CTRL_START_RENDERING);
		}
	}
}

/* lima_pp_task_fini
 *
 * purpose:     Post-task cleanup: begin an async soft reset on all active PP
 *              cores so they are ready for the next task without blocking the
 *              scheduler thread.
 * input:       pipe — the PP scheduler pipe
 * output:      none (void)
 * sideEffects: Calls lima_pp_soft_reset_async on broadcast IP or each
 *              individual PP processor; sets ip->data.async_reset.
 */
static void lima_pp_task_fini(struct lima_sched_pipe *pipe)
{
	if (pipe->bcast_processor) {
		lima_pp_soft_reset_async(pipe->bcast_processor);
	} else {
		int i;

		for (i = 0; i < pipe->num_processor; i++)
			lima_pp_soft_reset_async(pipe->processor[i]);
	}
}

/* lima_pp_task_error
 *
 * purpose:     Error recovery path: log interrupt and status registers for
 *              each PP core then hard-reset all cores.
 * input:       pipe — the PP scheduler pipe
 * output:      none (void)
 * sideEffects: Emits dev_err per core; calls lima_pp_hard_reset per core.
 */
static void lima_pp_task_error(struct lima_sched_pipe *pipe)
{
	int i;

	for (i = 0; i < pipe->num_processor; i++) {
		struct lima_ip *ip = pipe->processor[i];

		dev_err(ip->dev->dev,
			"pp task error %d int_state=%x status=%x\n",
			i, pp_read(LIMA_PP_INT_STATUS), pp_read(LIMA_PP_STATUS));

		lima_pp_hard_reset(ip);
	}
}

/* lima_pp_task_mmu_error
 *
 * purpose:     Handle an MMU page-fault interrupt for the PP pipe: decrement
 *              the task counter and signal completion so the scheduler can
 *              clean up.
 * input:       pipe — the PP scheduler pipe
 * output:      none (void)
 * sideEffects: May call lima_sched_pipe_task_done.
 */
static void lima_pp_task_mmu_error(struct lima_sched_pipe *pipe)
{
	if (atomic_dec_and_test(&pipe->task))
		lima_sched_pipe_task_done(pipe);
}

/* ---------------------------------------------------------------------------
 * PP task slab cache — shared across all lima_device instances on the system.
 *
 * FreeBSD / LinuxKPI note: kmem_cache_create_usercopy is not available in
 * LinuxKPI; the plain kmem_cache_create is used instead.  The copy-region
 * hint (sizeof(struct lima_sched_task), frame_size) is a Linux HARDENED /
 * usercopy whitelist feature; FreeBSD's uma(9) does not require it.
 * ------------------------------------------------------------------------- */
static struct kmem_cache *lima_pp_task_slab;
static int lima_pp_task_slab_refcnt;

/* ---------------------------------------------------------------------------
 * Public: lima_pp_pipe_init — initialise the PP scheduler pipe
 *
 * purpose:     Create (or share) the task slab cache for PP frame objects,
 *              set the frame size based on GPU model, and wire the scheduler
 *              pipe callbacks.
 * input:       dev — the lima_device to initialise the PP pipe for
 * output:      0 on success; -ENOMEM if slab creation fails.
 * sideEffects: May allocate a new kmem_cache; increments
 *              lima_pp_task_slab_refcnt; populates pipe->task_* function
 *              pointers and pipe->task_slab.
 * ------------------------------------------------------------------------- */
int lima_pp_pipe_init(struct lima_device *dev)
{
	int frame_size;
	struct lima_sched_pipe *pipe = dev->pipe + lima_pipe_pp;

	if (dev->id == lima_gpu_mali400)
		frame_size = sizeof(struct drm_lima_m400_pp_frame);
	else
		frame_size = sizeof(struct drm_lima_m450_pp_frame);

	if (!lima_pp_task_slab) {
		/*
		 * FreeBSD LinuxKPI does not provide kmem_cache_create_usercopy;
		 * use the standard kmem_cache_create instead.  The usercopy
		 * region hint is a Linux-specific HARDENED_USERCOPY feature and
		 * has no equivalent in FreeBSD's uma(9).
		 */
		lima_pp_task_slab = kmem_cache_create(
			"lima_pp_task",
			sizeof(struct lima_sched_task) + frame_size,
			0, SLAB_HWCACHE_ALIGN, NULL);
		if (!lima_pp_task_slab)
			return -ENOMEM;
	}
	lima_pp_task_slab_refcnt++;

	pipe->frame_size = frame_size;
	pipe->task_slab  = lima_pp_task_slab;

	pipe->task_validate  = lima_pp_task_validate;
	pipe->task_run       = lima_pp_task_run;
	pipe->task_fini      = lima_pp_task_fini;
	pipe->task_error     = lima_pp_task_error;
	pipe->task_mmu_error = lima_pp_task_mmu_error;

	return 0;
}

/* ---------------------------------------------------------------------------
 * Public: lima_pp_pipe_fini — tear down the PP scheduler pipe
 *
 * purpose:     Decrement the slab cache reference count and destroy it when
 *              the last lima_device releases the pipe.
 * input:       dev — the lima_device releasing the PP pipe
 * output:      none (void)
 * sideEffects: May call kmem_cache_destroy and null lima_pp_task_slab.
 * ------------------------------------------------------------------------- */
void lima_pp_pipe_fini(struct lima_device *dev)
{
	if (!--lima_pp_task_slab_refcnt) {
		kmem_cache_destroy(lima_pp_task_slab);
		lima_pp_task_slab = NULL;
	}
}
