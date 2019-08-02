/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2019 Frederik Peter Aalund, SBT Instruments
 */
#include <linux/module.h>
#include <linux/mtd/spi-nor.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>


#define AT25SF041_MAN_ID 0x1F
#define AT25SF041_DEV_ID1 0x84
#define AT25SF041_DEV_ID2 0x01
#define AT25SF041_PAGE_SIZE 256


struct at25sf041 {
	struct spi_nor nor;
};

struct at25sf041_page {
	loff_t spi_addr_start;
	const u_char *buffer_start;
	size_t len;
};


#ifdef CONFIG_SPI_AT25SF041_TEST_CON
/* Tests if the chip is connected by probing the status and ID registers.
 *
 * If either of the MISO, MOSI, or CLK pins are physically disconnected,
 * then the status register will read 0xFF.
 *
 * Note that it is not enough to probe the status register alone. Empirical
 * data shows, that if the CS pin is physically disconnected, then the status
 * register returns 0x00, which denotes 'device ready', unfortunately.
 * Therefore, we also probe the ID register.
 *
 * Note that we can't probe the ID register alone. If a write is in progress,
 * then the ID register will return 0xFF,0xFF,0xFF. Consequently, we have to
 * probe the status register first, to find out if a write is in progress.
 */
static int at25sf041_test_con(struct spi_device *spi)
{
	u8 op_rdsr = SPINOR_OP_RDSR;
	u8 status = 0xAB; /* dummy value */
	u8 op_rdid = SPINOR_OP_RDID;
	u8 id[3] = { 0, 0, 0 };
	struct spi_transfer sr_req = {
		.tx_buf = &op_rdsr,
		.len = 1,
	};
	struct spi_transfer sr_res = {
		.rx_buf = &status,
		.len = 1,
		/* pull chip select down between the two requests */
		.cs_change = 1,
	};
	struct spi_transfer id_req = {
		.tx_buf = &op_rdid,
		.len = 1,
	};
	struct spi_transfer id_res = {
		.rx_buf = id,
		.len = ARRAY_SIZE(id),
	};
	struct spi_message m;
	int result;
	spi_message_init(&m);
	spi_message_add_tail(&sr_req, &m);
	spi_message_add_tail(&sr_res, &m);
	spi_message_add_tail(&id_req, &m);
	spi_message_add_tail(&id_res, &m);
	result = spi_sync(spi, &m);
	if (0 != result) {
		return result;
	}
	dev_dbg(&spi->dev, "Status register: 0x%x\n", status);
	/* the chip will override the dummy value (0xAB) with 0xFF if there is a problem
	 * with the physical connection. */
	if (0xFF == status) {
		return -EIO;
	}
	/* the ID will only be available if a write is not in progress (WIP). */
	if (!(status & SR_WIP)) {
		/* the id will be malformed if there is a problem with the physical
		 * connection. */
		if (AT25SF041_MAN_ID  != id[0] ||
			AT25SF041_DEV_ID1 != id[1] ||
			AT25SF041_DEV_ID2 != id[2]) {
			dev_dbg(&spi->dev, "ID (fail): 0x%x 0x%x 0x%x\n", id[0], id[1], id[2]);
			return -EIO;
		} else {
			dev_dbg(&spi->dev, "ID (pass): 0x%x 0x%x 0x%x\n", id[0], id[1], id[2]);
		}
	}
	return 0;
}
#endif

static int at25sf041_read_reg(struct spi_nor *nor, u8 opcode, u8 *buf, int len)
{
	struct spi_device *spi = container_of(nor->dev, struct spi_device, dev);
	struct spi_transfer command_t = {
		.tx_buf = &opcode,
		.len = 1,
	};
	struct spi_transfer data_t = {
		.rx_buf = buf,
		.len = len,
	};
	struct spi_message m;
	int result;
#ifdef CONFIG_SPI_AT25SF041_TEST_CON
	result = at25sf041_test_con(spi);
	if (0 != result) {
		dev_dbg(nor->dev, "Connection test failed: %d\n", result);
		return result;
	}
#endif
	spi_message_init(&m);
	spi_message_add_tail(&command_t, &m);
	if (0 < len) {
		spi_message_add_tail(&data_t, &m);
	}
	result = spi_sync(spi, &m);
	if (0 != result) {
		return result;
	}
	return 0;
}

static int at25sf041_write_reg(struct spi_nor *nor, u8 opcode, u8 *buf, int len)
{
	struct spi_device *spi = container_of(nor->dev, struct spi_device, dev);
	struct spi_transfer command_t = {
		.tx_buf = &opcode,
		.len = 1,
	};
	struct spi_transfer data_t = {
		.tx_buf = buf,
		.len = len,
	};
	struct spi_message m;
	int result;
#ifdef CONFIG_SPI_AT25SF041_TEST_CON
	result = at25sf041_test_con(spi);
	if (0 != result) {
		dev_dbg(nor->dev, "Connection test failed: %d\n", result);
		return result;
	}
#endif
	spi_message_init(&m);
	spi_message_add_tail(&command_t, &m);
	if (0 < len) {
		spi_message_add_tail(&data_t, &m);
	}
	result = spi_sync(spi, &m);
	if (0 != result) {
		return result;
	}
	return 0;
}

static ssize_t at25sf041_read(struct spi_nor *nor, loff_t from,
                                  size_t len, u_char *read_buf)
{
	struct spi_device *spi = container_of(nor->dev, struct spi_device, dev);
	u8 command_buf[] = {
		/* read array opcode */
		0x0b,
		/* address */
		(from >> 16) & 0xFF,
		(from >> 8) & 0xFF,
		from & 0xFF,
		/* dummy byte */
		0,
	};
	struct spi_transfer command_t = {
		.tx_buf = command_buf,
		.len = ARRAY_SIZE(command_buf),
	};
	loff_t max_addr = nor->mtd.size;
	loff_t end = min(max_addr, from + len);
	size_t read_len = end - from;
	struct spi_transfer data_t = {
		.rx_buf = read_buf,
		.len = read_len,
	};
	struct spi_message m;
	int result;
#ifdef CONFIG_SPI_AT25SF041_TEST_CON
	result = at25sf041_test_con(spi);
	if (0 != result) {
		dev_dbg(nor->dev, "Connection test failed: %d\n", result);
		return result;
	}
#endif
	spi_message_init(&m);
	spi_message_add_tail(&command_t, &m);
	spi_message_add_tail(&data_t, &m);
	result = spi_sync(spi, &m);
	if (0 != result) {
		return result;
	}
	return read_len;
}

static ssize_t at25sf041_write_page(struct spi_nor *nor,
                            struct at25sf041_page *page)
{
	struct spi_device *spi = container_of(nor->dev, struct spi_device, dev);
	u8 command_buf[] = {
		/* program opcode */
		0x02,
		/* address */
		(page->spi_addr_start >> 16) & 0xFF,
		(page->spi_addr_start >> 8) & 0xFF,
		page->spi_addr_start & 0xFF,
	};
	struct spi_transfer command_t = {
		.tx_buf = command_buf,
		.len = ARRAY_SIZE(command_buf),
	};
	struct spi_transfer data_t = {
		.tx_buf = page->buffer_start,
		.len = page->len,
	};
	struct spi_message m;
	ssize_t result;
#ifdef CONFIG_SPI_AT25SF041_TEST_CON
	result = at25sf041_test_con(spi);
	if (0 != result) {
		dev_dbg(nor->dev, "Connection test failed: %d\n", result);
		return result;
	}
#endif
	spi_message_init(&m);
	spi_message_add_tail(&command_t, &m);
	spi_message_add_tail(&data_t, &m);
	result = spi_sync(spi, &m);
	if (0 != result) {
		return result;
	}
	return page->len;
}

/* Writes longer than a page must be split into pages */
static ssize_t at25sf041_write(struct spi_nor *nor, loff_t to,
                          size_t len, const u_char *write_buf)
{
	loff_t max_addr = nor->mtd.size;
	loff_t end = min(max_addr, to + len);
	size_t write_len = end - to;
	size_t data_left = write_len;
	ssize_t result;
	for (; 0 < data_left;) {
		size_t page_off = to & 0xFF;
		size_t page_rem = AT25SF041_PAGE_SIZE - page_off;
		struct at25sf041_page page = {
			.spi_addr_start = to,
			.buffer_start = write_buf,
			.len = min(page_rem, data_left),
		};
		result = at25sf041_write_page(nor, &page);
		if (0 > result) {
			return result;
		}
		if (page.len != result) {
			return -EIO;
		}
		to += page.len;
		write_buf += page.len;
		data_left -= page.len;
	}
	return write_len;
}

static int at25sf041_probe(struct spi_device *spi)
{
	struct at25sf041 *at25;
	struct device *dev = &spi->dev;
	struct spi_nor_hwcaps hwcaps = {
		.mask = SNOR_HWCAPS_READ |
		        SNOR_HWCAPS_READ_FAST |
		        SNOR_HWCAPS_PP,
	};
	int result;

	/* allocate at25sf041 data */
	at25 = devm_kzalloc(dev, sizeof(struct at25sf041), GFP_KERNEL);
	if (!at25) {
		return -ENOMEM;
	}
	spi_set_drvdata(spi, at25);

	at25->nor.priv = spi;
	at25->nor.spi = spi;

	/* initialize spi_nor struct */
	at25->nor.dev = dev;
	at25->nor.read_reg = at25sf041_read_reg;
	at25->nor.write_reg = at25sf041_write_reg;
	at25->nor.read = at25sf041_read;
	at25->nor.write = at25sf041_write;

	/* scan for flash chip */
	result = spi_nor_scan(&at25->nor, "at25sf041", &hwcaps);
	if (0 != result) {
		dev_err(dev, "Failed to find flash memory chip: %d\n", result);
		return result;
	}

	/* register memory technology device. E.g., /dev/mtd0 */
	result = mtd_device_register(&at25->nor.mtd, NULL, 0);
	if (0 != result) {
		dev_err(dev, "Failed to register MTD device: %d\n", result);
		return result;
	}

	dev_dbg(dev, "Success\n");
	return 0;
}

static int at25sf041_remove(struct spi_device *spi)
{
	struct at25sf041 *at25 = spi_get_drvdata(spi);
	struct spi_nor *nor = &at25->nor;
	mtd_device_unregister(&nor->mtd);
	return 0;
}


static const struct of_device_id at25sf041_of_match[] = {
	{.compatible = "at25sf041"},
	{},
};
MODULE_DEVICE_TABLE(of, at25sf041_of_match);


static struct spi_driver at25sf041_driver = {
	.driver = {
		.name = "at25sf041",
		.of_match_table = at25sf041_of_match
	},
	.probe = at25sf041_probe,
	.remove = at25sf041_remove,
};
module_spi_driver(at25sf041_driver)


MODULE_AUTHOR("Frederik Peter Aalund <fpa@sbtinstruments.com>");
MODULE_DESCRIPTION("AT25SF041 SPI Serial Flash Memory");
MODULE_LICENSE("GPL");
