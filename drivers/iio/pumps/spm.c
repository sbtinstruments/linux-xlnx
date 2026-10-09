/* SPDX-License-Identifier: GPL-2.0 */

/*
 * spm.c - Support for Smart Pump Module driver from The Lee Company.
 *
 * Copyright (C) 2024 SBT Instruments 
 * 
 * Marcos Gonzalez Diaz <mgd@sbtinstruments.com>
 *
 * I2C slave address: 0x25
 *
 * Datasheet:
 * https://www.theleeco.com/uploads/2023/06/TG003-PCB-Serial-Communications-Guide.pdf
 */

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/errno.h>
#include <linux/i2c.h>

#include <spm.h>

static const struct iio_chan_spec spm_channels[] = {
	{
		.type = IIO_PRESSURE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_POWER,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_VOLTAGE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
};

struct spm_state {
	struct i2c_client *client;
};

int spm_i2c_read_int16(struct i2c_client *client, u8 reg, s16 *val)
{
	int ret;
	u8 cmd_read_reg = SPM_READ_BIT + reg;
	u16 val_le16;

	ret = i2c_master_send(client, &cmd_read_reg, sizeof(cmd_read_reg));
	if (ret < 0) {
		dev_err(&client->dev, "Failed to send read16 command, ret=%d\n",
			ret);
		return ret;
	}
	if (ret != sizeof(cmd_read_reg))
		return -EIO;

	ret = i2c_master_recv(client, (u8 *)&val_le16, sizeof(val_le16));
	if (ret < 0) {
		dev_err(&client->dev,
			"Failed to receive data of a read16 command, ret=%d\n",
			ret);
		return ret;
	}
	if (ret != sizeof(val_le16))
		return -EIO;

	*val = le16_to_cpu(val_le16);
	return 0;
}

int spm_i2c_read_float(struct i2c_client *client, u8 reg, s32 *val)
{
	int ret;
	u8 cmd_read_reg = SPM_READ_BIT + reg;
	s32 val_le32;

	ret = i2c_master_send(client, &cmd_read_reg, sizeof(cmd_read_reg));
	if (ret < 0) {
		dev_err(&client->dev, "Failed to send read16 command, ret=%d\n",
			ret);
		return ret;
	}
	if (ret != sizeof(cmd_read_reg))
		return -EIO;

	ret = i2c_master_recv(client, (u8 *)&val_le32, sizeof(val_le32));
	if (ret < 0) {
		dev_err(&client->dev,
			"Failed to receive data of a read16 command, ret=%d\n",
			ret);
		return ret;
	}
	if (ret != sizeof(val_le32))
		return -EIO;

	*val = le32_to_cpu(val_le32);
	return 0;
}

int spm_i2c_write_int16(struct i2c_client *client, u8 reg, s16 val)
{
	int ret;
	s16 val_le16 = cpu_to_le16(val);
	struct spm_write_int16_cmd cmd = { .reg = reg, .val_le16 = val_le16 };

	ret = i2c_master_send(client, (u8 *)&cmd, sizeof(cmd));
	if (ret < 0) {
		dev_err(&client->dev,
			"Failed to send i2c write int16 command, ret=%d\n",
			ret);
	}
	if (ret != sizeof(cmd))
		return -EIO;

	return 0;
}

int spm_i2c_write_float(struct i2c_client *client, u8 reg, s32 val)
{
	int ret;
	s32 val_le32 = cpu_to_le32(val);
	struct spm_write_float_cmd cmd = { .reg = reg, .val_le32 = val_le32 };

	ret = i2c_master_send(client, (u8 *)&cmd, sizeof(cmd));
	if (ret < 0) {
		dev_err(&client->dev,
			"Failed to send i2c write float command, ret=%d\n",
			ret);
	}
	if (ret != sizeof(cmd))
		return -EIO;

	return 0;
}

static int spm_read_raw(struct iio_dev *indio_dev,
			struct iio_chan_spec const *chan, int *val, int *val2,
			long mask)
{
	struct spm_state *st = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_POWER:
			ret = spm_i2c_read_float(
				st->client, SPM_REG_MEAS_DRIVE_MILLIWATTS, val);
			if (ret < 0)
				return ret;

			return IIO_VAL_INT;
		case IIO_PRESSURE:
			ret = spm_i2c_read_float(
				st->client, SPM_REG_MEAS_DIGITAL_PRESSURE, val);
			if (ret < 0)
				return ret;

			return IIO_VAL_INT;
		case IIO_VOLTAGE:
			ret = spm_i2c_read_float(st->client, SPM_REG_SET_VAL,
						 val);
			if (ret < 0)
				return ret;

			return IIO_VAL_INT;
		default:
			break;
		}
	default:
		break;
	}

	return -EINVAL;
}

static int spm_write_raw(struct iio_dev *indio_dev,
			 struct iio_chan_spec const *chan, int val, int val2,
			 long mask)
{
	struct spm_state *st = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_VOLTAGE:
			ret = spm_i2c_write_float(st->client, SPM_REG_SET_VAL,
						  val);
			if (ret < 0)
				return ret;

			return ret;
		default:
			break;
		}
	default:
		break;
	}
	return -EINVAL;
}

static const struct iio_info spm_info = {
	.read_raw = &spm_read_raw,
	.write_raw = &spm_write_raw,
};

static int spm_init_device(struct iio_dev *indio_dev)
{
	struct spm_state *st = iio_priv(indio_dev);
	int ret;

	ret = spm_i2c_write_int16(st->client, SPM_REG_PUMP_ENABLE, 0);
	if (ret < 0)
		return ret;
	ret = spm_i2c_write_int16(st->client, SPM_REG_CONTROL_MODE,
				  SPM_MODE_PID);
	if (ret < 0)
		return ret;
	ret = spm_i2c_write_int16(st->client,
				  SPM_REG_MANUAL_MODE_SETPOINT_SOURCE,
				  SPM_SOURCE_SETVAL);
	if (ret < 0)
		return ret;
	ret = spm_i2c_write_int16(st->client, SPM_REG_PID_MODE_SETPOINT_SOURCE,
				  SPM_SOURCE_SETVAL);
	if (ret < 0)
		return ret;
	ret = spm_i2c_write_int16(st->client, SPM_REG_PID_MODE_MEAS_SOURCE,
				  SPM_SOURCE_DIGITAL_PRESSURE);
	if (ret < 0)
		return ret;
	ret = spm_i2c_write_float(st->client, SPM_REG_SET_VAL, 0);
	if (ret < 0)
		return ret;
	ret = spm_i2c_write_int16(st->client, SPM_REG_PUMP_ENABLE, 1);
	if (ret < 0)
		return ret;

	return ret;
}

static int spm_i2c_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct spm_state *st;
	struct iio_dev *indio_dev;
	int ret = -EINVAL;
	s16 device_type;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	st->client = client;

	ret = spm_i2c_read_int16(st->client, SPM_REG_DEVICE_TYPE, &device_type);
	if (ret < 0)
		return ret;
	dev_info(&client->dev, "device type: %d\n", device_type);

	ret = spm_init_device(indio_dev);
	if (ret < 0)
		return ret;

	indio_dev->name = id->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = spm_channels;
	indio_dev->num_channels = ARRAY_SIZE(spm_channels);
	indio_dev->info = &spm_info;

	ret = devm_iio_device_register(&client->dev, indio_dev);
	return ret;
}

static void spm_i2c_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);

	iio_device_unregister(indio_dev);
}

static const struct i2c_device_id spm_i2c_id[] = { { "spm" }, {} };
MODULE_DEVICE_TABLE(i2c, spm_i2c_id);

static const struct of_device_id spm_i2c_of_match[] = {
	{ .compatible = "theleecompany,spm" },
	{}
};
MODULE_DEVICE_TABLE(of, spm_i2c_of_match);

static struct i2c_driver spm_i2c_driver = {
	.driver = { .name = "spm",
		    .of_match_table = of_match_ptr(spm_i2c_of_match) },
	.probe = spm_i2c_probe,
	.remove = spm_i2c_remove,
	.id_table = spm_i2c_id,
};

module_i2c_driver(spm_i2c_driver);

MODULE_AUTHOR("Marcos Gonzalez Diaz <mgd@sbtinstruments.com>");
MODULE_DESCRIPTION("Driver for Smart Pump Module from The Lee Company");
MODULE_LICENSE("GPL");
