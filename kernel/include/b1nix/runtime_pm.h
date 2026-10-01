/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_RUNTIME_PM_H
#define B1NIX_RUNTIME_PM_H

#include <b1nix/types.h>

/*
 * Runtime power management (M135): a device that has been idle for its
 * autosuspend delay is put into a low-power state while the machine keeps
 * running, and brought back the moment it is used — what Linux's
 * pm_runtime_* does, reduced to the part the drivers here need.
 *
 * A driver registers with its two callbacks, brackets every use with
 * rpm_get() / rpm_put(), and says where its sysfs directory is, so the
 * device's power/ files (control, runtime_status, runtime_active_time,
 * runtime_suspended_time, autosuspend_delay_ms) appear there. As on Linux the
 * default is control=on — nothing is suspended until userspace (TLP,
 * powertop) writes auto.
 */

typedef int (*rpm_fn)(void *ctx);

/* Returns an id for the calls below, or -1. `sysfs_dir` is the device's
 * directory under /sys ("devices/pci0000:00/0000:00:0d.0"); may be NULL. */
int rpm_register(const char *name, rpm_fn suspend, rpm_fn resume, void *ctx,
                 const char *sysfs_dir, u32 autosuspend_ms);

/* Take the device for a use, resuming it first if it is suspended. Returns 0,
 * or -1 when it could not be brought back (the caller must not touch it). */
int rpm_get(int id);
/* Done with it: it may suspend once it has been idle for the delay. */
void rpm_put(int id);

/* Every device back up and held there, for a system suspend (which saves
 * device state that must be live), and let go again after it. */
void rpm_hold_all(void);
void rpm_release_all(void);

#endif /* B1NIX_RUNTIME_PM_H */
