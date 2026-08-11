/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Protection of the resident EL3 firmware in low DRAM on BCM2712.
 *
 * On the Raspberry Pi 5 the armstub (armstub8-2712.bin, a TF-A BL31) stays
 * resident in the first 512 KiB of DRAM and provides PSCI to whatever it
 * launches.  U-Boot has no other reason to know about that region, but a
 * bare metal payload chain-booted from U-Boot does: it needs PSCI intact in
 * order to start the secondary cores.
 */

#ifndef _MACH_FW_MEM_H
#define _MACH_FW_MEM_H

#include <linux/errno.h>
#include <linux/types.h>

/*
 * Layout of the resident EL3 firmware, from the TF-A Raspberry Pi 5 platform
 * (plat/rpi/rpi5/include/platform_def.h):
 *
 *   0x100          PLAT_RPI3_TM_ENTRYPOINT   trusted mailbox entry point
 *   0x108..0x128   PLAT_RPI3_TM_HOLD_BASE    per-core hold state
 *   0x1000         BL31_BASE                 BL31 text, data and stacks
 *   0x80000        BL31_LIMIT                end of BL31, start of the payload
 *
 * Circle additionally reads the firmware's device tree pointer from the
 * 32-bit word at 0xF8 (ARM_DTB_PTR32 in circle/include/circle/startup.h),
 * which lives in the same first page.
 */
#define BCM2712_FW_MEM_BASE	0x0
#define BCM2712_FW_MEM_SIZE	0x80000

/**
 * rpi_fw_mem_protect() - reserve and snapshot the resident EL3 firmware
 *
 * Reserves BCM2712_FW_MEM_BASE..+BCM2712_FW_MEM_SIZE in the LMB (and in the
 * EFI memory map) so that no load command, image relocation or allocation can
 * be placed on top of the running PSCI implementation, and takes a copy of
 * the region so that rpi_fw_mem_restore() can undo any damage anyway.
 *
 * Does nothing on SoCs other than BCM2712.  Safe to call more than once.
 *
 * Return: 0 on success or if the SoC is not a BCM2712, negative on error
 */
#ifdef CONFIG_ARM64
int rpi_fw_mem_protect(void);
#else
static inline int rpi_fw_mem_protect(void) { return 0; }
#endif

/**
 * rpi_fw_mem_restore() - put the resident EL3 firmware back as found
 *
 * Copies the snapshot taken by rpi_fw_mem_protect() back over low DRAM and
 * makes it visible to the parked secondary cores, which run with their caches
 * off.  Call this immediately before handing control to a bare metal payload,
 * while nothing can trap to EL3.
 *
 * Where nothing overwrote the region this is a no-op that copies identical
 * bytes.  It reliably repairs the trusted mailbox and the hold state; if BL31
 * *code* was overwritten the parked cores have already faulted and no amount
 * of restoring will bring them back, which is why the reservation above is
 * the primary defence.
 *
 * Return: 0 on success, -ENODATA if no snapshot was taken
 */
#ifdef CONFIG_ARM64
int rpi_fw_mem_restore(void);
#else
static inline int rpi_fw_mem_restore(void) { return -ENODATA; }
#endif

/**
 * rpi_fw_dtb_pointer() - device tree address supplied by the firmware
 *
 * Returns the address the firmware passed in x0, saved by save_boot_params().
 * Unlike gd->fdt_blob this is always the original low-memory address, which
 * matters for payloads that expect to find it below 4 GiB.
 *
 * Return: firmware device tree address, or 0 if it did not look like one
 */
unsigned long rpi_fw_dtb_pointer(void);

#endif /* _MACH_FW_MEM_H */
