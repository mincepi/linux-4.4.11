/* analog2pi.c -- Raspberry Pi dual channel ADC using the SPI and PCM peripherals.
 *
 * Copyright 2016 mincepi  https://sites.google.com/site/mincepi/analog2pi
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the Linux kernel source for more details.
 *
 * Based on my apfb.c and servo3.c modules
 *
 * You don't need to do anything special to compile this module.
 *
 * This module only works on the B+, A+, 2, and zero models. It could be altered for the Rev 2 A and B, but they would need a connection to P5.
 *
 * You should add disable_pvt=1 to /boot/config.txt
 *
 * If you read 2700 byte samples fast enough (better than once every 50 milliseconds) you will get continuous data.
 *
 * Sample rate is 54,253 samples/second.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/cdev.h>
#include <linux/syscalls.h>
#include <linux/slab.h>
#include <linux/platform_data/dma-bcm2708.h>
#include <linux/dma-mapping.h>

#define BASE	BCM2708_PERI_BASE

volatile unsigned *(timer) = 0;
volatile unsigned *(gpio) = 0;
volatile unsigned *(spi) = 0;
volatile unsigned *(pcm) = 0;
volatile unsigned *(clock) = 0;
volatile unsigned *(dma0) = 0;
volatile unsigned *(dma1) = 0;
volatile unsigned *(dma2) = 0;

unsigned interrupt, divider, clocks, spioff, pcmoff, mask, half, bus;
int channel0, channel1, channel2;
static dev_t Device;
static struct class* Class;
static struct cdev c_dev;
void *(base), *(mem), *(spiend), *(pcmend);

/* synchronize pcm and spi clocks */
void sync(void)
{

    static int j, sync;

    do
    {

	/* slow pcm clock for short while */
	iowrite32((0x5a<<24) | (48<<12) | 1, clock + 1);
//	iowrite32(0x5a<<24 | ioread32(clock + 1) | 1, clock + 1);
	udelay(1);
	iowrite32((0x5a<<24) | (48<<12), clock + 1);
//	iowrite32(0x5a<<24 | ioread32(clock + 1) & ~(1), clock + 1);

	//test clocks and calculate score
	sync = 0;

	for(j=0;j<200;j++)
	{

	    clocks = (ioread32(gpio + 13)) & ((1<<11) | (1<<18));
	    if ((clocks == (1<<18)) | (clocks == (1<<11))) sync++;

	}

    }
    while (sync > 5);

}

/* request and initialize dma
 * sets global variables dma0, dma1, dma2, channel0, channel1, channel2
 * returns -1 if fails
 */
int dma(void)
{

    //request dma channels
    channel0 = bcm_dma_chan_alloc(BCM_DMA_FEATURE_FAST, &base, &interrupt);

    if (channel0 < 0)
    {

	printk(KERN_INFO "analog2pi: dma0 alloc fail\n");
	return -1;

    }

    dma0 = (unsigned*)base;
    channel1 = bcm_dma_chan_alloc(BCM_DMA_FEATURE_FAST, &base, &interrupt);

    if (channel1 < 1)
    {

	bcm_dma_chan_free(channel0);
	printk(KERN_INFO "analog2pi: dma1 alloc fail\n");
	return -1;

    }

    dma1 = (unsigned*)base;

    channel2 = bcm_dma_chan_alloc(BCM_DMA_FEATURE_FAST, &base, &interrupt);

    if (channel2 < 2)
    {

	bcm_dma_chan_free(channel1);
	bcm_dma_chan_free(channel0);
	printk(KERN_INFO "analog2pi: dma2 alloc fail\n");
	return -1;

    }

    dma2 = (unsigned*)base;

    //reset the dma controllers
    iowrite32((1<<31), dma0);
    iowrite32((1<<31), dma1);
    iowrite32((1<<31), dma2);
    udelay(100);

    //set up spi transmit dma
    iowrite32((8<<20) | (2<<16), dma0);
    iowrite32(bus + 64, dma0 + 1);

    //set up spi receive dma
    iowrite32((8<<20) | (2<<16), dma1);
    iowrite32(bus + 96, dma1 + 1);

    //set up pcm receive dma
    iowrite32((8<<20) | (2<<16), dma2);
    iowrite32(bus, dma2 + 1);

    return 0;

}

/* request and initialize memory
 * sets global variable mem
 * returns -1 if fails
 */
int memory(void)
{

    mem = dma_zalloc_coherent(NULL, 131072, &bus, GFP_ATOMIC);

    if (mem == NULL)
    {

	printk(KERN_INFO "analog2pi: zalloc fail\n");
	return -1;

    }

    //dma control block for pcm receive
    *((unsigned*)mem + 0) = ((1<<26) | (3<<16) | (1<<10) | (1<<4) | (1<<3));
    *((unsigned*)mem + 1) = 0x7e203004;
    *((unsigned*)mem + 2) = bus + 64800 + 240;
    *((unsigned*)mem + 3) = 64800;
    *((unsigned*)mem + 4) = 0;
    *((unsigned*)mem + 5) = bus;
    *((unsigned*)mem + 6) = 0;
    *((unsigned*)mem + 7) = 0;

    //dma control block for dlen
    *((unsigned*)mem + 8) = ((31<<26) | (1<<12) | (1<<8) | (1<<6));
    *((unsigned*)mem + 9) = bus + 132;
    *((unsigned*)mem + 10) = 0x7e20400c;
    *((unsigned*)mem + 11) = 4;
    *((unsigned*)mem + 12) = 0;
    *((unsigned*)mem + 13) = bus + 64;
    *((unsigned*)mem + 14) = 0;
    *((unsigned*)mem + 15) = 0;

    //dma control block for spi transmit
    *((unsigned*)mem + 16) = ((1<<26) | (6<<16) | (1<<12) | (1<<8) | (1<<6) | (1<<3) | (1<<1));
    *((unsigned*)mem + 17) = bus + 136;
    *((unsigned*)mem + 18) = 0x7e204004;
    *((unsigned*)mem + 19) = (800<<16) | 24;
    *((unsigned*)mem + 20) = -24 & 0xffff;
    *((unsigned*)mem + 21) = bus + 32;
    *((unsigned*)mem + 22) = 0;
    *((unsigned*)mem + 23) = 0;

    //dma control block for spi receive
    *((unsigned*)mem + 24) = ((1<<26) | (7<<16) | (1<<10) | (1<<4) | (1<<3));
    *((unsigned*)mem + 25) = 0x7e204004;
    *((unsigned*)mem + 26) = bus + 240;
    *((unsigned*)mem + 27) = 64800;
    *((unsigned*)mem + 28) = 0;
    *((unsigned*)mem + 29) = bus + 96;
    *((unsigned*)mem + 30) = 0;
    *((unsigned*)mem + 31) = 0;

    //dma data to set dlen
    *((unsigned*)mem + 33) = 0xffff;

    //dma data for spi transmit (reset pulse)
    *((unsigned*)mem + 34) = 0x00feffff;
    *((unsigned*)mem + 35) = 0x0;
    *((unsigned*)mem + 36) = 0x0;
    *((unsigned*)mem + 37) = 0x0;
    *((unsigned*)mem + 38) = 0x0;
    *((unsigned*)mem + 39) = 0x0;

    //beginning of SPI receive data area
    //    *((unsigned*)mem + 60)

    //beginning of PCM receive data area
    //    *((unsigned*)mem + 60 + 16200)

    spiend = mem + 240 + 64800;
    pcmend = mem + 240 + 64800 + 64800;
    return 0;

}

/* determine PCM and SPI offset
 * sets global variables mask, spioff and pcmoff
 */
void offset (void)
{

    static unsigned j, spitop, pcmtop, word, factor;

    //disable reset signal
    *((unsigned*)mem + 34) = 0x00000000;

    //enable gpio 9 and 20 pulldowns
    iowrite32(1, gpio + 37);
    udelay(100);
    iowrite32((1<<9) | (1<<20), gpio + 38);
    udelay(100);
    iowrite32(0, gpio + 37);
    iowrite32(0, gpio + 38);
    udelay(1000);

    //fill buffers with zeroes
    memset(mem + 240, 0, 64800 * 2);

    //wait for all DMA writes to be at top of buffers
    spitop = bus + 240;
    pcmtop = bus + 240 + 64800;

    do {} while (ioread32(dma1 + 4) > spitop + 1000);
//    do {} while (ioread32(dma2 + 4) > pcmtop + 1000);

    //enable reset sgnal
    *((unsigned*)mem + 34) = 0x00feffff;
    udelay(1000);

    //scan for first one bit in SPI
    for (j = 0; j < 2000; j++)
    {

	if (*((unsigned*)mem + 60 + j) != 0) break;

    }

    spioff = j;

    //scan for first one bit in PCM
    for (j = 0; j < 2000; j++)
    {

	if (*((unsigned*)mem + 60 + 16200 + j) != 0) break;

    }

    word = *((unsigned*)mem + 60 + 16200 + j);
    pcmoff = j;

    for (j = 0x80000000; j > 0; j = (j>>1))
    {

	if ((word & j) != 0) break;

    }

    mask = j;
    if (pcmoff < spioff) {factor = pcmoff / 6;} else {factor = spioff / 6;};
    pcmoff = pcmoff - (factor * 6);
    spioff = spioff - (factor * 6);

    //disable gpio 9 and 20 pulldowns
    iowrite32(0, gpio + 37);
    udelay(100);
    iowrite32((1<<9) | (1<<20), gpio + 38);
    udelay(100);
    iowrite32(0, gpio + 37);
    iowrite32(0, gpio + 38);

}

/* returns position of first zero bit in SPI sample starting at address
 * returns 128 if no transition
 */
uint8_t spiparse (volatile uint32_t *address)
{

    static unsigned i, j, k, byte, word;
    static uint8_t value;
    value = 0;

    for (i = 0; i < 4; i++)
    {

	word = *(address + i);

	for (j = 0; j < 32; j+= 8)
	{

	    byte = (word >> j) & 0xff;

	    for (k = 0x80; k > 0; k = k >> 1)
	    {

		if ((k & byte) == 0) return value;
		value ++;
	    }
	}
    }
    return 128;
}

/* returns position of first zero bit in PCM sample starting at address
 * returns 128 if no transition
 */
static uint8_t pcmparse (volatile uint32_t *address, unsigned mask)
{
    static unsigned word;
    static uint8_t k;
    word = *address;

    for (k = 0; k < 128; k++)
    {

	if ((word & mask) == 0) break;
	mask = (mask>>1);

	if (mask == 0)
	{

	    mask = 0x80000000;
	    address++;

	    //back to top if past end of buffer
	    if (address == pcmend) address -= 16200;

	    word = *address;
	}
    }
    return k;
}

/* handle reads from device node */
static ssize_t read(struct file *filp, char *buffer, size_t length, loff_t * offset)
{

    static unsigned k, middle, time;
    volatile uint32_t *address;
    half ^= 1;
    middle = bus + 240 + (spioff * 4) + 32400;

    //range check length
    if (length > 2700) return 0;

//add slip check here

    //read top half of samples
    if (half == 0)
    {

	//wait for stable data
	do {time = ioread32(timer);} while (ioread32(dma1 + 4) < middle);

	//get SPI samples
	address = (uint32_t*)mem + 60 + spioff + 2;

	for (k = 0; k < length; k+= 2)
	{

	    put_user(spiparse(address), buffer + k);
	    address += 6;

	}

	//get PCM samples
	address = (uint32_t*)mem + 60 + 16200 + pcmoff + 2;

	for (k = 1; k < length; k+= 2)
	{

	    put_user(pcmparse(address, mask), buffer + k);
	    address += 6;
	}

	//verify stable data
	if (((ioread32(timer) - time) < 20000) & (ioread32(dma1 + 4) > middle)) return length;
	printk(KERN_INFO "analog2pi: failed to find stable read data\n");
	return 0;

    //read bottom half of samples
    } else {

	//wait for stable data
	do {time = ioread32(timer);} while (ioread32(dma1 + 4) > middle);

	//get SPI samples
	address = (uint32_t*)mem + 60 + 8100 + spioff + 2;

	for (k = 0; k < length; k+= 2)
	{

	    put_user(spiparse(address), buffer + k);
	    address += 6;
	    //back to top if past end of buffer
	    if (address >= (uint32_t*)spiend) address -= 16200;

	}

	//get PCM samples
	address = (uint32_t*)mem + 60 + 16200 + 8100 + pcmoff + 2;

	for (k = 1; k < length; k+= 2)
	{

	    put_user(pcmparse(address, mask), buffer + k);
	    address += 6;
	    //back to top if past end of buffer
	    if (address >= (uint32_t*)pcmend) address -= 16200;

	}

	//verify stable data
	if (((ioread32(timer) - time) < 20000) & (ioread32(dma1 + 4) < middle)) return length;
	printk(KERN_INFO "analog2pi: failed to find stable read data\n");
	return 0;
    }
}

/* device handler table */
    static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = read,
};


/* set device permissions to something useful */
static int uevent(struct device *dev, struct kobj_uevent_env *env)
{
    add_uevent_var(env, "DEVMODE=%#o", 0766);
    return 0;
}

/* initialize module */
int init_module(void)
{

    //set up memory
    if (memory() == -1) return -1;

    //set up dma
    if (dma() == -1)
    {

	dma_free_coherent(NULL, 131072, mem, bus);
	return -1;

    }

    //set up device node
    alloc_chrdev_region(&Device, 0, 1, "analog2pi");
    Class = class_create(THIS_MODULE, "analog2pi");
    Class->dev_uevent = uevent;
    device_create(Class, NULL, MKDEV(MAJOR(Device), MINOR(Device)), NULL, "analog2pi");
    cdev_init(&c_dev, &fops);
    cdev_add(&c_dev, Device, 1);

    //set up gpio 9 for SPI in, 10 for SPI out, 20 for PCM DIN
    gpio = ioremap(BASE + 0x200000, 160);
    iowrite32((ioread32(gpio) & ~(7<<27)) | (4<<27), gpio);
    iowrite32((ioread32(gpio + 1) & ~(7)) | (4), gpio + 1);
    iowrite32((ioread32(gpio + 2) & ~(7)) | (4), gpio + 2);

//debug output spi clock
iowrite32((ioread32(gpio + 1) & ~(7<<3)) | (4<<3), gpio + 1);

//debug output pcm clock
iowrite32((ioread32(gpio + 1) & ~(7<<24)) | (4<<24), gpio + 1);

    timer = ioremap(BASE + 0x003004, 4);

    //clear spi fifos
    spi = ioremap(BASE + 0x204000, 24);
    iowrite32((1<<5) | (1<<4), spi);//clear

    //set up spi peripheral
    iowrite32(24, spi + 2);//speed, must be even
    iowrite32(0, spi + 4);
    iowrite32((48<<24) | (32<<16) | (16<<8) | 32, spi + 5);
    udelay(100);
    iowrite32(0xffff, spi + 3);
    iowrite32((1<<11) | (1<<8) | (1<<7), spi);

    //start spi receive dma
    iowrite32(ioread32(dma1) | 1, dma1);

    //set up pcm clock
    clock = ioremap(BASE + 0x101098, 8);
    iowrite32(0x5a<<24 | (ioread32(clock) & ~(0xff<<24 | 1<<4)), clock);
    while ((ioread32(clock) & (1<<7)) == 1){}
    iowrite32((0x5a<<24) | (48<<12), clock + 1);
    iowrite32((0x5a<<24) | (1<<9) | 6, clock);
    udelay(10);
    iowrite32((0x5a<<24) | (1<<9) | (1<<4) | 6, clock);
    while ((ioread32(clock) & (1<<7)) == 0){}

    //set up pcm peripheral
    pcm = ioremap(BASE + 0x203000, 36);
    iowrite32((1<<25) | (1<<4) | 1, pcm);
    udelay(100);
    iowrite32((31<<10), pcm + 2);
    iowrite32((1<<31) | (1<<30) | (8<<16), pcm + 3);
    iowrite32(0, pcm + 4);
    iowrite32((48<<16) | 36, pcm + 5);
    iowrite32(0, pcm + 6);
    iowrite32(0, pcm + 8);
    udelay(100);
    iowrite32(ioread32(pcm) | (1<<9), pcm);

    //start spi transmit dma
    iowrite32(ioread32(dma0) | 1, dma0);

    //start the pcm dma
    iowrite32(ioread32(dma2) | 1, dma2);

    //pcm go
    iowrite32(ioread32(pcm) | (1<<1), pcm);

    sync();
    offset();
    half = 1;
    printk(KERN_INFO "analog2pi: loaded\n");
    return 0;
}

/* tear down module */
void cleanup_module(void)
{

    iounmap(timer);

    //stop the dma
    iowrite32(ioread32(dma0) & ~(1), dma0);
    iowrite32(ioread32(dma1) & ~(1), dma1);
    iowrite32(ioread32(dma2) & ~(1), dma2);

    //stop spi peripheral
    iowrite32(1<<4, spi);
    iounmap(spi);

    //stop pcm peripheral
    iowrite32(ioread32(pcm) & ~(1), pcm);
    udelay(100);
    iounmap(pcm);

    //stop pcm clock
    iowrite32(0x5a<<24 | (ioread32(clock) & ~(0xff<<24 | 1<<4)), clock);
    iounmap(clock);

    //enable gpio 9 and 20 pulldowns
    iowrite32(1, gpio + 37);
    udelay(100);
    iowrite32((1<<9) | (1<<20), gpio + 38);
    udelay(100);
    iowrite32(0, gpio + 37);
    iowrite32(0, gpio + 38);

    //set gpio 9, 10 and 20 as inputs, default pulldown will keep output low
    iowrite32(ioread32(gpio) & ~(7<<27), gpio);
    iowrite32(ioread32(gpio + 1) & ~(7), gpio + 1);
    iowrite32((ioread32(gpio + 2) & ~(7)), gpio + 2);
    iounmap(gpio);

    //remove device node
    device_destroy(Class, MKDEV(MAJOR(Device), MINOR(Device)));
    class_destroy(Class);
    unregister_chrdev_region(Device, 1);
    cdev_del(&c_dev);
    printk(KERN_INFO "analog2pi: unloaded\n");

    //free dma
    bcm_dma_chan_free(channel0);
    bcm_dma_chan_free(channel1);
    bcm_dma_chan_free(channel2);

    //free memory
    dma_free_coherent(NULL, 131072, mem, bus);
}

MODULE_DESCRIPTION("analog2pi");
MODULE_LICENSE("GPL");

