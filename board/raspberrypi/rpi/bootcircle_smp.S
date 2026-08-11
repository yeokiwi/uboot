/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Secondary core trampoline for the bootcircle PSCI self test.
 */

#include <config.h>
#include <linux/linkage.h>
#include <asm/psci.h>

/*
 * bootcircle_smp_probe_entry() - entry point handed to PSCI CPU_ON
 *
 * Runs on a secondary core with the MMU and caches off, at the same exception
 * level as the caller.  x0 holds the context ID that CPU_ON was given, which
 * is the address of the word to report through.
 *
 * Stores its own MPIDR (with bit 63 set, so the value is never zero) and then
 * takes the core back off again, leaving it in the state a bare metal payload
 * expects to find it in.
 */
ENTRY(bootcircle_smp_probe_entry)
	mrs	x1, mpidr_el1
	orr	x1, x1, #(1 << 63)
	str	x1, [x0]
	dsb	sy

	ldr	w0, =ARM_PSCI_0_2_FN_CPU_OFF
	smc	#0

	/* CPU_OFF does not return.  Park the core if it somehow does. */
0:	wfi
	b	0b
ENDPROC(bootcircle_smp_probe_entry)
