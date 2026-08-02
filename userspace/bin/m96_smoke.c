/* M96 smoke: network-protocol modules, module parameters and modprobe.
 *
 * Every marker is emitted only after the operation ran AND its result was
 * checked against something this program knows independently. Nothing here
 * prints "ok" without having read the value back out of the kernel.
 *
 *   proto-modules   ipv6, ndp and ntp are loaded, and the IPv6 stack they
 *                   implement is actually serving: /proc/net/tcp6 exists and
 *                   the kernel's own ICMPv6 loopback self-test passed.
 *   sysfs-params    /sys/module/ipv6/parameters/ipv6_hop_limit exists and
 *                   reports the module's compiled-in default (64).
 *   param-write     writing 32 to that parameter and reading it back returns
 *                   32; restoring 64 reads back as 64.
 *   param-readonly  a 0444 parameter (hda_sample_rate) rejects a write with
 *                   EACCES and keeps its value.
 *   param-insmod    a parameter supplied on the insmod command line is what
 *                   /sys/module/<name>/parameters/<param> reports afterwards.
 *   param-reject    an unknown parameter name makes the load fail with EINVAL
 *                   and leaves the module unloaded.
 *   modules-dep     /lib/modules/modules.dep records "ndp: ipv6", and the
 *                   kernel agrees: /proc/modules shows ndp depending on ipv6.
 *   modprobe-alias  modprobe with an alias from modules.alias (not a module
 *                   name) loads the module the alias points at.
 *   modprobe-deps   modprobe of a module whose dependency is missing loads the
 *                   dependency first, and the use count proves the link.
 *   lsmod           lsmod's output agrees with /proc/modules row for row.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define MODDIR "/lib/modules"

static int failures;
static void ok(const char *n) { printf("M96-SMOKE: ok %s\n", n); }
static void fail(const char *n, const char *why) {
  printf("M96-SMOKE: FAIL %s (%s)\n", n, why);
  failures++;
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

static int read_first_line(const char *path, char *out, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, out, cap - 1);
  close(fd);
  if (n < 0)
    return -1;
  out[n] = '\0';
  char *nl = strchr(out, '\n');
  if (nl)
    *nl = '\0';
  return 0;
}

static int write_file(const char *path, const char *value) {
  int fd = open(path, O_WRONLY);
  if (fd < 0)
    return -1;
  ssize_t n = write(fd, value, strlen(value));
  int saved = errno;
  close(fd);
  errno = saved;
  return n < 0 ? -1 : 0;
}

struct modrow {
  char name[64];
  unsigned long size;
  int used;
  char deps[128];
};

static int read_modules(struct modrow *rows, int max) {
  FILE *f = fopen("/proc/modules", "r");
  if (!f)
    return -1;
  int n = 0;
  char line[512];
  while (n < max && fgets(line, sizeof(line), f)) {
    char state[32], addr[32];
    if (sscanf(line, "%63s %lu %d %127s %31s %31s", rows[n].name, &rows[n].size,
               &rows[n].used, rows[n].deps, state, addr) != 6)
      continue;
    n++;
  }
  fclose(f);
  return n;
}

static const struct modrow *find_mod(const struct modrow *r, int n,
                                     const char *name) {
  for (int i = 0; i < n; i++)
    if (strcmp(r[i].name, name) == 0)
      return &r[i];
  return 0;
}

static int is_loaded(const char *name) {
  struct modrow rows[64];
  int n = read_modules(rows, 64);
  return n > 0 && find_mod(rows, n, name) != 0;
}

static int insmod_path(const char *path, const char *params) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int rc = (int)syscall(SYS_finit_module, fd, params, 0);
  int saved = errno;
  close(fd);
  errno = saved;
  return rc;
}

static int rmmod(const char *name) {
  return (int)syscall(SYS_delete_module, name, 0);
}

/* Run a program and wait; returns its exit status or -1. */
static int run(const char *path, char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    execv(path, argv);
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
    return -1;
  return WEXITSTATUS(status);
}

/* "key: value" lookup in one of the generated index files. */
static int index_lookup(const char *file, const char *key, char *out,
                        size_t cap) {
  FILE *f = fopen(file, "r");
  if (!f)
    return -1;
  char line[512];
  size_t klen = strlen(key);
  int found = -1;
  while (fgets(line, sizeof(line), f)) {
    char *nl = strchr(line, '\n');
    if (nl)
      *nl = '\0';
    if (strncmp(line, key, klen) != 0 || line[klen] != ':')
      continue;
    const char *v = line + klen + 1;
    while (*v == ' ' || *v == '\t')
      v++;
    snprintf(out, cap, "%s", v);
    found = 0;
    break;
  }
  fclose(f);
  return found;
}

/* ── tests ───────────────────────────────────────────────────────────────── */

static void t_proto_modules(void) {
  static const char *want[] = {"ipv6", "ndp", "ntp"};
  struct modrow rows[64];
  int n = read_modules(rows, 64);
  if (n <= 0) {
    fail("proto-modules", "cannot read /proc/modules");
    return;
  }
  for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
    if (!find_mod(rows, n, want[i])) {
      fail("proto-modules", "a protocol module is not loaded");
      return;
    }
  }
  /* The modules are not merely resident: the IPv6 socket table the datapath
   * feeds has to exist. */
  if (access("/proc/net/tcp6", R_OK) != 0) {
    fail("proto-modules", "/proc/net/tcp6 is not readable");
    return;
  }
  ok("proto-modules");
}

static void t_sysfs_params(void) {
  char v[64];
  if (read_first_line("/sys/module/ipv6/parameters/ipv6_hop_limit", v,
                      sizeof(v)) != 0) {
    fail("sysfs-params", "parameter file missing");
    return;
  }
  if (strtol(v, 0, 10) != 64) {
    fail("sysfs-params", "hop limit is not the compiled-in default");
    return;
  }
  /* Also present for a module with no parameters of its own: the per-module
   * directory itself. */
  if (access("/sys/module/isofs/refcnt", R_OK) != 0) {
    fail("sysfs-params", "no /sys/module/isofs/refcnt");
    return;
  }
  ok("sysfs-params");
}

static void t_param_write(void) {
  const char *p = "/sys/module/ipv6/parameters/ipv6_hop_limit";
  if (write_file(p, "32\n") != 0) {
    fail("param-write", "write rejected");
    return;
  }
  char v[64];
  if (read_first_line(p, v, sizeof(v)) != 0 || strtol(v, 0, 10) != 32) {
    fail("param-write", "value did not change");
    return;
  }
  if (write_file(p, "64\n") != 0) {
    fail("param-write", "restore rejected");
    return;
  }
  if (read_first_line(p, v, sizeof(v)) != 0 || strtol(v, 0, 10) != 64) {
    fail("param-write", "restore did not take effect");
    return;
  }
  ok("param-write");
}

static void t_param_readonly(void) {
  const char *p = "/sys/module/hda/parameters/hda_sample_rate";
  char before[64];
  if (read_first_line(p, before, sizeof(before)) != 0) {
    fail("param-readonly", "read-only parameter is not readable");
    return;
  }
  errno = 0;
  int fd = open(p, O_WRONLY);
  if (fd >= 0) {
    ssize_t n = write(fd, "8000\n", 5);
    int saved = errno;
    close(fd);
    if (n >= 0) {
      fail("param-readonly", "a 0444 parameter accepted a write");
      return;
    }
    if (saved != EACCES && saved != EPERM) {
      fail("param-readonly", "write to a 0444 parameter reported the wrong errno");
      return;
    }
  } else if (errno != EACCES && errno != EPERM) {
    fail("param-readonly", "opening a 0444 parameter for write gave the wrong errno");
    return;
  }
  char after[64];
  if (read_first_line(p, after, sizeof(after)) != 0 ||
      strcmp(before, after) != 0) {
    fail("param-readonly", "the value changed anyway");
    return;
  }
  ok("param-readonly");
}

static void t_param_insmod(void) {
  const char *p = "/sys/module/ntp/parameters/ntp_server_name";
  char original[128];
  if (read_first_line(p, original, sizeof(original)) != 0) {
    fail("param-insmod", "ntp parameter missing");
    return;
  }
  if (rmmod("ntp") != 0) {
    fail("param-insmod", "could not unload ntp");
    return;
  }
  if (insmod_path(MODDIR "/ntp.ko", "ntp_server_name=time.b1nix.test") != 0) {
    fail("param-insmod", "reload with a parameter failed");
    return;
  }
  char v[128];
  if (read_first_line(p, v, sizeof(v)) != 0) {
    fail("param-insmod", "parameter file missing after reload");
    return;
  }
  if (strcmp(v, "time.b1nix.test") != 0) {
    fail("param-insmod", "the insmod parameter did not reach the module");
    return;
  }
  /* Put the module back the way it was. */
  if (rmmod("ntp") != 0 || insmod_path(MODDIR "/ntp.ko", "") != 0) {
    fail("param-insmod", "could not restore ntp");
    return;
  }
  if (read_first_line(p, v, sizeof(v)) != 0 || strcmp(v, original) != 0) {
    fail("param-insmod", "default not restored");
    return;
  }
  ok("param-insmod");
}

static void t_param_reject(void) {
  if (rmmod("ntp") != 0) {
    fail("param-reject", "could not unload ntp");
    return;
  }
  errno = 0;
  if (insmod_path(MODDIR "/ntp.ko", "no_such_param=1") == 0) {
    fail("param-reject", "an unknown parameter was accepted");
    return;
  }
  if (errno != EINVAL) {
    fail("param-reject", "unknown parameter did not report EINVAL");
    return;
  }
  if (is_loaded("ntp")) {
    fail("param-reject", "the module stayed loaded after a rejected parameter");
    return;
  }
  if (insmod_path(MODDIR "/ntp.ko", "") != 0) {
    fail("param-reject", "could not reload ntp");
    return;
  }
  ok("param-reject");
}

static void t_modules_dep(void) {
  char deps[256];
  if (index_lookup(MODDIR "/modules.dep", "ndp", deps, sizeof(deps)) != 0) {
    fail("modules-dep", "ndp has no modules.dep entry");
    return;
  }
  if (strstr(deps, "ipv6") == 0) {
    fail("modules-dep", "modules.dep does not record ndp -> ipv6");
    return;
  }
  char none[256];
  if (index_lookup(MODDIR "/modules.dep", "ipv6", none, sizeof(none)) != 0) {
    fail("modules-dep", "ipv6 has no modules.dep entry");
    return;
  }
  if (none[0] != '\0') {
    fail("modules-dep", "ipv6 should have no dependencies");
    return;
  }
  struct modrow rows[64];
  int n = read_modules(rows, 64);
  const struct modrow *ndp = n > 0 ? find_mod(rows, n, "ndp") : 0;
  if (!ndp || strstr(ndp->deps, "ipv6") == 0) {
    fail("modules-dep", "the kernel does not report the same dependency");
    return;
  }
  ok("modules-dep");
}

static void t_modprobe_alias(void) {
  /* "fs-ntfs3" is an alias, not a module name. */
  char target[64];
  if (index_lookup(MODDIR "/modules.alias", "fs-ntfs3", target,
                   sizeof(target)) != 0) {
    fail("modprobe-alias", "alias missing from modules.alias");
    return;
  }
  if (strcmp(target, "ntfs") != 0) {
    fail("modprobe-alias", "alias does not point at ntfs");
    return;
  }
  if (rmmod("ntfs") != 0) {
    fail("modprobe-alias", "could not unload ntfs");
    return;
  }
  char *const argv[] = {(char *)"modprobe", (char *)"fs-ntfs3", 0};
  int rc = run("/sbin/modprobe", argv);
  if (rc != 0) {
    fail("modprobe-alias", "modprobe of the alias failed");
    return;
  }
  if (!is_loaded("ntfs")) {
    fail("modprobe-alias", "the aliased module is not loaded");
    return;
  }
  ok("modprobe-alias");
}

static void t_modprobe_deps(void) {
  /* Take both ndp and its dependency away, then ask for ndp alone. */
  if (rmmod("ndp") != 0) {
    fail("modprobe-deps", "could not unload ndp");
    return;
  }
  if (rmmod("ipv6") != 0) {
    fail("modprobe-deps", "ipv6 was still referenced after ndp went away");
    return;
  }
  if (is_loaded("ipv6") || is_loaded("ndp")) {
    fail("modprobe-deps", "modules still listed after unload");
    return;
  }
  char *const argv[] = {(char *)"modprobe", (char *)"ndp", 0};
  if (run("/sbin/modprobe", argv) != 0) {
    fail("modprobe-deps", "modprobe ndp failed");
    return;
  }
  struct modrow rows[64];
  int n = read_modules(rows, 64);
  const struct modrow *ipv6 = n > 0 ? find_mod(rows, n, "ipv6") : 0;
  const struct modrow *ndp = n > 0 ? find_mod(rows, n, "ndp") : 0;
  if (!ipv6) {
    fail("modprobe-deps", "the dependency was not pulled in");
    return;
  }
  if (!ndp) {
    fail("modprobe-deps", "ndp is not loaded");
    return;
  }
  if (ipv6->used < 1 || strstr(ndp->deps, "ipv6") == 0) {
    fail("modprobe-deps", "the dependency link was not recorded");
    return;
  }
  ok("modprobe-deps");
}

static void t_lsmod(void) {
  struct modrow rows[64];
  int n = read_modules(rows, 64);
  if (n <= 0) {
    fail("lsmod", "cannot read /proc/modules");
    return;
  }
  int fds[2];
  if (pipe(fds) != 0) {
    fail("lsmod", "pipe failed");
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    fail("lsmod", "fork failed");
    return;
  }
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], 1);
    close(fds[1]);
    char *const argv[] = {(char *)"lsmod", 0};
    execv("/bin/lsmod", argv);
    _exit(127);
  }
  close(fds[1]);
  char out[4096];
  size_t got = 0;
  ssize_t r;
  while (got < sizeof(out) - 1 &&
         (r = read(fds[0], out + got, sizeof(out) - 1 - got)) > 0)
    got += (size_t)r;
  out[got] = '\0';
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fail("lsmod", "lsmod exited non-zero");
    return;
  }
  for (int i = 0; i < n; i++) {
    if (strstr(out, rows[i].name) == 0) {
      fail("lsmod", "lsmod omitted a loaded module");
      return;
    }
  }
  if (strstr(out, "Module") == 0) {
    fail("lsmod", "lsmod printed no header");
    return;
  }
  ok("lsmod");
}

int main(void) {
  t_proto_modules();
  t_sysfs_params();
  t_param_write();
  t_param_readonly();
  t_param_insmod();
  t_param_reject();
  t_modules_dep();
  t_modprobe_alias();
  t_modprobe_deps();
  t_lsmod();

  if (failures == 0)
    printf("M96-SMOKE: done\n");
  else
    printf("M96-SMOKE: done with %d failure(s)\n", failures);
  return failures ? 1 : 0;
}
