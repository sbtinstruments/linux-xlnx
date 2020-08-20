// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * FB driver for the SSD2119 LCD Controller
 *
 * Copyright (C) 2019 Frederik Peter Aalund <fpa@sbtinstruments.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio.h>

#include "fbtft.h"

#define DRVNAME "fb_ssd2119"

#define SSD2119_REG_OUTPUT_CONTROL 0x01
#define SSD2119_REG_ENTRY_MODE 0x11

#define SSD2119_ENTRY_MODE_UPPER_BITS 0x6E40
#define SSD2119_ROT_0   0x30
#define SSD2119_ROT_90  0x18
#define SSD2119_ROT_180 0x00
#define SSD2119_ROT_270 0x28

static int init_display(struct fbtft_par *par)
{
	par->fbtftops.reset(par);
	write_reg(par, 0x28, 0x0006);
	write_reg(par, 0x00, 0x0001);
	write_reg(par, 0x10, 0x0000);
	write_reg(par, SSD2119_REG_OUTPUT_CONTROL, 0x30EF);
	write_reg(par, 0x02, 0x0600);
	write_reg(par, 0x03, 0x6A38);
	write_reg(par, SSD2119_REG_ENTRY_MODE,
	          SSD2119_ENTRY_MODE_UPPER_BITS | SSD2119_ROT_0);
	write_reg(par, 0X0F, 0x0000);
	write_reg(par, 0X0B, 0x5308);
	write_reg(par, 0x0C, 0x0003);
	write_reg(par, 0x0D, 0x000A);
	write_reg(par, 0x0E, 0x2E00);
	write_reg(par, 0x1E, 0x00BE);
	write_reg(par, 0x25, 0xA000);
	write_reg(par, 0x26, 0x7800);
	write_reg(par, 0x4E, 0x0000);
	write_reg(par, 0x4F, 0x0000);
	write_reg(par, 0x12, 0x08D9);
	write_reg(par, 0x30, 0x0000);
	write_reg(par, 0x31, 0x0104);
	write_reg(par, 0x32, 0x0100);
	write_reg(par, 0x33, 0x0305);
	write_reg(par, 0x34, 0x0505);
	write_reg(par, 0x35, 0x0305);
	write_reg(par, 0x36, 0x0707);
	write_reg(par, 0x37, 0x0300);
	write_reg(par, 0x3A, 0x1200);
	write_reg(par, 0x3B, 0x0800);
	write_reg(par, 0x07, 0x0033);
	write_reg(par, 0x22, 0x0000);
	return 0;
}

static void set_addr_win(struct fbtft_par *par, int xs, int ys, int xe, int ye)
{
	switch (par->info->var.rotate) {
	/* R4Eh - Set GDDRAM X address counter */
	/* R4Fh - Set GDDRAM Y address counter */
	case 0:
		write_reg(par, 0x4e, xs);
		write_reg(par, 0x4f, ys);
		break;
	case 180:
		write_reg(par, 0x4e, par->info->var.xres - 1 - xs);
		write_reg(par, 0x4f, par->info->var.yres - 1 - ys);
		break;
	case 270:
		write_reg(par, 0x4e, par->info->var.yres - 1 - ys);
		write_reg(par, 0x4f, xs);
		break;
	case 90:
		write_reg(par, 0x4e, ys);
		write_reg(par, 0x4f, par->info->var.xres - 1 - xs);
		break;
	}

	/* R22h - RAM data write */
	write_reg(par, 0x22, 0);
}

static int set_var(struct fbtft_par *par)
{
	s16 entry_mode = SSD2119_ENTRY_MODE_UPPER_BITS;
	switch (par->info->var.rotate) {
	case 0:
		entry_mode |= SSD2119_ROT_0;
		break;
	case 270:
		entry_mode |= SSD2119_ROT_270;
		break;
	case 180:
		entry_mode |= SSD2119_ROT_180;
		break;
	case 90:
		entry_mode |= SSD2119_ROT_90;
		break;
	}
	write_reg(par, SSD2119_REG_ENTRY_MODE, entry_mode);
	return 0;
}

void ssd2119_register_backlight(struct fbtft_par *par)
{
	struct device *dev = par->info->device;
	struct backlight_device *bl_dev;

	bl_dev = devm_of_find_backlight(dev);
	if (IS_ERR(bl_dev)) {
		dev_err(dev, "Could not find backlight: %ld\n", PTR_ERR(bl_dev));
		return;
	}
	par->info->bl_dev = bl_dev;
}

static struct fbtft_display display = {
	.regwidth = 16,
	.width = 320,
	.height = 240,
	.fbtftops = {
		.init_display = init_display,
		.set_addr_win = set_addr_win,
		.set_var = set_var,
		.register_backlight = ssd2119_register_backlight,
	},
};

FBTFT_REGISTER_DRIVER(DRVNAME, "solomon,ssd2119", &display);

MODULE_ALIAS("spi:" DRVNAME);
MODULE_ALIAS("platform:" DRVNAME);
MODULE_ALIAS("spi:ssd2119");
MODULE_ALIAS("platform:ssd2119");

MODULE_DESCRIPTION("FB driver for the SSD2119 LCD Controller");
MODULE_AUTHOR("Frederik Peter Aalund <fpa@sbtinstruments.com>");
MODULE_LICENSE("GPL");
