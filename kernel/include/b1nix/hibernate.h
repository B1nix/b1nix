/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_HIBERNATE_H
#define B1NIX_HIBERNATE_H

#include <b1nix/types.h>

/*
 * Hibernation, suspend-to-disk, ACPI S4 in the platform's terms (M135).
 *
 * `echo disk > /sys/power/state` copies every page of RAM the kernel is using
 * into free memory while nothing runs, writes the copy to the resume device
 * (a swap area, found by `resume=` or /sys/power/resume) and switches the
 * machine off -- or reboots it, as /sys/power/disk says. The next boot finds
 * the image before it mounts anything, reads it back, puts every page where
 * it was and continues the kernel that wrote it, which returns from its write
 * to /sys/power/state as though nothing had happened.
 */

/* Is there a resume device, on an architecture that can do this? */
int hibernate_available(void);
/* Why not, when it is not. */
const char *hibernate_why_not(void);

/* The whole of it: 0 once the image came back, a negative errno when it was
 * refused or could not be written. Called from suspend_enter("disk"). */
int hibernate_enter(void);

/* At boot, before any filesystem is mounted: restore an image if the resume
 * device holds one. Returns only when there is none, or it is unusable. */
void hibernate_resume_from_disk(void);

/* /sys/power/disk: what happens once the image is written. */
const char *hibernate_disk_modes(void);
int hibernate_set_disk_mode(const char *name);
/* /sys/power/resume: the device, by name or LABEL=. */
const char *hibernate_resume_device(void);
int hibernate_set_resume_device(const char *spec);

#endif /* B1NIX_HIBERNATE_H */
