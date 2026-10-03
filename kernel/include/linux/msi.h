/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_MSI_H
#define LKPI_LINUX_MSI_H
#include <linux/types.h>
/* A message-signalled interrupt as the device writes it. */
struct msi_msg {
	union {
		u32 address_lo;
		struct {
			u32 reserved_0     : 2,
			    dest_mode_logical : 1,
			    redirect_hint  : 1,
			    reserved_1     : 1,
			    virt_destid_8_14 : 7,
			    destid_0_7     : 8,
			    base_address   : 12;
		} arch_addr_lo;
	};
	u32 address_hi;
	union {
		u32 data;
		struct {
			u32 vector : 8,
			    delivery_mode : 3,
			    dest_mode_logical : 1,
			    reserved : 2,
			    active_low : 1,
			    is_level : 1;
		} arch_data;
	};
};
#endif
