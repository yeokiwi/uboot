#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
#
# Populate the boot partition of a Raspberry Pi 5 SD card so that the firmware
# starts U-Boot, and U-Boot starts a Circle bare metal application.
#
# Usage:
#   mk-circle-sdcard.sh <mountpoint> <u-boot.bin> <circle-app-dir> [firmware-dir]
#
# <circle-app-dir>  a built Circle sample or application directory, the one
#                   containing kernel_2712.img
# [firmware-dir]    where to find bcm2712-rpi-5-b.dtb and bcm2712d0.dtbo,
#                   normally the boot/ directory of a Circle checkout after
#                   "make" has downloaded the firmware.  Defaults to
#                   <circle-app-dir>/../../boot
#
# Everything is copied, nothing is partitioned or formatted: point this at an
# already mounted FAT partition.

set -eu

if [ $# -lt 3 ] || [ $# -gt 4 ]; then
	sed -n '4,20p' "$0" >&2
	exit 1
fi

mnt=$1
uboot=$2
appdir=$3
fwdir=${4:-$appdir/../../boot}

here=$(dirname "$0")

for f in "$uboot" "$appdir/kernel_2712.img" "$here/config.txt"; do
	if [ ! -f "$f" ]; then
		echo "mk-circle-sdcard.sh: missing $f" >&2
		exit 1
	fi
done

if [ ! -d "$mnt" ]; then
	echo "mk-circle-sdcard.sh: $mnt is not a directory" >&2
	exit 1
fi

# The Raspberry Pi 5 keeps its firmware in EEPROM, so beyond the kernel image
# only the device tree and the D0 stepping overlay are needed on the card.
dtb=$fwdir/bcm2712-rpi-5-b.dtb
dtbo=$fwdir/bcm2712d0.dtbo

for f in "$dtb" "$dtbo"; do
	if [ ! -f "$f" ]; then
		echo "mk-circle-sdcard.sh: missing $f" >&2
		echo "  run 'make' in the boot/ directory of your Circle checkout" >&2
		exit 1
	fi
done

echo "Installing to $mnt:"

install_file() {
	echo "  $2"
	cp "$1" "$mnt/$2"
}

install_file "$here/config.txt" config.txt
install_file "$uboot" u-boot.bin
install_file "$appdir/kernel_2712.img" kernel_2712.img
install_file "$dtb" bcm2712-rpi-5-b.dtb

mkdir -p "$mnt/overlays"
install_file "$dtbo" overlays/bcm2712d0.dtbo

sync

echo
echo "Done.  On boot the firmware starts u-boot.bin, which loads"
echo "kernel_2712.img to 0x80000 and runs 'bootcircle' after a two second"
echo "delay.  Press a key during the countdown for a U-Boot prompt."
