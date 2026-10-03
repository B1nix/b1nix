/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CLOCKSOURCE_IDS_H
#define LKPI_LINUX_CLOCKSOURCE_IDS_H
/* Which counter a clock snapshot's cycle count was read from. */
enum clocksource_ids {
	CSID_GENERIC = 0,
	CSID_ARM_ARCH_COUNTER,
	CSID_X86_TSC_EARLY,
	CSID_X86_TSC,
	CSID_X86_KVM_CLK,
	CSID_X86_ART,
	CSID_MAX,
};
#endif
