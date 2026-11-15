/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * 5.10 compat for the (generated/obfuscated) msm-5.4 datarmnet-ext sources:
 * y2038 time types and the netlink exact-length policy are gone.
 */
#ifndef _DATARMNET_EXT_COMPAT510_H
#define _DATARMNET_EXT_COMPAT510_H
#include <linux/kconfig.h>
#include <linux/types.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <net/netlink.h>

typedef time64_t time_t;
#define timespec timespec64
#define getnstimeofday(ts) ktime_get_real_ts64(ts)
#define ktime_to_timespec(kt) ktime_to_timespec64(kt)
#define timespec_to_ns(ts) timespec64_to_ns(ts)
#ifndef NLA_EXACT_LEN
#define NLA_EXACT_LEN NLA_BINARY
#endif
#ifndef cpu_isolated
/* 5.10: no WALT core isolation API; treat every CPU as available */
#define cpu_isolated(cpu) 0
#endif
#endif
