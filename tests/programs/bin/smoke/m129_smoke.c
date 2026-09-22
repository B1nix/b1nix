/* SPDX-License-Identifier: GPL-2.0-only */
/* m129_smoke — power management: a machine that stops ticking when it has
 * nothing to do, and says so where a tool can read it (M129).
 *
 * The measurement is the point. A kernel can claim to be tickless and still
 * take a thousand timer interrupts a second; the only honest test is to sit
 * still for a second and count them. /proc/interrupts' LOC row is that count.
 *
 * Markers (only emitted on verified success):
 *   M129-SMOKE: start
 *   M129-SMOKE: ok loc-counter
 *   M129-SMOKE: ok idle-measured
 *   M129-SMOKE: ok idle-is-quiet    (a tickless kernel whose fixed beat is >= 1 kHz)
 *   M129-SMOKE: ok idle-not-worse  (a tickless kernel already ticking at 100 Hz)
 *   M129-SMOKE: ok busy-still-ticks
 *   M129-SMOKE: ok cpuidle-sysfs
 *   M129-SMOKE: ok cpufreq-honest
 *   M129-SMOKE: ok cpufreq-governor-set (only where a driver exists)
 *   M129-SMOKE: ok pstates-declared    (only where ACPI declares _PSS)
 *   M129-SMOKE: ok pstates-selected
 *   M129-PSTATE: khz <list>            (what the firmware declared, in order)
 *   M129-PSTATE: control <list>
 *   M129-PSTATE: port <the register _PCT names>
 *   M129-SMOKE: idle <n> busy <n> per second
 *   M129-SMOKE: done
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails;

static void ok(const char *what) {
  printf("M129-SMOKE: ok %s\n", what);
  fflush(stdout);
}

static void bad(const char *what, const char *why, long v) {
  printf("M129-SMOKE: fail %s (%s, %ld)\n", what, why, v);
  fflush(stdout);
  fails++;
}

static void judge(const char *what, int good, const char *why, long v) {
  if (good)
    ok(what);
  else
    bad(what, why, v);
}

static int read_file(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  ssize_t n;

  if (fd < 0)
    return -1;
  n = read(fd, buf, cap - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = 0;
  return (int)n;
}

/* The LOC row of /proc/interrupts: local timer interrupts, machine-wide. */
static long loc_count(void) {
  char buf[4096], *p;

  if (read_file("/proc/interrupts", buf, sizeof(buf)) < 0)
    return -1;
  p = strstr(buf, "LOC:");
  if (!p)
    return -1;
  return strtol(p + 4, 0, 10);
}

static long monotonic_ms(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void) {
  long a, b, idle_rate = 0, busy_rate = 0;
  char buf[256];

  printf("M129-SMOKE: start\n");
  fflush(stdout);

  a = loc_count();
  judge("loc-counter", a > 0,
        "/proc/interrupts has no LOC row, so nothing here can be measured", a);
  if (a <= 0) {
    printf("M129-SMOKE: done\n");
    return 1;
  }

  /* One second asleep. A tickless kernel programs the timer for the next real
   * deadline and takes a handful of interrupts; a kernel with a fixed 1 kHz
   * beat takes a thousand per CPU. Other things on the machine still wake up,
   * so the bound is generous — it is there to tell one design from the other,
   * not to measure a specific number. */
  {
    struct timespec nap = {1, 0};
    long t0 = monotonic_ms(), t1;

    a = loc_count();
    nanosleep(&nap, 0);
    b = loc_count();
    t1 = monotonic_ms();
    if (t1 <= t0)
      t1 = t0 + 1;
    idle_rate = (b - a) * 1000 / (t1 - t0);
  }
  /* What the bound is depends on what this kernel was asked to do. With
   * b1nix.dynticks the timer is programmed for the next deadline and an idle
   * second is cheap; without it the tick is fixed and an idle second costs
   * one interrupt per millisecond per CPU — which is the default today, and
   * saying it plainly is better than a check that passes either way. */
  {
    char tick[256], *p;
    long hz = 0, cap = 0, ncpu = (long)sysconf(_SC_NPROCESSORS_ONLN);

    /* Ask the kernel what it is doing rather than guessing from the command
     * line: the default has changed once already, and a check that reads the
     * command line passes on a kernel that no longer behaves that way. */
    if (read_file("/proc/b1nix-tick", tick, sizeof(tick)) > 0) {
      if ((p = strstr(tick, "hz ")) != 0)
        hz = strtol(p + 3, 0, 10);
      if ((p = strstr(tick, "dynticks_cap ")) != 0)
        cap = strtol(p + 13, 0, 10);
    }
    if (ncpu < 1)
      ncpu = 1;
    judge("idle-measured", idle_rate > 0 && hz > 0,
          "an idle second was measured as zero timer interrupts, or the "
          "kernel does not say what its tick is",
          idle_rate);
    if (cap > 0 && hz * ncpu >= 1000)
      /* Tickless, on a kernel whose fixed beat would be fast enough for the
       * cap to matter: an idle second must cost clearly less. Half is a wide
       * bound on purpose — what is being told apart is a timer programmed for
       * the next deadline from one that fires every millisecond. */
      judge("idle-is-quiet", idle_rate < (hz * ncpu) / 2,
            "the timer is capped for idle CPUs and an idle second still costs "
            "what a fixed tick would",
            idle_rate);
    else if (cap > 0)
      /* A 100 Hz kernel already ticks every ten milliseconds, and the things
       * that wake on this machine ask for about that: capping the idle
       * interval can only save what nobody asked for. The claim here is
       * therefore the weaker true one — the cap costs nothing — and the
       * number is printed for whoever wants the trend. */
      judge("idle-not-worse", idle_rate <= hz * ncpu,
            "an idle second costs more than the fixed beat would, so the "
            "capped timer is firing more often than the tick it replaced",
            idle_rate);
    else
      printf("M129-SMOKE: fixed tick, idle costs %ld of %ld interrupts\n",
             idle_rate, hz * ncpu);
    printf("M129-SMOKE: hz %ld cap %ld cpus %ld\n", hz, cap, ncpu);
    fflush(stdout);
  }

  /* And the tick is still there when there is something to preempt: a
   * kernel that simply stopped its timer would pass the check above and fail
   * every scheduling guarantee. */
  {
    long t0 = monotonic_ms(), t1;
    volatile unsigned long spin = 0;

    a = loc_count();
    while (monotonic_ms() - t0 < 1000)
      spin++;
    b = loc_count();
    t1 = monotonic_ms();
    if (t1 <= t0)
      t1 = t0 + 1;
    busy_rate = (b - a) * 1000 / (t1 - t0);
  }
  judge("busy-still-ticks", busy_rate > 100,
        "a CPU burning a whole second took almost no timer interrupts, so "
        "nothing would preempt it", busy_rate);

  printf("M129-SMOKE: idle %ld busy %ld per second\n", idle_rate, busy_rate);
  fflush(stdout);

  /* What the machine says about its idle states. Linux publishes one
   * directory per state; a reader (powertop, tuned, a monitoring agent) wants
   * the name and the counters. */
  if (read_file("/sys/devices/system/cpu/cpu0/cpuidle/state0/name", buf,
                sizeof(buf)) > 0) {
    char usage[64], time_us[64], desc[128];
    long used = -1;

    desc[0] = 0;
    read_file("/sys/devices/system/cpu/cpu0/cpuidle/state0/desc", desc,
              sizeof(desc));
    if (read_file("/sys/devices/system/cpu/cpu0/cpuidle/state0/usage", usage,
                  sizeof(usage)) > 0)
      used = strtol(usage, 0, 10);
    time_us[0] = 0;
    read_file("/sys/devices/system/cpu/cpu0/cpuidle/state0/time", time_us,
              sizeof(time_us));
    judge("cpuidle-sysfs", buf[0] && desc[0] && used > 0,
          "the idle state is published but its counters never moved, so the "
          "kernel is not going through it",
          used);
  } else {
    bad("cpuidle-sysfs", "/sys/devices/system/cpu/cpu0/cpuidle is missing", -1);
  }

  /* Frequency scaling, as the machine really has it. On a guest whose
   * hypervisor hides the power-management leaves there is no driver, and the
   * check is that the kernel says so instead of offering a governor that
   * cannot move the clock. */
  {
    char driver[64], govs[128], gov[64], cur[64];
    int have_driver;

    driver[0] = govs[0] = gov[0] = cur[0] = 0;
    read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_driver", driver,
              sizeof(driver));
    read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors",
              govs, sizeof(govs));
    read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", gov,
              sizeof(gov));
    read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", cur,
              sizeof(cur));
    have_driver = driver[0] && strncmp(driver, "none", 4) != 0;
    printf("M129-SMOKE: cpufreq driver %.*s governors %.*s at %.*s kHz\n",
           (int)strcspn(driver, "\n"), driver, (int)strcspn(govs, "\n"), govs,
           (int)strcspn(cur, "\n"), cur);
    fflush(stdout);
    judge("cpufreq-honest",
          driver[0] && govs[0] && gov[0] && strtol(cur, 0, 10) > 0 &&
              (have_driver || strstr(govs, "powersave") == 0),
          "cpufreq offers a governor the machine has no driver for, or has a "
          "driver and no current frequency",
          strtol(cur, 0, 10));
    if (have_driver) {
      int fd = open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
                    O_WRONLY);
      int wrote = -1;
      char back[64];

      if (fd >= 0) {
        wrote = (int)write(fd, "powersave", 9);
        close(fd);
      }
      back[0] = 0;
      read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", back,
                sizeof(back));
      judge("cpufreq-governor-set", wrote > 0 && strncmp(back, "powersave", 9) == 0,
            "writing a governor the driver supports did not take", wrote);
      fd = open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", O_WRONLY);
      if (fd >= 0) {
        (void)!write(fd, "performance", 11);
        close(fd);
      }
    }
  }

  /* ── the P-states ACPI declared (M129) ──────────────────────────────
   *
   * Only a machine whose firmware declares `_PSS` has any: a guest with no such
   * table says so and the lane records the rest as skipped. Where they exist,
   * every number printed here is the FIRMWARE's, read back out of
   * /proc/b1nix-cpufreq, and the lane compares it against what the table
   * declares — so this passes only if the interpreter read the table, the
   * driver kept the values, and the sysfs files agree with both. */
  {
    static char proc[4096];
    int n = read_file("/proc/b1nix-cpufreq", proc, sizeof(proc));
    long states = -1;
    char *p = n > 0 ? strstr(proc, "states ") : 0;

    if (p)
      states = strtol(p + 7, 0, 10);
    if (states <= 0) {
      printf("M129-SMOKE: no-pstates\n");
      fflush(stdout);
    } else {
      char khz_list[256] = "";
      char ctl_list[256] = "";
      char avail[256] = "";
      long first_khz = -1, last_khz = -1;
      int good = 1;

      /* One "state <i> khz <k> control 0x<c>" line per declared state. */
      for (long i = 0; i < states; i++) {
        char want[32];
        char *line;
        long khz, ctl;

        snprintf(want, sizeof(want), "state %ld khz ", i);
        line = strstr(proc, want);
        if (!line) {
          good = 0;
          break;
        }
        khz = strtol(line + strlen(want), 0, 10);
        line = strstr(line, "control ");
        ctl = line ? strtol(line + 8, 0, 16) : -1;
        if (khz <= 0 || ctl < 0) {
          good = 0;
          break;
        }
        if (i == 0)
          first_khz = khz;
        last_khz = khz;
        snprintf(khz_list + strlen(khz_list), sizeof(khz_list) - strlen(khz_list),
                 "%s%ld", i ? " " : "", khz);
        snprintf(ctl_list + strlen(ctl_list), sizeof(ctl_list) - strlen(ctl_list),
                 "%s0x%lx", i ? " " : "", ctl);
      }
      printf("M129-PSTATE: khz %s\n", khz_list);
      printf("M129-PSTATE: control %s\n", ctl_list);
      {
        char *sp = strstr(proc, "pct_space ");
        char *pp = strstr(proc, "pss_path ");

        printf("M129-PSTATE: space %.*s path %.*s\n",
               sp ? (int)strcspn(sp + 10, "\n") : 4, sp ? sp + 10 : "none",
               pp ? (int)strcspn(pp + 9, "\n") : 4, pp ? pp + 9 : "none");
      }
      fflush(stdout);

      /* sysfs must publish the same list, and its ends as the limits. */
      read_file("/sys/devices/system/cpu/cpu0/cpufreq/"
                "scaling_available_frequencies", avail, sizeof(avail));
      {
        char want[256];

        snprintf(want, sizeof(want), "%s\n", khz_list);
        good = good && strcmp(avail, want) == 0;
      }
      {
        char mx[32] = "", mn[32] = "";

        read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", mx,
                  sizeof(mx));
        read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", mn,
                  sizeof(mn));
        good = good && strtol(mx, 0, 10) == first_khz &&
               strtol(mn, 0, 10) == last_khz;
      }
      judge("pstates-declared", good,
            "the P-states, the sysfs list and the limits do not agree",
            states);

      /* Asking for the slowest state by frequency: the kernel must select the
       * state the firmware declared for it, and report what the register did
       * with the request rather than assuming. */
      {
        int fd = open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed",
                      O_WRONLY);
        char text[32];
        int wrote = -1;
        long sel = -1, cur = -1, took = -1;

        snprintf(text, sizeof(text), "%ld", last_khz);
        if (fd >= 0) {
          wrote = (int)write(fd, text, strlen(text));
          close(fd);
        }
        n = read_file("/proc/b1nix-cpufreq", proc, sizeof(proc));
        p = n > 0 ? strstr(proc, "selected ") : 0;
        if (p)
          sel = strtol(p + 9, 0, 10);
        p = n > 0 ? strstr(proc, "cur_khz ") : 0;
        if (p)
          cur = strtol(p + 8, 0, 10);
        p = n > 0 ? strstr(proc, "request_took ") : 0;
        if (p)
          took = strtol(p + 13, 0, 10);
        printf("M129-PSTATE: asked %ld selected %ld cur %ld took %ld\n",
               last_khz, sel, cur, took);
        fflush(stdout);
        judge("pstates-selected",
              wrote > 0 && sel == states - 1 && cur == last_khz && took == 1,
              "the request did not reach the register the firmware named",
              took);
        /* Put the machine back where it was found. */
        fd = open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed",
                  O_WRONLY);
        if (fd >= 0) {
          snprintf(text, sizeof(text), "%ld", first_khz);
          (void)!write(fd, text, strlen(text));
          close(fd);
        }
      }
    }
  }

  printf("M129-SMOKE: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
