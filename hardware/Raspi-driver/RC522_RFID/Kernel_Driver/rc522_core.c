// SPDX-License-Identifier: GPL-2.0
/*
 * RC522 (MFRC522) core logic - ported from User_Space_test_src/rc522_full.c
 */

#include "rc522.h"
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sched/signal.h>
#include <linux/string.h>

#define dev_write_reg(dev, addr, val)  ((dev)->ops->write_reg((dev)->ctx, (addr), (val)))
#define dev_read_reg(dev, addr, val)   ((dev)->ops->read_reg((dev)->ctx, (addr), (val)))

static void rc522_set_bitmask(struct rc522_dev *dev, u8 reg, u8 mask)
{
	u8 tmp;

	if (dev->ops->read_reg(dev->ctx, reg, &tmp) != 0)
		return;
	dev_write_reg(dev, reg, tmp | mask);
}

static void rc522_clear_bitmask(struct rc522_dev *dev, u8 reg, u8 mask)
{
	u8 tmp;

	if (dev->ops->read_reg(dev->ctx, reg, &tmp) != 0)
		return;
	dev_write_reg(dev, reg, tmp & (u8)(~mask));
}

static void rc522_antenna_on(struct rc522_dev *dev)
{
	u8 temp;

	if (dev->ops->read_reg(dev->ctx, TxControlReg, &temp) != 0)
		return;
	if ((temp & 0x03) != 0x03)
		rc522_set_bitmask(dev, TxControlReg, 0x03);
}

static void rc522_reset(struct rc522_dev *dev, int has_rst_gpio)
{
	if (has_rst_gpio) {
		/* Caller toggles RST GPIO; we only do soft reset here */
	}
	dev_write_reg(dev, CommandReg, PCD_RESETPHASE);
	dev->ops->msleep(50);
}

static void rc522_init_chip(struct rc522_dev *dev, int has_rst_gpio)
{
	rc522_reset(dev, has_rst_gpio);

	dev_write_reg(dev, TModeReg, 0x8D);
	dev_write_reg(dev, TPrescalerReg, 0x3E);
	dev_write_reg(dev, TReloadRegL, 30);
	dev_write_reg(dev, TReloadRegH, 0);
	dev_write_reg(dev, TxAutoReg, 0x40);
	dev_write_reg(dev, ModeReg, 0x3D);
	rc522_antenna_on(dev);
}

static void rc522_calculate_crc(struct rc522_dev *dev, const u8 *data,
				size_t len, u8 *out_crc_low, u8 *out_crc_high)
{
	size_t i;
	u8 n;

	rc522_clear_bitmask(dev, DivIrqReg, 0x04);
	rc522_set_bitmask(dev, FIFOLevelReg, 0x80);

	for (i = 0; i < len; i++)
		dev_write_reg(dev, FIFODataReg, data[i]);

	dev_write_reg(dev, CommandReg, PCD_CALCCRC);

	i = 0xFF;
	while (1) {
		if (dev->ops->read_reg(dev->ctx, DivIrqReg, &n) != 0)
			break;
		i--;
		if (i == 0 || (n & 0x04))
			break;
		dev->ops->usleep(200);
	}

	dev->ops->read_reg(dev->ctx, CRCResultRegL, out_crc_low);
	dev->ops->read_reg(dev->ctx, CRCResultRegM, out_crc_high);
}

static int rc522_to_card(struct rc522_dev *dev, u8 command,
			 const u8 *send_data, size_t send_len,
			 u8 *back_data, size_t *back_bits)
{
	u8 irq_en = 0x00, wait_irq = 0x00, last_bits = 0, n = 0;
	int status = MI_ERR;
	int i;
	u8 fifo_level;
	u8 err;

	if (command == PCD_AUTHENT) {
		irq_en = 0x12;
		wait_irq = 0x10;
	} else if (command == PCD_TRANSCEIVE) {
		irq_en = 0x77;
		wait_irq = 0x30;
	}

	dev_write_reg(dev, CommIEnReg, irq_en | 0x80);
	rc522_clear_bitmask(dev, CommIrqReg, 0x80);
	rc522_set_bitmask(dev, FIFOLevelReg, 0x80);
	dev_write_reg(dev, CommandReg, PCD_IDLE);

	for (i = 0; i < (int)send_len; i++)
		dev_write_reg(dev, FIFODataReg, send_data[i]);

	dev_write_reg(dev, CommandReg, command);

	if (command == PCD_TRANSCEIVE)
		rc522_set_bitmask(dev, BitFramingReg, 0x80);

	i = 2000;
	while (1) {
		if (signal_pending(current))
			return MI_ERR;
		if (dev->ops->read_reg(dev->ctx, CommIrqReg, &n) != 0)
			break;
		i--;
		if (i == 0 || (n & 0x01) || (n & wait_irq))
			break;
		dev->ops->usleep(200);
	}

	rc522_clear_bitmask(dev, BitFramingReg, 0x80);

	if (i != 0) {
		if (dev->ops->read_reg(dev->ctx, ErrorReg, &err) != 0)
			return MI_ERR;
		if ((err & 0x1B) == 0x00) {
			status = MI_OK;
			if (n & irq_en & 0x01)
				status = MI_NOTAGERR;
			if (command == PCD_TRANSCEIVE) {
				dev->ops->read_reg(dev->ctx, FIFOLevelReg, &fifo_level);
				dev->ops->read_reg(dev->ctx, ControlReg, &last_bits);
				last_bits &= 0x07;
				if (last_bits != 0)
					*back_bits = (size_t)((fifo_level - 1) * 8 + last_bits);
				else
					*back_bits = (size_t)(fifo_level * 8);

				if (fifo_level == 0)
					fifo_level = 1;
				if (fifo_level > 16)
					fifo_level = 16;

				if (back_data) {
					for (i = 0; i < fifo_level; i++)
						dev->ops->read_reg(dev->ctx, FIFODataReg, &back_data[i]);
				}
			}
		}
	}
	return status;
}

static int rc522_request(struct rc522_dev *dev, u8 req_mode, u8 *tag_type)
{
	u8 buf[1];
	size_t back_bits = 0;
	int status;

	dev_write_reg(dev, BitFramingReg, 0x07);
	buf[0] = req_mode;
	status = rc522_to_card(dev, PCD_TRANSCEIVE, buf, 1, tag_type, &back_bits);
	if (status != MI_OK || back_bits != 0x10)
		return MI_ERR;
	return status;
}

static int rc522_anticoll(struct rc522_dev *dev, u8 *uid5)
{
	u8 ser_num[2];
	u8 back_data[16];
	size_t back_bits = 0;
	int status, i;
	u8 ser_num_check = 0;

	dev_write_reg(dev, BitFramingReg, 0x00);
	ser_num[0] = PICC_ANTICOLL;
	ser_num[1] = 0x20;

	status = rc522_to_card(dev, PCD_TRANSCEIVE, ser_num, 2, back_data, &back_bits);
	if (status != MI_OK || back_bits != 40)
		return MI_ERR;

	for (i = 0; i < 4; i++)
		ser_num_check ^= back_data[i];
	if (ser_num_check != back_data[4])
		return MI_ERR;

	for (i = 0; i < 5; i++)
		uid5[i] = back_data[i];
	return MI_OK;
}

static int rc522_select_tag(struct rc522_dev *dev, const u8 *uid5)
{
	u8 buf[9];
	u8 back_data[3];
	size_t back_bits = 0;
	int status, i;
	u8 crc_low, crc_high;

	buf[0] = PICC_SELECTTAG;
	buf[1] = 0x70;
	for (i = 0; i < 5; i++)
		buf[2 + i] = uid5[i];
	rc522_calculate_crc(dev, buf, 7, &crc_low, &crc_high);
	buf[7] = crc_low;
	buf[8] = crc_high;

	status = rc522_to_card(dev, PCD_TRANSCEIVE, buf, 9, back_data, &back_bits);
	if (status == MI_OK && back_bits == 0x18)
		return (int)back_data[0];
	return 0;
}

static int rc522_authenticate(struct rc522_dev *dev, u8 auth_mode, u8 block_addr,
			      const u8 *sector_key6, const u8 *uid4)
{
	u8 buff[12];
	int i, status;
	size_t back_bits = 0;
	u8 st2;

	buff[0] = auth_mode;
	buff[1] = block_addr;
	for (i = 0; i < 6; i++)
		buff[2 + i] = sector_key6[i];
	for (i = 0; i < 4; i++)
		buff[8 + i] = uid4[i];

	status = rc522_to_card(dev, PCD_AUTHENT, buff, 12, NULL, &back_bits);
	if (status != MI_OK)
		return MI_ERR;
	if (dev->ops->read_reg(dev->ctx, Status2Reg, &st2) != 0)
		return MI_ERR;
	if ((st2 & 0x08) == 0)
		return MI_ERR;
	return MI_OK;
}

static void rc522_stop_crypto1(struct rc522_dev *dev)
{
	rc522_clear_bitmask(dev, Status2Reg, 0x08);
}

static int rc522_read_block(struct rc522_dev *dev, u8 block_addr, u8 *out_16bytes)
{
	u8 buf[4];
	u8 back_data[18];
	size_t back_bits = 0;
	int status;

	buf[0] = PICC_READ;
	buf[1] = block_addr;
	rc522_calculate_crc(dev, buf, 2, &buf[2], &buf[3]);

	status = rc522_to_card(dev, PCD_TRANSCEIVE, buf, 4, back_data, &back_bits);
	if (status != MI_OK || back_bits < 16 * 8)
		return MI_ERR;
	memcpy(out_16bytes, back_data, 16);
	return MI_OK;
}

static int rc522_write_block(struct rc522_dev *dev, u8 block_addr, const u8 *in_16bytes)
{
	u8 buf[4];
	u8 data_buf[18];
	u8 back_data[18];
	size_t back_bits = 0;
	int status;

	buf[0] = PICC_WRITE;
	buf[1] = block_addr;
	rc522_calculate_crc(dev, buf, 2, &buf[2], &buf[3]);

	status = rc522_to_card(dev, PCD_TRANSCEIVE, buf, 4, back_data, &back_bits);
	if (status != MI_OK || back_bits != 4 || (back_data[0] & 0x0F) != 0x0A)
		return MI_ERR;

	memcpy(data_buf, in_16bytes, 16);
	rc522_calculate_crc(dev, in_16bytes, 16, &data_buf[16], &data_buf[17]);
	status = rc522_to_card(dev, PCD_TRANSCEIVE, data_buf, 18, back_data, &back_bits);
	if (status != MI_OK || back_bits != 4 || (back_data[0] & 0x0F) != 0x0A)
		return MI_ERR;
	return MI_OK;
}

static int rc522_is_valid_trailer(int trailer_block)
{
	return ((trailer_block + 1) % 4) == 0;
}

static u32 rc522_uid_to_num(const u8 uid5[5])
{
	u32 n = 0;
	int i;

	for (i = 0; i < 4; i++)
		n = n * 256U + uid5[i];
	return n;
}

int rc522_core_init(struct rc522_dev *dev)
{
	static const u8 default_key[6] = {
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
	};

	memcpy(dev->default_key, default_key, sizeof(dev->default_key));
	rc522_init_chip(dev, 1);
	return 0;
}

void rc522_core_cleanup(struct rc522_dev *dev)
{
	(void)dev;
}

void rc522_soft_reset(struct rc522_dev *dev)
{
	rc522_init_chip(dev, 0); /* GPIO 토글 없이 소프트 리셋만 */
}

int rc522_read_reg(struct rc522_dev *dev, u8 reg, u8 *val)
{
	return dev->ops->read_reg(dev->ctx, reg, val);
}

int rc522_write_reg(struct rc522_dev *dev, u8 reg, u8 val)
{
	return dev->ops->write_reg(dev->ctx, reg, val);
}

int rc522_read_uid_no_block(struct rc522_dev *dev, u32 *out_uid)
{
	u8 tag_type[2];
	u8 uid5[5];
	int status;

	status = rc522_request(dev, PICC_REQIDL, tag_type);
	if (status != MI_OK)
		return -1;
	status = rc522_anticoll(dev, uid5);
	if (status != MI_OK)
		return -1;
	*out_uid = rc522_uid_to_num(uid5);
	return 0;
}

int rc522_read_uid_blocking(struct rc522_dev *dev, u32 *out_uid)
{
	while (1) {
		if (signal_pending(current))
			return -ERESTARTSYS;  /* Ctrl+C 등으로 read()가 EINTR 반환 */
		if (rc522_read_uid_no_block(dev, out_uid) == 0)
			return 0;
		dev->ops->msleep(50);
	}
}

int rc522_read_text_sector_blocking(struct rc522_dev *dev, int trailer_block,
				    u32 *out_uid, char *out_text, size_t max_len)
{
	u8 uid5[5], uid4[4], key[6], data[16 * 3], tag_type[2];
	int blocks[3];
	int i, status;
	size_t copy_len, j;

	if (!rc522_is_valid_trailer(trailer_block) || max_len == 0)
		return -1;

	blocks[0] = trailer_block - 3;
	blocks[1] = trailer_block - 2;
	blocks[2] = trailer_block - 1;
	memcpy(key, dev->default_key, sizeof(key));

	for (;;) {
		if (signal_pending(current))
			return -ERESTARTSYS;
		status = rc522_request(dev, PICC_REQIDL, tag_type);
		if (status != MI_OK) {
			dev->ops->msleep(50);
			continue;
		}
		status = rc522_anticoll(dev, uid5);
		if (status != MI_OK) {
			dev->ops->msleep(50);
			continue;
		}
		break;
	}

	for (i = 0; i < 4; i++)
		uid4[i] = uid5[i];
	if (out_uid)
		*out_uid = rc522_uid_to_num(uid5);

	rc522_select_tag(dev, uid5);
	status = rc522_authenticate(dev, PICC_AUTHENT1A, (u8)trailer_block, key, uid4);
	if (status != MI_OK) {
		rc522_stop_crypto1(dev);
		return -1;
	}

	memset(data, 0, sizeof(data));
	for (i = 0; i < 3; i++) {
		if (rc522_read_block(dev, (u8)blocks[i], &data[i * 16]) != MI_OK) {
			rc522_stop_crypto1(dev);
			return -1;
		}
	}
	rc522_stop_crypto1(dev);

	copy_len = sizeof(data);
	if (copy_len >= max_len)
		copy_len = max_len - 1;
	for (j = 0; j < copy_len; j++)
		out_text[j] = (char)data[j];
	out_text[copy_len] = '\0';
	return 0;
}

int rc522_write_text_sector_blocking(struct rc522_dev *dev, int trailer_block,
				     const char *text, u32 *out_uid)
{
	u8 uid5[5], uid4[4], key[6], data[16 * 3], tag_type[2];
	int blocks[3];
	int i, status;
	size_t text_len;

	if (!rc522_is_valid_trailer(trailer_block))
		return -1;

	blocks[0] = trailer_block - 3;
	blocks[1] = trailer_block - 2;
	blocks[2] = trailer_block - 1;
	memcpy(key, dev->default_key, sizeof(key));
	memset(data, 0, sizeof(data));

	text_len = strnlen(text, sizeof(data));
	if (text_len > sizeof(data))
		text_len = sizeof(data);
	memcpy(data, text, text_len);

	for (;;) {
		if (signal_pending(current))
			return -ERESTARTSYS;
		status = rc522_request(dev, PICC_REQIDL, tag_type);
		if (status != MI_OK) {
			dev->ops->msleep(50);
			continue;
		}
		status = rc522_anticoll(dev, uid5);
		if (status != MI_OK) {
			dev->ops->msleep(50);
			continue;
		}
		break;
	}

	for (i = 0; i < 4; i++)
		uid4[i] = uid5[i];
	if (out_uid)
		*out_uid = rc522_uid_to_num(uid5);

	rc522_select_tag(dev, uid5);
	status = rc522_authenticate(dev, PICC_AUTHENT1A, (u8)trailer_block, key, uid4);
	if (status != MI_OK) {
		rc522_stop_crypto1(dev);
		return -1;
	}

	for (i = 0; i < 3; i++) {
		if (rc522_write_block(dev, (u8)blocks[i], &data[i * 16]) != MI_OK) {
			rc522_stop_crypto1(dev);
			return -1;
		}
	}
	rc522_stop_crypto1(dev);
	return 0;
}
