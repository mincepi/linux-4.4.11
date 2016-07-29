/*
 * knob.c -- Raspberry Pi generic switch matrix device driver.
 *
 * Copyright 2016 mincepi
 *
 * https://sites.google.com/site/mincepi/m2pi
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the Linux kernel source for more details.
 *
 * The following resources helped me greatly:
 *
 * The Linux kernel module programming howto
 * Copyright (C) 2001 Jay Salzman
 *
 * The Linux USB input subsystem Part 1
 * Copyright (C) 2007 Brad Hards
 *
 * My favorite software debouncers
 * Copyright (C) 2004 Jack Ganssle www.embedded.com
 *
 * kbd FAQ
 * Copyright (C) 2009 Andries Brouwer
 *
 *
 * This code is (hopefully) generic enough to drive any dimension switch matrix.
 *
 * To do so you must change the row and column array contents to designate which GPIOs are used
 * and change the translation table contents. You must also edit and recompile the device tree overlay.
 *
 * Code ensures that switches are stable for 32 milliseconds before they're state is considered valid.
 * This should debounce most types of switches.
 *
 * N-key rollover and phantom key lockout is implemented.
 *
 * You don't need to do anything special to compile this module.
 *
 *
 * Row inputs must be pulled down to ground with 22K resistors.
 *
 * Disable the serial console, I2C and SPI using raspi-config if you're using those GPIOs.
 *
 * Don't use GPIO 2 and 3 for row inputs. They have strond pullups that can't be disabled.
 *
 * Add dtoverlay=knob to /boot/config.txt and copy knob-overlay.dtb to /boot/overlays.
 *
 * Copy the compiled module to the proper /lib/modules directory and run depmod.
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/of_platform.h>
#include <linux/gpio.h>
#include <linux/hrtimer.h>

#define BASE		BCM2708_PERI_BASE

static unsigned ringbuf[8];
static unsigned ringptr = 0;
static unsigned direction = 0;
static unsigned mask = (1 << 2) | (1 << 3);
volatile unsigned *(gpio);
static struct hrtimer timer;
static struct input_dev *knob;
struct device;

/* device tree stuff */
static const struct of_device_id knob_dt_ids[] = {
	{ .compatible = "knob" },
	{},
};

MODULE_DEVICE_TABLE(of, knob_dt_ids);

static struct platform_driver knob_driver = {
	.driver = {
		.name	= "knob",
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(knob_dt_ids),
	},
};


/* handle timer interrupt and output any valid keys */
enum hrtimer_restart keycheck(struct hrtimer *timer)
{
    static unsigned key, i;

    /* put current switch state in ring buffer */
    ringbuf[ringptr] = (ioread32(gpio)) & mask;
    ringptr++;
    ringptr &= 7;

    if (direction == 0) {
	/* check for any down */
	key = 0;
        for (i = 0; i < 8; i++) key |= ringbuf[i];
	switch (key) {
	    case 4:
		direction = 1;
		input_report_key(knob, KEY_A, 1);
		input_sync(knob);
		break;

	    case 8:
		direction = -1;
		input_report_key(knob, KEY_B, 1);
		input_sync(knob);
		break;
	}

    } else {
	/* check for all up */
	key = mask;
        for (i = 0; i < 8; i++) key &= ringbuf[i];
	    if (key == mask) {
		if (direction == 1) input_report_key(knob, KEY_A, 0); else input_report_key(knob, KEY_B, 0);
		direction = 0;
	    }
    }

    /* do again in a bit, so that debounce time works out to 8 milliseconds */
    hrtimer_forward(timer, hrtimer_cb_get_time(timer), ktime_set(0, 1000000));
    return HRTIMER_RESTART;
}

/* set up */
static int __init knob_init(void)
{
    static int i, retval;

    /* set initial gpio values */
    gpio_request(2, "sysfs");
    gpio_request(3, "sysfs");

    /* clear ring buffer */
    for (i = 0; i < 8; i++) ringbuf[i] = mask;
    ringptr = 0;
    direction = 0;

    /* set up input device */
    gpio = ioremap(0x20200034, 4);
    knob=input_allocate_device();
    knob->name = "knob";
    knob->phys = "knob/input0";
    knob->id.bustype = BUS_HOST;
    knob->id.vendor = 0x0001;
    knob->id.product = 0x0001;
    knob->id.version = 0x0100;
    knob->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REP);
    knob->keycode = 0;
    knob->keycodesize = sizeof(unsigned char);
    knob->keycodemax = 256;
    for (i = 1; i < 0x256; i++) set_bit(i,knob->keybit);
    retval = input_register_device(knob);
    input_sync(knob);

    /* set up timer */
    hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    timer.function = &keycheck;
    hrtimer_start(&timer, ktime_set(0, 1000000), HRTIMER_MODE_REL);
    printk(KERN_INFO "knob: loaded\n");
    return platform_driver_register(&knob_driver);
}

/* tear down */
static void __exit knob_exit(void)
{
    /* stop timer */
    hrtimer_cancel(&timer);
    iounmap(gpio);
    /* free up everything */
    gpio_free(2);
    gpio_free(3);
    input_unregister_device(knob);
    platform_driver_unregister(&knob_driver);
    printk(KERN_INFO "knob: unloaded\n");
    return;
}

module_init(knob_init);
module_exit(knob_exit);

MODULE_LICENSE("GPL");
