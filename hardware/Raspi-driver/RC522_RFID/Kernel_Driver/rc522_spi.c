// SPDX-License-Identifier: GPL-2.0
/*
 * RC522 (MFRC522) SPI driver - probe/remove and register I/O
 */

#include "rc522.h"
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/spi/spi.h>

struct rc522_spi {
	struct spi_device *spi;
	struct gpio_desc *rst_gpio;
	struct rc522_dev chip;
};

static int rc522_spi_write_reg(void *ctx, u8 addr, u8 val)
{
	struct rc522_spi *rspi = ctx;
	u8 buf[2];

	buf[0] = (u8)((addr << 1) & 0x7E);
	buf[1] = val;
	return spi_write_then_read(rspi->spi, buf, 2, NULL, 0);
}

static int rc522_spi_read_reg(void *ctx, u8 addr, u8 *val)
{
	struct rc522_spi *rspi = ctx;
	u8 tx[2], rx[2];
	struct spi_transfer t = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len    = 2,
	};
	struct spi_message m;
	int ret;

	/* RC522는 풀듀플렉스: 보내는 2바이트와 동시에 받는 2바이트. spi_write_then_read는 쓰기/읽기 분리라 값이 0으로 옴 */
	tx[0] = (u8)(((addr << 1) & 0x7E) | 0x80);
	tx[1] = 0x00;
	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	ret = spi_sync(rspi->spi, &m);
	if (ret == 0)
		*val = rx[1];
	return ret;
}

static void rc522_spi_msleep(unsigned int msecs)
{
	msleep(msecs);
}

static void rc522_spi_usleep(unsigned int usecs)
{
	usleep_range(usecs, usecs + 50);
}

static const struct rc522_ops rc522_spi_ops = {
	.write_reg = rc522_spi_write_reg,
	.read_reg  = rc522_spi_read_reg,
	.msleep    = rc522_spi_msleep,
	.usleep    = rc522_spi_usleep,
};

static int rc522_spi_probe(struct spi_device *spi)
{
	struct rc522_spi *rspi;
	int ret;

	dev_info(&spi->dev, "rc522_spi_probe: called\n");
	dev_info(&spi->dev, "  chip_select: %d, max_speed: %d Hz\n", spi->chip_select, spi->max_speed_hz);

	rspi = devm_kzalloc(&spi->dev, sizeof(*rspi), GFP_KERNEL);
	if (!rspi)
		return -ENOMEM;

	rspi->spi = spi;
	spi_set_drvdata(spi, rspi);

	rspi->rst_gpio = devm_gpiod_get_optional(&spi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(rspi->rst_gpio))
		return PTR_ERR(rspi->rst_gpio);

	if (rspi->rst_gpio) {
		gpiod_direction_output(rspi->rst_gpio, 0);
		msleep(10);
		gpiod_set_value_cansleep(rspi->rst_gpio, 1);
		msleep(50);
	}

	rspi->chip.ctx = rspi;
	rspi->chip.ops = &rc522_spi_ops;

	ret = rc522_core_init(&rspi->chip);
	if (ret)
		return ret;

	ret = rc522_chardev_register(&spi->dev, &rspi->chip);
	if (ret) {
		dev_err(&spi->dev, "rc522_chardev_register failed: %d\n", ret);
		rc522_core_cleanup(&rspi->chip);
		return ret;
	}

	dev_info(&spi->dev, "rc522 probed successfully, /dev/rc522 created\n");
	return 0;
}

static void rc522_spi_remove(struct spi_device *spi)
{
	struct rc522_spi *rspi = spi_get_drvdata(spi);

	rc522_chardev_unregister(&rspi->chip);
	rc522_core_cleanup(&rspi->chip);
}

static const struct spi_device_id rc522_spi_id[] = {
	{ "rc522", 0 },
	{ "nxp,rc522", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, rc522_spi_id);

static const struct of_device_id rc522_of_match[] = {
	{ .compatible = "nxp,rc522", },
	{ }
};
MODULE_DEVICE_TABLE(of, rc522_of_match);

static struct spi_driver rc522_spi_driver = {
	.driver = {
		.name   = "rc522",
		.of_match_table = rc522_of_match,
	},
	.probe  = rc522_spi_probe,
	.remove = rc522_spi_remove,
	.id_table = rc522_spi_id,
};

module_spi_driver(rc522_spi_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("RC522/MFRC522 RFID SPI driver");
MODULE_AUTHOR("SFEPS");
