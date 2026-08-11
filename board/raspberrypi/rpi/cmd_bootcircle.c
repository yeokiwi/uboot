// SPDX-License-Identifier: GPL-2.0+
/*
 * bootcircle - start a Circle bare metal application
 *
 * Circle (https://github.com/rsta2/circle) builds a raw binary, not a Linux
 * ARM64 Image, so booti rejects it; and it needs the MMU and caches off on
 * entry, which rules out go.  This command reproduces the hand-off the
 * Raspberry Pi armstub performs:
 *
 *   - entry at 0x80000 (MEM_KERNEL_START in circle/include/circle/memorymap64.h)
 *   - MMU and caches off
 *   - EL2, which Circle leaves for EL1 itself
 *   - the firmware device tree address in the 32-bit word at 0xF8
 *     (ARM_DTB_PTR32 in circle/include/circle/startup.h)
 *
 * On BCM2712 it also puts the resident EL3 firmware back the way the
 * VideoCore firmware left it, so that Circle's PSCI CPU_ON can still start
 * the secondary cores, and reports the state of those cores first so that a
 * multicore failure names itself instead of just hanging.
 */

#define LOG_CATEGORY	LOGC_BOOT

#include <bootm.h>
#include <command.h>
#include <cpu_func.h>
#include <log.h>
#include <mapmem.h>
#include <time.h>
#include <vsprintf.h>
#include <asm/arch/fw_mem.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <asm/psci.h>
#include <asm/system.h>
#include <asm/u-boot-arm.h>
#include <dm/device.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/libfdt.h>

#include "bootcircle.h"

/* circle/include/circle/memorymap64.h: MEM_KERNEL_START */
#define CIRCLE_ENTRY_ADDR	0x80000
/* circle/include/circle/startup.h: ARM_DTB_PTR32 */
#define CIRCLE_DTB_PTR32	0xf8

/* Cortex-A76: the core number lives in Aff1, Aff0 is 0 on every core */
#define CIRCLE_CORES		4
#define MPIDR_FOR_CORE(n)	((ulong)(n) << 8)

/* PSCI AFFINITY_INFO return values */
#define AFFINITY_INFO_ON	0
#define AFFINITY_INFO_OFF	1
#define AFFINITY_INFO_ON_PENDING 2

/*
 * The secondary core reports in with its caches off, so the flag needs a
 * cache line to itself.
 */
static u64 smp_probe_flag[ARCH_DMA_MINALIGN / sizeof(u64)]
	__aligned(ARCH_DMA_MINALIGN);

/*
 * A local SMC rather than arm_smccc_smc(): CONFIG_ARM_SMCCC selects
 * CONFIG_ARM_PSCI_FW, whose driver would bind against the firmware device
 * tree and take over sysreset on every Raspberry Pi sharing this binary.
 * This command only needs to make the call, not to own PSCI.
 */
static long bootcircle_smc(ulong function, ulong arg0, ulong arg1, ulong arg2)
{
	register ulong x0 asm("x0") = function;
	register ulong x1 asm("x1") = arg0;
	register ulong x2 asm("x2") = arg1;
	register ulong x3 asm("x3") = arg2;

	asm volatile("smc	#0\n"
		     : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
		     :
		     : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
		       "x12", "x13", "x14", "x15", "x16", "x17", "memory");

	return (long)x0;
}

static bool is_bcm2712(void)
{
	return of_machine_is_compatible("brcm,bcm2712");
}

static const char *affinity_info_str(long state)
{
	switch (state) {
	case AFFINITY_INFO_ON:
		return "ON";
	case AFFINITY_INFO_OFF:
		return "OFF";
	case AFFINITY_INFO_ON_PENDING:
		return "ON_PENDING";
	default:
		return "unknown";
	}
}

/**
 * bootcircle_smp_probe() - start a secondary core and take it off again
 *
 * Exercises the exact path Circle depends on.  PSCI_VERSION answering only
 * proves the SMC vector is intact; it says nothing about whether the parked
 * cores can still be woken, which is the part that breaks when something is
 * loaded over the firmware.
 *
 * Return: 0 if the core reported in, negative otherwise
 */
static int bootcircle_smp_probe(unsigned int core)
{
	ulong addr = (ulong)map_to_sysmem(smp_probe_flag);
	ulong start;
	long ret;

	smp_probe_flag[0] = 0;
	flush_dcache_range(addr, addr + sizeof(smp_probe_flag));

	ret = bootcircle_smc(ARM_PSCI_0_2_FN64_CPU_ON, MPIDR_FOR_CORE(core),
			     (ulong)bootcircle_smp_probe_entry, addr);
	if (ret != ARM_PSCI_RET_SUCCESS) {
		printf("  core %u: CPU_ON failed (%ld)%s\n", core, ret,
		       ret == ARM_PSCI_RET_ALREADY_ON ?
		       " - the core is already running, Circle will see this too" :
		       "");
		return -EIO;
	}

	for (start = get_timer(0); get_timer(start) < 500;) {
		invalidate_dcache_range(addr, addr + sizeof(smp_probe_flag));
		if (smp_probe_flag[0]) {
			printf("  core %u: started, MPIDR %#llx\n", core,
			       smp_probe_flag[0] & ~(1ULL << 63));
			break;
		}
		udelay(100);
	}

	if (!smp_probe_flag[0]) {
		printf("  core %u: CPU_ON returned success but the core never arrived\n",
		       core);
		return -ETIMEDOUT;
	}

	/* Wait for it to take itself off again before handing over. */
	for (start = get_timer(0); get_timer(start) < 500;) {
		ret = bootcircle_smc(ARM_PSCI_0_2_FN64_AFFINITY_INFO,
				     MPIDR_FOR_CORE(core), 0, 0);
		if (ret == AFFINITY_INFO_OFF)
			return 0;
		udelay(100);
	}

	printf("  core %u: did not go back off (%s)\n", core,
	       affinity_info_str(ret));

	return -ETIMEDOUT;
}

/**
 * bootcircle_psci_report() - say what state the secondary cores are in
 *
 * @selftest: also start and stop each core
 */
static void bootcircle_psci_report(bool selftest)
{
	unsigned int core;
	long version;

	/*
	 * Only BCM2712 is known to have an armstub that answers SMCs from
	 * here; do not fire one blindly at anything else.
	 */
	if (!is_bcm2712())
		return;

	version = bootcircle_smc(ARM_PSCI_0_2_FN_PSCI_VERSION, 0, 0, 0);
	if (version < 0) {
		printf("PSCI: not available (%ld); Circle will be single core\n",
		       version);
		return;
	}

	printf("PSCI: v%ld.%ld\n", (version >> 16) & 0xffff, version & 0xffff);

	for (core = 1; core < CIRCLE_CORES; core++) {
		long state = bootcircle_smc(ARM_PSCI_0_2_FN64_AFFINITY_INFO,
					    MPIDR_FOR_CORE(core), 0, 0);

		if (state == AFFINITY_INFO_OFF)
			continue;

		printf("PSCI: core %u is %s - Circle's CPU_ON will return ALREADY_ON (%d)\n",
		       core, affinity_info_str(state),
		       ARM_PSCI_RET_ALREADY_ON);
	}

	if (!selftest)
		return;

	printf("PSCI self test:\n");
	for (core = 1; core < CIRCLE_CORES; core++)
		bootcircle_smp_probe(core);
}

static int do_bootcircle(struct cmd_tbl *cmdtp, int flag, int argc,
			 char *const argv[])
{
	ulong entry = CIRCLE_ENTRY_ADDR;
	bool selftest = false;
	ulong dtb;
	void *ptr;

	if (argc > 1 && !strcmp(argv[1], "-t")) {
		selftest = true;
		argc--;
		argv++;
	}

	if (argc > 3)
		return CMD_RET_USAGE;

	if (argc > 1)
		entry = hextoul(argv[1], NULL);

	dtb = argc > 2 ? hextoul(argv[2], NULL) : rpi_fw_dtb_pointer();

	if (entry < CIRCLE_ENTRY_ADDR) {
		printf("Refusing to start at %#lx: the resident EL3 firmware lives below %#x\n",
		       entry, CIRCLE_ENTRY_ADDR);
		return CMD_RET_FAILURE;
	}

	if (dtb && fdt_magic(dtb) != FDT_MAGIC) {
		printf("Warning: no device tree at %#lx\n", dtb);
		dtb = 0;
	}

	/* Circle reads the pointer from a 32-bit word */
	if (dtb > U32_MAX) {
		printf("Warning: device tree at %#lx is above 4 GiB, ignoring\n",
		       dtb);
		dtb = 0;
	}

	bootcircle_psci_report(selftest);

	printf("Starting Circle at %#lx (DTB %#lx)\n", entry, dtb);

	bootm_final(0);

	/*
	 * Undo anything that landed on the resident EL3 firmware, then stamp
	 * the device tree pointer where Circle looks for it.  Order matters:
	 * the restore covers the word at CIRCLE_DTB_PTR32 as well.
	 */
	rpi_fw_mem_restore();

	ptr = map_sysmem(CIRCLE_DTB_PTR32, sizeof(u32));
	writel((u32)dtb, ptr);
	flush_dcache_range(CIRCLE_DTB_PTR32,
			   CIRCLE_DTB_PTR32 + ARCH_DMA_MINALIGN);
	unmap_sysmem(ptr);

	cleanup_before_linux();

	/*
	 * Already in EL2, so this branches straight to the entry point with
	 * x0 intact - the same state armstub8-2712 hands to Circle.
	 */
	armv8_switch_to_el2(dtb, 0, 0, 0, entry, ES_TO_AARCH64);

	return CMD_RET_FAILURE;
}

U_BOOT_CMD(bootcircle, 4, 0, do_bootcircle,
	   "start a Circle bare metal application",
	   "[-t] [entry [dtb]]\n"
	   "    - Start the Circle application already loaded at 'entry'\n"
	   "      (default 0x80000), passing the device tree at 'dtb'\n"
	   "      (default: the one the firmware gave U-Boot).\n"
	   "      -t  first start and stop each secondary core, to check\n"
	   "          that PSCI can still bring them up.\n"
);
