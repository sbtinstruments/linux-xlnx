#include <linux/fs.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "hw.h"

static struct class *lockamp_class;
static const size_t signal_buf_capacity = 4194304; /* 4 MiB */

static int lockamp_probe(struct platform_device *pdev)
{
	struct lockamp *lockamp;
	u32 version;
	int result = 0;

	lockamp = devm_kzalloc(&pdev->dev, sizeof(*lockamp), GFP_KERNEL);
	if (NULL == lockamp) {
		result = -ENOMEM;
		goto out;
	}
	lockamp->dev = &pdev->dev;
	platform_set_drvdata(pdev, lockamp);

	/* Signal buffer mutex */
	mutex_init(&lockamp->signal_buf_m);

	/* ADC buffer mutex */
	mutex_init(&lockamp->adc_buf_m);

	/* Signal buffer */
	lockamp->signal_buf.buf = vmalloc(signal_buf_capacity);
	lockamp->signal_buf.capacity_n = signal_buf_capacity / sizeof(struct sample);
	lockamp->signal_buf.head = 0;
	lockamp->signal_buf.tail = 0;
	if (NULL == lockamp->signal_buf.buf) {
		dev_alert(lockamp->dev, "Failed to allocate signal buffer.\n");
		result = -ENOMEM;
		goto out;
	}
	/* Must be a power of 2 so that the CIRC_* macros work */
	if (!is_power_of_2(lockamp->signal_buf.capacity_n)) {
		dev_alert(lockamp->dev, "Signal buffer capacity must be a power of 2 (tried with %d).\n", lockamp->signal_buf.capacity_n);
		result = -EINVAL;
		goto out_sbuf;
	}

	/* Dev (device number) */
	result = alloc_chrdev_region(&lockamp->chrdev_no, 0, 1, pdev->name);
	if (0 != result) {
		dev_alert(lockamp->dev, "Failed to allocate character device region.\n");
		goto out_sbuf;
	}

	/* Cdev (character device) */
	cdev_init(&lockamp->cdev, &lockamp_fops);
	lockamp->cdev.owner = THIS_MODULE;
	lockamp->cdev.ops = &lockamp_fops;
	result = cdev_add(&lockamp->cdev, lockamp->chrdev_no, 1);
	if (0 != result) {
		dev_alert(lockamp->dev, "Failed to add character device.\n");
		goto out_chrdev;
	}

	/* Device (create /dev/ and /sys/dev/ entries) */
	lockamp->dev = device_create(lockamp_class, &pdev->dev, lockamp->chrdev_no, NULL, pdev->name);
	if (IS_ERR(lockamp->dev)) {
		dev_alert(lockamp->dev, "Failed to create device.\n");
		result = PTR_ERR(lockamp->dev);
		goto out_cdev;
	}
	dev_set_drvdata(lockamp->dev, lockamp);

	/* Initialize lock-in amplifier */
	result = lockamp_init(lockamp, pdev);
	if (0 != result) {
		dev_alert(lockamp->dev, "Failed to initialize lock-in amplifier.\n");
		goto out_device;
	}

	/* Adc buffer */
	lockamp->adc_buffer = devm_kmalloc(lockamp->dev, LOCKAMP_ADC_SAMPLES_SIZE, GFP_KERNEL);
	if (NULL == lockamp->adc_buffer) {
        dev_alert(lockamp->dev, "Failed to allocate adc buffer.\n");
		result = -ENOMEM;
		goto out_lockamp;
	}

	/* Other defaults */
	lockamp->sample_multiplier = 1;
	atomic_set(&lockamp->desyncs, 0);

	/* Set hardware defaults */
	// lockamp_set_filter_coefficients(lockamp, lockamp_fir_coefs[0]);

	/* Welcome message */
	version = 123; //lockamp_version(lockamp);
	dev_info(lockamp->dev, "enabled (hw_version:%x)\n", version);
	goto out;
out_lockamp:
	lockamp_exit(lockamp);
out_device:
	device_destroy(lockamp_class, lockamp->chrdev_no);
out_cdev:
	cdev_del(&lockamp->cdev);
out_chrdev:
	unregister_chrdev_region(lockamp->chrdev_no, 1);
out_sbuf:
	vfree(lockamp->signal_buf.buf);
out:
	return result;
}

static int lockamp_remove(struct platform_device *pdev)
{
	struct lockamp *lockamp = platform_get_drvdata(pdev);
	lockamp_exit(lockamp);
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
	},
	.probe = lockamp_probe,
	.remove = lockamp_remove,
};

static int __init lockamp_module_init(void)
{
	int result = 0;
	/* Class (create /sys/class/ entry) */
	lockamp_class = class_create(THIS_MODULE, LOCKAMP_CLASS_NAME);
	if (IS_ERR(lockamp_class)) {
		pr_alert(LOCKAMP_MSG "Failed to create class.\n");
		result = PTR_ERR(lockamp_class);
		goto out;
	}
	lockamp_class->dev_groups = lockamp_attr_groups;
	/* Register platform driver */
	result = platform_driver_register(&lockamp_driver);
	if (0 != result) {
		pr_alert(LOCKAMP_MSG "Failed to register platform driver.\n");
		goto out_class;
	}
	goto out;
out_class:
	class_destroy(lockamp_class);
out:
	return result;
}
module_init(lockamp_module_init);

static void __exit lockamp_module_exit(void)
{
	platform_driver_unregister(&lockamp_driver);
	class_destroy(lockamp_class);
}
module_exit(lockamp_module_exit);

MODULE_AUTHOR("Frederik Peter Aalund <fpa@sbtinstruments.com>");
MODULE_DESCRIPTION("SBT Instruments lock-in amplifier driver.");
MODULE_VERSION("v2.0.0-beta.3");
MODULE_LICENSE("GPL");
