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
	CIRCLE_PREBOOT_ENV \
	"circle_load=" \
		"fatload ${circle_dev} ${circle_part} " \
			"${circle_addr} ${circle_kernel}\0" \
	"circle_boot=" \
		"if run circle_load; then " \
			"bootcircle ${circle_addr}; " \
		"fi\0" \
	CIRCLE_NET_ENV_SETTINGS

/*
 * Networking.  Two interfaces are possible on a Raspberry Pi 5 and either can
 * serve TFTP and the network console:
 *
 *   - the onboard MAC, a Cadence GEM inside the RP1 southbridge, which hangs
 *     off PCIe, so a network boot has to bring PCIe and RP1 up first;
 *   - a USB Ethernet adapter, which needs "usb start" and, on this board, the
 *     RP1's DesignWare USB3 controllers.
 *
 * netdev names the one in use.  It is an interface number rather than a device
 * name because the two drivers name their devices quite differently, and
 * because "net list" - which is what tells you which is which - prints the
 * numbers.  The onboard MAC binds during board_late_init() and so comes up as
 * eth0 whenever it is present; a USB adapter binds later, at "usb start".
 *
 * Only offered where a driver stack is actually built in - see
 * rpi5_circle_net_defconfig.
 */
#if defined(CONFIG_MACB) || defined(CONFIG_USB_ETHER_RTL8152)

#define CIRCLE_NET_PREBOOT	"; run circle_netpreboot"

/*
 * The storage loader, U-Boot's web UI for putting a file on the SD card
 * (see doc/usage/cmd/httpd.rst).  Every boot asks one question before the
 * boot command runs: is there anyone out there who wants to give this board
 * a file?
 *
 *   web_net    brings the USB Ethernet adapter up and makes it the active
 *              interface - the same "run net_usb" as by hand;
 *   web_ip     leaves a static ipaddr alone and otherwise asks DHCP;
 *   web_ping   probes web_server, or whatever DHCP left in serverip;
 *   web_ui     runs the server, which owns the board until the page's
 *              "Continue boot" button is pressed or Ctrl-C is typed.
 *
 * Any of those failing means the boot carries on as it always did, and says
 * which one gave up.  Set web_enable to 0 (and saveenv) to skip the whole
 * thing, which also skips the "usb start" that comes with it.
 *
 * web_tries is a word list, not a count, because hush has no arithmetic -
 * "for" over its words is the only bounded loop available.  Two attempts,
 * because a USB adapter's link is often not up for the first one: the driver
 * waits five seconds for it in r8152_init_common() and then carries on
 * regardless, so an attempt made while a switch is still negotiating gets no
 * reply through no fault of the server.  Each attempt costs ping's ten
 * second timeout when nothing answers.
 *
 * web_force=1 skips the probe and always serves the page.  It is there
 * because a reply is not the same question as "is the server there": plenty
 * of machines, Windows hosts especially, drop ICMP echo by default while
 * serving HTTP perfectly well.
 *
 * The "else true" arms are load-bearing, not padding.  U-Boot's hush returns
 * the *condition's* status for an "if" whose test fails and which has no else
 * branch - unlike a POSIX shell, where such an if succeeds.  So a web_ip of
 * "if test -z ${ipaddr}; then dhcp; fi" reports failure precisely when ipaddr
 * is already set and there is nothing to do, which then breaks the && chain
 * in web_start and skips the loader on exactly the boards that were ready
 * for it.  web_force is compared as a quoted string for the same class of
 * reason: "test ${unset} -eq 1" is *true* in U-Boot's test.
 */
#ifdef CONFIG_CMD_HTTPD
#define CIRCLE_WEB_ENV_SETTINGS \
	"web_enable=1\0" \
	"web_force=0\0" \
	"web_server=\0" \
	"web_port=80\0" \
	"web_tries=1 2\0" \
	"web_net=run net_usb\0" \
	"web_ip=if test -z ${ipaddr}; then dhcp; else true; fi\0" \
	"web_ping=" \
		"setenv web_host ${web_server}; " \
		"if test -z ${web_host}; then setenv web_host ${serverip}; fi; " \
		"if test -z ${web_host}; then " \
			"echo 'Web loader: no web_server or serverip to probe'; " \
			"false; " \
		"else " \
			"echo Web loader: probing ${web_host}; " \
			"setenv web_ok 0; " \
			"for web_i in ${web_tries}; do " \
				"if test ${web_ok} -eq 0; then " \
					"if ping ${web_host}; then setenv web_ok 1; fi; " \
				"fi; " \
			"done; " \
			"test ${web_ok} -eq 1; " \
		"fi\0" \
	"web_ui=httpd ${web_port}\0" \
	"web_start=" \
		"if run web_net && run web_ip; then " \
			"if test \"${web_force}\" = 1 || run web_ping; then " \
				"run web_ui; " \
			"else " \
				"echo 'Web loader: no reply, continuing boot'; " \
			"fi; " \
		"else " \
			"echo 'Web loader: no network, continuing boot'; " \
		"fi\0" \
	"web_preboot=" \
		"if test ${web_enable} -eq 1; then run web_start; else true; fi\0"
#else
/* "run" on an undefined variable fails, so it has to exist either way */
#define CIRCLE_WEB_ENV_SETTINGS \
	"web_preboot=true\0"
#endif

#define CIRCLE_NET_ENV_SETTINGS \
	"autoload=no\0" \
	"hostname=rpi5-uboot\0" \
	"netdev=eth0\0" \
	"net_pick=setenv ethrotate no; setenv ethact ${netdev}\0" \
	"net_onboard=setenv netdev eth0; run net_pick\0" \
	"net_usb=usb start; setenv netdev eth1; run net_pick\0" \
	"net_up=run net_pick; dhcp\0" \
	"nc_base=serial\0" \
	"nc_on=setenv stdout ${nc_base},nc; setenv stderr ${nc_base},nc; " \
		"setenv stdin ${nc_base},nc\0" \
	"nc_off=setenv stdout ${nc_base}; setenv stderr ${nc_base}; " \
		"setenv stdin ${nc_base}\0" \
	"nc=run net_up; run nc_on\0" \
	"circle_usbnet=0\0" \
	"circle_netcon=0\0" \
	"circle_netpreboot=" \
		"if test ${circle_usbnet} -eq 1; then usb start; fi; " \
		"if test ${circle_netcon} -eq 1; then run nc; fi; " \
		"run web_preboot\0" \
	CIRCLE_WEB_ENV_SETTINGS \
	"circle_tftp=tftp ${circle_addr} ${circle_kernel}\0" \
	"circle_netboot=" \
		"if run net_up && run circle_tftp; then " \
			"bootcircle ${circle_addr}; " \
		"fi\0" \
	"circle_netcheck=" \
		"echo '-- pci --'; pci enum; pci; " \
		"echo '-- rp1 --'; dm tree; " \
		"echo '-- clk --'; clk dump; " \
		"echo '-- usb --'; usb start; usb tree; " \
		"echo '-- mdio --'; mdio list; " \
		"echo '-- eth --'; net list\0"
#else
#define CIRCLE_NET_PREBOOT	""
#define CIRCLE_NET_ENV_SETTINGS
#endif

/*
 * CIRCLE_NET_PREBOOT is a string literal, so it concatenates into the value.
 * Without a network stack it is empty and circle_preboot is exactly what it
 * always was - which matters, because "run" on an undefined variable fails.
 */
#define CIRCLE_PREBOOT_ENV \
	"circle_preboot=" \
		"if test ${circle_full_hw} -eq 1; then " \
			"pci enum; usb start; " \
		"fi" CIRCLE_NET_PREBOOT "\0"

#endif

#endif
