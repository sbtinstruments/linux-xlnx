// SPDX-License-Identifier: GPL-2.0+
/*
 * Device access for Dialog DA9063 modules
 *
 * Copyright 2012 Dialog Semiconductors Ltd.
 * Copyright 2013 Philipp Zabel, Pengutronix
 * Copyright 2019 Frederik Peter Aalund, SBT Instruments
 *
 * Author: Krystian Garbaciak, Dialog Semiconductor
 * Author: Michal Hajduk, Dialog Semiconductor
 * Author: Frederik Peter Aalund <fpa@sbtinstruments.com>
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/mfd/core.h>
#include <linux/regmap.h>
#include <linux/reboot.h>

#include <linux/mfd/da9063/core.h>
#include <linux/mfd/da9063/registers.h>

#include <linux/proc_fs.h>
#include <linux/kthread.h>
#include <linux/uaccess.h>
#include <linux/i2c.h>
#include <linux/pm.h>


static const struct resource da9063_regulators_resources[] = {
	{
		.name	= "LDO_LIM",
		.start	= DA9063_IRQ_LDO_LIM,
		.end	= DA9063_IRQ_LDO_LIM,
		.flags	= IORESOURCE_IRQ,
	},
};

static const struct resource da9063_rtc_resources[] = {
	{
		.name	= "ALARM",
		.start	= DA9063_IRQ_ALARM,
		.end	= DA9063_IRQ_ALARM,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.name	= "TICK",
		.start	= DA9063_IRQ_TICK,
		.end	= DA9063_IRQ_TICK,
		.flags	= IORESOURCE_IRQ,
	}
};

static const struct resource da9063_onkey_resources[] = {
	{
		.name	= "ONKEY",
		.start	= DA9063_IRQ_ONKEY,
		.end	= DA9063_IRQ_ONKEY,
		.flags	= IORESOURCE_IRQ,
	},
};

static const struct resource da9063_hwmon_resources[] = {
	{
		.start	= DA9063_IRQ_ADC_RDY,
		.end	= DA9063_IRQ_ADC_RDY,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct resource da9063_gpio_resources[] = {
	{
		.start	= DA9063_IRQ_GPI0,
		.end	= DA9063_IRQ_GPI0,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI1,
		.end	= DA9063_IRQ_GPI1,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI2,
		.end	= DA9063_IRQ_GPI2,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI3,
		.end	= DA9063_IRQ_GPI3,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI4,
		.end	= DA9063_IRQ_GPI4,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI5,
		.end	= DA9063_IRQ_GPI5,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI6,
		.end	= DA9063_IRQ_GPI6,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI7,
		.end	= DA9063_IRQ_GPI7,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI8,
		.end	= DA9063_IRQ_GPI8,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI9,
		.end	= DA9063_IRQ_GPI9,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI10,
		.end	= DA9063_IRQ_GPI10,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI11,
		.end	= DA9063_IRQ_GPI11,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI12,
		.end	= DA9063_IRQ_GPI12,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI13,
		.end	= DA9063_IRQ_GPI13,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI14,
		.end	= DA9063_IRQ_GPI14,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= DA9063_IRQ_GPI15,
		.end	= DA9063_IRQ_GPI15,
		.flags	= IORESOURCE_IRQ,
	},
};


static const struct mfd_cell da9063_common_devs[] = {
	{
		.name		= DA9063_DRVNAME_REGULATORS,
		.num_resources	= ARRAY_SIZE(da9063_regulators_resources),
		.resources	= da9063_regulators_resources,
	},
	{
		.name		= DA9063_DRVNAME_LEDS,
	},
	{
		.name		= DA9063_DRVNAME_WATCHDOG,
		.of_compatible	= "dlg,da9063-watchdog",
	},
	{
		.name		= DA9063_DRVNAME_HWMON,
		.num_resources	= ARRAY_SIZE(da9063_hwmon_resources),
		.resources	= da9063_hwmon_resources,
	},
	{
		.name		= DA9063_DRVNAME_ONKEY,
		.num_resources	= ARRAY_SIZE(da9063_onkey_resources),
		.resources	= da9063_onkey_resources,
		.of_compatible = "dlg,da9063-onkey",
	},
	{
		.name		= DA9063_DRVNAME_VIBRATION,
	},
	{
		.name		= DA9063_DRVNAME_GPIO,
		.num_resources	= ARRAY_SIZE(da9063_gpio_resources),
		.resources	= da9063_gpio_resources,
		.of_compatible  = "dlg,da9063-gpio",
	},
};

/* Only present on DA9063 , not on DA9063L */
static const struct mfd_cell da9063_devs[] = {
	{
		.name		= DA9063_DRVNAME_RTC,
		.num_resources	= ARRAY_SIZE(da9063_rtc_resources),
		.resources	= da9063_rtc_resources,
		.of_compatible	= "dlg,da9063-rtc",
	},
};

static int da9063_power_off_handler(struct sys_off_data *data)
{
	struct da9063 *da9063 = data->cb_data;
	struct i2c_client *client = to_i2c_client(da9063->dev);
	int ret;

	dev_dbg(da9063->dev, "Setting the DA9063_SHUTDOWN bit to "
	                     "power off the system\n");
	ret = i2c_smbus_write_byte_data(client, DA9063_REG_CONTROL_F,
	                                DA9063_SHUTDOWN);
	if (ret < 0)
		dev_alert(da9063->dev, "Failed to power off: %d\n", ret);

	return NOTIFY_DONE;
}

static int da9063_restart_handler(struct sys_off_data *data)
{
	struct da9063 *da9063 = data->cb_data;
	struct i2c_client *client = to_i2c_client(da9063->dev);
	int ret;

	/* This function restarts the system by setting the "wake up" bit and
	 * unsetting the "system enable" bit. In practice, this brings the DA906X
	 * chip into "POWER-DOWN mode" for a brief period.
	 *
	 * It is possible to go a step deeper into "Delivery (and RTC) mode" but
	 * this requires that we:
	 *
	 *  1. Set an RTC alarm for, say, 1 second into the future.
	 *  2. Power off the system via the DA9063_SHUTDOWN bit.
	 *
	 * Step 2 is easy (see the "da9063_power_off_handler" function).
	 * Step 1, however, is a bit more tricky.
	 *
	 * For now, we just use "POWER-DOWN mode" until there is a use case
	 * for a "deeper" (more low-level) reset.
	 */

	dev_dbg(da9063->dev, "Setting the DA9063_WAKE_UP bit to "
	                     "wake the system again once it is powered down\n");
	ret = i2c_smbus_write_byte_data(client, DA9063_REG_CONTROL_F,
	                                DA9063_WAKE_UP);
	if (ret < 0) {
		dev_alert(da9063->dev, "Failed to set DA9063_WAKE_UP bit: %d\n", ret);
		return NOTIFY_DONE;
	}

	dev_dbg(da9063->dev, "Clearing the DA9063_SYSTEM_EN bit to "
	                     "power down the system\n");
	/* Note that we mask out the bits that we do not want to clear using
	 * the "M_"-prefixed mask bits.
	 */
	ret = i2c_smbus_write_byte_data(client, DA9063_REG_CONTROL_A,
	                                DA9063_M_POWER_EN | DA9063_M_POWER1_EN);
	if (ret < 0) {
		dev_alert(da9063->dev,
		          "Failed to clear the DA9063_SYSTEM_EN bit: %d\n",
				  ret);
		return NOTIFY_DONE;
	}

	return NOTIFY_DONE;
}

static int da9063_clear_fault_log(struct da9063 *da9063)
{
	int ret = 0;
	int fault_log = 0;

	ret = regmap_read(da9063->regmap, DA9063_REG_FAULT_LOG, &fault_log);
	if (ret < 0) {
		dev_err(da9063->dev, "Cannot read FAULT_LOG.\n");
		return -EIO;
	}

	if (fault_log) {
		if (fault_log & DA9063_TWD_ERROR)
			dev_dbg(da9063->dev,
				"Fault log entry detected: DA9063_TWD_ERROR\n");
		if (fault_log & DA9063_POR)
			dev_dbg(da9063->dev,
				"Fault log entry detected: DA9063_POR\n");
		if (fault_log & DA9063_VDD_FAULT)
			dev_dbg(da9063->dev,
				"Fault log entry detected: DA9063_VDD_FAULT\n");
		if (fault_log & DA9063_VDD_START)
			dev_dbg(da9063->dev,
				"Fault log entry detected: DA9063_VDD_START\n");
		if (fault_log & DA9063_TEMP_CRIT)
			dev_dbg(da9063->dev,
				"Fault log entry detected: DA9063_TEMP_CRIT\n");
		if (fault_log & DA9063_KEY_RESET)
			dev_dbg(da9063->dev,
				"Fault log entry detected: DA9063_KEY_RESET\n");
		if (fault_log & DA9063_NSHUTDOWN)
			dev_dbg(da9063->dev,
				"Fault log entry detected: DA9063_NSHUTDOWN\n");
		if (fault_log & DA9063_WAIT_SHUT)
			dev_dbg(da9063->dev,
				"Fault log entry detected: DA9063_WAIT_SHUT\n");
	}

	ret = regmap_write(da9063->regmap,
			   DA9063_REG_FAULT_LOG,
			   fault_log);
	if (ret < 0)
		dev_err(da9063->dev,
			"Cannot reset FAULT_LOG values %d\n", ret);

	return ret;
}

int da9063_device_init(struct da9063 *da9063, unsigned int irq)
{
	int ret;

	ret = da9063_clear_fault_log(da9063);
	if (ret < 0)
		dev_err(da9063->dev, "Cannot clear fault log\n");

	da9063->flags = 0;
	da9063->irq_base = -1;
	da9063->chip_irq = irq;
	enable_irq_wake(da9063->chip_irq);

	ret = da9063_irq_init(da9063);
	if (ret) {
		dev_err(da9063->dev, "Cannot initialize interrupts.\n");
		return ret;
	}

	da9063->irq_base = regmap_irq_chip_get_base(da9063->regmap_irq);

	ret = devm_mfd_add_devices(da9063->dev, PLATFORM_DEVID_NONE,
				   da9063_common_devs,
				   ARRAY_SIZE(da9063_common_devs),
				   NULL, da9063->irq_base, NULL);
	if (ret) {
		dev_err(da9063->dev, "Failed to add child devices\n");
		return ret;
	}

	if (da9063->type == PMIC_TYPE_DA9063) {
		ret = devm_mfd_add_devices(da9063->dev, PLATFORM_DEVID_NONE,
					   da9063_devs, ARRAY_SIZE(da9063_devs),
					   NULL, da9063->irq_base, NULL);
		if (ret) {
			dev_err(da9063->dev, "Failed to add child devices\n");
			return ret;
		}
	}

	if (of_device_is_system_power_controller(da9063->dev->of_node)) {
		if (pm_power_off) {
			dev_warn(da9063->dev, "The global power off function (pm_power_off) is "
			                      "already set. We'll unset it and use the new "
								  "sys-off handler API (e.g., "
								  "register_restart_handler).\n");
			pm_power_off = NULL;
		}

		ret = devm_register_power_off_handler(da9063->dev,
		                                      &da9063_power_off_handler,
											  da9063);
		if (ret) {
			dev_err(da9063->dev, "Failed to register power off handler\n");
			return ret;
		}

		/* We know that, e.g., the ZYNQ SLCR-based restart handler has priority
		 * SYS_OFF_PRIO_HIGH (192). We want the PMIC (da9063) to have higher
		 * priority than this because the PMIC provides a sys-off mechanism that
		 * is closer to the hardware. Therefore, we use priority
		 * SYS_OFF_PRIO_HIGH + 1 = 193.
		 *
		 * Note that the "da9063_wdt" device (the watchdog device) also registers
		 * a restart handler (function "da9063_wdt_restart") with priority 128.
		 * Said handler, however, does not actually do a proper system restart.
		 * In fact, it merely does a power off (setting the DA9063_SHUTDOWN bit).
		 */
		ret = devm_register_sys_off_handler(da9063->dev,
		                                    SYS_OFF_MODE_RESTART,
		                                    SYS_OFF_PRIO_HIGH + 1,
		                                    &da9063_restart_handler,
											da9063);
		if (ret) {
			dev_err(da9063->dev, "Failed to register restart handler\n");
			return ret;
		}
	}

	return ret;
}

MODULE_DESCRIPTION("PMIC driver for Dialog DA9063");
MODULE_AUTHOR("Krystian Garbaciak");
MODULE_AUTHOR("Michal Hajduk");
MODULE_AUTHOR("Frederik Peter Aalund");
MODULE_LICENSE("GPL");
