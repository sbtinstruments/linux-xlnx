/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SBT Instruments Lock-in Amplifier
 *
 * Copyright (c) 2019, Frederik Peter Aalund <fpa@sbtinstruments.com>
 */
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/consumer.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "hw.h"

static struct class *lockamp_class;

static int lockamp_get_iio_chans(struct lockamp *lockamp,
                                 struct platform_device *pdev)
{
	/* adc */
	lockamp->adc_site0 = devm_iio_channel_get(&pdev->dev, "adc-site0");
	if (IS_ERR(lockamp->adc_site0)) {
		if (-EPROBE_DEFER == PTR_ERR(lockamp->adc_site0)) {
			return -EPROBE_DEFER;
		}
		dev_err(&pdev->dev, "Failed to get ADC for site0: %ld\n",
		        PTR_ERR(lockamp->adc_site0));
		return PTR_ERR(lockamp->adc_site0);
	}
	lockamp->adc_site1 = devm_iio_channel_get(&pdev->dev, "adc-site1");
	if (IS_ERR(lockamp->adc_site1)) {
		if (-EPROBE_DEFER == PTR_ERR(lockamp->adc_site1)) {
			return -EPROBE_DEFER;
		}
		dev_err(&pdev->dev, "Failed to get ADC for site1: %ld\n",
		        PTR_ERR(lockamp->adc_site1));
		return PTR_ERR(lockamp->adc_site1);
	}
	/* dac */
	lockamp->dac_site0 = devm_iio_channel_get(&pdev->dev, "dac-site0");
	if (IS_ERR(lockamp->dac_site0)) {
		if (-EPROBE_DEFER == PTR_ERR(lockamp->dac_site0)) {
			return -EPROBE_DEFER;
		}
		dev_err(&pdev->dev, "Failed to get DAC for site0: %ld\n",
		        PTR_ERR(lockamp->dac_site0));
		return PTR_ERR(lockamp->dac_site0);
	}
	lockamp->dac_site1 = devm_iio_channel_get(&pdev->dev, "dac-site1");
	if (IS_ERR(lockamp->dac_site1)) {
		if (-EPROBE_DEFER == PTR_ERR(lockamp->dac_site1)) {
			return -EPROBE_DEFER;
		}
		dev_err(&pdev->dev, "Failed to get DAC for site1: %ld\n",
		        PTR_ERR(lockamp->dac_site1));
		return PTR_ERR(lockamp->dac_site1);
	}
	return 0;
}

static int lockamp_get_iomem(struct lockamp *lockamp,
                             struct platform_device *pdev)
{
	struct resource *resource;
	/* register-based control interface */
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "lockamp-control");
	if (NULL == resource) {
		dev_err(&pdev->dev, "Unable to get resource 'lockamp-control' from the platform device.\n");
		return -ENOMEM;
	}
	lockamp->control = devm_ioremap_resource(lockamp->dev, resource);
	if (IS_ERR(lockamp->control)) {
		dev_err(&pdev->dev, "Unable to ioremap resource 'lockamp-control'.\n");
		return PTR_ERR(lockamp->control);
	}
	return 0;
}

static int lockamp_get_io_resources(struct lockamp *lockamp,
                                    struct platform_device *pdev)
{
	int ret;
	ret = lockamp_get_iio_chans(lockamp, pdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to get IIO channels: %d\n", ret);
		return ret;
	}
	ret = lockamp_get_iomem(lockamp, pdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to get IO memory: %d\n", ret);
		return ret;
	}
	return 0;
}

static bool lockamp_reg_gap(struct device *dev, unsigned int reg) {
	if (LOCKAMP_REG_GEN2_LOCK_PHASE < reg && reg < LOCKAMP_REG_DEBUG0) {
		return true;
	}
	if (LOCKAMP_REG_DEBUG_CONTROL < reg && reg < LOCKAMP_REG_FIR_COEF_BASE) {
		return true;
	}
	return false;
}

static bool lockamp_readable_reg(struct device *dev, unsigned int reg)
{
	return !lockamp_reg_gap(dev, reg);
}

static bool lockamp_writeable_reg(struct device *dev, unsigned int reg)
{
	if (lockamp_reg_gap(dev, reg)) {
		return false;
	}
	switch (reg) {
	case LOCKAMP_REG_VERSION:
	case LOCKAMP_REG_DEBUG0:
		return false;
	}
	return true;
}

static bool lockamp_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case LOCKAMP_REG_FIFO_SIZE:
	case LOCKAMP_REG_FIFO_DATA:
	case LOCKAMP_REG_ADC_BUFFER:
		return true;
	}
	return false;
}

static struct regmap_config lockamp_regmap_config = {
	.reg_bits             = 32,
	.val_bits             = 32,
	.reg_stride           = 4,
	.max_register         = 0x1000,
	/* num_reg_defaults_raw ensures that the cache is initialized from
	 * HW reads. It should be equal to the register count. I.e.,
	 * max_register / reg_stride. */
	.num_reg_defaults_raw = 1024,
	.cache_type           = REGCACHE_FLAT,
	.readable_reg         = lockamp_readable_reg,
	.writeable_reg        = lockamp_writeable_reg,
	.volatile_reg         = lockamp_volatile_reg,
};

static int lockamp_probe(struct platform_device *pdev)
{
	struct lockamp *lockamp;
	u32 version;
	int i;
	int ret = 0;

	lockamp = devm_kzalloc(&pdev->dev, sizeof(*lockamp), GFP_KERNEL);
	if (NULL == lockamp) {
		return -ENOMEM;
	}
	lockamp->dev = &pdev->dev;
	platform_set_drvdata(pdev, lockamp);

	/* Reset GPIO */
	lockamp->reset = devm_gpiod_get(&pdev->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(lockamp->reset)) {
		dev_err(&pdev->dev, "Failed to get reset GPIO: %ld.\n",
		        PTR_ERR(lockamp->reset));
		return PTR_ERR(lockamp->reset);
	}

	/* Reset */
	lockamp_reset(lockamp);

	/* Init mutexes */
	mutex_init(&lockamp->signal_buf_m);
	mutex_init(&lockamp->adc_buf_m);

	/* Signal buffer */
#ifdef CONFIG_SBT_LOCKAMP_USE_SBUF
	lockamp->signal_buf.buf = vmalloc(LOCKAMP_SIGNAL_BUF_CAPACITY);
	lockamp->signal_buf.capacity_n = LOCKAMP_SIGNAL_BUF_CAPACITY / sizeof(struct sample);
	lockamp->signal_buf.head = 0;
	lockamp->signal_buf.tail = 0;
	if (NULL == lockamp->signal_buf.buf) {
		dev_err(lockamp->dev, "Failed to allocate signal buffer.\n");
		return -ENOMEM;
	}
	/* Must be a power of 2 so that the CIRC_* macros work */
	if (!is_power_of_2(lockamp->signal_buf.capacity_n)) {
		dev_err(lockamp->dev, "Signal buffer capacity must be a power of 2 (tried with %d).\n",
		        lockamp->signal_buf.capacity_n);
		ret = -EINVAL;
		goto out_sbuf;
	}
#else
	lockamp->signal_buf.buf = NULL;
	lockamp->signal_buf.capacity_n = 0;
	lockamp->signal_buf.head = 0;
	lockamp->signal_buf.tail = 0;
#endif

	/* Dev (device number) */
	ret = alloc_chrdev_region(&lockamp->chrdev_no, 0, 1, pdev->name);
	if (ret < 0) {
		dev_err(lockamp->dev, "Failed to allocate character device region.\n");
		goto out_sbuf;
	}

	/* Cdev (character device) */
	cdev_init(&lockamp->cdev, &lockamp_fops);
	lockamp->cdev.owner = THIS_MODULE;
	lockamp->cdev.ops = &lockamp_fops;
	ret = cdev_add(&lockamp->cdev, lockamp->chrdev_no, 1);
	if (ret < 0) {
		dev_err(lockamp->dev, "Failed to add character device.\n");
		goto out_chrdev;
	}

	/* Device (create /dev/ and /sys/dev/ entries) */
	lockamp->dev = device_create(lockamp_class, &pdev->dev,
	                             lockamp->chrdev_no, NULL, pdev->name);
	if (IS_ERR(lockamp->dev)) {
		dev_err(lockamp->dev, "Failed to create device.\n");
		ret = PTR_ERR(lockamp->dev);
		goto out_cdev;
	}
	dev_set_drvdata(lockamp->dev, lockamp);

	/* I/O resources */
	ret = lockamp_get_io_resources(lockamp, pdev);
	if (ret < 0) {
		dev_err(lockamp->dev, "Failed to initialize lock-in amplifier.\n");
		goto out_device;
	}

	/* Vs regulator for the injection amp */
	lockamp->amp_supply = devm_regulator_get(&pdev->dev, "amp");
	if (IS_ERR(lockamp->amp_supply)) {
		dev_err(lockamp->dev, "Failed to get 'amp' regulator.\n");
		ret = PTR_ERR(lockamp->amp_supply);
		goto out_device;
	}
	lockamp->amp_supply_force_off = true;

	/* Power */
	/* Set regmap to null to avoid regmap access in the pm_runtime_resume
	 * callback. The regmap will be allocated soon after the power is
	 * turned on. */
	lockamp->regmap = NULL;
	pm_runtime_set_autosuspend_delay(&pdev->dev, 3000);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_get_sync(&pdev->dev);
	if (ret < 0) {
		dev_err(lockamp->dev, "Failed to get pm runtime\n");
		goto out_pm_enable;
	}
	ret = 0;

	/* Adc buffer */
	lockamp->adc_buffer = devm_kmalloc(lockamp->dev, LOCKAMP_ADC_SAMPLES_SIZE, GFP_KERNEL);
	if (NULL == lockamp->adc_buffer) {
        dev_err(lockamp->dev, "Failed to allocate adc buffer.\n");
		ret = -ENOMEM;
		goto out_pm_get;
	}

	/* Regmap */
	lockamp->regmap = devm_regmap_init_mmio(lockamp->dev, lockamp->control,
	                                        &lockamp_regmap_config);
	if (IS_ERR(lockamp->regmap)) {
		dev_err(lockamp->dev, "Failed to initialize regmap: %ld\n",
		        PTR_ERR(lockamp->regmap));
		ret = PTR_ERR(lockamp->regmap);
		goto out_pm_get;
	}

	/* Other defaults */
	for (i = 0; LOCKAMP_SITES_PER_SAMPLE > i; ++i) {
		lockamp->sample_multipliers[i] = 1;
	}
	atomic_set(&lockamp->desyncs, 0);

	/* Set hardware defaults */
	ret = lockamp_set_fir_coefs(lockamp, lockamp_fir_coefs[0]);
	if (ret < 0) {
		dev_err(lockamp->dev, "Failed to set FIR filter coefficients: %d\n", ret);
		goto out_pm_get;
	}

	/* Welcome message */
	ret = lockamp_version(lockamp, &version);
	if (ret < 0) {
		dev_err(lockamp->dev, "Failed to get hardware version: %d\n", ret);
		goto out_pm_get;
	}
	dev_info(lockamp->dev, "Probe success (hw_version:%x)\n", version);

	pm_runtime_put(&pdev->dev); /* ignore return value */

	return ret;

out_pm_get:
	pm_runtime_put(&pdev->dev); /* ignore return value */
out_pm_enable:
	pm_runtime_disable(&pdev->dev);
out_device:
	device_destroy(lockamp_class, lockamp->chrdev_no);
out_cdev:
	cdev_del(&lockamp->cdev);
out_chrdev:
	unregister_chrdev_region(lockamp->chrdev_no, 1);
out_sbuf:
	vfree(lockamp->signal_buf.buf);
	return ret;
}

static int lockamp_remove(struct platform_device *pdev)
{
	struct lockamp *lockamp = platform_get_drvdata(pdev);
	pm_runtime_disable(&pdev->dev);
	device_destroy(lockamp_class, lockamp->chrdev_no);
	cdev_del(&lockamp->cdev);
	unregister_chrdev_region(lockamp->chrdev_no, 1);
	vfree(lockamp->signal_buf.buf);
	return 0;
}

static const struct of_device_id lockamp_of_match_table[] = {
	{ .compatible = "sbt,lockamp" },
	{},
};
MODULE_DEVICE_TABLE(of, lockamp_of_match_table);

static struct platform_driver lockamp_driver = {
	.driver = {
		.name = "sbt-lockamp",
		.of_match_table = of_match_ptr(lockamp_of_match_table),
		.pm = &lockamp_pm_ops,
	},
	.probe = lockamp_probe,
	.remove = lockamp_remove,
};

static int __init lockamp_module_init(void)
{
	int ret = 0;
	/* Class (create /sys/class/ entry) */
	lockamp_class = class_create(THIS_MODULE, LOCKAMP_CLASS_NAME);
	if (IS_ERR(lockamp_class)) {
		pr_err(LOCKAMP_CLASS_NAME ": Failed to create class.\n");
		ret = PTR_ERR(lockamp_class);
		goto out;
	}
	lockamp_class->dev_groups = lockamp_attr_groups;
	/* Register platform driver */
	ret = platform_driver_register(&lockamp_driver);
	if (ret < 0) {
		pr_err(LOCKAMP_CLASS_NAME ": Failed to register platform driver.\n");
		goto out_class;
	}
	goto out;
out_class:
	class_destroy(lockamp_class);
out:
	return ret;
}
module_init(lockamp_module_init);

static void __exit lockamp_module_exit(void)
{
	platform_driver_unregister(&lockamp_driver);
	class_destroy(lockamp_class);
}
module_exit(lockamp_module_exit);

MODULE_AUTHOR("Frederik Peter Aalund <fpa@sbtinstruments.com>");
MODULE_DESCRIPTION("SBT Instruments lock-in amplifier driver");
MODULE_LICENSE("GPL");
