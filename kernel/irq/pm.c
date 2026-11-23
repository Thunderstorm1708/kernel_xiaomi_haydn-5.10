// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2009 Rafael J. Wysocki <rjw@sisk.pl>, Novell Inc.
 *
 * This file contains power management functions related to interrupts.
 */

#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/moduleparam.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>

#include "internals.h"

bool irq_pm_check_wakeup(struct irq_desc *desc)
{
	if (irqd_is_wakeup_armed(&desc->irq_data)) {
		irqd_clear(&desc->irq_data, IRQD_WAKEUP_ARMED);
		desc->istate |= IRQS_SUSPENDED | IRQS_PENDING;
		desc->depth++;
		irq_disable(desc);
		pm_system_irq_wakeup(irq_desc_get_irq(desc));
		return true;
	}
	return false;
}

/*
 * Called from __setup_irq() with desc->lock held after @action has
 * been installed in the action chain.
 */
void irq_pm_install_action(struct irq_desc *desc, struct irqaction *action)
{
	desc->nr_actions++;

	if (action->flags & IRQF_FORCE_RESUME)
		desc->force_resume_depth++;

	WARN_ON_ONCE(desc->force_resume_depth &&
		     desc->force_resume_depth != desc->nr_actions);

	if (action->flags & IRQF_NO_SUSPEND)
		desc->no_suspend_depth++;
	else if (action->flags & IRQF_COND_SUSPEND)
		desc->cond_suspend_depth++;

	WARN_ON_ONCE(desc->no_suspend_depth &&
		     (desc->no_suspend_depth +
			desc->cond_suspend_depth) != desc->nr_actions);
}

/*
 * Called from __free_irq() with desc->lock held after @action has
 * been removed from the action chain.
 */
void irq_pm_remove_action(struct irq_desc *desc, struct irqaction *action)
{
	desc->nr_actions--;

	if (action->flags & IRQF_FORCE_RESUME)
		desc->force_resume_depth--;

	if (action->flags & IRQF_NO_SUSPEND)
		desc->no_suspend_depth--;
	else if (action->flags & IRQF_COND_SUSPEND)
		desc->cond_suspend_depth--;
}

static bool suspend_device_irq(struct irq_desc *desc)
{
	unsigned long chipflags = irq_desc_get_chip(desc)->flags;
	struct irq_data *irqd = &desc->irq_data;

	if (!desc->action || irq_desc_is_chained(desc) ||
	    desc->no_suspend_depth)
		return false;

	if (irqd_is_wakeup_set(irqd)) {
		irqd_set(irqd, IRQD_WAKEUP_ARMED);

		if ((chipflags & IRQCHIP_ENABLE_WAKEUP_ON_SUSPEND) &&
		     irqd_irq_disabled(irqd)) {
			/*
			 * Interrupt marked for wakeup is in disabled state.
			 * Enable interrupt here to unmask/enable in irqchip
			 * to be able to resume with such interrupts.
			 */
			__enable_irq(desc);
			irqd_set(irqd, IRQD_IRQ_ENABLED_ON_SUSPEND);
		}
		/*
		 * We return true here to force the caller to issue
		 * synchronize_irq(). We need to make sure that the
		 * IRQD_WAKEUP_ARMED is visible before we return from
		 * suspend_device_irqs().
		 */
		return true;
	}

	desc->istate |= IRQS_SUSPENDED;
	__disable_irq(desc);

	/*
	 * Hardware which has no wakeup source configuration facility
	 * requires that the non wakeup interrupts are masked at the
	 * chip level. The chip implementation indicates that with
	 * IRQCHIP_MASK_ON_SUSPEND.
	 */
	if (chipflags & IRQCHIP_MASK_ON_SUSPEND)
		mask_irq(desc);
	return true;
}

/**
 * suspend_device_irqs - disable all currently enabled interrupt lines
 *
 * During system-wide suspend or hibernation device drivers need to be
 * prevented from receiving interrupts and this function is provided
 * for this purpose.
 *
 * So we disable all interrupts and mark them IRQS_SUSPENDED except
 * for those which are unused, those which are marked as not
 * suspendable via an interrupt request with the flag IRQF_NO_SUSPEND
 * set and those which are marked as active wakeup sources.
 *
 * The active wakeup sources are handled by the flow handler entry
 * code which checks for the IRQD_WAKEUP_ARMED flag, suspends the
 * interrupt and notifies the pm core about the wakeup.
 */
void suspend_device_irqs(void)
{
	struct irq_desc *desc;
	int irq;

	for_each_irq_desc(irq, desc) {
		unsigned long flags;
		bool sync;

		if (irq_settings_is_nested_thread(desc))
			continue;
		raw_spin_lock_irqsave(&desc->lock, flags);
		sync = suspend_device_irq(desc);
		raw_spin_unlock_irqrestore(&desc->lock, flags);

		if (sync)
			synchronize_irq(irq);
	}
}
EXPORT_SYMBOL_GPL(suspend_device_irqs);

static void resume_irq(struct irq_desc *desc)
{
	struct irq_data *irqd = &desc->irq_data;

	irqd_clear(irqd, IRQD_WAKEUP_ARMED);

	if (irqd_is_enabled_on_suspend(irqd)) {
		/*
		 * Interrupt marked for wakeup was enabled during suspend
		 * entry. Disable such interrupts to restore them back to
		 * original state.
		 */
		__disable_irq(desc);
		irqd_clear(irqd, IRQD_IRQ_ENABLED_ON_SUSPEND);
	}

	if (desc->istate & IRQS_SUSPENDED)
		goto resume;

	/* Force resume the interrupt? */
	if (!desc->force_resume_depth)
		return;

	/* Pretend that it got disabled ! */
	desc->depth++;
	irq_state_set_disabled(desc);
	irq_state_set_masked(desc);
resume:
	desc->istate &= ~IRQS_SUSPENDED;
	__enable_irq(desc);
}

/* haydn: PMIC SDAM breadcrumb, drivers/base/power/main.c */
void haydn_pm_irq(unsigned int irq);
void haydn_pm_irq_delay(void);
bool haydn_pm_irq_wanted(void);

/*
 * haydn: #182 showed the reset is tied to device IRQs coming back (a 300 ms
 * pause before them changes nothing, no hard handler runs in the window).
 * With haydn_irq_hold_max=N, suspended IRQs <= N are left masked by
 * resume_irqs() and released one by one, 1 ms apart, after all devices
 * resumed (before step 0x3f). Breadcrumb bit 24 = released late; with bit 30
 * the 1 ms after it passed. The last record names the IRQ whose unmask kills
 * the SoC. Can be changed at runtime to bisect.
 */
static unsigned int haydn_irq_hold_max;
core_param(haydn_irq_hold_max, haydn_irq_hold_max, uint, 0644);
static DECLARE_BITMAP(haydn_held, 64);

void haydn_release_held_irqs(void)
{
	unsigned int irq;

	for_each_set_bit(irq, haydn_held, 64) {
		struct irq_desc *desc = irq_to_desc(irq);
		unsigned long flags;

		clear_bit(irq, haydn_held);
		if (!desc)
			continue;
		haydn_pm_irq(irq | 0x01000000u);
		raw_spin_lock_irqsave(&desc->lock, flags);
		resume_irq(desc);
		raw_spin_unlock_irqrestore(&desc->lock, flags);
		mdelay(1);
		haydn_pm_irq(irq | 0x01000000u | 0x40000000u);
	}
}

static void resume_irqs(bool want_early)
{
	bool haydn = !want_early && haydn_pm_irq_wanted();
	struct irq_desc *desc;
	int irq;

	if (haydn)
		haydn_pm_irq_delay();

	for_each_irq_desc(irq, desc) {
		unsigned long flags;
		bool is_early = desc->action &&
			desc->action->flags & IRQF_EARLY_RESUME;

		if (!is_early && want_early)
			continue;
		if (irq_settings_is_nested_thread(desc))
			continue;
		if (haydn && irq < 64 && irq <= READ_ONCE(haydn_irq_hold_max) &&
		    (desc->istate & IRQS_SUSPENDED)) {
			set_bit(irq, haydn_held);
			continue;
		}

		/*
		 * haydn: the resume hang comes right after the last noirq
		 * resume callback, i.e. when device IRQs are re-enabled here.
		 * Record each suspended IRQ before enabling it and give a
		 * pending interrupt time to run, so the breadcrumb names the
		 * IRQ whose handler hangs.
		 */
		/*
		 * Wake IRQs are not IRQS_SUSPENDED (they stay armed through
		 * suspend) but resume_irq() replays a pending one here, so
		 * record those too (#152 showed the last suspended IRQ only).
		 */
		if (haydn && ((desc->istate & IRQS_SUSPENDED) ||
			      irqd_is_wakeup_armed(&desc->irq_data)))
			haydn_pm_irq(irq | (irqd_is_wakeup_armed(&desc->irq_data) ?
					    0x80000000u : 0));
		/*
		 * haydn: #172 proved the records reliable (a good resume reaches
		 * step 0x3f) and the hang lies between "before IRQ 38" and
		 * "IRQ 38 + 50 us". Split that window: bit 28 = the descriptor
		 * lock was busy, bit 27 = resume_irq() returned (depth in bits
		 * 16-23), bit 30 = 50 us later.
		 */
		if (haydn && irq == 38) {
			if (!raw_spin_trylock_irqsave(&desc->lock, flags)) {
				haydn_pm_irq(irq | 0x10000000u);
				raw_spin_lock_irqsave(&desc->lock, flags);
			}
		} else {
			raw_spin_lock_irqsave(&desc->lock, flags);
		}
		resume_irq(desc);
		raw_spin_unlock_irqrestore(&desc->lock, flags);
		if (haydn && irq == 38)
			haydn_pm_irq(irq | 0x08000000u |
				     ((desc->depth & 0xffu) << 16));
		if (haydn && ((desc->istate & IRQS_SUSPENDED) ||
			      irq == 38)) {
			udelay(50);
			/* bit 30: resume_irq() returned and 50 us passed */
			haydn_pm_irq(irq | 0x40000000u);
		} else if (haydn) {
			udelay(50);
		}
	}
}

/**
 * rearm_wake_irq - rearm a wakeup interrupt line after signaling wakeup
 * @irq: Interrupt to rearm
 */
void rearm_wake_irq(unsigned int irq)
{
	unsigned long flags;
	struct irq_desc *desc = irq_get_desc_buslock(irq, &flags, IRQ_GET_DESC_CHECK_GLOBAL);

	if (!desc)
		return;

	if (!(desc->istate & IRQS_SUSPENDED) ||
	    !irqd_is_wakeup_set(&desc->irq_data))
		goto unlock;

	desc->istate &= ~IRQS_SUSPENDED;
	irqd_set(&desc->irq_data, IRQD_WAKEUP_ARMED);
	__enable_irq(desc);

unlock:
	irq_put_desc_busunlock(desc, flags);
}

/**
 * irq_pm_syscore_ops - enable interrupt lines early
 *
 * Enable all interrupt lines with %IRQF_EARLY_RESUME set.
 */
static void irq_pm_syscore_resume(void)
{
	resume_irqs(true);
}

static struct syscore_ops irq_pm_syscore_ops = {
	.resume		= irq_pm_syscore_resume,
};

static int __init irq_pm_init_ops(void)
{
	register_syscore_ops(&irq_pm_syscore_ops);
	return 0;
}

device_initcall(irq_pm_init_ops);

/**
 * resume_device_irqs - enable interrupt lines disabled by suspend_device_irqs()
 *
 * Enable all non-%IRQF_EARLY_RESUME interrupt lines previously
 * disabled by suspend_device_irqs() that have the IRQS_SUSPENDED flag
 * set as well as those with %IRQF_FORCE_RESUME.
 */
void resume_device_irqs(void)
{
	resume_irqs(false);
}
EXPORT_SYMBOL_GPL(resume_device_irqs);
