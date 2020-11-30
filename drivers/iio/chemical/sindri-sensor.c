/*
 * sindri-sensor.c - Support for Sindri conductivity sensor board
 *
 * Copyright (C) 2020 SBT Instruments A/S
 * Author: Jonatan Midtgaard <jmi@sbtinstruments.com>
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/irq.h>
#include <linux/irq_work.h>
#include <linux/i2c.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/types.h>

#define SINDRI_REGMAP_NAME	"sindri_regmap"
#define SINDRI_DRV_NAME		"sindri"

/* REGISTERS */
#define SINDRI_REG_HW_VERSION 0x00
#define SINDRI_REG_FW_VERSION 0x01
#define SINDRI_REG_INTERRUPT_CTRL 0x02
#define SINDRI_REG_COND_CAL_VALID 0x03
#define SINDRI_REG_COND_CAL_OFFSET 0x04
#define SINDRI_REG_COND_CAL_GAIN 0x06
#define SINDRI_REG_COND 0x0a


struct sindri_data {
	struct i2c_client *client;
	struct iio_trigger *trig;
	struct sindri_device *chip;
	struct regmap *regmap;
	struct irq_work work;
	unsigned int interrupt_enabled;
	unsigned int hw_version;
	unsigned int fw_version;
	unsigned int calibration_valid;
	int calibration_offset;
	int calibration_gain;

	// A single datapoint
	// Elements need to be aligned to their own length.
	__be16 buffer[8]; /* 2 bytes conductivity + 6 bytes pad + 8 bytes timestamp */
};

static const struct regmap_config sindri_regmap_config = {
	.name = SINDRI_REGMAP_NAME,
	.reg_bits = 8,
	.val_bits = 8,
};

static int sindri_buffer_num_channels(const struct iio_chan_spec *spec)
{
	int idx = 0;

	for (; spec->type != IIO_TIMESTAMP; spec++)
		idx++;

	return idx;
};

static int sindri_reg_size(const uint8_t reg)
{
	switch (reg) {
	case SINDRI_REG_COND:
		return sizeof(__be16);
	case SINDRI_REG_COND_CAL_OFFSET:
	case SINDRI_REG_COND_CAL_GAIN:
		return sizeof(__be16);
	default:
		return sizeof(uint8_t);
	}
};

static const struct iio_chan_spec sindri_channels[] = {
	{
		.type = IIO_ELECTRICALCONDUCTIVITY,
		.address = SINDRI_REG_COND,
		.info_mask_separate =
			BIT(IIO_CHAN_INFO_RAW),
		.scan_index = 0,
		.scan_type = {
			.sign = 'u',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_BE,
		},
	},
	IIO_CHAN_SOFT_TIMESTAMP(1),
};


struct sindri_device {
	const struct iio_chan_spec *channels;
	int num_channels;
	int data_reg;
	bool scan_timestamp;
};

static struct sindri_device sindri_devices[] = {
	[0] = {
            .channels = sindri_channels,
            .num_channels = 3,
            .data_reg = SINDRI_REG_COND,
			.scan_timestamp = true,
	},
};

static int sindri_buffer_postenable(struct iio_dev *indio_dev)
{
	struct sindri_data *data = iio_priv(indio_dev);
	int ret;

	ret = iio_triggered_buffer_postenable(indio_dev);
	return ret;
}

static int sindri_buffer_predisable(struct iio_dev *indio_dev)
{
	struct sindri_data *data = iio_priv(indio_dev);
	int ret;

	ret = iio_triggered_buffer_predisable(indio_dev);
	return ret;
}

static const struct iio_buffer_setup_ops sindri_buffer_setup_ops = {
	.postenable = sindri_buffer_postenable,
	.predisable = sindri_buffer_predisable,
};

static const struct iio_trigger_ops sindri_interrupt_trigger_ops = {
};

static void sindri_work_handler(struct irq_work *work)
{
	struct sindri_data *data = container_of(work, struct sindri_data, work);
	iio_trigger_poll(data->trig);
}

// To get into this function, first enable the buffer from user-space
static irqreturn_t sindri_trigger_handler(int irq, void *private)
{
	struct iio_poll_func *pf = private;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct sindri_data *data = iio_priv(indio_dev);
	//int channels = sindri_buffer_num_channels(data->chip->channels);

	int ret;
	
	ret = regmap_bulk_read(data->regmap, data->chip->data_reg,
			      &data->buffer, sindri_reg_size(data->chip->data_reg));

	
	if (!ret)
		iio_push_to_buffers_with_timestamp(indio_dev, data->buffer,
				pf->timestamp);

	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

// Triggered in interrupt context
static irqreturn_t sindri_interrupt_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct sindri_data *data = iio_priv(indio_dev);

	irq_work_queue(&data->work);

	return IRQ_HANDLED;
}

static int sindri_read_measurement(struct sindri_data *data, int reg, __be32 *val)
{
	struct device *dev = &data->client->dev;
	int ret;

	ret = regmap_bulk_read(data->regmap, reg, val, sizeof(*val));

	return ret;
}

// When reading the sysfs-files manually
static int sindri_read_raw(struct iio_dev *indio_dev,
			  struct iio_chan_spec const *chan,
			  int *val, int *val2, long mask)
{
	struct sindri_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW: {
		int ret;
		uint8_t short_reg;
		__be16 long_reg;

		switch (chan->type) {
		case IIO_ELECTRICALCONDUCTIVITY:
			ret = regmap_bulk_read(data->regmap, chan->address,
					       &long_reg, sindri_reg_size(chan->address));
			if (!ret) {
				*val = be16_to_cpu(long_reg);
				ret = IIO_VAL_INT;
			}
			return ret;
		default:
			return -EINVAL;
		}
		return -EINVAL;
	}
	}
}

// Other sysfs attributes
// Modelled after proximity/as3935.c
// INTERRUPT CONTROL
static ssize_t sindri_interrupt_control_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct sindri_data *data = iio_priv(dev_to_iio_dev(dev));
	char val;
	int ret;

	ret = regmap_bulk_read(data->regmap, SINDRI_REG_INTERRUPT_CTRL,
					       &val, sindri_reg_size(SINDRI_REG_INTERRUPT_CTRL));
	if (ret)
		return ret;
	return sprintf(buf, "%d\n", val);
}

static ssize_t sindri_interrupt_control_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct sindri_data *data = iio_priv(dev_to_iio_dev(dev));
	unsigned long val;
	int ret;

	ret = kstrtoul((const char *) buf, 10, &val);
	if (ret)
		return -EINVAL;

	ret = regmap_bulk_write(data->regmap, SINDRI_REG_INTERRUPT_CTRL,
					       &val, sindri_reg_size(SINDRI_REG_INTERRUPT_CTRL));

	return len;
}

static IIO_DEVICE_ATTR(interrupt_ctrl, S_IRUGO | S_IWUSR,
	sindri_interrupt_control_show, sindri_interrupt_control_store, SINDRI_REG_INTERRUPT_CTRL);

// HW and FW versions are static values, acquired during probe.
// HW VERSION
unsigned int sindri_hw_version_acquire(struct sindri_data *data)
{
	char val;
	int ret;

	ret = regmap_bulk_read(data->regmap, SINDRI_REG_HW_VERSION,
					       &val, sindri_reg_size(SINDRI_REG_HW_VERSION));
	if (ret)
		return ret;
	return val;
}

static ssize_t sindri_hw_version_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct sindri_data *data = iio_priv(dev_to_iio_dev(dev));
	return sprintf(buf, "%d\n", data->hw_version);
}

static IIO_DEVICE_ATTR(hw_version, S_IRUGO,
	sindri_hw_version_show, NULL, SINDRI_REG_HW_VERSION);

// FW VERSION
unsigned int sindri_fw_version_acquire(struct sindri_data *data)
{
	char val;
	int ret;

	ret = regmap_bulk_read(data->regmap, SINDRI_REG_FW_VERSION,
					       &val, sindri_reg_size(SINDRI_REG_FW_VERSION));
	if (ret)
		return ret;
	return val;
}

static ssize_t sindri_fw_version_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct sindri_data *data = iio_priv(dev_to_iio_dev(dev));
	return sprintf(buf, "%d\n", data->fw_version);
}

static IIO_DEVICE_ATTR(fw_version, S_IRUGO,
	sindri_fw_version_show, NULL, SINDRI_REG_FW_VERSION);

// Same thing with calibration values
// Valid-ness

static ssize_t sindri_calibration_valid_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct sindri_data *data = iio_priv(dev_to_iio_dev(dev));
	char val;
	int ret;
	ret = regmap_bulk_read(data->regmap, SINDRI_REG_COND_CAL_VALID,
					       &val, sindri_reg_size(SINDRI_REG_COND_CAL_VALID));
	if (ret)
		return ret;
	return sprintf(buf, "%d\n", val);
}

static IIO_DEVICE_ATTR(calibration_valid, S_IRUGO,
	sindri_calibration_valid_show, NULL, SINDRI_REG_COND_CAL_VALID);

// Offset
// MAGIC! (TODO: Find something more portable. This apparently requires -O2)
int chars_to_int(char* c) {
	int i = *(signed char *)(&c[0]);
	i *= 1 << 8;
	i |= c[1];
	return i;
}

static ssize_t sindri_calibration_offset_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct sindri_data *data = iio_priv(dev_to_iio_dev(dev));
	__be16 val;
	int ret;
	ret = regmap_bulk_read(data->regmap, SINDRI_REG_COND_CAL_OFFSET,
					       &val, sindri_reg_size(SINDRI_REG_COND_CAL_OFFSET));
	if (ret)
		return ret;
	return sprintf(buf, "%d\n", be16_to_cpu(val));
}

static ssize_t sindri_calibration_offset_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct sindri_data *data = iio_priv(dev_to_iio_dev(dev));
	unsigned long val;
	int ret;

	ret = kstrtoul((const char *) buf, 10, &val);
	if (ret)
		return -EINVAL;

	// Convert to network format
	__be16 nval = cpu_to_be16(val);
	ret = regmap_bulk_write(data->regmap, SINDRI_REG_COND_CAL_OFFSET,
					       &nval, sindri_reg_size(SINDRI_REG_COND_CAL_OFFSET));
	

	return len;
}

static IIO_DEVICE_ATTR(calibration_offset, S_IRUGO | S_IWUSR,
	sindri_calibration_offset_show, sindri_calibration_offset_store, SINDRI_REG_COND_CAL_OFFSET);

// Gain
static ssize_t sindri_calibration_gain_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct sindri_data *data = iio_priv(dev_to_iio_dev(dev));
	__be16 val;
	int ret;
	ret = regmap_bulk_read(data->regmap, SINDRI_REG_COND_CAL_GAIN,
					       &val, sindri_reg_size(SINDRI_REG_COND_CAL_GAIN));
	if (ret)
		return ret;
	return sprintf(buf, "%d\n", be16_to_cpu(val));
}

static ssize_t sindri_calibration_gain_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct sindri_data *data = iio_priv(dev_to_iio_dev(dev));
	unsigned long val;
	int ret;

	ret = kstrtoul((const char *) buf, 10, &val);
	if (ret)
		return -EINVAL;

	// Convert to network format
	__be16 nval = cpu_to_be16(val);
	ret = regmap_bulk_write(data->regmap, SINDRI_REG_COND_CAL_GAIN,
					       &nval, sindri_reg_size(SINDRI_REG_COND_CAL_GAIN));
	

	return len;
}

static IIO_DEVICE_ATTR(calibration_gain, S_IRUGO | S_IWUSR,
	sindri_calibration_gain_show, sindri_calibration_gain_store, SINDRI_REG_COND_CAL_GAIN);


static struct attribute *sindri_attributes[] = {
	&iio_dev_attr_interrupt_ctrl.dev_attr.attr,
	&iio_dev_attr_hw_version.dev_attr.attr,
	&iio_dev_attr_fw_version.dev_attr.attr,
	&iio_dev_attr_calibration_valid.dev_attr.attr,
	&iio_dev_attr_calibration_offset.dev_attr.attr,
	&iio_dev_attr_calibration_gain.dev_attr.attr,
	NULL,
};

static const struct attribute_group sindri_attribute_group = {
	.attrs = sindri_attributes,
};

static const struct iio_info sindri_info = {
	.attrs = &sindri_attribute_group,
	.read_raw = &sindri_read_raw,
};

static const struct i2c_device_id sindri_id[] = {
	{ "sindri", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, sindri_id);

static const struct of_device_id sindri_dt_ids[] = {
	{ .compatible = "sbt,sindri", .data = (void*)0, },
	{ }
};
MODULE_DEVICE_TABLE(of, sindri_dt_ids);

static int sindri_probe(struct i2c_client *client,
		       const struct i2c_device_id *id)
{
	struct sindri_data *data;
	struct sindri_device *chip;
	const struct of_device_id *of_id;
	struct iio_trigger *trig;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	of_id = of_match_device(sindri_dt_ids, &client->dev);
	if (!of_id)
		chip = &sindri_devices[id->driver_data];
	else
		chip = &sindri_devices[(unsigned long)of_id->data];

	indio_dev->info = &sindri_info;
	indio_dev->name = SINDRI_DRV_NAME;
	indio_dev->channels = chip->channels;
	indio_dev->num_channels = chip->num_channels;
	indio_dev->modes = INDIO_BUFFER_SOFTWARE;

	trig = devm_iio_trigger_alloc(&client->dev, "%s",
				      indio_dev->name);

	if (!trig)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	data->trig = trig;
	data->chip = chip;
	trig->dev.parent = indio_dev->dev.parent;
	trig->ops = &sindri_interrupt_trigger_ops;
	iio_trigger_set_drvdata(trig, indio_dev);

	i2c_set_clientdata(client, indio_dev);

	data->regmap = devm_regmap_init_i2c(client, &sindri_regmap_config);
	if (IS_ERR(data->regmap)) {
		dev_err(&client->dev, "regmap initialization failed\n");
		return PTR_ERR(data->regmap);
	}

	ret = iio_trigger_register(trig);
	if (ret) {
		dev_err(&client->dev, "failed to register trigger\n");
		return ret;
	}

	ret = iio_triggered_buffer_setup(indio_dev, &iio_pollfunc_store_time,
		&sindri_trigger_handler, &sindri_buffer_setup_ops);
	if (ret) {
		dev_err(&client->dev, "cannot setup iio trigger\n");
		goto unregister_trigger;
	}

	init_irq_work(&data->work, sindri_work_handler);

	if (client->irq <= 0) {
		dev_err(&client->dev, "no valid irq defined\n");
		goto unregister_trigger;
	}
	/* interrupt pin rises when new measurement is ready */
	ret = devm_request_threaded_irq(&client->dev, client->irq,
			NULL, sindri_interrupt_handler,
			IRQF_TRIGGER_RISING | IRQF_ONESHOT,
			"sindri-interrupt",
			indio_dev);

	if (ret)
		dev_warn(&client->dev,
			"request irq (%d) failed\n", client->irq);
	else
		data->interrupt_enabled = 1;

	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&client->dev, "unable to register device\n");
		goto unregister_buffer;
	}

	// Acquire constant values
	data->hw_version = sindri_hw_version_acquire(data);
	data->fw_version = sindri_fw_version_acquire(data);
	//data->calibration_valid = sindri_calibration_valid_acquire(data);
	//data->calibration_offset = sindri_calibration_offset_acquire(data);
	//data->calibration_gain = sindri_calibration_gain_acquire(data);

	// Testing interface
	//uint8_t reg;
	//int retval;
	
	//retval = regmap_bulk_read(data->regmap, SINDRI_REG_FW_VERSION, &reg, sizeof(uint8_t));
	//dev_info(&client->dev, "fw version: %d\n", reg);
	//retval = regmap_bulk_read(data->regmap, SINDRI_REG_STATUS, &reg, sizeof(uint8_t));
	//dev_info(&client->dev, "status: %d\n", reg);
	
	//retval = regmap_write(data->regmap, SINDRI_REG_CYCLE_TIME, 0);
	//dev_info(&client->dev, "Return value: %d\n", retval);

	return 0;

unregister_buffer:
	iio_triggered_buffer_cleanup(indio_dev);

unregister_trigger:
	iio_trigger_unregister(data->trig);

	return ret;
}

static int sindri_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct sindri_data *data = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	iio_triggered_buffer_cleanup(indio_dev);
	iio_trigger_unregister(data->trig);

	return 0;
}

static struct i2c_driver sindri_driver = {
	.driver = {
		.name	= SINDRI_DRV_NAME,
		.of_match_table	= of_match_ptr(sindri_dt_ids),
	},
	.probe		= sindri_probe,
    .remove     = sindri_remove,
	.id_table	= sindri_id,
};
module_i2c_driver(sindri_driver);

MODULE_AUTHOR("Jonatan Midtgaard <jmi@sbtinstruments.com>");
MODULE_DESCRIPTION("Sindri sensor board");
MODULE_LICENSE("GPL");
