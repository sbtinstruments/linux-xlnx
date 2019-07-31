/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2019 Frederik Peter Aalund <fpa@sbtinstruments.com>
 */
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

struct sit9121 {
	struct clk_hw hw;
	struct regulator *vdd;
	unsigned long fixed_rate;
	unsigned long fixed_accuracy;
};

#define to_sit9121(_hw) container_of(_hw, struct sit9121, hw)

static int sit9121_prepare(struct clk_hw *hw)
{
	int error = regulator_enable(to_sit9121(hw)->vdd);
	if (error) {
		return error;
	}
	pr_debug("sit9121: Prepare\n");
	return 0;
}

static void sit9121_unprepare(struct clk_hw *hw)
{
	regulator_disable(to_sit9121(hw)->vdd); /* Ignore return value */
	pr_debug("sit9121: Unprepare\n");
}

static unsigned long sit9121_recalc_rate(struct clk_hw *hw,
                                         unsigned long parent_rate)
{
	return to_sit9121(hw)->fixed_rate;
}

static unsigned long sit9121_recalc_accuracy(struct clk_hw *hw,
                                             unsigned long parent_accuracy)
{
	return to_sit9121(hw)->fixed_accuracy;
}

const struct clk_ops sit9121_ops = {
	.prepare = sit9121_prepare,
	.unprepare = sit9121_unprepare,
	.recalc_rate = sit9121_recalc_rate,
	.recalc_accuracy = sit9121_recalc_accuracy,
};

static int sit9121_probe(struct platform_device *pdev)
{
	struct sit9121 *sit9121;
	struct device_node *node = pdev->dev.of_node;
	const char *clk_name = node->name;
	u32 rate;
	u32 accuracy = 0;
	struct clk_init_data init;
	int error;

	sit9121 = devm_kzalloc(&pdev->dev, sizeof(*sit9121), GFP_KERNEL);
	if (!sit9121) {
		dev_err(&pdev->dev, "Failed to allocate sit9121 struct.\n");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, sit9121);

	/* of properties */
	if (of_property_read_u32(node, "clock-frequency", &rate)) {
		return -EIO;
	}
	of_property_read_u32(node, "clock-accuracy", &accuracy);
	of_property_read_string(node, "clock-output-names", &clk_name);

	/* vdd */
	sit9121->vdd = devm_regulator_get(&pdev->dev, "vdd");
	if (IS_ERR(sit9121->vdd)) {
		dev_err(&pdev->dev, "Failed to get VDD regulator: %ld\n",
		        PTR_ERR(sit9121->vdd));
		return PTR_ERR(sit9121->vdd);
	}

	/* clock */
	init.name = clk_name;
	init.ops = &sit9121_ops;
	init.flags = CLK_IS_BASIC;
	init.parent_names = NULL;
	init.num_parents = 0;
	sit9121->fixed_rate = rate;
	sit9121->fixed_accuracy = accuracy;
	sit9121->hw.init = &init;
	error = devm_clk_hw_register(&pdev->dev, &sit9121->hw);
	if (error) {
		return error;
	}
	error = devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_simple_get,
	                                    &sit9121->hw);
	if (error) {
		return error;
	}

	return 0;
}

static const struct of_device_id sit9121_ids[] = {
	{ .compatible = "sitime,sit9121" },
	{ }
};

static struct platform_driver sit9121_driver = {
	.driver = {
		.name = "sit9121",
		.of_match_table = sit9121_ids,
	},
	.probe = sit9121_probe,
};
builtin_platform_driver(sit9121_driver);

MODULE_DEVICE_TABLE(of, sit9121_ids);
MODULE_AUTHOR("Frederik Peter Aalund <fpa@sbtinstruments.com>");
MODULE_DESCRIPTION("SiTime SiT9121 clock driver.");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:sit9121");
