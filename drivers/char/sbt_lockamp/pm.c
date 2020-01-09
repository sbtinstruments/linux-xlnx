/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SBT Instruments Lock-in Amplifier
 *
 * Copyright (c) 2019, Frederik Peter Aalund <fpa@sbtinstruments.com>
 */
#include <linux/delay.h>
#include <linux/iio/consumer.h>

#include "hw.h"

static int lockamp_pm_suspend(struct device *dev)
{
	struct lockamp *lockamp = dev_get_drvdata(dev);
	/* The "fifo_to_sbuf" kthread will wait for the mutex indefinitely.
	 * This way, the hardware is not used during the suspend.
	 */
	mutex_lock(&lockamp->signal_buf_m);
	dev_dbg(dev, "Success\n");
	return 0;
}

static int lockamp_pm_resume(struct device *dev)
{
	struct lockamp *lockamp = dev_get_drvdata(dev);
	/* Wait for a short while for the HW to power up. We determine the
	 * 'short while' as follows:
	 *   1) The kernel crashes when we use 1 ms
	 *   2) Seemingly works (couldn't get it not to work) when we use 5 ms
	 *   3) We choose 10 ms to be on the safe side
	 */
	msleep(10);
	mutex_unlock(&lockamp->signal_buf_m);
	dev_dbg(dev, "Success\n");
	return 0;
}

static int lockamp_powerdown_iio_chan(struct iio_channel *chan, bool powerdown)
{
	const char *powerdown_str = (powerdown) ? "y" : "n";
	ssize_t size = iio_write_channel_ext_info(chan, "powerdown",
	                                          powerdown_str, 2);
	if (size != 2) {
		return -EINVAL;
	}
	return 0;
}

static int lockamp_enable_converters(struct device *dev, bool enabled)
{
	struct lockamp *lockamp = dev_get_drvdata(dev);
	int ret;
	ret = lockamp_powerdown_iio_chan(lockamp->adc_site0, !enabled);
	if (ret < 0) {
		dev_err(dev, "Failed to power down the ADC\n");
		return ret;
	}
	ret = lockamp_powerdown_iio_chan(lockamp->dac_site0, !enabled);
	if (ret < 0) {
		dev_err(dev, "Failed to power down the DAC\n");
		return ret;
	}
	dev_dbg(dev, "Success\n");
	return 0;
}

static int lockamp_pm_runtime_suspend(struct device *dev)
{
	struct lockamp *lockamp = dev_get_drvdata(dev);
	int ret;
	ret = lockamp_enable_converters(dev, false);
	if (ret < 0) {
		dev_err(dev, "Failed to disable the AD/DA converters: %d\n", ret);
		return ret;
	}
	if (!lockamp->amp_supply_force_off) {
		ret = regulator_disable(lockamp->amp_supply);
		if (ret < 0) {
			dev_err(dev, "Failed to disable the regulator for the amplifiers: %d\n", ret);
			return ret;
		}
	}
	dev_dbg(dev, "Success\n");
	return 0;
}

static int lockamp_pm_runtime_resume(struct device *dev)
{
	struct lockamp *lockamp = dev_get_drvdata(dev);
	int ret = lockamp_enable_converters(dev, true);
	if (ret < 0) {
		dev_err(dev, "Failed to enable the AD/DA converters: %d\n", ret);
		return ret;
	}
	if (!lockamp->amp_supply_force_off) {
		ret = regulator_enable(lockamp->amp_supply);
		if (ret < 0) {
			dev_err(dev, "Failed to enable the regulator for the amplifiers: %d\n", ret);
			return ret;
		}
	}
	/* During a power off, the lock-in amplifier will not get clock input from
	 * the ADC. In turn, this corrupts the FPGA state (e.g., the MA filter's
	 * sum register). We reset the FPGA to get everything in working order
	 * again. */
	lockamp_reset(lockamp);
	/* Now that everything is reset, we need to restore the registers to the
	 * state that they were in before the power off. We do that using the
	 * regmap cache.
	 * One caveat: During the initial probe, the hardware must be turned on
	 * (indirectly calling this function) before the regmap is initialized.
	 * In order to prevent uninitalized access, we skip the state reset if
	 * the regmap is not defined yet. Again, this should only occur during
	 * the inital probe. */
	if (lockamp->regmap) {
		regcache_mark_dirty(lockamp->regmap);
		ret = regcache_sync(lockamp->regmap);
		if (ret < 0) {
			dev_err(dev, "Failed to sync regmap cache on resume: %d\n", ret);
			return ret;
		}
	}
	dev_dbg(dev, "Success\n");
	return 0;
}

struct dev_pm_ops lockamp_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(lockamp_pm_suspend,
	                        lockamp_pm_resume)
	SET_RUNTIME_PM_OPS(lockamp_pm_runtime_suspend,
	                   lockamp_pm_runtime_resume,
	                   NULL)
};

