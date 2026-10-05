/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_TTY_SYSFS_H
#define B1NIX_TTY_SYSFS_H

#include <b1nix/sysfs_attr.h>
#include <b1nix/types.h>

/*
 * A terminal device in /sys, laid out as Linux lays it out:
 *
 *   /sys/devices/<parent>/tty/<name>/   dev, uevent, subsystem -> class/tty
 *   /sys/class/tty/<name>               -> the device directory
 *   /sys/dev/char/<major>:<minor>       -> the device directory
 *
 * `parent` is the path under /sys/devices of what the terminal hangs off:
 * "virtual" for the consoles and anything else with no hardware behind it.
 * Returns the device directory, for attributes of the caller's own (tty0's
 * `active`), or NULL.
 */
struct sysfs_dir *tty_sysfs_publish(const char *parent, const char *name,
                                    int major, int minor);

/* /sys/class/tty/console/active: the consoles the command line named. */
void tty_sysfs_publish_console(void);

#endif
