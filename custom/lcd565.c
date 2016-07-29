/*
 * lcd565.c -- Raspberry Pi panel control device for DPI attached LCD.
 *
 * Copyright 2016 mincepi
 *
 * https://sites.google.com/site/mincepi/palmpi
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the Linux kernel source for more details.
 *
 * Based on my pi2hd44780.c modules
 *
 *
 * Add dtoverlay=lcd565 to /boot/config.txt and copy lcd565-overlay to /boot/overlays.
 *
 * Copy this module to correct /lib/modules directory and run depmod.
 *
 *
 * This module creates device /dev/lcd565.
 *
 * Echo "panel on" to /dev/lcd565 to enable panel.
 *
 * Echo "panel off" to /dev/lcd565 to disable panel.
 *
 * Echo "backlight on" to /dev/lcd565 to turn backlight on.
 *
 * Echo "backlight off" to /dev/lcd565 to turn backlight off.
 *
 * Echo "lowbits on" to /dev/lcd565 to set RGB low bits low.
 *
 * Echo "lowbits off" to /dev/lcd565 to set RGB low bits high.
 *
 * On module load panel and backlight are on, lowbits are low.
 *
 *
 * You don't need to do anything special to compile this module.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/input.h>
#include <linux/gpio.h>
#include <asm/uaccess.h>

/* device stuff */
static dev_t Device;
static struct class* Class;
static struct cdev c_dev;

/* device tree stuff */
static const struct of_device_id lcd565_dt_ids[] = {
	{ .compatible = "lcd565" },
	{},
};

MODULE_DEVICE_TABLE(of, lcd565_dt_ids);

static struct platform_driver lcd565_driver = {
	.driver = {
		.name	= "lcd565",
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(lcd565_dt_ids),
	},
};

/* process device write */
static ssize_t write(struct file *filp, const char *buffer, size_t length, loff_t * offset)
{
    static char command[20];

    if ((length < 20) && (copy_from_user(command, buffer, length) == 0)) {
	if ((length >= 8) && (strncmp(command, "panel on", 8) == 0)) {
		gpio_set_value(18, 1);
		return length;
	}

	if ((length >= 9) && (strncmp(command, "panel off", 9) == 0)) {
		gpio_set_value(18, 0);
		return length;
	}

	if ((length >= 10) && (strncmp(command, "lowbits on", 10) == 0)) {
		gpio_set_value(25, 1);
		return length;
	}

	if ((length >= 11) && (strncmp(command, "lowbits off", 11) == 0)) {
		gpio_set_value(25, 0);
		return length;
	}

	if ((length >= 12) && (strncmp(command, "backlight on", 12) == 0)) {
		gpio_set_value(26, 1);
		return length;
	}

	if ((length >= 13) && (strncmp(command, "backlight off", 13) == 0)) {
		gpio_set_value(26, 0);
		return length;
	}
    }

    return -1;
}

/* file operations struct */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .write = write,
};

/* set device permissions to something useful */
static int uevent(struct device *dev, struct kobj_uevent_env *env)
{
    add_uevent_var(env, "DEVMODE=%#o", 0766);
    return 0;
}

/* set up */
static int __init lcd565_init(void)
{
    /* set gpios to output and set initial conditions */
    gpio_request(18, "sysfs");
    gpio_direction_output(18, 1);
    gpio_export(18, false);
    gpio_request(25, "sysfs");
    gpio_direction_output(25, 0);
    gpio_export(25, false);
    gpio_request(26, "sysfs");
    gpio_direction_output(26, 1);
    gpio_export(26, false);

    /* set up device node and register device*/
    alloc_chrdev_region(&Device, 0, 1, "lcd565");
    Class = class_create(THIS_MODULE, "lcd565");
    Class->dev_uevent = uevent;
    device_create(Class, NULL, MKDEV(MAJOR(Device), MINOR(Device)), NULL, "lcd565");
    cdev_init(&c_dev, &fops);
    cdev_add(&c_dev, Device, 1);
    printk(KERN_INFO "lcd565: loaded\n");
    return platform_driver_register(&lcd565_driver);
}

/* tear down */
static void __exit lcd565_exit(void)
{
    /*set gpios back to input*/
    gpio_set_value(18, 0);
    gpio_unexport(18);
    gpio_free(18);
    gpio_set_value(25, 0);
    gpio_unexport(25);
    gpio_free(25);
    gpio_set_value(26, 0);
    gpio_unexport(26);
    gpio_free(26);

    /* remove node and unregister device */
    device_destroy(Class, MKDEV(MAJOR(Device), MINOR(Device)));
    class_destroy(Class);
    unregister_chrdev_region(Device, 1);
    cdev_del(&c_dev);
    platform_driver_unregister(&lcd565_driver);
    printk(KERN_INFO "lcd565: unloaded\n");
    return;
}

module_init(lcd565_init);
module_exit(lcd565_exit);

MODULE_LICENSE("GPL");
