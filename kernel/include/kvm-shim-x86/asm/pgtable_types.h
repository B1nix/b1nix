/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_PGTABLE_TYPES_H
#define KVM_SHIM_ASM_PGTABLE_TYPES_H
#include <linux/types.h>
/* Page-table levels as KVM counts them (4K is 1). */
enum pg_level {
	PG_LEVEL_NONE,
	PG_LEVEL_4K,
	PG_LEVEL_2M,
	PG_LEVEL_1G,
	PG_LEVEL_512G,
	PG_LEVEL_256T,
	PG_LEVEL_NUM
};
#endif
