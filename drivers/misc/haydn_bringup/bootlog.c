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
	int i, m, seen = 0;

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

static void append(const char *s, size_t n)
{
	while (n && used < max_pages * PAGE_SIZE) {
		size_t pg = used / PAGE_SIZE, off = used % PAGE_SIZE;
		size_t k = min(n, PAGE_SIZE - off);

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
	for (i = 20; i > 0; i -= 5) {
		pr_info("bootlog: ===== holding %d s for photos =====\n", i);
		ssleep(5);
	}
	WRITE_ONCE(haydn_probe_trace, true);
	pr_info("bootlog: probe trace on\n");
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
	while (!kthread_should_stop() && tick < 600) {
		if (time_after_eq(jiffies, beat)) {
			beat = jiffies + 2 * HZ;
			pr_info("bootlog: tick %d\n", tick);
			if (tick == 15 || tick == 45 || tick == 75)
				show_tasks();
			if (tick == 3 || tick == 12 || tick == 30 || tick == 60)
				driver_deferred_probe_dump();
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
	mutex_lock(&bootlog_lock);
	while (kmsg_dump_get_line(&sync_iter, false, line, sizeof(line), &len))
		append(line, len);
	if (log_bdev && used > done)
		flush();
	mutex_unlock(&bootlog_lock);
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
