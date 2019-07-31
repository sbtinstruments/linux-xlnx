/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/stepper.h>

#define TMC2100_VOLTAGE_MAX 3300
#define TMC2100_CFG_SIZE 6 /* Doesn't include cfg6_enn */

struct tmc2100 {
	struct gpio_desc *cfg[TMC2100_CFG_SIZE];
	struct gpio_desc *cfg6_enn, *dir, *index, *error;
	struct pwm_device *ref, *step;
};

enum tmc2100_cfg_state {
	TMC2100_GND,
	TMC2100_VCC_IO,
	TMC2100_OPEN,
};

struct tmc2100_state {
	enum tmc2100_cfg_state cfg[TMC2100_CFG_SIZE];
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
};

static void tmc2100_set_cfg(struct tmc2100 *tmc, int idx, enum tmc2100_cfg_state value)
{
	if (TMC2100_CFG_SIZE <= idx) {
		pr_warn("TMC2100: Invalid CFG index %d. Skipping.\n", idx);
		return;
	}
	/* The CFG GPIOs are tri-state so they can be set to input and detected
	 * as open. */
	switch (value) {
	case TMC2100_GND:
		gpiod_direction_output(tmc->cfg[idx], 0);
		break;
	case TMC2100_VCC_IO:
		gpiod_direction_output(tmc->cfg[idx], 1);
		break;
	case TMC2100_OPEN:
		gpiod_direction_input(tmc->cfg[idx]);
		break;
	};
}

static void tmc2100_apply_state(struct tmc2100 *tmc, struct tmc2100_state *state)
{
	int i;
	for (i = 0; TMC2100_CFG_SIZE > i; ++i) {
		tmc2100_set_cfg(tmc, i, state->cfg[i]);
	}
}

/**
 * @voltage_mv voltage in mV between 0 and TMC2100_VOLTAGE_MAX
 */
static int tmc2100_set_ref_voltage(struct tmc2100 *tmc, int voltage_mv)
{
	int err;
	struct pwm_state state;
	pwm_get_state(tmc->ref, &state);
	err = pwm_set_relative_duty_cycle(&state, voltage_mv, TMC2100_VOLTAGE_MAX);
	if (err) {
		return err;
	}
	pwm_apply_state(tmc->ref, &state);
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

static struct stepper_ops tmc2100_ops = {
	.set_velocity = tmc2100_set_velocity,
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
	/* ref */
	tmc->ref = devm_pwm_get(&pdev->dev, "ref");
	if (IS_ERR(tmc->ref)) {
		dev_err(&pdev->dev, "Failed to get ref PWM: %ld.\n",
		        PTR_ERR(tmc->ref));
		return PTR_ERR(tmc->ref);
	}
	/* step */
	tmc->step = devm_pwm_get(&pdev->dev, "step");
	if (IS_ERR(tmc->step)) {
		dev_err(&pdev->dev, "Failed to get step PWM: %ld.\n",
		        PTR_ERR(tmc->step));
		return PTR_ERR(tmc->step);
	}
	return 0;
}

static void tmc2100_init_pwms(struct tmc2100 *tmc)
{
	struct pwm_state ref_state;
	struct pwm_state step_state;
	pwm_init_state(tmc->ref, &ref_state);
	ref_state.enabled = true;
	pwm_apply_state(tmc->ref, &ref_state);
	pwm_init_state(tmc->step, &step_state);
	step_state.enabled = false;
	pwm_apply_state(tmc->step, &step_state);
}

static int tmc2100_init(struct tmc2100 *tmc, struct platform_device *pdev)
{
	int err;
	err = tmc2100_get_gpios(tmc, pdev);
	if (err) {
		return err;
	}
	err = tmc2100_get_pwms(tmc, pdev);
	if (err) {
		return err;
	}
	gpiod_set_value(tmc->cfg6_enn, 0);
	tmc2100_init_pwms(tmc);
	tmc2100_set_ref_voltage(tmc, 2500); /* 2.5 V */
	return 0;
}

static int tmc2100_of_get_state(struct device *dev, struct tmc2100_state *state)
{
	int result = 0;
	int i;
	struct device_node *of_node = dev->of_node;
	char cfg_str[5];
	const char *cfg_value;
	enum tmc2100_cfg_state cfg_state;

	if (NULL == of_node) {
		return result;
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
			dev_warn(dev, "Invalid %s state: %s. Skipping.\n",
			         cfg_str, cfg_value);
			continue;
		}
		state->cfg[i] = cfg_state;
	}

	return result;
}

static int tmc2100_probe(struct platform_device *pdev)
{
	int result;
	struct tmc2100 *tmc;
	struct device *classdev;
	struct tmc2100_state state = tmc2100_default_state;

	tmc = devm_kzalloc(&pdev->dev, sizeof(*tmc), GFP_KERNEL);
	if (NULL == tmc) {
		dev_err(&pdev->dev, "Failed to allocate tmc2100 struct.\n");
		return -ENOMEM;
	}

	result = tmc2100_init(tmc, pdev);
	if (0 != result) {
		dev_err(&pdev->dev, "Failed to initialize %s.\n", pdev->name);
		return result;
	}

	result = tmc2100_of_get_state(&pdev->dev, &state);
	if (0 != result) {
		dev_err(&pdev->dev, "Failed to get OF state.\n");
		return result;
	}
	tmc2100_apply_state(tmc, &state);

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
	},
};

module_platform_driver(tmc2100_driver);

MODULE_DEVICE_TABLE(of, of_tmc2100_match);
MODULE_AUTHOR("Frederik Peter Aalund <fpa@sbtinstruments.com>");
MODULE_DESCRIPTION("TMC2100 stepper motor driver.");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:tmc2100");
