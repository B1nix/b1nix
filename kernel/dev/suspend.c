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
#include <b1nix/hibernate.h>
#include <b1nix/ktime.h>
#include <b1nix/rtc.h>
#include <b1nix/sched.h>
#include <b1nix/types.h>
#include <b1nix/vfs.h>

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

/* How long a secondary CPU is given to reach its idle loop and park before an
 * S3 is refused. A CPU running a kernel thread takes as long as that thread
 * needs to block; a CPU that never parks means something is spinning, and
 * sleeping with it live would lose whatever it held. */
#define SUSPEND_PARK_MS 5000

#define SUSPEND_MAX_SOURCES 6

struct wake_source {
  const char *name;
  suspend_wake_armed_fn armed;
  void *ctx;
  u32 flags;
  /* /sys/class/wakeup/wakeupN (M135): the events it reported, the ones that
   * ended a suspend, and when it last did either. */
  u64 events;
  u64 wakeups;
  u64 last_ms;
};

static struct wake_source g_sources[SUSPEND_MAX_SOURCES];
static int g_nsources;

/* /sys/power/wakeup_count (M135): every wakeup event since boot, and the
 * count userspace wrote back, which a suspend must still match. */
static volatile u64 g_events;
static u64 g_saved_events;
static volatile int g_saved_valid;
/* /sys/power/mem_sleep: what `mem` means, deep (S3) or s2idle. */
static volatile int g_mem_deep = -1;
/* /sys/power/sync_on_suspend. */
static volatile int g_sync_on_suspend = 1;

int suspend_sync_on_suspend(void) { return g_sync_on_suspend; }
void suspend_set_sync_on_suspend(int on) {
  __atomic_store_n(&g_sync_on_suspend, on ? 1 : 0, __ATOMIC_RELEASE);
}

static volatile u64 g_wake_count;
static const char *volatile g_wake_source_name = "none";
/* Non-zero only between the freeze and the thaw. A wake event outside that
 * window is an ordinary interrupt and must not be counted as a wake. */
static volatile int g_suspended;

/* What this machine really has. `mem` appears only where the firmware and the
 * architecture both provide it; a state listed here that cannot be entered is
 * how a suspend becomes a hang. */
const char *suspend_states(void) {
  /* `mem` is whichever mem_sleep names, and s2idle is always one of them;
   * `disk` where there is somewhere to write the image. */
  return hibernate_available() ? "freeze mem disk" : "freeze mem";
}

static int mem_is_deep(void) {
  if (g_mem_deep < 0)
    g_mem_deep = arch_s3_supported() ? 1 : 0;
  return g_mem_deep;
}

const char *suspend_mem_sleep(void) {
  if (!arch_s3_supported())
    return "[s2idle]";
  return mem_is_deep() ? "s2idle [deep]" : "[s2idle] deep";
}

int suspend_set_mem_sleep(const char *name) {
  if (!strcmp(name, "s2idle")) {
    g_mem_deep = 0;
    return 0;
  }
  if (!strcmp(name, "deep") && arch_s3_supported()) {
    g_mem_deep = 1;
    return 0;
  }
  return -EINVAL;
}

u64 suspend_event_count(void) {
  return __atomic_load_n(&g_events, __ATOMIC_ACQUIRE);
}

int suspend_save_event_count(u64 count) {
  if (count != suspend_event_count()) {
    __atomic_store_n(&g_saved_valid, 0, __ATOMIC_RELEASE);
    return -EINVAL; /* an event arrived since it was read */
  }
  g_saved_events = count;
  __atomic_store_n(&g_saved_valid, 1, __ATOMIC_RELEASE);
  return 0;
}

int suspend_source_count(void) { return g_nsources; }

const char *suspend_source_name(int i) {
  return (i >= 0 && i < g_nsources) ? g_sources[i].name : "";
}

u64 suspend_source_stat(int i, int which) {
  if (i < 0 || i >= g_nsources)
    return 0;
  switch (which) {
  case SUSPEND_STAT_EVENTS:
    return g_sources[i].events;
  case SUSPEND_STAT_WAKEUPS:
    return g_sources[i].wakeups;
  default:
    return g_sources[i].last_ms;
  }
}

int suspend_register_wake_source_flags(const char *name,
                                       suspend_wake_armed_fn armed, void *ctx,
                                       u32 flags) {
  if (!name || !armed || g_nsources >= SUSPEND_MAX_SOURCES)
    return -1;
  g_sources[g_nsources].name = name;
  g_sources[g_nsources].armed = armed;
  g_sources[g_nsources].ctx = ctx;
  g_sources[g_nsources].flags = flags;
  g_nsources++;
  return 0;
}

int suspend_register_wake_source(const char *name, suspend_wake_armed_fn armed,
                                 void *ctx) {
  return suspend_register_wake_source_flags(name, armed, ctx,
                                            SUSPEND_WAKE_IDLE);
}

/* The registered source an event belongs to: its own name, or -- for a
 * keyboard, a serial line, the console -- the one "input" source they share. */
static struct wake_source *source_of(const char *name) {
  struct wake_source *input = 0;

  for (int i = 0; i < g_nsources; i++) {
    if (name && !strcmp(g_sources[i].name, name))
      return &g_sources[i];
    if (!strcmp(g_sources[i].name, "input"))
      input = &g_sources[i];
  }
  return input;
}

void suspend_wake_event(const char *source) {
  int suspended = __atomic_load_n(&g_suspended, __ATOMIC_ACQUIRE);
  struct wake_source *ws;

  /* A keypress while the machine is up is not a wakeup event; a power button
   * press or an alarm is, suspended or not, as Linux counts them. */
  if (!suspended) {
    ws = source_of(source);
    if (ws && (ws->flags & SUSPEND_WAKE_EVENTS)) {
      ws->events++;
      ws->last_ms = ktime_monotonic_ns() / 1000000ull;
      __atomic_fetch_add(&g_events, 1, __ATOMIC_RELEASE);
    }
    return;
  }
  ws = source_of(source);
  if (ws) {
    ws->events++;
    ws->wakeups++;
    ws->last_ms = ktime_monotonic_ns() / 1000000ull;
  }
  __atomic_fetch_add(&g_events, 1, __ATOMIC_RELEASE);
  g_wake_source_name = source ? source : "unknown";
  __atomic_fetch_add(&g_wake_count, 1, __ATOMIC_RELEASE);
  /* Nothing to kick: the suspending CPU is halted inside cpuidle_enter() and
   * the interrupt that got us here is what wakes it. */
}

u64 suspend_wake_count(void) {
  return __atomic_load_n(&g_wake_count, __ATOMIC_ACQUIRE);
}

const char *suspend_last_wake_source(void) { return g_wake_source_name; }

/* ── device resume ──────────────────────────────────────────────────────── */

#define SUSPEND_MAX_DEVICES 32

struct resume_dev {
  const char *name;
  suspend_resume_fn suspend;
  suspend_resume_fn resume;
  void *ctx;
};

static struct resume_dev g_devices[SUSPEND_MAX_DEVICES];
static int g_ndevices;

int suspend_register_device_ops(const char *name, suspend_resume_fn suspend,
                                suspend_resume_fn resume, void *ctx) {
  if (!name || !resume || g_ndevices >= SUSPEND_MAX_DEVICES)
    return -1;
  g_devices[g_ndevices].name = name;
  g_devices[g_ndevices].suspend = suspend;
  g_devices[g_ndevices].resume = resume;
  g_devices[g_ndevices].ctx = ctx;
  g_ndevices++;
  return 0;
}

int suspend_register_device(const char *name, suspend_resume_fn resume,
                            void *ctx) {
  return suspend_register_device_ops(name, 0, resume, ctx);
}

/* A device every other device's resume depends on -- an IOMMU, which has to
 * translate and remap before anything does DMA or raises a message
 * interrupt: first to resume, last to suspend. */
int suspend_register_device_early(const char *name, suspend_resume_fn resume,
                                  void *ctx) {
  if (!name || !resume || g_ndevices >= SUSPEND_MAX_DEVICES)
    return -1;
  for (int i = g_ndevices; i > 0; i--)
    g_devices[i] = g_devices[i - 1];
  g_devices[0].name = name;
  g_devices[0].suspend = 0;
  g_devices[0].resume = resume;
  g_devices[0].ctx = ctx;
  g_ndevices++;
  return 0;
}

/* A module going away takes its callbacks with it; the order of the others
 * is kept, it is the order they come back in. */
void suspend_unregister_device(suspend_resume_fn resume) {
  for (int i = 0; i < g_ndevices; i++) {
    if (g_devices[i].resume != resume)
      continue;
    for (int j = i + 1; j < g_ndevices; j++)
      g_devices[j - 1] = g_devices[j];
    g_ndevices--;
    return;
  }
}

int suspend_devices_suspend(void);

/* In the reverse of the resume order, as Linux suspends: the drivers that
 * depend on others go first. One that fails stops the sleep, and the ones
 * already suspended are resumed again. */
static int suspend_suspend_devices(void) {
  for (int i = g_ndevices - 1; i >= 0; i--) {
    if (!g_devices[i].suspend)
      continue;
    if (g_devices[i].suspend(g_devices[i].ctx) != 0) {
      console_write("power: ");
      console_write(g_devices[i].name);
      console_write(" refused to suspend\n");
      for (int k = i + 1; k < g_ndevices; k++)
        if (g_devices[k].suspend)
          (void)g_devices[k].resume(g_devices[k].ctx);
      return -EBUSY;
    }
  }
  return 0;
}

/* The same, for hibernation, which drives its own sequence. */
int suspend_devices_suspend(void) { return suspend_suspend_devices(); }

int suspend_resume_devices(void) {
  int failed = 0;

  for (int i = 0; i < g_ndevices; i++) {
    /* Named BEFORE it is called, not after: a driver whose resume hangs is the
     * most likely thing to go wrong on this path, and the last line on the
     * console is then the only evidence of which one it was. */
    console_write("power: resuming ");
    console_write(g_devices[i].name);
    console_write("\n");
    if (g_devices[i].resume(g_devices[i].ctx) != 0) {
      console_write("power: ");
      console_write(g_devices[i].name);
      console_write(" did not come back\n");
      failed++;
    }
  }
  return failed;
}

/* Is anything armed right now that could end a suspend? `need` is the class of
 * source the state requires: an idle suspend takes anything, an S3 takes only a
 * source the chipset itself wakes on. */
static const char *wake_source_armed_kind(u32 need) {
  for (int i = 0; i < g_nsources; i++) {
    if ((g_sources[i].flags & need) != need)
      continue;
    if (g_sources[i].armed(g_sources[i].ctx))
      return g_sources[i].name;
  }
  return 0;
}

static const char *wake_source_armed(void) {
  return wake_source_armed_kind(SUSPEND_WAKE_IDLE);
}

/* ACPI S3: the same freeze, then the platform's own sleep.
 *
 * Everything before the sleep is the s2idle path — the wake source has to be
 * armed and userspace has to be off the CPUs — and everything after it is the
 * architecture's (kernel/arch/x86_64/s3.c), which is where the processor is
 * saved, the firmware is asked to sleep, and the machine is put back together
 * on the way out. This function's own work is the bookkeeping either side of
 * that and the honesty about what happened: a platform that refuses the state
 * is reported as refusing it, not as a suspend that returned quickly. */
static int suspend_enter_s3(const char *armed) {
  u64 start, now;
  u64 base_wakes;
  int rc, slept;

  if (!arch_s3_supported()) {
    console_write("power: this machine has no S3 (");
    console_write(arch_s3_why_not());
    console_write(")\n");
    return -EINVAL;
  }
  /* The other CPUs have to be off the processor: their state does not survive
   * S3, and one that is INIT'd while it holds a lock takes the machine with
   * it. They are parked from their idle loop, which is the one place an AP
   * holds nothing. */
  rc = sched_park_secondary_cpus(SUSPEND_PARK_MS);
  if (rc < 0) {
    console_write("power: refusing S3, a CPU would not park\n");
    return -EBUSY;
  }

  base_wakes = suspend_wake_count();
  __atomic_store_n(&g_suspended, 1, __ATOMIC_RELEASE);
  rc = sched_freeze_userspace(SUSPEND_FREEZE_MS);
  if (rc < 0) {
    __atomic_store_n(&g_suspended, 0, __ATOMIC_RELEASE);
    sched_unpark_secondary_cpus();
    console_write("power: freeze aborted, userspace would not stop\n");
    return rc;
  }

  rc = suspend_suspend_devices();
  if (rc < 0) {
    __atomic_store_n(&g_suspended, 0, __ATOMIC_RELEASE);
    sched_unpark_secondary_cpus();
    sched_thaw_userspace();
    return rc;
  }

  console_write("power: mem (S3), ");
  console_write_dec((u64)sched_frozen_count());
  console_write(" task(s) held, wake source ");
  console_write(armed);
  console_write("\n");

  start = ktime_monotonic_ns();
  interrupts_disable();
  /* The monotonic clock is repaired inside this call, before the resume touches
   * anything that reads the time: the counter it is built on came back at zero,
   * and only the hardware clock knows how long the machine was away (see
   * x86_s3_restore_cpu). */
  slept = arch_s3_enter();
  interrupts_enable();
  /* The firmware's hook, now that there is a timer again: it is a program, and
   * QEMU's own spends time in Sleep(). Timed, because a resume that takes a
   * minute is indistinguishable from a resume that hung unless the log says
   * which part of it took the minute. */
  if (slept > 0) {
    arch_s3_firmware_wake();
    /* The wall clock is built on the monotonic one and lost the same time; the
     * hardware clock kept counting and is what it is corrected from. */
    rtc_resync_wallclock();
  }
  now = ktime_monotonic_ns();

  __atomic_store_n(&g_suspended, 0, __ATOMIC_RELEASE);

  sched_unpark_secondary_cpus();
  /* The devices, before userspace is let go: a task that resumes into a read
   * from a disk whose controller has not been rebuilt waits for ever. */
  if (slept > 0) {
    int bad = suspend_resume_devices();

    console_write("power: devices resumed");
    if (bad) {
      console_write(", ");
      console_write_dec((u64)bad);
      console_write(" failed");
    }
    console_write("\n");
  }
  sched_thaw_userspace();

  if (slept > 0) {
    arch_s3_note_ms((now - start) / 1000000ull);
    console_write("power: resumed from S3 after ");
    console_write_dec((now - start) / 1000000ull);
    console_write(" ms, woken by ");
    console_write(suspend_wake_count() != base_wakes ? suspend_last_wake_source()
                                                     : "the platform");
    console_write("\n");
    return 0;
  }
  if (slept == 0) {
    console_write("power: the platform did not enter S3\n");
    return -EIO;
  }
  return slept;
}

int suspend_enter(const char *state) {
  const char *armed;
  u64 start, now, target;
  u64 base_wakes;
  int rc;

  if (!state)
    return -EINVAL;
  /* The wakeup_count handshake: a count written back that events have since
   * overtaken means something wanted the machine awake after userspace
   * decided to suspend it. Checked once per attempt, as Linux does. */
  if (__atomic_exchange_n(&g_saved_valid, 0, __ATOMIC_ACQ_REL) &&
      g_saved_events != suspend_event_count()) {
    console_write("power: suspend aborted, a wakeup event arrived after "
                  "wakeup_count was written\n");
    return -EBUSY;
  }
  if (strcmp(state, "disk") == 0)
    return hibernate_available() ? hibernate_enter() : -EINVAL;
  if (strcmp(state, "mem") == 0 && !mem_is_deep())
    state = "freeze";
  /* What Linux does first (sync_on_suspend): the dirty data goes to the
   * disks while every task can still run, so a sleep the machine does not
   * come back from loses nothing that was already written. */
  if (__atomic_load_n(&g_sync_on_suspend, __ATOMIC_ACQUIRE) &&
      (!strcmp(state, "mem") || !strcmp(state, "freeze")))
    (void)vfs_sync();
  if (strcmp(state, "mem") == 0) {
    /* Only a source that can wake a powered-off machine will do here. There is
     * no ceiling on an S3 — once the processor is off nothing of this kernel is
     * left to time it out — so a sleep with nothing armed to end it is a dead
     * machine rather than a long one. */
    armed = wake_source_armed_kind(SUSPEND_WAKE_DEEP);
    if (!armed) {
      console_write("power: refusing S3, nothing armed can wake a powered-off "
                    "machine (arm the RTC alarm)\n");
      return -ENODEV;
    }
    return suspend_enter_s3(armed);
  }
  if (strcmp(state, "freeze") != 0)
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
#if defined(__aarch64__)
  {
    /* What this port's deep sleep would be: PSCI's, if the firmware has it. */
    extern long arch_psci_system_suspend_supported(void);
    long r = arch_psci_system_suspend_supported();

    console_write(r == 0 ? "power: PSCI SYSTEM_SUSPEND implemented\n"
                         : "power: PSCI SYSTEM_SUSPEND not implemented\n");
  }
#endif
  console_write("power: states:");
  console_write(" ");
  console_write(suspend_states());
  console_write("\n");
}
