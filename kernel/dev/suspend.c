/* SPDX-License-Identifier: GPL-2.0-only */
/* System suspend — s2idle behind /sys/power/state (M129).
 *
 * What a suspend-to-idle actually is, once the marketing is removed: stop
 * userspace, then stop asking the CPUs to do anything until an interrupt says
 * otherwise. There is no firmware call, no power state to restore and nothing
 * to save, which is why it is the state that works everywhere — and why it is
 * the only one this file offers. `mem` (ACPI S3) would mean writing SLP_TYP
 * into the FADT's PM1 control ports and coming back through a real-mode
 * trampoline with every device re-initialised; a state that goes to sleep and
 * cannot be woken is worse than a state that is not there, so it is not here.
 *
 * Two things make this a suspend rather than a long sleep:
 *
 *   - the freezer (kernel/sched/scheduler.c) holds every userspace task but
 *     the caller off the CPU, so nothing runs while the machine is down and
 *     everything resumes where it was; and
 *
 *   - a wake source. Idling forever is a hang, so a suspend is REFUSED unless
 *     something registered here says it is armed to raise an interrupt. The
 *     RTC alarm is that something (kernel/dev/rtc_dev.c), which is what makes
 *     `rtcwake` work: arm the alarm, write `freeze`, be woken by it.
 *
 * There is a ceiling on the sleep as well. A wake source can be armed and
 * still fail to fire — a misrouted line, an alarm the hardware quietly
 * dropped — and the difference between a bug and a dead machine is whether
 * anybody is left to report it. After SUSPEND_MAX_MS the machine resumes
 * anyway and says that is what happened.
 */

#include <b1nix/suspend.h>

#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/cpuidle.h>
#include <b1nix/errno.h>
#include <b1nix/ktime.h>
#include <b1nix/rtc.h>
#include <b1nix/sched.h>
#include <b1nix/types.h>

#include <string.h>

/* How long userspace is given to come off the CPUs before the freeze is
 * abandoned. A task reaches a scheduling point within a tick when it is in
 * ring 3 and at its next block or return when it is in the kernel, so this is
 * generous by three orders of magnitude; it only has to be finite. */
/* Twenty seconds, which is what Linux's freezer allows, and not because a
 * task needs that long: on an emulated machine under load a task that is
 * mid-syscall on another CPU can take a second or two to reach a scheduling
 * point, and a two-second window turned that into "userspace would not stop"
 * — a suspend refused for being busy, on a machine that was merely slow. The
 * window is only ever spent when the freeze is failing. */
#define SUSPEND_FREEZE_MS 20000

/* The ceiling on the sleep itself — see the header comment. Ten seconds
 * rather than thirty: the ceiling is only ever reached when a wake source was
 * armed and did not fire, and on a machine whose only source is a console
 * nobody is typing at (a headless guest, this project's aarch64 lane) that is
 * exactly what happens — so the cost of the safety net is paid on every such
 * suspend, and thirty seconds of it was most of a test lane's budget. */
#define SUSPEND_MAX_MS 10000

#define SUSPEND_MAX_SOURCES 4

struct wake_source {
  const char *name;
  suspend_wake_armed_fn armed;
  void *ctx;
};

static struct wake_source g_sources[SUSPEND_MAX_SOURCES];
static int g_nsources;

static volatile u64 g_wake_count;
static const char *volatile g_wake_source_name = "none";
/* Non-zero only between the freeze and the thaw. A wake event outside that
 * window is an ordinary interrupt and must not be counted as a wake. */
static volatile int g_suspended;

const char *suspend_states(void) { return "freeze"; }

int suspend_register_wake_source(const char *name, suspend_wake_armed_fn armed,
                                 void *ctx) {
  if (!name || !armed || g_nsources >= SUSPEND_MAX_SOURCES)
    return -1;
  g_sources[g_nsources].name = name;
  g_sources[g_nsources].armed = armed;
  g_sources[g_nsources].ctx = ctx;
  g_nsources++;
  return 0;
}

void suspend_wake_event(const char *source) {
  if (!__atomic_load_n(&g_suspended, __ATOMIC_ACQUIRE))
    return;
  g_wake_source_name = source ? source : "unknown";
  __atomic_fetch_add(&g_wake_count, 1, __ATOMIC_RELEASE);
  /* Nothing to kick: the suspending CPU is halted inside cpuidle_enter() and
   * the interrupt that got us here is what wakes it. */
}

u64 suspend_wake_count(void) {
  return __atomic_load_n(&g_wake_count, __ATOMIC_ACQUIRE);
}

const char *suspend_last_wake_source(void) { return g_wake_source_name; }

/* Is anything armed right now that could end a suspend? */
static const char *wake_source_armed(void) {
  for (int i = 0; i < g_nsources; i++) {
    if (g_sources[i].armed(g_sources[i].ctx))
      return g_sources[i].name;
  }
  return 0;
}

int suspend_enter(const char *state) {
  const char *armed;
  u64 start, now, target;
  u64 base_wakes;
  int rc;

  if (!state || strcmp(state, "freeze") != 0)
    return -EINVAL;

  armed = wake_source_armed();
  if (!armed) {
    console_write("power: refusing to freeze, no wake source is armed\n");
    return -ENODEV;
  }

  /* The window opens BEFORE the freeze, not after it.
   *
   * Freezing takes as long as the slowest task needs to come off its CPU —
   * on an emulated machine under load, seconds. An alarm armed for three
   * seconds fires inside that window, and a wake event that arrives while
   * the flag is still down is discarded: the machine then parks with its one
   * wake source already spent and sleeps until the ceiling. Counting from
   * here also gives the behaviour Linux has, where a wakeup event during
   * suspend preparation aborts the suspend instead of being lost. */
  base_wakes = suspend_wake_count();
  __atomic_store_n(&g_suspended, 1, __ATOMIC_RELEASE);

  rc = sched_freeze_userspace(SUSPEND_FREEZE_MS);
  if (rc < 0) {
    __atomic_store_n(&g_suspended, 0, __ATOMIC_RELEASE);
    console_write("power: freeze aborted, userspace would not stop\n");
    return rc;
  }

  console_write("power: freeze, ");
  console_write_dec((u64)sched_frozen_count());
  console_write(" task(s) held, wake source ");
  console_write(armed);
  console_write("\n");

  start = ktime_monotonic_ns();
  target = start + (u64)SUSPEND_MAX_MS * 1000000ull;

  for (;;) {
    if (suspend_wake_count() != base_wakes)
      break;
    now = ktime_monotonic_ns();
    if (now >= target)
      break;
    /* cpuidle_enter() wants interrupts off on the way in and hands them back
     * on the way out; the check inside the disable is what closes the window
     * between deciding to idle and idling. */
    interrupts_disable();
    if (suspend_wake_count() != base_wakes) {
      interrupts_enable();
      break;
    }
    cpuidle_enter();
  }

  __atomic_store_n(&g_suspended, 0, __ATOMIC_RELEASE);
  now = ktime_monotonic_ns();
  sched_thaw_userspace();

  console_write("power: resumed after ");
  console_write_dec((now - start) / 1000000ull);
  console_write(" ms, woken by ");
  if (suspend_wake_count() != base_wakes)
    console_write(g_wake_source_name);
  else
    console_write("nothing (the wake source never fired)");
  console_write("\n");
  return 0;
}

/* Human input, as a wake source.
 *
 * Armed whenever the machine has something a person can type on, because a
 * key really can end a suspend and that is what a desktop means by it: a
 * plain `systemctl suspend` arms no alarm and expects the hardware to be the
 * wake source. But only a driver that can actually DELIVER the event may
 * register it — an "always armed" input source on a machine whose console
 * never interrupts turns a suspend into a wait for the thirty-second
 * ceiling, which is what the aarch64 lane did. The keyboard ISR and the
 * console drains call suspend_wake_event; each costs one relaxed load while
 * no suspend is in progress. */
static int input_armed(void *ctx) {
  (void)ctx;
  return 1;
}

static volatile int g_input_registered;

void suspend_register_input_source(void) {
  if (__atomic_exchange_n(&g_input_registered, 1, __ATOMIC_ACQ_REL))
    return;
  suspend_register_wake_source("input", input_armed, 0);
}

void suspend_init(void) {
  rtc_wake_source_init();
  console_write("power: states:");
  console_write(" ");
  console_write(suspend_states());
  console_write("\n");
}
