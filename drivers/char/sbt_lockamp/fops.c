/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SBT Instruments Lock-in Amplifier
 *
 * Copyright (c) 2019, Frederik Peter Aalund <fpa@sbtinstruments.com>
 */
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/pm_runtime.h>
#include <linux/time.h>

#include "lockin_amplifier.h"
#include "hw.h"
#include "pm.h"
#include "stream.h"

struct chunk_header {
	u64 last_start_time_ns;
	u64 time_step_ns;
} __attribute__((packed));
_Static_assert (16 == sizeof(struct chunk_header), "struct 'chunk_header' is not packed on this platform");

struct chunk_info {
	struct chunk_header header;
	size_t data_size_n;
};

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
	mutex_lock(&lockamp->reload_m);
	/* Only a single reader at a time */
	if (lockamp->open_count) {
		ret = -EBUSY;
		goto out_unlock;
	}
	/* None if a reload of the FPGA image could not give it back */
	if (!lockamp->stream.chan) {
		ret = -ENODEV;
		goto out_unlock;
	}
	++lockamp->open_count;

	/* power */
	ret = lockamp_pm_get(lockamp);
	if (ret < 0) {
		dev_err(lockamp->dev, "Failed to get pm runtime: %d\n", ret);
		goto out_open_count;
	}

	ret = lockamp_stream_start(lockamp);
	if (ret < 0)
		goto out_pm;

	/* Synchronization */
	lockamp->last_desyncs = atomic_read(&lockamp->desyncs);
	reset_start_time(lockamp);
	mutex_unlock(&lockamp->reload_m);
	return 0;

out_pm:
	lockamp_pm_put(lockamp);
out_open_count:
	--lockamp->open_count;
out_unlock:
	mutex_unlock(&lockamp->reload_m);
	return ret;
}

static int device_release(struct inode *inode, struct file *file)
{
	struct lockamp *lockamp;
	lockamp = container_of(inode->i_cdev, struct lockamp, cdev);
	lockamp_stream_stop(lockamp);
	lockamp_pm_put(lockamp);
	mutex_lock(&lockamp->reload_m);
	--lockamp->open_count;
	mutex_unlock(&lockamp->reload_m);
	return 0;
}

static int chunk_get_info(struct lockamp *lockamp, size_t usr_buf_length,
			  size_t available_n, struct chunk_info *info)
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
	data_size_n = min(data_size_n, available_n);
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
	char *pos = buffer;
	struct chunk_info info;
	size_t available_n;

	available_n = lockamp_stream_available(lockamp) / sizeof(struct sample);
	synchronize(lockamp);
	ret = chunk_get_info(lockamp, length, available_n, &info);
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
	ret = lockamp_stream_read(lockamp, pos, length);
	if (ret < 0) {
		dev_alert(lockamp->dev, "Failed to copy chunk data to user space buffer.\n");
		return ret;
	}
	pos += ret;
	/* Effectuate the write */
	ret = chunk_commit_info(lockamp, &info);
	if (ret < 0) {
		return ret;
	}
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
