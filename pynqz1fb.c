/**
 * @file pynqz1fb.c
 * @author Kenta IDA <fuga@fugafuga.org>
 * @description
 * Frame-buffer driver for PYNQ-Z1 HDMI video output subsystem in the base overlay.
 * This driver is based on simplefb.c, written by Stephen Warren.
 * See also simplefb.c in the parent directory of the directory this file is placed.
 */
/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */
 
#include <linux/device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/dma-mapping.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include "pynqz1fb.h"

#define BIT_DISPLAY_RED 16
#define BIT_DISPLAY_BLUE 0
#define BIT_DISPLAY_GREEN 8

#define BIT_DISPLAY_START 0
#define BIT_CLOCK_RUNNING 0

#define DISPLAY_NOT_HDMI 0
#define DISPLAY_HDMI 1

#define DRIVER_NAME		"pynqz1_fb"

#define BYTES_PER_PIXEL	3
#define BITS_PER_PIXEL	(BYTES_PER_PIXEL * 8)

#define RED_SHIFT	16
#define GREEN_SHIFT	8
#define BLUE_SHIFT	0


static struct fb_fix_screeninfo pynqz1_fb_fix = {
	.id =		"PYNQ-Z1 FB",
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_TRUECOLOR,
	.accel =	FB_ACCEL_NONE
};

static struct fb_var_screeninfo pynqz1_fb_var = {
	.bits_per_pixel =	BITS_PER_PIXEL,

	.red =		{ RED_SHIFT, 8, 0 },
	.green =	{ GREEN_SHIFT, 8, 0 },
	.blue =		{ BLUE_SHIFT, 8, 0 },
	.transp =	{ 0, 0, 0 },

	.activate =	FB_ACTIVATE_NOW,
    .vmode = FB_VMODE_NONINTERLACED,

    .xres  = 1280,
    .yres  = 1024,
    .xres_virtual = 1280,
    .yres_virtual = 1024,

    .width  = (u32)(1280*2.54/72),
    .height = (u32)(720*2.54/72),
};

struct pynqz1_frame {
    u32     phys;
    void*   virt;
};

#define FB_NUMBER_OF_FRAMES 1
#define PALETTE_ENTRIES_NO 16


struct dynclk_param
{
    u16     prescaler;
    u16     multiplier;
    u16     postscaler;
    u16     reserved;
};

struct pynqz1_fb_screen_param
{
    u32     width;
    u32     height;
    
    u32     hFrameSize;
    u32     hSyncStart;
    u32     hSyncEnd;
    
    u32     vFrameSize;
    u32     vSyncStart;
    u32     vSyncEnd;

    struct dynclk_param dynclk;
};
struct pynqz1_fb_screen_param pynqz1_fb_screen_params[] = {
    /*w,    h,    hfsz, hss,  hse,  vfsz, vss,  vse,   pre, mul, post, rsvd */
    { 640,  480,  800,  656,  752,  525,  489,  491,  {1, 10, 8, 0} },
    { 800,  480,  1056, 840,  968,  525,  489,  491,  {1, 10, 6, 0} },
    { 800,  600,  1056, 840,  968,  628,  600,  604,  {1, 8,  4, 0} },
    { 1280, 720,  1650, 1390, 1430, 750,  724,  729,  {4, 30, 1, 0} },
    { 1280, 1024, 1688, 1328, 1440, 1066, 1024, 1027, {8, 86, 2, 0} },
    { 1920, 1080, 2200, 2008, 2052, 1125, 1083, 1088, {12,89, 1, 0} },
    { 0 },
};

// Number of memory resources this driver requires.
#define NUMBER_OF_MEM_RESOURCES 3

struct pynqz1_fb_device {
    struct fb_info  info;       // fb_info struct to register framebuffer

    void __iomem*   reg_dynclk; // virtual address to which dynclk register space is mapped.
    void __iomem*   reg_vtc;    // virtual address to which VTC register space is mapped.
    void __iomem*   reg_vdma;   // virtual address to which VDMA register space is mapped.

    struct device*  dev;        // device object
    struct pynqz1_frame frame[FB_NUMBER_OF_FRAMES]; // Frame information.
    u32    pseudo_palette[PALETTE_ENTRIES_NO];      // Pseudo palette table.

    u32 width;  // Number of horizontal pixels .
    u32 height; // Number of virtual pixels.
    u32 stride; // Number of bytes in a horizontal line.

    u32 debug;  // Debug level

    u32 flags;  // Flags. refer to PYNQZ1_FB_FLAGS_XXX constants.

    struct pynqz1_fb_screen_param* screen_param;    // Screen parameters
};
#define PYNQZ1_FB_FLAGS_REGISTERED (1u << 0)    // Is this framebuffer device registered ?

/* register access functions */
static u32 dynclk_read_reg(struct pynqz1_fb_device* fbdev, u32 offset) { return ioread32(fbdev->reg_dynclk + offset); }
static void dynclk_write_reg(struct pynqz1_fb_device* fbdev, u32 offset, u32 value) { iowrite32(value, fbdev->reg_dynclk + offset); }
static u32 vtc_read_reg(struct pynqz1_fb_device* fbdev, u32 offset) { return ioread32(fbdev->reg_vtc + offset); }
static void vtc_write_reg(struct pynqz1_fb_device* fbdev, u32 offset, u32 value) { iowrite32(value, fbdev->reg_vtc + offset); }

static u32 vdma_read_reg(struct pynqz1_fb_device* fbdev, u32 offset) { return ioread32(fbdev->reg_vdma + offset); }
static void vdma_write_reg(struct pynqz1_fb_device* fbdev, u32 offset, u32 value) { iowrite32(value, fbdev->reg_vdma + offset); }
static u32 vdma_tx_read_reg(struct pynqz1_fb_device* fbdev, u32 offset) { return ioread32(fbdev->reg_vdma + offset + VDMA_REG_TX); }
static void vdma_tx_write_reg(struct pynqz1_fb_device* fbdev, u32 offset, u32 value) { iowrite32(value, fbdev->reg_vdma + offset + VDMA_REG_TX); }
static u32 vdma_rx_read_reg(struct pynqz1_fb_device* fbdev, u32 offset) { return ioread32(fbdev->reg_vdma + offset + VDMA_REG_RX); }
static void vdma_rx_write_reg(struct pynqz1_fb_device* fbdev, u32 offset, u32 value) { iowrite32(value, fbdev->reg_vdma + offset + VDMA_REG_RX); }

// PLL multiplier (feedback divider) to lock pattern lookup table
static const u64 lock_lookup[64] = {
   0b0011000110111110100011111010010000000001,
   0b0011000110111110100011111010010000000001,
   0b0100001000111110100011111010010000000001,
   0b0101101011111110100011111010010000000001,
   0b0111001110111110100011111010010000000001,
   0b1000110001111110100011111010010000000001,
   0b1001110011111110100011111010010000000001,
   0b1011010110111110100011111010010000000001,
   0b1100111001111110100011111010010000000001,
   0b1110011100111110100011111010010000000001,
   0b1111111111111000010011111010010000000001,
   0b1111111111110011100111111010010000000001,
   0b1111111111101110111011111010010000000001,
   0b1111111111101011110011111010010000000001,
   0b1111111111101000101011111010010000000001,
   0b1111111111100111000111111010010000000001,
   0b1111111111100011111111111010010000000001,
   0b1111111111100010011011111010010000000001,
   0b1111111111100000110111111010010000000001,
   0b1111111111011111010011111010010000000001,
   0b1111111111011101101111111010010000000001,
   0b1111111111011100001011111010010000000001,
   0b1111111111011010100111111010010000000001,
   0b1111111111011001000011111010010000000001,
   0b1111111111011001000011111010010000000001,
   0b1111111111010111011111111010010000000001,
   0b1111111111010101111011111010010000000001,
   0b1111111111010101111011111010010000000001,
   0b1111111111010100010111111010010000000001,
   0b1111111111010100010111111010010000000001,
   0b1111111111010010110011111010010000000001,
   0b1111111111010010110011111010010000000001,
   0b1111111111010010110011111010010000000001,
   0b1111111111010001001111111010010000000001,
   0b1111111111010001001111111010010000000001,
   0b1111111111010001001111111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001,
   0b1111111111001111101011111010010000000001
};

// PLL multiplier (feedback divider) to feedback filter value lookup table.
static const u32 filter_lookup_low[64] = {
    0b0001011111,
    0b0001010111,
    0b0001111011,
    0b0001011011,
    0b0001101011,
    0b0001110011,
    0b0001110011,
    0b0001110011,
    0b0001110011,
    0b0001001011,
    0b0001001011,
    0b0001001011,
    0b0010110011,
    0b0001010011,
    0b0001010011,
    0b0001010011,
    0b0001010011,
    0b0001010011,
    0b0001010011,
    0b0001010011,
    0b0001010011,
    0b0001010011,
    0b0001010011,
    0b0001100011,
    0b0001100011,
    0b0001100011,
    0b0001100011,
    0b0001100011,
    0b0001100011,
    0b0001100011,
    0b0001100011,
    0b0001100011,
    0b0001100011,
    0b0001100011,
    0b0001100011,
    0b0001100011,
    0b0001100011,
    0b0010010011,
    0b0010010011,
    0b0010010011,
    0b0010010011,
    0b0010010011,
    0b0010010011,
    0b0010010011,
    0b0010010011,
    0b0010010011,
    0b0010010011,
    0b0010100011,
    0b0010100011,
    0b0010100011,
    0b0010100011,
    0b0010100011,
    0b0010100011,
    0b0010100011,
    0b0010100011,
    0b0010100011,
    0b0010100011,
    0b0010100011,
    0b0010100011,
    0b0010100011,
    0b0010100011,
    0b0010100011,
    0b0010100011,
    0b0010100011
};

/**
 * Calculate divider value from target divisor.
 * Ported from video_display.c in PYNQ source code>
 */
static u32 dynclk_calculate_divider(u32 divisor)
{
	u32 clock_on = divisor / 2;
	u32 clock_off = divisor - clock_on;
	u32 reg_value = 0;
    if( divisor == 1) {
        return 0x1041u;
    }
	if( divisor & 1 ) {
		reg_value = 1u << CLK_BIT_WEDGE;
		clock_off++;
	}
	reg_value |= (clock_off & 0x3fu) << 0;
	reg_value |= (clock_on  & 0x3fu) << 6;
	return reg_value;
}

/**
 * Calculate divider register value from target divisor.
 * Ported from video_display.c in PYNQ source code>
 */
static u32 dynclk_calculate_divider_config(u32 divisor)
{
    u32 divider = dynclk_calculate_divider(divisor);
    return (divider & 0xfffu) | ((divider & 0x3000u) << 10);
}

/**
 * Set the framebuffer hardware blank state.
 */
static int pynqz1_fb_blank(int blank_mode, struct fb_info *fbi)
{
    // Nothing to do.
    return 0;
}

/**
 * Set pseudo color palette.
 */
static int pynqz1_fb_setcolreg(u_int regno, u_int red, u_int green, u_int blue, u_int transp, struct fb_info* info)
{
	u32 *palette = info->pseudo_palette;

	if (regno >= PALETTE_ENTRIES_NO) {
        return -EINVAL;
    }
	
	red >>= 8;
	green >>= 8;
	blue >>= 8;
	palette[regno] = (red << RED_SHIFT) | (green << GREEN_SHIFT) | (blue << BLUE_SHIFT);

	return 0;
}

/**
 * Framebuffer operations.
 */
static struct fb_ops pynqz1_fb_ops =
{
	.owner			= THIS_MODULE,
	.fb_blank		= pynqz1_fb_blank,      // set blank state
	.fb_setcolreg   = pynqz1_fb_setcolreg,  // set pseudo color palette
    .fb_fillrect	= cfb_fillrect,         // fill rectangle area (use default function in the kernel)
	.fb_copyarea	= cfb_copyarea,         // copy rectangle area (use default function in the kernel)
	.fb_imageblit	= cfb_imageblit,        // image block transfer (use default function in the kernel)
};

/**
 * Parse device tree parameters.
 */
static int pynqz1_fb_parse_dt(struct platform_device* pdev, struct pynqz1_fb_device* fbdev)
{
    struct device_node* np = pdev->dev.of_node;
    int ret;
    struct pynqz1_fb_screen_param* screen_param = pynqz1_fb_screen_params;

    ret = of_property_read_u32(np, "width", &fbdev->width);
    if(ret) {
        dev_err(&pdev->dev, "Can't parse width property\n");
        return ret;
    }
    ret = of_property_read_u32(np, "height", &fbdev->height);
    if(ret) {
        dev_err(&pdev->dev, "Can't parse height property\n");
        return ret;
    }

    for(; screen_param->width != 0; ++screen_param ) {
        if( screen_param->width == fbdev->width && screen_param->height == fbdev->height ) {
            break;
        }
    }
    if( screen_param->width == 0 ) {
        screen_param = pynqz1_fb_screen_params;
        dev_info(&pdev->dev, "Requested resolution %dx%d is not supported.\n", fbdev->width, fbdev->height);
        fbdev->width  = screen_param->width;
        fbdev->height = screen_param->height;
        dev_info(&pdev->dev, "Fall back to %dx%d.\n", fbdev->width, fbdev->height);
    }
    else {
        dev_info(&pdev->dev, "Select resolution is %dx%d.\n", fbdev->width, fbdev->height);
    }
    fbdev->screen_param = screen_param;

    ret = of_property_read_u32(np, "debug", &fbdev->debug);

    return 0;
}

// Calculate page aligned framebuffer size.
#define GET_FB_SIZE(fbdev) PAGE_ALIGN((fbdev)->width * (fbdev)->height * BYTES_PER_PIXEL)
// Release register resource.
#define RELEASE_REG_RESOURCE(fbdev, resource) \
    do { if( (fbdev)->reg_##resource != NULL ) { devm_release_resource((fbdev)->dev, fbdev->reg_##resource ); fbdev->reg_##resource = NULL; } } while(0)

/**
 * Release framebuffer hardware resources.
 */
static void pynqz1_fb_release(struct pynqz1_fb_device* fbdev)
{
    int i;

    if( fbdev == NULL ) return;

    // Unregister framebuffer device
    if( fbdev->flags & PYNQZ1_FB_FLAGS_REGISTERED ) {
        unregister_framebuffer(&fbdev->info);
        fbdev->flags &= ~PYNQZ1_FB_FLAGS_REGISTERED;
    }

    // Stop modules
    if( fbdev->reg_vdma != NULL ) {
        // Reset DMA channels
        vdma_rx_write_reg(fbdev, VDMA_REG_CR, VDMA_CR_RESET_MASK);
        vdma_tx_write_reg(fbdev, VDMA_REG_CR, VDMA_CR_RESET_MASK);
    }
    if( fbdev->reg_vtc != NULL ) {
        // Reset the controller.
        vtc_write_reg(fbdev, VTC_REG_CTL, VTC_CTL_RESET_MASK);  
    }
    if( fbdev->reg_dynclk != NULL ) {
        // Stop clock generator.
        dynclk_write_reg(fbdev, OFST_DISPLAY_CLK_L, 0);
    }

    // Release memory mapped IO resources
    RELEASE_REG_RESOURCE(fbdev, dynclk);
    RELEASE_REG_RESOURCE(fbdev, vtc);
    RELEASE_REG_RESOURCE(fbdev, vdma);
    
    // Release framebuffer memories.
    for(i = 0; i < FB_NUMBER_OF_FRAMES; i++) {
        if( fbdev->frame[i].virt != NULL ) {
            dma_free_coherent(fbdev->dev, GET_FB_SIZE(fbdev), fbdev->frame[i].virt, fbdev->frame[i].phys );
            fbdev->frame[i].virt = NULL;
        }
    }
}

// Release resources and exit from the function if the return code indicates an error.
#define RELEASE_AND_RETURN(rc) do { pynqz1_fb_release(fbdev); return (rc); } while(0)

/**
 * Probe and initialize the framebuffer hardware.
 */
static int pynqz1_fb_probe(struct platform_device *pdev)
{
    struct pynqz1_fb_device* fbdev;
    u32 fbsize = 0;	
    int rc = 0;

	dev_info(&pdev->dev, "Probing PYNQ-Z1 Framebuffer...\n");

    /* Allocate memory to store device information */
    fbdev = devm_kzalloc(&pdev->dev, sizeof(*fbdev), GFP_KERNEL);
    if( !fbdev ) {
        return -ENOMEM;
    }
    memset(fbdev, 0, sizeof(*fbdev));
    fbdev->dev = &pdev->dev;
	/* Store driver-specific data */
    platform_set_drvdata(pdev, fbdev);

    /* parse parameters */
    rc = pynqz1_fb_parse_dt(pdev, fbdev);
    if( rc ) {
        return rc;
    }
    fbsize = GET_FB_SIZE(fbdev);

    /* Map registers to memory space. */
    {
        int regIndex;
        for(regIndex = 0; regIndex < NUMBER_OF_MEM_RESOURCES; regIndex++) {
			void* __iomem reg;
            struct resource* io = platform_get_resource(pdev, IORESOURCE_MEM, regIndex);
			if(!io) {
				dev_err(&pdev->dev, "No memory resource\n");
				RELEASE_AND_RETURN(-ENODEV);
			}
            
			reg = devm_ioremap_resource(fbdev->dev, io);
            if (IS_ERR(reg) ) {
				dev_err(&pdev->dev, "Failed to map device memory\n");
                RELEASE_AND_RETURN( PTR_ERR(reg) );
            }

            switch(regIndex)
            {
                case 0: fbdev->reg_dynclk = reg; dev_info(&pdev->dev, "DYNCLK : %08x\n", (u32)reg); break;  // Dynclk module
                case 1: fbdev->reg_vtc    = reg; dev_info(&pdev->dev, "VTC    : %08x\n", (u32)reg); break;  // Video Timing Controller
                case 2: fbdev->reg_vdma   = reg; dev_info(&pdev->dev, "VDMA   : %08x\n", (u32)reg); break;  // Video DMA
            }
        }
    }
    
    /* Allocate framebuffer */
    {
        int i;
        for(i = 0; i < FB_NUMBER_OF_FRAMES; i++) {
			void* virt;
            virt = dma_alloc_coherent(fbdev->dev, fbsize, &fbdev->frame[i].phys, GFP_KERNEL);
			if( !virt ) {
				dev_err(&pdev->dev, "Failed to allocate frame buffer\n");
				return -ENOMEM;
			}
            memset_io((void __iomem *)virt, 0, fbsize); // Clear
			fbdev->frame[i].virt = virt;
        }
    }
    
    /* Initialize other framebuffer parameters */
    fbdev->stride = fbdev->width*BYTES_PER_PIXEL;                   // Stride (bytes per line)

    fbdev->info.device = fbdev->dev;                                
    fbdev->info.pseudo_palette = fbdev->pseudo_palette;             // Pseudo color palette which is used to render console characters.
    fbdev->info.screen_base = (void __iomem*)fbdev->frame[0].virt;  // Virtual base address of frame buffer
    fbdev->info.fbops = &pynqz1_fb_ops;                             // function pointers which implements FB operations.
    fbdev->info.fix = pynqz1_fb_fix;                                // Fixed (Constant) framebuffer parameters.
	fbdev->info.fix.smem_start = fbdev->frame[0].phys;              // Physical address of frame buffer.
	fbdev->info.fix.smem_len = fbsize;                              // Length in bytes of the frame buffer.
	fbdev->info.fix.line_length = fbdev->stride;                    // Bytes per line (stride) of frame buffer lines.
	
    fbdev->info.var = pynqz1_fb_var;                                // Variable (changeable by request) parameters.
    fbdev->info.var.xres = fbdev->screen_param->width;              // X resolution of the frame buffer.
    fbdev->info.var.yres = fbdev->screen_param->height;             // Y resolution
    fbdev->info.var.xres_virtual = fbdev->info.var.xres;            // Virtual X resolution
    fbdev->info.var.yres_virtual = fbdev->info.var.yres;            // Virtual Y resolution
    fbdev->info.var.width  = (u32)(fbdev->info.var.xres*5/96/2);    // Physical screen width in millimeters
    fbdev->info.var.height = (u32)(fbdev->info.var.yres*5/96/2);    // Physical screen height in millimeters

    /* Enable dynamically generated clock */
    dynclk_write_reg(fbdev, OFST_DISPLAY_CTRL, 0);
    mdelay(1);
	if( dynclk_read_reg(fbdev, OFST_DISPLAY_STATUS) & (1u << BIT_CLOCK_RUNNING) ) {
		dev_err(&pdev->dev, "Failed to stop dynamic clock.\n");
		RELEASE_AND_RETURN(-ENOMEM);
	}

	{
		// Configure Dynamic clock module to generate required pixel rate.
        // Required pixel rate = (frame width*(frame height)*(vfreq)*5 for HDMI output.
        u32 clk_l   = dynclk_calculate_divider_config(fbdev->screen_param->dynclk.prescaler);
        u32 fb_l    = dynclk_calculate_divider_config(fbdev->screen_param->dynclk.multiplier);
        u32 div_    = dynclk_calculate_divider(fbdev->screen_param->dynclk.postscaler);
        u32 lock_l  = (u32)(lock_lookup[8 - 1] & 0xffffffffu);
		u32 filter_lock_h = (u32)(lock_lookup[8 - 1] >> 32) | ((filter_lookup_low[8 - 1] & 0x3ffu) << 16);

        dev_info(fbdev->dev, "DYNCLK CLK_L        : %08x\n", clk_l);
        dev_info(fbdev->dev, "DYNCLK FB_L         : %08x\n", fb_l);
        dev_info(fbdev->dev, "DYNCLK DIV          : %08x\n", div_);
        dev_info(fbdev->dev, "DYNCLK LOCK_L       : %08x\n", lock_l);
        dev_info(fbdev->dev, "DYNCLK FILTER_LOCK_H: %08x\n", filter_lock_h);

		dynclk_write_reg(fbdev, OFST_DISPLAY_CLK_L, clk_l);
		dynclk_write_reg(fbdev, OFST_DISPLAY_FB_L, fb_l);
		dynclk_write_reg(fbdev, OFST_DISPLAY_FB_H_CLK_H, 0);
		dynclk_write_reg(fbdev, OFST_DISPLAY_DIV, div_);
		dynclk_write_reg(fbdev, OFST_DISPLAY_LOCK_L, lock_l);
		dynclk_write_reg(fbdev, OFST_DISPLAY_FLTR_LOCK_H, filter_lock_h);		
	}

    dynclk_write_reg(fbdev, OFST_DISPLAY_CTRL, (1u << BIT_DISPLAY_START) );
    mdelay(1);
	if( !(dynclk_read_reg(fbdev, OFST_DISPLAY_STATUS) & (1u << BIT_CLOCK_RUNNING)) ) {
		dev_err(&pdev->dev, "Failed to start dynamic clock.\n");
		RELEASE_AND_RETURN(-ENOMEM);
	}
    dev_info(&pdev->dev, "DYNCLK configured.\n");

    /* Initialize Video Timing Controller */
    {
        u32 ctrl = 0;
        u32 status;
        u32 gfenc;
        struct pynqz1_fb_screen_param screen = *fbdev->screen_param;

        vtc_write_reg(fbdev, VTC_REG_CTL, VTC_CTL_RESET_MASK);  // Reset the controller.
        ctrl = vtc_read_reg(fbdev, VTC_REG_CTL);    // Read control register.
        ctrl &= ~(VTC_CTL_SW_MASK | VTC_CTL_GE_MASK | VTC_CTL_DE_MASK);
        ctrl &= ~VTC_CTL_ALLSS_MASK;
		//ctrl |= VTC_CTL_FIPSS_MASK;
		ctrl |= VTC_CTL_ACPSS_MASK;
		ctrl |= VTC_CTL_AVPSS_MASK;
		ctrl |= VTC_CTL_HSPSS_MASK;
		ctrl |= VTC_CTL_VSPSS_MASK;
		ctrl |= VTC_CTL_HBPSS_MASK;
		ctrl |= VTC_CTL_VBPSS_MASK;
		ctrl |= VTC_CTL_VCSS_MASK;
		ctrl |= VTC_CTL_VASS_MASK;
		ctrl |= VTC_CTL_VBSS_MASK;
		ctrl |= VTC_CTL_VSSS_MASK;
		ctrl |= VTC_CTL_VFSS_MASK;
		ctrl |= VTC_CTL_VTSS_MASK;
		ctrl |= VTC_CTL_HBSS_MASK;
		ctrl |= VTC_CTL_HSSS_MASK;
		ctrl |= VTC_CTL_HFSS_MASK;
		ctrl |= VTC_CTL_HTSS_MASK;

        ctrl |= VTC_CTL_GE_MASK;   /* Enable generator */
        ctrl |= VTC_CTL_RU_MASK;
        vtc_write_reg(fbdev, VTC_REG_CTL, ctrl);     

        status = vtc_read_reg(fbdev, VTC_REG_CTL);
        vtc_write_reg(fbdev, VTC_REG_CTL, status | VTC_CTL_RU_MASK);

        vtc_write_reg(fbdev, VTC_REG_GPOL, VTC_POL_ALLP_MASK);
        vtc_write_reg(fbdev, VTC_REG_GASIZE    , fbdev->width      | (fbdev->height << VTC_ASIZE_VERT_SHIFT)); // Horizontal/Vertical Active Size
        vtc_write_reg(fbdev, VTC_REG_GHSIZE    , screen.hFrameSize); // Horizontal Size
        vtc_write_reg(fbdev, VTC_REG_GVSIZE    , screen.vFrameSize | (screen.vFrameSize << VTC_VSIZE_F1_SHIFT)); // Vertical Size
        vtc_write_reg(fbdev, VTC_REG_GHSYNC    , screen.hSyncStart | (screen.hSyncEnd << VTC_SB_END_SHIFT)); // Horizontal Frame Sync
        vtc_write_reg(fbdev, VTC_REG_GVBHOFF   , screen.width      | (screen.width << VTC_SB_END_SHIFT)); // Horizontal Frame Sync
        vtc_write_reg(fbdev, VTC_REG_GVSYNC    , screen.vSyncStart | (screen.vSyncEnd << VTC_SB_END_SHIFT)); // Vertical Frame0 Sync
        vtc_write_reg(fbdev, VTC_REG_GVSHOFF   , screen.hSyncStart | (screen.hSyncStart << VTC_SB_END_SHIFT)); // Horizontal Frame Sync
        vtc_write_reg(fbdev, VTC_REG_GVBHOFF_F1, screen.width      | (screen.width << VTC_SB_END_SHIFT)); // Horizontal Frame Sync
        vtc_write_reg(fbdev, VTC_REG_GVSYNC_F1 , screen.vSyncStart | (screen.vSyncEnd << VTC_SB_END_SHIFT)); // Vertical Frame0 Sync
        vtc_write_reg(fbdev, VTC_REG_GVSHOFF_F1, screen.hSyncStart | (screen.hSyncStart << VTC_SB_END_SHIFT)); // Vertical Frame1 Sync

        gfenc = vtc_read_reg(fbdev, VTC_REG_GFENC);
        gfenc &= ~VTC_ENC_CPARITY_MASK;    // Clear VTC_ENC_CPARITY_MASK
        gfenc &= ~VTC_ENC_PROG_MASK;       // Clear VTC_ENC_PROG_MASK (0 = Progressive)
        gfenc |= 2;                         // Video format = RGB.
        vtc_write_reg(fbdev, VTC_REG_GFENC, gfenc); 
    }
    dev_info(&pdev->dev, "VTC configured.\n");

    /* Initialize VDMA */
    {
        u32 cr = 0;
        int i;

        /* Reset the VDMA channels */
        vdma_rx_write_reg(fbdev, VDMA_REG_CR, VDMA_CR_RESET_MASK);
        vdma_tx_write_reg(fbdev, VDMA_REG_CR, VDMA_CR_RESET_MASK);

        vdma_tx_write_reg(fbdev, VDMA_REG_CR, cr);

        vdma_write_reg(fbdev, VDMA_REG_MM2S_ADDR+VDMA_REG_HSIZE, fbdev->width*BYTES_PER_PIXEL);
        vdma_write_reg(fbdev, VDMA_REG_MM2S_ADDR+VDMA_REG_STRD_FRMDLY, fbdev->stride | (0 << VDMA_FRMDLY_SHIFT));    // FrameDelay = 0;

        for(i = 0; i < FB_NUMBER_OF_FRAMES; i++) {
            u32 reg = VDMA_REG_MM2S_ADDR+VDMA_REG_START_ADDR+i*VDMA_START_ADDR_LEN;
            vdma_write_reg(fbdev, reg, fbdev->frame[i].phys);
        }

        // Start VDMA TX channel
        cr = vdma_tx_read_reg(fbdev, VDMA_REG_CR); // Set RUN/STOP bit
        cr |= VDMA_CR_RUNSTOP_MASK;                   //
        vdma_tx_write_reg(fbdev, VDMA_REG_CR, cr); // /
        vdma_write_reg(fbdev, VDMA_REG_MM2S_ADDR+VDMA_REG_VSIZE, fbdev->height);  // Set VSIZE to start DMA

        // Start parking to the initial frame 0
        {
            u32 parkPtr = vdma_read_reg(fbdev, VDMA_REG_PARKPTR);
            parkPtr &= ~VDMA_PARKPTR_READREF_MASK;
            vdma_write_reg(fbdev, VDMA_REG_PARKPTR, parkPtr);
            cr = vdma_tx_read_reg(fbdev, VDMA_REG_CR) & ~VDMA_CR_TAIL_EN_MASK;
            vdma_tx_write_reg(fbdev, VDMA_REG_CR, cr);
        }
    }
    dev_info(&pdev->dev, "VDMA configured.\n");

    /* register framebuffer */
    rc = register_framebuffer(&fbdev->info);
    if( rc ) {
        dev_err(&pdev->dev, "Could not register frame buffer\n");
        RELEASE_AND_RETURN(1);
    }

    fbdev->flags |= PYNQZ1_FB_FLAGS_REGISTERED;
    dev_info(&pdev->dev, "PYNQ-Z1 Framebuffer Probed.\n");

    return 0;
}

/**
 * Remove this framebuffer driver
 */
static int pynqz1_fb_remove(struct platform_device *pdev)
{
	struct pynqz1_fb_device* fbdev = (struct pynqz1_fb_device*)platform_get_drvdata(pdev);
    if( fbdev != NULL ) {
        pynqz1_fb_release(fbdev);
    }
	return 0;
}

static const struct of_device_id pynqz1_fb_of_ids[] = {
	{ .compatible = "fugafuga,pynqz1_fb",},
	{}
};
MODULE_DEVICE_TABLE(of, pynqz1_fb_of_ids);

static struct platform_driver pynqz1_fb_driver = {
	.driver = {
		.name = "pynqz1-fb",
		.of_match_table = pynqz1_fb_of_ids,
	},
	.probe = pynqz1_fb_probe,
	.remove = pynqz1_fb_remove,
};

module_platform_driver(pynqz1_fb_driver);

MODULE_AUTHOR("fugafuga.org");
MODULE_DESCRIPTION("PYNQ-Z1 HDMI Framebuffer Driver");
MODULE_LICENSE("GPL");
