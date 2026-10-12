// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Framebuffer driver for the SSD2119 panel on SBT's Green Mango boards, fed
 * by the FPGA image's display engine.
 *
 * The engine sits between the panel's 4-wire SPI pins and the AXI Quad SPI
 * that used to drive them alone. Once this driver sets its "own" bit, the
 * engine drives the pins: register writes go through its command FIFO, and
 * pixels go by an AXI DMA (MM2S) channel. The engine sends each RGB565 pixel
 * high byte first, so the DMA reads the pixels as they lie in memory.
 *
 * The display is double buffered (sbt/drupe#59). An update copies the dirty
 * lines into the DMA's own buffer, starts the DMA and returns without waiting
 * for it. That releases deferred I/O's lock, so an application draws the next
 * frame into the framebuffer while the panel takes this one. The next update
 * first waits for this transfer to end, so the DMA's buffer never changes
 * under it. The copy runs under deferred I/O's lock, with every framebuffer
 * page write-protected, so it is a whole snapshot of what mmap writers drew.
 *
 * The panel keeps what U-Boot set up: this driver never resets it, never
 * runs its init sequence and never clears it, as fb_ssd2119 did with
 * "linux,skip-reset" and "linux,skip-clear". It sends the same bytes as
 * fb_ssd2119 for each update: the GDDRAM X and Y address (R4Eh, R4Fh), a
 * write of 0x0000 to R22h, then the dirty lines.
 *
 * Copyright (C) 2026 SBT Instruments
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/fb.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define DRVNAME "sbt_ssd2119fb"

#define ENGINE_VERSION		0x00
#define ENGINE_CTRL		0x04
#define ENGINE_DIV		0x08
#define ENGINE_STATUS		0x0c
#define ENGINE_CMD		0x10
#define ENGINE_COUNT		0x14

#define ENGINE_VERSION_ID	0x5d150000
#define ENGINE_VERSION_ID_MASK	0xffff0000
#define ENGINE_CTRL_OWN		BIT(0)
#define ENGINE_CTRL_FLUSH	BIT(1)
#define ENGINE_STATUS_BUSY	BIT(0)
#define ENGINE_STATUS_OVERFLOW	BIT(2)
#define ENGINE_DIV_HIGH(div)	((div) & 0xff)
#define ENGINE_DIV_LOW(div)	(((div) >> 8) & 0xff)
#define ENGINE_CMD_DC		BIT(8)
#define ENGINE_FIFO_DEPTH	32
#define ENGINE_ACLK_HZ		100000000

#define SSD2119_WIDTH		320
#define SSD2119_HEIGHT		240
#define SSD2119_BPP		16
#define SSD2119_LINE_LENGTH	(SSD2119_WIDTH * SSD2119_BPP / 8)
#define SSD2119_VMEM_SIZE	(SSD2119_LINE_LENGTH * SSD2119_HEIGHT)

#define SSD2119_REG_X		0x4e
#define SSD2119_REG_Y		0x4f
#define SSD2119_REG_GRAM	0x22

#define SBT_SSD2119_FPS		50

struct sbt_ssd2119 {
	struct device *dev;
	void __iomem *regs;
	struct fb_info *info;
	struct fb_deferred_io defio;
	struct dma_chan *chan;
	struct device *dma_dev;
	void *vmem;
	void *dmabuf; /* what the DMA reads: a copy of vmem's dirty lines */
	dma_addr_t dmabuf_dma;
	struct completion done;
	/* The transfer in flight, under defio.lock */
	bool sending;
	size_t sending_len;
	u32 sending_count; /* ENGINE_COUNT before it */
	spinlock_t dirty_lock; /* the dirty lines that fb ops mark */
	unsigned int dirty_start;
	unsigned int dirty_end;
	u32 pseudo_palette[16];
	struct backlight_device *backlight;
	/* Across a reload of the FPGA image (sbt/drupe#55) */
	struct fpga_mgr_pl_hook pl_hook;
	bool paused; /* under defio.lock */
	u32 div; /* the engine's DIV while paused */
	ktime_t paused_at;
};

static u32 engine_read(struct sbt_ssd2119 *par, unsigned int reg)
{
	return readl(par->regs + reg);
}

static void engine_write(struct sbt_ssd2119 *par, unsigned int reg, u32 val)
{
	writel(val, par->regs + reg);
}

/* How long the engine takes to send @bytes at its current SCK, with margin */
static unsigned int engine_timeout_us(struct sbt_ssd2119 *par, size_t bytes)
{
	u32 div = engine_read(par, ENGINE_DIV);
	u64 cycles = (u64)bytes * 8 * (ENGINE_DIV_HIGH(div) + ENGINE_DIV_LOW(div) + 2);

	return div_u64(cycles * USEC_PER_SEC, ENGINE_ACLK_HZ) + 20000;
}

static int engine_wait_idle(struct sbt_ssd2119 *par, size_t bytes)
{
	u32 status;
	int ret;

	ret = readl_poll_timeout(par->regs + ENGINE_STATUS, status,
				 !(status & ENGINE_STATUS_BUSY), 10,
				 engine_timeout_us(par, bytes));
	if (ret)
		dev_err_ratelimited(par->dev, "display engine stuck busy\n");
	return ret;
}

/* One 16-bit register write, as fbtft_write_reg16_bus8 sends it */
static void engine_write_reg(struct sbt_ssd2119 *par, u8 reg, u16 val)
{
	engine_write(par, ENGINE_CMD, 0x00);
	engine_write(par, ENGINE_CMD, reg);
	engine_write(par, ENGINE_CMD, ENGINE_CMD_DC | (val >> 8));
	engine_write(par, ENGINE_CMD, ENGINE_CMD_DC | (val & 0xff));
}

static void engine_reset(struct sbt_ssd2119 *par)
{
	engine_write(par, ENGINE_CTRL, ENGINE_CTRL_OWN | ENGINE_CTRL_FLUSH);
}

static void sbt_ssd2119_dma_done(void *data)
{
	struct sbt_ssd2119 *par = data;

	complete(&par->done);
}

/* Wait for the transfer in flight, if any, to end. Under defio.lock. */
static void sbt_ssd2119_finish(struct sbt_ssd2119 *par)
{
	size_t len = par->sending_len;
	unsigned long timeout;
	u32 count;

	if (!par->sending)
		return;
	par->sending = false;

	timeout = usecs_to_jiffies(engine_timeout_us(par, len));
	if (!wait_for_completion_timeout(&par->done, timeout)) {
		dev_err_ratelimited(par->dev, "the pixel DMA timed out\n");
		dmaengine_terminate_sync(par->chan);
		goto reset;
	}
	/* The DMA is done once the engine took the last word, not sent it */
	if (engine_wait_idle(par, 4))
		goto reset;
	count = engine_read(par, ENGINE_COUNT) - par->sending_count;
	if (count != len)
		dev_err_ratelimited(par->dev, "the engine sent %u of %zu bytes\n",
				    count, len);
	return;

reset:
	engine_reset(par);
}

/*
 * Start sending lines @start to @end, both included, to the panel. Under
 * defio.lock. It returns once the DMA runs; sbt_ssd2119_finish waits for it.
 */
static void sbt_ssd2119_update(struct sbt_ssd2119 *par, unsigned int start,
			       unsigned int end)
{
	size_t offset = start * SSD2119_LINE_LENGTH;
	size_t len = (end - start + 1) * SSD2119_LINE_LENGTH;
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;

	sbt_ssd2119_finish(par);

	/* Three registers of four bytes each fit the FIFO */
	engine_write_reg(par, SSD2119_REG_X, 0);
	engine_write_reg(par, SSD2119_REG_Y, start);
	engine_write_reg(par, SSD2119_REG_GRAM, 0);
	if (engine_wait_idle(par, 12)) {
		engine_reset(par);
		return;
	}

	memcpy(par->dmabuf + offset, par->vmem + offset, len);
	dma_sync_single_for_device(par->dma_dev, par->dmabuf_dma + offset, len,
				   DMA_TO_DEVICE);
	desc = dmaengine_prep_slave_single(par->chan, par->dmabuf_dma + offset,
					   len, DMA_MEM_TO_DEV,
					   DMA_PREP_INTERRUPT);
	if (!desc) {
		dev_err_ratelimited(par->dev, "cannot prepare the pixel DMA\n");
		return;
	}
	reinit_completion(&par->done);
	desc->callback = sbt_ssd2119_dma_done;
	desc->callback_param = par;
	par->sending_count = engine_read(par, ENGINE_COUNT);
	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie)) {
		dev_err_ratelimited(par->dev, "cannot submit the pixel DMA\n");
		return;
	}
	dma_async_issue_pending(par->chan);
	par->sending = true;
	par->sending_len = len;
}

static void sbt_ssd2119_mkdirty(struct fb_info *info, unsigned int y,
				unsigned int height)
{
	struct sbt_ssd2119 *par = info->par;
	unsigned int end;

	if (!height || y >= SSD2119_HEIGHT)
		return;
	end = min(y + height - 1, SSD2119_HEIGHT - 1U);
	spin_lock(&par->dirty_lock);
	par->dirty_start = min(par->dirty_start, y);
	par->dirty_end = max(par->dirty_end, end);
	spin_unlock(&par->dirty_lock);
	schedule_delayed_work(&info->deferred_work, par->defio.delay);
}

static void sbt_ssd2119_deferred_io(struct fb_info *info,
				    struct list_head *pagereflist)
{
	struct sbt_ssd2119 *par = info->par;
	struct fb_deferred_io_pageref *pageref;
	unsigned int start, end;

	/* Resuming sends the whole screen */
	if (par->paused)
		return;

	spin_lock(&par->dirty_lock);
	start = par->dirty_start;
	end = par->dirty_end;
	par->dirty_start = SSD2119_HEIGHT - 1;
	par->dirty_end = 0;
	spin_unlock(&par->dirty_lock);

	list_for_each_entry(pageref, pagereflist, list) {
		start = min_t(unsigned int, start,
			      pageref->offset / SSD2119_LINE_LENGTH);
		end = max_t(unsigned int, end,
			    min_t(unsigned long, SSD2119_HEIGHT - 1,
				  (pageref->offset + PAGE_SIZE - 1) /
				  SSD2119_LINE_LENGTH));
	}
	if (start > end)
		return;
	sbt_ssd2119_update(par, start, end);
}

static ssize_t sbt_ssd2119_write(struct fb_info *info, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	/* Below smem_len, or fb_sys_write fails */
	unsigned int pos = *ppos;
	ssize_t ret;

	ret = fb_sys_write(info, buf, count, ppos);
	if (ret > 0)
		sbt_ssd2119_mkdirty(info, pos / SSD2119_LINE_LENGTH,
				    DIV_ROUND_UP(pos % SSD2119_LINE_LENGTH + ret,
						 SSD2119_LINE_LENGTH));
	return ret;
}

static void sbt_ssd2119_fillrect(struct fb_info *info,
				 const struct fb_fillrect *rect)
{
	sys_fillrect(info, rect);
	sbt_ssd2119_mkdirty(info, rect->dy, rect->height);
}

static void sbt_ssd2119_copyarea(struct fb_info *info,
				 const struct fb_copyarea *area)
{
	sys_copyarea(info, area);
	sbt_ssd2119_mkdirty(info, area->dy, area->height);
}

static void sbt_ssd2119_imageblit(struct fb_info *info,
				  const struct fb_image *image)
{
	sys_imageblit(info, image);
	sbt_ssd2119_mkdirty(info, image->dy, image->height);
}

static int sbt_ssd2119_setcolreg(unsigned int regno, unsigned int red,
				 unsigned int green, unsigned int blue,
				 unsigned int transp, struct fb_info *info)
{
	u32 *palette = info->pseudo_palette;

	if (regno >= 16)
		return -EINVAL;
	palette[regno] = ((red >> 11) << 11) | ((green >> 10) << 5) |
			 (blue >> 11);
	return 0;
}

static const struct fb_ops sbt_ssd2119_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= fb_sys_read,
	.fb_write	= sbt_ssd2119_write,
	.fb_fillrect	= sbt_ssd2119_fillrect,
	.fb_copyarea	= sbt_ssd2119_copyarea,
	.fb_imageblit	= sbt_ssd2119_imageblit,
	.fb_setcolreg	= sbt_ssd2119_setcolreg,
	.fb_mmap	= fb_deferred_io_mmap,
};

static ssize_t sck_divider_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct sbt_ssd2119 *par = dev_get_drvdata(dev);
	u32 div;

	mutex_lock(&par->defio.lock);
	div = par->paused ? par->div : engine_read(par, ENGINE_DIV);
	mutex_unlock(&par->defio.lock);
	return sysfs_emit(buf, "0x%04x\n", div & 0xffff);
}

static ssize_t sck_divider_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct sbt_ssd2119 *par = dev_get_drvdata(dev);
	u16 div;
	int ret;

	ret = kstrtou16(buf, 0, &div);
	if (ret)
		return ret;
	/* Not while a transfer runs */
	mutex_lock(&par->defio.lock);
	if (par->paused) {
		par->div = div;
	} else {
		sbt_ssd2119_finish(par);
		engine_write(par, ENGINE_DIV, div);
	}
	mutex_unlock(&par->defio.lock);
	return count;
}

/*
 * The engine's DIV register: bits 7:0 are the cycles of 100 MHz that SCL
 * stays high, less one; bits 15:8 the cycles it stays low, less one. The
 * engine's default, 0x0303, is 12.5 MHz, the Quad SPI's rate.
 */
static DEVICE_ATTR_RW(sck_divider);

static struct attribute *sbt_ssd2119_attrs[] = {
	&dev_attr_sck_divider.attr,
	NULL
};
ATTRIBUTE_GROUPS(sbt_ssd2119);

/*
 * A full reload of the FPGA image resets the engine and its DMA, and an access
 * while it loads never completes. So the driver stops sending, and keeps
 * /dev/fb0 and its memory: dash goes on drawing into it, and the panel holds
 * its last frame. After the reload it sets the engine up again and sends the
 * whole screen.
 *
 * The backlight's PWM is in the FPGA image too. Its lock keeps brightness
 * changes out until the reload is over.
 */
static int sbt_ssd2119_pl_pause(struct fpga_mgr_pl_hook *hook)
{
	struct sbt_ssd2119 *par = container_of(hook, struct sbt_ssd2119, pl_hook);

	if (par->backlight)
		mutex_lock(&par->backlight->ops_lock);
	/* Deferred I/O holds this lock across an update's start */
	mutex_lock(&par->defio.lock);
	sbt_ssd2119_finish(par);
	par->paused = true;
	par->div = engine_read(par, ENGINE_DIV);
	mutex_unlock(&par->defio.lock);
	/*
	 * Released, so that requesting it again sets the DMA's interrupts up
	 * after the reload. The DMA itself stays bound, so the framebuffer's
	 * mapping stays valid.
	 */
	dmaengine_terminate_sync(par->chan);
	dma_release_channel(par->chan);
	par->chan = NULL;
	par->paused_at = ktime_get();
	return 0;
}

/*
 * The PWM core skips a state it already holds, and the reload stopped the
 * timer. Blanking and unblanking makes pwm-backlight write it again.
 */
static void sbt_ssd2119_backlight_restore(struct backlight_device *backlight)
{
	int power = backlight->props.power;

	backlight->props.power = FB_BLANK_POWERDOWN;
	backlight_update_status(backlight);
	backlight->props.power = power;
	backlight_update_status(backlight);
}

static void sbt_ssd2119_pl_resume(struct fpga_mgr_pl_hook *hook)
{
	struct sbt_ssd2119 *par = container_of(hook, struct sbt_ssd2119, pl_hook);
	struct dma_chan *chan;

	chan = dma_request_chan(par->dev, "tx");
	mutex_lock(&par->defio.lock);
	if (IS_ERR(chan)) {
		dev_err(par->dev, "cannot get the pixel DMA channel back: %ld\n",
			PTR_ERR(chan));
	} else if (chan->device->dev != par->dma_dev) {
		dev_err(par->dev, "the pixel DMA channel moved to another DMA\n");
		dma_release_channel(chan);
	} else {
		par->chan = chan;
		engine_write(par, ENGINE_DIV, par->div);
		engine_reset(par);
		par->paused = false;
		sbt_ssd2119_update(par, 0, SSD2119_HEIGHT - 1);
	}
	mutex_unlock(&par->defio.lock);
	if (par->backlight) {
		sbt_ssd2119_backlight_restore(par->backlight);
		mutex_unlock(&par->backlight->ops_lock);
	}
	if (!par->paused)
		dev_info(par->dev, "the screen held its frame for %lld ms\n",
			 ktime_ms_delta(ktime_get(), par->paused_at));
}

static int sbt_ssd2119_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct backlight_device *backlight;
	struct sbt_ssd2119 *par;
	struct fb_info *info;
	u32 version, div;
	int ret;

	info = framebuffer_alloc(sizeof(*par), dev);
	if (!info)
		return -ENOMEM;
	par = info->par;
	par->dev = dev;
	par->info = info;
	init_completion(&par->done);
	spin_lock_init(&par->dirty_lock);
	par->dirty_start = SSD2119_HEIGHT - 1;
	par->dirty_end = 0;

	par->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(par->regs)) {
		ret = PTR_ERR(par->regs);
		goto err_release;
	}
	version = engine_read(par, ENGINE_VERSION);
	if ((version & ENGINE_VERSION_ID_MASK) != ENGINE_VERSION_ID) {
		ret = dev_err_probe(dev, -ENODEV,
				    "no display engine in the FPGA image (version %08x)\n",
				    version);
		goto err_release;
	}

	backlight = devm_of_find_backlight(dev);
	if (IS_ERR(backlight)) {
		ret = dev_err_probe(dev, PTR_ERR(backlight),
				    "cannot get the backlight\n");
		goto err_release;
	}

	par->chan = dma_request_chan(dev, "tx");
	if (IS_ERR(par->chan)) {
		ret = dev_err_probe(dev, PTR_ERR(par->chan),
				    "cannot get the pixel DMA channel\n");
		goto err_release;
	}
	par->dma_dev = par->chan->device->dev;

	par->vmem = alloc_pages_exact(SSD2119_VMEM_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!par->vmem) {
		ret = -ENOMEM;
		goto err_dma;
	}
	par->dmabuf = alloc_pages_exact(SSD2119_VMEM_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!par->dmabuf) {
		ret = -ENOMEM;
		goto err_vmem;
	}
	par->dmabuf_dma = dma_map_single(par->dma_dev, par->dmabuf,
					 SSD2119_VMEM_SIZE, DMA_TO_DEVICE);
	if (dma_mapping_error(par->dma_dev, par->dmabuf_dma)) {
		ret = -ENOMEM;
		goto err_dmabuf;
	}

	info->fbops = &sbt_ssd2119_ops;
	info->flags = FBINFO_FLAG_DEFAULT | FBINFO_VIRTFB;
	info->screen_buffer = par->vmem;
	info->pseudo_palette = par->pseudo_palette;
	strscpy(info->fix.id, DRVNAME, sizeof(info->fix.id));
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.line_length = SSD2119_LINE_LENGTH;
	info->fix.accel = FB_ACCEL_NONE;
	info->fix.smem_start = __pa(par->vmem);
	info->fix.smem_len = SSD2119_VMEM_SIZE;
	info->var.xres = SSD2119_WIDTH;
	info->var.yres = SSD2119_HEIGHT;
	info->var.xres_virtual = SSD2119_WIDTH;
	info->var.yres_virtual = SSD2119_HEIGHT;
	info->var.bits_per_pixel = SSD2119_BPP;
	info->var.red.offset = 11;
	info->var.red.length = 5;
	info->var.green.offset = 5;
	info->var.green.length = 6;
	info->var.blue.offset = 0;
	info->var.blue.length = 5;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.height = -1;
	info->var.width = -1;

	par->defio.delay = HZ / SBT_SSD2119_FPS ?: 1;
	par->defio.sort_pagereflist = true;
	par->defio.deferred_io = sbt_ssd2119_deferred_io;
	info->fbdefio = &par->defio;
	ret = fb_deferred_io_init(info);
	if (ret)
		goto err_unmap;

	if (!of_property_read_u32(dev->of_node, "sbt,sck-divider", &div))
		engine_write(par, ENGINE_DIV, div & 0xffff);
	engine_reset(par);

	platform_set_drvdata(pdev, par);
	ret = register_framebuffer(info);
	if (ret)
		goto err_defio;

	if (backlight) {
		backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(backlight);
	}
	par->backlight = backlight;
	par->pl_hook.pause = sbt_ssd2119_pl_pause;
	par->pl_hook.resume = sbt_ssd2119_pl_resume;
	par->pl_hook.priority = FPGA_MGR_PL_HOOK_CONSUMER;
	fpga_mgr_pl_hook_register(&par->pl_hook);
	dev_info(dev, "fb%d: %ux%u, pixels by DMA, SCK divider 0x%04x\n",
		 info->node, SSD2119_WIDTH, SSD2119_HEIGHT,
		 engine_read(par, ENGINE_DIV) & 0xffff);
	return 0;

err_defio:
	fb_deferred_io_cleanup(info);
	engine_write(par, ENGINE_CTRL, ENGINE_CTRL_FLUSH);
err_unmap:
	dma_unmap_single(par->dma_dev, par->dmabuf_dma, SSD2119_VMEM_SIZE,
			 DMA_TO_DEVICE);
err_dmabuf:
	free_pages_exact(par->dmabuf, SSD2119_VMEM_SIZE);
err_vmem:
	free_pages_exact(par->vmem, SSD2119_VMEM_SIZE);
err_dma:
	dma_release_channel(par->chan);
err_release:
	framebuffer_release(info);
	return ret;
}

/*
 * Unbinding hands the pins back to the Quad SPI, so the FPGA image can go
 * away after it (sbt/drupe#52). Close /dev/fb0 first, i.e. stop dash: as
 * with every deferred I/O fbdev, a mapping that outlives the device faults
 * into freed memory.
 */
static int sbt_ssd2119_remove(struct platform_device *pdev)
{
	struct sbt_ssd2119 *par = platform_get_drvdata(pdev);
	struct fb_info *info = par->info;

	fpga_mgr_pl_hook_unregister(&par->pl_hook);
	unregister_framebuffer(info);
	fb_deferred_io_cleanup(info);
	/* None if it could not be had back after a reload */
	if (par->chan) {
		sbt_ssd2119_finish(par);
		dmaengine_terminate_sync(par->chan);
	}
	engine_write(par, ENGINE_CTRL, ENGINE_CTRL_FLUSH);
	dma_unmap_single(par->dma_dev, par->dmabuf_dma, SSD2119_VMEM_SIZE,
			 DMA_TO_DEVICE);
	free_pages_exact(par->dmabuf, SSD2119_VMEM_SIZE);
	free_pages_exact(par->vmem, SSD2119_VMEM_SIZE);
	if (par->chan)
		dma_release_channel(par->chan);
	framebuffer_release(info);
	return 0;
}

static const struct of_device_id sbt_ssd2119_of_match[] = {
	{ .compatible = "sbt,display-engine-1.0" },
	{ }
};
MODULE_DEVICE_TABLE(of, sbt_ssd2119_of_match);

static struct platform_driver sbt_ssd2119_driver = {
	.probe = sbt_ssd2119_probe,
	.remove = sbt_ssd2119_remove,
	.driver = {
		.name = DRVNAME,
		.of_match_table = sbt_ssd2119_of_match,
		.dev_groups = sbt_ssd2119_groups,
	},
};
module_platform_driver(sbt_ssd2119_driver);

MODULE_DESCRIPTION("SSD2119 framebuffer on the Green Mango display engine, by DMA");
MODULE_AUTHOR("SBT Instruments");
MODULE_LICENSE("GPL");
