// SPDX-License-Identifier: GPL-2.0
/*
 * RC522 character device - /dev/rc522 (read UID, ioctl API)
 */

#include "rc522.h"
#include "rc522_ioctl.h"
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#define RC522_DEV_NAME "rc522"

static DEFINE_MUTEX(rc522_chardev_lock);
static struct rc522_dev *rc522_chardev_chip;
static int rc522_chardev_refcnt;

static int rc522_chardev_open(struct inode *inode, struct file *filp)
{
	(void)inode;
	mutex_lock(&rc522_chardev_lock);
	if (!rc522_chardev_chip) {
		mutex_unlock(&rc522_chardev_lock);
		return -ENODEV;
	}
	filp->private_data = rc522_chardev_chip;
	rc522_chardev_refcnt++;
	mutex_unlock(&rc522_chardev_lock);
	return 0;
}

static int rc522_chardev_release(struct inode *inode, struct file *filp)
{
	(void)inode;
	(void)filp;
	mutex_lock(&rc522_chardev_lock);
	rc522_chardev_refcnt--;
	mutex_unlock(&rc522_chardev_lock);
	return 0;
}

static ssize_t rc522_chardev_read(struct file *filp, char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct rc522_dev *dev = filp->private_data;
	u32 uid;
	int ret;

	if (count < 4)
		return -EINVAL;

	ret = rc522_read_uid_blocking(dev, &uid);
	if (ret)
		return ret;

	if (copy_to_user(buf, &uid, 4))
		return -EFAULT;
	*ppos += 4;
	return 4;
}

static long rc522_chardev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct rc522_dev *dev = filp->private_data;
	void __user *uarg = (void __user *)arg;
	int ret;

	switch (cmd) {
	case RC522_RESET:
		rc522_soft_reset(dev);
		return 0;

	case RC522_READ_CARD: {
		u32 uid;

		ret = rc522_read_uid_blocking(dev, &uid);
		if (ret)
			return ret;
		if (copy_to_user(uarg, &uid, sizeof(uid)))
			return -EFAULT;
		return 0;
	}

	case RC522_READ_REG: {
		struct rc522_reg_data d;

		if (copy_from_user(&d, uarg, sizeof(d)))
			return -EFAULT;
		ret = rc522_read_reg(dev, d.reg, &d.val);
		if (ret)
			return ret;
		if (copy_to_user(uarg, &d, sizeof(d)))
			return -EFAULT;
		return 0;
	}

	case RC522_WRITE_REG: {
		struct rc522_reg_data d;

		if (copy_from_user(&d, uarg, sizeof(d)))
			return -EFAULT;
		return rc522_write_reg(dev, d.reg, d.val);
	}

	case RC522_READ_TEXT_SECTOR: {
		struct rc522_read_text d;

		if (copy_from_user(&d, uarg, sizeof(d)))
			return -EFAULT;
		ret = rc522_read_text_sector_blocking(dev, d.trailer_block,
						      &d.uid, d.text, sizeof(d.text));
		if (ret)
			return ret;
		if (copy_to_user(uarg, &d, sizeof(d)))
			return -EFAULT;
		return 0;
	}

	default:
		return -ENOTTY;
	}
}

static const struct file_operations rc522_chardev_fops = {
	.owner          = THIS_MODULE,
	.open           = rc522_chardev_open,
	.release        = rc522_chardev_release,
	.read           = rc522_chardev_read,
	.unlocked_ioctl = rc522_chardev_ioctl,
};

static struct miscdevice rc522_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = RC522_DEV_NAME,
	.fops  = &rc522_chardev_fops,
};

int rc522_chardev_register(struct device *parent, struct rc522_dev *chip)
{
	int ret;

	mutex_lock(&rc522_chardev_lock);
	if (rc522_chardev_chip) {
		mutex_unlock(&rc522_chardev_lock);
		return -EBUSY;
	}
	rc522_chardev_chip = chip;
	rc522_miscdev.parent = parent;
	ret = misc_register(&rc522_miscdev);
	if (ret)
		rc522_chardev_chip = NULL;
	mutex_unlock(&rc522_chardev_lock);
	return ret;
}

void rc522_chardev_unregister(struct rc522_dev *chip)
{
	mutex_lock(&rc522_chardev_lock);
	if (rc522_chardev_chip == chip) {
		misc_deregister(&rc522_miscdev);
		rc522_chardev_chip = NULL;
	}
	mutex_unlock(&rc522_chardev_lock);
}
