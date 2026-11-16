/* SPDX-License-Identifier: GPL-2.0-only */
/* 5.10 compat for the msm-5.4 haydn qcacld-3.0 / qca-wifi-host-cmn sources */
#ifndef _QCACLD_COMPAT510_H
#define _QCACLD_COMPAT510_H
#include <linux/kconfig.h>
#include <linux/types.h>
#include <net/netlink.h>
#include <linux/pm_qos.h>
#include <linux/qcom-iommu-util.h>

#ifndef NLA_EXACT_LEN
#define NLA_EXACT_LEN NLA_BINARY
#endif

/* WALT "wake up idle" hint (5.4) is gone */
#define set_wake_up_idle(enable) 0

/*
 * PM_QOS_CPU_DMA_LATENCY notifiers were removed in 5.7; the driver only
 * used them to follow latency votes for IRQ affinity tuning.
 */
#ifndef PM_QOS_CPU_DMA_LATENCY
#define PM_QOS_CPU_DMA_LATENCY 1
#endif
#define pm_qos_add_notifier(cls, nb) ((void)(nb), 0)
#define pm_qos_remove_notifier(cls, nb) ((void)(nb), 0)
#endif
