/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SBT Instruments Lock-in Amplifier
 *
 * Copyright (c) 2019, Frederik Peter Aalund <fpa@sbtinstruments.com>
 */
#ifndef LOCKAMP_LOCKAMP_H
#define LOCKAMP_LOCKAMP_H
#include <asm/atomic.h>
#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/dmaengine.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>

/* Class name as it appears in /sys/class  */
#define LOCKAMP_CLASS_NAME  "lockin_amplifier"

#define LOCKAMP_ADC_SAMPLES_SIZE_S32 16384
#define LOCKAMP_ADC_SAMPLES_SIZE     (LOCKAMP_ADC_SAMPLES_SIZE_S32 * sizeof(s32))
#define LOCKAMP_FIR_FILTER_COUNT     10
#define LOCKAMP_FIR_COEF_LEN         512
#define LOCKAMP_SITES_PER_SAMPLE     2
#define LOCKAMP_ENTRIES_PER_SITE     4
#define LOCKAMP_ENTRIES_PER_SAMPLE   (LOCKAMP_ENTRIES_PER_SITE * LOCKAMP_SITES_PER_SAMPLE)
#define LOCKAMP_SIGNAL_BUF_CAPACITY  SZ_4M

/*
 * The FPGA ends a stream packet (tlast) after every period, and each period
 * is one DMA descriptor. A period holds whole samples (1024 of them).
 */
#define LOCKAMP_PERIOD_SIZE          SZ_32K
#define LOCKAMP_STREAM_BUFS          (LOCKAMP_SIGNAL_BUF_CAPACITY / LOCKAMP_PERIOD_SIZE)
/*
 * xilinx_dma starts every pending descriptor as one batch when the channel
 * goes idle. Two batches in flight keep the next one pending.
 */
#define LOCKAMP_STREAM_BATCH         2
#define LOCKAMP_STREAM_IN_FLIGHT     (2 * LOCKAMP_STREAM_BATCH)
/* Stopping waits for the periods in flight: 4 x 45 ms at the slowest rate */
#define LOCKAMP_DRAIN_MS             500

struct lockamp_buf {
	void *vaddr;
	dma_addr_t dma;
};

/* A FIFO of buffers. "first" and "last" run free; mask them to index. */
struct lockamp_bufq {
	struct lockamp_buf *bufs[LOCKAMP_STREAM_BUFS];
	unsigned int first;
	unsigned int last;
};

struct lockamp_stream {
	struct dma_chan *chan;
	struct device *dma_dev;
	struct lockamp_buf bufs[LOCKAMP_STREAM_BUFS];
	/* Takes a period when no buffer is free (the reader is behind) */
	struct lockamp_buf discard;
	/* Guards the queues, tail_offset and running */
	spinlock_t lock;
	/* Serialises stopping with restart_work */
	struct mutex control_m;
	/* Restarts the stream after the FPGA overflowed */
	struct work_struct restart_work;
	struct lockamp_bufq free;
	struct lockamp_bufq in_flight;
	/* Periods with data, oldest first */
	struct lockamp_bufq filled;
	/* Bytes already read from the oldest filled period */
	size_t tail_offset;
	bool running;
	/* The FPGA streams (enabled by lockamp_stream_go) */
	bool streaming;
	/* Completes once "running" is false and no period is in flight */
	struct completion drained;
};

struct lockamp {
	struct cdev cdev;
	struct device *dev;
	dev_t chrdev_no;
	struct gpio_desc *reset;
	struct iio_channel *adc_site0, *adc_site1, *dac_site0, *dac_site1;
	struct regmap *regmap;
	u8 __iomem *control;
	struct regulator *amp_supply;
	bool amp_supply_force_off;

	struct lockamp_stream stream;
	struct mutex adc_buf_m;
	char *adc_buffer;
	int sample_multipliers[LOCKAMP_SITES_PER_SAMPLE];

	atomic_t desyncs;
	int last_desyncs;
	u64 last_start_time_ns;
};

struct site_sample {
	s32 hf_re;
	s32 hf_im;
	s32 lf_re;
	s32 lf_im;
} __attribute__((packed));
_Static_assert (LOCKAMP_ENTRIES_PER_SITE * sizeof(s32) == sizeof(struct site_sample), "struct 'site_sample' is not packed on this platform");

struct sample {
	struct site_sample sites[LOCKAMP_SITES_PER_SAMPLE];
} __attribute__((packed));
_Static_assert (LOCKAMP_SITES_PER_SAMPLE * sizeof(struct site_sample) == sizeof(struct sample), "struct 'sample' is not packed on this platform");

extern unsigned int lockamp_ma_time_step_ns;
extern const s32 lockamp_fir_coefs[LOCKAMP_FIR_FILTER_COUNT][LOCKAMP_FIR_COEF_LEN];
extern const struct attribute_group *lockamp_attr_groups[2];
extern struct file_operations lockamp_fops;
extern struct dev_pm_ops lockamp_pm_ops;

#endif /* LOCKAMP_LOCKAMP_H */
