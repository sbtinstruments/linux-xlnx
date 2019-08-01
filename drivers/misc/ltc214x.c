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

#define LTC214X_RESET_REG			0x0 /* Reset register */
#define LTC214X_POWERDOWN_REG		0x1 /* Power-down register*/
#define LTC214X_TIMING_REG			0x2 /* Timing register */
#define LTC214X_OUTPUT_MODE_REG		0x3 /* Output mode register */
#define LTC214X_DATA_FORMAT_REG		0x4 /* Data format register */

/* RESET_REG bits */
#define LTC214X_RESET				BIT(7)
/* POWERDOWN_REG bits */
#define LTC214X_PWRCTRL_MASK 		0x3
/* TIMING_REG bits */
#define LTC214X_DCS					BIT(0) /* clock duty cycle stabilizer */
#define LTC214X_CLKINV				BIT(3)
#define LTC214X_CLKPHASE_MASK	 	0x6
/* DATA_FORMAT_REG bits */
#define LTC214X_TWOSCOMP			BIT(0)
#define LTC214X_RAND				BIT(1)
#define LTC214X_ABP					BIT(2)
#define LTC214X_TEST_PATTERN_MASK 	0x38

#define LTC214X_TO_VALUE(enabled) ((enabled) ? 0xFF : 0x0)

enum ltc214x_pwrctrl {
	LTC214X_NORMAL = 0x0,
	LTC214X_CHAN2_NAP = 0x1,
	LTC214X_BOTH_CHAN_NAP = 0x2,
	LTC214X_SLEEP_MODE = 0x3,
};

enum ltc214x_clkphase {
	LTC214X_DELAY_NONE = 0x0,
	LTC214X_DELAY_45 = 0x1,
	LTC214X_DELAY_90 = 0x2,
	LTC214X_DELAY_135 = 0x3,
};

enum ltc214x_test_pattern {
	LTC214X_TEST_PATTERN_OFF = 0x0,
	LTC214X_ALL_ZERO = 0x1,
	LTC214X_ALL_ONE = 0x3,
	LTC214X_CHECKERBOARD = 0x5,
	LTC214X_ALTERNATING = 0x7,
};

struct ltc214x {
	struct regmap *regmap;
	struct clk *enc;
	struct regulator *vdd;
};

struct ltc214x_state {
	bool clkinv;
	bool twos_complement;
	bool rand;
	bool abp;
	enum ltc214x_pwrctrl power_control;
	enum ltc214x_clkphase clkphase;
	enum ltc214x_test_pattern test_pattern;
};

static struct ltc214x_state ltc214x_default_state = {
	.clkinv = false,
	.twos_complement = false,
	.rand = false,
	.abp = false,
	.power_control = LTC214X_NORMAL,
	.clkphase = LTC214X_DELAY_NONE,
	.test_pattern = LTC214X_TEST_PATTERN_OFF,
};

static int ltc214x_reset(struct ltc214x *ltc214x)
{
	return regmap_write(ltc214x->regmap, LTC214X_RESET_REG, LTC214X_RESET);
}

static int ltc214x_set_pwrctrl(struct ltc214x *ltc214x,
                               enum ltc214x_pwrctrl pwrctrl)
{
	return regmap_update_bits(ltc214x->regmap, LTC214X_POWERDOWN_REG,
	                          LTC214X_PWRCTRL_MASK, pwrctrl);
}

static int ltc214x_set_clkinv(struct ltc214x *ltc214x, bool enabled)
{
	return regmap_update_bits(ltc214x->regmap, LTC214X_TIMING_REG,
	                          LTC214X_CLKINV, LTC214X_TO_VALUE(enabled));
}

static int ltc214x_set_clkphase(struct ltc214x *ltc214x,
                                enum ltc214x_clkphase clkphase)
{
	return regmap_update_bits(ltc214x->regmap, LTC214X_TIMING_REG,
	                          LTC214X_CLKPHASE_MASK,
	                          (unsigned int)clkphase << 1);
}

static int ltc214x_set_dcs(struct ltc214x *ltc214x, bool enabled)
{
	return regmap_update_bits(ltc214x->regmap, LTC214X_TIMING_REG,
	                          LTC214X_DCS, LTC214X_TO_VALUE(enabled));
}

static int ltc214x_set_twoscomp(struct ltc214x *ltc214x, bool enabled)
{
	return regmap_update_bits(ltc214x->regmap, LTC214X_DATA_FORMAT_REG,
	                          LTC214X_TWOSCOMP, LTC214X_TO_VALUE(enabled));
}

static int ltc214x_set_rand(struct ltc214x *ltc214x, bool enabled)
{
	return regmap_update_bits(ltc214x->regmap, LTC214X_DATA_FORMAT_REG,
	                          LTC214X_RAND, LTC214X_TO_VALUE(enabled));
}

static int ltc214x_set_abp(struct ltc214x *ltc214x, bool enabled)
{
	return regmap_update_bits(ltc214x->regmap, LTC214X_DATA_FORMAT_REG,
	                          LTC214X_ABP, LTC214X_TO_VALUE(enabled));
}

static int ltc214x_set_test_pattern(struct ltc214x *ltc214x,
                                    enum ltc214x_test_pattern pattern)
{
	return regmap_update_bits(ltc214x->regmap, LTC214X_DATA_FORMAT_REG,
	                          LTC214X_TEST_PATTERN_MASK,
	                          (unsigned int)pattern << 3);
}

static int ltc214x_disable(struct device *dev)
{
	struct ltc214x *ltc214x = dev_get_drvdata(dev);
	int error;
	error = regulator_disable(ltc214x->vdd);
	if (error) {
		dev_err(dev, "Failed to disable VDD regulator: %d\n", error);
		return error;
	}
	clk_disable_unprepare(ltc214x->enc);
	return 0;
}

static int ltc214x_enable(struct device *dev)
{
	struct ltc214x *ltc214x = dev_get_drvdata(dev);
	int error;
	error = regulator_enable(ltc214x->vdd);
	if (error) {
		dev_err(dev, "Failed to enable VDD regulator: %d\n", error);
		return error;
	}
	error = clk_prepare_enable(ltc214x->enc);
	if (error < 0) {
		dev_err(dev, "Failed to enable ENC clock: %d\n", error);
		return error;
	}
	/* Wait a bit for the hw to power up.
	 * The duration is chosen arbitrarily. */
	msleep(10);
	return 0;
}

static int ltc214x_apply_state(struct device *dev, struct ltc214x_state *state)
{
	struct ltc214x *ltc214x = dev_get_drvdata(dev);
	int error;
	/* Reset is volatile (dispatched to hw immediately) */
	error = ltc214x_reset(ltc214x);
	if (error) {
		dev_err(dev, "Failed to reset: %d\n", error);
		return error;
	}
	/* Other registries are cached */
	error = ltc214x_set_pwrctrl(ltc214x, state->power_control);
	if (error) {
		dev_err(dev, "Failed to set power control: %d\n", error);
		return error;
	}
	error = ltc214x_set_clkinv(ltc214x, state->clkinv);
	if (error) {
		dev_err(dev, "Failed to set clock invert: %d\n", error);
		return error;
	}
	error = ltc214x_set_clkphase(ltc214x, state->clkphase);
	if (error) {
		dev_err(dev, "Failed to set clock phase: %d\n", error);
		return error;
	}
	error = ltc214x_set_dcs(ltc214x, true);
	if (error) {
		dev_err(dev, "Failed to enable DCS: %d\n", error);
		return error;
	}
	error = ltc214x_set_twoscomp(ltc214x, state->twos_complement);
	if (error) {
		dev_err(dev, "Failed to set two's complement: %d\n", error);
		return error;
	}
	error = ltc214x_set_rand(ltc214x, state->rand);
	if (error) {
		dev_err(dev, "Failed to set randomizer: %d\n", error);
		return error;
	}
	error = ltc214x_set_abp(ltc214x, state->abp);
	if (error) {
		dev_err(dev, "Failed to set ABP: %d\n", error);
		return error;
	}
	error = ltc214x_set_test_pattern(ltc214x, state->test_pattern);
	if (error) {
		dev_err(dev, "Failed to set test pattern: %d\n", error);
		return error;
	}
	/* Synchronize cache with the hw */
	error = regcache_sync(ltc214x->regmap);
	if (error) {
		dev_err(dev, "Failed to sync regmap cache: %d\n", error);
		return error;
	}
	return 0;
}

static int ltc214x_of_read_clkphase(struct device_node *of_node,
                                    enum ltc214x_clkphase *clkphase,
                                    u32 *clkphase_degrees)
{
	if (!of_property_read_u32(of_node, "clock-phase-delay", clkphase_degrees)) {
		switch (*clkphase_degrees) {
		case 0:
			*clkphase = LTC214X_DELAY_NONE;
			break;
		case 45:
			*clkphase = LTC214X_DELAY_45;
			break;
		case 90:
			*clkphase = LTC214X_DELAY_90;
			break;
		case 135:
			*clkphase = LTC214X_DELAY_135;
			break;
		default:
			return -EINVAL;
		}
	}
	return 0;
}

static int ltc214x_of_read_test_pattern(struct device_node *of_node,
                                        enum ltc214x_test_pattern *pattern,
                                        const char **pattern_name)
{
	if (!of_property_read_string(of_node, "test-pattern", pattern_name)) {
		if (0 == strcmp(*pattern_name, "none")) {
			*pattern = LTC214X_TEST_PATTERN_OFF;
		} else if (0 == strcmp(*pattern_name, "all-zero")) {
			*pattern = LTC214X_ALL_ZERO;
		} else if (0 == strcmp(*pattern_name, "all-one")) {
			*pattern = LTC214X_ALL_ONE;
		} else if (0 == strcmp(*pattern_name, "checkerboard")) {
			*pattern = LTC214X_CHECKERBOARD;
		} else if (0 == strcmp(*pattern_name, "alternating")) {
			*pattern = LTC214X_ALTERNATING;
		} else {
			return -EINVAL;
		}
	}
	return 0;
}

static int ltc214x_of_read_pwrctrl(struct device_node *of_node,
                                   enum ltc214x_pwrctrl *pwrctrl,
                                   const char **pwrctrl_name)
{
	if (!of_property_read_string(of_node, "power-control", pwrctrl_name)) {
		if (0 == strcmp(*pwrctrl_name, "normal")) {
			*pwrctrl = LTC214X_NORMAL;
		} else if (0 == strcmp(*pwrctrl_name, "channel2-nap")) {
			*pwrctrl = LTC214X_CHAN2_NAP;
		} else if (0 == strcmp(*pwrctrl_name, "both-channels-nap")) {
			*pwrctrl = LTC214X_BOTH_CHAN_NAP;
		} else if (0 == strcmp(*pwrctrl_name, "sleep-mode")) {
			*pwrctrl = LTC214X_SLEEP_MODE;
		} else {
			return -EINVAL;
		}
	}
	return 0;
}

static int ltc214x_of_get_state(struct device *dev, struct ltc214x_state *state)
{
	int error = 0;
	struct device_node *node = dev->of_node;
	const char *pattern_name;
	const char *pwrctrl_name;
	u32 clkphase_degrees;
	bool bval;

	if (NULL == node) {
		return error;
	}

	/* Only override the given state if the property exists.
	 * Otherwise, we may erroneously override a 'true' value with
	 * 'false' when the property is missing. */
	bval = of_property_read_bool(node, "invert-clock");
	if (bval) {
		state->clkinv = bval;
	}
	bval = of_property_read_bool(node, "twos-complement");
	if (bval) {
		state->twos_complement = bval;
	}
	bval = of_property_read_bool(node, "output-randomizer");
	if (bval) {
		state->rand = bval;
	}
	bval = of_property_read_bool(node, "alternate-bit-polarity");
	if (bval) {
		state->abp = bval;
	}

	error = ltc214x_of_read_pwrctrl(node,
	                                &state->power_control,
	                                &pwrctrl_name);
	if (error) {
		dev_warn(dev, "Invalid power control mode: %s. "
		              "Using normal operation mode.\n", pwrctrl_name);
		return error;
	}

	error = ltc214x_of_read_clkphase(node,
	                                 &state->clkphase,
	                                 &clkphase_degrees);
	if (error) {
		dev_warn(dev, "Invalid clock phase delay: %d. "
		              "Using default clock phase delay.\n", clkphase_degrees);
		return error;
	}

	error = ltc214x_of_read_test_pattern(node,
	                                     &state->test_pattern,
	                                     &pattern_name);
	if (error) {
		dev_warn(dev, "Invalid test pattern: %s. "
		              "Disabling test pattern.\n", pattern_name);
		return error;
	}

	return error;
}

static int ltc214x_read_raw(struct iio_dev *indio_dev,
                            struct iio_chan_spec const *chan,
                            int *val, int *val2, long mask)
{
	return -EINVAL;
}

static ssize_t ltc214x_write_powerdown(struct iio_dev *indio_dev,
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
static const struct iio_chan_spec_ext_info ltc214x_ext_info[] = {
	{
		.name = "powerdown",
		.write = ltc214x_write_powerdown,
		.shared = IIO_SHARED_BY_ALL,
	},
	{ },
};

static const struct iio_chan_spec ltc214x_channels[] = {
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.ext_info = ltc214x_ext_info,
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.ext_info = ltc214x_ext_info,
	},
};

static const struct iio_info ltc214x_info = {
	.read_raw = ltc214x_read_raw,
};

static int ltc214x_probe(struct device *dev, struct regmap *regmap)
{
	int error;
	struct iio_dev *indio_dev;
	struct ltc214x *ltc214x;
	struct ltc214x_state state = ltc214x_default_state;
	const char *name = (dev->of_node) ? dev->of_node->name : NULL;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*ltc214x));
	if (!indio_dev) {
		dev_err(dev, "Failed to allocate memory for the device.\n");
		return -ENOMEM;
	}
	indio_dev->dev.parent = dev;
	indio_dev->name = name;
	indio_dev->info = &ltc214x_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ltc214x_channels;
	indio_dev->num_channels = ARRAY_SIZE(ltc214x_channels);

	ltc214x = iio_priv(indio_dev);
	ltc214x->regmap = regmap;
	dev_set_drvdata(dev, ltc214x);

	/* vdd */
	ltc214x->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(ltc214x->vdd)) {
		dev_err(dev, "Failed to get VDD regulator: %ld\n",
		        PTR_ERR(ltc214x->vdd));
		return PTR_ERR(ltc214x->vdd);
	}

	/* enc */
	ltc214x->enc = devm_clk_get(dev, "enc");
	if (IS_ERR(ltc214x->enc)) {
		dev_err(dev, "Failed to get ENC clock: %ld\n", PTR_ERR(ltc214x->enc));
		return PTR_ERR(ltc214x->enc);
	}

	/* power */
	pm_runtime_enable(dev);
	error = pm_runtime_get_sync(dev);
	if (error) {
		dev_err(dev, "Failed to get pm runtime: %d\n", error);
		goto out_pm_enable;
	}

	/* hw init */
	error = ltc214x_of_get_state(dev, &state);
	if (error) {
		dev_err(dev, "Failed to get OF state: %d\n", error);
		goto out_pm_enable;
	}
	error = ltc214x_apply_state(dev, &state);
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

static bool ltc214x_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case LTC214X_RESET_REG:
		return true;
	}
	return false;
}

static const struct regmap_config ltc214x_regmap_spi_conf = {
	.reg_bits = 8, /* MSB is R/W bit; address is actually only 7 bit */
	.val_bits = 8,
	.write_flag_mask = 0x00, /* R/W bit is 0 */
	.read_flag_mask = 0x80, /* R/W bit is 1 */
	.max_register = 0x4,
	.cache_type = REGCACHE_FLAT,
	.volatile_reg = ltc214x_volatile_reg,
};

static int ltc214x_spi_probe(struct spi_device *spi)
{
	struct regmap *regmap;
	regmap = devm_regmap_init_spi(spi, &ltc214x_regmap_spi_conf);
	if (IS_ERR(regmap)) {
		dev_err(&spi->dev, "Failed to register spi regmap: %ld\n",
		        PTR_ERR(regmap));
		return PTR_ERR(regmap);
	}
	return ltc214x_probe(&spi->dev, regmap);
}

static int ltc214x_pm_runtime_suspend(struct device *dev)
{
	int error = ltc214x_disable(dev);
	if (error) {
		dev_err(dev, "Failed to disable device on suspend: %d\n", error);
		return error;
	}
	dev_dbg(dev, "Success\n");
	return 0;
}

static int ltc214x_pm_runtime_resume(struct device *dev)
{
	struct ltc214x *ltc214x = dev_get_drvdata(dev);
	int error = ltc214x_enable(dev);
	if (error) {
		dev_err(dev, "Failed to enable device on resume: %d\n", error);
		return error;
	}
	/* restore hw context */
	regcache_mark_dirty(ltc214x->regmap);
	error = regcache_sync(ltc214x->regmap);
	if (error) {
		dev_err(dev, "Failed to sync regmap cache on resume: %d\n", error);
		return error;
	}
	dev_dbg(dev, "Success\n");
	return 0;
}

static const struct dev_pm_ops ltc214x_pm_ops= {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
	                        pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(ltc214x_pm_runtime_suspend,
                       ltc214x_pm_runtime_resume,
                       NULL)
};

static const struct of_device_id of_ltc214x_match[] = {
	{ .compatible = "lineartechnology,ltc2145" },
	{},
};

static struct spi_driver ltc214x_driver = {
	.probe = ltc214x_spi_probe,
	.driver = {
		.name = "ltc214x",
		.of_match_table = of_match_ptr(of_ltc214x_match),
		.pm = &ltc214x_pm_ops,
	},
};

module_spi_driver(ltc214x_driver);

MODULE_DEVICE_TABLE(of, of_ltc214x_match);
MODULE_AUTHOR("Frederik Peter Aalund <fpa@sbtinstruments.com>");
MODULE_DESCRIPTION("Linear Technology LTC214x driver.");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:ltc2145");
