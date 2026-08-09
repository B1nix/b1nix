/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_DMI_H
#define LKPI_LINUX_DMI_H
#include <linux/types.h>
/* SMBIOS/DMI tables, used for per-machine quirks. b1nix does not parse them, so
 * every match fails and no quirk is applied — which is the right default: a
 * quirk applied to the wrong machine is worse than one not applied. */
struct dmi_system_id { int (*callback)(const struct dmi_system_id *);
                       const char *ident; void *driver_data; };
static inline int dmi_check_system(const struct dmi_system_id *list)
{ (void)list; return 0; }
static inline const char *dmi_get_system_info(int field) { (void)field; return 0; }
#define DMI_MATCH(a, b) { }
#endif
