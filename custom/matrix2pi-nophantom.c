/*
 * matrix2pi.c -- Raspberry Pi generic switch matrix device driver.
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
 * N-key rollover is implemented but phantom key lockout is not.
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
 * Add dtoverlay=matrix2pi to /boot/config.txt and copy matrix2pi-overlay.dtb to /boot/overlays.
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

/* next two lines set row and column gpio numbers */
static unsigned row[] = {4, 6, 9, 11, 14, 17, 22, 26};
static unsigned column[] = {1, 5, 7, 8, 10, 12, 13, 15, 16, 18, 20, 21, 23, 24, 25, 27};

static unsigned rows = sizeof(row)/sizeof(row[0]);
static unsigned columns = sizeof(column)/sizeof(column[0]);
static unsigned previous[sizeof(column)/sizeof(column[0])] = {0};
static unsigned ringbuf[sizeof(column)/sizeof(column[0])][8];
static unsigned ringptr = 0;
static unsigned scan = 0;
static unsigned rowmask = 0;
volatile unsigned *(gpio);
static struct hrtimer timer;
static struct input_dev *matrix2pi;
struct device;

/* device tree stuff */
static const struct of_device_id matrix2pi_dt_ids[] = {
	{ .compatible = "matrix2pi" },
	{},
};

MODULE_DEVICE_TABLE(of, matrix2pi_dt_ids);

static struct platform_driver matrix2pi_driver = {
	.driver = {
		.name	= "matrix2pi",
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(matrix2pi_dt_ids),
	},
};

/* model m scancode to linux keycode translation table */
static unsigned char translate[128] = {
/* 00 */	KEY_BACKSPACE,	KEY_ENTER,	KEY_RESERVED,	KEY_BACKSLASH,	KEY_F5,		KEY_F9,		KEY_F10,	KEY_SPACE,
/* 08 */	KEY_T,		KEY_V,		KEY_R,		KEY_F,		KEY_G,		KEY_5,		KEY_4,		KEY_B,
/* 10 */	KEY_Y,		KEY_M,		KEY_U,		KEY_J,		KEY_H,		KEY_6,		KEY_7,		KEY_N,
/* 18 */	KEY_RIGHTBRACE,	KEY_COMMA,	KEY_I,		KEY_K,		KEY_F6,		KEY_EQUAL,	KEY_8,		KEY_RESERVED,
/* 20 */	KEY_LEFTBRACE,	KEY_RESERVED,	KEY_P,		KEY_SEMICOLON,	KEY_APOSTROPHE,	KEY_MINUS,	KEY_0,		KEY_SLASH,
/* 28 */	KEY_F3,		KEY_C,		KEY_E,		KEY_D,		KEY_F4,		KEY_F2,		KEY_3,		KEY_RESERVED,
/* 30 */	KEY_CAPSLOCK,	KEY_X,		KEY_W,		KEY_S,		KEY_RESERVED,	KEY_F1,		KEY_2,		KEY_RESERVED,
/* 38 */	KEY_RESERVED,	KEY_RESERVED,	KEY_SCROLLLOCK,	KEY_RESERVED,	KEY_LEFTALT,	KEY_RESERVED,	KEY_SYSRQ,	KEY_RIGHTALT,
/* 40 */	KEY_TAB,	KEY_Z,		KEY_Q,		KEY_A,		KEY_ESC,	KEY_GRAVE,	KEY_1,		KEY_RESERVED,
/* 48 */	KEY_RESERVED,	KEY_PAUSE,	KEY_KPPLUS,	KEY_KPENTER,	KEY_UP,		KEY_HOME,	KEY_END,	KEY_LEFT,
/* 50 */	KEY_LEFTSHIFT,	KEY_RIGHTSHIFT,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/* 58 */	KEY_RESERVED,	KEY_RIGHTCTRL,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_LEFTCTRL,	KEY_RESERVED,	KEY_RESERVED,
/* 60 */	KEY_KP5,	KEY_KPSLASH,	KEY_KP8,	KEY_KP2,	KEY_KP0,	KEY_INSERT,	KEY_F12,	KEY_RIGHT,
/* 68 */	KEY_KP4,	KEY_NUMLOCK,	KEY_KP7,	KEY_KP1,	KEY_RESERVED,	KEY_DELETE,	KEY_F11,	KEY_DOWN,
/* 70 */	KEY_F7,		KEY_DOT,	KEY_O,		KEY_L,		KEY_RESERVED,	KEY_F8,		KEY_9,		KEY_RESERVED,
/* 78 */	KEY_KP6,	KEY_KPASTERISK,	KEY_KP9,	KEY_KP3,	KEY_KPDOT,	KEY_PAGEUP,	KEY_PAGEDOWN,	KEY_KPMINUS,
};

/* handle timer interrupt and output any valid keys */
enum hrtimer_restart keycheck(struct hrtimer *timer)
{
    static unsigned pressed, released, i;

    pressed = rowmask;
    released = 0;
    ringbuf[scan][ringptr] = ioread32(gpio) & rowmask;

    for (i = 0; i < 8; i++) {
	pressed &= ringbuf[scan][i];
	released |= ringbuf[scan][i];
    }
//phantom lockout here




    pressed = pressed & ~previous[scan];
    released = ~released & previous[scan];

    if (pressed != 0) {
        for (i = 0; i < rows; i++) {
	    if ((pressed & (1 << row[i])) > 0) {
		/* normal output: comment out when using debug output */
		input_report_key(matrix2pi, translate[(scan * rows) + i], 1);
		/* debug output: uncomment to get raw scan codes*/
//		printk(KERN_EMERG "key scancode is %x\n", (scan * rows) + i);
	    }
	}
	input_sync(matrix2pi);
    }

    if (released != 0) {
        for (i = 0; i < rows; i++) {
	    if ((released & (1 << row[i])) > 0) {
		/* normal output: comment out when using debug output */
		input_report_key(matrix2pi, translate[(scan * rows) + i], 0);
	    }
	}
	input_sync(matrix2pi);
    }

    previous[scan] = (previous[scan] | pressed) & ~released;
    gpio_direction_input(column[scan]);
    scan++;

    if (scan >= columns) {
	scan = 0;
	ringptr++;
	if (ringptr >= 8) ringptr = 0;
    }

    gpio_direction_output(column[scan], 1);

    /* do again in a bit, so that debounce time works out to 32 milliseconds */
    hrtimer_forward(timer, hrtimer_cb_get_time(timer), ktime_set(0, 4000000 / columns));
    return HRTIMER_RESTART;
}

/* set up */
static int __init matrix2pi_init(void)
{
    static int i, j, retval;

    /* set initial gpio values */
    for (i = 0; i < columns; i++) gpio_request(column[i], "sysfs");
    for (i = 0; i < rows; i++) gpio_request(row[i], "sysfs");
    gpio_direction_output(column[0], 1);

    /* calculate row mask */
    for (i = 0; i < rows; i++) {
	rowmask |= 1 << (row[i]);
    }

    /* clear ring buffer */
    for (i = 0; i < columns; i++) {
	for (j = 0; j < 8; j++) {
	    ringbuf[i][j] = 0;
	}
    }

    gpio = ioremap(0x20200034, 4);
    matrix2pi=input_allocate_device();
    matrix2pi->name = "matrix2pi";
    matrix2pi->phys = "matrix2pi/input0";
    matrix2pi->id.bustype = BUS_HOST;
    matrix2pi->id.vendor = 0x0001;
    matrix2pi->id.product = 0x0001;
    matrix2pi->id.version = 0x0100;
    matrix2pi->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REP);
    matrix2pi->keycode = translate;
    matrix2pi->keycodesize = sizeof(unsigned char);
    matrix2pi->keycodemax = ARRAY_SIZE(translate);
    for (i = 1; i < 0x256; i++) set_bit(i,matrix2pi->keybit);
    retval = input_register_device(matrix2pi);
//    input_report_key(matrix2pi,KEY_NUMLOCK,1);	/* numlock on if you like it that way */
    input_sync(matrix2pi);

    /* set up timer */
    hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    timer.function = &keycheck;
    hrtimer_start(&timer, ktime_set(0,4000000 / columns), HRTIMER_MODE_REL);
    printk(KERN_INFO "matrix2pi: loaded\n");
    return platform_driver_register(&matrix2pi_driver);
}

/* tear down */
static void __exit matrix2pi_exit(void)
{
    static int i;

    /* stop timer */
    hrtimer_cancel(&timer);
    iounmap(gpio);

    for (i = 0; i < columns; i++) {
	gpio_direction_input(column[i]);
	gpio_free(column[i]);
    }

    for (i = 0; i < rows; i++) 	gpio_free(row[i]);
    input_unregister_device(matrix2pi);
    platform_driver_unregister(&matrix2pi_driver);
    printk(KERN_INFO "matrix2pi: unloaded\n");
    return;
}

module_init(matrix2pi_init);
module_exit(matrix2pi_exit);

MODULE_LICENSE("GPL");
