/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Internal interface between the bootcircle command and its secondary core
 * trampoline.
 */

#ifndef _RPI_BOOTCIRCLE_H
#define _RPI_BOOTCIRCLE_H

/**
 * bootcircle_smp_probe_entry() - PSCI CPU_ON entry point for the self test
 *
 * Implemented in bootcircle_smp.S.  Never called directly: its address is
 * handed to PSCI CPU_ON, and it runs on a secondary core with the MMU and
 * caches off.  Reports in through the word whose address CPU_ON was given as
 * the context ID, then takes the core off again.
 */
void bootcircle_smp_probe_entry(void);

#endif /* _RPI_BOOTCIRCLE_H */
