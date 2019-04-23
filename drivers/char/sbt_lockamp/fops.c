#include <asm/io.h>
#include <linux/circ_buf.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/sched/types.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "lockin_amplifier.h"
#include "hw.h"

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

struct task_struct *thread;
static unsigned int open_count = 0;
static u64 last_ma_time_ns = 0;
static unsigned int ma_factor = 20;

static int increase_task_priority(struct task_struct *t)
{
	int result;
	struct sched_param param = { .sched_priority = 99 };
	result = sched_setscheduler(t, SCHED_FIFO, &param);
	if (0 != result) {
		return result;
	}
	set_user_nice(t, -20);
	return result;
}

static void update_ma_time_ns(size_t size_n_since_last)
{
	u64 ma_time_ns;
	unsigned int ma_delta_ns;
	unsigned int time_step_ns;
	struct timespec ts;
	getnstimeofday(&ts);
	ma_time_ns = timespec_to_ns(&ts);
	ma_delta_ns = ma_time_ns - last_ma_time_ns;
	last_ma_time_ns = ma_time_ns;
	if (0 < size_n_since_last) {
		time_step_ns = ma_delta_ns / size_n_since_last;
		lockamp_ma_time_step_ns = (time_step_ns + (ma_factor - 1) * lockamp_ma_time_step_ns) / ma_factor;
	}
}

static u64 sleep_until_fifo_half_full(struct lockamp *lockamp)
{
	struct timespec ts;
	u64 before_sleep_ns;
	u64 after_sleep_ns;
	unsigned long target_sleep_ns;
	unsigned long sleep_upper_us;
	unsigned long sleep_lower_us;
	/* Profile begin */
	getnstimeofday(&ts);
	before_sleep_ns = timespec_to_ns(&ts);
	/* Target sleep duration. E.g., 178 ms */
	target_sleep_ns = lockamp_get_read_delay_ns(lockamp);
	/* Sleep range. E.g., 168 ms to 178 ms */
	sleep_upper_us = max(((long)target_sleep_ns - (long)lockamp_fifo_read_duration) / 1000, 3000L);
	sleep_lower_us = max((long)sleep_upper_us - 10000, 2000L);
	lockamp_debug2 = sleep_lower_us;
	lockamp_debug3 = sleep_upper_us;
	usleep_range(sleep_lower_us, sleep_upper_us);
	/* Profile end */
	getnstimeofday(&ts);
	after_sleep_ns = timespec_to_ns(&ts);
	/* Teturn actual sleep duration */
	return after_sleep_ns - before_sleep_ns;
}

static int fifo_to_sbuf(void *data)
{
	struct lockamp *lockamp = data;
	size_t size_n;
	struct timespec ts;
	u64 start, end;
	/* Continously poll the FIFO */
	for (;;) {
		if (kthread_should_stop()) {
			break;
		}
		/* Profile begin */
		getnstimeofday(&ts);
		start = timespec_to_ns(&ts);
		/* Actual work */
		mutex_lock(&lockamp->signal_buf_m);
		size_n = lockamp_fifo_move_to_sbuf(lockamp);
		update_ma_time_ns(size_n);
		mutex_unlock(&lockamp->signal_buf_m);
		/* Profile end */
		getnstimeofday(&ts);
		end = timespec_to_ns(&ts);
		lockamp_fifo_read_duration = end - start;
		/* Wait for data */
		lockamp_fifo_read_delay = sleep_until_fifo_half_full(lockamp);
	}
	return 0;
}

static void reset_start_time(struct lockamp *lockamp)
{
	struct timespec ts;
	getnstimeofday(&ts);
	lockamp->last_start_time_ns = timespec_to_ns(&ts);
}


/*
 * Character Device Functions
 */
static int device_open(struct inode *inode, struct file *file)
{
	int result;
	struct lockamp *lockamp;
	lockamp = container_of(inode->i_cdev, struct lockamp, cdev);
	file->private_data = lockamp;
	/* Only a single reader at a time */
	if (0 < open_count)
		return -EBUSY;
	++open_count;
	/* Thread */
	thread = kthread_create(fifo_to_sbuf, lockamp, "lockamp0");
	if (IS_ERR(thread)) {
		dev_alert(lockamp->dev, "Failed to create kthread.\n");
		result = PTR_ERR(thread);
		thread = NULL;
        goto out_open_count;
	}
	result = increase_task_priority(thread);
	if (0 != result) {
		goto out_thread;
	}
	wake_up_process(thread);
	/* Synchronization */
	lockamp->last_desyncs = atomic_read(&lockamp->desyncs);
	reset_start_time(lockamp);
	/* Out */
	result = 0;
	goto out;
out_thread:
	kthread_stop(thread);
	thread = NULL;
out_open_count:
	--open_count;
out:
	return result;
}

static int device_release(struct inode *inode, struct file *file)
{
	struct lockamp *lockamp;
	lockamp = container_of(inode->i_cdev, struct lockamp, cdev);
	if (thread) {
		kthread_stop(thread);
		thread = NULL;
	}
	--open_count;
	return 0;
}

static ssize_t pop_chunk_to_user(struct circ_sample_buf *cbuf,
           struct csbuf_snapshot *cbuf_snap, char __user *buffer, size_t length)
{
	size_t cbuf_size_to_end_n = (size_t)CIRC_CNT_TO_END(cbuf_snap->head,
	                                         cbuf_snap->tail, cbuf->capacity_n);
	size_t chunk_size = cbuf_size_to_end_n * sizeof(struct sample);
	size_t copy_length = min(chunk_size, length);
	size_t copy_length_n = copy_length / sizeof(struct sample);
	ptrdiff_t new_tail;
	if (0 != copy_to_user(buffer, cbuf->buf + cbuf_snap->tail, copy_length)) {
		return -EFAULT;
	}
	new_tail = (cbuf_snap->tail + copy_length_n) & (cbuf->capacity_n - 1);
	smp_store_release(&cbuf->tail, new_tail);
	cbuf_snap->tail = new_tail;
	return copy_length;
}

static ssize_t pop_to_user(struct circ_sample_buf *cbuf,
           struct csbuf_snapshot *cbuf_snap, char __user *buffer, size_t length)
{
	ssize_t chunk_result;
	char *pos = buffer;
	/* Read the first contiguous chunk of data */
	chunk_result = pop_chunk_to_user(cbuf, cbuf_snap, pos, length);
	if (0 > chunk_result) {
		return chunk_result;
	}
	pos += chunk_result;
	length -= chunk_result;
	/* Read the second contiguous chunk of data */
	chunk_result = pop_chunk_to_user(cbuf, cbuf_snap, pos, length);
	if (0 > chunk_result) {
		return chunk_result;
	}
	pos += chunk_result;
	length -= chunk_result;
	return pos - buffer;
}

static int chunk_get_info(struct lockamp *lockamp, size_t usr_buf_length,
                      struct csbuf_snapshot *sbuf_snap, struct chunk_info *info)
{
	size_t data_size_n;
	/* The user-provided buffer can not contain a chunk */
	if (sizeof(struct chunk_header) > usr_buf_length)
		return -EINVAL;
	data_size_n = (usr_buf_length - sizeof(struct chunk_header)) / sizeof(struct sample);
	data_size_n = min(data_size_n, sbuf_snap->size_n);
	info->header.last_start_time_ns = lockamp->last_start_time_ns;
	info->header.time_step_ns = lockamp_get_time_step_ns(lockamp);
	info->data_size_n = data_size_n;
	return 0;
}

static void chunk_commit_info(struct lockamp *lockamp, struct chunk_info *info)
{
	u64 duration_ns = lockamp_get_duration_ns(lockamp, info->data_size_n);
	lockamp->last_start_time_ns += duration_ns;
}

static ssize_t write_header_to_user(struct lockamp *lockamp,
                struct chunk_header *header, char __user *buffer, size_t length)
{
	ssize_t result;
	result = copy_to_user(buffer, header, sizeof(struct chunk_header));
	if (0 != result) {
		dev_alert(lockamp->dev, "Failed to copy chunk header to user space buffer.\n");
		return result;
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
	ssize_t result;
	char *pos = buffer;
	struct lockamp *lockamp = filp->private_data;
	struct csbuf_snapshot sbuf_snap;
	struct chunk_info info;
	reader_get_sbuf_snapshot(lockamp, &sbuf_snap);
	synchronize(lockamp);
	result = chunk_get_info(lockamp, length, &sbuf_snap, &info);
	if (0 != result) {
		return result;
	}
	/* Chunk header */
	result = write_header_to_user(lockamp, &info.header, pos, length);
	if (0 > result) {
		dev_alert(lockamp->dev, "Failed to copy header to user space buffer.\n");
		return result;
	}
	pos += result;
	length = info.data_size_n * sizeof(struct sample);
	/* Chunk data */
	result = pop_to_user(&lockamp->signal_buf, &sbuf_snap, pos, length);
	if (0 > result) {
		dev_alert(lockamp->dev, "Failed to copy chunk data to user space buffer.\n");
		return result;
	}
	pos += result;
	length -= result;
	/* Effectuate the write */
	chunk_commit_info(lockamp, &info);
	return pos - buffer;
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
