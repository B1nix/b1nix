/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_KOBJECT_H
#define LKPI_LINUX_KOBJECT_H
/* kobject is declared alongside the device it is embedded in — the lifetime
 * rule only makes sense with both in view, so they share a header. */
#include <lkpi/device.h>
#include <linux/sysfs.h>

/* The sysfs operations a plain kobject's attributes go through. One shared
 * instance, because every kobj_attribute dispatches the same way — the
 * attribute carries the behaviour, not the kobject. */
extern const struct sysfs_ops kobj_sysfs_ops;


/* struct kobj_type, kobject_init_and_add() and the two-step kobject_init() /
 * kobject_add() split are declared in <lkpi/device.h>, included above: the type
 * and the lifetime rule belong with the object they are about. */

#endif
