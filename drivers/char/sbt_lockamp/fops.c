/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SBT Instruments Lock-in Amplifier
 *
 * Copyright (c) 2019, Frederik Peter Aalund <fpa@sbtinstruments.com>
 */
#include <asm/io.h>
#include <linux/circ_buf.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/pm_runtime.h>
#include <linux/time.h>
#include <uapi/linux/sched/types.h>

#include "lockin_amplifier.h"
#include "hw.h"
#include "pm.h"

struct chunk_header {
	u64 last_start_time_ns;
	u64 time_step_ns;
} __attribute__((packed));
_Static_assert (16 == sizeof(struct chunk_header), "struct 'chunk_header' is not packed on this platform");

struct chunk_info {
	struct chunk_header header;
	size_t data_size_n;
};

unsigned int lockamp_ma_time_step_ns = 0;
u64 lockamp_fifo_read_duration = 0;
u64 lockamp_fifo_read_delay = 0;
int lockamp_debug1 = 0;
int lockamp_debug2 = 0;
int lockamp_debug3 = 0;

static unsigned int open_count = 0;

#ifdef CONFIG_SBT_LOCKAMP_USE_SBUF

struct task_struct *thread = NULL;
static u64 last_ma_time_ns = 0;
static unsigned int ma_factor = 20;


static int increase_task_priority(struct task_struct *t)
{
	int ret;
	struct sched_param param = { .sched_priority = 99 };
	ret = sched_setscheduler(t, SCHED_FIFO, &param);
	if (ret < 0) {
		return ret;
	}
	set_user_nice(t, -20);
	return ret;
}

static void update_ma_time_ns(size_t size_n_since_last)
{
	u64 ma_time_ns;
	unsigned int ma_delta_ns;
	unsigned int time_step_ns;
	ma_time_ns = ktime_get_ns();
	ma_delta_ns = ma_time_ns - last_ma_time_ns;
	last_ma_time_ns = ma_time_ns;
	if (0 < size_n_since_last) {
		time_step_ns = ma_delta_ns / size_n_since_last;
		lockamp_ma_time_step_ns = (time_step_ns + (ma_factor - 1) * lockamp_ma_time_step_ns) / ma_factor;
	}
}

static u64 sleep_until_fifo_half_full(struct lockamp *lockamp)
{
	u64 before_sleep_ns;
	u64 after_sleep_ns;
	u64 ret;
	unsigned long target_sleep_ns;
	unsigned long sleep_upper_us;
	unsigned long sleep_lower_us;
	/* Profile begin */
	before_sleep_ns = ktime_get_ns();
	/* Target sleep duration. E.g., 178 ms */
	ret = lockamp_get_read_delay_ns(lockamp, &target_sleep_ns);
	if (ret < 0) {
		return ret;
	}
	/* Sleep range. E.g., 168 ms to 178 ms */
	sleep_upper_us = max(((long)target_sleep_ns - (long)lockamp_fifo_read_duration) / 1000, 3000L);
	sleep_lower_us = max((long)sleep_upper_us - 10000, 2000L);
	lockamp_debug2 = sleep_lower_us;
	lockamp_debug3 = sleep_upper_us;
	usleep_range(sleep_lower_us, sleep_upper_us);
	/* Profile end */
	after_sleep_ns = ktime_get_ns();
	/* Return actual sleep duration */
	return after_sleep_ns - before_sleep_ns;
}

static int fifo_to_sbuf(void *data)
{
	struct lockamp *lockamp = data;
	size_t size_n;
	u64 start, end;
	/* Continuously poll the FIFO */
	for (;;) {
		if (kthread_should_stop()) {
			break;
		}
		/* Profile begin */
		start = ktime_get_ns();
		/* Actual work */
		mutex_lock(&lockamp->signal_buf_m);
		size_n = lockamp_fifo_move_to_sbuf(lockamp);
		update_ma_time_ns(size_n);
		mutex_unlock(&lockamp->signal_buf_m);
		/* Profile end */
		end = ktime_get_ns();
		lockamp_fifo_read_duration = end - start;
		/* Wait for data */
		lockamp_fifo_read_delay = sleep_until_fifo_half_full(lockamp);
		if (lockamp_fifo_read_delay < 0) {
			dev_warn_ratelimited(lockamp->dev, "Could not determine FIFO read delay. Will sleep for 250 ms. Data loss may occur.\n");
			msleep(250);
		}
	}
	return 0;
}

#endif

static void reset_start_time(struct lockamp *lockamp)
{
	lockamp->last_start_time_ns = ktime_get_ns();
}


/*
 * Character Device Functions
 */
static int device_open(struct inode *inode, struct file *file)
{
	int ret;
	struct lockamp *lockamp;
	lockamp = container_of(inode->i_cdev, struct lockamp, cdev);
	file->private_data = lockamp;
	/* Only a single reader at a time */
	if (0 < open_count) {
		return -EBUSY;
	}
	++open_count;

#ifdef CONFIG_SBT_LOCKAMP_USE_SBUF
	/* power */
	ret = lockamp_pm_get(lockamp);
	if (ret < 0) {
		dev_err(lockamp->dev, "Failed to get pm runtime: %d\n", ret);
		goto out_open_count;
	}

	/* start buffering thread if there is a signal buffer */
	thread = kthread_create(fifo_to_sbuf, lockamp, "lockamp0");
	if (IS_ERR(thread)) {
		dev_alert(lockamp->dev, "Failed to create kthread.\n");
		ret = PTR_ERR(thread);
		thread = NULL;
		goto out_pm;
	}
	ret = increase_task_priority(thread);
	if (ret < 0) {
		goto out_thread;
	}
	wake_up_process(thread);
#endif

	/* Synchronization */
	lockamp->last_desyncs = atomic_read(&lockamp->desyncs);
	reset_start_time(lockamp);
	/* Out */
	ret = 0;
	goto out;

#ifdef CONFIG_SBT_LOCKAMP_USE_SBUF
out_thread:
	if (thread) {
		kthread_stop(thread);
		thread = NULL;
	}
out_pm:
	lockamp_pm_put(lockamp);
out_open_count:
#endif

	--open_count;
out:
	return ret;
}

static int device_release(struct inode *inode, struct file *file)
{
#ifdef CONFIG_SBT_LOCKAMP_USE_SBUF
	struct lockamp *lockamp;
	lockamp = container_of(inode->i_cdev, struct lockamp, cdev);
	if (thread) {
		kthread_stop(thread);
		thread = NULL;
	}
	lockamp_pm_put(lockamp);
#endif
	--open_count;
	return 0;
}

#ifdef CONFIG_SBT_LOCKAMP_USE_SBUF

static ssize_t pop_chunk_to_user(struct circ_sample_buf *cbuf,
                                 struct csbuf_snapshot *cbuf_snap,
                                 char __user *buffer, size_t length)
{
	size_t cbuf_size_to_end_n = (size_t)CIRC_CNT_TO_END(cbuf_snap->head,
	                                         cbuf_snap->tail, cbuf->capacity_n);
	size_t chunk_size = cbuf_size_to_end_n * sizeof(struct sample);
	size_t copy_length = min(chunk_size, length);
	size_t copy_length_n = copy_length / sizeof(struct sample);
	ptrdiff_t new_tail;
	if (copy_to_user(buffer, cbuf->buf + cbuf_snap->tail, copy_length)) {
		return -EFAULT;
	}
	new_tail = (cbuf_snap->tail + copy_length_n) & (cbuf->capacity_n - 1);
	smp_store_release(&cbuf->tail, new_tail);
	cbuf_snap->tail = new_tail;
	return copy_length;
}

static ssize_t pop_to_user(struct circ_sample_buf *cbuf,
                           struct csbuf_snapshot *cbuf_snap,
                           char __user *buffer, size_t length)
{
	ssize_t ret;
	char *pos = buffer;
	/* Read the first contiguous chunk of data */
	ret = pop_chunk_to_user(cbuf, cbuf_snap, pos, length);
	if (ret < 0) {
		return ret;
	}
	pos += ret;
	length -= ret;
	/* Read the second contiguous chunk of data */
	ret = pop_chunk_to_user(cbuf, cbuf_snap, pos, length);
	if (ret < 0) {
		return ret;
	}
	pos += ret;
	length -= ret;
	return pos - buffer;
}

static int chunk_get_info(struct lockamp *lockamp, size_t usr_buf_length,
                          struct csbuf_snapshot *sbuf_snap,
                          struct chunk_info *info)
{
	size_t data_size_n;
	unsigned int time_step_ns;
	int ret;
	/* The user-provided buffer can not contain a chunk */
	if (sizeof(struct chunk_header) > usr_buf_length) {
		return -EINVAL;
	}
	ret = lockamp_get_time_step_ns(lockamp, &time_step_ns);
	if (ret < 0) {
		return ret;
	}
	data_size_n = (usr_buf_length - sizeof(struct chunk_header)) / sizeof(struct sample);
	data_size_n = min(data_size_n, sbuf_snap->size_n);
	info->header.last_start_time_ns = lockamp->last_start_time_ns;
	info->header.time_step_ns = time_step_ns;
	info->data_size_n = data_size_n;
	return 0;
}

static int chunk_commit_info(struct lockamp *lockamp, struct chunk_info *info)
{
	u64 duration_ns;
	int ret = lockamp_get_duration_ns(lockamp, info->data_size_n, &duration_ns);
	if (ret < 0) {
		return ret;
	}
	lockamp->last_start_time_ns += duration_ns;
	return 0;
}

static ssize_t write_header_to_user(struct lockamp *lockamp,
                                    struct chunk_header *header,
                                    char __user *buffer, size_t length)
{
	if (copy_to_user(buffer, header, sizeof(struct chunk_header))) {
		dev_alert(lockamp->dev, "Failed to copy chunk header to user space buffer.\n");
		return -EFAULT;
	}
	return sizeof(struct chunk_header);
}

static void reader_get_sbuf_snapshot(struct lockamp *lockamp,
                                     struct csbuf_snapshot *snap)
{
	snap->head = smp_load_acquire(&lockamp->signal_buf.head);
	snap->tail = lockamp->signal_buf.tail;
	snap->size_n = (size_t)CIRC_CNT(snap->head, snap->tail, lockamp->signal_buf.capacity_n);
	if (lockamp->signal_buf.capacity_n - 1 == snap->size_n) {
		atomic_inc(&lockamp->desyncs);
		dev_warn_ratelimited(lockamp->dev, "Data loss. Signal buffer was not popped in time and has reached its maximum capacity.\n");
	}
}

#endif

static void synchronize(struct lockamp *lockamp)
{
	if (atomic_read(&lockamp->desyncs) != lockamp->last_desyncs) {
		dev_warn(lockamp->dev, "Resetting start time due to desync.\n");
		reset_start_time(lockamp);
	}
	lockamp->last_desyncs = atomic_read(&lockamp->desyncs);
}

static ssize_t device_read(
	struct file *filp,
	__user char *buffer,
	size_t length,
	loff_t *offset)
{
	ssize_t ret;
	struct lockamp *lockamp = filp->private_data;
#ifdef CONFIG_SBT_LOCKAMP_USE_SBUF
	char *pos = buffer;
	struct chunk_info info;
	struct csbuf_snapshot sbuf_snap;
	reader_get_sbuf_snapshot(lockamp, &sbuf_snap);
	synchronize(lockamp);
	ret = chunk_get_info(lockamp, length, &sbuf_snap, &info);
	if (ret < 0) {
		return ret;
	}
	/* Chunk header */
	ret = write_header_to_user(lockamp, &info.header, pos, length);
	if (ret < 0) {
		dev_alert(lockamp->dev, "Failed to copy header to user space buffer.\n");
		return ret;
	}
	pos += ret;
	length = info.data_size_n * sizeof(struct sample);
	/* Chunk data */
	ret = pop_to_user(&lockamp->signal_buf, &sbuf_snap, pos, length);
	if (ret < 0) {
		dev_alert(lockamp->dev, "Failed to copy chunk data to user space buffer.\n");
		return ret;
	}
	pos += ret;
	length -= ret;
	/* Effectuate the write */
	ret = chunk_commit_info(lockamp, &info);
	if (ret < 0) {
		return ret;
	}
	return pos - buffer;
#else
	size_t fifo_size_n;
	size_t buffer_size_n = length / sizeof(struct sample);
	size_t bounded_size_n;
	size_t bounded_size;
	char *kbuf = NULL;
	char *pos;
	int i;
	/* get power */
	ret = lockamp_pm_get(lockamp);
	if (ret < 0) {
		dev_err(lockamp->dev, "Failed to get pm runtime: %d\n", ret);
		ret = -EFAULT;
		goto out;
	}
	/* bounds */
	fifo_size_n = lockamp_fifo_size_n(lockamp);
	bounded_size_n = min(fifo_size_n, buffer_size_n);
	bounded_size = bounded_size_n * sizeof(struct sample);
	/* allocate kernel memory */
	kbuf = vmalloc(bounded_size);
	if (NULL == kbuf) {
		dev_err(lockamp->dev, "Failed to allocate kernel buffer.\n");
		ret = -ENOMEM;
		goto out_pm;
	}
	/* copy from FIFO into kernel buffer */
	synchronize(lockamp);
	pos = kbuf;
	for (i = 0; bounded_size_n != i; ++i) {
		lockamp_fifo_pop_sample(lockamp, (struct sample*)pos);
		pos += sizeof(struct sample);
	}
	/* copy from kernel space to user space */
	if (copy_to_user(buffer, kbuf, bounded_size)) {
		dev_err(lockamp->dev, "Failed to copy memory to user space.\n");
		ret = -EFAULT;
		goto out_kbuf;
	}
	ret = bounded_size;
	goto out_pm;
out_kbuf:
	vfree(kbuf);
out_pm:
	lockamp_pm_put(lockamp);
out:
	return ret;
#endif
}

static ssize_t device_write(struct file *filp, const char __user *buff,
                            size_t len, loff_t * off)
{
	return -EPERM;
}

struct file_operations lockamp_fops = {
	.owner = THIS_MODULE,
	.read = device_read,
	.write = device_write,
	.open = device_open,
	.release = device_release
};
