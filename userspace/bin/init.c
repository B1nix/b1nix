/*
 * /bin/init — PID 1. Ring 3 orchestrator: auto-discovers and runs every test
 * binary in /bin (skipping the known non-test utilities/daemons/shells),
 * then either reboots (test mode) or falls into a real init's reap-any-orphan
 * loop. Replaces the old B1NXEXEC hack that had the kernel (Ring 0) hardcode
 * and interpret a fixed test binary list itself.
 *
 * Auto-discovery (rather than a hand-maintained list) means a new smoke test
 * binary just needs a files[] entry in kernel/fs/initramfs.c — no init.c edit
 * — to get picked up here. Only the small, stable set of non-test binaries
 * below needs to be kept in sync.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <syscall.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Shutdown/restart. /bin/init is a musl binary running under the Linux
 * personality, so it must issue Linux reboot(2) (nr 169, magic1/magic2/cmd) —
 * the native b1nix SYS_REBOOT number (84) is rmdir(2) on the Linux ABI, which
 * silently did nothing and let init fall through to _exit(). */
/* Mirrors kernel/include/b1nix/fb.h — /dev/fb0 claim query (int *). */
#define B1NIX_FBIOGET_CLAIM 0xFB03

#define LINUX_SYS_REBOOT        169
#define LINUX_REBOOT_MAGIC1     0xfee1deadu
#define LINUX_REBOOT_MAGIC2     672274793u /* 0x28121969 */
#define LINUX_REBOOT_CMD_RESTART   0x01234567u
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321fedcu

static inline void b1nix_reboot(unsigned cmd) {
    syscall(LINUX_SYS_REBOOT, (long)LINUX_REBOOT_MAGIC1,
            (long)LINUX_REBOOT_MAGIC2, (long)cmd, 0L, 0L, 0L);
}

/* Everything in /bin that is NOT a smoke/test binary: shells, daemons, login
 * utilities, GUI apps, and the package/toolchain helpers. Run explicitly
 * elsewhere (or not at all in test mode) rather than via the discovery loop. */
static const char *const excluded[] = {
    "zsh", "csh", "sh", "busybox",
    "init", "telinit", "getty",
    "displayd", "netd",
    "dropbear", "dropbearconvert", "dropbearkey", "dbclient", "ssh",
    "gclock", "gterm", "gpaint", "gdesktop", "gabout",
    "su", "passwd", "useradd", "userdel", "groupadd", "groups",
    "halt", "reboot", "poweroff", "shutdown", "setfattr",
    "bpkg", "curl",
    "mc", "ne", "b1fetch", "gpuinfo", "meminfo",
    "js", "tcc",
    "m40-linux-hello", "m40-linux-abi", "m67-rust", /* driven by their *-smoke.sh wrappers below */
    "m53_httpd", "m53_httpsd", /* server daemons — block on accept(), must not run via discovery */
    "netsurf-fb", /* browser: needs -f/-T/URL arguments, driven by the M53 smoke */
    NULL,
};

static void init_log(const char *msg);

static int is_excluded(const char *name) {
  for (int i = 0; excluded[i]; i++)
    if (strcmp(name, excluded[i]) == 0)
      return 1;
  /* b1cc's own "better C" test corpus is a separate, currently-disabled
   * compiler suite, not a b1nix smoke test — skip anything under that name. */
  if (strncmp(name, "b1cc", 4) == 0)
    return 1;
  return 0;
}

/* ── M39: inittab parsing + runlevel matching (ported from the retired
 * kernel-side init during the ring3 migration). The parser backs both the
 * M39 self-test below and the /run/initctl runlevel-request consumption. ── */
#define INITTAB_MAX 16
struct inittab_entry {
  char id[8];
  char runlevels[12]; /* empty = valid in all runlevels */
  char action[16];    /* initdefault|sysinit|wait|respawn|once */
  char process[96];
};
static struct inittab_entry g_inittab[INITTAB_MAX];
static int g_inittab_count;
static int g_initdefault = 3;

static int init_parse_inittab(const char *buf, int len) {
  g_inittab_count = 0;
  int pos = 0;
  while (pos < len && g_inittab_count < INITTAB_MAX) {
    /* One line: id:runlevels:action:process\n — skip comments and blanks. */
    int eol = pos;
    while (eol < len && buf[eol] != '\n')
      eol++;
    int llen = eol - pos;
    const char *line = buf + pos;
    pos = eol + 1;
    if (llen == 0 || line[0] == '#')
      continue;
    struct inittab_entry *e = &g_inittab[g_inittab_count];
    memset(e, 0, sizeof(*e));
    const char *fields[4] = {0, 0, 0, 0};
    int flens[4] = {0, 0, 0, 0};
    int f = 0, fstart = 0;
    for (int i = 0; i <= llen && f < 4; i++) {
      if (i == llen || line[i] == ':') {
        fields[f] = line + fstart;
        flens[f] = i - fstart;
        f++;
        fstart = i + 1;
      }
    }
    if (f < 3)
      continue;
    /* Field 4 (process) runs to end of line, colons allowed inside. */
    if (f == 4 && fields[3]) {
      int rest = llen - (int)(fields[3] - line);
      flens[3] = rest;
    }
#define COPY_FIELD(dst, idx)                                        \
    do {                                                            \
      int c = flens[idx] < (int)sizeof(e->dst) - 1                  \
                  ? flens[idx] : (int)sizeof(e->dst) - 1;           \
      if (fields[idx]) memcpy(e->dst, fields[idx], c);              \
      e->dst[c] = '\0';                                             \
    } while (0)
    COPY_FIELD(id, 0);
    COPY_FIELD(runlevels, 1);
    COPY_FIELD(action, 2);
    COPY_FIELD(process, 3);
#undef COPY_FIELD
    if (strcmp(e->action, "initdefault") == 0 && e->runlevels[0])
      g_initdefault = e->runlevels[0] - '0';
    g_inittab_count++;
  }
  return g_inittab_count;
}

static int init_entry_in_runlevel(const struct inittab_entry *e, int runlevel) {
  if (!e->runlevels[0])
    return 1; /* empty runlevels field = valid everywhere */
  for (const char *p = e->runlevels; *p; p++)
    if (*p == '0' + runlevel)
      return 1;
  return 0;
}

/* Read and consume a runlevel request left in /run/initctl by telinit. */
static int init_poll_initctl(void) {
  int fd = open("/run/initctl", O_RDONLY);
  if (fd < 0)
    return -1;
  char c = 0;
  ssize_t n = read(fd, &c, 1);
  close(fd);
  if (n != 1)
    return -1;
  unlink("/run/initctl");
  if (c >= '0' && c <= '6')
    return c - '0';
  if (c == 'S')
    return 1;
  return -1;
}

static int cmdline_has_flag(const char *flag) {
  int fd = open("/proc/cmdline", O_RDONLY);
  if (fd < 0)
    return 0;
  char buf[512];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n < 0)
    return 0;
  buf[n] = '\0';
  return strstr(buf, flag) != NULL;
}

/* Per-instance test-category split: the Makefile's iso-core/iso-graphics/
 * iso-shell targets already stamp b1nix.smoke=core|graphics|shell into the
 * kernel cmdline (tests/smoke.sh boots one QEMU instance per ISO, in
 * parallel, to spread wall-clock time) — but this init.c rewrite (the old
 * kernel-side B1NXEXEC dispatcher never had this problem) never read that
 * flag, so every instance ran the FULL ~850-test discovery loop redundantly,
 * and any single slow/hung test (e.g. an off-link DNS/TCP probe) stalled
 * every instance identically instead of just the one that needed it. This
 * restores that split on the new dispatcher: b1nix.smoke=X restricts
 * run_discovered_tests()/scripts[] to binaries relevant to that instance;
 * absent (or "quick"/anything unrecognized) runs everything, same as before.
 * Since smoke.sh concatenates all instance logs before grepping markers, the
 * exact split only needs to (a) cover every binary exactly once across the
 * known suites and (b) roughly balance runtime — it does not need to match
 * how smoke.sh's own check_output calls are commented/grouped. */
enum test_suite { SUITE_ALL, SUITE_CORE, SUITE_GRAPHICS, SUITE_SHELL };

static enum test_suite current_suite(void) {
  int fd = open("/proc/cmdline", O_RDONLY);
  if (fd < 0)
    return SUITE_ALL;
  char buf[512];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n < 0)
    return SUITE_ALL;
  buf[n] = '\0';
  const char *p = strstr(buf, "b1nix.smoke=");
  if (!p)
    return SUITE_ALL;
  p += strlen("b1nix.smoke=");
  if (strncmp(p, "core", 4) == 0) return SUITE_CORE;
  if (strncmp(p, "graphics", 8) == 0) return SUITE_GRAPHICS;
  if (strncmp(p, "shell", 5) == 0) return SUITE_SHELL;
  return SUITE_ALL;
}

/* GFX/Wayland/GL/browser/C++ stack — the heaviest, slowest tests (Mesa,
 * NetSurf, libc++). Milestone numbers only; matched by "mNN" prefix. */
static const int graphics_milestones[] = {
    47, 48, 49, 50, 51, 52, 53, 55, 56, 57, 58, 59, 64, 91, -1,
};
/* Shell/coreutils/net/security — everything driven by the *-smoke.sh
 * wrapper scripts plus the userspace ELFs that exercise the same area. */
static const int shell_milestones[] = {
    29, 31, 32, 39, 42, 46, 63, 71, 73, 92, -1,
};
static const char *const shell_extra_binaries[] = {
    "su", "passwd", "useradd", "userdel", "groupadd", "groups", "telinit",
    "m32_nettool", "m92_abi_smoke", "musl_posix_smoke", "lock_smoke", NULL,
};
static const char *const graphics_extra_binaries[] = {
    "cxx_smoke", "m69_plugin",
    "m51_smoke", "m51_cairo_smoke", "m51_cairo_wayland", "m51_clipboard_smoke",
    "m51_fontconfig_smoke", "m51_freetype_smoke", "m51_harfbuzz_smoke",
    "m51_pixman_smoke", "m51_xkb_smoke", NULL,
};

/* Parses the leading "mNN" (or "mNNb"/"mNN_"...) milestone number out of a
 * test binary's name; returns -1 if the name doesn't start with that shape
 * (non-milestone utilities, which default to the core suite). */
static int milestone_of(const char *name) {
  if (name[0] != 'm' || name[1] < '0' || name[1] > '9')
    return -1;
  int v = 0;
  int i = 1;
  for (; name[i] >= '0' && name[i] <= '9'; i++)
    v = v * 10 + (name[i] - '0');
  return i > 1 ? v : -1;
}

static int in_list(int v, const int *list) {
  for (int i = 0; list[i] != -1; i++)
    if (list[i] == v)
      return 1;
  return 0;
}

static int name_in(const char *name, const char *const *list) {
  for (int i = 0; list[i]; i++)
    if (strcmp(name, list[i]) == 0)
      return 1;
  return 0;
}

static enum test_suite classify_binary(const char *name) {
  if (name_in(name, graphics_extra_binaries)) return SUITE_GRAPHICS;
  if (name_in(name, shell_extra_binaries)) return SUITE_SHELL;
  int m = milestone_of(name);
  if (m < 0) return SUITE_CORE;
  if (in_list(m, graphics_milestones)) return SUITE_GRAPHICS;
  if (in_list(m, shell_milestones)) return SUITE_SHELL;
  return SUITE_CORE;
}

/* Wait for `pid` while (a) reaping any orphans reparented to us mid-run (a
 * suite of hundreds of forking tests would otherwise pile up zombies against
 * the kernel task table) and (b) enforcing a per-child watchdog: one wedged
 * test binary must cost its own slot, not stall the whole instance until the
 * host-side 120s serial-silence killer shoots the entire QEMU run. The old
 * kernel-side dispatcher had this watchdog; the init.c rewrite lost it. */
/* Three timers, deliberately ordered against the HOST-side watchdogs in
 * tests/smoke.sh (STALL_TIMEOUT: 120 s of serial silence kills the whole QEMU
 * instance):
 *
 *   HEARTBEAT  — print a line while waiting, so a slow-but-alive child can
 *                never look like serial silence to the host. Without this the
 *                host shot the entire instance (and every later marker in it
 *                read as a failure) before the guest watchdog below ever fired.
 *   WATCHDOG   — SIGKILL a child that produced nothing for this long.
 *   ABANDON    — a task wedged in an uninterruptible kernel path never reaps;
 *                give up on it and run the remaining tests anyway. One wedged
 *                test must cost one test, not the rest of the suite.
 */
#define CHILD_HEARTBEAT_SECS 20
#define CHILD_WATCHDOG_SECS 90
#define CHILD_ABANDON_SECS 20
static void wait_with_watchdog_named(pid_t pid, const char *name) {
  int status = 0;
  int elapsed_ds = 0; /* deciseconds */
  int next_beat_ds = CHILD_HEARTBEAT_SECS * 10;
  int killed_ds = 0;
  int killed = 0;
  for (;;) {
    pid_t r = waitpid(-1, &status, WNOHANG);
    if (r == pid)
      break;
    if (r > 0)
      continue; /* reaped an orphan; check again immediately */
    if (r < 0)
      break; /* no children left (shouldn't happen while pid lives) */
    elapsed_ds++;
    if (elapsed_ds >= next_beat_ds) {
      {
        char b[352];
        snprintf(b, sizeof(b), "init: waiting on %s (pid %d) for %ds\n", name,
                 (int)pid, elapsed_ds / 10);
        init_log(b);
      }
      next_beat_ds += CHILD_HEARTBEAT_SECS * 10;
    }
    if (!killed && elapsed_ds >= CHILD_WATCHDOG_SECS * 10) {
      {
        char b[352];
        snprintf(b, sizeof(b),
                 "INIT-WATCHDOG: killing wedged child %s (pid %d) after %ds\n",
                 name, (int)pid, CHILD_WATCHDOG_SECS);
        init_log(b);
      }
      kill(pid, SIGKILL);
      killed = 1;
      killed_ds = elapsed_ds;
    }
    if (killed && elapsed_ds - killed_ds >= CHILD_ABANDON_SECS * 10) {
      {
        char b[352];
        snprintf(b, sizeof(b),
                 "INIT-WATCHDOG: abandoning unreapable child %s (pid %d); "
                 "continuing with the remaining tests\n", name, (int)pid);
        init_log(b);
      }
      break;
    }
    usleep(100000);
  }
}

/* PID 1's own log line, written straight to fd 1 — and repaired if fd 1 has
 * stopped working. A test that leaves the console in a bad state (a pty/tty
 * session test hanging up a shared handle, a stray close) used to silence
 * every later marker: the suite kept running but the log ended mid-run and the
 * host reported dozens of "missing" markers with no clue why. Re-opening
 * /dev/console on write failure keeps the rest of the run observable. */
static void init_log(const char *msg) {
  size_t len = strlen(msg);
  if (write(1, msg, len) == (ssize_t)len)
    return;
  int fd = open("/dev/console", O_WRONLY);
  if (fd < 0)
    fd = open("/dev/ttyS0", O_WRONLY);
  if (fd < 0)
    return;
  if (fd != 1) {
    dup2(fd, 1);
    close(fd);
  }
  write(1, "init: repaired lost stdout\n", 27);
  write(1, msg, len);
}

static void run_and_wait(const char *path) {
  char line[352];
  snprintf(line, sizeof(line), "init: run %s\n", path);
  init_log(line);
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = {(char *)path, NULL};
    char *envp[] = {(char *)"PATH=/bin", NULL};
    execve(path, argv, envp);
    _exit(127);
  } else if (pid > 0) {
    wait_with_watchdog_named(pid, path);
  }
}

/* Scripts (the ELF loader itself has no shebang support, so run_script()
 * below parses #! and dispatches to the right shell). /etc/rc mirrors the
 * old inittab "si::sysinit:/etc/rc" entry: sets
 * up dirs and starts sshd. The *-smoke.sh scripts are the REAL busybox/zsh
 * coverage (BB-SMOKE/POSIX-SMOKE/ZSH-SMOKE) — busybox itself is excluded
 * from the discovery loop below because running the bare applet dispatcher
 * with no args does nothing; these scripts exercise its applets properly. */
static const char *const scripts[] = {
    "/etc/rc",
    "/etc/posix-smoke.sh",
    "/etc/initctl-smoke.sh",
    "/etc/zsh-smoke.sh",
    "/etc/bpkg-smoke.sh",
    "/etc/m40-smoke.sh",
    "/etc/m67-smoke.sh",
    NULL,
};

/* Loader doesn't parse #!, so read the shebang ourselves and pick the
 * interpreter: zsh-smoke.sh needs real zsh arrays/[[ ]]/(( )) that
 * /bin/sh (busybox ash) doesn't support. Defaults to /bin/sh when no
 * shebang names a specific interpreter. */
static const char *shebang_interp(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return "/bin/sh";
  char buf[64];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return "/bin/sh";
  buf[n] = '\0';
  if (strncmp(buf, "#!", 2) != 0)
    return "/bin/sh";
  if (strstr(buf, "/bin/zsh") != NULL)
    return "/bin/zsh";
  return "/bin/sh";
}

static void wait_with_watchdog_named(pid_t pid, const char *name);

static void run_script(const char *path) {
  const char *interp = shebang_interp(path);
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = {(char *)interp, (char *)path, NULL};
    char *envp[] = {(char *)"PATH=/bin", NULL};
    execve(interp, argv, envp);
    _exit(127);
  } else if (pid > 0) {
    wait_with_watchdog_named(pid, path);
  }
}

/* displayd is a long-running Wayland-shaped compositor: the M47/M49/M51/M52/
 * M53 GFX/Wayland/GL smoke binaries are clients that connect to it, so it
 * must already be running (backgrounded, not waited on) before they spawn.
 * Mirrors the old inittab "display:5:respawn:/bin/displayd" entry, minus the
 * runlevel gate and respawn supervision (test mode only needs one instance
 * alive for the duration of the client tests below). */
static pid_t g_displayd_pid = -1;

static void spawn_displayd(void) {
  enum test_suite suite = current_suite();
  if (suite != SUITE_ALL && suite != SUITE_GRAPHICS)
    return;
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = {(char *)"/bin/displayd", NULL};
    char *envp[] = {(char *)"PATH=/bin", NULL};
    execve("/bin/displayd", argv, envp);
    _exit(127);
  }
  /* Parent: don't wait — it's a daemon. Give it a moment to bind its socket
   * before the first client test tries to connect. */
  if (pid > 0) {
    g_displayd_pid = pid;
    usleep(500000);
  }
}

/* M47-DSP: display-server lifecycle. Terminating the compositor must hand
 * scanout back to the kernel console (every /dev/fb0 mapping gone), and a
 * fresh server must be able to claim the reclaimed framebuffer and serve
 * clients again. Lives in PID 1 because PID 1 owns displayd's lifetime; it
 * used to live in the kernel's built-in program dispatcher. */
static int fb_claim_count(void) {
  int fd = open("/dev/fb0", O_RDWR);
  if (fd < 0)
    return -1;
  int claimed = -1;
  if (ioctl(fd, B1NIX_FBIOGET_CLAIM, &claimed) < 0)
    claimed = -1;
  close(fd);
  return claimed;
}

static int displayd_socket_reachable(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return 0;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, "/run/wayland-0", sizeof(addr.sun_path) - 1);
  int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  close(fd);
  return rc == 0;
}

static void displayd_lifecycle_test(void) {
  if (g_displayd_pid <= 0)
    return;

  kill(g_displayd_pid, SIGTERM);
  int status = 0;
  waitpid(g_displayd_pid, &status, 0);
  g_displayd_pid = -1;

  /* The claim drops when the exiting process's VMAs are torn down, which the
   * kernel may finish just after wait() returns. */
  int claimed = fb_claim_count();
  for (int i = 0; i < 50 && claimed != 0; i++) {
    usleep(20000);
    claimed = fb_claim_count();
  }
  init_log(claimed == 0 ? "M47-DSP: ok console-reclaim\n"
                        : "M47-DSP: fail console-reclaim\n");

  spawn_displayd();
  if (g_displayd_pid <= 0) {
    init_log("M47-DSP: fail server-restart\n");
    return;
  }
  int reachable = 0;
  for (int i = 0; i < 50 && !reachable; i++) {
    reachable = displayd_socket_reachable();
    if (!reachable)
      usleep(100000);
  }
  init_log(reachable ? "M47-DSP: ok server-restart\n"
                     : "M47-DSP: fail server-restart\n");
}

/* Run every test binary in /bin, alphabetically for determinism, skipping the
 * fixed exclude list above and every symlink. The applet-manifest generates a
 * /bin/<name> -> /opt/busybox/bin/busybox symlink for every coreutil (cat,
 * mkdir, id, whoami, ls, ...) plus /bin/sh — those need real arguments to do
 * anything, are exercised properly by the *-smoke.sh scripts, and several
 * (cat, sh) block reading stdin forever if just exec'd bare. Real b1nix test
 * ELFs are never symlinks, so this is an exact, low-maintenance filter.
 * lstat() rather than d_type: the initramfs readdir doesn't fill d_type for
 * symlink entries (always reports them as regular files). */
static void run_discovered_tests(void) {
  struct dirent **entries;
  int n = scandir("/bin", &entries, 0, alphasort);
  if (n < 0)
    return;

  enum test_suite suite = current_suite();
  char path[320];
  for (int i = 0; i < n; i++) {
    const char *name = entries[i]->d_name;
    if (name[0] == '.' || is_excluded(name))
      continue;
    if (suite != SUITE_ALL && classify_binary(name) != suite)
      continue;
    snprintf(path, sizeof(path), "/bin/%s", name);
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode))
      continue;
    run_and_wait(path);
  }
}

/* M39 self-test (test mode): inittab parser, runlevel matching, the
 * telinit → /run/initctl round-trip, and getty applet presence. The serial
 * tty half lives in /bin/m39_smoke (needs TIOCSTI injection, not PID-1 work). */
static void m39_init_test(void) {
  puts("M39-INIT: start");

  static const char test_tab[] =
      "# comment line\n"
      "id:4:initdefault:\n"
      "si::sysinit:/etc/rc\n"
      "co:2345:respawn:/bin/zsh\n"
      "tt:23:respawn:/bin/getty ttyS0\n"
      "lo:5:wait:/bin/true\n";
  int count = init_parse_inittab(test_tab, (int)sizeof(test_tab) - 1);
  puts(count == 5 ? "M39-INIT: ok parse-inittab" : "M39-INIT: fail parse-inittab");
  puts(g_initdefault == 4 ? "M39-INIT: ok initdefault" : "M39-INIT: fail initdefault");

  /* g_inittab[3] is the "tt:23:..." getty entry: valid in 2/3, not 4/5. */
  if (init_entry_in_runlevel(&g_inittab[3], 2) &&
      init_entry_in_runlevel(&g_inittab[3], 3) &&
      !init_entry_in_runlevel(&g_inittab[3], 4) &&
      init_entry_in_runlevel(&g_inittab[1], 4) /* empty runlevels = all */)
    puts("M39-INIT: ok runlevel-match");
  else
    puts("M39-INIT: fail runlevel-match");

  /* telinit round-trip: /bin/telinit 4 leaves runlevel 4 in /run/initctl,
   * which init_poll_initctl() reads back and consumes. Runlevel 4 (not 5)
   * so a racing real runlevel switch can't start the desktop mid-smoke. */
  {
    mkdir("/run", 0755);
    int ok = 0;
    pid_t pid = fork();
    if (pid == 0) {
      char *av[] = {(char *)"/bin/telinit", (char *)"4", NULL};
      char *ev[] = {(char *)"PATH=/bin", NULL};
      execve("/bin/telinit", av, ev);
      _exit(127);
    }
    if (pid > 0) {
      int st = 0;
      waitpid(pid, &st, 0);
      if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && init_poll_initctl() == 4)
        ok = 1;
    }
    puts(ok ? "M39-INIT: ok telinit" : "M39-INIT: fail telinit");
  }

  /* getty applet present (BusyBox), reachable via the /bin/getty symlink. */
  {
    int fd = open("/bin/getty", O_RDONLY);
    if (fd >= 0) {
      close(fd);
      puts("M39-INIT: ok getty-applet");
    } else {
      puts("M39-INIT: fail getty-applet");
    }
  }
}

int main(void) {
  /* PID 1's stdout goes to the boot console; markers must hit the serial log
   * the moment they're emitted (a buffered marker lost in a crash reads as a
   * missing test). */
  setvbuf(stdout, NULL, _IONBF, 0);

  /* Leave the boot/kernel process group (pgrp 1, inherited from the kernel
   * spawn) for a session of our own. Otherwise every pty in the system has
   * PID 1 in its foreground group's blast radius: an SSH session closing its
   * master sends SIGHUP to that group, whose default action terminated init
   * silently — the suite then stopped mid-run with no message and every later
   * marker read as missing. Same fix the kernel-spawned daemons already use. */
  setsid();

  run_script("/etc/rc");
  spawn_displayd();

  if (cmdline_has_flag("b1nix.test=1")) {
    enum test_suite suite = current_suite();
    if (suite == SUITE_ALL || suite == SUITE_SHELL)
      m39_init_test();
  }

  run_discovered_tests();

  /* Display-server teardown/restart — after every graphics client test has
   * run against the original compositor. */
  if (cmdline_has_flag("b1nix.test=1"))
    displayd_lifecycle_test();

  enum test_suite suite = current_suite();
  for (int i = 1; scripts[i]; i++) {
    if (suite != SUITE_ALL) {
      if (suite != SUITE_CORE) continue; /* run all test scripts in CORE instance */
    }
    run_script(scripts[i]);
  }

  if (cmdline_has_flag("b1nix.test=1")) {
    init_log("B1NIX-TEST: done\n");
    /* Test mode ends with a real machine restart (tests/smoke.sh checks for
     * the kernel's "reboot: restarting" line); QEMU runs with -no-reboot, so
     * the reset ends the instance instead of looping the suite. */
    b1nix_reboot(LINUX_REBOOT_CMD_RESTART);
    _exit(0);
  }

  /* Interactive shell — only in real (non-test) boot. In test mode this
   * would block forever reading stdin for a command that never comes. */
  run_and_wait("/bin/sh");

  /* Real init: reap orphans forever. -1 = any child (POSIX pid 0 means "my
   * process group", which would miss orphans reparented here from elsewhere). */
  for (;;) {
    int status = 0;
    waitpid(-1, &status, 0);
  }
}
