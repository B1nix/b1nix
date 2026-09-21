/* SPDX-License-Identifier: GPL-2.0-only */
/* System suspend (M129).
 *
 * One state so far, and it is the honest one: `freeze`, which Linux calls
 * suspend-to-idle or s2idle. Userspace is frozen, every CPU is parked in its
 * deepest idle state through cpuidle_enter(), and the machine comes back when
 * a wake source raises an interrupt. Nothing is powered off and no firmware is
 * involved, which is exactly why it works on hardware whose ACPI sleep path
 * this kernel cannot yet drive.
 *
 * `mem` (ACPI S3) is deliberately absent rather than present and broken: see
 * docs/kernel/platforms.md.
 */
#ifndef B1NIX_SUSPEND_H
#define B1NIX_SUSPEND_H

#include <b1nix/types.h>

/* The states this machine really supports, space separated, as
 * /sys/power/state reads them. */
const char *suspend_states(void);

/* Suspend into `state`. Returns 0 once the machine has resumed, or a negative
 * errno: -EINVAL for a state this machine does not have, -ENODEV when nothing
 * is armed that could wake it again, -EBUSY when the freezer could not stop
 * userspace. */
int suspend_enter(const char *state);

/* ── Wake sources ────────────────────────────────────────────────────────
 *
 * A suspend with nothing able to end it is a hang, so a driver that can wake
 * the machine registers here and is asked, at the moment of the suspend,
 * whether it is armed right now. The RTC alarm is the one this kernel has.
 */
typedef int (*suspend_wake_armed_fn)(void *ctx);
int suspend_register_wake_source(const char *name, suspend_wake_armed_fn armed,
                                 void *ctx);

/* A driver that can deliver a keypress (or a byte on the console) says so
 * here; the suspend path then treats human input as a wake source. Idempotent
 * — several drivers may call it. */
void suspend_register_input_source(void);

/* Called from the wake source's interrupt handler: ends the suspend and
 * records what ended it. Safe in interrupt context. */
void suspend_wake_event(const char *source);

/* How many times this machine has been woken out of a suspend, and by what.
 * Read by /proc/interrupts and by the suspend smoke. */
u64 suspend_wake_count(void);
const char *suspend_last_wake_source(void);

/* Register the wake sources and publish the state list. Called once at boot,
 * after /dev/rtc0 exists. */
void suspend_init(void);

#endif
