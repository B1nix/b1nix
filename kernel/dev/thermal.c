/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Thermal management (M135). See include/b1nix/thermal.h.
 *
 * M134 made a thermal zone readable: _TMP, converted and published. Nothing
 * acted on it — a laptop could cook with the processor at full clock and the
 * fans off, and a zone past its critical point kept running until the
 * hardware's own trip cut the power without a sync. This is the part of
 * Linux's ACPI thermal driver and thermal core that does something about it:
 *
 *   critical   _CRT      an orderly power-off through /sbin/poweroff, and the
 *                        firmware's \_S5 directly if userspace does not finish
 *   hot        _HOT      said, to the log and to userspace (a uevent)
 *   passive    _PSV      the processor slowed one P-state at a time, by the
 *                        ACPI formula dP = _TC1 * (Tn - Tn-1) + _TC2 * (Tn - Tpsv)
 *                        every _TSP, and let back up the same way
 *   active     _ACx/_ALx the fans in _ALx switched on through the power
 *                        resources in their _PR0, and off again below _ACx
 *
 * The processor is one cooling device because every CPU here shares one
 * frequency policy; its states are the _PSS states below the first.
 */

#include <b1nix/thermal.h>

#include <b1nix/acpi_event.h>
#include <b1nix/acpi_power.h>
#include <b1nix/aml.h>
#include <b1nix/cpufreq.h>
#include <b1nix/kprintf.h>
#include <b1nix/ktime.h>
#include <b1nix/sched.h>
#include <b1nix/types.h>
#include <b1nix/uevent.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>

#include <stdio.h>
#include <string.h>

#define TZ_PATH_MAX 96
#define FAN_MAX_PR 4
#define TRIP_NONE (-1000000)

enum { TRIP_CRITICAL, TRIP_HOT, TRIP_PASSIVE, TRIP_ACTIVE };
static const char *const g_trip_names[] = {"critical", "hot", "passive",
                                           "active"};

enum { CDEV_PROCESSOR, CDEV_FAN };

struct trip {
  int kind;
  int mc;
  u16 fans; /* active trips: the cooling devices its _ALx names, a bit each */
};

struct zone {
  char path[TZ_PATH_MAX];
  struct trip trip[THERMAL_MAX_TRIPS];
  int ntrips;
  int passive_trip;   /* index into trip[], or -1 */
  int tc1, tc2;
  u32 tsp_ms, tzp_ms; /* passive sampling and polling periods; 0 = none */
  int emul_mc;        /* 0: the sensor */
  int prev_mc;
  int have_prev;
  int passive_state;  /* what this zone asks of the processor */
  u16 fans_on;        /* what this zone asks of the fans */
  int hot_said, critical_said;
  u64 next_ns;        /* when to look again; 0 = on an event only */
};

struct cdev {
  int kind;
  char path[TZ_PATH_MAX];
  char pr[FAN_MAX_PR][TZ_PATH_MAX]; /* a fan's power resources */
  int npr;
  int state;
};

static struct zone g_zone[ACPI_PS_MAX_THERMAL];
static int g_nzones;
static struct cdev g_cdev[THERMAL_MAX_CDEV];
static int g_ncdev;
static int g_proc_cdev = -1;
static int g_chan;
static u32 g_kick;   /* zones to look at now, a bit each */
static u32 g_reread; /* zones whose trips the firmware changed */
static u64 g_critical_at_ns;

/* ── reading the firmware ───────────────────────────────────────────────── */

static int eval_int(const char *base, const char *leaf, u64 *out) {
  char p[TZ_PATH_MAX + 8];
  struct aml_result r;

  snprintf(p, sizeof(p), "%s.%s", base, leaf);
  if (!aml_exists(p) || aml_evaluate(p, 0, 0, &r) != AML_OK ||
      r.type != AML_T_INTEGER)
    return -1;
  *out = r.integer;
  return 0;
}

/* Tenths of a kelvin to millidegrees Celsius. */
static int dk_to_mc(u64 dk) { return (int)((long long)dk * 100 - 273150); }

static int cdev_find(int kind, const char *path) {
  for (int i = 0; i < g_ncdev; i++)
    if (g_cdev[i].kind == kind && !strcmp(g_cdev[i].path, path))
      return i;
  return -1;
}

/* A fan named by an _ALx: the power resources in its _PR0 are its switch. */
static int fan_add(const char *path) {
  char p[TZ_PATH_MAX + 8];
  struct aml_result r;
  struct cdev *c;
  int i = cdev_find(CDEV_FAN, path);

  if (i >= 0)
    return i;
  if (g_ncdev >= THERMAL_MAX_CDEV)
    return -1;
  snprintf(p, sizeof(p), "%s._PR0", path);
  if (!aml_exists(p) || aml_evaluate(p, 0, 0, &r) != AML_OK ||
      r.type != AML_T_PACKAGE)
    return -1; /* no way to switch it: not a cooling device */
  c = &g_cdev[g_ncdev];
  memset(c, 0, sizeof(*c));
  c->kind = CDEV_FAN;
  strncpy(c->path, path, TZ_PATH_MAX - 1);
  for (u32 k = 0; k < r.length && c->npr < FAN_MAX_PR; k++) {
    struct aml_result e;

    if (aml_evaluate_element(p, 0, 0, k, &e) == AML_OK &&
        e.type == AML_T_STRING && e.bytes_copied < TZ_PATH_MAX) {
      memcpy(c->pr[c->npr], e.bytes, e.bytes_copied);
      c->pr[c->npr][e.bytes_copied] = 0;
      c->npr++;
    }
  }
  if (!c->npr)
    return -1;
  return g_ncdev++;
}

static void trip_add(struct zone *z, int kind, int mc, u16 fans) {
  if (z->ntrips >= THERMAL_MAX_TRIPS)
    return;
  z->trip[z->ntrips].kind = kind;
  z->trip[z->ntrips].mc = mc;
  z->trip[z->ntrips].fans = fans;
  z->ntrips++;
}

/* The trips, in Linux's order: critical, hot, passive, active0..9. */
static void zone_read_trips(struct zone *z) {
  u64 v;

  z->ntrips = 0;
  z->passive_trip = -1;
  if (eval_int(z->path, "_CRT", &v) == 0 && v > 2732)
    trip_add(z, TRIP_CRITICAL, dk_to_mc(v), 0);
  if (eval_int(z->path, "_HOT", &v) == 0 && v > 2732)
    trip_add(z, TRIP_HOT, dk_to_mc(v), 0);
  if (g_proc_cdev >= 0 && eval_int(z->path, "_PSV", &v) == 0 && v > 2732) {
    z->passive_trip = z->ntrips;
    trip_add(z, TRIP_PASSIVE, dk_to_mc(v), 0);
  }
  for (int i = 0; i < 10; i++) {
    char leaf[8], al[TZ_PATH_MAX + 8];
    struct aml_result r;
    u16 fans = 0;

    snprintf(leaf, sizeof(leaf), "_AC%d", i);
    if (eval_int(z->path, leaf, &v) != 0 || v <= 2732)
      break; /* the active trips are numbered without gaps */
    snprintf(al, sizeof(al), "%s._AL%d", z->path, i);
    if (aml_exists(al) && aml_evaluate(al, 0, 0, &r) == AML_OK &&
        r.type == AML_T_PACKAGE)
      for (u32 k = 0; k < r.length; k++) {
        struct aml_result e;
        char fan[TZ_PATH_MAX];
        int c;

        if (aml_evaluate_element(al, 0, 0, k, &e) != AML_OK ||
            e.type != AML_T_STRING || e.bytes_copied >= TZ_PATH_MAX)
          continue;
        memcpy(fan, e.bytes, e.bytes_copied);
        fan[e.bytes_copied] = 0;
        c = fan_add(fan);
        if (c >= 0)
          fans |= (u16)(1u << c);
      }
    trip_add(z, TRIP_ACTIVE, dk_to_mc(v), fans);
  }
  z->tc1 = eval_int(z->path, "_TC1", &v) == 0 ? (int)v : 0;
  z->tc2 = eval_int(z->path, "_TC2", &v) == 0 ? (int)v : 0;
  /* Both in tenths of a second. */
  z->tsp_ms = eval_int(z->path, "_TSP", &v) == 0 ? (u32)v * 100 : 0;
  z->tzp_ms = eval_int(z->path, "_TZP", &v) == 0 ? (u32)v * 100 : 0;
}

/* ── the cooling devices ────────────────────────────────────────────────── */

static int fan_is_on(const struct cdev *c) {
  u64 sta = 0;

  /* On when every power resource it needs is on, as _STA says. */
  for (int i = 0; i < c->npr; i++)
    if (eval_int(c->pr[i], "_STA", &sta) != 0 || !(sta & 1))
      return 0;
  return 1;
}

static int fan_switch(const struct cdev *c, int on) {
  int rc = 0;

  for (int i = 0; i < c->npr; i++) {
    char p[TZ_PATH_MAX + 8];
    struct aml_result r;

    snprintf(p, sizeof(p), "%s.%s", c->pr[i], on ? "_ON" : "_OFF");
    if (aml_evaluate(p, 0, 0, &r) != AML_OK)
      rc = -1;
  }
  return rc;
}

static int cdev_apply(int idx, int state) {
  struct cdev *c = &g_cdev[idx];
  int rc;

  if (c->kind == CDEV_PROCESSOR)
    rc = cpufreq_set_thermal_limit(state);
  else
    rc = fan_switch(c, state);
  if (rc == 0 && state != c->state) {
    k_info("thermal", "cooling_device%d (%s) state %d -> %d", idx,
           c->kind == CDEV_PROCESSOR ? "Processor" : "Fan", c->state, state);
    c->state = state;
  }
  return rc;
}

/* What every zone asks of the devices, combined: the processor as slow as
 * the hottest zone wants it, a fan on if any zone wants it on. */
static void cdev_settle(void) {
  int proc = 0;
  u16 fans = 0;

  for (int i = 0; i < g_nzones; i++) {
    if (g_zone[i].passive_state > proc)
      proc = g_zone[i].passive_state;
    fans |= g_zone[i].fans_on;
  }
  if (g_proc_cdev >= 0 && proc != g_cdev[g_proc_cdev].state)
    (void)cdev_apply(g_proc_cdev, proc);
  for (int c = 0; c < g_ncdev; c++) {
    int want = (fans >> c) & 1;

    if (g_cdev[c].kind == CDEV_FAN && want != g_cdev[c].state)
      (void)cdev_apply(c, want);
  }
}

/* ── acting on a temperature ────────────────────────────────────────────── */

/* Linux's orderly_poweroff: userspace shuts the machine down properly; if it
 * cannot even be started, sync and switch off here. */
static void critical_shutdown(int zone, int mc) {
  static const char *argv[] = {"/sbin/poweroff", 0};

  k_warn("thermal", "thermal_zone%d: critical temperature reached (%d C), "
                    "shutting down",
         zone, mc / 1000);
  g_critical_at_ns = ktime_monotonic_ns();
  if (user_spawn(argv[0], 1, argv) < 0) {
    k_warn("thermal", "no %s: powering off now", argv[0]);
    (void)vfs_sync();
    acpi_poweroff();
  }
}

static void zone_update(int idx) {
  struct zone *z = &g_zone[idx];
  int mc;

  if (thermal_zone_temp(idx, &mc) != 0)
    return;
  for (int t = 0; t < z->ntrips; t++) {
    struct trip *tr = &z->trip[t];

    if (tr->kind == TRIP_CRITICAL && mc >= tr->mc && !z->critical_said) {
      z->critical_said = 1;
      critical_shutdown(idx, mc);
    }
    if (tr->kind == TRIP_HOT) {
      if (mc >= tr->mc && !z->hot_said) {
        char name[24];

        k_warn("thermal", "thermal_zone%d: hot (%d C)", idx, mc / 1000);
        snprintf(name, sizeof(name), "/class/thermal/thermal_zone%d", idx);
        uevent_post("change", name, "thermal", 0, 0, -1, -1);
      }
      z->hot_said = mc >= tr->mc;
    }
  }

  /* Passive: the ACPI formula, one step per sample. */
  if (z->passive_trip >= 0) {
    int psv = z->trip[z->passive_trip].mc;
    int max = thermal_cdev_max_state(g_proc_cdev);

    if (mc >= psv || z->passive_state > 0) {
      /* In tenths of a degree, as the formula's constants expect. */
      long long d = (long long)z->tc1 * ((mc - (z->have_prev ? z->prev_mc
                                                              : mc)) / 100) +
                    (long long)z->tc2 * ((mc - psv) / 100);

      if (d > 0 && z->passive_state < max)
        z->passive_state++;
      else if (d < 0 && z->passive_state > 0)
        z->passive_state--;
    }
  }

  /* Active: every fan of every trip at or below the temperature. */
  {
    u16 fans = 0;

    for (int t = 0; t < z->ntrips; t++)
      if (z->trip[t].kind == TRIP_ACTIVE && mc >= z->trip[t].mc)
        fans |= z->trip[t].fans;
    z->fans_on = fans;
  }
  z->prev_mc = mc;
  z->have_prev = 1;

  {
    u32 ms = z->passive_state > 0 && z->tsp_ms ? z->tsp_ms : z->tzp_ms;

    z->next_ns = ms ? ktime_monotonic_ns() + (u64)ms * 1000000ull : 0;
  }
}

static void kthermald(void *arg) {
  (void)arg;
  for (;;) {
    u64 now = ktime_monotonic_ns(), next = 0;
    u32 kicked = __atomic_exchange_n(&g_kick, 0, __ATOMIC_ACQ_REL);
    u32 reread = __atomic_exchange_n(&g_reread, 0, __ATOMIC_ACQ_REL);

    for (int i = 0; i < g_nzones; i++) {
      struct zone *z = &g_zone[i];

      if (reread & (1u << i))
        zone_read_trips(z);
      if (kicked & (1u << i) || (z->next_ns && now >= z->next_ns))
        zone_update(i);
      if (z->next_ns && (!next || z->next_ns < next))
        next = z->next_ns;
    }
    cdev_settle();
    /* Userspace was told to power off and has not, 30 s on: do it here. An
     * OpenRC shutdown takes a couple of seconds; a hung one must not leave a
     * machine past its critical temperature running. */
    now = ktime_monotonic_ns(); /* the critical stamp may be newer than the
                                 * loop's first reading */
    if (g_critical_at_ns && now > g_critical_at_ns &&
        now - g_critical_at_ns > 30000000000ull) {
      k_warn("thermal", "the orderly power-off did not finish: forcing it");
      (void)vfs_sync();
      acpi_poweroff();
    }
    if (g_critical_at_ns && (!next || next > now + 1000000000ull))
      next = now + 1000000000ull;

    if (next) {
      u64 ms = next > now ? (next - now + 999999) / 1000000 : 1;

      scheduler_wait_prepare_timeout(&g_chan, SCHED_MS_TO_TICKS(ms));
    } else {
      scheduler_wait_prepare(&g_chan);
    }
    if (__atomic_load_n(&g_kick, __ATOMIC_ACQUIRE))
      scheduler_wait_cancel();
    else
      scheduler_wait_commit();
  }
}

static void kick(int zone) {
  __atomic_or_fetch(&g_kick, 1u << zone, __ATOMIC_ACQ_REL);
  scheduler_wake_all(&g_chan);
}

/* ── the interface ──────────────────────────────────────────────────────── */

void thermal_init(void) {
  int n = acpi_power_thermal_count();

  if (n <= 0)
    return;
  if (cpufreq_thermal_states() > 0) {
    g_cdev[0].kind = CDEV_PROCESSOR;
    strncpy(g_cdev[0].path, cpufreq_pss_path(), TZ_PATH_MAX - 1);
    g_proc_cdev = 0;
    g_ncdev = 1;
  }
  for (int i = 0; i < n && i < ACPI_PS_MAX_THERMAL; i++) {
    struct zone *z = &g_zone[g_nzones++];

    memset(z, 0, sizeof(*z));
    strncpy(z->path, acpi_power_thermal_path(i), TZ_PATH_MAX - 1);
    zone_read_trips(z);
    k_info("thermal", "thermal_zone%d %s: %d trip(s), passive every %u ms, "
                      "polled every %u ms",
           i, z->path, z->ntrips, z->tsp_ms, z->tzp_ms);
  }
  for (int c = 0; c < g_ncdev; c++)
    if (g_cdev[c].kind == CDEV_FAN)
      g_cdev[c].state = fan_is_on(&g_cdev[c]);
  if (kthread_create("kthermald", kthermald, 0) < 0) {
    k_warn("thermal", "no thread: the zones are published but not acted on");
    return;
  }
  for (int i = 0; i < g_nzones; i++)
    kick(i);
}

void thermal_notify(int idx, u64 value) {
  if (idx < 0 || idx >= g_nzones)
    return;
  if (value == 0x81)
    __atomic_or_fetch(&g_reread, 1u << idx, __ATOMIC_ACQ_REL);
  kick(idx);
}

int thermal_zone_temp(int zone, int *mc) {
  char buf[32];
  int emul;

  if (zone < 0 || zone >= g_nzones)
    return -1;
  emul = __atomic_load_n(&g_zone[zone].emul_mc, __ATOMIC_ACQUIRE);
  if (emul) {
    *mc = emul;
    return 0;
  }
  if (acpi_power_thermal_attr(zone, ACPI_TZ_TEMP, buf, sizeof(buf)) < 0)
    return -1;
  {
    int v = 0, neg = buf[0] == '-';

    for (const char *p = buf + neg; *p >= '0' && *p <= '9'; p++)
      v = v * 10 + (*p - '0');
    *mc = neg ? -v : v;
  }
  return 0;
}

int thermal_zone_set_emul(int zone, int mc) {
  if (zone < 0 || zone >= g_nzones)
    return -1;
  __atomic_store_n(&g_zone[zone].emul_mc, mc, __ATOMIC_RELEASE);
  kick(zone);
  return 0;
}

int thermal_zone_emul(int zone) {
  return (zone >= 0 && zone < g_nzones) ? g_zone[zone].emul_mc : 0;
}

int thermal_trip_count(int zone) {
  return (zone >= 0 && zone < g_nzones) ? g_zone[zone].ntrips : 0;
}

int thermal_trip(int zone, int trip, const char **type, int *mc) {
  if (zone < 0 || zone >= g_nzones || trip < 0 ||
      trip >= g_zone[zone].ntrips)
    return -1;
  *type = g_trip_names[g_zone[zone].trip[trip].kind];
  *mc = g_zone[zone].trip[trip].mc;
  return 0;
}

int thermal_cdev_count(void) { return g_ncdev; }

const char *thermal_cdev_type(int c) {
  if (c < 0 || c >= g_ncdev)
    return "";
  return g_cdev[c].kind == CDEV_PROCESSOR ? "Processor" : "Fan";
}

int thermal_cdev_max_state(int c) {
  if (c < 0 || c >= g_ncdev)
    return 0;
  return g_cdev[c].kind == CDEV_PROCESSOR ? cpufreq_thermal_states() : 1;
}

int thermal_cdev_cur_state(int c) {
  if (c < 0 || c >= g_ncdev)
    return 0;
  /* A fan's state is its power resources', read back from the firmware
   * rather than remembered. */
  return g_cdev[c].kind == CDEV_PROCESSOR ? cpufreq_thermal_limit()
                                          : fan_is_on(&g_cdev[c]);
}

int thermal_cdev_set_state(int c, int state) {
  if (c < 0 || c >= g_ncdev || state < 0 || state > thermal_cdev_max_state(c))
    return -1;
  return cdev_apply(c, state);
}

int thermal_zone_binding(int zone, int k, int *cdev, int *trip) {
  int seen = 0;

  if (zone < 0 || zone >= g_nzones)
    return -1;
  for (int t = 0; t < g_zone[zone].ntrips; t++) {
    const struct trip *tr = &g_zone[zone].trip[t];

    if (tr->kind == TRIP_PASSIVE && g_proc_cdev >= 0) {
      if (seen++ == k) {
        *cdev = g_proc_cdev;
        *trip = t;
        return 0;
      }
    }
    if (tr->kind == TRIP_ACTIVE)
      for (int c = 0; c < g_ncdev; c++)
        if ((tr->fans >> c) & 1 && seen++ == k) {
          *cdev = c;
          *trip = t;
          return 0;
        }
  }
  return -1;
}
