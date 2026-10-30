// SPDX-License-Identifier: GPL-2.0-only
/*
 * Normal-world resources for Goodix fingerprint sensors managed by a TEE.
 * Reset timing follows Goodix's downstream gf_spi / goodix_fod drivers:
 * Copyright (c) 2016-2017 Goodix. Copyright (c) 2022 Xiaomi.
 * Imaging, templates and matching remain in the trusted application.
 */

#include <linux/atomic.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/poll.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <uapi/linux/goodix-fingerprint.h>

struct goodix_fp {
	struct device *dev;
	struct miscdevice misc;
	struct gpio_desc *reset;
	struct regulator *vdd;
	struct mutex lock; /* Serialize open, resource changes and removal. */
	struct kref ref;
	wait_queue_head_t wait;
	atomic_t pending;
	const char *firmware_name;
	int irq;
	bool opened;
	bool irq_enabled;
	bool wake_enabled;
	bool removed;
};

static void goodix_fp_free(struct kref *ref)
{
	struct goodix_fp *fp = container_of(ref, struct goodix_fp, ref);

	mutex_destroy(&fp->lock);
	kfree(fp);
}

static irqreturn_t goodix_fp_irq(int irq, void *data)
{
	struct goodix_fp *fp = data;

	atomic_inc(&fp->pending);
	pm_wakeup_event(fp->dev, 2000);
	wake_up_interruptible_poll(&fp->wait, EPOLLIN | EPOLLRDNORM);

	return IRQ_HANDLED;
}

/* The caller holds lock. The IRQ handler never takes it. */
static void goodix_fp_disable_irq(struct goodix_fp *fp)
{
	if (fp->wake_enabled) {
		disable_irq_wake(fp->irq);
		fp->wake_enabled = false;
	}
	if (fp->irq_enabled) {
		disable_irq(fp->irq);
		fp->irq_enabled = false;
	}
}

static void goodix_fp_power_off(struct goodix_fp *fp)
{
	goodix_fp_disable_irq(fp);
	gpiod_set_value_cansleep(fp->reset, 1);
	regulator_disable(fp->vdd);
	atomic_set(&fp->pending, 0);
}

static int goodix_fp_open(struct inode *inode, struct file *file)
{
	struct goodix_fp *fp = container_of(file->private_data,
					  struct goodix_fp, misc);
	int ret;

	guard(mutex)(&fp->lock);
	if (fp->removed)
		return -ENODEV;
	if (fp->opened)
		return -EBUSY;

	ret = regulator_enable(fp->vdd);
	if (ret)
		return ret;
	usleep_range(10000, 11000);
	atomic_set(&fp->pending, 0);
	fp->opened = true;
	kref_get(&fp->ref);
	file->private_data = fp;

	return nonseekable_open(inode, file);
}

static int goodix_fp_release(struct inode *inode, struct file *file)
{
	struct goodix_fp *fp = file->private_data;

	scoped_guard(mutex, &fp->lock) {
		if (!fp->removed)
			goodix_fp_power_off(fp);
		fp->opened = false;
	}
	kref_put(&fp->ref, goodix_fp_free);

	return 0;
}

static long goodix_fp_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct goodix_fp *fp = file->private_data;
	int ret;

	guard(mutex)(&fp->lock);
	if (fp->removed)
		return -ENODEV;

	switch (cmd) {
	case GOODIX_FP_IOC_RESET:
		/* Clear IRQ state before pulsing active-low reset. */
		goodix_fp_disable_irq(fp);
		gpiod_set_value_cansleep(fp->reset, 1);
		usleep_range(3000, 4000);
		gpiod_set_value_cansleep(fp->reset, 0);
		usleep_range(3000, 4000);
		atomic_set(&fp->pending, 0);
		return 0;
	case GOODIX_FP_IOC_ENABLE_IRQ:
		if (fp->irq_enabled)
			return 0;
		if (device_may_wakeup(fp->dev)) {
			ret = enable_irq_wake(fp->irq);
			if (ret)
				return ret;
			fp->wake_enabled = true;
		}
		fp->irq_enabled = true;
		enable_irq(fp->irq);
		return 0;
	case GOODIX_FP_IOC_DISABLE_IRQ:
		goodix_fp_disable_irq(fp);
		return 0;
	default:
		return -ENOTTY;
	}
}

static ssize_t goodix_fp_read(struct file *file, char __user *buf,
			      size_t count, loff_t *pos)
{
	struct goodix_fp *fp = file->private_data;
	struct goodix_fp_event event = {};
	int pending, ret;

	if (count < sizeof(event))
		return -EINVAL;
	for (;;) {
		if (READ_ONCE(fp->removed))
			return -ENODEV;
		pending = atomic_xchg(&fp->pending, 0);
		if (pending)
			break;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(fp->wait,
					       atomic_read(&fp->pending) ||
					       READ_ONCE(fp->removed));
		if (ret)
			return ret;
	}

	event.irq_count = pending;
	if (copy_to_user(buf, &event, sizeof(event))) {
		atomic_add(pending, &fp->pending);
		return -EFAULT;
	}

	return sizeof(event);
}

static __poll_t goodix_fp_poll(struct file *file, poll_table *wait)
{
	struct goodix_fp *fp = file->private_data;

	poll_wait(file, &fp->wait, wait);
	if (READ_ONCE(fp->removed))
		return EPOLLHUP | EPOLLERR;
	if (atomic_read(&fp->pending))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static const struct file_operations goodix_fp_fops = {
	.owner = THIS_MODULE,
	.open = goodix_fp_open,
	.release = goodix_fp_release,
	.unlocked_ioctl = goodix_fp_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.read = goodix_fp_read,
	.poll = goodix_fp_poll,
};

static ssize_t firmware_name_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct goodix_fp *fp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", fp->firmware_name);
}
static DEVICE_ATTR_RO(firmware_name);

static struct attribute *goodix_fp_attrs[] = {
	&dev_attr_firmware_name.attr,
	NULL,
};
ATTRIBUTE_GROUPS(goodix_fp);

static int goodix_fp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct goodix_fp *fp;
	int ret;

	fp = kzalloc_obj(*fp);
	if (!fp)
		return -ENOMEM;
	fp->dev = dev;
	mutex_init(&fp->lock);
	kref_init(&fp->ref);
	init_waitqueue_head(&fp->wait);
	atomic_set(&fp->pending, 0);
	platform_set_drvdata(pdev, fp);

	fp->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(fp->reset)) {
		ret = PTR_ERR(fp->reset);
		goto err_put;
	}
	fp->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(fp->vdd)) {
		ret = PTR_ERR(fp->vdd);
		goto err_put;
	}
	fp->irq = platform_get_irq(pdev, 0);
	if (fp->irq < 0) {
		ret = fp->irq;
		goto err_put;
	}
	ret = device_property_read_string(dev, "firmware-name", &fp->firmware_name);
	if (ret)
		goto err_put;

	ret = devm_request_threaded_irq(dev, fp->irq, NULL, goodix_fp_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), fp);
	if (ret)
		goto err_put;
	ret = device_init_wakeup(dev, device_property_read_bool(dev, "wakeup-source"));
	if (ret)
		goto err_irq;

	fp->misc.minor = MISC_DYNAMIC_MINOR;
	fp->misc.name = "goodix_fp";
	fp->misc.fops = &goodix_fp_fops;
	fp->misc.parent = dev;
	fp->misc.mode = 0600;
	ret = misc_register(&fp->misc);
	if (ret) {
		device_init_wakeup(dev, false);
		goto err_irq;
	}
	return 0;

err_irq:
	devm_free_irq(dev, fp->irq, fp);
err_put:
	kref_put(&fp->ref, goodix_fp_free);
	return dev_err_probe(dev, ret, "failed to register fingerprint sensor\n");
}

static void goodix_fp_remove(struct platform_device *pdev)
{
	struct goodix_fp *fp = platform_get_drvdata(pdev);

	misc_deregister(&fp->misc);
	scoped_guard(mutex, &fp->lock) {
		if (fp->opened)
			goodix_fp_power_off(fp);
		WRITE_ONCE(fp->removed, true);
	}
	wake_up_interruptible_poll(&fp->wait, EPOLLHUP | EPOLLERR);
	device_init_wakeup(&pdev->dev, false);
	/* Free the IRQ before the last reference can release its handler data. */
	devm_free_irq(&pdev->dev, fp->irq, fp);
	kref_put(&fp->ref, goodix_fp_free);
}

static const struct of_device_id goodix_fp_of_match[] = {
	{ .compatible = "xiaomi,vermeer-fingerprint" },
	{}
};
MODULE_DEVICE_TABLE(of, goodix_fp_of_match);

static struct platform_driver goodix_fp_driver = {
	.probe = goodix_fp_probe,
	.remove = goodix_fp_remove,
	.driver = {
		.name = "goodix-fingerprint",
		.of_match_table = goodix_fp_of_match,
		.dev_groups = goodix_fp_groups,
	},
};
module_platform_driver(goodix_fp_driver);

MODULE_DESCRIPTION("Goodix TEE fingerprint sensor resource driver");
MODULE_LICENSE("GPL");
