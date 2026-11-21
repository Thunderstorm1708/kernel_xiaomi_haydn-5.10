/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HAYDN_BOOTLOG_H
#define _LINUX_HAYDN_BOOTLOG_H
#if IS_BUILTIN(CONFIG_HAYDN_BOOTLOG)
bool haydn_bootlog_ready(void);
void haydn_bootlog_flush_now(void);
void haydn_bootlog_sync(void);
void haydn_bootlog_sync_fast(void);
void haydn_bootlog_panic_wait(void);
void driver_deferred_probe_dump(void);
extern bool haydn_probe_trace;
void haydn_bootlog_before_init(void);
#else
static inline bool haydn_bootlog_ready(void) { return false; }
static inline void haydn_bootlog_flush_now(void) { }
static inline void haydn_bootlog_sync(void) { }
static inline void haydn_bootlog_sync_fast(void) { }
static inline void haydn_bootlog_panic_wait(void) { }
static inline void driver_deferred_probe_dump(void) { }
static inline void haydn_bootlog_before_init(void) { }
#endif
#endif
