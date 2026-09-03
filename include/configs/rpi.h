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
 *   web_probe  TFTPs web_file from web_server, or from whatever DHCP left
 *              in serverip;
 *   web_ui     runs the server, which owns the board until the page's
 *              "Continue boot" button is pressed or Ctrl-C is typed.
 *
 * The probe is a real transfer of a real file rather than a ping, and that
 * is the point: it answers "can this board be given a file right now" with
 * the very mechanism the answer depends on, instead of with ICMP - which
 * plenty of servers drop while serving perfectly well.  It also puts the
 * decision in the hands of whoever runs the server: drop test.img into the
 * TFTP root to have the next boot stop at the web UI, take it away to have
 * the board boot straight through.  A server that answers with "file not
 * found" is a decision too, and an immediate one - U-Boot does not retry it.
 *
 * Five seconds, near enough, is the cost when nothing answers.  Two limits
 * set it, and neither can be left at its default: tftptimeout (1 s) and
 * tftptimeoutcountmax (4) bound the transfer to about five seconds, and
 * CONFIG_ARP_TIMEOUT (1 s in rpi5_circle_net_defconfig, against a 5 s
 * default) bounds the case where the server does not answer ARP at all -
 * five retries of it, and no environment variable can shorten them.  The two
 * TFTP limits are saved and put back around the probe so that a real
 * circle_netboot keeps the patience it was written with.
 *
 * web_addr is where the probe file lands.  0x80000 is circle_addr, which is
 * safe because circle_boot only jumps after its own fatload has succeeded -
 * it never boots what the probe left there.  httpd_addr is set explicitly
 * for the same reason in reverse: "tftpboot <addr>" moves image_load_addr,
 * and the upload buffer must not follow the probe around.
 *
 * web_tries is a word list, not a count, because hush has no arithmetic;
 * "for" over its words is the only bounded loop there is.  One attempt keeps
 * to the five seconds asked of it - add a word for a second attempt if the
 * adapter's link is slow to come up.
 *
 * web_force=1 skips the probe and always serves the page.
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
	"web_file=test.img\0" \
	"web_addr=0x80000\0" \
	"web_tries=1\0" \
	"web_tftp_ms=1000\0" \
	"web_tftp_max=4\0" \
	"httpd_addr=0x1000000\0" \
	"web_net=run net_usb\0" \
	"web_ip=if test -z ${ipaddr}; then dhcp; else true; fi\0" \
	"web_probe=" \
		"setenv web_host ${web_server}; " \
		"if test -z ${web_host}; then setenv web_host ${serverip}; fi; " \
		"if test -z ${web_host}; then " \
			"echo 'Web loader: no web_server or serverip to probe'; " \
			"false; " \
		"else " \
			"echo Web loader: asking ${web_host} for ${web_file}; " \
			"setenv web_t ${tftptimeout}; " \
			"setenv web_c ${tftptimeoutcountmax}; " \
			"setenv tftptimeout ${web_tftp_ms}; " \
			"setenv tftptimeoutcountmax ${web_tftp_max}; " \
			"setenv web_ok 0; " \
			"for web_i in ${web_tries}; do " \
				"if test ${web_ok} -eq 0; then " \
					"if tftpboot ${web_addr} ${web_host}:${web_file}; then " \
						"setenv web_ok 1; " \
					"fi; " \
				"fi; " \
			"done; " \
			"setenv tftptimeout ${web_t}; " \
			"setenv tftptimeoutcountmax ${web_c}; " \
			"test ${web_ok} -eq 1; " \
		"fi\0" \
	"web_ui=httpd ${web_port}\0" \
	"web_start=" \
		"if run web_net && run web_ip; then " \
			"if test \"${web_force}\" = 1 || run web_probe; then " \
				"run web_ui; " \
			"else " \
				"echo Web loader: no ${web_file}, continuing boot; " \
			"fi; " \
		"else " \
			"echo 'Web loader: no network, continuing boot'; " \
		"fi\0" \
	"web_preboot=" \
		"if test ${web_enable} -eq 1; then run web_start; " \
		"else echo 'Web loader: disabled (web_enable=0)'; fi\0"
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
		"if test ${circle_netcon} -eq 1; then run nc; fi\0" \
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
