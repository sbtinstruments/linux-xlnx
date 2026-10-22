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
#include <linux/rtc.h>
#include <linux/timekeeping.h>

#include <linux/mfd/da9063/core.h>
#include <linux/mfd/da9063/registers.h>

#include <linux/proc_fs.h>
#include <linux/kthread.h>
#include <linux/math64.h>
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

/*
 * A warm restart goes through the PMIC's RESET mode, the datasheet's complete
 * shutdown (DA9063 datasheet rev 2.4, section 6.2.3). It switches off every
 * buck and every LDO except LDO1, asserts nRESET first, and reloads the whole
 * OTP configuration on the way back up, as a cold start does. The older path
 * through POWERDOWN mode keeps every supply with Bxxx_CONF set, which on the
 * green mango is every PS supply. That restart is only a reset pulse, and the
 * BootROM sometimes parks after it (REBOOT_STATUS error code 0x2000), leaving
 * the board dark until a power cycle (sbt/drupe#48).
 *
 * Without AUTO_BOOT in OTP, only a wake-up event ends RESET mode, so the
 * restart arms the RTC alarm first. An alarm must match once the PMIC has
 * left RESET mode for POWERDOWN mode: events are cleared when RESET mode
 * starts (p.73), and the alarm wakes the PMIC "if the DA9063 is in POWERDOWN
 * mode" (p.118). An alarm that matches earlier is lost, and the board stays
 * dark. The arming and its checks need I2C transfers that can sleep, so they
 * run in the restart-prepare handler. The restart handler then only writes
 * SHUTDOWN.
 *
 * The alarm has one-second resolution and matches between LEAD_S - 1 and
 * LEAD_S seconds after the RTC is read. RESET mode lasts RESET_DURATION
 * after the walk down (about 26 ms with our OTP sequencer timing). So the
 * restart handler must follow the arming within DEADLINE_NS:
 * 250 ms + 26 ms + 500 ms is still 1.2 s short of the earliest match.
 * The supplies stay off from RESET mode until the alarm, about 2 to 3 s.
 */
#define DA9063_RESTART_ALARM_LEAD_S		3
#define DA9063_RESTART_RESET_DURATION		DA9063_RESET_DURATION_500MS
#define DA9063_RESTART_DEADLINE_NS		(250 * NSEC_PER_MSEC)
/* Long enough for nRESET to stop the CPU after a SHUTDOWN write */
#define DA9063_RESTART_SHUTDOWN_WAIT_MS		500

enum {
	DA9063_RTC_SEC,
	DA9063_RTC_MIN,
	DA9063_RTC_HOUR,
	DA9063_RTC_DAY,
	DA9063_RTC_MONTH,
	DA9063_RTC_YEAR,
	DA9063_RTC_LEN
};

static int da9063_read_rtc(struct da9063 *da9063, time64_t *now)
{
	struct rtc_time tm = {};
	u8 count[DA9063_RTC_LEN];
	int ret;

	/* A read that starts at COUNT_S latches the whole calendar count */
	ret = regmap_bulk_read(da9063->regmap, DA9063_REG_COUNT_S, count,
			       DA9063_RTC_LEN);
	if (ret < 0)
		return ret;

	if (!(count[DA9063_RTC_SEC] & DA9063_RTC_READ))
		return -EAGAIN;

	tm.tm_sec = count[DA9063_RTC_SEC] & DA9063_COUNT_SEC_MASK;
	tm.tm_min = count[DA9063_RTC_MIN] & DA9063_COUNT_MIN_MASK;
	tm.tm_hour = count[DA9063_RTC_HOUR] & DA9063_COUNT_HOUR_MASK;
	tm.tm_mday = count[DA9063_RTC_DAY] & DA9063_COUNT_DAY_MASK;
	tm.tm_mon = (count[DA9063_RTC_MONTH] & DA9063_COUNT_MONTH_MASK) - 1;
	tm.tm_year = (count[DA9063_RTC_YEAR] & DA9063_COUNT_YEAR_MASK) + 100;
	if (rtc_valid_tm(&tm))
		return -EINVAL;

	*now = rtc_tm_to_time64(&tm);
	return 0;
}

static int da9063_arm_restart_alarm(struct da9063 *da9063)
{
	static const u8 mask[DA9063_RTC_LEN] = {
		[DA9063_RTC_SEC] = DA9063_BB_ALARM_S_MASK,
		[DA9063_RTC_MIN] = DA9063_ALARM_MIN_MASK,
		[DA9063_RTC_HOUR] = DA9063_ALARM_HOUR_MASK,
		[DA9063_RTC_DAY] = DA9063_ALARM_DAY_MASK,
		[DA9063_RTC_MONTH] = DA9063_ALARM_MONTH_MASK,
		[DA9063_RTC_YEAR] = DA9063_ALARM_YEAR_MASK,
	};
	u8 alarm[DA9063_RTC_LEN], readback[DA9063_RTC_LEN];
	struct rtc_time tm;
	time64_t now, at;
	int ret, i;

	ret = da9063_read_rtc(da9063, &now);
	if (ret < 0)
		return ret;

	at = now + DA9063_RESTART_ALARM_LEAD_S;
	rtc_time64_to_tm(at, &tm);
	if (tm.tm_year - 100 > DA9063_ALARM_YEAR_MASK)
		return -ERANGE;

	alarm[DA9063_RTC_SEC] = tm.tm_sec;
	alarm[DA9063_RTC_MIN] = tm.tm_min;
	alarm[DA9063_RTC_HOUR] = tm.tm_hour;
	alarm[DA9063_RTC_DAY] = tm.tm_mday;
	alarm[DA9063_RTC_MONTH] = tm.tm_mon + 1;
	alarm[DA9063_RTC_YEAR] = tm.tm_year - 100;

	/* The same steps as the RTC driver's set_alarm, which also clears
	 * TICK_ON and TICK_WAKE.
	 */
	ret = regmap_clear_bits(da9063->regmap, DA9063_BB_REG_ALARM_Y,
				DA9063_ALARM_ON);
	if (ret < 0)
		return ret;
	ret = regmap_bulk_write(da9063->regmap, DA9063_BB_REG_ALARM_S, alarm,
				DA9063_RTC_LEN);
	if (ret < 0)
		return ret;
	ret = regmap_set_bits(da9063->regmap, DA9063_BB_REG_ALARM_Y,
			      DA9063_ALARM_ON);
	if (ret < 0)
		return ret;

	ret = regmap_bulk_read(da9063->regmap, DA9063_BB_REG_ALARM_S, readback,
			       DA9063_RTC_LEN);
	if (ret < 0)
		return ret;
	for (i = 0; i < DA9063_RTC_LEN; i++)
		if ((readback[i] & mask[i]) != alarm[i])
			return -EIO;
	if ((readback[DA9063_RTC_YEAR] & (DA9063_ALARM_ON | DA9063_TICK_ON)) !=
	    DA9063_ALARM_ON)
		return -EIO;

	/* An alarm time that has already passed never matches */
	ret = da9063_read_rtc(da9063, &now);
	if (ret < 0)
		return ret;
	if (at - now < DA9063_RESTART_ALARM_LEAD_S - 1)
		return -ETIME;

	return 0;
}

static int da9063_restart_prepare_handler(struct sys_off_data *data)
{
	struct da9063 *da9063 = data->cb_data;
	unsigned int config_j;
	int ret;

	da9063->restart_alarm_armed = false;

	/* The DA9063L has no RTC. The AD variant's alarm has no seconds. */
	if (da9063->type != PMIC_TYPE_DA9063 ||
	    da9063->variant_code == PMIC_DA9063_AD) {
		dev_info(da9063->dev,
			 "No RTC alarm with seconds, so restarting through POWERDOWN mode\n");
		return NOTIFY_DONE;
	}

	ret = regmap_update_bits(da9063->regmap, DA9063_REG_CONFIG_J,
				 DA9063_RESET_DURATION_MASK,
				 DA9063_RESTART_RESET_DURATION);
	if (ret < 0)
		goto fallback;
	/* Read the register itself back, not the cache */
	ret = regcache_drop_region(da9063->regmap, DA9063_REG_CONFIG_J,
				   DA9063_REG_CONFIG_J);
	if (ret < 0)
		goto fallback;
	ret = regmap_read(da9063->regmap, DA9063_REG_CONFIG_J, &config_j);
	if (ret < 0)
		goto fallback;
	if ((config_j & DA9063_RESET_DURATION_MASK) !=
	    DA9063_RESTART_RESET_DURATION) {
		ret = -EIO;
		goto fallback;
	}

	/* Page 0 registers only, so the regmap leaves PAGE_CON at page 0 for
	 * the raw SMBus writes of the restart handler.
	 */
	ret = da9063_arm_restart_alarm(da9063);
	if (ret < 0)
		goto fallback;

	da9063->restart_alarm_armed_ns = ktime_get_mono_fast_ns();
	da9063->restart_alarm_armed = true;
	dev_info(da9063->dev,
		 "Restarting through RESET mode, woken by the RTC alarm in %d s\n",
		 DA9063_RESTART_ALARM_LEAD_S);
	return NOTIFY_DONE;

fallback:
	dev_warn(da9063->dev,
		 "Failed to arm the RTC alarm (%d), so restarting through POWERDOWN mode\n",
		 ret);
	return NOTIFY_DONE;
}

/* Returns only if the PMIC did not take the board down */
static void da9063_restart_through_reset_mode(struct da9063 *da9063)
{
	struct i2c_client *client = to_i2c_client(da9063->dev);
	u64 since = ktime_get_mono_fast_ns() - da9063->restart_alarm_armed_ns;
	int ret;

	if (since > DA9063_RESTART_DEADLINE_NS) {
		dev_alert(da9063->dev,
			  "%llu ms after arming the RTC alarm, too late for RESET mode\n",
			  div_u64(since, NSEC_PER_MSEC));
		return;
	}

	ret = i2c_smbus_write_byte_data(client, DA9063_REG_PAGE_CON,
					DA9063_REG_PAGE0);
	if (ret >= 0)
		ret = i2c_smbus_write_byte_data(client, DA9063_REG_CONTROL_F,
						DA9063_SHUTDOWN);
	if (ret < 0) {
		dev_alert(da9063->dev, "Failed to set DA9063_SHUTDOWN bit: %d\n",
			  ret);
		return;
	}

	mdelay(DA9063_RESTART_SHUTDOWN_WAIT_MS);
	dev_alert(da9063->dev, "Still running %d ms after DA9063_SHUTDOWN\n",
		  DA9063_RESTART_SHUTDOWN_WAIT_MS);
}

static int da9063_restart_handler(struct sys_off_data *data)
{
	struct da9063 *da9063 = data->cb_data;
	struct i2c_client *client = to_i2c_client(da9063->dev);
	int ret;

	if (da9063->restart_alarm_armed)
		da9063_restart_through_reset_mode(da9063);

	/* The fallback, also for a restart that skips the prepare handler
	 * (emergency_restart): set the "wake up" bit and clear the "system
	 * enable" bit. The DA9063 goes through POWERDOWN mode for a few
	 * milliseconds, and the supplies with Bxxx_CONF set stay on.
	 *
	 * Hold nRESET (the ZYNQ's PS_POR_B) for the longest reset timer after
	 * the wake-up. With the OTP default of 1 ms, the BootROM parks more
	 * often (REBOOT_STATUS error code 0x2000), and the board stays dark
	 * until a power cycle (sbt/drupe#48). A cold start reloads the OTP
	 * default, so this only lengthens a warm restart, by about 1 s.
	 */
	ret = i2c_smbus_read_byte_data(client, DA9063_REG_RESET);
	if (ret >= 0)
		ret = i2c_smbus_write_byte_data(client, DA9063_REG_RESET,
		                                (ret & DA9063_RESET_EVENT_MASK) |
		                                DA9063_RESET_TIMER_1048MS);
	if (ret < 0)
		dev_alert(da9063->dev, "Failed to set the reset timer: %d\n", ret);

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
						    SYS_OFF_MODE_RESTART_PREPARE,
						    SYS_OFF_PRIO_HIGH + 1,
						    &da9063_restart_prepare_handler,
						    da9063);
		if (ret) {
			dev_err(da9063->dev,
				"Failed to register restart prepare handler\n");
			return ret;
		}

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
