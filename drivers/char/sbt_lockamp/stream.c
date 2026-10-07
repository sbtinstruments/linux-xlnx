// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SBT Instruments Lock-in Amplifier: the sample stream
 *
 * Copyright (c) 2026, SBT Instruments A/S
 *
 * The FPGA streams samples (8 lanes of s32 each) to an AXI DMA, which
 * writes them to RAM. The FPGA ends a packet after every period.
 *
 * Each period is its own dmaengine descriptor, not a slice of one cyclic
 * descriptor. xilinx_dma calls a cyclic callback once per tasklet run,
 * however many periods completed, and reports no residue for it. So a
 * cyclic ring cannot tell where its head is. Non-cyclic descriptors each
 * get their own callback, in order, with a result and a residue.
 *
 * A buffer goes free -> in flight -> filled -> (read) -> free. When no
 * buffer is free the reader is behind: the period goes to a discard
 * buffer and counts as data loss. The DMA never stalls on the reader.
 *
 * When the FPGA's own buffer overflows (the DMA stalled), the stream
 * restarts. Filled periods stay readable across a restart.
 */
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/gfp.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>

#include "hw.h"
#include "stream.h"

#define LOCKAMP_BUFQ_MASK	(LOCKAMP_STREAM_BUFS - 1)
#define LOCKAMP_PERIOD_N	(LOCKAMP_PERIOD_SIZE / sizeof(struct sample))

static_assert(LOCKAMP_PERIOD_SIZE % sizeof(struct sample) == 0);
static_assert((LOCKAMP_STREAM_BUFS & LOCKAMP_BUFQ_MASK) == 0);
static_assert(LOCKAMP_STREAM_IN_FLIGHT < LOCKAMP_STREAM_BUFS);

unsigned int lockamp_ma_time_step_ns;
static u64 last_ma_time_ns;
static const unsigned int ma_factor = 20;

static void lockamp_bufq_reset(struct lockamp_bufq *q)
{
	q->first = 0;
	q->last = 0;
}

static unsigned int lockamp_bufq_count(const struct lockamp_bufq *q)
{
	return q->last - q->first;
}

static void lockamp_bufq_push(struct lockamp_bufq *q, struct lockamp_buf *buf)
{
	q->bufs[q->last++ & LOCKAMP_BUFQ_MASK] = buf;
}

static struct lockamp_buf *lockamp_bufq_pop(struct lockamp_bufq *q)
{
	if (q->first == q->last)
		return NULL;
	return q->bufs[q->first++ & LOCKAMP_BUFQ_MASK];
}

/* The measured sample period, as a moving average over periods */
static void lockamp_update_ma_time_ns(size_t size_n)
{
	u64 now_ns = ktime_get_ns();
	unsigned int delta_ns = now_ns - last_ma_time_ns;
	unsigned int time_step_ns = delta_ns / size_n;

	last_ma_time_ns = now_ns;
	lockamp_ma_time_step_ns = (time_step_ns +
				   (ma_factor - 1) * lockamp_ma_time_step_ns) /
				  ma_factor;
}

static void lockamp_data_loss(struct lockamp *lockamp, const char *why)
{
	atomic_inc(&lockamp->desyncs);
	dev_warn_ratelimited(lockamp->dev, "Data loss. %s.\n", why);
}

static int lockamp_stream_enable(struct lockamp *lockamp, bool enable)
{
	return regmap_write(lockamp->regmap, LOCKAMP_REG_STREAM_CONTROL,
			    enable ? LOCKAMP_STREAM_ENABLE : 0);
}

/* Reads and clears the FPGA's sticky overflow flag */
static bool lockamp_stream_overflowed(struct lockamp *lockamp)
{
	u32 status;

	if (regmap_read(lockamp->regmap, LOCKAMP_REG_STREAM_STATUS, &status))
		return false;
	if (!(status & LOCKAMP_STREAM_OVERFLOW))
		return false;
	regmap_write(lockamp->regmap, LOCKAMP_REG_STREAM_STATUS,
		     LOCKAMP_STREAM_OVERFLOW);
	return true;
}

static void lockamp_stream_period_done(void *param,
				       const struct dmaengine_result *result);

/* Gives the DMA one more period to fill. Call with the lock held. */
static int lockamp_stream_arm(struct lockamp *lockamp)
{
	struct lockamp_stream *s = &lockamp->stream;
	struct dma_async_tx_descriptor *desc;
	struct lockamp_buf *buf;
	dma_cookie_t cookie;

	buf = lockamp_bufq_pop(&s->free);
	if (buf) {
		dma_sync_single_for_device(s->dma_dev, buf->dma,
					   LOCKAMP_PERIOD_SIZE,
					   DMA_FROM_DEVICE);
	} else {
		buf = &s->discard;
		lockamp_data_loss(lockamp,
				  "There is no more space in the signal buffer");
	}

	desc = dmaengine_prep_slave_single(s->chan, buf->dma,
					   LOCKAMP_PERIOD_SIZE, DMA_DEV_TO_MEM,
					   DMA_PREP_INTERRUPT);
	if (!desc)
		goto err;
	desc->callback_result = lockamp_stream_period_done;
	desc->callback_param = lockamp;
	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie))
		goto err;

	lockamp_bufq_push(&s->in_flight, buf);
	return 0;

err:
	if (buf != &s->discard)
		lockamp_bufq_push(&s->free, buf);
	return -EIO;
}

/* Arms every in-flight period, then lets the FPGA stream */
static int lockamp_stream_go(struct lockamp *lockamp)
{
	struct lockamp_stream *s = &lockamp->stream;
	int ret = 0;
	int i;

	/* The overflow flag is sticky: clear it from the last run */
	lockamp_stream_overflowed(lockamp);

	spin_lock_bh(&s->lock);
	for (i = 0; i < LOCKAMP_STREAM_IN_FLIGHT; ++i) {
		ret = lockamp_stream_arm(lockamp);
		if (ret)
			break;
		/* Starts the first batch, so the rest waits as the next one */
		if (i == LOCKAMP_STREAM_BATCH - 1)
			dma_async_issue_pending(s->chan);
	}
	spin_unlock_bh(&s->lock);
	if (ret)
		return ret;

	return lockamp_stream_enable(lockamp, true);
}

/*
 * Stops the DMA, then the FPGA. Filled periods stay readable. Call with
 * "running" false, so no callback re-arms a period meanwhile.
 *
 * The S2MM channel halts only once its current transfer has its data. So
 * the FPGA must still stream while the DMA halts: with the stream disabled
 * first, the halt waits out xilinx_dma's 1 s timeout ("Cannot stop
 * channel") and the channel gets reset anyway.
 */
static void lockamp_stream_halt(struct lockamp *lockamp)
{
	struct lockamp_stream *s = &lockamp->stream;
	struct lockamp_buf *buf;

	dmaengine_terminate_sync(s->chan);
	lockamp_stream_enable(lockamp, false);

	spin_lock_bh(&s->lock);
	while ((buf = lockamp_bufq_pop(&s->in_flight)))
		if (buf != &s->discard)
			lockamp_bufq_push(&s->free, buf);
	spin_unlock_bh(&s->lock);
}

static void lockamp_stream_restart(struct work_struct *work)
{
	struct lockamp *lockamp = container_of(work, struct lockamp,
					       stream.restart_work);
	struct lockamp_stream *s = &lockamp->stream;

	mutex_lock(&s->control_m);
	if (s->running) {
		spin_lock_bh(&s->lock);
		s->running = false;
		spin_unlock_bh(&s->lock);
		lockamp_stream_halt(lockamp);
		spin_lock_bh(&s->lock);
		s->running = true;
		spin_unlock_bh(&s->lock);
		if (lockamp_stream_go(lockamp))
			dev_err(lockamp->dev, "Failed to restart the sample stream\n");
	}
	mutex_unlock(&s->control_m);
}

static void lockamp_stream_period_done(void *param,
				       const struct dmaengine_result *result)
{
	struct lockamp *lockamp = param;
	struct lockamp_stream *s = &lockamp->stream;
	struct lockamp_buf *buf;
	bool overflow = lockamp_stream_overflowed(lockamp);
	bool issue = false;

	spin_lock(&s->lock);
	buf = lockamp_bufq_pop(&s->in_flight);
	if (WARN_ON_ONCE(!buf))
		goto unlock;

	if (buf == &s->discard) {
		/* Counted as data loss when armed */
	} else if (result->result != DMA_TRANS_NOERROR || result->residue) {
		lockamp_data_loss(lockamp, "A DMA period is incomplete");
		lockamp_bufq_push(&s->free, buf);
	} else {
		dma_sync_single_for_cpu(s->dma_dev, buf->dma,
					LOCKAMP_PERIOD_SIZE, DMA_FROM_DEVICE);
		lockamp_bufq_push(&s->filled, buf);
		lockamp_update_ma_time_ns(LOCKAMP_PERIOD_N);
	}

	if (!s->running)
		goto unlock;
	if (overflow) {
		lockamp_data_loss(lockamp, "The FPGA stream overflowed");
		schedule_work(&s->restart_work);
	} else if (lockamp_stream_arm(lockamp)) {
		dev_err_ratelimited(lockamp->dev, "Failed to arm a DMA period\n");
	} else {
		issue = true;
	}
unlock:
	spin_unlock(&s->lock);
	/* Restarts the channel if it went idle before this period was armed */
	if (issue)
		dma_async_issue_pending(s->chan);
}

int lockamp_stream_start(struct lockamp *lockamp)
{
	struct lockamp_stream *s = &lockamp->stream;
	int ret;
	int i;

	lockamp_bufq_reset(&s->free);
	lockamp_bufq_reset(&s->in_flight);
	lockamp_bufq_reset(&s->filled);
	for (i = 0; i < LOCKAMP_STREAM_BUFS; ++i)
		lockamp_bufq_push(&s->free, &s->bufs[i]);
	s->tail_offset = 0;
	last_ma_time_ns = ktime_get_ns();

	ret = regmap_write(lockamp->regmap, LOCKAMP_REG_STREAM_PERIOD,
			   LOCKAMP_PERIOD_N);
	if (ret)
		goto err;

	mutex_lock(&s->control_m);
	s->running = true;
	ret = lockamp_stream_go(lockamp);
	mutex_unlock(&s->control_m);
	if (ret)
		goto err;
	return 0;

err:
	dev_err(lockamp->dev, "Failed to start the sample stream: %d\n", ret);
	lockamp_stream_stop(lockamp);
	return ret;
}

void lockamp_stream_stop(struct lockamp *lockamp)
{
	struct lockamp_stream *s = &lockamp->stream;

	mutex_lock(&s->control_m);
	spin_lock_bh(&s->lock);
	s->running = false;
	spin_unlock_bh(&s->lock);
	lockamp_stream_halt(lockamp);
	mutex_unlock(&s->control_m);
	cancel_work_sync(&s->restart_work);
}

/* Bytes the reader can read now. Always whole samples. */
size_t lockamp_stream_available(struct lockamp *lockamp)
{
	struct lockamp_stream *s = &lockamp->stream;
	size_t size;

	spin_lock_bh(&s->lock);
	size = lockamp_bufq_count(&s->filled) * LOCKAMP_PERIOD_SIZE -
	       s->tail_offset;
	spin_unlock_bh(&s->lock);
	return size;
}

static void lockamp_stream_consume(struct lockamp *lockamp, size_t size)
{
	struct lockamp_stream *s = &lockamp->stream;

	spin_lock_bh(&s->lock);
	size += s->tail_offset;
	while (size >= LOCKAMP_PERIOD_SIZE) {
		lockamp_bufq_push(&s->free, lockamp_bufq_pop(&s->filled));
		size -= LOCKAMP_PERIOD_SIZE;
	}
	s->tail_offset = size;
	spin_unlock_bh(&s->lock);
}

/*
 * Copies the oldest "length" bytes to user space and frees what it read.
 * "length" must not exceed lockamp_stream_available(). Only the reader
 * takes filled periods, so the ones it copies stay put.
 */
ssize_t lockamp_stream_read(struct lockamp *lockamp, char __user *buffer,
			    size_t length)
{
	struct lockamp_stream *s = &lockamp->stream;
	struct lockamp_buf *buf;
	unsigned int period;
	size_t offset;
	size_t done = 0;
	size_t size;

	spin_lock_bh(&s->lock);
	period = s->filled.first;
	offset = s->tail_offset;
	spin_unlock_bh(&s->lock);

	while (done < length) {
		buf = s->filled.bufs[period++ & LOCKAMP_BUFQ_MASK];
		size = min(length - done, LOCKAMP_PERIOD_SIZE - offset);
		if (copy_to_user(buffer + done, buf->vaddr + offset, size)) {
			lockamp_stream_consume(lockamp, done);
			return -EFAULT;
		}
		done += size;
		offset = 0;
	}
	lockamp_stream_consume(lockamp, done);
	return done;
}

static int lockamp_buf_alloc(struct lockamp_stream *s, struct lockamp_buf *buf)
{
	struct page *page;

	page = alloc_pages(GFP_KERNEL, get_order(LOCKAMP_PERIOD_SIZE));
	if (!page)
		return -ENOMEM;
	buf->vaddr = page_address(page);
	buf->dma = dma_map_page(s->dma_dev, page, 0, LOCKAMP_PERIOD_SIZE,
				DMA_FROM_DEVICE);
	if (dma_mapping_error(s->dma_dev, buf->dma)) {
		__free_pages(page, get_order(LOCKAMP_PERIOD_SIZE));
		buf->vaddr = NULL;
		return -ENOMEM;
	}
	return 0;
}

static void lockamp_buf_free(struct lockamp_stream *s, struct lockamp_buf *buf)
{
	if (!buf->vaddr)
		return;
	dma_unmap_page(s->dma_dev, buf->dma, LOCKAMP_PERIOD_SIZE,
		       DMA_FROM_DEVICE);
	free_pages((unsigned long)buf->vaddr, get_order(LOCKAMP_PERIOD_SIZE));
	buf->vaddr = NULL;
}

int lockamp_stream_init(struct lockamp *lockamp, struct device *dev)
{
	struct lockamp_stream *s = &lockamp->stream;
	int ret;
	int i;

	spin_lock_init(&s->lock);
	mutex_init(&s->control_m);
	INIT_WORK(&s->restart_work, lockamp_stream_restart);
	s->chan = dma_request_chan(dev, "rx");
	if (IS_ERR(s->chan))
		return dev_err_probe(dev, PTR_ERR(s->chan),
				     "Failed to get the 'rx' DMA channel\n");
	s->dma_dev = dmaengine_get_dma_device(s->chan);

	/* Page-backed periods: CMA is too small to take 4 MiB */
	for (i = 0; i < LOCKAMP_STREAM_BUFS; ++i) {
		ret = lockamp_buf_alloc(s, &s->bufs[i]);
		if (ret)
			goto err;
	}
	ret = lockamp_buf_alloc(s, &s->discard);
	if (ret)
		goto err;
	return 0;

err:
	dev_err(dev, "Failed to allocate the signal buffer\n");
	lockamp_stream_exit(lockamp);
	return ret;
}

void lockamp_stream_exit(struct lockamp *lockamp)
{
	struct lockamp_stream *s = &lockamp->stream;
	int i;

	for (i = 0; i < LOCKAMP_STREAM_BUFS; ++i)
		lockamp_buf_free(s, &s->bufs[i]);
	lockamp_buf_free(s, &s->discard);
	dma_release_channel(s->chan);
}
