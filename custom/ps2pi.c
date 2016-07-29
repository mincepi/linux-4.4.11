/*
 * ps2pi.c -- Raspberry Pi PS/2 keyboard device driver using the UART.
 *
 * Copyright 2016 mincepi
 *
 * https://sites.google.com/site/mincepi/ps2pi
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the Linux kernel source for more details.
 *
 * The following resources helped me greatly:
 *
 * The matrix-keypad Linux kernel module: matrix_keypad.c
 *
 * The platform device API
 * Copyright (C) 2011 Eklektix, Inc.
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
 * Disable the serial console using raspi-config.
 *
 * Add dtoverlay=ps2pi to /boot/config.txt and copy ps2pi-overlay.dtb to /boot/overlays.
 *
 * Use companion userspace program ps2test to determine the clock divider if you don't have a Model M keyboard.
 * Enter these values into ps2pi-overlay.dts, compile it and copy to /boot/overlays.
 *
 * If you need to change the keyboard translation table, it's also in the device tree overlay.
 * The keycodes you'll need for this are in the kernel source at include/uapi/linux/input.h
 *
 * You don't need to do anything special to compile this module.
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/of_platform.h>
#include <mach/platform.h>
#include <linux/types.h>

#define BASE		BCM2708_PERI_BASE

static unsigned keyup = 0;
static unsigned escape = 0;
static unsigned pause = 0;
volatile unsigned *(uart);
volatile unsigned *(gpio);
static struct input_dev *ps2;
struct platform_device *pdev;

/* Raw SET 2 scancode to linux keycode translation table, filled from device tree overlay during probe */
static uint8_t translate[256] = {0};

/* device tree stuff */
static const struct of_device_id ps2pi_dt_ids[] = {
	{ .compatible = "ps2pi" },
	{},
};

MODULE_DEVICE_TABLE(of, ps2pi_dt_ids);

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
static int ps2pi_probe(struct platform_device *pdev)
{
    static uint32_t integer, fractional;
    static int key, retval, i;
    struct device *dev = &pdev->dev;
    struct device_node *np = dev->of_node;

    /* set up input event device */
    of_property_read_u8_array(np, "translate", &translate[0], 256);
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

    /* numlock on because I like it that way */
    input_report_key(ps2, KEY_NUMLOCK, 1);    input_sync(ps2);

    /* disable uart */
    uart = ioremap(BASE + 0x201000, 60);
    iowrite32(0, uart + 12);
    mdelay(10);

    /* set baud rate from device tree */
    of_property_read_u32(np, "integer", &integer);
    of_property_read_u32(np, "fractional", &fractional);
    iowrite32(integer, uart + 9);
    iowrite32(fractional, uart + 10);

    /* set uart to receive 8 bit, 1 stop, odd parity, no fifo */
    iowrite32(((3<<5) | (1<<1)), uart + 11);

    /* flush buffer */
    key = ioread32(uart);

    /* interrupt on */
    iowrite32((1<<4), uart + 14);

    /* uart on */
    retval = request_irq(83,(irq_handler_t)irq_handler,IRQF_SHARED,"ps2pi",(void *)irq_handler);
    iowrite32((1<<9) | 1, uart + 12);
    printk(KERN_INFO "ps2pi: divider integer %i fractional %i\n", integer, fractional);
    return 0;
}

/* tear down */
static int ps2pi_remove(struct platform_device *pdev)
{

    /* uart off */
    iowrite32(0, uart + 12);

    /* interrupts off */
    iowrite32(0, uart + 14);
    free_irq(83,(void *)irq_handler);
    iounmap(uart);
    input_unregister_device(ps2);
    return 0;
}

static struct platform_driver ps2pi_driver = {
	.probe	=	ps2pi_probe,
	.remove	=	ps2pi_remove,
	.driver = {
		.name	= "ps2pi",
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(ps2pi_dt_ids),
	},
};

/* register driver */
static int __init ps2pi_init(void)
{
    printk(KERN_INFO "ps2pi: loaded\n");
    return platform_driver_register(&ps2pi_driver);
    return 0;
}

/* unregister driver */
static void __exit ps2pi_exit(void)
{
    platform_driver_unregister(&ps2pi_driver);
    printk(KERN_INFO "ps2pi: unloaded\n");
    return;
}

module_init(ps2pi_init);
module_exit(ps2pi_exit);

MODULE_LICENSE("GPL");
