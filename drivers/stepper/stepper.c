/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/stepper.h>

struct stepper_device {
	struct device dev;
	struct delayed_work velocity_dwork;
	struct mutex velocity_mutex;
	int velocity_current;
	int velocity_target;
	bool velocity_shifting;
	struct stepper_ops ops;
	struct stepper_vel_cfg cfg;
};

#define to_stepper_device(d) container_of(d, struct stepper_device, dev)

static struct workqueue_struct *stepper_workqueue;

static void stepper_reach_target_velocity(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct stepper_device *stepdev = container_of(dwork,
	                                     struct stepper_device, velocity_dwork);
	struct stepper_ops *ops = &stepdev->ops;
	struct stepper_vel_cfg *cfg = &stepdev->cfg;
	int delta;
	mutex_lock(&stepdev->velocity_mutex);
	delta = stepdev->velocity_target - stepdev->velocity_current;
	delta = clamp(delta, -cfg->rate_of_change, cfg->rate_of_change);
	stepdev->velocity_current += delta;
	ops->set_velocity(&stepdev->dev, stepdev->velocity_current);
	if (stepdev->velocity_current == stepdev->velocity_target) {
		stepdev->velocity_shifting = false;
		goto out_mutex;
	}
	queue_delayed_work(stepper_workqueue, dwork,
	                   msecs_to_jiffies(cfg->shift_delay_ms));
out_mutex:
	mutex_unlock(&stepdev->velocity_mutex);
}

static int stepper_validate_velocity(struct stepper_device *stepdev, int vel)
{
	int min_vel = stepdev->cfg.min;
	int max_vel = stepdev->cfg.max;
	return (min_vel <= vel && vel <= max_vel) ? 0 : -EINVAL;
}

static int stepper_set_target_velocity(struct stepper_device *stepdev, int vel)
{
	int result = 0;
	result = stepper_validate_velocity(stepdev, vel);
	if (0 != result) {
		goto out;
	}
	mutex_lock(&stepdev->velocity_mutex);
	stepdev->velocity_target = vel;
	if (stepdev->velocity_current == stepdev->velocity_target) {
		goto out_mutex;
	}
	if (stepdev->velocity_shifting) {
		goto out_mutex;
	}
	stepdev->velocity_shifting = true;
	queue_delayed_work(stepper_workqueue, &stepdev->velocity_dwork,
	                   msecs_to_jiffies(stepdev->cfg.shift_delay_ms));
out_mutex:
	mutex_unlock(&stepdev->velocity_mutex);
out:
	return result;
}

/* velocity_current */
static ssize_t velocity_current_show(
	struct device *dev,
	struct device_attribute *attr,
	char *buf)
{
	int result;
	struct stepper_device *stepdev = to_stepper_device(dev);
	mutex_lock(&stepdev->velocity_mutex);
	result = scnprintf(buf, PAGE_SIZE, "%d\n", stepdev->velocity_current);
	mutex_unlock(&stepdev->velocity_mutex);
	return result;
}
DEVICE_ATTR(velocity_current, S_IRUGO, velocity_current_show, NULL);

/* velocity_target */
static ssize_t velocity_target_show(
	struct device *dev,
	struct device_attribute *attr,
	char *buf)
{
	int result;
	struct stepper_device *stepdev = to_stepper_device(dev);
	mutex_lock(&stepdev->velocity_mutex);
	result = scnprintf(buf, PAGE_SIZE, "%d\n", stepdev->velocity_target);
	mutex_unlock(&stepdev->velocity_mutex);
	return result;
}
static ssize_t velocity_target_store(
	struct device *dev,
	struct device_attribute *attr,
	const char *buf,
	size_t count)
{
	int value;
	int result = kstrtoint(buf, 0, &value);
	struct stepper_device *stepdev = to_stepper_device(dev);
	if (0 != result)
		return result;
	result = stepper_set_target_velocity(stepdev, value);
	if (0 != result)
		return result;
	return count;
}
DEVICE_ATTR(velocity_target, S_IRUGO | S_IWUSR, velocity_target_show, velocity_target_store);

/* velocity_min */
static ssize_t velocity_min_show(
	struct device *dev,
	struct device_attribute *attr,
	char *buf)
{
	struct stepper_device *stepdev = to_stepper_device(dev);
	return scnprintf(buf, PAGE_SIZE, "%d\n", stepdev->cfg.min);
}
DEVICE_ATTR(velocity_min, S_IRUGO, velocity_min_show, NULL);

/* velocity_max */
static ssize_t velocity_max_show(
	struct device *dev,
	struct device_attribute *attr,
	char *buf)
{
	struct stepper_device *stepdev = to_stepper_device(dev);
	return scnprintf(buf, PAGE_SIZE, "%d\n", stepdev->cfg.max);
}
DEVICE_ATTR(velocity_max, S_IRUGO, velocity_max_show, NULL);

/* attribute group */
static struct attribute *attrs[] = {
	&dev_attr_velocity_current.attr,
	&dev_attr_velocity_target.attr,
	&dev_attr_velocity_min.attr,
	&dev_attr_velocity_max.attr,
	NULL,
};
static struct attribute_group attr_group = {
	.attrs = attrs
};
static const struct attribute_group *stepper_dev_attr_groups[] = {
	&attr_group,
	NULL
};

static void stepper_dev_release(struct device *dev)
{
	kfree(to_stepper_device(dev));
}

static struct class stepper_class = {
	.name = "stepper",
	.owner = THIS_MODULE,
	.dev_groups = stepper_dev_attr_groups,
	.dev_release = stepper_dev_release,
};

static struct device *
__stepper_device_register(struct device *dev, const char *name, void *drvdata,
	                      struct stepper_ops *ops, struct stepper_vel_cfg *cfg)
{
	int err;
	struct stepper_device *stepdev;
	struct device *hdev;
	stepdev = kzalloc(sizeof(*stepdev), GFP_KERNEL);
	if (stepdev == NULL) {
		err = -ENOMEM;
		goto error;
	}

	hdev = &stepdev->dev;
	hdev->class = &stepper_class;
	hdev->parent = dev;
	hdev->of_node = dev ? dev->of_node : NULL;
	dev_set_drvdata(hdev, drvdata);
	dev_set_name(hdev, name);
	err = device_register(hdev);
	if (err) {
		goto free_stepdev;
	}

	INIT_DELAYED_WORK(&stepdev->velocity_dwork, stepper_reach_target_velocity);
	mutex_init(&stepdev->velocity_mutex);
	stepdev->velocity_current = 0;
	stepdev->velocity_target = 0;
	stepdev->velocity_shifting = false;
	stepdev->ops = *ops;
	stepdev->cfg = *cfg;

	return hdev;

free_stepdev:
	kfree(stepdev);
error:
	return ERR_PTR(err);
}

struct device *stepper_device_register(struct device *dev, const char *name,
                                       void *drvdata, struct stepper_ops *ops,
                                       struct stepper_vel_cfg *cfg)
{
	if (NULL == name) {
		return ERR_PTR(-EINVAL);
	}
	if (NULL == ops) {
		return ERR_PTR(-EINVAL);
	}
	if (NULL == cfg) {
		return ERR_PTR(-EINVAL);
	}
	return __stepper_device_register(dev, name, drvdata, ops, cfg);
}
EXPORT_SYMBOL(stepper_device_register);

void stepper_device_unregister(struct device *dev)
{
	device_unregister(dev);
}
EXPORT_SYMBOL(stepper_device_unregister);

static void devm_stepper_release(struct device *dev, void *res)
{
	struct device *hwdev = *(struct device **)res;
	stepper_device_unregister(hwdev);
}

struct device *devm_stepper_device_register(struct device *dev,
                                            const char *name, void *drvdata,
                                            struct stepper_ops *ops,
                                            struct stepper_vel_cfg *cfg)
{
	struct device **ptr, *hwdev;

	if (!dev)
		return ERR_PTR(-EINVAL);

	ptr = devres_alloc(devm_stepper_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	hwdev = stepper_device_register(dev, name, drvdata, ops, cfg);
	if (IS_ERR(hwdev))
		goto error;

	*ptr = hwdev;
	devres_add(dev, ptr);

	return hwdev;

error:
	devres_free(ptr);
	return hwdev;
}
EXPORT_SYMBOL(devm_stepper_device_register);

static int devm_stepper_match(struct device *dev, void *res, void *data)
{
	struct device **hwdev = res;

	return *hwdev == data;
}

void devm_stepper_device_unregister(struct device *dev)
{
	WARN_ON(devres_release(dev, devm_stepper_release, devm_stepper_match, dev));
}
EXPORT_SYMBOL(devm_stepper_device_unregister);

static int __init stepper_init(void)
{
	int err;
	/* class */
	err = class_register(&stepper_class);
	if (0 != err) {
		pr_err("stepper: Failed to register sysfs class.\n");
		goto out;
	}
	/* workqueue */
	stepper_workqueue = alloc_workqueue("stepper", 0, 0);
	if (NULL == stepper_workqueue) {
		pr_err("stepper: Failed to allocate workqueue.\n");
		err = -ENOMEM;
		goto out_class;
	}
	return 0;

out_class:
	class_unregister(&stepper_class);
out:
	return err;
}

static void __exit stepper_exit(void)
{
	flush_workqueue(stepper_workqueue);
	destroy_workqueue(stepper_workqueue);
	class_unregister(&stepper_class);
}

subsys_initcall(stepper_init);
module_exit(stepper_exit);

MODULE_AUTHOR("Frederik Peter Aalund <fpa@sbtinstruments.com>");
MODULE_DESCRIPTION("Stepper motor driver subsystem");
MODULE_LICENSE("GPL");
