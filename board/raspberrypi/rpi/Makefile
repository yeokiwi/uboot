# SPDX-License-Identifier: GPL-2.0
#
# (C) Copyright 2012 Stephen Warren

obj-y	:= rpi.o
obj-y	+= lowlevel_init.o
obj-$(CONFIG_ARM64) += firmware_mem.o
obj-$(CONFIG_CMD_BOOTCIRCLE) += cmd_bootcircle.o bootcircle_smp.o
obj-$(CONFIG_RPI_UART0_CONSOLE) += rp1_uart0.o

obj-$(CONFIG_GENERATE_ACPI_TABLE) += dsdt_generated.o
