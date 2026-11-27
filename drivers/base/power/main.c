// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/base/power/main.c - Where the driver meets power management.
 *
 * Copyright (c) 2003 Patrick Mochel
 * Copyright (c) 2003 Open Source Development Lab
 *
 * The driver model core calls device_pm_add() when a device is registered.
 * This will initialize the embedded device_pm_info object in the device
 * and add it to the list of power-controlled devices. sysfs entries for
 * controlling device power management will also be added.
 *
 * A separate list is used for keeping track of power info, because the power
 * domain dependencies may differ from the ancestral dependencies that the
 * subsystem list maintains.
 */

#define pr_fmt(fmt) "PM: " fmt
#define dev_fmt pr_fmt

#include <linux/device.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/pm-trace.h>
#include <linux/pm_wakeirq.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/async.h>
#include <linux/suspend.h>
#include <trace/events/power.h>
#include <linux/cpufreq.h>
#include <linux/cpuidle.h>
#include <linux/haydn_bootlog.h>
#include <linux/nvmem-consumer.h>
#include <linux/crc32.h>
#include <linux/of.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/devfreq.h>
#include <linux/timer.h>
#include <linux/wakeup_reason.h>

#include <linux/spmi.h>
#include <linux/kdebug.h>
#include <linux/stacktrace.h>
#include <linux/sched/debug.h>
#include <asm/irq_regs.h>
#include <linux/kobject.h>
#include <asm/sections.h>
#include <linux/delay.h>
#include <linux/of_platform.h>
#include "../base.h"
#include "power.h"

typedef int (*pm_callback_t)(struct device *);

#define list_for_each_entry_rcu_locked(pos, head, member) \
	list_for_each_entry_rcu(pos, head, member, \
			device_links_read_lock_held())

/*
 * The entries in the dpm_list list are in a depth first order, simply
 * because children are guaranteed to be discovered after parents, and
 * are inserted at the back of the list on discovery.
 *
 * Since device_pm_add() may be called with a device lock held,
 * we must never try to acquire a device lock while holding
 * dpm_list_mutex.
 */

LIST_HEAD(dpm_list);
static LIST_HEAD(dpm_prepared_list);
static LIST_HEAD(dpm_suspended_list);
static LIST_HEAD(dpm_late_early_list);
static LIST_HEAD(dpm_noirq_list);

struct suspend_stats suspend_stats;
static DEFINE_MUTEX(dpm_list_mtx);
static pm_message_t pm_transition;

static int async_error;

static const char *pm_verb(int event)
{
	switch (event) {
	case PM_EVENT_SUSPEND:
		return "suspend";
	case PM_EVENT_RESUME:
		return "resume";
	case PM_EVENT_FREEZE:
		return "freeze";
	case PM_EVENT_QUIESCE:
		return "quiesce";
	case PM_EVENT_HIBERNATE:
		return "hibernate";
	case PM_EVENT_THAW:
		return "thaw";
	case PM_EVENT_RESTORE:
		return "restore";
	case PM_EVENT_RECOVER:
		return "recover";
	default:
		return "(unknown PM event)";
	}
}

/**
 * device_pm_sleep_init - Initialize system suspend-related device fields.
 * @dev: Device object being initialized.
 */
void device_pm_sleep_init(struct device *dev)
{
	dev->power.is_prepared = false;
	dev->power.is_suspended = false;
	dev->power.is_noirq_suspended = false;
	dev->power.is_late_suspended = false;
	init_completion(&dev->power.completion);
	complete_all(&dev->power.completion);
	dev->power.wakeup = NULL;
	INIT_LIST_HEAD(&dev->power.entry);
}

/**
 * device_pm_lock - Lock the list of active devices used by the PM core.
 */
void device_pm_lock(void)
{
	mutex_lock(&dpm_list_mtx);
}

/**
 * device_pm_unlock - Unlock the list of active devices used by the PM core.
 */
void device_pm_unlock(void)
{
	mutex_unlock(&dpm_list_mtx);
}

/**
 * device_pm_add - Add a device to the PM core's list of active devices.
 * @dev: Device to add to the list.
 */
void device_pm_add(struct device *dev)
{
	/* Skip PM setup/initialization. */
	if (device_pm_not_required(dev))
		return;

	pr_debug("Adding info for %s:%s\n",
		 dev->bus ? dev->bus->name : "No Bus", dev_name(dev));
	device_pm_check_callbacks(dev);
	mutex_lock(&dpm_list_mtx);
	if (dev->parent && dev->parent->power.is_prepared)
		dev_warn(dev, "parent %s should not be sleeping\n",
			dev_name(dev->parent));
	list_add_tail(&dev->power.entry, &dpm_list);
	dev->power.in_dpm_list = true;
	mutex_unlock(&dpm_list_mtx);
}

/**
 * device_pm_remove - Remove a device from the PM core's list of active devices.
 * @dev: Device to be removed from the list.
 */
void device_pm_remove(struct device *dev)
{
	if (device_pm_not_required(dev))
		return;

	pr_debug("Removing info for %s:%s\n",
		 dev->bus ? dev->bus->name : "No Bus", dev_name(dev));
	complete_all(&dev->power.completion);
	mutex_lock(&dpm_list_mtx);
	list_del_init(&dev->power.entry);
	dev->power.in_dpm_list = false;
	mutex_unlock(&dpm_list_mtx);
	device_wakeup_disable(dev);
	pm_runtime_remove(dev);
	device_pm_check_callbacks(dev);
}

/**
 * device_pm_move_before - Move device in the PM core's list of active devices.
 * @deva: Device to move in dpm_list.
 * @devb: Device @deva should come before.
 */
void device_pm_move_before(struct device *deva, struct device *devb)
{
	pr_debug("Moving %s:%s before %s:%s\n",
		 deva->bus ? deva->bus->name : "No Bus", dev_name(deva),
		 devb->bus ? devb->bus->name : "No Bus", dev_name(devb));
	/* Delete deva from dpm_list and reinsert before devb. */
	list_move_tail(&deva->power.entry, &devb->power.entry);
}

/**
 * device_pm_move_after - Move device in the PM core's list of active devices.
 * @deva: Device to move in dpm_list.
 * @devb: Device @deva should come after.
 */
void device_pm_move_after(struct device *deva, struct device *devb)
{
	pr_debug("Moving %s:%s after %s:%s\n",
		 deva->bus ? deva->bus->name : "No Bus", dev_name(deva),
		 devb->bus ? devb->bus->name : "No Bus", dev_name(devb));
	/* Delete deva from dpm_list and reinsert after devb. */
	list_move(&deva->power.entry, &devb->power.entry);
}

/**
 * device_pm_move_last - Move device to end of the PM core's list of devices.
 * @dev: Device to move in dpm_list.
 */
void device_pm_move_last(struct device *dev)
{
	pr_debug("Moving %s:%s to end of list\n",
		 dev->bus ? dev->bus->name : "No Bus", dev_name(dev));
	list_move_tail(&dev->power.entry, &dpm_list);
}

static ktime_t initcall_debug_start(struct device *dev, void *cb)
{
	if (!pm_print_times_enabled)
		return 0;

	dev_info(dev, "calling %pS @ %i, parent: %s\n", cb,
		 task_pid_nr(current),
		 dev->parent ? dev_name(dev->parent) : "none");
	return ktime_get();
}

static void initcall_debug_report(struct device *dev, ktime_t calltime,
				  void *cb, int error)
{
	ktime_t rettime;
	s64 nsecs;

	if (!pm_print_times_enabled)
		return;

	rettime = ktime_get();
	nsecs = (s64) ktime_to_ns(ktime_sub(rettime, calltime));

	dev_info(dev, "%pS returned %d after %Ld usecs\n", cb, error,
		 (unsigned long long)nsecs >> 10);
}

/**
 * dpm_wait - Wait for a PM operation to complete.
 * @dev: Device to wait for.
 * @async: If unset, wait only if the device's power.async_suspend flag is set.
 */
static void dpm_wait(struct device *dev, bool async)
{
	if (!dev)
		return;

	if (async || (pm_async_enabled && dev->power.async_suspend))
		wait_for_completion(&dev->power.completion);
}

static int dpm_wait_fn(struct device *dev, void *async_ptr)
{
	dpm_wait(dev, *((bool *)async_ptr));
	return 0;
}

static void dpm_wait_for_children(struct device *dev, bool async)
{
       device_for_each_child(dev, &async, dpm_wait_fn);
}

static void dpm_wait_for_suppliers(struct device *dev, bool async)
{
	struct device_link *link;
	int idx;

	idx = device_links_read_lock();

	/*
	 * If the supplier goes away right after we've checked the link to it,
	 * we'll wait for its completion to change the state, but that's fine,
	 * because the only things that will block as a result are the SRCU
	 * callbacks freeing the link objects for the links in the list we're
	 * walking.
	 */
	list_for_each_entry_rcu_locked(link, &dev->links.suppliers, c_node)
		if (READ_ONCE(link->status) != DL_STATE_DORMANT)
			dpm_wait(link->supplier, async);

	device_links_read_unlock(idx);
}

static bool dpm_wait_for_superior(struct device *dev, bool async)
{
	struct device *parent;

	/*
	 * If the device is resumed asynchronously and the parent's callback
	 * deletes both the device and the parent itself, the parent object may
	 * be freed while this function is running, so avoid that by reference
	 * counting the parent once more unless the device has been deleted
	 * already (in which case return right away).
	 */
	mutex_lock(&dpm_list_mtx);

	if (!device_pm_initialized(dev)) {
		mutex_unlock(&dpm_list_mtx);
		return false;
	}

	parent = get_device(dev->parent);

	mutex_unlock(&dpm_list_mtx);

	dpm_wait(parent, async);
	put_device(parent);

	dpm_wait_for_suppliers(dev, async);

	/*
	 * If the parent's callback has deleted the device, attempting to resume
	 * it would be invalid, so avoid doing that then.
	 */
	return device_pm_initialized(dev);
}

static void dpm_wait_for_consumers(struct device *dev, bool async)
{
	struct device_link *link;
	int idx;

	idx = device_links_read_lock();

	/*
	 * The status of a device link can only be changed from "dormant" by a
	 * probe, but that cannot happen during system suspend/resume.  In
	 * theory it can change to "dormant" at that time, but then it is
	 * reasonable to wait for the target device anyway (eg. if it goes
	 * away, it's better to wait for it to go away completely and then
	 * continue instead of trying to continue in parallel with its
	 * unregistration).
	 */
	list_for_each_entry_rcu_locked(link, &dev->links.consumers, s_node)
		if (READ_ONCE(link->status) != DL_STATE_DORMANT)
			dpm_wait(link->consumer, async);

	device_links_read_unlock(idx);
}

static void dpm_wait_for_subordinate(struct device *dev, bool async)
{
	dpm_wait_for_children(dev, async);
	dpm_wait_for_consumers(dev, async);
}

/**
 * pm_op - Return the PM operation appropriate for given PM event.
 * @ops: PM operations to choose from.
 * @state: PM transition of the system being carried out.
 */
static pm_callback_t pm_op(const struct dev_pm_ops *ops, pm_message_t state)
{
	switch (state.event) {
#ifdef CONFIG_SUSPEND
	case PM_EVENT_SUSPEND:
		return ops->suspend;
	case PM_EVENT_RESUME:
		return ops->resume;
#endif /* CONFIG_SUSPEND */
#ifdef CONFIG_HIBERNATE_CALLBACKS
	case PM_EVENT_FREEZE:
	case PM_EVENT_QUIESCE:
		return ops->freeze;
	case PM_EVENT_HIBERNATE:
		return ops->poweroff;
	case PM_EVENT_THAW:
	case PM_EVENT_RECOVER:
		return ops->thaw;
	case PM_EVENT_RESTORE:
		return ops->restore;
#endif /* CONFIG_HIBERNATE_CALLBACKS */
	}

	return NULL;
}

/**
 * pm_late_early_op - Return the PM operation appropriate for given PM event.
 * @ops: PM operations to choose from.
 * @state: PM transition of the system being carried out.
 *
 * Runtime PM is disabled for @dev while this function is being executed.
 */
static pm_callback_t pm_late_early_op(const struct dev_pm_ops *ops,
				      pm_message_t state)
{
	switch (state.event) {
#ifdef CONFIG_SUSPEND
	case PM_EVENT_SUSPEND:
		return ops->suspend_late;
	case PM_EVENT_RESUME:
		return ops->resume_early;
#endif /* CONFIG_SUSPEND */
#ifdef CONFIG_HIBERNATE_CALLBACKS
	case PM_EVENT_FREEZE:
	case PM_EVENT_QUIESCE:
		return ops->freeze_late;
	case PM_EVENT_HIBERNATE:
		return ops->poweroff_late;
	case PM_EVENT_THAW:
	case PM_EVENT_RECOVER:
		return ops->thaw_early;
	case PM_EVENT_RESTORE:
		return ops->restore_early;
#endif /* CONFIG_HIBERNATE_CALLBACKS */
	}

	return NULL;
}

/**
 * pm_noirq_op - Return the PM operation appropriate for given PM event.
 * @ops: PM operations to choose from.
 * @state: PM transition of the system being carried out.
 *
 * The driver of @dev will not receive interrupts while this function is being
 * executed.
 */
static pm_callback_t pm_noirq_op(const struct dev_pm_ops *ops, pm_message_t state)
{
	switch (state.event) {
#ifdef CONFIG_SUSPEND
	case PM_EVENT_SUSPEND:
		return ops->suspend_noirq;
	case PM_EVENT_RESUME:
		return ops->resume_noirq;
#endif /* CONFIG_SUSPEND */
#ifdef CONFIG_HIBERNATE_CALLBACKS
	case PM_EVENT_FREEZE:
	case PM_EVENT_QUIESCE:
		return ops->freeze_noirq;
	case PM_EVENT_HIBERNATE:
		return ops->poweroff_noirq;
	case PM_EVENT_THAW:
	case PM_EVENT_RECOVER:
		return ops->thaw_noirq;
	case PM_EVENT_RESTORE:
		return ops->restore_noirq;
#endif /* CONFIG_HIBERNATE_CALLBACKS */
	}

	return NULL;
}

static void pm_dev_dbg(struct device *dev, pm_message_t state, const char *info)
{
	dev_dbg(dev, "%s%s%s\n", info, pm_verb(state.event),
		((state.event & PM_EVENT_SLEEP) && device_may_wakeup(dev)) ?
		", may wakeup" : "");
}

static void pm_dev_err(struct device *dev, pm_message_t state, const char *info,
			int error)
{
	dev_err(dev, "failed to %s%s: error %d\n", pm_verb(state.event), info,
		error);
}

static void dpm_show_time(ktime_t starttime, pm_message_t state, int error,
			  const char *info)
{
	ktime_t calltime;
	u64 usecs64;
	int usecs;

	calltime = ktime_get();
	usecs64 = ktime_to_ns(ktime_sub(calltime, starttime));
	do_div(usecs64, NSEC_PER_USEC);
	usecs = usecs64;
	if (usecs == 0)
		usecs = 1;

	pm_pr_dbg("%s%s%s of devices %s after %ld.%03ld msecs\n",
		  info ?: "", info ? " " : "", pm_verb(state.event),
		  error ? "aborted" : "complete",
		  usecs / USEC_PER_MSEC, usecs % USEC_PER_MSEC);
}

/*
 * haydn: the phone hangs (whole SoC, no oops) during system suspend right
 * after WLAN suspends. With haydn_pm_trace=1, log every suspend-direction
 * device callback before and after it runs and push the line to logdump
 * at once, so the last "haydn-pm: call" line names the device. Writes stop
 * landing once UFS itself is suspended; then the last line is UFS.
 */
static bool haydn_pm_trace;
core_param(haydn_pm_trace, haydn_pm_trace, bool, 0644);
static bool haydn_pm_keep_ufs;
core_param(haydn_pm_keep_ufs, haydn_pm_keep_ufs, bool, 0644);

/*
 * haydn: PMIC SDAM breadcrumb. The suspend hang resets the SoC after UFS is
 * asleep, so logdump cannot show where it stopped. pmk8350 SDAM 2
 * (sdam@7100, restart reason at 0x48, writable from HLOS) keeps its
 * contents across resets; 0x4a-0x4f read as zero and are unused. (SDAM 23,
 * sdam@8600, holds other data at 0x46-0x4f and did not keep our writes.) Before and after every PM callback (suspend and
 * resume, all phases) and at the steps of suspend_enter(), 6 bytes go to
 * offset 0x4a: magic 0xa5, step code, crc32 of the device name. The next
 * boot prints the last step and the matching device ("haydn-pmtrace:").
 * Only with haydn_pm_sdam=1, and never with interrupts disabled (SPMI
 * regmap may sleep).
 */
static bool haydn_pm_sdam;
core_param(haydn_pm_sdam, haydn_pm_sdam, bool, 0644);
static struct nvmem_device *haydn_sdam;
static bool haydn_pm_resuming;
#define HAYDN_SDAM_OFF	0x4a
#define HAYDN_SDAM_MAGIC	0xa5

static int haydn_sdam_match(struct device *dev, const void *data)
{
	return dev->of_node && of_node_name_eq(dev->of_node, "sdam") &&
	       dev->of_node->full_name &&
	       !strcmp(dev->of_node->full_name, "sdam@7100");
}

static struct nvmem_device *haydn_sdam_get(void)
{
	struct nvmem_device *nv;

	if (haydn_sdam)
		return haydn_sdam;
	nv = nvmem_device_find(NULL, haydn_sdam_match);
	if (IS_ERR_OR_NULL(nv))
		return NULL;
	haydn_sdam = nv;
	return nv;
}

/*
 * haydn: #174-#178 show the SoC dying a few microseconds after IRQ 38 is
 * handled on resume, with no device of ours at fault, i.e. inside some
 * hard IRQ handler. nvmem/regmap may sleep, so from hard IRQ context write
 * the same 6 bytes straight over SPMI (pmic_arb uses a raw spinlock):
 * bit 25 = hard handler entered, bit 26 = it returned. Only between
 * "before dpm_resume_noirq" and "resume completed".
 */
static struct spmi_device *haydn_sdev;

static void haydn_sdev_get(void)
{
	struct device_node *np;
	struct platform_device *pdev;

	if (haydn_sdev)
		return;
	np = of_find_node_by_name(NULL, "sdam");
	while (np && strcmp(np->full_name, "sdam@7100"))
		np = of_find_node_by_name(np, "sdam");
	if (!np)
		return;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	/* sdam -> PMIC spmi_device -> SPMI controller (both on the spmi bus) */
	if (pdev && pdev->dev.parent && pdev->dev.parent->bus &&
	    !strcmp(pdev->dev.parent->bus->name, "spmi") &&
	    pdev->dev.parent->parent && pdev->dev.parent->parent->bus &&
	    !strcmp(pdev->dev.parent->parent->bus->name, "spmi"))
		haydn_sdev = to_spmi_device(pdev->dev.parent);
}

/*
 * haydn: #184 died ~1 ms after a hard handler returned that touched no
 * hardware; what runs then is the softirq work on IRQ exit. Record softirq
 * vector entry (bit 23) and return (bit 22) the same way.
 */
void haydn_pm_softirq(unsigned int vec, bool done)
{
	u32 v = vec | (done ? 0x00400000u : 0x00800000u);
	u8 buf[6] = { HAYDN_SDAM_MAGIC, 0x40, v, v >> 8, v >> 16, v >> 24 };
	struct spmi_device *sdev = READ_ONCE(haydn_sdev);

	if (!READ_ONCE(haydn_pm_resuming) || !sdev)
		return;
	spmi_ext_register_writel(sdev, 0x7100 + HAYDN_SDAM_OFF, buf, sizeof(buf));
}

/*
 * haydn: #184 (no network): the first device IRQ after wakeup returns and
 * the SoC dies within ~1 ms, whichever IRQ it is. Name the function that
 * runs then: every timer, hrtimer and tasklet callback during resume.
 * Code 0x43: offset of a core kernel function from _stext; code 0x44:
 * crc32 of the module name holding it.
 */
void haydn_pm_fn(void *fn)
{
	unsigned long a = (unsigned long)fn;
	struct spmi_device *sdev = READ_ONCE(haydn_sdev);
	u8 buf[6] = { HAYDN_SDAM_MAGIC, 0x43 };
	u32 v;

	if (!READ_ONCE(haydn_pm_resuming) || !sdev)
		return;
	if (core_kernel_text(a)) {
		v = a - (unsigned long)_stext;
	} else {
		struct module *mod;

		preempt_disable();
		mod = __module_address(a);
		v = mod ? crc32_le(0, mod->name, strlen(mod->name)) : 0;
		preempt_enable();
		buf[1] = 0x44;
	}
	buf[2] = v; buf[3] = v >> 8; buf[4] = v >> 16; buf[5] = v >> 24;
	spmi_ext_register_writel(sdev, 0x7100 + HAYDN_SDAM_OFF, buf, sizeof(buf));
}

/*
 * haydn: a 60 s pm_test=core suspend (no real sleep, no wake IRQ) freezes
 * like a real suspend, and the diag config has CONFIG_PANIC_TIMEOUT=0, so the
 * "freeze" is most likely a kernel panic (BUG) after a long suspend. Record
 * where: on die() the PC of the BUG (0x48 core kernel offset from _stext,
 * 0x49 crc32 of the module), on a plain panic() 0x4a + first 4 chars of the
 * message. Other records stop after this.
 */
static bool haydn_died;

/*
 * haydn: with UFS out of the way the SoC dies right after IPA's IRQ (36,
 * "ipa") is re-enabled, as in the clean #174-#182 runs. Park the "ipa" and
 * "gsi" IRQs from PM_SUSPEND_PREPARE and re-enable them 2 s after
 * PM_POST_SUSPEND, recording code 0x50 + irq before each enable and 0x51
 * + irq 50 ms after. haydn_park_ipa=0 turns it off.
 */
static bool haydn_park_ipa = true;
core_param(haydn_park_ipa, haydn_park_ipa, bool, 0644);
static unsigned int haydn_parked[2];
static unsigned int haydn_nparked;

static void haydn_sdam_put(u8 code, u32 val);

/*
 * haydn diag: IPA IRQ path breadcrumb (code 0x52, step in byte 2, value in
 * bytes 3-5), written over SPMI so it works in hard IRQ context. Active from
 * PM_SUSPEND_PREPARE until 20 s after PM_POST_SUSPEND.
 */
static bool haydn_ipa_window;
static unsigned long haydn_ipa_window_end;

void haydn_pm_ipa_mark(u8 step, u32 val)
{
	struct spmi_device *sdev = READ_ONCE(haydn_sdev);
	u8 buf[6] = { HAYDN_SDAM_MAGIC, 0x52, step, val, val >> 8, val >> 16 };

	if (!READ_ONCE(haydn_ipa_window) || !sdev || READ_ONCE(haydn_died))
		return;
	if (haydn_ipa_window_end && time_after(jiffies, haydn_ipa_window_end)) {
		WRITE_ONCE(haydn_ipa_window, false);
		return;
	}
	spmi_ext_register_writel(sdev, 0x7100 + HAYDN_SDAM_OFF, buf, sizeof(buf));
}
EXPORT_SYMBOL(haydn_pm_ipa_mark);

/*
 * haydn diag: camera freeze. Codes 0x60-0x7f mark CVP register/queue access
 * and KGSL GMU OOB requests just before they happen (6 bytes, SPMI, works in
 * atomic context), so after a hard freeze the next boot names the last
 * hardware access. haydn_hw_mark=0 turns it off.
 */
static bool haydn_hw_mark = true;
core_param(haydn_hw_mark, haydn_hw_mark, bool, 0644);

void haydn_mark(u8 code, u32 val)
{
	struct spmi_device *sdev = READ_ONCE(haydn_sdev);
	u8 buf[6] = { HAYDN_SDAM_MAGIC, code, val, val >> 8, val >> 16,
		      val >> 24 };

	if (!READ_ONCE(haydn_hw_mark) || !sdev || READ_ONCE(haydn_died) ||
	    READ_ONCE(haydn_pm_resuming))
		return;
	spmi_ext_register_writel(sdev, 0x7100 + HAYDN_SDAM_OFF, buf, sizeof(buf));
}
EXPORT_SYMBOL(haydn_mark);

/*
 * haydn diag #203: the SoC died right at enable_irq(36) 2 s after resume,
 * before ipa3_isr() was entered. Keep IPA's own IRQ ("ipa") disabled for good
 * after the first suspend (GSI, which carries the data path, is still
 * re-enabled). haydn_unpark_ipa=1 restores the old behaviour.
 */
static bool haydn_unpark_ipa;
core_param(haydn_unpark_ipa, haydn_unpark_ipa, bool, 0644);
static int haydn_ipa_kept = -1;

static bool haydn_irq_is(unsigned int irq, const char *name)
{
	struct irq_desc *desc = irq_to_desc(irq);

	return desc && desc->action && desc->action->name &&
	       !strcmp(desc->action->name, name);
}

static void haydn_unpark_fn(struct work_struct *w)
{
	unsigned int i;

	for (i = 0; i < haydn_nparked; i++) {
		if (!READ_ONCE(haydn_unpark_ipa) &&
		    haydn_irq_is(haydn_parked[i], "ipa")) {
			if (haydn_ipa_kept < 0)
				pr_info("haydn: IPA IRQ %u stays disabled after suspend\n",
					haydn_parked[i]);
			haydn_ipa_kept = haydn_parked[i];
			continue;
		}
		haydn_sdam_put(0x50, haydn_parked[i]);
		enable_irq(haydn_parked[i]);
		msleep(50);
		haydn_sdam_put(0x51, haydn_parked[i]);
	}
	haydn_nparked = 0;
}
static DECLARE_DELAYED_WORK(haydn_unpark_work, haydn_unpark_fn);

static int haydn_park_notify(struct notifier_block *nb, unsigned long action,
			     void *data)
{
	struct irq_desc *desc;
	unsigned int irq;

	if (action == PM_SUSPEND_PREPARE) {
		haydn_sdev_get();
		haydn_ipa_window_end = 0;
		WRITE_ONCE(haydn_ipa_window, true);
		cancel_delayed_work_sync(&haydn_unpark_work);
		if (haydn_nparked)
			haydn_unpark_fn(NULL);
		if (!READ_ONCE(haydn_park_ipa))
			return NOTIFY_DONE;
		for_each_irq_desc(irq, desc) {
			const char *n = desc->action ? desc->action->name : NULL;

			if (!n || (strcmp(n, "ipa") && strcmp(n, "gsi")))
				continue;
			if ((int)irq == haydn_ipa_kept)
				continue;	/* already disabled for good */
			if (haydn_nparked >= ARRAY_SIZE(haydn_parked))
				break;
			disable_irq(irq);
			haydn_parked[haydn_nparked++] = irq;
		}
	} else if (action == PM_POST_SUSPEND) {
		haydn_ipa_window_end = jiffies + 20 * HZ;
		if (haydn_nparked)
			schedule_delayed_work(&haydn_unpark_work, 2 * HZ);
	}
	return NOTIFY_DONE;
}

static struct notifier_block haydn_park_nb = {
	.notifier_call = haydn_park_notify,
};

/*
 * haydn: #195 named the stuck device (1d84000.ufshc). Now record where its
 * suspend/resume task is: if it sleeps, the first non-scheduler frame of its
 * stack (what it waits for); if it runs on a CPU, the PC interrupted by an IPI.
 * Codes 0x4c/0x4d (sleeping: core offset / module crc), 0x4e/0x4f (running).
 */
static void haydn_put_pc(unsigned long pc, u8 core_code);

static void haydn_dpm_ipi(void *unused)
{
	struct pt_regs *regs = get_irq_regs();

	haydn_put_pc(regs ? instruction_pointer(regs) : 0, 0x4e);
}

void haydn_pm_dpm_timeout(struct device *dev, struct task_struct *tsk)
{
	struct spmi_device *sdev = READ_ONCE(haydn_sdev);
	const char *n = dev_name(dev);
	u32 v = crc32_le(0, n, strlen(n));
	u8 buf[6] = { HAYDN_SDAM_MAGIC, 0x4b, v, v >> 8, v >> 16, v >> 24 };
	unsigned long entries[16];
	unsigned int nr, i;

	WRITE_ONCE(haydn_pm_resuming, false);
	if (!sdev)
		return;
	spmi_ext_register_writel(sdev, 0x7100 + HAYDN_SDAM_OFF, buf, sizeof(buf));
	if (!tsk)
		return;
	if (task_curr(tsk)) {
		smp_call_function_single(task_cpu(tsk), haydn_dpm_ipi, NULL, 0);
		return;
	}
	nr = stack_trace_save_tsk(tsk, entries, ARRAY_SIZE(entries), 0);
	for (i = 0; i < nr; i++) {
		if (in_sched_functions(entries[i]))
			continue;
		haydn_put_pc(entries[i], 0x4c);
		return;
	}
}

static void haydn_put_pc(unsigned long pc, u8 core_code)
{
	struct spmi_device *sdev = READ_ONCE(haydn_sdev);
	u8 buf[6] = { HAYDN_SDAM_MAGIC, core_code };
	u32 v;

	if (!sdev)
		return;
	if (pc && core_kernel_text(pc)) {
		v = pc - (unsigned long)_stext;
	} else {
		struct module *mod = pc ? __module_address(pc) : NULL;

		v = mod ? crc32_le(0, mod->name, strlen(mod->name)) : 0;
		buf[1] = core_code + 1;
	}
	buf[2] = v; buf[3] = v >> 8; buf[4] = v >> 16; buf[5] = v >> 24;
	spmi_ext_register_writel(sdev, 0x7100 + HAYDN_SDAM_OFF, buf, sizeof(buf));
}

static int haydn_die_notify(struct notifier_block *nb, unsigned long val,
			    void *data)
{
	struct die_args *args = data;

	if (READ_ONCE(haydn_died) || !args || !args->regs)
		return NOTIFY_DONE;
	WRITE_ONCE(haydn_died, true);
	WRITE_ONCE(haydn_pm_resuming, false);
	{
		unsigned long pc = instruction_pointer(args->regs);
		struct module *mod = core_kernel_text(pc) ? NULL :
				     __module_address(pc);
		struct spmi_device *sdev = READ_ONCE(haydn_sdev);

		/*
		 * Oops in a module: code 0x5a, low byte of the crc32 of the
		 * module name, then the 24-bit offset of the PC in its core
		 * text, so the next boot can name the function.
		 */
		if (mod && sdev) {
			u32 c = crc32_le(0, mod->name, strlen(mod->name));
			u32 off = pc - (unsigned long)mod->core_layout.base;
			u8 buf[6] = { HAYDN_SDAM_MAGIC, 0x5a, c, off,
				      off >> 8, off >> 16 };

			spmi_ext_register_writel(sdev, 0x7100 + HAYDN_SDAM_OFF,
						 buf, sizeof(buf));
		} else {
			haydn_put_pc(pc, 0x48);
		}
	}
	return NOTIFY_DONE;
}

static struct notifier_block haydn_die_nb = {
	.notifier_call = haydn_die_notify,
	.priority = INT_MAX,
};

static int haydn_panic_notify(struct notifier_block *nb, unsigned long val,
			      void *data)
{
	struct spmi_device *sdev = READ_ONCE(haydn_sdev);
	const char *msg = data;
	u8 buf[6] = { HAYDN_SDAM_MAGIC, 0x4a, ' ', ' ', ' ', ' ' };
	int i;

	WRITE_ONCE(haydn_pm_resuming, false);
	if (READ_ONCE(haydn_died) || !sdev)
		return NOTIFY_DONE;
	WRITE_ONCE(haydn_died, true);
	for (i = 0; msg && i < 4 && msg[i]; i++)
		buf[2 + i] = msg[i];
	spmi_ext_register_writel(sdev, 0x7100 + HAYDN_SDAM_OFF, buf, sizeof(buf));
	return NOTIFY_DONE;
}

static struct notifier_block haydn_panic_nb = {
	.notifier_call = haydn_panic_notify,
	.priority = INT_MAX,
};

void haydn_pm_hardirq(unsigned int irq, bool done)
{
	u32 v = irq | (done ? 0x04000000u : 0x02000000u);
	u8 buf[6] = { HAYDN_SDAM_MAGIC, 0x40, v, v >> 8, v >> 16, v >> 24 };
	struct spmi_device *sdev = READ_ONCE(haydn_sdev);

	if (!READ_ONCE(haydn_pm_resuming) || !sdev)
		return;
	spmi_ext_register_writel(sdev, 0x7100 + HAYDN_SDAM_OFF, buf, sizeof(buf));
}

static void haydn_sdam_put(u8 code, u32 crc)
{
	u8 buf[6] = { HAYDN_SDAM_MAGIC, code, crc, crc >> 8, crc >> 16,
		      crc >> 24 };
	struct nvmem_device *nv;

	if (!READ_ONCE(haydn_pm_sdam) || irqs_disabled() || in_atomic())
		return;
	nv = haydn_sdam_get();
	if (nv)
		nvmem_device_write(nv, HAYDN_SDAM_OFF, sizeof(buf), buf);
}

/* step codes from suspend_enter(), see kernel/power/suspend.c */
void haydn_pm_stage(u8 code)
{
	if (code == 0x31) {
		haydn_sdev_get();
		WRITE_ONCE(haydn_pm_resuming, true);
	}
	else if (code == 0x3f)
		WRITE_ONCE(haydn_pm_resuming, false);
	haydn_sdam_put(code, 0);
}

bool haydn_pm_irq_wanted(void)
{
	return READ_ONCE(haydn_pm_sdam);
}

/*
 * Threaded IRQ handlers run in a kthread a little after their IRQ is
 * re-enabled, so a hang inside one shows up as a later position of the
 * resume_irqs() loop. Between "before dpm_resume_noirq" and "resume
 * completed" record each threaded handler (bit 29) before it runs; the
 * thread is sleepable, so the SPMI write is allowed.
 */
void haydn_pm_irq_thread(unsigned int irq)
{
	if (READ_ONCE(haydn_pm_resuming))
		haydn_sdam_put(0x40, irq | 0x20000000u);
}

/*
 * haydn: is the reset tied to the resume_irqs() position (always "IRQ 38")
 * or to the time since wakeup? With haydn_irq_delay_ms=N, pause N ms right
 * before device IRQs come back, with a record every 10 ms (code 0x34).
 */
static unsigned int haydn_irq_delay_ms;
core_param(haydn_irq_delay_ms, haydn_irq_delay_ms, uint, 0644);

void haydn_pm_irq_delay(void)
{
	unsigned int ms = READ_ONCE(haydn_irq_delay_ms), t;

	if (!ms || !READ_ONCE(haydn_pm_sdam))
		return;
	for (t = 0; t < ms; t += 10) {
		haydn_sdam_put(0x34, t);
		mdelay(10);
	}
	haydn_sdam_put(0x34, ms | 0x80000000u);
}

/* kernel/irq/pm.c: about to re-enable this device IRQ on resume */
void haydn_pm_irq(unsigned int irq)
{
	haydn_sdam_put(0x40, irq);
}

static u8 haydn_pm_code(pm_message_t state, const char *info, bool done)
{
	u8 code = 0;

	if (info && !strncmp(info, "noirq", 5))
		code = 2;
	else if (info && (!strncmp(info, "late", 4) || !strncmp(info, "early", 5)))
		code = 1;
	if (!(state.event & (PM_EVENT_SUSPEND | PM_EVENT_FREEZE)))
		code |= 4;	/* resume direction */
	if (done)
		code |= 8;
	return code;
}

static const char * const haydn_steps[] = {
	[0x20] = "dpm_suspend_noirq done",
	[0x21] = "platform_suspend_prepare_noirq done",
	[0x22] = "before disabling secondary CPUs",
	[0x23] = "secondary CPUs off, before irqs off/syscore/firmware",
	[0x24] = "s2idle loop entered",
	[0x30] = "back: secondary CPUs on",
	[0x31] = "back: before dpm_resume_noirq",
	[0x32] = "device IRQs re-enabled",
	[0x3f] = "resume completed",
};

/*
 * The report line scrolls out of dmesg within a minute on haydn, so keep
 * it readable at any time: /sys/module/haydn_pm/parameters/last
 */
static char haydn_pmtrace_last[512];
static size_t haydn_pmtrace_len;
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "haydn_pm."
module_param_string(last, haydn_pmtrace_last, sizeof(haydn_pmtrace_last), 0444);
#define HREPORT(fmt, ...) do {						\
	haydn_pmtrace_len += scnprintf(haydn_pmtrace_last + haydn_pmtrace_len, \
		sizeof(haydn_pmtrace_last) - haydn_pmtrace_len, fmt, ##__VA_ARGS__); \
	pr_info(fmt, ##__VA_ARGS__);					\
} while (0)

static void haydn_pmtrace_report(struct work_struct *w)
{
	struct nvmem_device *nv = haydn_sdam_get();
	struct device *dev;
	u8 b[6] = { 0 };
	u32 crc;
	bool found = false;

	/* the hardware marks need the SPMI device even when nothing was saved */
	haydn_sdev_get();
	if (!nv) {
		HREPORT("haydn-pmtrace: SDAM sdam@7100 not found\n");
		return;
	}
	if (nvmem_device_read(nv, HAYDN_SDAM_OFF, sizeof(b), b) != sizeof(b) ||
	    b[0] != HAYDN_SDAM_MAGIC) {
		HREPORT("haydn-pmtrace: no record from the previous boot (%02x)\n", b[0]);
		return;
	}
	crc = b[2] | b[3] << 8 | b[4] << 16 | (u32)b[5] << 24;
	if (b[1] == 0x40) {
		bool wake = crc & 0x80000000u;
		bool after = crc & 0x40000000u;
		bool thread = crc & 0x20000000u;
		bool busy = crc & 0x10000000u;
		bool unlocked = crc & 0x08000000u;
		bool hdone = crc & 0x04000000u;
		bool hentry = crc & 0x02000000u;
		bool late = crc & 0x01000000u;
		unsigned int depth;
		struct irq_desc *desc;
		struct irq_data *d;
		bool sentry = crc & 0x00800000u;
		bool sdone = crc & 0x00400000u;

		if (sentry || sdone) {
			unsigned int vec = crc & 0xffu;

			HREPORT("haydn-pmtrace: previous boot ended %s softirq %s (vector %u)\n",
				sentry ? "INSIDE" : "right after",
				vec < NR_SOFTIRQS ? softirq_to_name[vec] : "?", vec);
			goto haydn_done;
		}
		depth = (crc >> 16) & 0xffu;
		crc &= 0xffffu;
		desc = irq_to_desc(crc);
		d = desc ? irq_desc_get_irq_data(desc) : NULL;
		if (late && after)
			HREPORT("haydn-pmtrace: held IRQ %u was released and 1 ms passed; the next one was never reached\n",
				crc);
		else if (late)
			HREPORT("haydn-pmtrace: previous boot DIED right after RELEASING held IRQ %u (its unmask kills the SoC)\n",
				crc);
		if (hentry)
			HREPORT("haydn-pmtrace: previous boot ended INSIDE the hard IRQ handler of IRQ %u (it never returned)\n",
				crc);
		if (hdone)
			HREPORT("haydn-pmtrace: IRQ %u hard handler returned; nothing was recorded after it\n",
				crc);
		if (busy)
			HREPORT("haydn-pmtrace: IRQ %u: its lock was HELD by someone else on resume; hung waiting for it\n",
				crc);
		if (unlocked)
			HREPORT("haydn-pmtrace: IRQ %u: resume_irq() returned (depth %u, %s); died within the next 50 us (an interrupt in that window)\n",
				crc, depth, depth ? "still masked" : "UNMASKED");
		if (thread)
			HREPORT("haydn-pmtrace: previous boot ended inside the THREADED handler of IRQ %u: %s (chip %s, hwirq %lu)\n",
				crc, desc && desc->action && desc->action->name ?
				desc->action->name : "?",
				d && d->chip && d->chip->name ? d->chip->name : "?",
				d ? d->hwirq : 0);
		if (after && !late)
			HREPORT("haydn-pmtrace: IRQ %u was re-enabled and 50 us passed; the next IRQ was never reached\n",
				crc);

		HREPORT("haydn-pmtrace: %s %s IRQ %u: %s (chip %s, hwirq %lu)\n",
			(hentry || hdone || late) ? "that is" :
			"previous boot ended while re-enabling",
			wake ? "WAKE" : "device", crc, desc && desc->action && desc->action->name ?
			desc->action->name : "?",
			d && d->chip && d->chip->name ? d->chip->name : "?",
			d ? d->hwirq : 0);
	} else if (b[1] == 0x48) {
		HREPORT("haydn-pmtrace: previous boot hit a KERNEL BUG/OOPS at %pS\n",
			(void *)((unsigned long)_stext + crc));
	} else if (b[1] == 0x49) {
		struct kobject *k;
		const char *mname = NULL;

		spin_lock(&module_kset->list_lock);
		list_for_each_entry(k, &module_kset->list, entry) {
			const char *n = kobject_name(k);

			if (n && crc32_le(0, n, strlen(n)) == crc) {
				mname = n;
				break;
			}
		}
		HREPORT("haydn-pmtrace: previous boot hit a KERNEL BUG/OOPS in module %s (crc %08x)\n",
			mname ?: "?", crc);
		spin_unlock(&module_kset->list_lock);
	} else if (b[1] >= 0x60 && b[1] <= 0x7f) {
		static const char * const hw[0x20] = {
			[0x00] = "CVP register WRITE (reg | value<<24)",
			[0x01] = "CVP register READ (reg)",
			[0x02] = "CVP command queue write (packet type)",
			[0x03] = "CVP power collapse start (force)",
			[0x05] = "CVP resume start (power_enabled)",
			[0x11] = "KGSL GMU OOB set was SLOW (oob | ms<<8)",
			[0x12] = "KGSL GMU OOB set TIMEOUT (oob)",
		};
		u32 v = b[2] | b[3] << 8 | b[4] << 16 | (u32)b[5] << 24;

		HREPORT("haydn-pmtrace: last hardware mark before the freeze: %s: %08x (code %02x)\n",
			hw[b[1] - 0x60] ?: "?", v, b[1]);
	} else if (b[1] == 0x5a) {
		struct kobject *k;
		u32 off = b[3] | b[4] << 8 | b[5] << 16;
		bool found = false;

		spin_lock(&module_kset->list_lock);
		list_for_each_entry(k, &module_kset->list, entry) {
			struct module *mod =
				container_of(k, struct module_kobject, kobj)->mod;

			if (!mod || (crc32_le(0, mod->name, strlen(mod->name)) & 0xff) != b[2] ||
			    off >= mod->core_layout.text_size)
				continue;
			found = true;
			HREPORT("haydn-pmtrace: previous boot hit a KERNEL BUG/OOPS at %pS [%s+0x%x]\n",
				(void *)((unsigned long)mod->core_layout.base + off),
				mod->name, off);
		}
		spin_unlock(&module_kset->list_lock);
		if (!found)
			HREPORT("haydn-pmtrace: previous boot hit a KERNEL BUG/OOPS in a module (crc byte %02x, offset 0x%x)\n",
				b[2], off);
	} else if (b[1] == 0x4c) {
		HREPORT("haydn-pmtrace: stuck UFS/device PM task was WAITING in %pS\n",
			(void *)((unsigned long)_stext + crc));
	} else if (b[1] == 0x4d) {
		struct kobject *k;
		const char *mname = NULL;

		spin_lock(&module_kset->list_lock);
		list_for_each_entry(k, &module_kset->list, entry) {
			const char *n = kobject_name(k);

			if (n && crc32_le(0, n, strlen(n)) == crc) {
				mname = n;
				break;
			}
		}
		HREPORT("haydn-pmtrace: stuck UFS/device PM task was WAITING in module %s (crc %08x)\n",
			mname ?: "?", crc);
		spin_unlock(&module_kset->list_lock);
	} else if (b[1] == 0x4e) {
		HREPORT("haydn-pmtrace: stuck UFS/device PM task was RUNNING at %pS\n",
			(void *)((unsigned long)_stext + crc));
	} else if (b[1] == 0x4f) {
		struct kobject *k;
		const char *mname = NULL;

		spin_lock(&module_kset->list_lock);
		list_for_each_entry(k, &module_kset->list, entry) {
			const char *n = kobject_name(k);

			if (n && crc32_le(0, n, strlen(n)) == crc) {
				mname = n;
				break;
			}
		}
		HREPORT("haydn-pmtrace: stuck UFS/device PM task was RUNNING at module %s (crc %08x)\n",
			mname ?: "?", crc);
		spin_unlock(&module_kset->list_lock);
	} else if (b[1] == 0x52) {
		static const char * const st[] = {
			"?", "ipa3_isr entered", "ipa3_isr: IPA already clocked, reading regs",
			"ipa3_isr: IPA not clocked, deferred to work",
			"defer work: voting IPA clocks", "defer work: IPA clocks voted",
			"process_interrupts: about to read IRQ_EN", "read IRQ_EN ok (value)",
			"read IRQ_STTS ok (value)", "calling handler for IPA irq (value)",
			"handler returned for IPA irq (value)", "process_interrupts done",
		};
		u32 val = b[3] | b[4] << 8 | b[5] << 16;

		HREPORT("haydn-pmtrace: IPA path, last step %u: %s, value 0x%x\n",
			b[2], b[2] < ARRAY_SIZE(st) ? st[b[2]] : "?", val);
	} else if (b[1] == 0x50 || b[1] == 0x51) {
		struct irq_desc *pd = irq_to_desc(crc & 0xffffu);

		const char *pn = pd && pd->action && pd->action->name ?
				 pd->action->name : "?";

		if (b[1] == 0x50)
			HREPORT("haydn-pmtrace: previous boot DIED re-enabling parked IRQ %u (%s) 2 s after resume\n",
				crc & 0xffffu, pn);
		else
			HREPORT("haydn-pmtrace: parked IRQ %u (%s) re-enabled after resume, alive 50 ms later; died later\n",
				crc & 0xffffu, pn);
	} else if (b[1] == 0x4b) {
		struct kobject *k;
		const char *dname = NULL;

		spin_lock(&devices_kset->list_lock);
		list_for_each_entry(k, &devices_kset->list, entry) {
			const char *n = kobject_name(k);

			if (n && crc32_le(0, n, strlen(n)) == crc) {
				dname = n;
				break;
			}
		}
		HREPORT("haydn-pmtrace: DEVICE SUSPEND/RESUME STUCK > 5 s: %s (crc %08x)\n",
			dname ?: "?", crc);
		spin_unlock(&devices_kset->list_lock);
	} else if (b[1] == 0x4a) {
		HREPORT("haydn-pmtrace: previous boot PANICKED: \"%c%c%c%c...\"\n",
			b[2], b[3], b[4], b[5]);
	} else if (b[1] == 0x43) {
		HREPORT("haydn-pmtrace: previous boot ended in timer/tasklet callback %pS\n",
			(void *)((unsigned long)_stext + crc));
	} else if (b[1] == 0x44) {
		struct kobject *k;
		const char *mname = NULL;

		spin_lock(&module_kset->list_lock);
		list_for_each_entry(k, &module_kset->list, entry) {
			const char *n = kobject_name(k);

			if (n && crc32_le(0, n, strlen(n)) == crc) {
				mname = n;
				break;
			}
		}
		HREPORT("haydn-pmtrace: previous boot ended in a timer/tasklet callback of module %s (crc %08x)\n",
			mname ?: "?", crc);
		spin_unlock(&module_kset->list_lock);
	} else if (b[1] == 0x34) {
		crc = b[2] | b[3] << 8 | b[4] << 16 | (u32)b[5] << 24;
		if (crc & 0x80000000u)
			HREPORT("haydn-pmtrace: the %u ms pause before device IRQs FINISHED; nothing after it was recorded\n",
				crc & 0xffffu);
		else
			HREPORT("haydn-pmtrace: previous boot DIED %u-%u ms INTO the pause before device IRQs (time-based, not IRQ-based)\n",
				crc, crc + 10);
	} else if (b[1] >= 0x20) {
		HREPORT("haydn-pmtrace: previous boot ended after step 0x%02x (%s)\n",
			b[1], b[1] < ARRAY_SIZE(haydn_steps) && haydn_steps[b[1]] ?
			haydn_steps[b[1]] : "?");
	} else {
		device_pm_lock();
		list_for_each_entry(dev, &dpm_list, power.entry) {
			if (crc32_le(0, dev_name(dev), strlen(dev_name(dev))) == crc) {
				found = true;
				break;
			}
		}
		HREPORT("haydn-pmtrace: previous boot ended at %s %s%s callback (%s), device %s %s (crc %08x)\n",
			b[1] & 8 ? "after" : "inside",
			b[1] & 4 ? "resume " : "suspend ",
			(b[1] & 3) == 2 ? "noirq" : (b[1] & 3) == 1 ? "late/early" : "main",
			b[1] & 8 ? "it returned" : "it never returned",
			found ? dev_driver_string(dev) : "?",
			found ? dev_name(dev) : "?", crc);
		device_pm_unlock();
	}
haydn_done:
	memset(b, 0, sizeof(b));
	nvmem_device_write(nv, HAYDN_SDAM_OFF, sizeof(b), b);

	/* self-test of the hard IRQ path: direct SPMI write with IRQs off */
	haydn_sdev_get();
	if (haydn_sdev) {
		static const u8 t[6] = { HAYDN_SDAM_MAGIC, 0x41, 0x11, 0x22, 0x33, 0x44 };
		unsigned long flags;
		int rc;

		local_irq_save(flags);
		rc = spmi_ext_register_writel(haydn_sdev, 0x7100 + HAYDN_SDAM_OFF,
					      t, sizeof(t));
		local_irq_restore(flags);
		if (!rc && nvmem_device_read(nv, HAYDN_SDAM_OFF, sizeof(b), b) == sizeof(b) &&
		    !memcmp(b, t, sizeof(t)))
			HREPORT("haydn-pmtrace: hard-IRQ record path self-test OK\n");
		else
			HREPORT("haydn-pmtrace: hard-IRQ record path self-test FAILED (rc %d)\n", rc);
	} else {
		HREPORT("haydn-pmtrace: hard-IRQ record path NOT available (no spmi device)\n");
	}
	memset(b, 0, sizeof(b));
	nvmem_device_write(nv, HAYDN_SDAM_OFF, sizeof(b), b);
}
static DECLARE_DELAYED_WORK(haydn_pmtrace_work, haydn_pmtrace_report);

static int __init haydn_pmtrace_init(void)
{
	schedule_delayed_work(&haydn_pmtrace_work, 20 * HZ);
	register_die_notifier(&haydn_die_nb);
	register_pm_notifier(&haydn_park_nb);
	atomic_notifier_chain_register(&panic_notifier_list, &haydn_panic_nb);
	return 0;
}
late_initcall_sync(haydn_pmtrace_init);

static void haydn_pm_mark(struct device *dev, pm_message_t state,
			  const char *info, pm_callback_t cb,
			  const char *what, int error)
{
	if (READ_ONCE(haydn_pm_sdam))
		haydn_sdam_put(haydn_pm_code(state, info, what[0] == 'd'),
			       crc32_le(0, dev_name(dev), strlen(dev_name(dev))));
	if (!READ_ONCE(haydn_pm_trace))
		return;
	if (!(state.event & (PM_EVENT_SUSPEND | PM_EVENT_FREEZE)))
		return;
	pr_info("haydn-pm: %s %s %s %s %ps %d\n", what, info ? info : "",
		dev_driver_string(dev), dev_name(dev), cb, error);
	haydn_bootlog_sync_fast();
}

static int dpm_run_callback(pm_callback_t cb, struct device *dev,
			    pm_message_t state, const char *info)
{
	ktime_t calltime;
	int error;

	if (!cb)
		return 0;

	/*
	 * haydn: haydn_pm_keep_ufs=1 leaves the UFS host and its SCSI disks
	 * running through system suspend/resume (every phase), so logdump
	 * writes keep landing and the haydn-pm trace can name devices that
	 * suspend after storage. Debug only: UFS then never sleeps.
	 */
	if (READ_ONCE(haydn_pm_keep_ufs) && dev->driver &&
	    (!strcmp(dev->driver->name, "sd") ||
	     !strncmp(dev->driver->name, "ufshcd", 6))) {
		haydn_pm_mark(dev, state, info, cb, "skip", 0);
		return 0;
	}

	calltime = initcall_debug_start(dev, cb);

	pm_dev_dbg(dev, state, info);
	haydn_pm_mark(dev, state, info, cb, "call", 0);
	trace_device_pm_callback_start(dev, info, state.event);
	error = cb(dev);
	trace_device_pm_callback_end(dev, error);
	haydn_pm_mark(dev, state, info, cb, "done", error);
	suspend_report_result(cb, error);

	initcall_debug_report(dev, calltime, cb, error);

	return error;
}

#ifdef CONFIG_DPM_WATCHDOG
struct dpm_watchdog {
	struct device		*dev;
	struct task_struct	*tsk;
	struct timer_list	timer;
};

#define DECLARE_DPM_WATCHDOG_ON_STACK(wd) \
	struct dpm_watchdog wd

/**
 * dpm_watchdog_handler - Driver suspend / resume watchdog handler.
 * @t: The timer that PM watchdog depends on.
 *
 * Called when a driver has timed out suspending or resuming.
 * There's not much we can do here to recover so panic() to
 * capture a crash-dump in pstore.
 */
void haydn_pm_dpm_timeout(struct device *dev, struct task_struct *tsk);

static void dpm_watchdog_handler(struct timer_list *t)
{
	struct dpm_watchdog *wd = from_timer(wd, t, timer);

	/*
	 * haydn diag: #188 + 60 s pm_test=core died in this handler: a device
	 * suspend/resume callback took longer than DPM_WATCHDOG_TIMEOUT. Name
	 * it in the PMIC breadcrumb (code 0x4b, crc32 of dev_name) and print
	 * its stack instead of panicking, so dmesg shows where it is stuck.
	 */
	haydn_pm_dpm_timeout(wd->dev, wd->tsk);
	dev_emerg(wd->dev, "**** DPM device timeout **** driver %s\n",
		  dev_driver_string(wd->dev));
	show_stack(wd->tsk, NULL, KERN_EMERG);
}

/**
 * dpm_watchdog_set - Enable pm watchdog for given device.
 * @wd: Watchdog. Must be allocated on the stack.
 * @dev: Device to handle.
 */
static void dpm_watchdog_set(struct dpm_watchdog *wd, struct device *dev)
{
	struct timer_list *timer = &wd->timer;

	wd->dev = dev;
	wd->tsk = current;

	timer_setup_on_stack(timer, dpm_watchdog_handler, 0);
	/* use same timeout value for both suspend and resume */
	timer->expires = jiffies + HZ * CONFIG_DPM_WATCHDOG_TIMEOUT;
	add_timer(timer);
}

/**
 * dpm_watchdog_clear - Disable suspend/resume watchdog.
 * @wd: Watchdog to disable.
 */
static void dpm_watchdog_clear(struct dpm_watchdog *wd)
{
	struct timer_list *timer = &wd->timer;

	del_timer_sync(timer);
	destroy_timer_on_stack(timer);
}
#else
#define DECLARE_DPM_WATCHDOG_ON_STACK(wd)
#define dpm_watchdog_set(x, y)
#define dpm_watchdog_clear(x)
#endif

/*------------------------- Resume routines -------------------------*/

/**
 * dev_pm_skip_resume - System-wide device resume optimization check.
 * @dev: Target device.
 *
 * Return:
 * - %false if the transition under way is RESTORE.
 * - Return value of dev_pm_skip_suspend() if the transition under way is THAW.
 * - The logical negation of %power.must_resume otherwise (that is, when the
 *   transition under way is RESUME).
 */
bool dev_pm_skip_resume(struct device *dev)
{
	if (pm_transition.event == PM_EVENT_RESTORE)
		return false;

	if (pm_transition.event == PM_EVENT_THAW)
		return dev_pm_skip_suspend(dev);

	return !dev->power.must_resume;
}

/**
 * device_resume_noirq - Execute a "noirq resume" callback for given device.
 * @dev: Device to handle.
 * @state: PM transition of the system being carried out.
 * @async: If true, the device is being resumed asynchronously.
 *
 * The driver of @dev will not receive interrupts while this function is being
 * executed.
 */
static void device_resume_noirq(struct device *dev, pm_message_t state, bool async)
{
	pm_callback_t callback = NULL;
	const char *info = NULL;
	bool skip_resume;
	int error = 0;

	TRACE_DEVICE(dev);
	TRACE_RESUME(0);

	if (dev->power.syscore || dev->power.direct_complete)
		goto Out;

	if (!dev->power.is_noirq_suspended) {
		/*
		 * This means that system suspend has been aborted in the noirq
		 * phase before invoking the noirq suspend callback for the
		 * device, so if device_suspend_late() has left it in suspend,
		 * device_resume_early() should leave it in suspend either in
		 * case the early resume of it depends on the noirq resume that
		 * has not run.
		 */
		if (dev_pm_skip_suspend(dev))
			dev->power.must_resume = false;

		goto Out;
	}

	if (!dpm_wait_for_superior(dev, async))
		goto Out;

	skip_resume = dev_pm_skip_resume(dev);
	/*
	 * If the driver callback is skipped below or by the middle layer
	 * callback and device_resume_early() also skips the driver callback for
	 * this device later, it needs to appear as "suspended" to PM-runtime,
	 * so change its status accordingly.
	 *
	 * Otherwise, the device is going to be resumed, so set its PM-runtime
	 * status to "active", but do that only if DPM_FLAG_SMART_SUSPEND is set
	 * to avoid confusing drivers that don't use it.
	 */
	if (skip_resume)
		pm_runtime_set_suspended(dev);
	else if (dev_pm_skip_suspend(dev))
		pm_runtime_set_active(dev);

	if (dev->pm_domain) {
		info = "noirq power domain ";
		callback = pm_noirq_op(&dev->pm_domain->ops, state);
	} else if (dev->type && dev->type->pm) {
		info = "noirq type ";
		callback = pm_noirq_op(dev->type->pm, state);
	} else if (dev->class && dev->class->pm) {
		info = "noirq class ";
		callback = pm_noirq_op(dev->class->pm, state);
	} else if (dev->bus && dev->bus->pm) {
		info = "noirq bus ";
		callback = pm_noirq_op(dev->bus->pm, state);
	}
	if (callback)
		goto Run;

	if (skip_resume)
		goto Skip;

	if (dev->driver && dev->driver->pm) {
		info = "noirq driver ";
		callback = pm_noirq_op(dev->driver->pm, state);
	}

Run:
	error = dpm_run_callback(callback, dev, state, info);

Skip:
	dev->power.is_noirq_suspended = false;

Out:
	complete_all(&dev->power.completion);
	TRACE_RESUME(error);

	if (error) {
		suspend_stats.failed_resume_noirq++;
		dpm_save_failed_step(SUSPEND_RESUME_NOIRQ);
		dpm_save_failed_dev(dev_name(dev));
		pm_dev_err(dev, state, async ? " async noirq" : " noirq", error);
	}
}

static bool is_async(struct device *dev)
{
	return dev->power.async_suspend && pm_async_enabled
		&& !pm_trace_is_enabled();
}

static bool dpm_async_fn(struct device *dev, async_func_t func)
{
	reinit_completion(&dev->power.completion);

	if (is_async(dev)) {
		dev->power.async_in_progress = true;

		get_device(dev);

		if (async_schedule_dev_nocall(func, dev))
			return true;

		put_device(dev);
	}
	/*
	 * Because async_schedule_dev_nocall() above has returned false or it
	 * has not been called at all, func() is not running and it is safe to
	 * update the async_in_progress flag without extra synchronization.
	 */
	dev->power.async_in_progress = false;
	return false;
}

static void async_resume_noirq(void *data, async_cookie_t cookie)
{
	struct device *dev = data;

	device_resume_noirq(dev, pm_transition, true);
	put_device(dev);
}

static void dpm_noirq_resume_devices(pm_message_t state)
{
	struct device *dev;
	ktime_t starttime = ktime_get();

	trace_suspend_resume(TPS("dpm_resume_noirq"), state.event, true);
	mutex_lock(&dpm_list_mtx);
	pm_transition = state;

	/*
	 * Trigger the resume of "async" devices upfront so they don't have to
	 * wait for the "non-async" ones they don't depend on.
	 */
	list_for_each_entry(dev, &dpm_noirq_list, power.entry)
		dpm_async_fn(dev, async_resume_noirq);

	while (!list_empty(&dpm_noirq_list)) {
		dev = to_device(dpm_noirq_list.next);
		list_move_tail(&dev->power.entry, &dpm_late_early_list);

		if (!dev->power.async_in_progress) {
			get_device(dev);

			mutex_unlock(&dpm_list_mtx);

			device_resume_noirq(dev, state, false);

			put_device(dev);

			mutex_lock(&dpm_list_mtx);
		}
	}
	mutex_unlock(&dpm_list_mtx);
	async_synchronize_full();
	dpm_show_time(starttime, state, 0, "noirq");
	trace_suspend_resume(TPS("dpm_resume_noirq"), state.event, false);
}

/**
 * dpm_resume_noirq - Execute "noirq resume" callbacks for all devices.
 * @state: PM transition of the system being carried out.
 *
 * Invoke the "noirq" resume callbacks for all devices in dpm_noirq_list and
 * allow device drivers' interrupt handlers to be called.
 */
/*
 * haydn: the resume hang hits in the hard handler of the first pending
 * device IRQ (IRQ 38: spss, then sps with SPSS disabled), i.e. as soon as
 * resume_device_irqs() lets a handler touch its hardware, before the early
 * resume callbacks (clock controllers' pm_runtime_force_resume, IPA,
 * i2c-geni, ...) have restored what the hardware needs. haydn_late_irqs=1
 * re-enables device IRQs only after dpm_resume_early() instead.
 */
static bool haydn_late_irqs;
core_param(haydn_late_irqs, haydn_late_irqs, bool, 0644);
static bool haydn_irqs_deferred;

void dpm_resume_noirq(pm_message_t state)
{
	dpm_noirq_resume_devices(state);

	if (READ_ONCE(haydn_late_irqs)) {
		haydn_irqs_deferred = true;
	} else {
		resume_device_irqs();
		haydn_pm_stage(0x32);
	}
	device_wakeup_disarm_wake_irqs();

	cpuidle_resume();
}

/**
 * device_resume_early - Execute an "early resume" callback for given device.
 * @dev: Device to handle.
 * @state: PM transition of the system being carried out.
 * @async: If true, the device is being resumed asynchronously.
 *
 * Runtime PM is disabled for @dev while this function is being executed.
 */
static void device_resume_early(struct device *dev, pm_message_t state, bool async)
{
	pm_callback_t callback = NULL;
	const char *info = NULL;
	int error = 0;

	TRACE_DEVICE(dev);
	TRACE_RESUME(0);

	if (dev->power.syscore || dev->power.direct_complete)
		goto Out;

	if (!dev->power.is_late_suspended)
		goto Out;

	if (!dpm_wait_for_superior(dev, async))
		goto Out;

	if (dev->pm_domain) {
		info = "early power domain ";
		callback = pm_late_early_op(&dev->pm_domain->ops, state);
	} else if (dev->type && dev->type->pm) {
		info = "early type ";
		callback = pm_late_early_op(dev->type->pm, state);
	} else if (dev->class && dev->class->pm) {
		info = "early class ";
		callback = pm_late_early_op(dev->class->pm, state);
	} else if (dev->bus && dev->bus->pm) {
		info = "early bus ";
		callback = pm_late_early_op(dev->bus->pm, state);
	}
	if (callback)
		goto Run;

	if (dev_pm_skip_resume(dev))
		goto Skip;

	if (dev->driver && dev->driver->pm) {
		info = "early driver ";
		callback = pm_late_early_op(dev->driver->pm, state);
	}

Run:
	error = dpm_run_callback(callback, dev, state, info);

Skip:
	dev->power.is_late_suspended = false;

Out:
	TRACE_RESUME(error);

	pm_runtime_enable(dev);
	complete_all(&dev->power.completion);

	if (error) {
		suspend_stats.failed_resume_early++;
		dpm_save_failed_step(SUSPEND_RESUME_EARLY);
		dpm_save_failed_dev(dev_name(dev));
		pm_dev_err(dev, state, async ? " async early" : " early", error);
	}
}

static void async_resume_early(void *data, async_cookie_t cookie)
{
	struct device *dev = data;

	device_resume_early(dev, pm_transition, true);
	put_device(dev);
}

/**
 * dpm_resume_early - Execute "early resume" callbacks for all devices.
 * @state: PM transition of the system being carried out.
 */
void dpm_resume_early(pm_message_t state)
{
	struct device *dev;
	ktime_t starttime = ktime_get();

	trace_suspend_resume(TPS("dpm_resume_early"), state.event, true);
	mutex_lock(&dpm_list_mtx);
	pm_transition = state;

	/*
	 * Trigger the resume of "async" devices upfront so they don't have to
	 * wait for the "non-async" ones they don't depend on.
	 */
	list_for_each_entry(dev, &dpm_late_early_list, power.entry)
		dpm_async_fn(dev, async_resume_early);

	while (!list_empty(&dpm_late_early_list)) {
		dev = to_device(dpm_late_early_list.next);
		list_move_tail(&dev->power.entry, &dpm_suspended_list);

		if (!dev->power.async_in_progress) {
			get_device(dev);

			mutex_unlock(&dpm_list_mtx);

			device_resume_early(dev, state, false);

			put_device(dev);

			mutex_lock(&dpm_list_mtx);
		}
	}
	mutex_unlock(&dpm_list_mtx);
	async_synchronize_full();
	dpm_show_time(starttime, state, 0, "early");
	trace_suspend_resume(TPS("dpm_resume_early"), state.event, false);
	if (haydn_irqs_deferred) {
		haydn_irqs_deferred = false;
		resume_device_irqs();
		haydn_pm_stage(0x32);
	}
}

/**
 * dpm_resume_start - Execute "noirq" and "early" device callbacks.
 * @state: PM transition of the system being carried out.
 */
void dpm_resume_start(pm_message_t state)
{
	dpm_resume_noirq(state);
	dpm_resume_early(state);
}
EXPORT_SYMBOL_GPL(dpm_resume_start);

/**
 * device_resume - Execute "resume" callbacks for given device.
 * @dev: Device to handle.
 * @state: PM transition of the system being carried out.
 * @async: If true, the device is being resumed asynchronously.
 */
static void device_resume(struct device *dev, pm_message_t state, bool async)
{
	pm_callback_t callback = NULL;
	const char *info = NULL;
	int error = 0;
	DECLARE_DPM_WATCHDOG_ON_STACK(wd);

	TRACE_DEVICE(dev);
	TRACE_RESUME(0);

	if (dev->power.syscore)
		goto Complete;

	if (!dev->power.is_suspended)
		goto Complete;

	dev->power.is_suspended = false;

	if (dev->power.direct_complete) {
		/* Match the pm_runtime_disable() in __device_suspend(). */
		pm_runtime_enable(dev);
		goto Complete;
	}

	if (!dpm_wait_for_superior(dev, async))
		goto Complete;

	dpm_watchdog_set(&wd, dev);
	device_lock(dev);

	/*
	 * This is a fib.  But we'll allow new children to be added below
	 * a resumed device, even if the device hasn't been completed yet.
	 */
	dev->power.is_prepared = false;

	if (dev->pm_domain) {
		info = "power domain ";
		callback = pm_op(&dev->pm_domain->ops, state);
		goto Driver;
	}

	if (dev->type && dev->type->pm) {
		info = "type ";
		callback = pm_op(dev->type->pm, state);
		goto Driver;
	}

	if (dev->class && dev->class->pm) {
		info = "class ";
		callback = pm_op(dev->class->pm, state);
		goto Driver;
	}

	if (dev->bus) {
		if (dev->bus->pm) {
			info = "bus ";
			callback = pm_op(dev->bus->pm, state);
		} else if (dev->bus->resume) {
			info = "legacy bus ";
			callback = dev->bus->resume;
			goto End;
		}
	}

 Driver:
	if (!callback && dev->driver && dev->driver->pm) {
		info = "driver ";
		callback = pm_op(dev->driver->pm, state);
	}

 End:
	error = dpm_run_callback(callback, dev, state, info);

	device_unlock(dev);
	dpm_watchdog_clear(&wd);

 Complete:
	complete_all(&dev->power.completion);

	TRACE_RESUME(error);

	if (error) {
		suspend_stats.failed_resume++;
		dpm_save_failed_step(SUSPEND_RESUME);
		dpm_save_failed_dev(dev_name(dev));
		pm_dev_err(dev, state, async ? " async" : "", error);
	}
}

static void async_resume(void *data, async_cookie_t cookie)
{
	struct device *dev = data;

	device_resume(dev, pm_transition, true);
	put_device(dev);
}

/**
 * dpm_resume - Execute "resume" callbacks for non-sysdev devices.
 * @state: PM transition of the system being carried out.
 *
 * Execute the appropriate "resume" callback for all devices whose status
 * indicates that they are suspended.
 */
void dpm_resume(pm_message_t state)
{
	struct device *dev;
	ktime_t starttime = ktime_get();

	trace_suspend_resume(TPS("dpm_resume"), state.event, true);
	might_sleep();

	mutex_lock(&dpm_list_mtx);
	pm_transition = state;
	async_error = 0;

	/*
	 * Trigger the resume of "async" devices upfront so they don't have to
	 * wait for the "non-async" ones they don't depend on.
	 */
	list_for_each_entry(dev, &dpm_suspended_list, power.entry)
		dpm_async_fn(dev, async_resume);

	while (!list_empty(&dpm_suspended_list)) {
		dev = to_device(dpm_suspended_list.next);

		get_device(dev);

		if (!dev->power.async_in_progress) {
			mutex_unlock(&dpm_list_mtx);

			device_resume(dev, state, false);

			mutex_lock(&dpm_list_mtx);
		}

		if (!list_empty(&dev->power.entry))
			list_move_tail(&dev->power.entry, &dpm_prepared_list);

		mutex_unlock(&dpm_list_mtx);

		put_device(dev);

		mutex_lock(&dpm_list_mtx);
	}
	mutex_unlock(&dpm_list_mtx);
	async_synchronize_full();
	dpm_show_time(starttime, state, 0, NULL);

	cpufreq_resume();
	devfreq_resume();
	trace_suspend_resume(TPS("dpm_resume"), state.event, false);
}

/**
 * device_complete - Complete a PM transition for given device.
 * @dev: Device to handle.
 * @state: PM transition of the system being carried out.
 */
static void device_complete(struct device *dev, pm_message_t state)
{
	void (*callback)(struct device *) = NULL;
	const char *info = NULL;

	if (dev->power.syscore)
		goto out;

	device_lock(dev);

	if (dev->pm_domain) {
		info = "completing power domain ";
		callback = dev->pm_domain->ops.complete;
	} else if (dev->type && dev->type->pm) {
		info = "completing type ";
		callback = dev->type->pm->complete;
	} else if (dev->class && dev->class->pm) {
		info = "completing class ";
		callback = dev->class->pm->complete;
	} else if (dev->bus && dev->bus->pm) {
		info = "completing bus ";
		callback = dev->bus->pm->complete;
	}

	if (!callback && dev->driver && dev->driver->pm) {
		info = "completing driver ";
		callback = dev->driver->pm->complete;
	}

	if (callback) {
		pm_dev_dbg(dev, state, info);
		callback(dev);
	}

	device_unlock(dev);

out:
	pm_runtime_put(dev);
}

/**
 * dpm_complete - Complete a PM transition for all non-sysdev devices.
 * @state: PM transition of the system being carried out.
 *
 * Execute the ->complete() callbacks for all devices whose PM status is not
 * DPM_ON (this allows new devices to be registered).
 */
void dpm_complete(pm_message_t state)
{
	struct list_head list;

	trace_suspend_resume(TPS("dpm_complete"), state.event, true);
	might_sleep();

	INIT_LIST_HEAD(&list);
	mutex_lock(&dpm_list_mtx);
	while (!list_empty(&dpm_prepared_list)) {
		struct device *dev = to_device(dpm_prepared_list.prev);

		get_device(dev);
		dev->power.is_prepared = false;
		list_move(&dev->power.entry, &list);

		mutex_unlock(&dpm_list_mtx);

		trace_device_pm_callback_start(dev, "", state.event);
		device_complete(dev, state);
		trace_device_pm_callback_end(dev, 0);

		put_device(dev);

		mutex_lock(&dpm_list_mtx);
	}
	list_splice(&list, &dpm_list);
	mutex_unlock(&dpm_list_mtx);

	/* Allow device probing and trigger re-probing of deferred devices */
	device_unblock_probing();
	trace_suspend_resume(TPS("dpm_complete"), state.event, false);
}

/**
 * dpm_resume_end - Execute "resume" callbacks and complete system transition.
 * @state: PM transition of the system being carried out.
 *
 * Execute "resume" callbacks for all devices and complete the PM transition of
 * the system.
 */
void dpm_resume_end(pm_message_t state)
{
	dpm_resume(state);
	dpm_complete(state);
}
EXPORT_SYMBOL_GPL(dpm_resume_end);


/*------------------------- Suspend routines -------------------------*/

/**
 * resume_event - Return a "resume" message for given "suspend" sleep state.
 * @sleep_state: PM message representing a sleep state.
 *
 * Return a PM message representing the resume event corresponding to given
 * sleep state.
 */
static pm_message_t resume_event(pm_message_t sleep_state)
{
	switch (sleep_state.event) {
	case PM_EVENT_SUSPEND:
		return PMSG_RESUME;
	case PM_EVENT_FREEZE:
	case PM_EVENT_QUIESCE:
		return PMSG_RECOVER;
	case PM_EVENT_HIBERNATE:
		return PMSG_RESTORE;
	}
	return PMSG_ON;
}

static void dpm_superior_set_must_resume(struct device *dev)
{
	struct device_link *link;
	int idx;

	if (dev->parent)
		dev->parent->power.must_resume = true;

	idx = device_links_read_lock();

	list_for_each_entry_rcu_locked(link, &dev->links.suppliers, c_node)
		link->supplier->power.must_resume = true;

	device_links_read_unlock(idx);
}

/**
 * __device_suspend_noirq - Execute a "noirq suspend" callback for given device.
 * @dev: Device to handle.
 * @state: PM transition of the system being carried out.
 * @async: If true, the device is being suspended asynchronously.
 *
 * The driver of @dev will not receive interrupts while this function is being
 * executed.
 */
static int __device_suspend_noirq(struct device *dev, pm_message_t state, bool async)
{
	pm_callback_t callback = NULL;
	const char *info = NULL;
	int error = 0;

	TRACE_DEVICE(dev);
	TRACE_SUSPEND(0);

	dpm_wait_for_subordinate(dev, async);

	if (async_error)
		goto Complete;

	if (dev->power.syscore || dev->power.direct_complete)
		goto Complete;

	if (dev->pm_domain) {
		info = "noirq power domain ";
		callback = pm_noirq_op(&dev->pm_domain->ops, state);
	} else if (dev->type && dev->type->pm) {
		info = "noirq type ";
		callback = pm_noirq_op(dev->type->pm, state);
	} else if (dev->class && dev->class->pm) {
		info = "noirq class ";
		callback = pm_noirq_op(dev->class->pm, state);
	} else if (dev->bus && dev->bus->pm) {
		info = "noirq bus ";
		callback = pm_noirq_op(dev->bus->pm, state);
	}
	if (callback)
		goto Run;

	if (dev_pm_skip_suspend(dev))
		goto Skip;

	if (dev->driver && dev->driver->pm) {
		info = "noirq driver ";
		callback = pm_noirq_op(dev->driver->pm, state);
	}

Run:
	error = dpm_run_callback(callback, dev, state, info);
	if (error) {
		async_error = error;
		log_suspend_abort_reason("Device %s failed to %s noirq: error %d",
					 dev_name(dev), pm_verb(state.event), error);
		goto Complete;
	}

Skip:
	dev->power.is_noirq_suspended = true;

	/*
	 * Devices must be resumed unless they are explicitly allowed to be left
	 * in suspend, but even in that case skipping the resume of devices that
	 * were in use right before the system suspend (as indicated by their
	 * runtime PM usage counters and child counters) would be suboptimal.
	 */
	if (!(dev_pm_test_driver_flags(dev, DPM_FLAG_MAY_SKIP_RESUME) &&
	      dev->power.may_skip_resume) || !pm_runtime_need_not_resume(dev))
		dev->power.must_resume = true;

	if (dev->power.must_resume)
		dpm_superior_set_must_resume(dev);

Complete:
	complete_all(&dev->power.completion);
	TRACE_SUSPEND(error);
	return error;
}

static void async_suspend_noirq(void *data, async_cookie_t cookie)
{
	struct device *dev = data;
	int error;

	error = __device_suspend_noirq(dev, pm_transition, true);
	if (error) {
		dpm_save_failed_dev(dev_name(dev));
		pm_dev_err(dev, pm_transition, " async", error);
	}

	put_device(dev);
}

static int device_suspend_noirq(struct device *dev)
{
	if (dpm_async_fn(dev, async_suspend_noirq))
		return 0;

	return __device_suspend_noirq(dev, pm_transition, false);
}

static int dpm_noirq_suspend_devices(pm_message_t state)
{
	ktime_t starttime = ktime_get();
	int error = 0;

	trace_suspend_resume(TPS("dpm_suspend_noirq"), state.event, true);
	mutex_lock(&dpm_list_mtx);
	pm_transition = state;
	async_error = 0;

	while (!list_empty(&dpm_late_early_list)) {
		struct device *dev = to_device(dpm_late_early_list.prev);

		get_device(dev);
		mutex_unlock(&dpm_list_mtx);

		error = device_suspend_noirq(dev);

		mutex_lock(&dpm_list_mtx);

		if (error) {
			pm_dev_err(dev, state, " noirq", error);
			dpm_save_failed_dev(dev_name(dev));
		} else if (!list_empty(&dev->power.entry)) {
			list_move(&dev->power.entry, &dpm_noirq_list);
		}

		mutex_unlock(&dpm_list_mtx);

		put_device(dev);

		mutex_lock(&dpm_list_mtx);

		if (error || async_error)
			break;
	}
	mutex_unlock(&dpm_list_mtx);
	async_synchronize_full();
	if (!error)
		error = async_error;

	if (error) {
		suspend_stats.failed_suspend_noirq++;
		dpm_save_failed_step(SUSPEND_SUSPEND_NOIRQ);
	}
	dpm_show_time(starttime, state, error, "noirq");
	trace_suspend_resume(TPS("dpm_suspend_noirq"), state.event, false);
	return error;
}

/**
 * dpm_suspend_noirq - Execute "noirq suspend" callbacks for all devices.
 * @state: PM transition of the system being carried out.
 *
 * Prevent device drivers' interrupt handlers from being called and invoke
 * "noirq" suspend callbacks for all non-sysdev devices.
 */
int dpm_suspend_noirq(pm_message_t state)
{
	int ret;

	cpuidle_pause();

	device_wakeup_arm_wake_irqs();
	suspend_device_irqs();

	ret = dpm_noirq_suspend_devices(state);
	if (ret)
		dpm_resume_noirq(resume_event(state));

	return ret;
}

static void dpm_propagate_wakeup_to_parent(struct device *dev)
{
	struct device *parent = dev->parent;

	if (!parent)
		return;

	spin_lock_irq(&parent->power.lock);

	if (dev->power.wakeup_path && !parent->power.ignore_children)
		parent->power.wakeup_path = true;

	spin_unlock_irq(&parent->power.lock);
}

/**
 * __device_suspend_late - Execute a "late suspend" callback for given device.
 * @dev: Device to handle.
 * @state: PM transition of the system being carried out.
 * @async: If true, the device is being suspended asynchronously.
 *
 * Runtime PM is disabled for @dev while this function is being executed.
 */
static int __device_suspend_late(struct device *dev, pm_message_t state, bool async)
{
	pm_callback_t callback = NULL;
	const char *info = NULL;
	int error = 0;

	TRACE_DEVICE(dev);
	TRACE_SUSPEND(0);

	__pm_runtime_disable(dev, false);

	dpm_wait_for_subordinate(dev, async);

	if (async_error)
		goto Complete;

	if (pm_wakeup_pending()) {
		async_error = -EBUSY;
		goto Complete;
	}

	if (dev->power.syscore || dev->power.direct_complete)
		goto Complete;

	if (dev->pm_domain) {
		info = "late power domain ";
		callback = pm_late_early_op(&dev->pm_domain->ops, state);
	} else if (dev->type && dev->type->pm) {
		info = "late type ";
		callback = pm_late_early_op(dev->type->pm, state);
	} else if (dev->class && dev->class->pm) {
		info = "late class ";
		callback = pm_late_early_op(dev->class->pm, state);
	} else if (dev->bus && dev->bus->pm) {
		info = "late bus ";
		callback = pm_late_early_op(dev->bus->pm, state);
	}
	if (callback)
		goto Run;

	if (dev_pm_skip_suspend(dev))
		goto Skip;

	if (dev->driver && dev->driver->pm) {
		info = "late driver ";
		callback = pm_late_early_op(dev->driver->pm, state);
	}

Run:
	error = dpm_run_callback(callback, dev, state, info);
	if (error) {
		async_error = error;
		log_suspend_abort_reason("Device %s failed to %s late: error %d",
					 dev_name(dev), pm_verb(state.event), error);
		goto Complete;
	}
	dpm_propagate_wakeup_to_parent(dev);

Skip:
	dev->power.is_late_suspended = true;

Complete:
	TRACE_SUSPEND(error);
	complete_all(&dev->power.completion);
	return error;
}

static void async_suspend_late(void *data, async_cookie_t cookie)
{
	struct device *dev = data;
	int error;

	error = __device_suspend_late(dev, pm_transition, true);
	if (error) {
		dpm_save_failed_dev(dev_name(dev));
		pm_dev_err(dev, pm_transition, " async", error);
	}
	put_device(dev);
}

static int device_suspend_late(struct device *dev)
{
	if (dpm_async_fn(dev, async_suspend_late))
		return 0;

	return __device_suspend_late(dev, pm_transition, false);
}

/**
 * dpm_suspend_late - Execute "late suspend" callbacks for all devices.
 * @state: PM transition of the system being carried out.
 */
int dpm_suspend_late(pm_message_t state)
{
	ktime_t starttime = ktime_get();
	int error = 0;

	trace_suspend_resume(TPS("dpm_suspend_late"), state.event, true);
	mutex_lock(&dpm_list_mtx);
	pm_transition = state;
	async_error = 0;

	while (!list_empty(&dpm_suspended_list)) {
		struct device *dev = to_device(dpm_suspended_list.prev);

		get_device(dev);

		mutex_unlock(&dpm_list_mtx);

		error = device_suspend_late(dev);

		mutex_lock(&dpm_list_mtx);

		if (!list_empty(&dev->power.entry))
			list_move(&dev->power.entry, &dpm_late_early_list);

		if (error) {
			pm_dev_err(dev, state, " late", error);
			dpm_save_failed_dev(dev_name(dev));
		}

		mutex_unlock(&dpm_list_mtx);

		put_device(dev);

		mutex_lock(&dpm_list_mtx);

		if (error || async_error)
			break;
	}
	mutex_unlock(&dpm_list_mtx);
	async_synchronize_full();
	if (!error)
		error = async_error;
	if (error) {
		suspend_stats.failed_suspend_late++;
		dpm_save_failed_step(SUSPEND_SUSPEND_LATE);
		dpm_resume_early(resume_event(state));
	}
	dpm_show_time(starttime, state, error, "late");
	trace_suspend_resume(TPS("dpm_suspend_late"), state.event, false);
	return error;
}

/**
 * dpm_suspend_end - Execute "late" and "noirq" device suspend callbacks.
 * @state: PM transition of the system being carried out.
 */
int dpm_suspend_end(pm_message_t state)
{
	ktime_t starttime = ktime_get();
	int error;

	error = dpm_suspend_late(state);
	if (error)
		goto out;

	error = dpm_suspend_noirq(state);
	if (error)
		dpm_resume_early(resume_event(state));

out:
	dpm_show_time(starttime, state, error, "end");
	return error;
}
EXPORT_SYMBOL_GPL(dpm_suspend_end);

/**
 * legacy_suspend - Execute a legacy (bus or class) suspend callback for device.
 * @dev: Device to suspend.
 * @state: PM transition of the system being carried out.
 * @cb: Suspend callback to execute.
 * @info: string description of caller.
 */
static int legacy_suspend(struct device *dev, pm_message_t state,
			  int (*cb)(struct device *dev, pm_message_t state),
			  const char *info)
{
	int error;
	ktime_t calltime;

	calltime = initcall_debug_start(dev, cb);

	trace_device_pm_callback_start(dev, info, state.event);
	error = cb(dev, state);
	trace_device_pm_callback_end(dev, error);
	suspend_report_result(cb, error);

	initcall_debug_report(dev, calltime, cb, error);

	return error;
}

static void dpm_clear_superiors_direct_complete(struct device *dev)
{
	struct device_link *link;
	int idx;

	if (dev->parent) {
		spin_lock_irq(&dev->parent->power.lock);
		dev->parent->power.direct_complete = false;
		spin_unlock_irq(&dev->parent->power.lock);
	}

	idx = device_links_read_lock();

	list_for_each_entry_rcu_locked(link, &dev->links.suppliers, c_node) {
		spin_lock_irq(&link->supplier->power.lock);
		link->supplier->power.direct_complete = false;
		spin_unlock_irq(&link->supplier->power.lock);
	}

	device_links_read_unlock(idx);
}

/**
 * __device_suspend - Execute "suspend" callbacks for given device.
 * @dev: Device to handle.
 * @state: PM transition of the system being carried out.
 * @async: If true, the device is being suspended asynchronously.
 */
static int __device_suspend(struct device *dev, pm_message_t state, bool async)
{
	pm_callback_t callback = NULL;
	const char *info = NULL;
	int error = 0;
	DECLARE_DPM_WATCHDOG_ON_STACK(wd);

	TRACE_DEVICE(dev);
	TRACE_SUSPEND(0);

	dpm_wait_for_subordinate(dev, async);

	if (async_error) {
		dev->power.direct_complete = false;
		goto Complete;
	}

	/*
	 * Wait for possible runtime PM transitions of the device in progress
	 * to complete and if there's a runtime resume request pending for it,
	 * resume it before proceeding with invoking the system-wide suspend
	 * callbacks for it.
	 *
	 * If the system-wide suspend callbacks below change the configuration
	 * of the device, they must disable runtime PM for it or otherwise
	 * ensure that its runtime-resume callbacks will not be confused by that
	 * change in case they are invoked going forward.
	 */
	pm_runtime_barrier(dev);

	if (pm_wakeup_pending()) {
		dev->power.direct_complete = false;
		async_error = -EBUSY;
		goto Complete;
	}

	if (dev->power.syscore)
		goto Complete;

	/* Avoid direct_complete to let wakeup_path propagate. */
	if (device_may_wakeup(dev) || dev->power.wakeup_path)
		dev->power.direct_complete = false;

	if (dev->power.direct_complete) {
		if (pm_runtime_status_suspended(dev)) {
			pm_runtime_disable(dev);
			if (pm_runtime_status_suspended(dev)) {
				pm_dev_dbg(dev, state, "direct-complete ");
				dev->power.is_suspended = true;
				goto Complete;
			}

			pm_runtime_enable(dev);
		}
		dev->power.direct_complete = false;
	}

	dev->power.may_skip_resume = true;
	dev->power.must_resume = !dev_pm_test_driver_flags(dev, DPM_FLAG_MAY_SKIP_RESUME);

	dpm_watchdog_set(&wd, dev);
	device_lock(dev);

	if (dev->pm_domain) {
		info = "power domain ";
		callback = pm_op(&dev->pm_domain->ops, state);
		goto Run;
	}

	if (dev->type && dev->type->pm) {
		info = "type ";
		callback = pm_op(dev->type->pm, state);
		goto Run;
	}

	if (dev->class && dev->class->pm) {
		info = "class ";
		callback = pm_op(dev->class->pm, state);
		goto Run;
	}

	if (dev->bus) {
		if (dev->bus->pm) {
			info = "bus ";
			callback = pm_op(dev->bus->pm, state);
		} else if (dev->bus->suspend) {
			pm_dev_dbg(dev, state, "legacy bus ");
			error = legacy_suspend(dev, state, dev->bus->suspend,
						"legacy bus ");
			goto End;
		}
	}

 Run:
	if (!callback && dev->driver && dev->driver->pm) {
		info = "driver ";
		callback = pm_op(dev->driver->pm, state);
	}

	error = dpm_run_callback(callback, dev, state, info);

 End:
	if (!error) {
		dev->power.is_suspended = true;
		if (device_may_wakeup(dev))
			dev->power.wakeup_path = true;

		dpm_propagate_wakeup_to_parent(dev);
		dpm_clear_superiors_direct_complete(dev);
	} else {
		log_suspend_abort_reason("Device %s failed to %s: error %d",
					 dev_name(dev), pm_verb(state.event), error);
	}

	device_unlock(dev);
	dpm_watchdog_clear(&wd);

 Complete:
	if (error)
		async_error = error;

	complete_all(&dev->power.completion);
	TRACE_SUSPEND(error);
	return error;
}

static void async_suspend(void *data, async_cookie_t cookie)
{
	struct device *dev = data;
	int error;

	error = __device_suspend(dev, pm_transition, true);
	if (error) {
		dpm_save_failed_dev(dev_name(dev));
		pm_dev_err(dev, pm_transition, " async", error);
	}

	put_device(dev);
}

static int device_suspend(struct device *dev)
{
	if (dpm_async_fn(dev, async_suspend))
		return 0;

	return __device_suspend(dev, pm_transition, false);
}

/**
 * dpm_suspend - Execute "suspend" callbacks for all non-sysdev devices.
 * @state: PM transition of the system being carried out.
 */
int dpm_suspend(pm_message_t state)
{
	ktime_t starttime = ktime_get();
	int error = 0;

	trace_suspend_resume(TPS("dpm_suspend"), state.event, true);
	might_sleep();

	devfreq_suspend();
	cpufreq_suspend();

	mutex_lock(&dpm_list_mtx);
	pm_transition = state;
	async_error = 0;
	while (!list_empty(&dpm_prepared_list)) {
		struct device *dev = to_device(dpm_prepared_list.prev);

		get_device(dev);

		mutex_unlock(&dpm_list_mtx);

		error = device_suspend(dev);

		mutex_lock(&dpm_list_mtx);

		if (error) {
			pm_dev_err(dev, state, "", error);
			dpm_save_failed_dev(dev_name(dev));
		} else if (!list_empty(&dev->power.entry)) {
			list_move(&dev->power.entry, &dpm_suspended_list);
		}

		mutex_unlock(&dpm_list_mtx);

		put_device(dev);

		mutex_lock(&dpm_list_mtx);

		if (error || async_error)
			break;
	}
	mutex_unlock(&dpm_list_mtx);
	async_synchronize_full();
	if (!error)
		error = async_error;
	if (error) {
		suspend_stats.failed_suspend++;
		dpm_save_failed_step(SUSPEND_SUSPEND);
	}
	dpm_show_time(starttime, state, error, NULL);
	trace_suspend_resume(TPS("dpm_suspend"), state.event, false);
	return error;
}

/**
 * device_prepare - Prepare a device for system power transition.
 * @dev: Device to handle.
 * @state: PM transition of the system being carried out.
 *
 * Execute the ->prepare() callback(s) for given device.  No new children of the
 * device may be registered after this function has returned.
 */
static int device_prepare(struct device *dev, pm_message_t state)
{
	int (*callback)(struct device *) = NULL;
	int ret = 0;

	/*
	 * If a device's parent goes into runtime suspend at the wrong time,
	 * it won't be possible to resume the device.  To prevent this we
	 * block runtime suspend here, during the prepare phase, and allow
	 * it again during the complete phase.
	 */
	pm_runtime_get_noresume(dev);

	if (dev->power.syscore)
		return 0;

	device_lock(dev);

	dev->power.wakeup_path = false;

	if (dev->power.no_pm_callbacks)
		goto unlock;

	if (dev->pm_domain)
		callback = dev->pm_domain->ops.prepare;
	else if (dev->type && dev->type->pm)
		callback = dev->type->pm->prepare;
	else if (dev->class && dev->class->pm)
		callback = dev->class->pm->prepare;
	else if (dev->bus && dev->bus->pm)
		callback = dev->bus->pm->prepare;

	if (!callback && dev->driver && dev->driver->pm)
		callback = dev->driver->pm->prepare;

	if (callback)
		ret = callback(dev);

unlock:
	device_unlock(dev);

	if (ret < 0) {
		suspend_report_result(callback, ret);
		pm_runtime_put(dev);
		return ret;
	}
	/*
	 * A positive return value from ->prepare() means "this device appears
	 * to be runtime-suspended and its state is fine, so if it really is
	 * runtime-suspended, you can leave it in that state provided that you
	 * will do the same thing with all of its descendants".  This only
	 * applies to suspend transitions, however.
	 */
	spin_lock_irq(&dev->power.lock);
	dev->power.direct_complete = state.event == PM_EVENT_SUSPEND &&
		(ret > 0 || dev->power.no_pm_callbacks) &&
		!dev_pm_test_driver_flags(dev, DPM_FLAG_NO_DIRECT_COMPLETE);
	spin_unlock_irq(&dev->power.lock);
	return 0;
}

/**
 * dpm_prepare - Prepare all non-sysdev devices for a system PM transition.
 * @state: PM transition of the system being carried out.
 *
 * Execute the ->prepare() callback(s) for all devices.
 */
int dpm_prepare(pm_message_t state)
{
	int error = 0;

	trace_suspend_resume(TPS("dpm_prepare"), state.event, true);
	might_sleep();

	/*
	 * Give a chance for the known devices to complete their probes, before
	 * disable probing of devices. This sync point is important at least
	 * at boot time + hibernation restore.
	 */
	wait_for_device_probe();
	/*
	 * It is unsafe if probing of devices will happen during suspend or
	 * hibernation and system behavior will be unpredictable in this case.
	 * So, let's prohibit device's probing here and defer their probes
	 * instead. The normal behavior will be restored in dpm_complete().
	 */
	device_block_probing();

	mutex_lock(&dpm_list_mtx);
	while (!list_empty(&dpm_list) && !error) {
		struct device *dev = to_device(dpm_list.next);

		get_device(dev);

		mutex_unlock(&dpm_list_mtx);

		trace_device_pm_callback_start(dev, "", state.event);
		error = device_prepare(dev, state);
		trace_device_pm_callback_end(dev, error);

		mutex_lock(&dpm_list_mtx);

		if (!error) {
			dev->power.is_prepared = true;
			if (!list_empty(&dev->power.entry))
				list_move_tail(&dev->power.entry, &dpm_prepared_list);
		} else if (error == -EAGAIN) {
			error = 0;
		} else {
			dev_info(dev, "not prepared for power transition: code %d\n",
				 error);
			log_suspend_abort_reason("Device %s not prepared for power transition: code %d",
						 dev_name(dev), error);
			dpm_save_failed_dev(dev_name(dev));
		}

		mutex_unlock(&dpm_list_mtx);

		put_device(dev);

		mutex_lock(&dpm_list_mtx);
	}
	mutex_unlock(&dpm_list_mtx);
	trace_suspend_resume(TPS("dpm_prepare"), state.event, false);
	return error;
}

/**
 * dpm_suspend_start - Prepare devices for PM transition and suspend them.
 * @state: PM transition of the system being carried out.
 *
 * Prepare all non-sysdev devices for system PM transition and execute "suspend"
 * callbacks for them.
 */
int dpm_suspend_start(pm_message_t state)
{
	ktime_t starttime = ktime_get();
	int error;

	error = dpm_prepare(state);
	if (error) {
		suspend_stats.failed_prepare++;
		dpm_save_failed_step(SUSPEND_PREPARE);
	} else
		error = dpm_suspend(state);
	dpm_show_time(starttime, state, error, "start");
	return error;
}
EXPORT_SYMBOL_GPL(dpm_suspend_start);

void __suspend_report_result(const char *function, void *fn, int ret)
{
	if (ret)
		pr_err("%s(): %pS returns %d\n", function, fn, ret);
}
EXPORT_SYMBOL_GPL(__suspend_report_result);

/**
 * device_pm_wait_for_dev - Wait for suspend/resume of a device to complete.
 * @subordinate: Device that needs to wait for @dev.
 * @dev: Device to wait for.
 */
int device_pm_wait_for_dev(struct device *subordinate, struct device *dev)
{
	dpm_wait(dev, subordinate->power.async_suspend);
	return async_error;
}
EXPORT_SYMBOL_GPL(device_pm_wait_for_dev);

/**
 * dpm_for_each_dev - device iterator.
 * @data: data for the callback.
 * @fn: function to be called for each device.
 *
 * Iterate over devices in dpm_list, and call @fn for each device,
 * passing it @data.
 */
void dpm_for_each_dev(void *data, void (*fn)(struct device *, void *))
{
	struct device *dev;

	if (!fn)
		return;

	device_pm_lock();
	list_for_each_entry(dev, &dpm_list, power.entry)
		fn(dev, data);
	device_pm_unlock();
}
EXPORT_SYMBOL_GPL(dpm_for_each_dev);

static bool pm_ops_is_empty(const struct dev_pm_ops *ops)
{
	if (!ops)
		return true;

	return !ops->prepare &&
	       !ops->suspend &&
	       !ops->suspend_late &&
	       !ops->suspend_noirq &&
	       !ops->resume_noirq &&
	       !ops->resume_early &&
	       !ops->resume &&
	       !ops->complete;
}

void device_pm_check_callbacks(struct device *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->power.lock, flags);
	dev->power.no_pm_callbacks =
		(!dev->bus || (pm_ops_is_empty(dev->bus->pm) &&
		 !dev->bus->suspend && !dev->bus->resume)) &&
		(!dev->class || pm_ops_is_empty(dev->class->pm)) &&
		(!dev->type || pm_ops_is_empty(dev->type->pm)) &&
		(!dev->pm_domain || pm_ops_is_empty(&dev->pm_domain->ops)) &&
		(!dev->driver || (pm_ops_is_empty(dev->driver->pm) &&
		 !dev->driver->suspend && !dev->driver->resume));
	spin_unlock_irqrestore(&dev->power.lock, flags);
}

bool dev_pm_skip_suspend(struct device *dev)
{
	return dev_pm_test_driver_flags(dev, DPM_FLAG_SMART_SUSPEND) &&
		pm_runtime_status_suspended(dev);
}
