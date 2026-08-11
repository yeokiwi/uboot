// SPDX-License-Identifier: GPL-2.0
/*
 * U-Boot console on UART0 (GPIO 14/15) instead of UART10 on BCM2712.
 *
 * The Raspberry Pi 5 has two usable serial ports, and the firmware picks the
 * one that is least useful to anybody chain-booting a Circle application: the
 * device tree it hands to U-Boot says
 *
 *	stdout-path = "serial10:115200n8"
 *
 * which is uart10, the PL011 inside the BCM2712, wired to the 3-pin debug
 * header next to the HDMI ports.  A Circle application as shipped talks on
 * UART0 of the RP1 southbridge - GPIO 14/15, pins 8 and 10 of the 40-pin
 * header - so the two halves of the boot come out of two different connectors.
 *
 * This moves U-Boot onto UART0 as well, so one cable sees the whole boot.
 * UART0 is a PL011 too, so the existing driver does the work.  What it needs
 * on top of that is:
 *
 *   - the address.  RP1 sits behind the PCIe controller that the firmware
 *     brings up for USB and network boot, mapped at 0x1f00000000, which is
 *     where Circle and Linux find it as well.  U-Boot cannot enumerate PCIe
 *     this early - the console is wanted before relocation - so the address is
 *     hardcoded, exactly as Circle hardcodes it.
 *
 *   - the clock.  The firmware leaves RP1's UART clock at 50 MHz and nothing
 *     in U-Boot changes it.  Circle assumes the same rate.
 *
 *   - the pin muxing.  The firmware only sets up the debug UART pins, so
 *     GPIO 14/15 have to be switched to their UART0 function by hand.  There
 *     is no RP1 pinctrl driver in U-Boot, and this runs before driver model
 *     could use one, so the two registers are programmed directly.
 *
 *   - the console selection.  A device bound from platform data has no device
 *     tree node, so it can never be what stdout-path points at.  Removing
 *     stdout-path leaves the serial uclass with nothing to prefer, and it
 *     falls back to the first serial device it has - which is this one,
 *     because platform data is bound before the device tree is scanned.
 *
 * See: Raspberry Pi RP1 Peripherals
 *      https://datasheets.raspberrypi.com/rp1/rp1-peripherals.pdf
 */

#define LOG_CATEGORY	UCLASS_SERIAL

#include <dm.h>
#include <log.h>
#include <serial.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <dm/device.h>
#include <dm/platform_data/serial_pl01x.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/libfdt.h>

/* For the PL011 register access and the ops this driver reuses */
#include "../../../drivers/serial/serial_pl01x_internal.h"

DECLARE_GLOBAL_DATA_PTR;

/* RP1, where the firmware maps it behind PCIe */
#define RP1_UART0_BASE			0x1f00030000UL
#define RP1_UART0_CLOCK			50000000	/* clk_uart */

#define RP1_IO_BANK0_BASE		0x1f000d0000UL
#define RP1_PADS_BANK0_BASE		0x1f000f0000UL

/* Bank 0 pin control: an 8 byte status/control pair per pin */
#define RP1_IO_BANK0_CTRL(pin)		(RP1_IO_BANK0_BASE + (pin) * 8 + 4)
#define RP1_CTRL_FUNCSEL		GENMASK(4, 0)
#define RP1_CTRL_OUTOVER		GENMASK(13, 12)
#define RP1_CTRL_OEOVER			GENMASK(15, 14)
#define RP1_CTRL_INOVER			GENMASK(17, 16)

/* Bank 0 pad control: a word per pin, after a header word */
#define RP1_PADS_BANK0_CTRL(pin)	(RP1_PADS_BANK0_BASE + 4 + (pin) * 4)
#define RP1_PADS_PDE			BIT(2)
#define RP1_PADS_PUE			BIT(3)
#define RP1_PADS_IE			BIT(6)
#define RP1_PADS_OD			BIT(7)

/* GPIO 14/15 reach UART0 through function 4 */
#define RP1_GPIO_TXD			14
#define RP1_GPIO_RXD			15
#define RP1_FUNCSEL_UART0		4

static bool rp1_uart0_present(void)
{
	/* RP1 only exists on the Raspberry Pi 5 */
	return of_machine_is_compatible("brcm,bcm2712");
}

/**
 * rp1_gpio_select_uart0() - hand a pin to UART0
 * @pin: GPIO number in RP1 bank 0
 *
 * Drives the pad from the peripheral with its input buffer on and no pulls,
 * then selects the UART0 function, leaving the output, output enable and
 * input overrides to the function itself.  This is what Circle's
 * CGPIOPin::SetAlternateFunction() does.
 */
static void rp1_gpio_select_uart0(unsigned int pin)
{
	u32 val;

	val = readl(RP1_PADS_BANK0_CTRL(pin));
	val &= ~(RP1_PADS_OD | RP1_PADS_PUE | RP1_PADS_PDE);
	val |= RP1_PADS_IE;
	writel(val, RP1_PADS_BANK0_CTRL(pin));

	val = readl(RP1_IO_BANK0_CTRL(pin));
	val &= ~(RP1_CTRL_FUNCSEL | RP1_CTRL_OUTOVER | RP1_CTRL_OEOVER |
		 RP1_CTRL_INOVER);
	val |= RP1_FUNCSEL_UART0;
	writel(val, RP1_IO_BANK0_CTRL(pin));
}

/**
 * rp1_uart0_drop_stdout_path() - stop the firmware naming uart10 as the console
 *
 * The serial uclass only falls back to the devices it has bound when the
 * device tree does not name a console, so the firmware's stdout-path has to
 * go.  The blob this edits before relocation is the firmware one, which a
 * payload started by bootcircle is handed as well: Circle does not read
 * stdout-path, and for a Linux payload the console belongs on the command
 * line once it has moved off the port the device tree describes.
 *
 * Deleting a property only ever shrinks the blob, so this cannot fail for
 * want of space.
 */
static void rp1_uart0_drop_stdout_path(void)
{
	void *blob = (void *)gd->fdt_blob;
	int node, ret;

	if (!blob)
		return;

	node = fdt_path_offset(blob, "/chosen");
	if (node < 0)
		return;

	ret = fdt_delprop(blob, node, "stdout-path");
	if (ret && ret != -FDT_ERR_NOTFOUND) {
		/*
		 * Nothing has a console yet, so this cannot be reported.  The
		 * device tree keeps naming uart10, U-Boot keeps talking to the
		 * debug header, and the muxed pins go unused.
		 */
		log_debug("cannot drop stdout-path: %s\n", fdt_strerror(ret));
	}
}

static int rp1_uart0_bind(struct udevice *dev)
{
	/*
	 * Driver model is built twice, but the pins only have to be muxed
	 * once, and by the second pass the console is already running on
	 * them.  The device tree edit is only useful in the first pass too:
	 * the blob is relocated once it is done with.
	 */
	if (!rp1_uart0_present() || gd->flags & GD_FLG_RELOC)
		return 0;

	rp1_gpio_select_uart0(RP1_GPIO_TXD);
	rp1_gpio_select_uart0(RP1_GPIO_RXD);

	rp1_uart0_drop_stdout_path();

	return 0;
}

static int rp1_uart0_probe(struct udevice *dev)
{
	/*
	 * The device is bound unconditionally, because platform data has to
	 * be bound before the device tree is scanned for the console fallback
	 * to reach it.  On anything but a Raspberry Pi 5 there is no RP1 to
	 * talk to, and its registers are not even mapped, so refuse here
	 * rather than fault: the device tree console stays in charge.
	 */
	if (!rp1_uart0_present())
		return -ENODEV;

	return pl01x_serial_probe(dev);
}

static const struct dm_serial_ops rp1_uart0_serial_ops = {
	.putc = pl01x_serial_putc,
	.pending = pl01x_serial_pending,
	.getc = pl01x_serial_getc,
	.setbrg = pl01x_serial_setbrg,
};

U_BOOT_DRIVER(rp1_uart0) = {
	.name		= "rp1_uart0",
	.id		= UCLASS_SERIAL,
	.bind		= rp1_uart0_bind,
	.probe		= rp1_uart0_probe,
	.ops		= &rp1_uart0_serial_ops,
	.flags		= DM_FLAG_PRE_RELOC,
	.plat_auto	= sizeof(struct pl01x_serial_plat),
	.priv_auto	= sizeof(struct pl01x_priv),
};

static struct pl01x_serial_plat rp1_uart0_plat = {
	.base	= RP1_UART0_BASE,
	.type	= TYPE_PL011,
	.clock	= RP1_UART0_CLOCK,
};

U_BOOT_DRVINFO(rp1_uart0) = {
	.name	= "rp1_uart0",
	.plat	= &rp1_uart0_plat,
};
