/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RC522 (MFRC522) RFID kernel driver - internal definitions
 * Based on User_Space_test_src/rc522_full.c, rc522_full.h
 */

#ifndef _RC522_H_
#define _RC522_H_

#include <linux/types.h>
#include <linux/wait.h>
#include <linux/atomic.h>

/* PCD Commands */
#define PCD_IDLE        0x00
#define PCD_AUTHENT     0x0E
#define PCD_RECEIVE     0x08
#define PCD_TRANSMIT    0x04
#define PCD_TRANSCEIVE  0x0C
#define PCD_RESETPHASE  0x0F
#define PCD_CALCCRC     0x03

/* PICC Commands */
#define PICC_REQIDL     0x26
#define PICC_REQALL     0x52
#define PICC_ANTICOLL   0x93
#define PICC_SELECTTAG  0x93
#define PICC_AUTHENT1A  0x60
#define PICC_AUTHENT1B  0x61
#define PICC_READ       0x30
#define PICC_WRITE      0xA0
#define PICC_HALT       0x50

#define MI_OK       0
#define MI_NOTAGERR 1
#define MI_ERR      2

/* MFRC522 Registers */
#define CommandReg        0x01
#define CommIEnReg        0x02
#define DivlEnReg         0x03
#define CommIrqReg        0x04
#define DivIrqReg         0x05
#define ErrorReg          0x06
#define Status1Reg        0x07
#define Status2Reg        0x08
#define FIFODataReg       0x09
#define FIFOLevelReg      0x0A
#define WaterLevelReg     0x0B
#define ControlReg        0x0C
#define BitFramingReg     0x0D
#define CollReg           0x0E

#define ModeReg           0x11
#define TxModeReg         0x12
#define RxModeReg         0x13
#define TxControlReg      0x14
#define TxAutoReg         0x15
#define TxSelReg          0x16
#define RxSelReg          0x17
#define RxThresholdReg    0x18
#define DemodReg          0x19

#define TModeReg          0x2A
#define TPrescalerReg     0x2B
#define TReloadRegH       0x2C
#define TReloadRegL       0x2D
#define TCounterValueRegH 0x2E
#define TCounterValueRegL 0x2F

#define CRCResultRegM     0x21
#define CRCResultRegL     0x22

#define RFCfgReg          0x26

#define RC522_DEFAULT_KEY_LEN 6
#define RC522_UID_LEN         5
#define RC522_BLOCK_SIZE      16
#define RC522_SECTOR_DATA_BLOCKS 3
#define RC522_SECTOR_DATA_LEN  (RC522_BLOCK_SIZE * RC522_SECTOR_DATA_BLOCKS)

struct rc522_ops {
	int (*write_reg)(void *ctx, u8 addr, u8 val);
	int (*read_reg)(void *ctx, u8 addr, u8 *val);
	void (*msleep)(unsigned int msecs);
	void (*usleep)(unsigned int usecs);
};

struct rc522_dev {
	void *ctx;
	const struct rc522_ops *ops;
	u8 default_key[RC522_DEFAULT_KEY_LEN];

	/* IRQ 기반 모드용 상태 (폴백 모드에서도 있어도 무방) */
	wait_queue_head_t waitq;   /* 블로킹 read/ioctl 대기용 */
	atomic_t          irq_event; /* IRQ 스레드가 이벤트 발생 시 1로 set */
};

int rc522_core_init(struct rc522_dev *dev);
void rc522_core_cleanup(struct rc522_dev *dev);
void rc522_soft_reset(struct rc522_dev *dev);
int rc522_read_reg(struct rc522_dev *dev, u8 reg, u8 *val);
int rc522_write_reg(struct rc522_dev *dev, u8 reg, u8 val);
int rc522_read_uid_blocking(struct rc522_dev *dev, u32 *out_uid);
int rc522_read_uid_no_block(struct rc522_dev *dev, u32 *out_uid);
int rc522_read_text_sector_blocking(struct rc522_dev *dev, int trailer_block,
				    u32 *out_uid, char *out_text, size_t max_len);
int rc522_write_text_sector_blocking(struct rc522_dev *dev, int trailer_block,
				     const char *text, u32 *out_uid);

/* Chardev (called from rc522_spi) */
struct device;
int rc522_chardev_register(struct device *parent, struct rc522_dev *chip);
void rc522_chardev_unregister(struct rc522_dev *chip);

#endif /* _RC522_H_ */
