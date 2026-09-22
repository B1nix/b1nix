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
 * `mem` (ACPI S3) is offered as well, but only on a machine that really has
 * it: the firmware must declare \_S3 and the FADT must name the registers the
 * sleep is entered through. See kernel/arch/x86_64/s3.c.
 */
#ifndef B1NIX_SUSPEND_H
#define B1NIX_SUSPEND_H

#include <b1nix/types.h>

/* ── the architecture's half of an S3 sleep ───────────────────────────────
 *
 * Implemented where the processor state and the firmware's sleep registers
 * are: kernel/arch/x86_64/s3.c. A port without it says so and /sys/power/state
 * lists `freeze` alone. */
int arch_s3_supported(void);
const char *arch_s3_why_not(void);
/* 1 when the machine slept and came back, 0 when the platform refused the
 * state, negative errno when this kernel refused to try. Interrupts off. */
int arch_s3_enter(void);
/* The firmware's own resume hook (ACPI `_WAK`), called with interrupts back on
 * because the method is a program and may sleep. */
void arch_s3_firmware_wake(void);
u64 arch_s3_count(void);
void arch_s3_note_ms(u64 ms);
u64 arch_s3_last_ms(void);

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

/* SUSPEND_WAKE_DEEP: this source can end an ACPI S3 as well as an idle
 * suspend. The distinction is not cosmetic — a byte on a serial console wakes a
 * machine that is merely idling and cannot wake one whose processor has been
 * powered off, because there is nothing left to take the interrupt. Only a
 * source the CHIPSET wakes on (the RTC alarm, a power button, a PME) may claim
 * it, and an S3 with none of them armed is refused: a sleep nothing can end is
 * a dead machine, and there is no software ceiling once the power is gone. */
#define SUSPEND_WAKE_IDLE 0u
#define SUSPEND_WAKE_DEEP 1u

int suspend_register_wake_source_flags(const char *name,
                                       suspend_wake_armed_fn armed, void *ctx,
                                       u32 flags);
int suspend_register_wake_source(const char *name, suspend_wake_armed_fn armed,
                                 void *ctx);

/* ── devices, across a sleep that resets them ─────────────────────────────
 *
 * An S3 removes power from the machine, and what comes back is hardware at its
 * reset state: a virtio device with no negotiated features and empty queues, a
 * UART with no divisor, a controller that has forgotten its ring. The memory
 * the driver keeps is intact, the device's own state is gone, and the two no
 * longer agree — which is not a crash, it is a machine whose disk answers
 * nothing and whose console prints nothing, which is worse.
 *
 * So a driver that has device state to rebuild registers here, and the resume
 * path calls it back with interrupts enabled and the scheduler running, in
 * registration order (the console first, because everything after it reports
 * through it). A driver that needs nothing registers nothing. */
typedef int (*suspend_resume_fn)(void *ctx);
int suspend_register_device(const char *name, suspend_resume_fn resume,
                            void *ctx);
/* Call every registered resume callback. Returns how many reported failure. */
int suspend_resume_devices(void);

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
