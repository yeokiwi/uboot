/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2012-2016 Stephen Warren
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include <linux/sizes.h>
#include <asm/arch/timer.h>

#ifndef __ASSEMBLY__
#include <asm/arch/base.h>
#endif

/* Use SoC timer for AArch32, but architected timer for AArch64 */
#ifndef CONFIG_ARM64
#define CFG_SYS_TIMER_RATE		1000000
#define CFG_SYS_TIMER_COUNTER	\
	(&((struct bcm2835_timer_regs *)BCM2835_TIMER_PHYSADDR)->clo)
#endif

/* Memory layout */
#define CFG_SYS_SDRAM_BASE		0x00000000
#define CFG_SYS_UBOOT_BASE		CONFIG_TEXT_BASE
/*
 * The board really has 256M. However, the VC (VideoCore co-processor) shares
 * the RAM, and uses a configurable portion at the top. We tell U-Boot that a
 * smaller amount of RAM is present in order to avoid stomping on the area
 * the VC uses.
 */
#define CFG_SYS_SDRAM_SIZE		SZ_128M

#ifdef CONFIG_CMD_BOOTCIRCLE
/*
 * Chain-booting a Circle bare metal application.
 *
 * circle_addr is fixed by Circle itself (MEM_KERNEL_START); loading anywhere
 * below it would land on the resident EL3 firmware, which bootcircle refuses
 * and the LMB reservation prevents.
 *
 * circle_full_hw selects how much hardware U-Boot brings up before handing
 * over.  The default leaves the RP1, PCIe and the framebuffer as the firmware
 * left them, which is the safest state for Circle to initialise from.  Set it
 * to 1 and save the environment to get a USB keyboard and HDMI console in
 * U-Boot itself.
 */
#define CFG_EXTRA_ENV_SETTINGS \
	"circle_kernel=kernel_2712.img\0" \
	"circle_addr=0x80000\0" \
	"circle_dev=mmc\0" \
	"circle_part=0:1\0" \
	"circle_full_hw=0\0" \
	"circle_preboot=" \
		"if test ${circle_full_hw} -eq 1; then " \
			"pci enum; usb start; " \
		"fi\0" \
	"circle_load=" \
		"fatload ${circle_dev} ${circle_part} " \
			"${circle_addr} ${circle_kernel}\0" \
	"circle_boot=" \
		"if run circle_load; then " \
			"bootcircle ${circle_addr}; " \
		"fi\0" \
	CIRCLE_NET_ENV_SETTINGS

/*
 * Ethernet on the Raspberry Pi 5 is a Cadence GEM inside RP1, behind PCIe,
 * so a network boot has to bring PCIe and RP1 up first.  Only offered where
 * the driver stack is actually built in - see rpi5_circle_net_defconfig.
 */
#ifdef CONFIG_MACB
#define CIRCLE_NET_ENV_SETTINGS \
	"circle_tftp=tftp ${circle_addr} ${circle_kernel}\0" \
	"circle_netboot=" \
		"if dhcp && run circle_tftp; then " \
			"bootcircle ${circle_addr}; " \
		"fi\0" \
	"circle_netcheck=" \
		"echo '-- pci --'; pci enum; pci; " \
		"echo '-- rp1 --'; dm tree; " \
		"echo '-- mdio --'; mdio list; " \
		"echo '-- eth --'; net list\0"
#else
#define CIRCLE_NET_ENV_SETTINGS
#endif

#endif

#endif
