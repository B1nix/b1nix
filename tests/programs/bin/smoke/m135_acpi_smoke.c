/* SPDX-License-Identifier: GPL-2.0-only */
/* m135_acpi_smoke — ACPI events (M135).
 *
 * The SCI, the fixed power button, and a general-purpose event whose method
 * Notify()s a device, seen the way userspace sees them: a key on an input
 * device named "Power Button", a PCI function that appears with an "add"
 * uevent, and the counters under /sys/firmware/acpi/interrupts.
 *
 * Nothing inside the guest can press a button or plug a card, so the host does
 * it when asked: tests/support/acpi/acpi-actor.py watches the serial log for
 * the two request lines below and answers over QMP with system_powerdown (the
 * power button) and device_add of a virtio-rng (ACPI PCI hotplug on the pc
 * machine: QEMU raises GPE 1, and the DSDT's _E01 notifies the slot).
 *
 * The frequency policy is here too, because the P-states it governs are the
 * lane's ACPI fixture: the platform's _PPC ceiling, the ondemand governor
 * following load, scaling_max_freq, and the Linux policy0 layout.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtc.h>

#define KEY_POWER_ 116
#define EV_KEY_ 1
#define EVIOCGNAME_(len) _IOC(_IOC_READ, 'E', 0x06, len)

struct lx_input_event {
  int64_t sec, usec;
  uint16_t type, code;
  int32_t value;
};

static int fails;

static long read_long(const char *path);
static void read_str(const char *path, char *buf, int cap);
static int write_str(const char *path, const char *text);

static void ok(const char *what) {
  printf("M135-ACPI: ok %s\n", what);
  fflush(stdout);
}

static void bad(const char *what, const char *why, long v) {
  printf("M135-ACPI: FAIL %s — %s (%ld, errno=%d)\n", what, why, v, errno);
  fflush(stdout);
  fails++;
}

/* A counter from /sys/firmware/acpi/interrupts, or -1. */
static long counter(const char *name) {
  char path[96], buf[96];
  int fd, n;

  snprintf(path, sizeof(path), "/sys/firmware/acpi/interrupts/%s", name);
  fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  n = (int)read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return -1;
  buf[n] = 0;
  return strtol(buf, 0, 10);
}

/* The event device with this name, opened, or -1. */
static int find_input(const char *want) {
  for (int i = 0; i < 16; i++) {
    char path[32], name[64] = "";
    int fd;

    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      continue;
    if (ioctl(fd, EVIOCGNAME_(sizeof(name)), name) >= 0 &&
        strcmp(name, want) == 0)
      return fd;
    close(fd);
  }
  return -1;
}

static long now_ms(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The wakeup_count handshake around the power button press. */
static long g_wc_before = -1;
static char g_wc_stale[24];

static void check_power_button(void) {
  int fd = find_input("Power Button");
  long pwr0 = counter("ff_pwr_btn"), sci0 = counter("sci");

  g_wc_before = read_long("/sys/power/wakeup_count");
  snprintf(g_wc_stale, sizeof(g_wc_stale), "%ld", g_wc_before);
  int pressed = 0, released = 0;

  if (fd < 0) {
    bad("power-button-device", "no input device named \"Power Button\"", -1);
    return;
  }
  ok("power-button-device");
  printf("M135-ACPI: press the power button\n");
  fflush(stdout);
  for (long end = now_ms() + 30000; now_ms() < end && !released;) {
    struct pollfd p = {fd, POLLIN, 0};
    struct lx_input_event ev;

    if (poll(&p, 1, 1000) <= 0)
      continue;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
      if (ev.type == EV_KEY_ && ev.code == KEY_POWER_ && ev.value == 1)
        pressed = 1;
      if (ev.type == EV_KEY_ && ev.code == KEY_POWER_ && ev.value == 0 &&
          pressed)
        released = 1;
    }
  }
  close(fd);
  long pwr1 = counter("ff_pwr_btn"), sci1 = counter("sci");

  printf("M135-ACPI:   power button: press %d release %d, ff_pwr_btn %ld -> "
         "%ld, sci %ld -> %ld\n",
         pressed, released, pwr0, pwr1, sci0, sci1);
  if (pressed && released && pwr1 > pwr0 && sci1 > sci0)
    ok("power-button");
  else
    bad("power-button",
        "the press did not arrive as KEY_POWER down and up, or was not counted",
        pwr1);
}

/* A card plugged into a hotplug slot: QEMU raises the GPE, the DSDT's method
 * notifies the slot, and the kernel publishes the new function. */
static void check_pci_hotplug(void) {
  struct sockaddr_nl sa;
  int s = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
  long gpe0 = counter("gpe_all");
  int seen = 0;
  char slot[64] = "";

  memset(&sa, 0, sizeof(sa));
  sa.nl_family = AF_NETLINK;
  sa.nl_groups = 1;
  if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    bad("pci-hotplug", "no uevent socket", -1);
    if (s >= 0)
      close(s);
    return;
  }
  printf("M135-ACPI: plug a pci device\n");
  fflush(stdout);
  for (long end = now_ms() + 30000; now_ms() < end && !seen;) {
    struct pollfd p = {s, POLLIN, 0};
    char msg[2048];
    ssize_t n;
    int add = 0, pci = 0, rng = 0;

    if (poll(&p, 1, 1000) <= 0)
      continue;
    n = recv(s, msg, sizeof(msg) - 1, 0);
    if (n <= 0)
      continue;
    msg[n] = 0;
    for (ssize_t i = 0; i < n; i += (ssize_t)strlen(msg + i) + 1) {
      if (strcmp(msg + i, "ACTION=add") == 0)
        add = 1;
      if (strcmp(msg + i, "SUBSYSTEM=pci") == 0)
        pci = 1;
      /* virtio-rng: vendor 1af4, device 1005 (transitional) or 1044. */
      if (strncmp(msg + i, "DEVPATH=/devices/pci0000:00/", 28) == 0)
        snprintf(slot, sizeof(slot), "%s", msg + i + 28);
    }
    if (add && pci && slot[0]) {
      char path[128], v[16] = "";
      int fd;

      snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", slot);
      fd = open(path, O_RDONLY);
      if (fd >= 0) {
        if (read(fd, v, sizeof(v) - 1) > 0 && strncmp(v, "0x1af4", 6) == 0)
          rng = 1;
        close(fd);
      }
    }
    seen = add && pci && rng;
  }
  close(s);
  long gpe1 = counter("gpe_all");

  printf("M135-ACPI:   pci hotplug: uevent %d for %s, gpe_all %ld -> %ld\n",
         seen, slot[0] ? slot : "-", gpe0, gpe1);
  if (seen && gpe1 > gpe0)
    ok("pci-hotplug");
  else
    bad("pci-hotplug",
        "the GPE did not run its method, or the slot's Notify() did not "
        "bring the new function into /sys/bus/pci with an add uevent",
        gpe1);
}


#define CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq/"

/* A small file's contents, without the newline; "" when unreadable. */
static void read_str(const char *path, char *buf, int cap) {
  int fd = open(path, O_RDONLY), n = -1;

  buf[0] = 0;
  if (fd < 0)
    return;
  n = (int)read(fd, buf, (size_t)cap - 1);
  close(fd);
  buf[n > 0 ? n : 0] = 0;
  buf[strcspn(buf, "\n")] = 0;
}

static long read_long(const char *path) {
  char b[64];

  read_str(path, b, sizeof(b));
  return b[0] ? strtol(b, 0, 10) : -1;
}

/* 0, or the errno the write failed with. */
static int write_str(const char *path, const char *text) {
  int fd = open(path, O_WRONLY), rc = 0;

  if (fd < 0)
    return errno;
  if (write(fd, text, strlen(text)) != (ssize_t)strlen(text))
    rc = errno ? errno : EIO;
  close(fd);
  return rc;
}

/* A "<key> <number>" line out of /proc/b1nix-cpufreq, or -1. */
static long cpufreq_field(const char *key) {
  static char proc[4096];
  char want[48];
  int fd = open("/proc/b1nix-cpufreq", O_RDONLY), n;
  char *p;

  if (fd < 0)
    return -1;
  n = (int)read(fd, proc, sizeof(proc) - 1);
  close(fd);
  if (n <= 0)
    return -1;
  proc[n] = 0;
  snprintf(want, sizeof(want), "\n%s ", key);
  p = strstr(proc, want);
  return p ? strtol(p + strlen(want), 0, 10) : -1;
}

/* Wait up to `ms` for the selected state to become `want`. */
static int wait_state(long want, long ms) {
  for (long end = now_ms() + ms; now_ms() < end;) {
    if (cpufreq_field("selected") == want)
      return 1;
    usleep(20000);
  }
  return cpufreq_field("selected") == want;
}

static void check_cpufreq(void) {
  long states = cpufreq_field("states");
  long ppc, bios, smax, imax, ppc_khz, slowest_khz, t0, t1;
  char gov[64], link[64], govs[128], cpus[64];
  int ncpu = 0;
  pid_t child;
  int rc, went_up, came_down;

  if (states <= 0) {
    printf("M135-CPUFREQ: skip — the firmware declares no _PSS here\n");
    fflush(stdout);
    return;
  }

  /* policy0, and every CPU's cpufreq a link to it. */
  for (;; ncpu++) {
    char path[48];

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", ncpu);
    if (access(path, F_OK) != 0)
      break;
  }
  {
    int good = ncpu > 0;
    char want[128] = "";

    for (int i = 0; i < ncpu; i++) {
      char path[64];
      ssize_t n;

      snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq", i);
      n = readlink(path, link, sizeof(link) - 1);
      link[n > 0 ? n : 0] = 0;
      good = good && strcmp(link, "../cpufreq/policy0") == 0;
      snprintf(want + strlen(want), sizeof(want) - strlen(want), "%s%d",
               i ? " " : "", i);
    }
    read_str("/sys/devices/system/cpu/cpufreq/policy0/affected_cpus", cpus,
             sizeof(cpus));
    read_str(CPUFREQ "scaling_governor", gov, sizeof(gov));
    good = good && strcmp(cpus, want) == 0 && gov[0];
    printf("M135-CPUFREQ:   policy: %d CPUs, cpuN/cpufreq -> %s, affected_cpus "
           "\"%s\"\n",
           ncpu, link, cpus);
    if (good)
      ok("cpufreq-policy-layout");
    else
      bad("cpufreq-policy-layout",
          "cpuN/cpufreq is not a link to ../cpufreq/policy0 covering every CPU",
          -1);
  }

  /* The platform's ceiling: _PPC names a state, bios_limit is its frequency,
   * scaling_max_freq cannot exceed it, and performance runs at it. */
  ppc = cpufreq_field("ppc");
  bios = read_long(CPUFREQ "bios_limit");
  smax = read_long(CPUFREQ "scaling_max_freq");
  imax = read_long(CPUFREQ "cpuinfo_max_freq");
  printf("M135-CPUFREQ: ppc %ld bios_limit %ld\n", ppc, bios);
  printf("M135-CPUFREQ:   scaling_max_freq %ld cpuinfo_max_freq %ld selected "
         "%ld\n",
         smax, imax, cpufreq_field("selected"));
  if (ppc >= 0 && bios > 0 && smax == bios && imax >= bios &&
      strcmp(gov, "performance") == 0 && cpufreq_field("selected") == ppc)
    ok("cpufreq-ppc-ceiling");
  else
    bad("cpufreq-ppc-ceiling",
        "the platform's _PPC does not cap the policy, or performance runs "
        "above it",
        ppc);
  ppc_khz = bios;

  /* scaling_setspeed belongs to the userspace governor. */
  rc = write_str(CPUFREQ "scaling_setspeed", "600000");
  if (rc == EINVAL)
    ok("cpufreq-setspeed-refused");
  else
    bad("cpufreq-setspeed-refused",
        "scaling_setspeed took a frequency under the performance governor", rc);

  /* scaling_max_freq narrows the policy, and performance follows it. */
  {
    char text[32];
    long st2;

    slowest_khz = read_long(CPUFREQ "cpuinfo_min_freq");
    snprintf(text, sizeof(text), "%ld", slowest_khz);
    rc = write_str(CPUFREQ "scaling_max_freq", text);
    st2 = cpufreq_field("selected");
    smax = read_long(CPUFREQ "scaling_max_freq");
    printf("M135-CPUFREQ:   scaling_max_freq <- %ld: rc %d, reads %ld, "
           "selected %ld\n",
           slowest_khz, rc, smax, st2);
    snprintf(text, sizeof(text), "%ld", imax);
    write_str(CPUFREQ "scaling_max_freq", text);
    if (rc == 0 && smax == slowest_khz && st2 == states - 1 &&
        read_long(CPUFREQ "scaling_max_freq") == ppc_khz &&
        cpufreq_field("selected") == ppc)
      ok("cpufreq-policy-limit");
    else
      bad("cpufreq-policy-limit",
          "scaling_max_freq did not move the ceiling, or writing the processor "
          "maximum back did not return to the platform's",
          rc);
  }

  /* ondemand: idle goes to the slowest state, a spinning CPU to the ceiling,
   * and idle again back down. */
  read_str(CPUFREQ "scaling_available_governors", govs, sizeof(govs));
  if (!strstr(govs, "ondemand")) {
    bad("cpufreq-ondemand", "the ACPI driver does not offer ondemand", -1);
    return;
  }
  t0 = cpufreq_field("transitions");
  write_str("/sys/devices/system/cpu/cpufreq/ondemand/sampling_rate", "10000");
  rc = write_str(CPUFREQ "scaling_governor", "ondemand");
  read_str(CPUFREQ "scaling_governor", gov, sizeof(gov));
  came_down = rc == 0 && wait_state(states - 1, 3000);
  child = fork();
  if (child == 0) {
    for (volatile unsigned long x = 0;; x++)
      ;
  }
  went_up = child > 0 && wait_state(ppc, 3000);
  printf("M135-CPUFREQ:   ondemand: governor %s, idle %d, busy %d (load %ld)\n",
         gov, came_down, went_up, cpufreq_field("ondemand_load"));
  if (child > 0) {
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
  }
  came_down = came_down && wait_state(states - 1, 3000);
  t1 = cpufreq_field("transitions");
  write_str(CPUFREQ "scaling_governor", "performance");
  write_str("/sys/devices/system/cpu/cpufreq/ondemand/sampling_rate", "50000");
  {
    char tis[512];

    int fd = open(CPUFREQ "stats/time_in_state", O_RDONLY);
    int n = fd >= 0 ? (int)read(fd, tis, sizeof(tis) - 1) : -1;

    if (fd >= 0)
      close(fd);
    tis[n > 0 ? n : 0] = 0;
    for (char *q = tis; *q; q++)
      if (*q == '\n')
        *q = ';';
    printf("M135-CPUFREQ:   transitions %ld -> %ld, time_in_state %s\n", t0,
           t1, tis);
  }
  if (came_down && went_up && t1 >= t0 + 2)
    ok("cpufreq-ondemand");
  else
    bad("cpufreq-ondemand",
        "ondemand did not drop to the slowest state when idle, or did not "
        "raise the clock to the ceiling under load",
        t1 - t0);
}

/* ── idle states ─────────────────────────────────────────────────────── */

#define CPUIDLE "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/%s"

static long idle_attr(int cpu, int st, const char *what) {
  char path[96];

  snprintf(path, sizeof(path), CPUIDLE, cpu, st, what);
  return read_long(path);
}

static int idle_set_disable(int ncpu, int st, int on) {
  int rc = 0;

  for (int c = 0; c < ncpu; c++) {
    char path[96];

    snprintf(path, sizeof(path), CPUIDLE, c, st, "disable");
    rc |= write_str(path, on ? "1" : "0");
  }
  return rc;
}

/* Entries into state `st`, summed over the CPUs. */
static long idle_usage(int ncpu, int st) {
  long sum = 0;

  for (int c = 0; c < ncpu; c++)
    sum += idle_attr(c, st, "usage");
  return sum;
}

static void pin(int cpu) {
  cpu_set_t set;

  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  sched_setaffinity(0, sizeof(set), &set);
}

static void check_cpuidle(void) {
  char driver[32], gov[32], states[128] = "";
  int nst = 0, ncpu = 0;

  for (;; ncpu++) {
    char path[48];

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", ncpu);
    if (access(path, F_OK) != 0)
      break;
  }
  read_str("/sys/devices/system/cpu/cpuidle/current_driver", driver,
           sizeof(driver));
  read_str("/sys/devices/system/cpu/cpuidle/current_governor", gov,
           sizeof(gov));
  for (; nst < 8; nst++) {
    char name[16], path[96];

    snprintf(path, sizeof(path), CPUIDLE, 0, nst, "name");
    read_str(path, name, sizeof(name));
    if (!name[0])
      break;
    snprintf(states + strlen(states), sizeof(states) - strlen(states), "%s%s:%ld",
             nst ? " " : "", name, idle_attr(0, nst, "latency"));
  }
  printf("M135-IDLE: driver %s governor %s states %s\n", driver, gov, states);
  fflush(stdout);
  if (strcmp(driver, "acpi_idle") != 0) {
    printf("M135-IDLE: skip — no _CST on this platform\n");
    fflush(stdout);
    return;
  }
  if (strcmp(gov, "menu") == 0 && nst >= 2 &&
      idle_attr(0, 1, "residency") == 2 * idle_attr(0, 1, "latency"))
    ok("idle-cst");
  else
    bad("idle-cst", "the _CST states or their residencies are not published",
        nst);

  /* A quiet half second: long idles, so the deepest state. */
  {
    long d0 = idle_usage(ncpu, nst - 1), d1;

    usleep(500000);
    d1 = idle_usage(ncpu, nst - 1);
    printf("M135-IDLE:   quiet: %s entered %ld times\n", "deepest", d1 - d0);
    if (d1 > d0)
      ok("idle-deep-when-quiet");
    else
      bad("idle-deep-when-quiet", "a quiet machine never entered its deepest "
                                  "state",
          d1 - d0);
  }

  /* The deepest disabled: the governor takes the next one instead. */
  {
    long a0, a1, b0, b1;
    int rc = idle_set_disable(ncpu, nst - 1, 1);

    /* A CPU asleep in the state when it was disabled still wakes from it. */
    usleep(50000);
    a0 = idle_usage(ncpu, nst - 1);
    b0 = idle_usage(ncpu, nst - 2);
    usleep(300000);
    a1 = idle_usage(ncpu, nst - 1);
    b1 = idle_usage(ncpu, nst - 2);
    idle_set_disable(ncpu, nst - 1, 0);
    printf("M135-IDLE:   state%d disabled (rc %d): it %ld, state%d %ld\n",
           nst - 1, rc, a1 - a0, nst - 2, b1 - b0);
    if (rc == 0 && a1 == a0 && b1 > b0)
      ok("idle-disable");
    else
      bad("idle-disable", "a disabled state was still entered, or the next "
                          "one was not",
          a1 - a0);
  }

  /* A ping-pong across two CPUs: every idle is short, and the history says
   * so. The governor must stop taking the deepest state. */
  if (ncpu >= 2) {
    int ab[2], ba[2];
    long u0[8], u1[8], t0 = 0, t1 = 0, above0 = 0, above1 = 0, shallow = 0,
         deep, n = 0;
    pid_t child;
    char c = 0;

    for (int st = 0; st < nst; st++)
      u0[st] = idle_attr(1, st, "usage");
    for (int st = 0; st < nst; st++) {
      above0 += idle_attr(1, st, "above");
      t0 += idle_attr(1, st, "time");
    }
    if (pipe(ab) != 0 || pipe(ba) != 0) {
      bad("idle-follows-prediction", "pipe", -1);
      return;
    }
    child = fork();
    if (child == 0) {
      pin(1);
      while (read(ab[0], &c, 1) == 1 && c != 'q')
        (void)!write(ba[1], &c, 1);
      _exit(0);
    }
    pin(0);
    long pp_start = now_ms();
    /* Paced: a byte every 20 us, so cpu1's idles are short AND regular --
     * the pattern a predictor can learn, where an unpaced ping-pong on a
     * guest gives idles scattered over two orders of magnitude. */
    for (int i = 0; i < 4000; i++) {
      struct timespec a, b;

      clock_gettime(CLOCK_MONOTONIC, &a);
      c = 'p';
      if (write(ab[1], &c, 1) != 1 || read(ba[0], &c, 1) != 1)
        break;
      do
        clock_gettime(CLOCK_MONOTONIC, &b);
      while ((b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec) <
             20000);
    }
    long pp_ms = now_ms() - pp_start;

    c = 'q';
    (void)!write(ab[1], &c, 1);
    waitpid(child, 0, 0);
    {
      cpu_set_t all;

      CPU_ZERO(&all);
      for (int i = 0; i < ncpu; i++)
        CPU_SET(i, &all);
      sched_setaffinity(0, sizeof(all), &all);
    }
    for (int st = 0; st < nst; st++)
      u1[st] = idle_attr(1, st, "usage");
    for (int st = 0; st < nst; st++) {
      above1 += idle_attr(1, st, "above");
      t1 += idle_attr(1, st, "time");
      n += u1[st] - u0[st];
    }
    for (int st = 0; st + 1 < nst; st++)
      shallow += u1[st] - u0[st];
    deep = u1[nst - 1] - u0[nst - 1];
    printf("M135-IDLE:   ping-pong on cpu1: shallower states %ld, deepest %ld, "
           "too deep %ld, mean idle %ld us, round trip %ld us\n",
           shallow, deep, above1 - above0, n ? (t1 - t0) / n : -1,
           pp_ms * 1000 / 4000);
    long mean = n ? (t1 - t0) / n : 0;

    if (mean > idle_attr(1, nst - 1, "residency")) {
      /* A loaded host stretched every idle past the deepest state's
       * residency, and then the deepest state is the right answer: there
       * were no short idles to predict. Said, not passed. */
      printf("M135-IDLE: skip idle-follows-prediction — the host stretched "
             "the idles to %ld us\n",
             mean);
      fflush(stdout);
    } else if (shallow > deep)
      ok("idle-follows-prediction");
    else
      bad("idle-follows-prediction",
          "short idles still went to the deepest state", deep);
  }
}

/* ── thermal ─────────────────────────────────────────────────────────── */

#define TZ0 "/sys/class/thermal/thermal_zone0/"
#define CDEV "/sys/class/thermal/cooling_device%d/%s"

static long cdev_attr(int cd, const char *what) {
  char path[96];

  snprintf(path, sizeof(path), CDEV, cd, what);
  return read_long(path);
}

static void emul(long mc) {
  char text[24];

  snprintf(text, sizeof(text), "%ld", mc);
  write_str(TZ0 "emul_temp", text);
}

/* Wait up to `ms` for cooling device `cd` to report `want` (or, with
 * want < 0, any state above 0). */
static int wait_cdev(int cd, long want, long ms) {
  for (long end = now_ms() + ms;; usleep(50000)) {
    long v = cdev_attr(cd, "cur_state");

    if (want < 0 ? v > 0 : v == want)
      return 1;
    if (now_ms() >= end)
      return 0;
  }
}

static void check_thermal(void) {
  char trips[256] = "", cdevs[128] = "";
  int ncdev = 0, proc = -1, fan0 = -1, fan1 = -1;

  if (access(TZ0 "trip_point_0_type", F_OK) != 0) {
    printf("M135-THERMAL: skip — the firmware declares no trip points\n");
    fflush(stdout);
    return;
  }
  for (int t = 0; t < 16; t++) {
    char p1[80], type[24];
    long mc;

    snprintf(p1, sizeof(p1), TZ0 "trip_point_%d_type", t);
    read_str(p1, type, sizeof(type));
    if (!type[0])
      break;
    snprintf(p1, sizeof(p1), TZ0 "trip_point_%d_temp", t);
    mc = read_long(p1);
    snprintf(trips + strlen(trips), sizeof(trips) - strlen(trips), "%s%s:%ld",
             t ? " " : "", type, mc);
  }
  for (;; ncdev++) {
    char path[96], type[24];

    snprintf(path, sizeof(path), CDEV, ncdev, "type");
    read_str(path, type, sizeof(type));
    if (!type[0])
      break;
    if (!strcmp(type, "Processor"))
      proc = ncdev;
    else if (fan0 < 0)
      fan0 = ncdev;
    else if (fan1 < 0)
      fan1 = ncdev;
    snprintf(cdevs + strlen(cdevs), sizeof(cdevs) - strlen(cdevs), "%s%s",
             ncdev ? " " : "", type);
  }
  printf("M135-THERMAL: trips %s\n", trips);
  printf("M135-THERMAL: cdevs %s\n", cdevs);
  fflush(stdout);
  if (proc < 0 || fan0 < 0 || fan1 < 0) {
    bad("thermal-cdevs", "the processor and the two fans are not all cooling "
                         "devices",
        ncdev);
    return;
  }

  /* 75 C: past _PSV and _AC1. The processor slows a step at a time and the
   * first fan comes on. */
  {
    long lim = read_long(CPUFREQ "bios_limit");
    int up, fan_on, fan_off;
    long smax, st;

    emul(75000);
    up = wait_cdev(proc, -1, 5000);
    usleep(1500000); /* a few _TSP steps */
    st = cdev_attr(proc, "cur_state");
    smax = read_long(CPUFREQ "scaling_max_freq");
    fan_on = wait_cdev(fan0, 1, 2000);
    fan_off = cdev_attr(fan1, "cur_state") == 0;
    printf("M135-THERMAL:   75 C: processor state %ld of %ld, scaling_max_freq "
           "%ld (bios_limit %ld), fans %ld %ld\n",
           st, cdev_attr(proc, "max_state"), smax, lim,
           cdev_attr(fan0, "cur_state"), cdev_attr(fan1, "cur_state"));
    if (up && st > 0 && smax < lim)
      ok("thermal-passive");
    else
      bad("thermal-passive", "past _PSV the processor was not slowed", st);

    /* 85 C: past _AC0 too, both fans. */
    emul(85000);
    if (fan_on && fan_off && wait_cdev(fan1, 1, 2000) &&
        cdev_attr(fan0, "cur_state") == 1)
      ok("thermal-active");
    else
      bad("thermal-active", "the fans _AL1 and _AL0 name did not come on at "
                            "their trips, through their power resources",
          cdev_attr(fan1, "cur_state"));

    /* 50 C: everything lets go. */
    emul(50000);
    {
      int calm = wait_cdev(proc, 0, 8000) && wait_cdev(fan0, 0, 2000) &&
                 wait_cdev(fan1, 0, 2000);
      long back = read_long(CPUFREQ "scaling_max_freq");

      emul(0);
      printf("M135-THERMAL:   50 C: processor %ld, fans %ld %ld, "
             "scaling_max_freq %ld; sensor again %ld\n",
             cdev_attr(proc, "cur_state"), cdev_attr(fan0, "cur_state"),
             cdev_attr(fan1, "cur_state"), back, read_long(TZ0 "temp"));
      if (calm && back == lim)
        ok("thermal-release");
      else
        bad("thermal-release", "cooling was not released below the trips",
            back);
    }
  }
}

/* /sys/power/wakeup_count and /sys/class/wakeup, around the power button
 * press check_power_button made. */
static void check_wakeup(void) {
  char ms[48], name[32];
  long after = read_long("/sys/power/wakeup_count"), btn_events = -1;
  int stale, fresh, i;
  char text[24];

  read_str("/sys/power/mem_sleep", ms, sizeof(ms));
  for (i = 0; i < 8; i++) {
    char path[64];

    snprintf(path, sizeof(path), "/sys/class/wakeup/wakeup%d/name", i);
    read_str(path, name, sizeof(name));
    if (!name[0])
      break;
    if (!strcmp(name, "acpi-button")) {
      snprintf(path, sizeof(path), "/sys/class/wakeup/wakeup%d/event_count", i);
      btn_events = read_long(path);
    }
  }
  stale = write_str("/sys/power/wakeup_count", g_wc_stale);
  snprintf(text, sizeof(text), "%ld", after);
  fresh = write_str("/sys/power/wakeup_count", text);
  printf("M135-WAKEUP: mem_sleep \"%s\", wakeup_count %ld -> %ld, stale write "
         "%d, fresh write %d, acpi-button events %ld\n",
         ms, g_wc_before, after, stale, fresh, btn_events);
  fflush(stdout);
  if (strstr(ms, "s2idle") && after > g_wc_before && stale == EINVAL &&
      fresh == 0 && btn_events > 0)
    ok("wakeup-count");
  else
    bad("wakeup-count", "the power button press was not a wakeup event, or "
                        "the wakeup_count handshake did not refuse a stale "
                        "count",
        after);
  /* Armed with a fresh count, then an alarm fires: the next suspend must be
   * refused (EBUSY) rather than sleep through an event userspace never saw. */
  {
    int rtc = open("/dev/rtc0", O_RDONLY);
    struct rtc_wkalrm alarm;
    struct rtc_time t;
    int armed = -1, rc;
    long before;

    if (rtc >= 0 && ioctl(rtc, RTC_RD_TIME, &t) == 0) {
      memset(&alarm, 0, sizeof(alarm));
      alarm.enabled = 1;
      alarm.time = t;
      alarm.time.tm_sec += 2;
      if (alarm.time.tm_sec >= 60) {
        alarm.time.tm_sec -= 60;
        if (++alarm.time.tm_min >= 60) {
          alarm.time.tm_min = 0;
          alarm.time.tm_hour = (alarm.time.tm_hour + 1) % 24;
        }
      }
      before = read_long("/sys/power/wakeup_count");
      snprintf(text, sizeof(text), "%ld", before);
      armed = write_str("/sys/power/wakeup_count", text);
      if (armed == 0)
        armed = ioctl(rtc, RTC_WKALM_SET, &alarm) == 0 ? 0 : errno;
    }
    for (long end = now_ms() + 4000;
         now_ms() < end && read_long("/sys/power/wakeup_count") <= before;)
      usleep(100000);
    rc = write_str("/sys/power/state", "freeze");
    printf("M135-WAKEUP:   armed %d, alarm fired: wakeup_count %ld, then "
           "freeze -> %d\n",
           armed, read_long("/sys/power/wakeup_count"), rc);
    if (rtc >= 0)
      close(rtc);
    if (armed == 0 && rc == EBUSY)
      ok("wakeup-count-aborts");
    else
      bad("wakeup-count-aborts", "a suspend went ahead although a wakeup "
                                 "event arrived after wakeup_count was written",
          rc);
  }
}

/* ── the battery's static information and its alarm ──────────────────── */

#define BAT0 "/sys/class/power_supply/BAT0/"

static void check_battery(void) {
  char tech[24], model[48], serial[24], maker[24], lvl0[16], lvl1[16], lvl2[16];
  long cycles, design, vmin, now, alarm;
  int w1, w0;
  char text[24];

  if (access(BAT0 "cycle_count", F_OK) != 0) {
    printf("M135-BAT: skip — no battery with _BIX here\n");
    fflush(stdout);
    return;
  }
  cycles = read_long(BAT0 "cycle_count");
  design = read_long(BAT0 "energy_full_design");
  vmin = read_long(BAT0 "voltage_min_design");
  read_str(BAT0 "technology", tech, sizeof(tech));
  read_str(BAT0 "model_name", model, sizeof(model));
  read_str(BAT0 "serial_number", serial, sizeof(serial));
  read_str(BAT0 "manufacturer", maker, sizeof(maker));
  printf("M135-BAT: cycles %ld design %ld vmin %ld tech %s model %s serial %s "
         "maker %s\n",
         cycles, design, vmin, tech, model, serial, maker);

  /* The alarm: above what is left, the level reads Low; cleared, Normal. */
  now = read_long(BAT0 "energy_now");
  read_str(BAT0 "capacity_level", lvl0, sizeof(lvl0));
  snprintf(text, sizeof(text), "%ld", now + 100000);
  w1 = write_str(BAT0 "alarm", text);
  alarm = read_long(BAT0 "alarm");
  read_str(BAT0 "capacity_level", lvl1, sizeof(lvl1));
  w0 = write_str(BAT0 "alarm", "0");
  read_str(BAT0 "capacity_level", lvl2, sizeof(lvl2));
  printf("M135-BAT:   alarm %s -> %d, reads %ld; level %s, %s, %s\n", text, w1,
         alarm, lvl0, lvl1, lvl2);
  fflush(stdout);
  if (w1 == 0 && w0 == 0 && alarm == now + 100000 && !strcmp(lvl1, "Low") &&
      !strcmp(lvl2, lvl0) && strcmp(lvl0, "Low"))
    ok("battery-alarm");
  else
    bad("battery-alarm", "_BTP did not take the alarm, or capacity_level did "
                         "not follow it",
        w1);

  /* charge_behaviour: what the firmware can do, from _BMD, the choice handed
   * to _BMC, and _BMD reading back what it was asked -- each mode in turn,
   * then back to auto; a mode that does not exist is refused. */
  {
    static const char *modes[] = {"inhibit-charge", "force-discharge", "auto"};
    static const char *want[] = {"auto [inhibit-charge] force-discharge",
                                 "auto inhibit-charge [force-discharge]",
                                 "[auto] inhibit-charge force-discharge"};
    char cb0[64], cb[64];
    int good = 1, wbad;

    read_str(BAT0 "charge_behaviour", cb0, sizeof(cb0));
    printf("M135-BAT:   charge_behaviour \"%s\"\n", cb0);
    if (strcmp(cb0, "[auto] inhibit-charge force-discharge"))
      good = 0;
    for (unsigned k = 0; good && k < sizeof(modes) / sizeof(modes[0]); k++) {
      int w = write_str(BAT0 "charge_behaviour", modes[k]);

      read_str(BAT0 "charge_behaviour", cb, sizeof(cb));
      printf("M135-BAT:   %s -> %d, reads \"%s\"\n", modes[k], w, cb);
      if (w != 0 || strcmp(cb, want[k]))
        good = 0;
    }
    wbad = write_str(BAT0 "charge_behaviour", "overcharge");
    fflush(stdout);
    if (good && wbad == EINVAL)
      ok("charge-behaviour");
    else
      bad("charge-behaviour", "_BMC did not take the mode, or _BMD did not "
                              "report it back, or a bogus mode was accepted",
          wbad);
  }
}

/* ── runtime PM ────────────────────────────────────────────────────────── */

/* The D-state of a function, from the PM capability in its config space, or
 * -1 when it has none. */
static int pci_dstate(const char *slot) {
  unsigned char cfg[256];
  char path[128];
  int fd, n;

  snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", slot);
  fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  n = (int)read(fd, cfg, sizeof(cfg));
  close(fd);
  if (n < 64 || !(cfg[6] & 0x10))
    return -1;
  for (int p = cfg[0x34] & 0xfc, hops = 0; p && p < n - 5 && hops < 48;
       p = cfg[p + 1] & 0xfc, hops++)
    if (cfg[p] == 0x01)
      return cfg[p + 4] & 3;
  return -1;
}

static int wait_status(const char *dir, const char *want, long ms) {
  char path[160], st[24];

  snprintf(path, sizeof(path), "%s/power/runtime_status", dir);
  for (long end = now_ms() + ms;; usleep(50000)) {
    read_str(path, st, sizeof(st));
    if (!strcmp(st, want))
      return 1;
    if (now_ms() >= end)
      return 0;
  }
}

static void check_runtime_pm(void) {
  DIR *d = opendir("/sys/bus/pci/devices");
  struct dirent *de;
  char slot[32] = "", dir[96], path[160];
  int suspended, d3, resumed, d0, rd = -1;
  long act0, act1, susp;
  char buf[4096];

  while (d && (de = readdir(d))) {
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/power/control",
             de->d_name);
    if (de->d_name[0] != '.' && access(path, F_OK) == 0) {
      snprintf(slot, sizeof(slot), "%s", de->d_name);
      break;
    }
  }
  if (d)
    closedir(d);
  if (!slot[0]) {
    printf("M135-RPM: skip — no device under runtime PM here\n");
    fflush(stdout);
    return;
  }
  snprintf(dir, sizeof(dir), "/sys/bus/pci/devices/%s", slot);
  snprintf(path, sizeof(path), "%s/power/autosuspend_delay_ms", dir);
  write_str(path, "300");
  snprintf(path, sizeof(path), "%s/power/control", dir);
  write_str(path, "auto");

  suspended = wait_status(dir, "suspended", 5000);
  d3 = pci_dstate(slot);
  snprintf(path, sizeof(path), "%s/power/runtime_suspended_time", dir);
  usleep(200000);
  susp = read_long(path);
  snprintf(path, sizeof(path), "%s/power/runtime_active_time", dir);
  act0 = read_long(path);

  /* A read the block cache cannot answer: the disk must wake for it. */
  {
    int fd = open("/dev/nvme0n1", O_RDONLY);
    off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;

    if (fd >= 0 && size > 8192) {
      rd = (int)pread(fd, buf, sizeof(buf), (size / 2 / 4096 + 7) * 4096);
      close(fd);
    } else if (fd >= 0) {
      close(fd);
    }
  }
  snprintf(path, sizeof(path), "%s/power/runtime_status", dir);
  {
    char st[24];

    read_str(path, st, sizeof(st));
    resumed = !strcmp(st, "active") || !strcmp(st, "suspended");
  }
  d0 = pci_dstate(slot);
  snprintf(path, sizeof(path), "%s/power/runtime_active_time", dir);
  act1 = read_long(path);
  /* And "on" keeps it up. */
  snprintf(path, sizeof(path), "%s/power/control", dir);
  write_str(path, "on");
  printf("M135-RPM: %s suspended %d (D%d, %ld ms), read %d -> D%d, active "
         "time %ld -> %ld ms\n",
         slot, suspended, d3, susp, rd, d0, act0, act1);
  fflush(stdout);
  if (suspended && d3 == 3 && susp > 0 && rd == (int)sizeof(buf) && resumed &&
      act1 > act0 && wait_status(dir, "active", 2000) && pci_dstate(slot) == 0)
    ok("runtime-pm");
  else
    bad("runtime-pm", "an idle device with control=auto did not reach D3hot, "
                      "or did not come back to D0 for a read",
        rd);
}

int main(void) {
  printf("M135-ACPI: start\n");
  fflush(stdout);
  if (counter("sci") < 0) {
    /* No SCI to count: a hardware-reduced platform (arm64), whose events are
     * not this file's. Said, not passed. */
    printf("M135-ACPI: skip acpi-events — no /sys/firmware/acpi/interrupts on "
           "this platform\n");
    printf("M135-ACPI: done\n");
    return 0;
  }
  if (counter("sci_not") >= 0 && counter("gpe_all") >= 0 &&
      counter("ff_pwr_btn") >= 0)
    ok("interrupts-sysfs");
  else
    bad("interrupts-sysfs", "a counter under /sys/firmware/acpi/interrupts is "
                            "missing", -1);
  check_power_button();
  check_pci_hotplug();
  check_wakeup();
  check_battery();
  check_cpufreq();
  check_cpuidle();
  check_thermal();
  check_runtime_pm();
  printf("M135-ACPI: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
