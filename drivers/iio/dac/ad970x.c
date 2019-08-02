/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2019 Frederik Peter Aalund <fpa@sbtinstruments.com>
 */
#include <linux/clk.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/stepper.h>

#define AD970X_SPI_CTL_REG	0x00
#define AD970X_DATA_REG		0x02
#define AD970X_CALMEM_REG	0x0E
#define AD970X_MEMRDWR_REG	0x0F

/* SPI CTL reg bits */
#define AD970X_SLEEP	BIT(2)
#define AD970X_SWRST	BIT(5)
#define AD970X_SDIODIR	BIT(7)
/* Data reg bits */
#define AD970X_CALCLK	BIT(0)
#define AD970X_CLKDIFF	BIT(2)
#define AD970X_DESKEW	BIT(3)
#define AD970X_DCLKPOL	BIT(4)
#define AD970X_DATAFMT	BIT(7) /* 1 for two's complement input */
/* CALMEM reg bits */
#define AD970X_DIVSEL_MASK	0x07
#define AD970X_CALMEM_MASK	0x30
/* MEMRDWR reg bits */
#define AD970X_UNCAL	BIT(0)
#define AD970X_CALEN	BIT(6)
#define AD970X_CALSTAT	BIT(7)

/* Calibration constants */
#define AD970X_CALCLK_TARGET_RATE 10000000 /* 10 MHz */
#define AD970X_CALCLK_CAL_CYCLES  4500 /* as per the data sheet */

/* Utility */
#define AD970X_TO_VALUE(enabled) ((enabled) ? 0xFF : 0x0)

enum ad970x_divsel {
	AD970X_DIVIDE_BY_256 = 0x0,
	AD970X_DIVIDE_BY_128 = 0x1,
	AD970X_DIVIDE_BY_64  = 0x2,
	AD970X_DIVIDE_BY_32  = 0x3,
	AD970X_DIVIDE_BY_16  = 0x4,
	AD970X_DIVIDE_BY_8   = 0x5,
	AD970X_DIVIDE_BY_4   = 0x6,
	AD970X_DIVIDE_BY_2   = 0x7,
};

enum ad970x_calmem {
	AD970X_UNCALIBRATED     = 0x0,
	AD970X_SELF_CALIBRATION = 0x1,
	AD970X_NOT_USED         = 0x2,
	AD970X_USER_INPUT       = 0x3,
};

struct ad970x {
	struct regmap *regmap;
	struct clk *clk;
	struct regulator *vdd;
};

struct ad970x_state {
	bool calibrate_on_init;
	bool clkdiff;
	bool deskew;
	bool dclkpol;
	bool twos_complement;
};

static struct ad970x_state ad970x_default_state = {
	.calibrate_on_init = false,
	.clkdiff = false,
	.deskew = false,
	.dclkpol = false,
	.twos_complement = false,
};

static int ad970x_reset(struct ad970x *ad970x)
{
	int error;
	regcache_cache_bypass(ad970x->regmap, true);
	error = regmap_write(ad970x->regmap, AD970X_SPI_CTL_REG, AD970X_SWRST);
	regcache_cache_bypass(ad970x->regmap, false);
	/* Sleep to ensure that the reset is done.
	 * The duration is chosen arbitrarily. */
	msleep(10);
	return error;
}

static int ad970x_set_spi_4wire(struct ad970x *ad970x)
{
	return regmap_write(ad970x->regmap, AD970X_SPI_CTL_REG, 0x0);
}

static int ad970x_set_calclk(struct ad970x *ad970x, bool enabled)
{
	return regmap_update_bits(ad970x->regmap, AD970X_DATA_REG,
	                          AD970X_CALCLK, AD970X_TO_VALUE(enabled));
}

static int ad970x_set_clkdiff(struct ad970x *ad970x, bool enabled)
{
	return regmap_update_bits(ad970x->regmap, AD970X_DATA_REG,
	                          AD970X_CLKDIFF, AD970X_TO_VALUE(enabled));
}

static int ad970x_set_deskew(struct ad970x *ad970x, bool enabled)
{
	return regmap_update_bits(ad970x->regmap, AD970X_DATA_REG,
	                          AD970X_DESKEW, AD970X_TO_VALUE(enabled));
}

static int ad970x_set_dclkpol(struct ad970x *ad970x, bool enabled)
{
	return regmap_update_bits(ad970x->regmap, AD970X_DATA_REG,
	                          AD970X_DCLKPOL, AD970X_TO_VALUE(enabled));
}

static int ad970x_set_twos_complement(struct ad970x *ad970x, bool enabled)
{
	return regmap_update_bits(ad970x->regmap, AD970X_DATA_REG,
	                          AD970X_DATAFMT, AD970X_TO_VALUE(enabled));
}

static int ad970x_set_calclk_div(struct ad970x *ad970x, enum ad970x_divsel divsel)
{
	return regmap_update_bits(ad970x->regmap, AD970X_CALMEM_REG,
	                          AD970X_DIVSEL_MASK, divsel);
}

static int ad970x_set_ceiled_calclk_div(struct ad970x *ad970x,
                                        unsigned int div,
                                        unsigned int *chosen_div)
{
	enum ad970x_divsel divsel;
	if (div <= 2) {
		divsel = AD970X_DIVIDE_BY_2;
		*chosen_div = 2;
	} else if (div <= 4) {
		divsel = AD970X_DIVIDE_BY_4;
		*chosen_div = 4;
	} else if (div <= 8) {
		divsel = AD970X_DIVIDE_BY_8;
		*chosen_div = 8;
	} else if (div <= 16) {
		divsel = AD970X_DIVIDE_BY_16;
		*chosen_div = 16;
	} else if (div <= 32) {
		divsel = AD970X_DIVIDE_BY_32;
		*chosen_div = 32;
	} else if (div <= 64) {
		divsel = AD970X_DIVIDE_BY_64;
		*chosen_div = 64;
	} else if (div <= 128) {
		divsel = AD970X_DIVIDE_BY_128;
		*chosen_div = 128;
	} else {
		divsel = AD970X_DIVIDE_BY_256;
		*chosen_div = 256;
	}
	return ad970x_set_calclk_div(ad970x, divsel);
}

static int ad970x_get_calmem(struct ad970x *ad970x, enum ad970x_calmem *calmem)
{
	unsigned int raw;
	int error = regmap_read(ad970x->regmap, AD970X_CALMEM_REG, &raw);
	raw = (raw & AD970X_CALMEM_MASK) >> 4;
	*calmem = raw;
	return error;
}

static int ad970x_set_uncal(struct ad970x *ad970x, bool enabled)
{
	return regmap_update_bits(ad970x->regmap, AD970X_MEMRDWR_REG,
	                          AD970X_UNCAL, AD970X_TO_VALUE(enabled));
}

static int ad970x_set_calen(struct ad970x *ad970x, bool enabled)
{
	return regmap_update_bits(ad970x->regmap, AD970X_MEMRDWR_REG,
	                          AD970X_CALEN, AD970X_TO_VALUE(enabled));
}

static int ad970x_get_calstat(struct ad970x *ad970x, unsigned int *calstat)
{
	int error = regmap_read(ad970x->regmap, AD970X_MEMRDWR_REG, calstat);
	*calstat = (*calstat & AD970X_CALSTAT) >> 7;
	return error;
}

static int ad970x_acknowledge_cal(struct ad970x *ad970x)
{
	return regmap_write(ad970x->regmap, AD970X_MEMRDWR_REG, 0x00);
}

static int ad970x_disable(struct device *dev)
{
	struct ad970x *ad970x = dev_get_drvdata(dev);
	int error;
	error = regulator_disable(ad970x->vdd);
	if (error) {
		dev_err(dev, "Failed to disable VDD regulator: %d\n", error);
		return error;
	}
	clk_disable_unprepare(ad970x->clk);
	return 0;
}

static int ad970x_enable(struct device *dev)
{
	struct ad970x *ad970x = dev_get_drvdata(dev);
	int error;
	error = regulator_enable(ad970x->vdd);
	if (error) {
		dev_err(dev, "Failed to enable VDD regulator: %d\n", error);
		return error;
	}
	error = clk_prepare_enable(ad970x->clk);
	if (error < 0) {
		dev_err(dev, "Failed to enable clock: %d\n", error);
		return error;
	}
	/* Wait a bit for the hw to power up.
	 * The duration is chosen arbitrarily. */
	msleep(10);
	return 0;
}

static int ad970x_calibrate(struct device *dev)
{
	struct ad970x *ad970x = dev_get_drvdata(dev);
	struct clk *clk = ad970x->clk;
	int error;
	unsigned int calstat;
	unsigned long cal_time_us;
	int i;
	int max_tries = 3;
	unsigned long clk_rate;
	unsigned int calclk_div;
	unsigned long calclk_period_ns;
	enum ad970x_calmem calmem;

	clk_rate = clk_get_rate(clk);
	calclk_div = DIV_ROUND_UP(clk_rate, AD970X_CALCLK_TARGET_RATE);

	error = ad970x_set_ceiled_calclk_div(ad970x, calclk_div, &calclk_div);
	if (error) {
		dev_err(dev, "Failed to set calclk div: %d\n", error);
		return error;
	}

	error = ad970x_set_calclk(ad970x, true);
	if (error) {
		dev_err(dev, "Failed to enable calibration clock: %d\n", error);
		return error;
	}
	error = ad970x_set_calen(ad970x, true);
	if (error) {
		dev_err(dev, "Failed to start calibration: %d\n", error);
		return error;
	}

	/* Wait until calibration completes */
	for (i = 0; max_tries > i; ++i) {
		/* Convert freq to period (Hz to ns) */
		calclk_period_ns = 1000000000 / (clk_rate / calclk_div);
		/* Wait 4500 calibration clock cycles. This should be enough
		 * according to the data sheet. */
		cal_time_us = (calclk_period_ns * AD970X_CALCLK_CAL_CYCLES) / 1000;
		/* Sleep for the calibration time (but at most 100 ms) */
		usleep_range(cal_time_us, 100000);
		/* Check completion status */
		error = ad970x_get_calstat(ad970x, &calstat);
		if (error) {
			dev_err(dev, "Failed to get calibration status: %d\n", error);
			return error;
		}
		dev_dbg(dev, "Calibration status: %d\n", calstat);
		if (1 == calstat) {
			dev_dbg(dev, "Calibration completed after %d sleep cycles.\n", i + 1);
			break;
		}
	}
	/* Calibration did not complete in time */
	if (i == max_tries) {
		dev_err(dev, "Calibration did not complete in time.\n");
		return -EFAULT;
	}

	error = ad970x_acknowledge_cal(ad970x);
	if (error) {
		dev_err(dev, "Failed to acknowledge calibration: %d.\n", error);
		return error;
	}

	error = ad970x_set_calclk(ad970x, false);
	if (error) {
		dev_err(dev, "Failed to disable calibration clock: %d\n", error);
		return error;
	}

	error = ad970x_get_calmem(ad970x, &calmem);
	if (error) {
		dev_err(dev, "Failed to get calibration status: %d\n", error);
		return error;
	}
	dev_dbg(dev, "Calmem register: 0x%x\n", calmem);
	if (AD970X_SELF_CALIBRATION != calmem) {
		dev_err(dev, "Calibration did not complete correctly.\n");
		return -EFAULT;
	}

	dev_dbg(dev, "Calibration completed successfully.\n");
	return 0;
}

static int ad970x_apply_state(struct device *dev, struct ad970x_state *state)
{
	struct ad970x *ad970x = dev_get_drvdata(dev);
	struct spi_device *spi = container_of(dev, struct spi_device, dev);
	int error;
	error = ad970x_reset(ad970x);
	if (error) {
		dev_err(dev, "Failed to reset: %d\n", error);
		return error;
	}
	if (spi->mode & SPI_3WIRE) {
		dev_dbg(dev, "Using 3-wire SPI mode.\n");
	} else {
		dev_dbg(dev, "Using 4-wire SPI mode.\n");
		error = ad970x_set_spi_4wire(ad970x);
		if (error) {
			dev_err(dev, "Failed to enable 4-wire SPI mode: %d.\n", error);
			return error;
		}
	}
	error = ad970x_set_clkdiff(ad970x, state->clkdiff);
	if (error) {
		dev_err(dev, "Failed to enable clkdiff mode: %d.\n", error);
		return error;
	}
	error = ad970x_set_deskew(ad970x, state->deskew);
	if (error) {
		dev_err(dev, "Failed to enable deskew mode: %d.\n", error);
		return error;
	}
	error = ad970x_set_dclkpol(ad970x, state->dclkpol);
	if (error) {
		dev_err(dev, "Failed to enable dclkpol mode: %d.\n", error);
		return error;
	}
	error = ad970x_set_twos_complement(ad970x, state->twos_complement);
	if (error) {
		dev_err(dev, "Failed to enable two's complement mode: %d.\n", error);
		return error;
	}
	if (state->calibrate_on_init) {
		error = ad970x_calibrate(dev);
		if (error) {
			dev_err(dev, "Failed to calibrate: %d\n", error);
			return error;
		}
	} else {
		error = ad970x_set_uncal(ad970x, true);
		if (error) {
			dev_err(dev, "Failed to disable calibration coefficients: %d.\n",
			        error);
			return error;
		}
	}
	dev_dbg(dev, "Init completed successfully.\n");
	return 0;
}

static int ad970x_of_get_state(struct device *dev, struct ad970x_state *state)
{
	int error = 0;
	struct device_node *node = dev->of_node;
	bool bval;

	if (NULL == node) {
		return error;
	}

	/* Only override the given state if the property exists.
	 * Otherwise, we may erroneously override a 'true' value with
	 * 'false' when the property is missing. */
	bval = of_property_read_bool(node, "calibrate-on-init");
	if (bval) {
		state->calibrate_on_init = bval;
	}
	bval = of_property_read_bool(node, "diff-clock-input");
	if (bval) {
		state->clkdiff = bval;
	}
	bval = of_property_read_bool(node, "deskew-mode");
	if (bval) {
		state->deskew = bval;
	}
	bval = of_property_read_bool(node, "data-on-clk-falling-edge");
	if (bval) {
		state->dclkpol = bval;
		if (!state->deskew) {
			dev_warn(dev, "Note that 'data-on-clk-falling-edge' only works in "
			              "deskew mode, which is disabled. Enable deskew mode "
			              "to fix this.\n");
		}
	}
	bval = of_property_read_bool(node, "twos-complement");
	if (bval) {
		state->twos_complement = bval;
	}

	return error;
}

static int ad970x_read_raw(struct iio_dev *indio_dev,
                            struct iio_chan_spec const *chan,
                            int *val, int *val2, long mask)
{
	return -EINVAL;
}

static ssize_t ad970x_write_powerdown(struct iio_dev *indio_dev,
                                       uintptr_t private,
                                       const struct iio_chan_spec *chan,
                                       const char *buf, size_t len)
{
	bool powerdown;
	int error;
	error = strtobool(buf, &powerdown);
	if (error) {
		return error;
	}
	if (powerdown) {
		error = pm_runtime_put_sync(indio_dev->dev.parent);
		if (error) {
			dev_err(&indio_dev->dev, "Failed to put pm runtime: %d\n", error);
			return error;
		}
	} else {
		error = pm_runtime_get_sync(indio_dev->dev.parent);
		if (error) {
			dev_err(&indio_dev->dev, "Failed to get pm runtime: %d\n", error);
			return error;
		}
	}
	return len;
}

/* Inspired by the powerdown channel of:
 * drivers/iio/dac/ad5758.c
 */
static const struct iio_chan_spec_ext_info ad970x_ext_info[] = {
	{
		.name = "powerdown",
		.write = ad970x_write_powerdown,
		.shared = IIO_SHARED_BY_ALL,
	},
	{ },
};

static const struct iio_chan_spec ad970x_channels[] = {
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 0,
		.output = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.ext_info = ad970x_ext_info,
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 1,
		.output = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.ext_info = ad970x_ext_info,
	},
};

static const struct iio_info ad970x_info = {
	.read_raw = ad970x_read_raw,
};

static int ad970x_probe(struct device *dev, struct regmap *regmap)
{
	int error;
	struct iio_dev *indio_dev;
	struct ad970x *ad970x;
	struct ad970x_state state = ad970x_default_state;
	const char *name = (dev->of_node) ? dev->of_node->name : NULL;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*ad970x));
	if (!indio_dev) {
		dev_err(dev, "Failed to allocate memory for the device.\n");
		return -ENOMEM;
	}
	indio_dev->dev.parent = dev;
	indio_dev->name = name;
	indio_dev->info = &ad970x_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ad970x_channels;
	indio_dev->num_channels = ARRAY_SIZE(ad970x_channels);
	ad970x = iio_priv(indio_dev);
	ad970x->regmap = regmap;
	dev_set_drvdata(dev, ad970x);

	/* vdd */
	ad970x->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(ad970x->vdd)) {
		dev_err(dev, "Failed to get VDD regulator: %ld\n",
		        PTR_ERR(ad970x->vdd));
		return PTR_ERR(ad970x->vdd);
	}

	/* clk */
	ad970x->clk = devm_clk_get(dev, "clk");
	if (IS_ERR(ad970x->clk)) {
		dev_err(dev, "Failed to get clock: %ld\n", PTR_ERR(ad970x->clk));
		return PTR_ERR(ad970x->clk);
	}

	/* power */
	pm_runtime_enable(dev);
	error = pm_runtime_get_sync(dev);
	if (error) {
		dev_err(dev, "Failed to get pm runtime: %d\n", error);
		goto out_pm_enable;
	}

	/* hw init */
	error = ad970x_of_get_state(dev, &state);
	if (error) {
		dev_err(dev, "Failed to get OF state: %d\n", error);
		goto out_pm_enable;
	}
	error = ad970x_apply_state(dev, &state);
	if (error) {
		dev_err(dev, "Failed to apply state: %d\n", error);
		goto out_pm_enable;
	}

	/* register iio device */
	error = devm_iio_device_register(dev, indio_dev);
	if (error) {
		dev_err(dev, "Failed to register iio device: %d\n", error);
		goto out_pm_enable;
	}

	/* power down */
	error = pm_runtime_put_sync(dev);
	if (error) {
		dev_err(dev, "Failed to put pm runtime: %d\n", error);
		goto out_pm_enable;
	}

	return 0;

out_pm_enable:
	pm_runtime_disable(dev);
	return error;
}

static bool ad970x_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case AD970X_SPI_CTL_REG:
	case AD970X_DATA_REG:
	case AD970X_CALMEM_REG:
	case AD970X_MEMRDWR_REG:
		return true;
	}
	return false;
}

static const struct regmap_config ad970x_regmap_spi_conf = {
	.reg_bits = 8, /* MSB is R/W bit; address is actually only 5 bit (and 2
	                  bits for transfer size) */
	.val_bits = 8,
	.write_flag_mask = 0x00, /* R/W bit is 0 */
	.read_flag_mask = 0x80, /* R/W bit is 1 */
	.max_register = 0x11,
	.cache_type = REGCACHE_FLAT,
	.volatile_reg = ad970x_volatile_reg,
};

static int ad970x_spi_probe(struct spi_device *spi)
{
	struct regmap *regmap;
	regmap = devm_regmap_init_spi(spi, &ad970x_regmap_spi_conf);
	if (IS_ERR(regmap)) {
		dev_err(&spi->dev, "Failed to register spi regmap: %ld\n",
		        PTR_ERR(regmap));
		return PTR_ERR(regmap);
	}
	return ad970x_probe(&spi->dev, regmap);
}

static int ad970x_pm_runtime_suspend(struct device *dev)
{
	int error = ad970x_disable(dev);
	if (error) {
		dev_err(dev, "Failed to disable device on suspend: %d\n", error);
		return error;
	}
	dev_dbg(dev, "Success\n");
	return 0;
}

static int ad970x_pm_runtime_resume(struct device *dev)
{
	struct ad970x *ad970x = dev_get_drvdata(dev);
	int error = ad970x_enable(dev);
	if (error) {
		dev_err(dev, "Failed to enable device on resume: %d\n", error);
		return error;
	}
	/* restore hw context (note that calibration information is saved
	 * in persistent storage, so it will be automatically preserved) */
	regcache_mark_dirty(ad970x->regmap);
	error = regcache_sync(ad970x->regmap);
	if (error) {
		dev_err(dev, "Failed to sync regmap cache on resume: %d\n", error);
		return error;
	}
	return 0;
}

static const struct dev_pm_ops ad970x_pm_ops= {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
	                        pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(ad970x_pm_runtime_suspend,
                       ad970x_pm_runtime_resume,
                       NULL)
};

static const struct of_device_id of_ad970x_match[] = {
	{ .compatible = "analogdevices,ad9704" },
	{},
};

static struct spi_driver ad970x_driver = {
	.probe = ad970x_spi_probe,
	.driver = {
		.name = "ad970x",
		.of_match_table = of_match_ptr(of_ad970x_match),
		.pm = &ad970x_pm_ops,
	},
};

module_spi_driver(ad970x_driver);

MODULE_DEVICE_TABLE(of, of_ad970x_match);
MODULE_AUTHOR("Frederik Peter Aalund <fpa@sbtinstruments.com>");
MODULE_DESCRIPTION("Analog Devices AD970x driver.");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:ad970x");
