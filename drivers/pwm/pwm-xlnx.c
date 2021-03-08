/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * pwm-xlnx driver
 * Tested by Thomas More on zedboard - axi timer v2.00a - test
 * Tested by Frederik Peter Aalund on custom Zynq7020-based board
 *
 * Copyright (C) 2014 Thomas More
 * Copyright (C) 2019 Frederik Peter Aalund <fpa@sbtinstruments.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

/* mmio regiser mapping */

#define TCSR0		0x00	/* Timer 0 Control and Status Register */
#define TLR0		0x04	/* Timer 0 Load Register */
#define TCSR1		0x10	/* Timer 1 Control and Status Register */
#define TLR1		0x14	/* Timer 1 Load Register */

#define PERIOD		TLR0
#define DUTY		TLR1

#define UDT_BIT		BIT(1)	/* Up/Down Count Timer */
#define GENT_BIT	BIT(2)	/* Enable External Generate Signal Timer */
#define ENT_BIT		BIT(7)	/* Enable Timer */
#define PWMA_BIT	BIT(9)	/* Enable Pulse Width Modulation for Timer */
#define PWM_CONF	(UDT_BIT | GENT_BIT | ENT_BIT | PWMA_BIT)

struct xlnx_pwm_chip {
	struct pwm_chip chip;
	struct device *dev;
	int clk_period;
	void __iomem *mmio_base;
};

static inline struct xlnx_pwm_chip *to_xlnx_pwm_chip(struct pwm_chip *chip)
{
	return container_of(chip, struct xlnx_pwm_chip, chip);
}

static int xlnx_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
                           int duty_ns, int period_ns)
{
	struct xlnx_pwm_chip *pc = container_of(chip, struct xlnx_pwm_chip, chip);
	int tlrx_duty = max(2, duty_ns / pc->clk_period) - 2;
	int tlrx_period = max(2, period_ns / pc->clk_period) - 2;
	/* When duty_cycle==period, the output is zero. The output should have
	 * been full saturation. As a workaround, we cap the duty cycle so that
	 * duty_cycle<=period. In practice, this means that the output will never
	 * reach full saturation (100% duty cycle) but only close to it (~99.9%
	 * duty cycle). */
	tlrx_duty = min(tlrx_period - 1, tlrx_duty);
	dev_dbg(chip->dev, "duty cycle [ns]: %d\n", duty_ns);
	dev_dbg(chip->dev, "period     [ns]: %d\n", period_ns);
	dev_dbg(chip->dev, "clk_period  [1]: %d\n", pc->clk_period);
	dev_dbg(chip->dev, "tlrx_duty   [1]: %d\n", tlrx_duty);
	dev_dbg(chip->dev, "tlrx_period [1]: %d\n", tlrx_period);
	iowrite32(tlrx_duty, pc->mmio_base + DUTY);
	iowrite32(tlrx_period, pc->mmio_base + PERIOD);
	return 0;
}

static int xlnx_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct xlnx_pwm_chip *pc = container_of(chip, struct xlnx_pwm_chip, chip);
	iowrite32(PWM_CONF, pc->mmio_base + TCSR0);
	iowrite32(PWM_CONF, pc->mmio_base + TCSR1);
	return 0;
}

static void xlnx_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct xlnx_pwm_chip *pc = to_xlnx_pwm_chip(chip);
	iowrite32(0, pc->mmio_base + TCSR0);
	iowrite32(0, pc->mmio_base + TCSR1);
}

static void xlnx_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
                               struct pwm_state *state)
{
	struct xlnx_pwm_chip *pc = container_of(chip, struct xlnx_pwm_chip, chip);
	unsigned int tcsr0 = ioread32(pc->mmio_base + TCSR0);
	unsigned int tcsr1 = ioread32(pc->mmio_base + TCSR1);
	bool timer0_enabled = (PWM_CONF & tcsr0) == PWM_CONF;
	bool timer1_enabled = (PWM_CONF & tcsr1) == PWM_CONF;
	state->enabled = timer0_enabled && timer1_enabled;
}

static int xlnx_pwm_set_polarity(struct pwm_chip *chip, struct pwm_device *pwm,
                                 enum pwm_polarity polarity)
{
	return -ENOTSUPP;
}

static const struct pwm_ops xlnx_pwm_ops = {
	.config = xlnx_pwm_config,
	.enable = xlnx_pwm_enable,
	.disable = xlnx_pwm_disable,
	.get_state = xlnx_pwm_get_state,
	.set_polarity = xlnx_pwm_set_polarity,
	.owner = THIS_MODULE,
};

static int xlnx_pwm_probe(struct platform_device *pdev)
{
	int ret;
	u32 start, end;
	struct resource *r;
	struct clk *clk;
	struct xlnx_pwm_chip *pwm;

	pwm = devm_kzalloc(&pdev->dev, sizeof(*pwm), GFP_KERNEL);
	if (!pwm) {
		return -ENOMEM;
	}

	pwm->dev = &pdev->dev;

	clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "could not find clk: %ld\n", PTR_ERR(clk));
		return PTR_ERR(clk);
	}

	/* convert the clock rate into the clock period (Hz to ns) */
	pwm->clk_period = (int)1000000000 / clk_get_rate(clk);

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pwm->mmio_base = devm_ioremap_resource(&pdev->dev, r);
	if (IS_ERR(pwm->mmio_base)) {
		return PTR_ERR(pwm->mmio_base);
	}

	start = r->start;
	end = r->end;

	pwm->chip.dev = &pdev->dev;
	pwm->chip.ops = &xlnx_pwm_ops;
	pwm->chip.base = (int)&pdev->id;
	pwm->chip.npwm = 1;

	ret = pwmchip_add(&pwm->chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "pwmchip_add() failed: %d\n", ret);
		return -1;
	}

	platform_set_drvdata(pdev, pwm);

	return 0;
}

static int xlnx_pwm_remove(struct platform_device *pdev)
{
	struct xlnx_pwm_chip *pc = platform_get_drvdata(pdev);
	if (WARN_ON(!pc))
		return -ENODEV;
	return pwmchip_remove(&pc->chip);
}

static const struct of_device_id xlnx_pwm_of_match[] = {
	{ .compatible = "xlnx,pwm-xlnx", },
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, xlnx_pwm_of_match);

static struct platform_driver xlnx_pwm_driver = {
	.driver = {
		.name = "pwm-xlnx",
		.owner = THIS_MODULE,
		.of_match_table = xlnx_pwm_of_match,
	},
	.probe = xlnx_pwm_probe,
	.remove = xlnx_pwm_remove,
};
module_platform_driver(xlnx_pwm_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Bart Tanghe <bart.tanghe@thomasmore.be>");
MODULE_DESCRIPTION("A Xilinx PWM driver");

