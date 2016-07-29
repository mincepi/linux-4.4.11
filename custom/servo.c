/*   servo.c -- Raspberry Pi servo driver using the pwm peripheral.
 *
 *   Copyright 2015 the pi hacker  https://sites.google.com/site/thepihacker
 *
 *   This file is subject to the terms and conditions of the GNU General Public
 *   License. See the file COPYING in the Linux kernel source for more details.
 *
 *   Based on my pcm2servo.c, eggplotPi.c and motors.c modules
 *
 *   You don't need to do anything special to compile this module.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/cdev.h>
#include <linux/syscalls.h>

volatile unsigned *(gpio) = 0;
volatile unsigned *(clock) = 0;
volatile unsigned *(pwm) = 0;
static dev_t Device;
static struct class* Class;
static struct cdev c_dev;

//handle writes to device node and change pulse width
static ssize_t write(struct file *filp, const char *buffer, size_t length, loff_t * offset)
{

    static u8 value;

    //get value
    get_user(value, buffer);

    //sanitize
    if (value > 100) value = 100;

    //change divider
    iowrite32(value + 100, pwm + 9);
    return 1;

}

//device handler table
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .write = write,
};

//initialize module
int init_module(void)
{

    //set up device node
    alloc_chrdev_region(&Device, 0, 1, "servo");
    Class = class_create(THIS_MODULE, "servo");
    device_create(Class, NULL, MKDEV(MAJOR(Device), MINOR(Device)), NULL, "servo");
    cdev_init(&c_dev, &fops);
    cdev_add(&c_dev, Device, 1);

    //set up gpio 19 for pwm out
    gpio = ioremap(0x20200000, 8);
    iowrite32((ioread32(gpio + 1) & ~(7<<27)) | (2<<27), gpio + 1);
    iounmap(gpio);

    //set up pwm clock 100KHz
    clock = ioremap(0x201010a0, 8);
    iowrite32(0x5a<<24 | (ioread32(clock) & ~(0xff<<24 | 1<<4)), clock);
    while ((ioread32(clock) & 1<<7) == 1){}
    iowrite32(0x5a<<24 | 192<<12, clock + 1);
    iowrite32(0x5a<<24 | 1, clock);
    udelay(10);
    iowrite32(0x5a<<24 | 1<<4 | 1, clock);
    while ((ioread32(clock) & 1<<7) == 0){}
    iounmap(clock);

    //set up pwm 20 millisecond period
    pwm = ioremap(0x2020c000, 24);
    iowrite32(0, pwm);
    mdelay(50);
    iowrite32(2000, pwm + 8);
    iowrite32(150, pwm + 9);
    iowrite32(((1<<15) | (1<<8) | (1<<7)), pwm);
    printk(KERN_INFO "servo: loaded\n");
    return 0;
}

//tear down module
void cleanup_module(void)
{

    //set gpio 19 as input, default pulldown will keep output low
    gpio = ioremap(0x20200000, 8);
    iowrite32(ioread32(gpio + 1) & ~(7<<27), gpio + 1);
    iounmap(gpio);

    //stop pwm peripheral
    iowrite32(0, pwm);
    iounmap(pwm);
    mdelay(30);

    //stop pwm clock
    clock = ioremap(0x201010a0, 8);
    iowrite32(0x5a<<24 | (ioread32(clock) & ~(0xff<<24 | 1<<4)), clock);
    iounmap(clock);

    //remove device node
    device_destroy(Class, MKDEV(MAJOR(Device), MINOR(Device)));
    class_destroy(Class);
    unregister_chrdev_region(Device, 1);
    cdev_del(&c_dev);
    printk(KERN_INFO "servo: unloaded\n");

}

MODULE_DESCRIPTION("servo");
MODULE_LICENSE("GPL");

