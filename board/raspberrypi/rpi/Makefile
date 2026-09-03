# SPDX-License-Identifier: GPL-2.0
#
# (C) Copyright 2012 Stephen Warren

obj-y	:= rpi.o
obj-y	+= lowlevel_init.o
obj-$(CONFIG_ARM64) += firmware_mem.o
obj-$(CONFIG_CMD_BOOTCIRCLE) += cmd_bootcircle.o bootcircle_smp.o

# The web loader's boot hook restores the circle_* network environment when a
# saved one has lost it, so it needs that environment to be defined at all.
ifdef CONFIG_CMD_BOOTCIRCLE
obj-$(CONFIG_CMD_HTTPD) += web_loader.o
endif

obj-$(CONFIG_GENERATE_ACPI_TABLE) += dsdt_generated.o
