// SPDX-License-Identifier: GPL-2.0
/*
 * Keep the kernel log on the logdump partition while testing ACK 5.10 on
 * haydn with a 5.4 vendor ROM. Runs in kernel context via bios so it keeps
 * flushing after Android's init turns SELinux enforcing on.
 *
 * From the haydn ACK 5.10 bring-up tree (xiaomi-lahaina-devs/5.10-sm8350-H),
 * used here with the author's permission.
 *
 * Read it back from recovery with:
 *   dd if=/dev/block/by-name/logdump bs=1M count=16 | tr -d '\0'
 */
#include <linux/bio.h>
#include <linux/completion.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/kmsg_dump.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/haydn_bootlog.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/vmalloc.h>

#define LOG_PAGES	4096	/* 16 MiB */
#define SCAN_MINORS	1024

static struct page *pages[LOG_PAGES];
static size_t used, done, max_pages = LOG_PAGES;
static struct block_device *log_bdev;
static bool bootlog_flushed;
static DEFINE_MUTEX(bootlog_lock);
static struct kmsg_dumper sync_iter = { .active = true };

bool haydn_bootlog_ready(void)
{
	return READ_ONCE(bootlog_flushed);
}
static struct task_struct *task;
static struct task_struct *panic_tasks[2];
static atomic_t panic_req = ATOMIC_INIT(0);
static atomic_t panic_done = ATOMIC_INIT(0);
static bool dumped_parts;

static bool is_logdump(struct block_device *bdev)
{
	return bdev->bd_part && bdev->bd_part->info &&
	       !strcmp(bdev->bd_part->info->volname, "logdump");
}

#define HEADER		"HAYDN510ANDROID seq="
#define HEADER_LEN	(sizeof(HEADER) - 1)
#define SEQ_DIGITS	8

static long log_seq(struct block_device *bdev)
{
	struct page *p = alloc_page(GFP_KERNEL | __GFP_ZERO);
	struct bio *bio;
	long seq = -2;
	char digits[SEQ_DIGITS + 1];
	u8 *head;
	int i, err;

	if (!p)
		return -2;
	bio = bio_alloc(GFP_KERNEL, 1);
	if (!bio) {
		__free_page(p);
		return -2;
	}
	bio_set_dev(bio, bdev);
	bio->bi_opf = REQ_OP_READ;
	bio->bi_iter.bi_sector = 0;
	bio_add_page(bio, p, PAGE_SIZE, 0);
	err = submit_bio_wait(bio);
	bio_put(bio);
	if (err)
		pr_info("bootlog: reading %pg failed: %d\n", bdev, err);
	if (!err) {
		head = page_address(p);
		for (i = 0; i < 64 && !head[i]; i++)
			;
		if (i == 64) {
			seq = -1;
		} else if (!memcmp(head, HEADER, HEADER_LEN)) {
			memcpy(digits, head + HEADER_LEN, SEQ_DIGITS);
			digits[SEQ_DIGITS] = 0;
			if (kstrtol(digits, 10, &seq) || seq < 0)
				seq = 0;
		} else {
			seq = 0;
		}
	}
	__free_page(p);
	return seq;
}

static long my_seq;

static int count_disk(struct device *dev, void *data)
{
	const char *n = dev_name(dev);

	if (dev->type == &disk_type && n && (!strncmp(n, "sd", 2) || !strncmp(n, "mmcblk", 6)))
		(*(int *)data)++;
	return 0;
}

/* no disk yet: don't probe minors (each miss is a module_request + avc line) */
static bool have_disk(void)
{
	int n = 0;

	class_for_each_device(&block_class, NULL, &n, count_disk);
	return n > 0;
}

static struct block_device *find_logdump(void)
{
	static const unsigned int majors[] = { 8, 259 };
	dev_t best = 0;
	long best_seq = LONG_MAX, max_seq = 0;
	loff_t best_size = 0;
	struct block_device *bdev;
	int i, m, seen = 0, logs = 0;
	static unsigned long next_try;

	/* rescanning every minor is costly; retry at most twice a second */
	if (next_try && time_before(jiffies, next_try))
		return NULL;
	next_try = jiffies + HZ / 2;

	for (i = 0; i < ARRAY_SIZE(majors); i++) {
		for (m = 0; m < SCAN_MINORS; m++) {
			loff_t sz;

			bdev = blkdev_get_by_dev(MKDEV(majors[i], m), FMODE_READ, NULL);
			if (IS_ERR(bdev))
				continue;
			sz = i_size_read(bdev->bd_inode);
			if (bdev->bd_part && bdev->bd_part->info &&
			    bdev->bd_part->info->volname[0]) {
				seen++;
				if (!dumped_parts &&
				    (strstr(bdev->bd_part->info->volname, "log") ||
				     strstr(bdev->bd_part->info->volname, "dump")))
					pr_info("bootlog: candidate %pg vol=%s size=%lld\n",
						bdev, bdev->bd_part->info->volname,
						(long long)sz);
			}
			if (is_logdump(bdev) && sz >= 262144) {
				long seq = log_seq(bdev);

				logs++;
				pr_info("bootlog: %pg (size=%lld) holds log %ld\n",
					bdev, (long long)sz, seq);
				if (seq > max_seq)
					max_seq = seq;
				if (seq >= -1 && seq < best_seq) {
					best_seq = seq;
					best = bdev->bd_dev;
					best_size = sz;
				}
			}
			blkdev_put(bdev, FMODE_READ);
		}
	}
	if (seen && !dumped_parts) {
		dumped_parts = true;
		if (!best)
			pr_warn("bootlog: no logdump found among %d named partitions\n", seen);
	}
	if (!best) {
		return NULL;
	}
	/*
	 * haydn has two logdump partitions (sda24, sde59) on different UFS
	 * LUNs, and sde can show up later than sda. Choosing with only one in
	 * sight wrote over the newest log (the crash we wanted) and gave both
	 * the same number. Wait for both, up to 30 s after boot.
	 */
	if (logs < 2 && jiffies_to_msecs(jiffies - INITIAL_JIFFIES) < 30000) {
		pr_info_ratelimited("bootlog: %d logdump partition(s) so far, waiting for the second\n",
				    logs);
		return NULL;
	}
	max_pages = min_t(size_t, LOG_PAGES, (size_t)(best_size >> PAGE_SHIFT));
	my_seq = max_seq + 1;
	bdev = blkdev_get_by_dev(best, FMODE_READ | FMODE_WRITE, NULL);
	if (!IS_ERR(bdev))
		pr_info("bootlog: writing log %ld to %pg (max_pages=%zu)\n",
			my_seq, bdev, max_pages);
	else
		return NULL;
	return bdev;
}

static void flush(void);

/*
 * Once the buffer is full, keep the first quarter (the boot) and wrap the
 * rest as a ring, so a freeze long after boot is still in the log. The
 * wrap point shows up in the dump as a jump back in the timestamps: the
 * newest lines are the ones right before that jump.
 */
static void wrap(void)
{
	static const char mark[] = "\nbootlog: ===== ring wrapped =====\n";
	size_t start = (max_pages / 4) * PAGE_SIZE;

	if (log_bdev && used > done)
		flush();
	used = done = start;
	memcpy(page_address(pages[used / PAGE_SIZE]), mark, sizeof(mark) - 1);
	used += sizeof(mark) - 1;
}

static void append(const char *s, size_t n)
{
	while (n) {
		size_t pg, off, k;

		if (used >= max_pages * PAGE_SIZE) {
			if (!log_bdev || max_pages < 8)
				return;
			wrap();
		}
		pg = used / PAGE_SIZE;
		off = used % PAGE_SIZE;
		k = min(n, PAGE_SIZE - off);
		if (!pages[pg]) {
			pages[pg] = alloc_page(GFP_KERNEL | __GFP_ZERO);
			if (!pages[pg])
				return;
		}
		memcpy(page_address(pages[pg]) + off, s, k);
		used += k;
		s += k;
		n -= k;
	}
}

static void flush(void)
{
	size_t first = done / PAGE_SIZE, last = DIV_ROUND_UP(used, PAGE_SIZE);
	size_t pg = first;

	while (pg < last) {
		unsigned int nr = min_t(size_t, last - pg, BIO_MAX_PAGES);
		struct bio *bio = bio_alloc(GFP_KERNEL, nr);
		unsigned int i;

		if (!bio)
			return;
		bio_set_dev(bio, log_bdev);
		bio->bi_opf = REQ_OP_WRITE | REQ_SYNC | REQ_FUA;
		bio->bi_iter.bi_sector = (sector_t)pg << (PAGE_SHIFT - SECTOR_SHIFT);
		for (i = 0; i < nr; i++)
			bio_add_page(bio, pages[pg + i], PAGE_SIZE, 0);
		if (submit_bio_wait(bio)) {
			bio_put(bio);
			return;
		}
		bio_put(bio);
		pg += nr;
	}
	done = used;
}

/* CPU frequency and which CPUs the scheduler may use (paused/isolated) */
static void show_cpufreq(void)
{
	int cpu;

	pr_info("bootlog: cpus online=%*pbl active=%*pbl\n",
		cpumask_pr_args(cpu_online_mask),
		cpumask_pr_args(cpu_active_mask));
	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *pol = cpufreq_cpu_get(cpu);

		if (!pol)
			continue;
		if (pol->cpu == cpu)
			pr_info("bootlog: cpufreq cpu%d gov=%s cur=%u min=%u max=%u hw_max=%u\n",
				cpu, pol->governor ? pol->governor->name : "-",
				pol->cur, pol->min, pol->max,
				pol->cpuinfo.max_freq);
		cpufreq_cpu_put(pol);
	}
}

static void show_tasks(void)
{
	struct task_struct *p, *t;

	rcu_read_lock();
	for_each_process(p)
		if (p->mm)
			pr_info("bootlog: ps %d %d %c %s\n", task_pid_nr(p), task_ppid_nr(p),
				task_state_to_char(p), p->comm);
	for_each_process_thread(p, t) {
		long state = READ_ONCE(t->state);

		if ((state & TASK_UNINTERRUPTIBLE) && !(state & TASK_NOLOAD))
			sched_show_task(t);
	}
	rcu_read_unlock();
}

/*
 * Called by kernel_init() right before /init runs: every built-in driver
 * has had its chance, so the deferred list shows what is still missing.
 * Hold it on screen for 15 s, then log every probe that follows (first
 * stage init loading modules) so the last line before a silent reset
 * names the device.
 */
void haydn_bootlog_before_init(void)
{
	int i;

	pr_info("bootlog: ===== before /init: devices still waiting =====\n");
	driver_deferred_probe_dump();
	for (i = 5; i > 0; i -= 5) {
		pr_info("bootlog: ===== holding %d s for photos =====\n", i);
		ssleep(5);
	}
	WRITE_ONCE(haydn_probe_trace, true);
	pr_info("bootlog: probe trace on\n");
}

/*
 * Copy the tail of the previous boot's pstore records (ramoops: panic dmesg
 * and console) into this boot's log. Resets during suspend never reach the
 * log partition (storage is already suspended), but a panic - e.g. from the
 * DPM watchdog - is kept in ramoops across the warm reset. OrangeFox cannot
 * read it and Android needs root, so do it here. Called with the lock held.
 */
static void dump_pstore_file(const char *path, size_t tail)
{
	struct file *f;
	loff_t size, pos;
	char *buf;
	ssize_t n;
	char hdr[160];
	int hl;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return;
	size = i_size_read(file_inode(f));
	if (size <= 0)
		size = 2 << 20;
	buf = vmalloc(tail);
	if (!buf)
		goto out;
	pos = size > tail ? size - tail : 0;
	n = kernel_read(f, buf, tail, &pos);
	hl = scnprintf(hdr, sizeof(hdr),
		       "==== bootlog: previous boot %s (last %zd bytes) ====\n",
		       path, n > 0 ? n : 0);
	append(hdr, hl);
	if (n > 0)
		append(buf, n);
	{
		static const char end[] = "==== bootlog: end of pstore record ====\n";

		append(end, strlen(end));
	}
	vfree(buf);
out:
	filp_close(f, NULL);
}

static void dump_pstore(void)
{
	dump_pstore_file("/sys/fs/pstore/dmesg-ramoops-0", 96 << 10);
	dump_pstore_file("/sys/fs/pstore/console-ramoops-0", 48 << 10);
	dump_pstore_file("/sys/fs/pstore/console-ramoops", 48 << 10);
}

static int bootlog_thread(void *unused)
{
	static char line[1024];
	struct kmsg_dumper *iter = &sync_iter;
	unsigned long start = jiffies, beat = jiffies;
	int tick = 0;
	size_t len;

	mutex_lock(&bootlog_lock);
	kmsg_dump_rewind(iter);
	append(HEADER "00000000\n", HEADER_LEN + SEQ_DIGITS + 1);
	mutex_unlock(&bootlog_lock);
	while (!kthread_should_stop()) {
		if (time_after_eq(jiffies, beat)) {
			beat = jiffies + 2 * HZ;
			pr_info("bootlog: tick %d\n", tick);
			if (tick % 15 == 0)
				show_cpufreq();
			if (tick == 15 || tick == 45 || tick == 75)
				show_tasks();
			if (tick == 3 || tick == 12 || tick == 30 || tick == 60)
				driver_deferred_probe_dump();
			if (tick == 20) {
				mutex_lock(&bootlog_lock);
				dump_pstore();
				mutex_unlock(&bootlog_lock);
			}
			tick++;
		}
		mutex_lock(&bootlog_lock);
		while (kmsg_dump_get_line(iter, false, line, sizeof(line), &len))
			append(line, len);
		if (!log_bdev && have_disk()) {
			log_bdev = find_logdump();
			if (log_bdev) {
				char seq[SEQ_DIGITS + 1];

				snprintf(seq, sizeof(seq), "%0*ld", SEQ_DIGITS, my_seq % 100000000);
				memcpy(page_address(pages[0]) + HEADER_LEN, seq, SEQ_DIGITS);
			}
		}
		if (log_bdev && used > done) {
			flush();
			WRITE_ONCE(bootlog_flushed, true);
		}
		mutex_unlock(&bootlog_lock);
		msleep_interruptible(log_bdev ? (tick < 60 ? 5 : 50) : 50);
	}
	pr_info("bootlog: stopped after %u ms\n", jiffies_to_msecs(jiffies - start));
	mutex_lock(&bootlog_lock);
	while (kmsg_dump_get_line(iter, false, line, sizeof(line), &len))
		append(line, len);
	if (log_bdev && used > done)
		flush();
	mutex_unlock(&bootlog_lock);
	while (!kthread_should_stop())
		msleep_interruptible(1000);
	return 0;
}
void haydn_bootlog_flush_now(void)
{
	static char line[1024];
	size_t len;

	if (!READ_ONCE(bootlog_flushed))
		return;

	/*
	 * Never block here: callers can run with interrupts disabled or in
	 * atomic context (PCM trigger, codec DAI ops), and flush() sleeps in
	 * submit_bio_wait(). If the lock is busy, skip; the kthread flushes
	 * on its own schedule.
	 */
	if (!mutex_trylock(&bootlog_lock))
		return;
	while (kmsg_dump_get_line(&sync_iter, false, line, sizeof(line), &len))
		append(line, len);
	if (log_bdev && used > done)
		flush();
	mutex_unlock(&bootlog_lock);
}

EXPORT_SYMBOL_GPL(haydn_bootlog_flush_now);

/*
 * Like haydn_bootlog_flush_now(), but for sleepable callers that want the
 * line they just printed on storage before a step that may hang the whole
 * SoC (WLAN WoW at system suspend): the last marker in logdump then names
 * the step. Bounded, because it runs inside system suspend where UFS may
 * already be suspending in parallel (async suspend): wait at most 100 ms
 * for the lock and 250 ms for the write, then give up instead of blocking.
 */
static DECLARE_COMPLETION(sync_bio_done);
static bool sync_bio_busy;

static void sync_bio_end(struct bio *bio)
{
	bio_put(bio);
	complete(&sync_bio_done);
}

static void bootlog_sync(int lock_tries, long timeout)
{
	static char line[1024];
	size_t len, first, last;
	unsigned int nr, i;
	struct bio *bio;
	int tries;

	if (!READ_ONCE(bootlog_flushed) || in_atomic() || irqs_disabled())
		return;
	for (tries = 0; !mutex_trylock(&bootlog_lock); tries++) {
		if (tries >= lock_tries)
			return;
		msleep(5);
	}
	if (sync_bio_busy) {
		if (!try_wait_for_completion(&sync_bio_done))
			goto out;
		sync_bio_busy = false;
	}
	while (kmsg_dump_get_line(&sync_iter, false, line, sizeof(line), &len))
		append(line, len);
	if (!log_bdev || used <= done)
		goto out;
	first = done / PAGE_SIZE;
	last = DIV_ROUND_UP(used, PAGE_SIZE);
	nr = min_t(size_t, last - first, BIO_MAX_PAGES);
	bio = bio_alloc(GFP_NOIO, nr);
	if (!bio)
		goto out;
	bio_set_dev(bio, log_bdev);
	bio->bi_opf = REQ_OP_WRITE | REQ_SYNC | REQ_FUA;
	bio->bi_iter.bi_sector = (sector_t)first << (PAGE_SHIFT - SECTOR_SHIFT);
	for (i = 0; i < nr; i++)
		bio_add_page(bio, pages[first + i], PAGE_SIZE, 0);
	bio->bi_end_io = sync_bio_end;
	reinit_completion(&sync_bio_done);
	sync_bio_busy = true;
	submit_bio(bio);
	if (wait_for_completion_timeout(&sync_bio_done, timeout)) {
		sync_bio_busy = false;
		if (first + nr >= last)
			done = used;
	}
out:
	mutex_unlock(&bootlog_lock);
}

void haydn_bootlog_sync(void)
{
	bootlog_sync(20, HZ / 4);
}
EXPORT_SYMBOL_GPL(haydn_bootlog_sync);

/*
 * For the PM core's per-device suspend markers: hundreds of calls per
 * suspend, some after UFS is already suspended. Never wait for the lock,
 * wait at most 50 ms for the write, and skip outright while an earlier
 * write is still pending (storage asleep).
 */
void haydn_bootlog_sync_fast(void)
{
	bootlog_sync(0, HZ / 20);
}

static int panic_flusher_fn(void *unused)
{
	static char panic_line[1024];
	size_t len;

	while (!kthread_should_stop()) {
		if (atomic_read(&panic_req) && READ_ONCE(bootlog_flushed)) {
			if (mutex_trylock(&bootlog_lock)) {
				while (kmsg_dump_get_line(&sync_iter, false,
						panic_line, sizeof(panic_line), &len))
					append(panic_line, len);
				if (log_bdev && used > done)
					flush();
				mutex_unlock(&bootlog_lock);
				atomic_set(&panic_done, 1);
			}
		}
		msleep_interruptible(10);
	}
	return 0;
}

void haydn_bootlog_panic_wait(void)
{
	int cpu = raw_smp_processor_id();
	int idx = (cpu == 0) ? 1 : 0;
	int i;

	if (!READ_ONCE(bootlog_flushed) || !panic_tasks[idx])
		return;
	atomic_set(&panic_done, 0);
	atomic_set(&panic_req, 1);
	wake_up_process(panic_tasks[idx]);
	for (i = 0; i < 300 && !atomic_read(&panic_done); i++)
		mdelay(1);
}

static int bootlog_reboot(struct notifier_block *nb, unsigned long action, void *data)
{
	if (log_bdev && used > done)
		flush();
	return NOTIFY_DONE;
}

static struct notifier_block bootlog_nb = {
	.notifier_call = bootlog_reboot,
	.priority = INT_MIN,
};

static int __init bootlog_init(void)
{
	register_reboot_notifier(&bootlog_nb);
	task = kthread_run(bootlog_thread, NULL, "bootlog");
	panic_tasks[0] = kthread_create(panic_flusher_fn, NULL, "bootlog_p0");
	if (!IS_ERR(panic_tasks[0])) {
		kthread_bind(panic_tasks[0], 0);
		wake_up_process(panic_tasks[0]);
	} else {
		panic_tasks[0] = NULL;
	}
	panic_tasks[1] = kthread_create(panic_flusher_fn, NULL, "bootlog_p1");
	if (!IS_ERR(panic_tasks[1])) {
		int c = cpumask_last(cpu_online_mask);
		if (c == 0 && num_online_cpus() > 1)
			c = cpumask_next(0, cpu_online_mask);
		kthread_bind(panic_tasks[1], c);
		wake_up_process(panic_tasks[1]);
	} else {
		panic_tasks[1] = NULL;
	}
	return PTR_ERR_OR_ZERO(task);
}

static void __exit bootlog_exit(void)
{
	int i;

	kthread_stop(task);
	if (log_bdev)
		blkdev_put(log_bdev, FMODE_READ | FMODE_WRITE);
	for (i = 0; i < LOG_PAGES; i++)
		if (pages[i])
			__free_page(pages[i]);
}

fs_initcall(bootlog_init);
module_exit(bootlog_exit);
MODULE_DESCRIPTION("Kernel log writer to logdump partition for haydn 5.10 bring-up");
MODULE_LICENSE("GPL");
