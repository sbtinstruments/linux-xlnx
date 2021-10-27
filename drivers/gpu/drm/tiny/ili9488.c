// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for Ilitek ILI9488 panels
 *
 * Copyright 2019 Frederik Peter Aalund <fpa@sbtinstruments.com
 */

#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/sched/clock.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modeset_helper.h>
#include <video/mipi_display.h>

#define MIPI_DBI_B_REG_VERSION      0x0
#define MIPI_DBI_B_REG_CONTROL      0x4
#define MIPI_DBI_B_REG_COMMAND      0x10
#define MIPI_DBI_B_REG_DATA         0x20

#define MIPI_DBI_B_CONTROL_RESET    BIT(0)
#define MIPI_DBI_B_CONTROL_CS       BIT(1)

#define ILI9488_CMD_DISPLAY_INVERSION_CONTROL   0xb4
#define ILI9488_CMD_FRAME_RATE_CONTROL          0xb1

#define ILI9488_DINV_2_DOT_INVERSION            0x02
#define ILI9488_DPI_16_BPP                      0x5
#define ILI9488_DBI_16_BPP                      0x5

#define ILI9488_MADCTL_BGR          BIT(3)
#define ILI9488_MADCTL_MV           BIT(5)
#define ILI9488_MADCTL_MX           BIT(6)
#define ILI9488_MADCTL_MY           BIT(7)

struct type_b {
	void __iomem *base;
	bool skip_initial_reset;
};

struct type_b *type_b_from_mipi_dbi(struct mipi_dbi *dbi)
{
	return (struct type_b *)dbi->spi;
}

/* Copied from drm_mipi_dbi.c */
#define MIPI_DBI_DEBUG_COMMAND(cmd, data, len) \
({ \
	if (!len) \
		DRM_DEBUG_DRIVER("cmd=%02x\n", cmd); \
	else if (len <= 32) \
		DRM_DEBUG_DRIVER("cmd=%02x, par=%*ph\n", cmd, (int)len, data);\
	else \
		DRM_DEBUG_DRIVER("cmd=%02x, len=%zu\n", cmd, len); \
})

/* Copied from drm_mipi_dbi.c since it is not exported from there. */
static const u8 mipi_dbi_dcs_read_commands[] = {
	MIPI_DCS_GET_DISPLAY_ID,
	MIPI_DCS_GET_RED_CHANNEL,
	MIPI_DCS_GET_GREEN_CHANNEL,
	MIPI_DCS_GET_BLUE_CHANNEL,
	MIPI_DCS_GET_DISPLAY_STATUS,
	MIPI_DCS_GET_POWER_MODE,
	MIPI_DCS_GET_ADDRESS_MODE,
	MIPI_DCS_GET_PIXEL_FORMAT,
	MIPI_DCS_GET_DISPLAY_MODE,
	MIPI_DCS_GET_SIGNAL_MODE,
	MIPI_DCS_GET_DIAGNOSTIC_RESULT,
	MIPI_DCS_READ_MEMORY_START,
	MIPI_DCS_READ_MEMORY_CONTINUE,
	MIPI_DCS_GET_SCANLINE,
	MIPI_DCS_GET_DISPLAY_BRIGHTNESS,
	MIPI_DCS_GET_CONTROL_DISPLAY,
	MIPI_DCS_GET_POWER_SAVE,
	MIPI_DCS_GET_CABC_MIN_BRIGHTNESS,
	MIPI_DCS_READ_DDB_START,
	MIPI_DCS_READ_DDB_CONTINUE,
	0, /* sentinel */
};

static int mipi_dbi_type_b_command(struct mipi_dbi *dbi, u8 *cmd, u8 *param, size_t num)
{
	struct type_b *type_b = type_b_from_mipi_dbi(dbi);
	MIPI_DBI_DEBUG_COMMAND(*cmd, param, num);
	/* Assert CS */
	iowrite32(MIPI_DBI_B_CONTROL_CS, type_b->base + MIPI_DBI_B_REG_CONTROL);
	/* Write command */
	iowrite8(*cmd, type_b->base + MIPI_DBI_B_REG_COMMAND);
	/* Some special commands may send the parameters in an optimized way */
	switch (*cmd) {
	/* 8 bits at a time is the default */
	default:
		while (0 < num) {
			iowrite8(*param, type_b->base + MIPI_DBI_B_REG_DATA);
			++param;
			--num;
		}
		break;
	/* Memory writes are optimized in hardware */
	case MIPI_DCS_WRITE_MEMORY_START:
		while (0 < num) {
			iowrite32(*(u32*)param, type_b->base + MIPI_DBI_B_REG_DATA);
			param += 4;
			num -= 4;
		}
		break;
	}
	/* Deassert CS */
	iowrite32(0, type_b->base + MIPI_DBI_B_REG_CONTROL);
	return 0;
}

int mipi_dbi_type_b_init(struct type_b *type_b, struct mipi_dbi *dbi)
{
	/* HACK: Use the spi field to store the type_b struct. */
	dbi->spi = (void *)type_b;

	dbi->read_commands = mipi_dbi_dcs_read_commands;
	dbi->command = mipi_dbi_type_b_command;
	dbi->swap_bytes = false;

	DRM_DEBUG_DRIVER("Using MIPI DBI Type B (Intel 8080 type parallel bus)\n");

	return 0;
}

static void mipi_dbi_type_b_hw_reset(struct mipi_dbi *dbi)
{
	struct type_b *type_b = type_b_from_mipi_dbi(dbi);
	iowrite32(MIPI_DBI_B_CONTROL_RESET, type_b->base + MIPI_DBI_B_REG_CONTROL);
	msleep(10);
	iowrite32(0, type_b->base + MIPI_DBI_B_REG_CONTROL);
	msleep(120);
}

static void ili9488_pipe_enable(struct drm_simple_display_pipe *pipe,
				struct drm_crtc_state *crtc_state,
				struct drm_plane_state *plane_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct mipi_dbi *dbi = &dbidev->dbi;
	struct type_b *type_b = type_b_from_mipi_dbi(dbi);
	u32 version;
	u8 addr_mode;

	version = ioread32(type_b->base + MIPI_DBI_B_REG_VERSION);
	DRM_DEBUG_DRIVER("MIPI DBI Type B HW version: %d\n", version);

	/* Sometimes, the boot loader does the initial reset. E.g., to show
	 * a splash screen before Linux boots. To avoid resetting twice
	 * (and potentially undo the work of the boot loader) we do a
	 * simple check here. */
	if (type_b->skip_initial_reset) {
		type_b->skip_initial_reset = false;
		goto out_enable_flush;
	}

	if (mipi_dbi_display_is_on(dbi)) {
		goto out_enable_flush;
	}

	/* Hardware reset */
	mipi_dbi_type_b_hw_reset(dbi);
	mipi_dbi_command(dbi, MIPI_DCS_SOFT_RESET);
	msleep(120);

	/*** Apply settings ***/
	/* Display off */
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_OFF);
	/* Positive gamma control */
	mipi_dbi_command(dbi, 0xE0, 0x00, 0x03, 0x09, 0x08, 0x16, \
			0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, \
			0x08, 0x16, 0x1A, 0x0F);
	/* Negative gamma control */
	mipi_dbi_command(dbi, 0xE1, 0x00, 0x16, 0x19, 0x03, 0x0F, \
			0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, \
			0x0D, 0x35, 0x37, 0x0F);
	/* Power control 1 */
	mipi_dbi_command(dbi, 0xC0, 0x17, 0x15);
	/* Power control 2 */
	mipi_dbi_command(dbi, 0xC1, 0x41);
	/* VCOM control 1 */
	mipi_dbi_command(dbi, 0xC5, 0x00, 0x12, 0x80);
	/* Pixel interchange format: RGB565 over MIPI 16 bit */
	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT,
	                 ILI9488_DBI_16_BPP | (ILI9488_DPI_16_BPP << 4));
	/* Interface mode control */
	mipi_dbi_command(dbi, 0xB0, 0x00);
	/* Frame rate control (0x01 is 30.38 Hz; 0xA0 is 60.76 Hz) */
	mipi_dbi_command(dbi, ILI9488_CMD_FRAME_RATE_CONTROL, 0xA0);
	/* Display inversion ON */
	mipi_dbi_command(dbi, 0x21);
	/* Display inversion control */
	mipi_dbi_command(dbi, ILI9488_CMD_DISPLAY_INVERSION_CONTROL,
                     ILI9488_DINV_2_DOT_INVERSION);
	/* Write CTRL display value (brightness, dimming, backlight) */
	mipi_dbi_command(dbi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x28);
	/* Write display brightness value */
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x7F);
	/* Exit sleep */
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(120);
	/* Display on */
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
	msleep(50);

out_enable_flush:
	/* Memory access control: MX and BGR */
	switch (dbidev->rotation) {
	default:
		addr_mode = ILI9488_MADCTL_MX;
		break;
	case 90:
		addr_mode = ILI9488_MADCTL_MV;
		break;
	case 180:
		addr_mode = ILI9488_MADCTL_MY;
		break;
	case 270:
		addr_mode = ILI9488_MADCTL_MV | ILI9488_MADCTL_MY |
			    ILI9488_MADCTL_MX;
		break;
	}
	addr_mode |= ILI9488_MADCTL_BGR;
	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);
	mipi_dbi_enable_flush(dbidev, crtc_state, plane_state);
}

static const struct drm_simple_display_pipe_funcs ili9488_pipe_funcs = {
	.enable = ili9488_pipe_enable,
	.disable = mipi_dbi_pipe_disable,
	.update = mipi_dbi_pipe_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

DEFINE_DRM_GEM_CMA_FOPS(ili9488_fops);

static struct drm_driver ili9488_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &ili9488_fops,
	.release		= mipi_dbi_release,
	DRM_GEM_CMA_VMAP_DRIVER_OPS,
	.name			= "ili9488",
	.desc			= "Ilitek ILI9488",
	.date			= "20190716",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id ili9488_of_match[] = {
	{ .compatible = "urt,p220md-t" },
	{},
};
MODULE_DEVICE_TABLE(of, ili9488_of_match);

static int ili9488_probe(struct platform_device *pdev)
{
	const struct drm_display_mode mode = {
		DRM_SIMPLE_MODE(320, 480, 49, 73)
	};
	struct device *dev = &pdev->dev;
	struct mipi_dbi_dev *dbidev;
	struct drm_device *drm;
	struct mipi_dbi *dbi;
	struct type_b *type_b;
	struct resource *resource;
	int rotation = 0;
	int ret;

	dbidev = devm_kzalloc(dev, sizeof(*dbidev), GFP_KERNEL);
	if (!dbidev) {
		return -ENOMEM;
	}

	dbi = &dbidev->dbi;
	drm = &dbidev->drm;
	ret = devm_drm_dev_init(dev, drm, &ili9488_driver);
	if (ret) {
		return ret;
	}

	drm_mode_config_init(drm);

	type_b = devm_kzalloc(dev, sizeof(*type_b), GFP_KERNEL);
	if (!type_b) {
		return -ENOMEM;
	}

	/* MIPI DBI interface */
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
	                                        "mipi-dbi-type-b");
	if (NULL == resource) {
		DRM_DEV_ERROR(dev, "Failed to get resource 'mipi-dbi-type-b'\n");
		return -ENOMEM;
	}
	type_b->base = devm_ioremap_resource(dev, resource);
	if (IS_ERR(type_b->base)) {
		DRM_DEV_ERROR(dev, "Failed to ioremap 'mipi-dbi-type-b'\n");
		return PTR_ERR(type_b->base);
	}
	type_b->skip_initial_reset = of_property_read_bool(dev->of_node, "linux,skip-reset");

	ret = mipi_dbi_type_b_init(type_b, dbi);
	if (ret) {
		return ret;
	}

	/* Backlight */
	dbidev->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(dbidev->backlight)) {
		DRM_DEV_ERROR(dev, "Failed to find backlight\n");
		return PTR_ERR(dbidev->backlight);
	}

	/* Rotation */
	device_property_read_u32(dev, "rotation", &rotation);

	ret = mipi_dbi_dev_init(dbidev, &ili9488_pipe_funcs, &mode, rotation);
	if (ret) {
		return ret;
	}

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		return ret;
	}

	platform_set_drvdata(pdev, drm);

	drm_fbdev_generic_setup(drm, 0);

	return 0;
}

static int ili9488_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);

	return 0;
}

static int ili9488_pm_suspend(struct device *dev)
{
	return drm_mode_config_helper_suspend(dev_get_drvdata(dev));
}

static int ili9488_pm_resume(struct device *dev)
{
	return drm_mode_config_helper_resume(dev_get_drvdata(dev));
}

static const struct dev_pm_ops ili9488_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ili9488_pm_suspend, ili9488_pm_resume)
};

static void ili9488_shutdown(struct platform_device *pdev)
{
	drm_atomic_helper_shutdown(platform_get_drvdata(pdev));
}

static struct platform_driver ili9488_platform_driver = {
	.driver = {
		.name = "ili9488",
		.owner = THIS_MODULE,
		.of_match_table = ili9488_of_match,
		.pm = &ili9488_pm_ops,
	},
	.probe = ili9488_probe,
	.remove = ili9488_remove,
	.shutdown = ili9488_shutdown,
};
module_platform_driver(ili9488_platform_driver);

MODULE_DESCRIPTION("Ilitek ILI9488 DRM driver");
MODULE_AUTHOR("Frederik Aalund");
MODULE_LICENSE("GPL");
