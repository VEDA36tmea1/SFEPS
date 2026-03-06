// SPDX-License-Identifier: GPL-2.0
/*
 * RC522 (MFRC522) SPI driver - probe/remove and register I/O
 * IRQ 전환 작업은 이 파일에서 단계별로 진행
 */

#include "rc522.h"
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
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

	/* RC522는 풀듀플렉스: 보내는 2바이트와 동시에 받는 2바이트 */
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

/* 3단계: IRQ 요청 + 간단한 로그용 핸들러 (동작 변경 없음) */
static irqreturn_t rc522_spi_irq_thread(int irq, void *dev_id)
{
	struct rc522_spi *rspi = dev_id;

	atomic_set(&rspi->chip.irq_event, 1);
	wake_up_interruptible(&rspi->chip.wagititq);
	dev_info(&rspi->spi->dev, "rc522_irq: interrupt received (irq=%d)\n", irq);
	return IRQ_HANDLED;
}

static int rc522_spi_probe(struct spi_device *spi)
{
	struct rc522_spi *rspi;
	int ret;

	dev_info(&spi->dev, "rc522_irq/probe: called\n");
	dev_info(&spi->dev, "  chip_select: %d, max_speed: %d Hz\n",
		 spi->chip_select, spi->max_speed_hz);
	dev_info(&spi->dev, "  spi->irq: %d (IRQ 단계에서 사용 예정)\n", spi->irq);

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

	/* spi->irq가 유효하면 인터럽트 요청 (현재는 로그만 출력) */
	if (spi->irq > 0) {
		ret = devm_request_threaded_irq(&spi->dev, spi->irq,
						NULL, rc522_spi_irq_thread,
						IRQF_ONESHOT |
						IRQF_TRIGGER_FALLING,
						"rc522-irq", rspi);
		if (ret) {
			dev_warn(&spi->dev,
				 "rc522_irq: failed to request irq %d: %d\n",
				 spi->irq, ret);
		} else {
			dev_info(&spi->dev,
				 "rc522_irq: requested irq %d successfully\n",
				 spi->irq);
		}
	} else {
		dev_warn(&spi->dev, "rc522_irq: spi->irq is 0, IRQ not requested\n");
	}

	dev_info(&spi->dev, "rc522_irq/probed successfully, /dev/rc522 created\n");
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
MODULE_DESCRIPTION("RC522/MFRC522 RFID SPI driver (IRQ workspace)");
MODULE_AUTHOR("SFEPS");

