/*
 * ps2pi.c -- Raspberry Pi PS/2 keyboard device driver using the UART.
 *
 * Copyright 2014 mincepi
 *
 * https://sites.google.com/site/mincepi/ps2pi
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
 * PS/2 keyboard interfacing
 * Copyright (C) 1998-2013 Adam Chapweske
 *
 * kbd FAQ
 * Copyright (C) 2009 Andries Brouwer
 *
 * Keyboard data and clock lines must be pulled up to +5V with 4.7K resistors.
 * Data line must be limited to 3.0V, so connect a blue or white LED between
 * the clock line and pin 10 (RX) on the Raspberry Pi.
 * Clock line is not connected to the Pi.
 *
 * The keyboard also needs +5v (about 125 mA) and ground.
 *
 * Disable the serial console using raspi-conifg.
 *
 * Add dtoverlay=ps2pi to /boot/config.txt and copy ps2pi-overlay to /boot/overlays.
 *
 * Use companion userspace program ps2test to determine clock dividers
 * default parameters should be correct for an IBM Model M keyboard.
 *
 * You don't need to do anything special to compile this module.
 *
 * Portions of this module are a bit of a hack since I don't understand how to use pinctrl yet.
 * At least I got device tree working!
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/of_platform.h>
//#include <linux/of_gpio.h>
//#include <linux/of.h>

#define BASE		BCM2708_PERI_BASE

int integer = 15;
int fractional = 38;
static unsigned keyup = 0;
static unsigned escape = 0;
static unsigned pause = 0;
volatile unsigned *(uart);
volatile unsigned *(gpio);
static struct input_dev *ps2;
struct device;

/* device tree stuff */
static const struct of_device_id ps2pi_dt_ids[] = {
	{ .compatible = "ps2pi" },
	{},
};

MODULE_DEVICE_TABLE(of, ps2pi_dt_ids);

static struct platform_driver ps2pi_driver = {
	.driver = {
		.name	= "ps2pi",
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(ps2pi_dt_ids),
	},
};

/* Raw SET 2 scancode to linux keycode translation table */
static unsigned char translate[256] = {
/* 00 */  KEY_RESERVED, KEY_F9,        KEY_RESERVED,  KEY_F5,        KEY_F3,        KEY_F1,       KEY_F2,        KEY_F12,
/* 08 */  KEY_ESC,      KEY_F10,       KEY_F8,        KEY_F6,        KEY_F4,        KEY_TAB,      KEY_GRAVE,     KEY_RESERVED,
/* 10 */  KEY_RESERVED, KEY_LEFTALT,   KEY_LEFTSHIFT, KEY_RESERVED,  KEY_LEFTCTRL,  KEY_Q,        KEY_1,         KEY_RESERVED,
/* 18 */  KEY_RESERVED, KEY_RESERVED,  KEY_Z,         KEY_S,         KEY_A,         KEY_W,        KEY_2,         KEY_RESERVED, 
/* 20 */  KEY_RESERVED, KEY_C,         KEY_X,         KEY_D,         KEY_E,         KEY_4,        KEY_3,         KEY_RESERVED,
/* 28 */  KEY_RESERVED, KEY_SPACE,     KEY_V,         KEY_F,         KEY_T,         KEY_R,        KEY_5,         KEY_RESERVED,
/* 30 */  KEY_RESERVED, KEY_N,         KEY_B,         KEY_H,         KEY_G,         KEY_Y,        KEY_6,         KEY_RESERVED,
/* 38 */  KEY_RESERVED, KEY_RIGHTALT,  KEY_M,         KEY_J,         KEY_U,         KEY_7,        KEY_8,         KEY_RESERVED,
/* 40 */  KEY_RESERVED, KEY_COMMA,     KEY_K,         KEY_I,         KEY_O,         KEY_0,        KEY_9,         KEY_RESERVED,
/* 48 */  KEY_RESERVED, KEY_DOT,       KEY_SLASH,     KEY_L,         KEY_SEMICOLON, KEY_P,        KEY_MINUS,     KEY_RESERVED,
/* 50 */  KEY_RESERVED, KEY_RESERVED,  KEY_APOSTROPHE,KEY_RESERVED,  KEY_LEFTBRACE, KEY_EQUAL,    KEY_RESERVED,  KEY_RESERVED,
/* 58 */  KEY_CAPSLOCK, KEY_RIGHTSHIFT,KEY_ENTER,     KEY_RIGHTBRACE,KEY_RESERVED,  KEY_BACKSLASH,KEY_RESERVED,  KEY_RESERVED,
/* 60 */  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED, KEY_BACKSPACE, KEY_RESERVED,
/* 68 */  KEY_RESERVED, KEY_KP1,       KEY_RESERVED,  KEY_KP4,       KEY_KP7,       KEY_RESERVED, KEY_HOME,      KEY_RESERVED,
/* 70 */  KEY_KP0,      KEY_KPDOT,     KEY_KP2,       KEY_KP5,       KEY_KP6,       KEY_KP8,      KEY_ESC,       KEY_NUMLOCK,
/* 78 */  KEY_F11,      KEY_KPPLUS,    KEY_KP3,       KEY_KPMINUS,   KEY_KPASTERISK,KEY_KP9,      KEY_SCROLLLOCK,KEY_RESERVED,
/* 80 */  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,  KEY_F7,        KEY_SYSRQ,     KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,
/* 88 */  KEY_PAUSE,    KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,
/* 90 */  KEY_RESERVED, KEY_RIGHTALT,  KEY_RESERVED,  KEY_RESERVED,  KEY_RIGHTCTRL, KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,
/* 98 */  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,
/* a0 */  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,
/* a8 */  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,
/* b0 */  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,
/* b8 */  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,
/* c0 */  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,
/* c8 */  KEY_RESERVED, KEY_RESERVED,  KEY_KPSLASH,   KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,
/* d0 */  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,
/* d8 */  KEY_RESERVED, KEY_RESERVED,  KEY_KPENTER,   KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,
/* e0 */  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,
/* e8 */  KEY_RESERVED, KEY_END,       KEY_RESERVED,  KEY_LEFT,      KEY_HOME,      KEY_RESERVED, KEY_RESERVED,  KEY_RESERVED,
/* f0 */  KEY_INSERT,   KEY_DELETE,    KEY_DOWN,      KEY_RESERVED,  KEY_RIGHT,     KEY_UP,       KEY_RESERVED,  KEY_RESERVED,
/* f8 */  KEY_RESERVED, KEY_RESERVED,  KEY_PAGEDOWN,  KEY_RESERVED,  KEY_PRINT,     KEY_PAGEUP,   KEY_RESERVED,  KEY_RESERVED
};

/* handle uart interrupt: read uart, translate to proper keycode, and send to event subsystem */
irq_handler_t irq_handler(int irq, void *dev_id, struct pt_regs *regs)
{
    static unsigned key;
    key = ioread32(uart);
    if ((key & (1<<8)) != 0) printk(KERN_INFO "ps2pi: framing error\n");
    if ((key & (1<<9)) != 0) printk(KERN_INFO "ps2pi: parity error\n");
    if ((key & (1<<10)) != 0) printk(KERN_INFO "ps2pi: break error\n");
    if ((key & (1<<11)) != 0) printk(KERN_INFO "ps2pi: overrun error\n");
    key = key & 0xff;

    if (key == 0xf0) {
        keyup = 1;
        return 0;
    }

    if (key == 0xe0) {
        escape = 1;
        return 0;
    }

    if (key == 0xe1) {
        pause = 2;
        return 0;
    }

    if (pause == 2) {
        pause = 1;
        return 0;
    }

    if (pause == 1) {
        key = 0x88;
        pause = 0;
    }

    if (escape == 1) {
        key |= 0x80;
        escape = 0;
    }

    key = translate[key];

    if (keyup == 1) {
        input_report_key(ps2,key,0);
        keyup = 0;
    } else {
	input_report_key(ps2,key,1);
    }

    input_sync(ps2);
    return 0;
}

/* set up */
static int __init ps2pi_init(void)
{
    static int i, retval;
    static unsigned key;

    /* set uart to receive 8 bit, 1 stop, odd parity */
    uart = ioremap(BASE + 0x201000, 60);
    iowrite32(0, uart + 12);
    iowrite32(((3<<5) | (1<<1)), uart + 11);

    /* set baud rate to command line parameters */
    iowrite32(integer, uart + 9);
    iowrite32(fractional, uart + 10);

    /* flush buffer */
    key = ioread32(uart);
    key = ioread32(uart);
    key = ioread32(uart);
    key = ioread32(uart);

    /* interrupts on */
    iowrite32((1<<4), uart + 14);

    /* uart on */
    iowrite32((1<<9) | 1, uart + 12);
    ps2=input_allocate_device();
    ps2->name = "ps2pi";
    ps2->phys = "ps2/input0";
    ps2->id.bustype = BUS_HOST;
    ps2->id.vendor = 0x0001;
    ps2->id.product = 0x0001;
    ps2->id.version = 0x0100;
    ps2->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REP);
    ps2->keycode = translate;
    ps2->keycodesize = sizeof(unsigned char);
    ps2->keycodemax = ARRAY_SIZE(translate);
    for (i = 1; i < 0x256; i++) set_bit(i,ps2->keybit);
    retval = input_register_device(ps2);
    retval = request_irq(83,(irq_handler_t)irq_handler,IRQF_SHARED,"ps2pi",(void *)irq_handler);
    input_report_key(ps2,KEY_NUMLOCK,1);	/* numlock on because I like it that way */
    input_sync(ps2);
    printk(KERN_INFO "ps2pi: loaded integer %i fractional %i\n", integer, fractional);
    return platform_driver_register(&ps2pi_driver);
}

/* tear down */
static void __exit ps2pi_exit(void)
{
    /* interrupts off */
    iowrite32(0, uart + 14);

    /* uart off */
    iowrite32(0, uart + 12);
    iounmap(uart);
    input_unregister_device(ps2);
    free_irq(83,(void *)irq_handler);
    platform_driver_unregister(&ps2pi_driver);
    printk(KERN_INFO "ps2pi: unloaded\n");
    return;
}

module_init(ps2pi_init);
module_exit(ps2pi_exit);

MODULE_LICENSE("GPL");

