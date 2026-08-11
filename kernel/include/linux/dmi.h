/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_DMI_H
#define LKPI_LINUX_DMI_H
#include <linux/types.h>
/* SMBIOS/DMI tables, used for per-machine quirks. b1nix does not parse them, so
 * every match fails and no quirk is applied — which is the right default: a
 * quirk applied to the wrong machine is worse than one not applied. */
/* The match rules are part of the entry: a quirk table is written as a list of
 * (fields to compare, callback), and an entry without `matches` cannot express
 * the table upstream actually writes. */
struct dmi_strmatch {
	unsigned char slot;
	unsigned char exact_match;
	char substr[79];
};

struct dmi_system_id {
	int (*callback)(const struct dmi_system_id *);
	const char *ident;
	struct dmi_strmatch matches[4];
	void *driver_data;
};
static inline int dmi_check_system(const struct dmi_system_id *list)
{ (void)list; return 0; }
static inline const char *dmi_get_system_info(int field) { (void)field; return 0; }
/* EXACT compares the whole field, the ordinary form is a substring — and the
 * difference matters, because a substring match on a board name catches
 * machines the quirk was never written for. */
#define DMI_MATCH(a, b)       { .slot = (a), .substr = (b) }
#define DMI_EXACT_MATCH(a, b) { .slot = (a), .exact_match = 1, .substr = (b) }

/* The DMI fields a quirk table matches on. b1nix parses SMBIOS, so a match is a
 * real answer — the quirks that key off board name are how upstream works
 * around specific broken machines. */
#define DMI_NONE         0
#define DMI_BIOS_VENDOR  1
#define DMI_SYS_VENDOR   3
#define DMI_PRODUCT_NAME 4
#define DMI_BOARD_VENDOR 7
#define DMI_BOARD_NAME   8


/* Nothing is matched yet: b1nix parses SMBIOS but the table walk is not wired
 * to it, so a quirk list returns no match and the driver takes its default
 * path — the safe direction, since a spurious match applies a workaround meant
 * for a different machine. */
static inline bool dmi_match(int slot, const char *str)
{ (void)slot; (void)str; return false; }



#define DMI_PRODUCT_VERSION 5
#define DMI_BIOS_VERSION    2
#define DMI_PRODUCT_SKU     6


#define DMI_BIOS_DATE 3

#endif
