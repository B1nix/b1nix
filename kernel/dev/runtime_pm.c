/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Runtime power management (M135). See include/b1nix/runtime_pm.h.
 *
 * Before this a device was at full power from probe to power-off: an NVMe
 * drive nothing had read for an hour drew what it draws under load, and only a
 * system suspend ever put it lower. The shape is Linux's: a usage count taken
 * around every use, an autosuspend delay after the last one, a thread that
 * suspends what has been idle long enough, and a resume on the next use that
 * the user of the device waits for.
 */

#include <b1nix/runtime_pm.h>

#include <b1nix/errno.h>
#include <b1nix/kprintf.h>
#include <b1nix/ktime.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/suspend.h>
#include <b1nix/sysfs_attr.h>
#include <b1nix/types.h>

#include <stdio.h>
#include <string.h>

#define RPM_MAX 16

enum { RPM_ACTIVE, RPM_SUSPENDED, RPM_SUSPENDING, RPM_RESUMING };
static const char *const g_status_name[] = {"active", "suspended", "suspending",
                                            "resuming"};

struct rpm_dev {
  const char *name;
  rpm_fn suspend, resume;
  void *ctx;
  int usage;
  int status;
  int allow;       /* power/control: 1 auto, 0 on */
  u32 delay_ms;
  u64 last_busy_ns;
  u64 since_ns;    /* when the status last changed */
  u64 active_ns, suspended_ns;
  u64 suspends, resumes;
};

static struct rpm_dev g_dev[RPM_MAX];
static int g_ndev;
static spinlock_t g_rpm_lock = SPINLOCK_INIT;
static int g_chan;
static u32 g_kick; /* bumped by every wake, so one between scan and sleep counts */
static int g_thread;
static volatile int g_held; /* a system suspend is in progress */

static void kick(void) {
  __atomic_fetch_add(&g_kick, 1, __ATOMIC_ACQ_REL);
  scheduler_wake_all(&g_chan);
}

static void account_locked(struct rpm_dev *d, u64 now) {
  if (d->status == RPM_SUSPENDED)
    d->suspended_ns += now - d->since_ns;
  else
    d->active_ns += now - d->since_ns;
  d->since_ns = now;
}

static void set_status_locked(struct rpm_dev *d, int status) {
  account_locked(d, ktime_monotonic_ns());
  d->status = status;
}

/* Bring one device back. Called with the lock NOT held; the status marks it
 * busy meanwhile so no one else suspends or resumes it. */
static int do_resume(int id) {
  struct rpm_dev *d = &g_dev[id];
  u64 flags;
  int rc;

  for (;;) {
    spin_lock_irqsave(&g_rpm_lock, &flags);
    if (d->status == RPM_ACTIVE) {
      spin_unlock_irqrestore(&g_rpm_lock, flags);
      return 0;
    }
    if (d->status == RPM_SUSPENDED) {
      set_status_locked(d, RPM_RESUMING);
      spin_unlock_irqrestore(&g_rpm_lock, flags);
      break;
    }
    /* Someone else is between states: wait for them. */
    spin_unlock_irqrestore(&g_rpm_lock, flags);
    scheduler_yield();
  }
  rc = d->resume(d->ctx);
  spin_lock_irqsave(&g_rpm_lock, &flags);
  set_status_locked(d, rc == 0 ? RPM_ACTIVE : RPM_SUSPENDED);
  if (rc == 0)
    d->resumes++;
  d->last_busy_ns = ktime_monotonic_ns();
  spin_unlock_irqrestore(&g_rpm_lock, flags);
  if (rc != 0)
    k_warn("rpm", "%s did not resume", d->name);
  return rc == 0 ? 0 : -1;
}

int rpm_get(int id) {
  u64 flags;

  if (id < 0 || id >= g_ndev)
    return 0; /* not under runtime PM: always usable */
  spin_lock_irqsave(&g_rpm_lock, &flags);
  g_dev[id].usage++;
  spin_unlock_irqrestore(&g_rpm_lock, flags);
  if (do_resume(id) == 0)
    return 0;
  spin_lock_irqsave(&g_rpm_lock, &flags);
  g_dev[id].usage--;
  spin_unlock_irqrestore(&g_rpm_lock, flags);
  return -1;
}

void rpm_put(int id) {
  u64 flags;

  if (id < 0 || id >= g_ndev)
    return;
  spin_lock_irqsave(&g_rpm_lock, &flags);
  if (g_dev[id].usage > 0)
    g_dev[id].usage--;
  g_dev[id].last_busy_ns = ktime_monotonic_ns();
  spin_unlock_irqrestore(&g_rpm_lock, flags);
  kick();
}

/* The autosuspend thread: suspend whatever has been idle for its delay, then
 * sleep until the next device could be. */

static void krpmd(void *arg) {
  (void)arg;
  for (;;) {
    u64 now = ktime_monotonic_ns(), next = 0;
    u32 gen = __atomic_load_n(&g_kick, __ATOMIC_ACQUIRE);

    for (int i = 0; i < g_ndev; i++) {
      struct rpm_dev *d = &g_dev[i];
      u64 flags, due;
      int go = 0;

      spin_lock_irqsave(&g_rpm_lock, &flags);
      due = d->last_busy_ns + (u64)d->delay_ms * 1000000ull;
      if (d->allow && !g_held && d->usage == 0 && d->status == RPM_ACTIVE) {
        if (now >= due) {
          set_status_locked(d, RPM_SUSPENDING);
          go = 1;
        } else if (!next || due < next) {
          next = due;
        }
      }
      spin_unlock_irqrestore(&g_rpm_lock, flags);
      if (go) {
        int rc = d->suspend(d->ctx);

        spin_lock_irqsave(&g_rpm_lock, &flags);
        set_status_locked(d, rc == 0 ? RPM_SUSPENDED : RPM_ACTIVE);
        if (rc == 0)
          d->suspends++;
        else
          d->last_busy_ns = ktime_monotonic_ns(); /* try again later */
        spin_unlock_irqrestore(&g_rpm_lock, flags);
        if (rc == 0)
          k_info("rpm", "%s suspended after %u ms idle", d->name, d->delay_ms);
      }
    }
    if (next) {
      u64 ms = next > now ? (next - now + 999999) / 1000000 : 1;

      scheduler_wait_prepare_timeout(&g_chan, SCHED_MS_TO_TICKS(ms));
    } else {
      scheduler_wait_prepare(&g_chan);
    }
    if (__atomic_load_n(&g_kick, __ATOMIC_ACQUIRE) != gen)
      scheduler_wait_cancel();
    else
      scheduler_wait_commit();
  }
}

void rpm_hold_all(void) {
  g_held = 1;
  for (int i = 0; i < g_ndev; i++)
    (void)do_resume(i);
}

void rpm_release_all(void) {
  u64 now = ktime_monotonic_ns();

  for (int i = 0; i < g_ndev; i++)
    g_dev[i].last_busy_ns = now;
  g_held = 0;
  kick();
}

static int rpm_system_suspend(void *ctx) {
  (void)ctx;
  rpm_hold_all();
  return 0;
}

static int rpm_system_resume(void *ctx) {
  (void)ctx;
  rpm_release_all();
  return 0;
}

/* ── power/ in sysfs ────────────────────────────────────────────────────── */

enum { F_CONTROL, F_STATUS, F_ACTIVE, F_SUSPENDED, F_DELAY };

struct rpm_file {
  int id, which;
};

static isize rpm_show(void *ctx, char *buf, usize cap) {
  const struct rpm_file *f = (const struct rpm_file *)ctx;
  struct rpm_dev *d = &g_dev[f->id];
  u64 flags, active, suspended;
  int status;

  spin_lock_irqsave(&g_rpm_lock, &flags);
  account_locked(d, ktime_monotonic_ns());
  active = d->active_ns;
  suspended = d->suspended_ns;
  status = d->status;
  spin_unlock_irqrestore(&g_rpm_lock, flags);
  switch (f->which) {
  case F_CONTROL:
    return snprintf(buf, cap, "%s\n", d->allow ? "auto" : "on");
  case F_STATUS:
    return snprintf(buf, cap, "%s\n", g_status_name[status]);
  case F_ACTIVE:
    return snprintf(buf, cap, "%lu\n", (unsigned long)(active / 1000000ull));
  case F_SUSPENDED:
    return snprintf(buf, cap, "%lu\n",
                    (unsigned long)(suspended / 1000000ull));
  default:
    return snprintf(buf, cap, "%u\n", d->delay_ms);
  }
}

static isize rpm_store(void *ctx, const char *buf, usize len) {
  const struct rpm_file *f = (const struct rpm_file *)ctx;
  struct rpm_dev *d = &g_dev[f->id];
  char text[16];
  usize n = len < sizeof(text) - 1 ? len : sizeof(text) - 1;

  memcpy(text, buf, n);
  text[n] = 0;
  while (n && (text[n - 1] == '\n' || text[n - 1] == ' '))
    text[--n] = 0;
  if (f->which == F_CONTROL) {
    if (!strcmp(text, "auto")) {
      d->allow = 1;
      d->last_busy_ns = ktime_monotonic_ns();
    } else if (!strcmp(text, "on")) {
      d->allow = 0;
      /* "on" means on: a suspended device comes back now. */
      if (do_resume(f->id) != 0)
        return -EIO;
    } else {
      return -EINVAL;
    }
    kick();
    return (isize)len;
  }
  if (f->which == F_DELAY) {
    u32 v = 0;

    if (!text[0])
      return -EINVAL;
    for (usize i = 0; text[i]; i++) {
      if (text[i] < '0' || text[i] > '9')
        return -EINVAL;
      v = v * 10 + (u32)(text[i] - '0');
    }
    d->delay_ms = v;
    kick();
    return (isize)len;
  }
  return -EINVAL;
}

static void rpm_publish(int id, const char *sysfs_dir) {
  static const struct {
    const char *name;
    int which;
    u16 mode;
  } files[] = {
      {"control", F_CONTROL, 0644},
      {"runtime_status", F_STATUS, 0444},
      {"runtime_active_time", F_ACTIVE, 0444},
      {"runtime_suspended_time", F_SUSPENDED, 0444},
      {"autosuspend_delay_ms", F_DELAY, 0644},
  };
  struct sysfs_dir *dir = 0;
  char part[64];
  const char *p = sysfs_dir;

  /* Walk (and create) the path one directory at a time. */
  while (p && *p) {
    usize n = 0;

    while (p[n] && p[n] != '/' && n < sizeof(part) - 1) {
      part[n] = p[n];
      n++;
    }
    part[n] = 0;
    dir = sysfs_reg_dir(dir, part);
    if (!dir)
      return;
    p += n;
    if (*p == '/')
      p++;
  }
  if (!dir || !(dir = sysfs_reg_dir(dir, "power")))
    return;
  for (usize i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    struct rpm_file *f = kzalloc(sizeof(*f));

    if (!f)
      return;
    f->id = id;
    f->which = files[i].which;
    if (sysfs_reg_attr(dir, files[i].name, files[i].mode, rpm_show,
                       files[i].mode & 0200 ? rpm_store : 0, f, 0) != 0)
      kfree(f);
  }
}

int rpm_register(const char *name, rpm_fn suspend, rpm_fn resume, void *ctx,
                 const char *sysfs_dir, u32 autosuspend_ms) {
  struct rpm_dev *d;
  int id;

  if (!name || !suspend || !resume || g_ndev >= RPM_MAX)
    return -1;
  if (!g_thread) {
    if (kthread_create("krpmd", krpmd, 0) < 0)
      return -1;
    g_thread = 1;
    /* Last in the order, so its suspend runs first: every device is back up
     * before the others save their state for the sleep. */
    suspend_register_device_ops("runtime-pm", rpm_system_suspend,
                                rpm_system_resume, 0);
  }
  id = g_ndev;
  d = &g_dev[id];
  memset(d, 0, sizeof(*d));
  d->name = name;
  d->suspend = suspend;
  d->resume = resume;
  d->ctx = ctx;
  d->delay_ms = autosuspend_ms;
  d->status = RPM_ACTIVE;
  d->since_ns = d->last_busy_ns = ktime_monotonic_ns();
  g_ndev++;
  rpm_publish(id, sysfs_dir);
  return id;
}
