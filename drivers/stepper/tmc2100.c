/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2019 Frederik Peter Aalund <fpa@sbtinstruments.com>
 */
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/regulator/consumer.h>
#include <linux/stepper.h>

#define TMC2100_REF_VOLTAGE_LOGICAL_MIN  500
#define TMC2100_REF_VOLTAGE_LOGICAL_MAX 2500
#define TMC2100_CFG_SIZE 6 /* Doesn't include cfg6_enn */

enum tmc2100_cfg_state {
	TMC2100_GND = 0,
	TMC2100_VCC_IO,
	TMC2100_OPEN,
	TMC_CFG_STATE_SIZE,
};

enum tmc2100_resolution {
	TMC2100_MS1_N_SPREAD = 0,
	TMC2100_MS2_N_SPREAD,
	TMC2100_MS2_Y_SPREAD,
	TMC2100_MS4_N_SPREAD,
	TMC2100_MS16_N_SPREAD,
	TMC2100_MS4_Y_SPREAD,
	TMC2100_MS16_Y_SPREAD,
	TMC2100_MS4_Y_STEALTH,
	TMC2100_MS16_Y_STEALTH,
	TMC2100_RESOLUTION_SIZE,
};

const char *const tmc2100_resolution_names[TMC2100_RESOLUTION_SIZE] = {
	"microstep-1,spread-cycle",
	"microstep-2,spread-cycle",
	"microstep-2,interpolation-256,spread-cycle",
	"microstep-4,spread-cycle",
	"microstep-16,spread-cycle",
	"microstep-4,interpolation-256,spread-cycle",
	"microstep-16,interpolation-256,spread-cycle",
	"microstep-4,interpolation-256,stealth-chop",
	"microstep-16,interpolation-256,stealth-chop",
};

struct tmc2100_state {
	enum tmc2100_cfg_state cfg[TMC2100_CFG_SIZE];
	unsigned int ref_voltage; /* mV */
};

struct tmc2100 {
	struct gpio_desc *cfg[TMC2100_CFG_SIZE];
	struct gpio_desc *cfg6_enn, *dir, *index, *error;
	struct pwm_device *step;
	struct regulator *ref;
	struct tmc2100_state state;
};

static struct stepper_vel_cfg tmc2100_cfg = {
	.rate_of_change = 1,
	.shift_delay_ms = 10,
	.min = -100,
	.max = 100,
};

static struct tmc2100_state tmc2100_default_state = {
	.cfg = {
		/* Not used in stealthChop mode. Can be set to any value. */
		TMC2100_GND,
		/* 16 microsteps (stealthChop mode enabled: interpolated up to
		 * 256 microsteps) */
		TMC2100_OPEN,
		TMC2100_OPEN,
		/* GND: Internal reference voltage. Current scale set by external
		 * sense resistors.
		 *
		 * Uses 6 W almost regardless of velocity
		 */
		/* VCC: Internal sense resistors. AIN sets reference current for
		 * internal sense resistors. Best results combined with stealthChop.
		 * Very power efficient!
		 */
		/* Open: External reference voltage on AIN. Current scale set by
		 * sense resistors and scaled by AIN.
		 * In between in terms of power efficiency
		 */
		TMC2100_GND,
		/* Not used in stealthChop mode. Can be set to any value. */
		TMC2100_GND,
		/* GND:  16 clock cycles
		 * VCC:  24 clock cycles
		 * Open: 36 clock cycles
		 *
		 * Data sheet says that 16 clock cycles is best for stealthChop mode
		 * but in practice this setting results in an unpleasant noise from
		 * the motor. We use 24 clock cycles, which is the "universal choice".
		 */
		TMC2100_VCC_IO,
	},
	/* Reference voltage should not be lower than about 0.5V to 1.0V.
	 * The maximum voltage is 2.5V. */
	.ref_voltage = 2500,
};

static int tmc2100_set_cfg(struct tmc2100 *tmc, int idx, enum tmc2100_cfg_state value)
{
	struct gpio_desc *desc;
	if (TMC2100_CFG_SIZE <= idx) {
		return -EINVAL;
	}
	desc = tmc->cfg[idx];
	/* The CFG GPIOs are tri-state so they can be set to input and detected
	 * as open. */
	switch (value) {
	case TMC2100_GND:
		return gpiod_direction_output(desc, 0);
	case TMC2100_VCC_IO:
		return gpiod_direction_output(desc, 1);
	case TMC2100_OPEN:
		return gpiod_direction_input(desc);
	default:
		return -EINVAL;
	};
}

static int tmc2100_apply_state_to_hw(struct tmc2100 *tmc)
{
	int ret;
	int i;
	int voltage_uv;
	struct tmc2100_state *state = &tmc->state;
	for (i = 0; TMC2100_CFG_SIZE > i; ++i) {
		ret = tmc2100_set_cfg(tmc, i, state->cfg[i]);
		if (0 != ret) {
			return ret;
		}
	}
	voltage_uv = state->ref_voltage * 1000; /* mV to uV */
	ret = regulator_set_voltage(tmc->ref, voltage_uv, voltage_uv);
	if (ret) {
		return ret;
	}
	return 0;
}

/**
 * @velocity unitless value between -100 and 100
 */
static void tmc2100_get_pwm_state(struct tmc2100 *tmc, int velocity,
                                  struct pwm_state *state)
{
	/* Linear increase in frequency from hz_min (at speed 1)
	 * to hz_max (at speed 100).
	 */
	int hz_min = 200;
	int hz_max = 25000;
	int hz_range = hz_max - hz_min;
	int freq;
	state->polarity = PWM_POLARITY_NORMAL;
	if (0 != velocity) {
		freq = (abs(velocity) - 1) * hz_range / (tmc2100_cfg.max - 1) + hz_min;
		/* Convert frequency to corresponding period (Hz to ns) */
		state->period = 1000000000 / freq;
		state->duty_cycle = state->period / 2; /* 50 % */
		state->enabled = true;
	} else {
		state->period = 0;
		state->duty_cycle = 0;
		state->enabled = false;
	}
}

/**
 * @velocity unitless value between -100 and 100
 */
static int tmc2100_set_velocity(struct device *dev, int velocity)
{
	int forward = 0 <= velocity;
	struct tmc2100 *tmc = dev_get_drvdata(dev);
	struct pwm_state state;
	tmc2100_get_pwm_state(tmc, velocity, &state);
	pwm_apply_state(tmc->step, &state);
	gpiod_set_value(tmc->dir, forward);
	gpiod_set_value(tmc->cfg6_enn, state.enabled);
	return 0;
}

/**
 * @abs_torque unitless value between 0 and 100
 */
static int tmc2100_get_abs_torque(struct device *dev, unsigned* abs_torque)
{
	struct tmc2100 *tmc = dev_get_drvdata(dev);
	/* Convert [VOL_MIN; VOL_MAX] to [0; 100]  */
	unsigned diff = (TMC2100_REF_VOLTAGE_LOGICAL_MAX - TMC2100_REF_VOLTAGE_LOGICAL_MIN);
	unsigned voltage_uv = regulator_get_voltage(tmc->ref);
	unsigned voltage_mv = voltage_uv / 1000;
	*abs_torque = min(100u, (voltage_mv - TMC2100_REF_VOLTAGE_LOGICAL_MIN) * 100u / diff);
	return 0;
}

/**
 * @abs_torque unitless value between 0 and 100
 */
static int tmc2100_set_abs_torque(struct device *dev, unsigned abs_torque)
{
	struct tmc2100 *tmc = dev_get_drvdata(dev);
	/* Convert [0; 100] to [VOL_MIN; VOL_MAX] */
	unsigned diff = (TMC2100_REF_VOLTAGE_LOGICAL_MAX - TMC2100_REF_VOLTAGE_LOGICAL_MIN);
	unsigned voltage_mv = (diff * abs_torque) / 100 + TMC2100_REF_VOLTAGE_LOGICAL_MIN;
	unsigned voltage_uv = voltage_mv * 1000;
	return regulator_set_voltage(tmc->ref, voltage_uv, voltage_uv);
}

static struct stepper_ops tmc2100_ops = {
	.set_velocity = tmc2100_set_velocity,
	.get_abs_torque = tmc2100_get_abs_torque,
	.set_abs_torque = tmc2100_set_abs_torque,
};

static int tmc2100_get_gpios(struct tmc2100 *tmc, struct platform_device *pdev)
{
	int i;
	/* cfg0-5 */
	for (i = 0; ARRAY_SIZE(tmc->cfg) > i; ++i) {
		tmc->cfg[i] = devm_gpiod_get_index(&pdev->dev, "cfg", i,
		                                   GPIOD_OUT_HIGH);
		if (IS_ERR(tmc->cfg[i])) {
			dev_err(&pdev->dev, "Failed to get cfg%d GPIO: %ld.\n", i,
			        PTR_ERR(tmc->cfg[i]));
			return PTR_ERR(tmc->cfg[i]);
		}
	}
	/* cfg6-enn */
	tmc->cfg6_enn = devm_gpiod_get(&pdev->dev, "cfg6-enn", GPIOD_OUT_LOW);
	if (IS_ERR(tmc->cfg6_enn)) {
		dev_err(&pdev->dev, "Failed to get cfg6-enn GPIO: %ld.\n",
		        PTR_ERR(tmc->cfg6_enn));
		return PTR_ERR(tmc->cfg6_enn);
	}
	/* dir */
	tmc->dir = devm_gpiod_get(&pdev->dev, "dir", GPIOD_OUT_LOW);
	if (IS_ERR(tmc->dir)) {
		dev_err(&pdev->dev, "Failed to get dir GPIO: %ld.\n",
		        PTR_ERR(tmc->dir));
		return PTR_ERR(tmc->dir);
	}
	/* index */
	tmc->index = devm_gpiod_get(&pdev->dev, "index", GPIOD_IN);
	if (IS_ERR(tmc->index)) {
		dev_err(&pdev->dev, "Failed to get index GPIO: %ld.\n",
		        PTR_ERR(tmc->index));
		return PTR_ERR(tmc->index);
	}
	/* error */
	tmc->error = devm_gpiod_get(&pdev->dev, "error", GPIOD_IN);
	if (IS_ERR(tmc->error)) {
		dev_err(&pdev->dev, "Failed to get error GPIO: %ld.\n",
		        PTR_ERR(tmc->error));
		return PTR_ERR(tmc->error);
	}
	return 0;
}

static int tmc2100_get_pwms(struct tmc2100 *tmc, struct platform_device *pdev)
{
	/* step */
	tmc->step = devm_pwm_get(&pdev->dev, "step");
	if (IS_ERR(tmc->step)) {
		dev_err(&pdev->dev, "Failed to get step PWM: %ld.\n",
		        PTR_ERR(tmc->step));
		return PTR_ERR(tmc->step);
	}
	return 0;
}

static int tmc2100_get_regulators(struct tmc2100 *tmc, struct platform_device *pdev)
{
	/* ref */
	tmc->ref = devm_regulator_get(&pdev->dev, "ref");
	if (IS_ERR(tmc->ref)) {
		dev_err(&pdev->dev, "Failed to get 'ref' regulator.\n");
		return PTR_ERR(tmc->ref);
	}
	return 0;
}

static int tmc2100_init_pwms(struct tmc2100 *tmc)
{
	struct pwm_state step_state;
	pwm_init_state(tmc->step, &step_state);
	step_state.enabled = false;
	return pwm_apply_state(tmc->step, &step_state);
}

static int tmc2100_init_handles(struct tmc2100 *tmc, struct platform_device *pdev)
{
	int ret;
	ret = tmc2100_get_gpios(tmc, pdev);
	if (ret) {
		return ret;
	}
	ret = tmc2100_get_pwms(tmc, pdev);
	if (ret) {
		return ret;
	}
	ret = tmc2100_get_regulators(tmc, pdev);
	if (ret) {
		return ret;
	}
	gpiod_set_value(tmc->cfg6_enn, 0);
	ret = tmc2100_init_pwms(tmc);
	if (ret) {
		dev_err(&pdev->dev, "Failed to initialize pwms: %d\n", ret);
		return ret;
	}
	ret = regulator_enable(tmc->ref);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable regulator 'ref': %d\n", ret);
		return ret;
	}
	return 0;
}

static int tmc2100_of_get_state(struct device *dev, struct tmc2100_state *state)
{
	int ret = 0;
	int i;
	struct device_node *of_node = dev->of_node;
	char cfg_str[5];
	const char *cfg_value;
	enum tmc2100_cfg_state cfg_state;
	u32 ref_voltage;

	if (NULL == of_node) {
		return ret;
	}

	for (i = 0; TMC2100_CFG_SIZE > i; ++i) {
		snprintf(cfg_str, ARRAY_SIZE(cfg_str), "cfg%d", i);
		if (of_property_read_string(of_node, cfg_str, &cfg_value)) {
			continue;
		}
		if (0 == strcmp(cfg_value, "gnd")) {
			cfg_state = TMC2100_GND;
		} else if (0 == strcmp(cfg_value, "vcc_io")) {
			cfg_state = TMC2100_VCC_IO;
		} else if (0 == strcmp(cfg_value, "open")) {
			cfg_state = TMC2100_OPEN;
		} else {
			dev_warn(dev, "Invalid %s state: %s. Using default.\n",
			         cfg_str, cfg_value);
			continue;
		}
		state->cfg[i] = cfg_state;
	}

	ret = of_property_read_u32(of_node, "ref-voltage", &ref_voltage);
	if (ret == -EINVAL) {
		/* ref-voltage was not set in the device tree. Ignore. */
		ret = 0;
	} else {
		/* the given ref-voltage is not within spec */
		if (ret || TMC2100_REF_VOLTAGE_LOGICAL_MAX < ref_voltage
				|| TMC2100_REF_VOLTAGE_LOGICAL_MIN > ref_voltage) {
			dev_warn(dev, "Invalid ref_voltage: %d. Must be between %d and %d. Using default.\n",
		             ref_voltage, TMC2100_REF_VOLTAGE_LOGICAL_MIN,
		             TMC2100_REF_VOLTAGE_LOGICAL_MAX);
			ret = 0; /* Ignore the error since we are simply using the default */
		} else {
			state->ref_voltage = ref_voltage;
		}
	}

	return ret;
}

static int tmc2100_get_resolution(struct tmc2100 *tmc, enum tmc2100_resolution *res)
{
	/* 2D table look-up */
	*res = tmc->state.cfg[2] * TMC_CFG_STATE_SIZE + tmc->state.cfg[1];
	return 0;
}

static int tmc2100_set_resolution(struct tmc2100 *tmc, enum tmc2100_resolution res)
{
	int ret;
	enum tmc2100_cfg_state cfg1, cfg2;
	/* Inverse 2D table look-up */
	cfg2 = res / TMC_CFG_STATE_SIZE;
	cfg1 = res % TMC_CFG_STATE_SIZE;
	/* Apply state */
	ret = tmc2100_set_cfg(tmc, 1, cfg1);
	if (ret != 0)
		return ret;
	tmc->state.cfg[1] = cfg1;
	ret = tmc2100_set_cfg(tmc, 2, cfg2);
	if (ret != 0)
		return ret;
	tmc->state.cfg[2] = cfg2;
	return 0;
}

/* resolution */
static ssize_t tmc2100_resolution_show(
	struct device *dev,
	struct device_attribute *attr,
	char *buf)
{
	int ret;
	enum tmc2100_resolution res;
	const char *res_name;
	struct tmc2100 *tmc = dev_get_drvdata(dev);
	ret = tmc2100_get_resolution(tmc, &res);
	if (0 != ret)
		return ret;
	res_name = tmc2100_resolution_names[res];
	ret = scnprintf(buf, PAGE_SIZE, "%s\n", res_name);
	if (0 != ret)
		return ret;
	return ret;
}
static ssize_t tmc2100_resolution_store(
	struct device *dev,
	struct device_attribute *attr,
	const char *buf,
	size_t count)
{
	int ret;
	struct tmc2100 *tmc = dev_get_drvdata(dev);
	ret = sysfs_match_string(tmc2100_resolution_names, buf);
	if (0 > ret)
		return ret;
	ret = tmc2100_set_resolution(tmc, ret);
	if (0 != ret)
		return ret;
	return count;
}
DEVICE_ATTR(resolution, S_IRUGO | S_IWUSR, tmc2100_resolution_show, tmc2100_resolution_store);

static struct attribute *tmc2100_attrs[] = {
	&dev_attr_resolution.attr,
	NULL
};

static const struct attribute_group tmc2100_attr_group = {
	.attrs = tmc2100_attrs,
};

/* TODO: Replace tmc2100_attr_group with ATTRIBUTE_GROUPS when
 * we get kernel support for .dev_groups */
/* ATTRIBUTE_GROUPS(tmc2100); */

static int tmc2100_probe(struct platform_device *pdev)
{
	int ret;
	struct tmc2100 *tmc;
	struct device *classdev;

	tmc = devm_kzalloc(&pdev->dev, sizeof(*tmc), GFP_KERNEL);
	if (NULL == tmc) {
		dev_err(&pdev->dev, "Failed to allocate tmc2100 struct.\n");
		return -ENOMEM;
	}

	/* So that we can access the tmc2100 struct from, e.g., sysfs attributes */
	dev_set_drvdata(&pdev->dev, tmc);

	/* Get GPIOs, PWMs, regulators, etc. */
	ret = tmc2100_init_handles(tmc, pdev);
	if (0 != ret) {
		dev_err(&pdev->dev, "Failed to initialize %s.\n", pdev->name);
		return ret;
	}

	/* Set the default state.
	 * Note that we haven't applied this state to the hardware yet. */
	tmc->state = tmc2100_default_state;

	/* Get state modifications from the device tree */
	ret = tmc2100_of_get_state(&pdev->dev, &tmc->state);
	if (0 != ret) {
		dev_err(&pdev->dev, "Failed to get OF state.\n");
		return ret;
	}

	/* Now we apply the state to the HW. After this call, the state of the
	 * HW is synchronized with the state in the tmc2100 struct. */
	ret = tmc2100_apply_state_to_hw(tmc);
	if (0 != ret) {
		dev_err(&pdev->dev, "Failed to apply state.\n");
		return ret;
	}

	ret = devm_device_add_group(&pdev->dev, &tmc2100_attr_group);
	if (0 != ret) {
		dev_err(&pdev->dev, "Failed to add sysfs group: %d.\n", ret);
		return ret;
	}

	classdev = devm_stepper_device_register(&pdev->dev, pdev->name, tmc,
	                                        &tmc2100_ops, &tmc2100_cfg);

	/* Welcome message */
	dev_info(&pdev->dev, "Registered %s.\n", pdev->name);

	return PTR_ERR_OR_ZERO(classdev);
}

/* All allocations use devres so remove() is not needed. */

static const struct of_device_id of_tmc2100_match[] = {
	{ .compatible = "tmc2100" },
	{},
};

static struct platform_driver tmc2100_driver = {
	.probe = tmc2100_probe,
	.driver = {
		.name = "tmc2100",
		.of_match_table = of_match_ptr(of_tmc2100_match),
		/* TODO: Use .dev_groups instead of the devm_device_add_group call
		 * in the probe function when we get kernel support for .dev_groups */
		/* .dev_groups	= tmc2100_groups, */
	},
};

module_platform_driver(tmc2100_driver);

MODULE_DEVICE_TABLE(of, of_tmc2100_match);
MODULE_AUTHOR("Frederik Peter Aalund <fpa@sbtinstruments.com>");
MODULE_DESCRIPTION("TMC2100 stepper motor driver.");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:tmc2100");
