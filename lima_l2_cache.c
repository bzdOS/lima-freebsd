// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * lima_l2_cache.c — Mali-400/450 L2 cache control (FreeBSD 15.1)
 *
 * PORTED_FROM: Linux 6.6 drivers/gpu/drm/lima/lima_l2_cache.c
 *
 * ============================================================================
 * HISTORY: BOTH FUNCTIONS HERE WERE STUBS (fixed 2026-08-11)
 * ============================================================================
 * `lima_l2_cache_init()` was `dev_dbg(...); return 0;` and
 * `lima_l2_cache_flush()` was `return 0;`. The file's own contract said
 * "writes L2 cache flush/invalidate registers; no-op stub" — accurate, and the
 * consequences were not small:
 *
 *   - The L2 cache was never ENABLED. Upstream's init writes
 *     LIMA_L2_CACHE_ENABLE_ACCESS | _READ_ALLOCATE and sets MAX_READS; ours
 *     wrote nothing, so those bits stayed at whatever reset left them.
 *   - The flush was a lie. lima_sched.c calls it in exactly the right two
 *     places (`lima_sched.c:505` and `:827`, before/around task execution),
 *     and it did nothing. Combined with lima_vm.c:87 mapping EVERY buffer
 *     `LIMA_VM_FLAGS_CACHE` unconditionally, GPU writes had no guaranteed path
 *     to visibility.
 *
 * That combination does not hang: it produces STALE PIXELS or TORN TILES,
 * silently. Which is worse to debug than a hang, and it would have been
 * discovered as "Mesa renders garbage" long after the fact.
 *
 * This was the seventh stub in this port found to report success while doing
 * nothing (after linux/clk.h, linux/reset.h, linux/regulator/consumer.h,
 * lima_pmu.c, and two gaps in drm-kmod). Every register it needs was already
 * defined in lima_regs.h and never written. Pattern worth remembering: a
 * `#define` for a register is not evidence that anything writes it.
 *
 * NOTE ON WHAT IS STILL NOT FIXED HERE: `lima_vm_map_page()` (lima_vm.c:87)
 * still hardcodes LIMA_VM_FLAGS_CACHE for every BO, and LIMA_VM_FLAGS_UNCACHE
 * is defined but unused. For a scanout buffer written by the PP and read by the
 * display controller, uncached mapping is likely the right answer rather than
 * relying on a flush. Changing that signature touches the path every ordinary
 * buffer uses, so it is deliberately left alone here and documented in
 * `SCANOUT-IMPORT.md`.
 */

#include <linux/device.h>
#include <linux/spinlock.h>

#include "lima_device.h"
#include "lima_l2_cache.h"
#include "lima_regs.h"

#define l2_cache_write(reg, data) \
	writel(data, (uint8_t __iomem *)ip->iomem + (reg))
#define l2_cache_read(reg) \
	readl((uint8_t __iomem *)ip->iomem + (reg))

/*
 * purpose:   Predicate for lima_poll_timeout(): true once the L2 has finished
 *            the command in flight.
 * input:     ip — the L2 cache lima_ip.
 * output:    nonzero when STATUS.COMMAND_BUSY is clear.
 * sideEffects: none (one MMIO read per call).
 */
static int
lima_l2_cache_wait_command(struct lima_ip *ip)
{
	return !(l2_cache_read(LIMA_L2_CACHE_STATUS) &
		 LIMA_L2_CACHE_STATUS_COMMAND_BUSY);
}

/*
 * purpose:   Invalidate the whole L2 cache and wait for completion. Called by
 *            lima_sched.c around task execution so the CPU and the display
 *            controller can see what the GPU wrote.
 * input:     ip — the L2 cache lima_ip.
 * output:    0 on success; -ETIMEDOUT if the command never completes.
 * sideEffects: writes LIMA_L2_CACHE_COMMAND; serialised on ip->data.lock
 *            because lima_sched.c can call this from more than one pipe.
 */
int lima_l2_cache_flush(struct lima_ip *ip)
{
	int err;

	spin_lock(&ip->data.lock);
	l2_cache_write(LIMA_L2_CACHE_COMMAND, LIMA_L2_CACHE_COMMAND_CLEAR_ALL);
	/*
	 * 20 ms, not upstream's 1000 us. Two reasons, and the first is a porting
	 * hazard worth stating: LinuxKPI's ktime_get() is getnanouptime(), the
	 * COARSE clock, which only advances on a tick -- so with hz=1000 a
	 * "1000 us" deadline here is really one to two milliseconds, and no
	 * sub-tick deadline expressed this way means anything at all.
	 *
	 * Even at that, the command genuinely misses the deadline under load:
	 * 41 times across six hours of presenting runs on this board, with the
	 * L2 register base verified correct (init reads back a sensible
	 * 64K/4-way/64-byte-line geometry through the same iomem). And BOTH
	 * callers in lima_sched.c ignore the return value, exactly as upstream
	 * does, so an abandoned flush means the next task runs against an
	 * unflushed L2 -- which shows up as stale pixels or torn tiles, silently,
	 * never as an error. Waiting an order of magnitude longer costs
	 * throughput only in the rare case and removes that risk.
	 */
	err = lima_poll_timeout(ip, lima_l2_cache_wait_command, 0, 20000);
	spin_unlock(&ip->data.lock);

	if (err) {
		/*
		 * Rate-limited to powers of two: this used to print per
		 * occurrence, and a driver that floods the console during heavy
		 * rendering makes every other message in the ring unreadable --
		 * on this board the console ring is also the postmortem log.
		 */
		static unsigned long timeouts;

		timeouts++;
		if ((timeouts & (timeouts - 1)) == 0)
			dev_err(ip->dev->dev,
			    "l2 cache flush timed out (%lu so far)\n",
			    timeouts);
	}
	return err;
}

/*
 * purpose:   Bring the L2 cache up: report its geometry, invalidate it, then
 *            enable access and read-allocate.
 * input:     ip — the L2 cache lima_ip (iomem already offset to its block).
 * output:    0 on success; -ENODEV if this is l2_cache2 on a GPU with no
 *            PP4-7; negative errno from the flush.
 * sideEffects: initialises ip->data.lock; writes LIMA_L2_CACHE_COMMAND,
 *            _ENABLE and _MAX_READS; prints the cache geometry once.
 *
 * The l2_cache2 check mirrors upstream: that block only exists when at least
 * one of PP4-7 is present, which on this board (Mali-400 MP2) it is not — the
 * descriptor table in lima_device.c marks it absent, so this is belt-and-braces
 * rather than the live path.
 */
int lima_l2_cache_init(struct lima_ip *ip)
{
	struct lima_device *dev = ip->dev;
	u32 size;
	int i;

	if (ip->id == lima_ip_l2_cache2) {
		for (i = lima_ip_pp4; i <= lima_ip_pp7; i++) {
			if (dev->ip[i].present)
				break;
		}
		if (i > lima_ip_pp7)
			return -ENODEV;
	}

	spin_lock_init(&ip->data.lock);

	size = l2_cache_read(LIMA_L2_CACHE_SIZE);
	dev_info(dev->dev,
		 "l2 cache %uK, %u-way, %u byte cache line, %u bit external bus\n",
		 1 << (((size >> 16) & 0xff) - 10),
		 1 << ((size >> 8) & 0xff),
		 1 << ((size >> 0) & 0xff),
		 1 << ((size >> 24) & 0xff));

	lima_l2_cache_flush(ip);

	l2_cache_write(LIMA_L2_CACHE_ENABLE,
		       LIMA_L2_CACHE_ENABLE_ACCESS |
		       LIMA_L2_CACHE_ENABLE_READ_ALLOCATE);
	l2_cache_write(LIMA_L2_CACHE_MAX_READS, 0x1c);

	return 0;
}

/*
 * purpose:   Re-initialise after a runtime/system resume.
 * input:     ip — the L2 cache lima_ip.
 * output:    0 on success; negative errno from lima_l2_cache_init().
 * sideEffects: as lima_l2_cache_init().
 */
int lima_l2_cache_resume(struct lima_ip *ip)
{
	return lima_l2_cache_init(ip);
}

/*
 * purpose:   Release L2 resources on driver teardown.
 * input:     ip — the L2 cache lima_ip.
 * output:    none
 * sideEffects: none. Nothing to free: the lock is embedded in ip and the
 *            block is left as the next init will find it.
 */
void lima_l2_cache_fini(struct lima_ip *ip)
{
}

/*
 * purpose:   Quiesce the L2 for suspend.
 * input:     ip — the L2 cache lima_ip.
 * output:    none
 * sideEffects: none. Upstream is empty here too — the PMU powers the block
 *            down, and a flush before that is the caller's job.
 */
void lima_l2_cache_suspend(struct lima_ip *ip)
{
}
