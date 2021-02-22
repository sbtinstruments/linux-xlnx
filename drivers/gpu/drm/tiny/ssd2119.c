// SPDX-License-Identifier: GPL-2.0+
/*
 * DRM driver for Solomon Systech SSD2119 panels
 *
 * Copyright 2021 SBT Instruments, Frederik Aalund <fpa@sbtinstruments.com>
 *
 * Based on ili9341.c:
 * Copyright 2018 David Lechner <david@lechnology.com>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_vblank.h>
#include <video/mipi_display.h>

enum {
	SSD2119_REG_OUTPUT_CONTROL				= 0x01,
	SSD2119_REG_ENTRY_MODE					= 0x11,
	SSD2119_REG_RAM_DATA_WRITE				= 0x22,
	SSD2119_REG_VERTICAL_RAM_POS			= 0x44,
	SSD2119_REG_HORIZONTAL_RAM_POS_START	= 0x45,
	SSD2119_REG_HORIZONTAL_RAM_POS_END		= 0x46,
	SSD2119_REG_RAM_ADDRESS_X				= 0x4e,
	SSD2119_REG_RAM_ADDRESS_Y				= 0x4f,
};

#define SSD2119_ENTRY_MODE_UPPER_BITS 0x6E40
#define SSD2119_ROT_0   0x30
#define SSD2119_ROT_90  0x18
#define SSD2119_ROT_180 0x00
#define SSD2119_ROT_270 0x28

struct ssd2119_dev {
	struct mipi_dbi_dev dbidev;
	int (*original_command)(struct mipi_dbi *dbi, u8 *cmd, u8 *param, size_t num);
	bool skip_initial_reset;
};

static inline struct ssd2119_dev *mipi_dbi_to_ssd2119_dev(struct mipi_dbi_dev *dbidev)
{
	return container_of(dbidev, struct ssd2119_dev, dbidev);
}

/* SSD2119 isn't actually a MIPI DBI type C device. It is very
 * close to that specification, though. Therefore, we use the
 * existing infrastructure for MIPI DBI type c with a bunch of
 * hacks on top. E.g.:
 *  * Split a single command into multiple
 *  * Remap certain register adresses
 */
static int ssd2119_dbi_command(struct mipi_dbi *dbi, u8 *cmd,
				   u8 *par, size_t num)
{
	struct mipi_dbi_dev *dbidev = container_of(dbi, struct mipi_dbi_dev, dbi);
	struct ssd2119_dev *ssd2119 = mipi_dbi_to_ssd2119_dev(dbidev);
	int ret;
	u8 ssd2119_cmd;
	u8 ssd2119_par[2];
	switch (*cmd) {
	case MIPI_DCS_SET_COLUMN_ADDRESS:
		// par[0]: rect->x1 (upper bits)
		// par[1]: rect->x1 (lower bits)
		// par[2]: rect->x2 - 1 (upper bits)
		// par[3]: rect->x2 - 1 (lower bits)
		ssd2119_cmd = SSD2119_REG_HORIZONTAL_RAM_POS_START;
		ret = ssd2119->original_command(dbi, &ssd2119_cmd, par, 2);
		if (ret)
			return ret;
		ssd2119_cmd = SSD2119_REG_HORIZONTAL_RAM_POS_END;
		ret = ssd2119->original_command(dbi, &ssd2119_cmd, par + 2, 2);
		if (ret)
			return ret;
		ssd2119_cmd = SSD2119_REG_RAM_ADDRESS_X;
		return ssd2119->original_command(dbi, &ssd2119_cmd, par, 2);
	case MIPI_DCS_SET_PAGE_ADDRESS:
		// par[0]: rect->y1 (upper bits)
		// par[1]: rect->y1 (lower bits)
		// par[2]: rect->y2 - 1 (upper bits)
		// par[3]: rect->y2 - 1 (lower bits)
		ssd2119_cmd = SSD2119_REG_VERTICAL_RAM_POS;
		ssd2119_par[0] = par[3];
		ssd2119_par[1] = par[1];
		ret = ssd2119->original_command(dbi, &ssd2119_cmd, ssd2119_par, 2);
		if (ret)
			return ret;
		ssd2119_cmd = SSD2119_REG_RAM_ADDRESS_Y;
		return ssd2119->original_command(dbi, &ssd2119_cmd, par, 2);
	case MIPI_DCS_WRITE_MEMORY_START:
		ssd2119_cmd = SSD2119_REG_RAM_DATA_WRITE;
		break;
	default:
		ssd2119_cmd = *cmd;
		break;
	}
	return ssd2119->original_command(dbi, &ssd2119_cmd, par, num);
}

static void ssd2119_pipe_enable(struct drm_simple_display_pipe *pipe,
			     struct drm_crtc_state *crtc_state,
			     struct drm_plane_state *plane_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct ssd2119_dev *ssd2119 = mipi_dbi_to_ssd2119_dev(dbidev);
	// struct mipi_dbi *dbi = &dbidev->dbi;
	// u8 addr_mode;
	int ret, idx;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	DRM_DEBUG_KMS("\n");

	/* Sometimes, the boot loader does the initial reset. E.g., to show
	 * a splash screen before Linux boots. To avoid resetting twice
	 * (and potentially undo the work of the boot loader) we do a
	 * simple check here. */
	if (ssd2119->skip_initial_reset) {
		ssd2119->skip_initial_reset = false;
		goto out_enable;
	}

	ret = mipi_dbi_poweron_conditional_reset(dbidev);
	if (ret < 0)
		goto out_exit;
	if (ret == 1)
		goto out_enable;

	// TODO: Implement initialization logic

	// mipi_dbi_command(dbi, 0x28, 0x0006);
	// mipi_dbi_command(dbi, 0x00, 0x0001);
	// mipi_dbi_command(dbi, 0x10, 0x0000);
	// mipi_dbi_command(dbi, SSD2119_REG_OUTPUT_CONTROL, 0x30EF);
	// mipi_dbi_command(dbi, 0x02, 0x0600);
	// mipi_dbi_command(dbi, 0x03, 0x6A38);
	// mipi_dbi_command(dbi, SSD2119_REG_ENTRY_MODE,
	//           SSD2119_ENTRY_MODE_UPPER_BITS | SSD2119_ROT_0);
	// mipi_dbi_command(dbi, 0X0F, 0x0000);
	// mipi_dbi_command(dbi, 0X0B, 0x5308);
	// mipi_dbi_command(dbi, 0x0C, 0x0003);
	// mipi_dbi_command(dbi, 0x0D, 0x000A);
	// mipi_dbi_command(dbi, 0x0E, 0x2E00);
	// mipi_dbi_command(dbi, 0x1E, 0x00BE);
	// mipi_dbi_command(dbi, 0x25, 0xA000);
	// mipi_dbi_command(dbi, 0x26, 0x7800);
	// mipi_dbi_command(dbi, 0x4E, 0x0000);
	// mipi_dbi_command(dbi, 0x4F, 0x0000);
	// mipi_dbi_command(dbi, 0x12, 0x08D9);
	// mipi_dbi_command(dbi, 0x30, 0x0000);
	// mipi_dbi_command(dbi, 0x31, 0x0104);
	// mipi_dbi_command(dbi, 0x32, 0x0100);
	// mipi_dbi_command(dbi, 0x33, 0x0305);
	// mipi_dbi_command(dbi, 0x34, 0x0505);
	// mipi_dbi_command(dbi, 0x35, 0x0305);
	// mipi_dbi_command(dbi, 0x36, 0x0707);
	// mipi_dbi_command(dbi, 0x37, 0x0300);
	// mipi_dbi_command(dbi, 0x3A, 0x1200);
	// mipi_dbi_command(dbi, 0x3B, 0x0800);
	// mipi_dbi_command(dbi, 0x07, 0x0033);
	// mipi_dbi_command(dbi, 0x22, 0x0000);

out_enable:
	mipi_dbi_enable_flush(dbidev, crtc_state, plane_state);
out_exit:
	drm_dev_exit(idx);
}

static const struct drm_simple_display_pipe_funcs ssd2119_pipe_funcs = {
	.enable = ssd2119_pipe_enable,
	.disable = mipi_dbi_pipe_disable,
	.update = mipi_dbi_pipe_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

static const struct drm_display_mode ssd2119_mode = {
	DRM_SIMPLE_MODE(320, 240, 70, 53), 
};

DEFINE_DRM_GEM_CMA_FOPS(ssd2119_fops);

static struct drm_driver ssd2119_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &ssd2119_fops,
	.release		= mipi_dbi_release,
	DRM_GEM_CMA_VMAP_DRIVER_OPS,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "ssd2119",
	.desc			= "Solomon Systech SSD2119",
	.date			= "20210216",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id ssd2119_of_match[] = {
	{ .compatible = "solomon,ssd2119" },
	{ }
};
MODULE_DEVICE_TABLE(of, ssd2119_of_match);

static const struct spi_device_id ssd2119_id[] = {
	{ "ssd2119", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, ssd2119_id);

static int ssd2119_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct ssd2119_dev *ssd2119;
	struct mipi_dbi_dev *dbidev;
	struct drm_device *drm;
	struct mipi_dbi *dbi;
	struct gpio_desc *dc;
	u32 rotation = 0;
	int ret;

	ssd2119 = kzalloc(sizeof(*ssd2119), GFP_KERNEL);
	if (!ssd2119)
		return -ENOMEM;

	dbidev = &ssd2119->dbidev;
	dbi = &dbidev->dbi;
	drm = &dbidev->drm;
	ret = devm_drm_dev_init(dev, drm, &ssd2119_driver);
	if (ret) {
		kfree(dbidev);
		return ret;
	}

	drm_mode_config_init(drm);

	dbi->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(dbi->reset)) {
		DRM_DEV_ERROR(dev, "Failed to get gpio 'reset'\n");
		return PTR_ERR(dbi->reset);
	}

	dc = devm_gpiod_get_optional(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc)) {
		DRM_DEV_ERROR(dev, "Failed to get gpio 'dc'\n");
		return PTR_ERR(dc);
	}

	dbidev->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(dbidev->backlight))
		return PTR_ERR(dbidev->backlight);

	device_property_read_u32(dev, "rotation", &rotation);

	ssd2119->skip_initial_reset = of_property_read_bool(dev->of_node, "linux,skip-reset");

	ret = mipi_dbi_spi_init(spi, dbi, dc);
	if (ret)
		return ret;

	/* override the command function set in mipi_dbi_spi_init() */
	ssd2119->original_command = dbi->command;
	dbi->command = ssd2119_dbi_command;

	ret = mipi_dbi_dev_init(dbidev, &ssd2119_pipe_funcs, &ssd2119_mode, rotation);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	spi_set_drvdata(spi, drm);

	drm_fbdev_generic_setup(drm, 0);

	return 0;
}

static int ssd2119_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);

	return 0;
}

static void ssd2119_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver ssd2119_spi_driver = {
	.driver = {
		.name = "ssd2119",
		.of_match_table = ssd2119_of_match,
	},
	.id_table = ssd2119_id,
	.probe = ssd2119_probe,
	.remove = ssd2119_remove,
	.shutdown = ssd2119_shutdown,
};
module_spi_driver(ssd2119_spi_driver);

MODULE_DESCRIPTION("Solomon Systech SSD2119 DRM driver");
MODULE_AUTHOR("Frederik Aalund <fpa@sbtinstruments.com>");
MODULE_LICENSE("GPL");
