// SPDX-License-Identifier: GPL-2.0
/*
 * Protection of the resident EL3 firmware in low DRAM on BCM2712.
 *
 * The Raspberry Pi 5 armstub (armstub8-2712.bin, a TF-A BL31) does not go
 * away once it has launched its payload: it stays resident in the first
 * 512 KiB of DRAM and services PSCI calls from EL3 for the lifetime of the
 * system.  Cores 1-3 never leave it, they sit in its hold loop waiting for a
 * PSCI CPU_ON to hand them an entry point.
 *
 * U-Boot itself never notices, because core 0 is not executing any of that
 * code.  But DRAM bank 0 starts at 0x0, so as far as the LMB allocator, image
 * relocation and every load command are concerned, the region the firmware
 * lives in is ordinary free memory.  A payload loaded over it boots quite
 * happily on core 0 and then fails the moment it issues an SMC to start the
 * other cores.
 *
 * Reserve the region so nothing can be placed there, and keep a copy so that
 * a payload can be handed a pristine one regardless.
 */

#define LOG_CATEGORY	LOGC_BOARD

#include <cpu_func.h>
#include <efi_loader.h>
#include <lmb.h>
#include <log.h>
#include <malloc.h>
#include <mapmem.h>
#include <memalign.h>
#include <asm/arch/fw_mem.h>
#include <dm/device.h>
#include <linux/errno.h>
#include <linux/string.h>

static void *fw_mem_snapshot;

static bool is_bcm2712(void)
{
	return of_machine_is_compatible("brcm,bcm2712");
}

/*
 * Out of line so that the compiler cannot reason about the source or
 * destination being the NULL pointer: the region deliberately starts at
 * physical address 0.
 */
static noinline void fw_mem_copy(void *dst, const void *src, size_t len)
{
	memcpy(dst, src, len);
}

int rpi_fw_mem_protect(void)
{
	phys_addr_t base = BCM2712_FW_MEM_BASE;
	void *low;
	int ret;

	if (!is_bcm2712())
		return 0;

	if (CONFIG_IS_ENABLED(LMB)) {
		ret = lmb_alloc_mem(LMB_MEM_ALLOC_ADDR, 0, &base,
				    BCM2712_FW_MEM_SIZE, LMB_NOOVERWRITE);
		/*
		 * -EEXIST just means someone got there first, which is as good
		 * an outcome as reserving it ourselves.
		 */
		if (ret && ret != -EEXIST)
			log_warning("Could not reserve firmware memory at %#x (%d); loading below %#x may break PSCI\n",
				    BCM2712_FW_MEM_BASE, ret,
				    BCM2712_FW_MEM_BASE + BCM2712_FW_MEM_SIZE);
	}

	if (fw_mem_snapshot)
		return 0;

	fw_mem_snapshot = memalign(ARCH_DMA_MINALIGN, BCM2712_FW_MEM_SIZE);
	if (!fw_mem_snapshot) {
		log_warning("No memory for a firmware snapshot\n");
		return -ENOMEM;
	}

	low = map_sysmem(BCM2712_FW_MEM_BASE, BCM2712_FW_MEM_SIZE);
	fw_mem_copy(fw_mem_snapshot, low, BCM2712_FW_MEM_SIZE);
	unmap_sysmem(low);

	log_debug("Snapshotted %#x bytes of firmware from %#x\n",
		  BCM2712_FW_MEM_SIZE, BCM2712_FW_MEM_BASE);

	return 0;
}

int rpi_fw_mem_restore(void)
{
	void *low;

	if (!fw_mem_snapshot)
		return -ENODATA;

	low = map_sysmem(BCM2712_FW_MEM_BASE, BCM2712_FW_MEM_SIZE);
	fw_mem_copy(low, fw_mem_snapshot, BCM2712_FW_MEM_SIZE);
	unmap_sysmem(low);

	/*
	 * The parked secondary cores are running with their caches off, and
	 * we may just have rewritten instructions.  Push the lot out to DRAM
	 * and drop any stale copies from the instruction cache.
	 */
	flush_dcache_range(BCM2712_FW_MEM_BASE,
			   BCM2712_FW_MEM_BASE + BCM2712_FW_MEM_SIZE);
	invalidate_icache_all();

	return 0;
}
