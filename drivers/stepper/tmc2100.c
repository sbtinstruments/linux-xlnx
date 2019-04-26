/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/stepper.h>

#include "tmc2100.h"

static struct stepper_vel_cfg tmc2100_cfg = {
	.rate_of_change = 1,
	.shift_delay_ms = 10,
	.min = -100,
	.max = 100,
};

#define TMC2100_VOLTAGE_MAX 3300

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
	int is = tmc2100_cfg.max - abs(velocity);
	state->period = is * (is / 10) + is + 125;;
	state->polarity = PWM_POLARITY_NORMAL;
	if (0 != velocity) {
		state->duty_cycle = state->period / 2; /* 50 % */
		state->enabled = true;
	} else {
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

static void tmc2100_init_gpios(struct tmc2100 *tmc)
{
	/* The GPIOs are tri-state so some are set to input intentionally. */
	/* Low setting, recommended */
	gpiod_set_value(tmc->cfg[0], 0);
	/* 16 usteps, interpolation (256 usteps), stealthChop */
	gpiod_direction_input(tmc->cfg[1]);
	gpiod_direction_input(tmc->cfg[2]);
	/* Use ref PWM, optimize for stealthChop */
	gpiod_set_value(tmc->cfg[3], 1);
	/* Low setting, recommended */
	gpiod_set_value(tmc->cfg[4], 0);
	/* Medium, universal choice */
	gpiod_set_value(tmc->cfg[5], 1);
	/* Enabled, standstill power down */
	gpiod_direction_input(tmc->cfg6_enn);
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
	tmc2100_init_gpios(tmc);
	tmc2100_init_pwms(tmc);
	tmc2100_set_ref_voltage(tmc, 2500); /* 2.5 V */
	return 0;
}

static int tmc2100_probe(struct platform_device *pdev)
{
	int result;
	struct tmc2100 *tmc;
	struct device *classdev;

	tmc = devm_kzalloc(&pdev->dev, sizeof(*tmc), GFP_KERNEL);
	if (NULL == tmc) {
		dev_err(&pdev->dev, "Failed to allocate tmc2100 struct.\n");
		return -ENOMEM;
	}

	/* Initialize lock-in amplifier */
	result = tmc2100_init(tmc, pdev);
	if (0 != result) {
		dev_err(&pdev->dev, "Failed to initialize %s.\n", pdev->name);
		return result;
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
	},
};

module_platform_driver(tmc2100_driver);

MODULE_DEVICE_TABLE(of, of_tmc2100_match);
MODULE_AUTHOR("Frederik Peter Aalund <fpa@sbtinstruments.com>");
MODULE_DESCRIPTION("TMC2100 stepper motor driver.");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:tmc2100");
