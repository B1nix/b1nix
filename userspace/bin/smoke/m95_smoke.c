/* M95 smoke: the loadable-kernel-module framework and the device/filesystem
 * modules built on it.
 *
 * Every marker is emitted only after the operation ran AND its result was
 * checked against something this program knows independently — a second,
 * unrelated kernel interface, or a value it computed itself. There are no
 * unconditional "ok" prints.
 *
 *   proc-modules     /proc/modules lists exactly the modules present in
 *                    /lib/modules, each Live and mapped inside the module
 *                    region at 0xffffffffc0000000.
 *   modinfo          the .modinfo section of a .ko on disk carries name,
 *                    license and a vermagic whose version prefix equals the
 *                    running kernel's uname release.
 *   fs-modules       every filesystem-module name in /lib/modules appears in
 *                    /proc/filesystems — i.e. the module really registered.
 *   sound-module     /sys/module/hda reports initstate "live" and a coresize
 *                    that matches the size column of /proc/modules.
 *   rmmod-insmod     unloading ntfs removes it from BOTH /proc/modules and
 *                    /proc/filesystems; loading it back restores both.
 *   refcount         ipv6 is held by ndp: /proc/modules shows a non-zero use
 *                    count, /sys/module/ipv6/refcnt agrees, and rmmod ipv6
 *                    fails with EBUSY while ndp is loaded.
 *   dup-load         loading an already-loaded module fails with EEXIST.
 *   vermagic-reject  a copy of a .ko whose vermagic string was corrupted is
 *                    refused, and the intact original still loads.
 *   unpriv           a process that dropped to an unprivileged uid cannot
 *                    delete a module (EPERM) and the module stays loaded.
 *   init-module      the raw init_module(2) entry point loads an image read
 *                    into this process's own memory.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define MODDIR "/lib/modules"
#define MODULE_REGION_BASE 0xffffffffc0000000ULL

static int failures;

static void ok(const char *name) { printf("M95-SMOKE: ok %s\n", name); }
static void fail(const char *name, const char *why) {
  printf("M95-SMOKE: FAIL %s (%s)\n", name, why);
  failures++;
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

struct modrow {
  char name[64];
  unsigned long size;
  int used;
  char deps[128];
  char state[32];
  unsigned long long addr;
};

/* Echo /proc/modules verbatim. A parsed field that looks wrong (a truncated
 * address, a shifted column) is indistinguishable from a kernel that really
 * reported it, and only the raw line tells the two apart. */
static void dump_proc_modules(void) {
  FILE *f = fopen("/proc/modules", "r");
  if (!f)
    return;
  char line[512];
  while (fgets(line, sizeof(line), f))
    printf("M95-SMOKE: /proc/modules| %s", line);
  fclose(f);
}

static int read_modules(struct modrow *rows, int max) {
  FILE *f = fopen("/proc/modules", "r");
  if (!f)
    return -1;
  int n = 0;
  char line[512];
  while (n < max && fgets(line, sizeof(line), f)) {
    struct modrow *r = &rows[n];
    char addr[32];
    if (sscanf(line, "%63s %lu %d %127s %31s %31s", r->name, &r->size, &r->used,
               r->deps, r->state, addr) != 6)
      continue;
    r->addr = strtoull(addr, 0, 16);
    n++;
  }
  fclose(f);
  return n;
}

static const struct modrow *find_mod(const struct modrow *rows, int n,
                                     const char *name) {
  for (int i = 0; i < n; i++)
    if (strcmp(rows[i].name, name) == 0)
      return &rows[i];
  return 0;
}

static int file_contains_word(const char *path, const char *word) {
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  char line[512];
  int hit = 0;
  while (fgets(line, sizeof(line), f)) {
    char *p = line;
    while (*p) {
      while (*p == ' ' || *p == '\t' || *p == '\n')
        p++;
      char *s = p;
      while (*p && *p != ' ' && *p != '\t' && *p != '\n')
        p++;
      if (p > s && (size_t)(p - s) == strlen(word) &&
          strncmp(s, word, (size_t)(p - s)) == 0) {
        hit = 1;
        break;
      }
    }
    if (hit)
      break;
  }
  fclose(f);
  return hit;
}

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

/* Names of the .ko files in /lib/modules, sorted by directory order. */
static int list_modules(char names[][64], int max) {
  DIR *d = opendir(MODDIR);
  if (!d)
    return -1;
  int n = 0;
  struct dirent *e;
  while (n < max && (e = readdir(d))) {
    size_t len = strlen(e->d_name);
    if (len < 4 || strcmp(e->d_name + len - 3, ".ko") != 0)
      continue;
    snprintf(names[n], 64, "%.*s", (int)(len - 3), e->d_name);
    n++;
  }
  closedir(d);
  return n;
}

/* Load a whole file. Caller frees. */
static char *slurp(const char *path, size_t *out_size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return 0;
  }
  char *buf = malloc((size_t)st.st_size);
  if (!buf) {
    close(fd);
    return 0;
  }
  ssize_t got = 0;
  while (got < st.st_size) {
    ssize_t r = read(fd, buf + got, (size_t)(st.st_size - got));
    if (r <= 0)
      break;
    got += r;
  }
  close(fd);
  if (got != st.st_size) {
    free(buf);
    return 0;
  }
  *out_size = (size_t)st.st_size;
  return buf;
}

/* Find the .modinfo blob inside a relocatable object. */
static const char *find_modinfo(const char *buf, size_t size, size_t *out_len) {
  if (size < sizeof(Elf64_Ehdr))
    return 0;
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)buf;
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_type != ET_REL)
    return 0;
  const Elf64_Shdr *sh = (const Elf64_Shdr *)(buf + eh->e_shoff);
  const char *shstr = buf + sh[eh->e_shstrndx].sh_offset;
  for (int i = 0; i < eh->e_shnum; i++) {
    if (strcmp(shstr + sh[i].sh_name, ".modinfo") == 0) {
      *out_len = (size_t)sh[i].sh_size;
      return buf + sh[i].sh_offset;
    }
  }
  return 0;
}

static const char *modinfo_value(const char *blob, size_t len, const char *key) {
  size_t klen = strlen(key);
  size_t pos = 0;
  while (pos < len) {
    const char *rec = blob + pos;
    size_t rlen = strnlen(rec, len - pos);
    if (rlen > klen && rec[klen] == '=' && strncmp(rec, key, klen) == 0)
      return rec + klen + 1;
    pos += rlen + 1;
  }
  return 0;
}

static int insmod_path(const char *path, const char *params) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int rc = (int)syscall(SYS_finit_module, fd, params ? params : "", 0);
  int saved = errno;
  close(fd);
  errno = saved;
  return rc;
}

static int rmmod(const char *name) {
  return (int)syscall(SYS_delete_module, name, 0);
}

/* ── tests ───────────────────────────────────────────────────────────────── */

static void t_proc_modules(void) {
  char names[32][64];
  int nfiles = list_modules(names, 32);
  if (nfiles <= 0) {
    fail("proc-modules", "no .ko files in " MODDIR);
    return;
  }
  struct modrow rows[64];
  int n = read_modules(rows, 64);
  if (n < 0) {
    fail("proc-modules", "cannot read /proc/modules");
    return;
  }
  if (n != nfiles) {
    fail("proc-modules", "row count differs from /lib/modules");
    return;
  }
  for (int i = 0; i < nfiles; i++) {
    const struct modrow *r = find_mod(rows, n, names[i]);
    if (!r) {
      fail("proc-modules", "a module in /lib/modules is not listed");
      return;
    }
    if (strcmp(r->state, "Live") != 0) {
      fail("proc-modules", "module is not Live");
      return;
    }
    if (r->size == 0) {
      fail("proc-modules", "zero core size");
      return;
    }
    if (r->addr < MODULE_REGION_BASE) {
      /* Name the module and its address: "not in the region" alone says
       * nothing about which one, and the answer differs between a module that
       * fell back to the general heap and one whose address was mis-parsed. */
      char why[128];
      snprintf(why, sizeof(why), "%s is at 0x%llx, outside the module region",
               names[i], (unsigned long long)r->addr);
      fail("proc-modules", why);
      dump_proc_modules();
      return;
    }
  }
  ok("proc-modules");
}

static void t_modinfo(void) {
  size_t size = 0;
  char *buf = slurp(MODDIR "/isofs.ko", &size);
  if (!buf) {
    fail("modinfo", "cannot read isofs.ko");
    return;
  }
  size_t milen = 0;
  const char *mi = find_modinfo(buf, size, &milen);
  if (!mi) {
    fail("modinfo", "no .modinfo section");
    free(buf);
    return;
  }
  const char *name = modinfo_value(mi, milen, "name");
  const char *lic = modinfo_value(mi, milen, "license");
  const char *vm = modinfo_value(mi, milen, "vermagic");
  if (!name || strcmp(name, "isofs") != 0) {
    fail("modinfo", "name tag wrong");
    free(buf);
    return;
  }
  if (!lic || lic[0] == '\0') {
    fail("modinfo", "license tag missing");
    free(buf);
    return;
  }
  struct utsname u;
  if (uname(&u) != 0) {
    fail("modinfo", "uname failed");
    free(buf);
    return;
  }
  size_t rl = strlen(u.release);
  if (!vm || strncmp(vm, u.release, rl) != 0 || vm[rl] != ' ') {
    fail("modinfo", "vermagic does not start with the running release");
    free(buf);
    return;
  }
  free(buf);
  ok("modinfo");
}

static void t_fs_modules(void) {
  /* Each of these is a filesystem module: it must be visible as a filesystem
   * type, which only happens if its init function ran inside the kernel. */
  static const char *fs[] = {"isofs", "ntfs", "btrfs"};
  for (unsigned i = 0; i < sizeof(fs) / sizeof(fs[0]); i++) {
    if (!file_contains_word("/proc/filesystems", fs[i])) {
      fail("fs-modules", "a loaded filesystem module is not in /proc/filesystems");
      return;
    }
  }
  ok("fs-modules");
}

static void t_sound_module(void) {
  char state[64];
  if (read_first_line("/sys/module/hda/initstate", state, sizeof(state)) != 0) {
    fail("sound-module", "no /sys/module/hda/initstate");
    return;
  }
  if (strcmp(state, "live") != 0) {
    fail("sound-module", "hda is not live");
    return;
  }
  char csz[64];
  if (read_first_line("/sys/module/hda/coresize", csz, sizeof(csz)) != 0) {
    fail("sound-module", "no coresize");
    return;
  }
  struct modrow rows[64];
  int n = read_modules(rows, 64);
  const struct modrow *r = n > 0 ? find_mod(rows, n, "hda") : 0;
  if (!r) {
    fail("sound-module", "hda missing from /proc/modules");
    return;
  }
  if (strtoul(csz, 0, 10) != r->size) {
    fail("sound-module", "sysfs coresize disagrees with /proc/modules");
    return;
  }
  ok("sound-module");
}

static void t_rmmod_insmod(void) {
  if (!file_contains_word("/proc/filesystems", "ntfs")) {
    fail("rmmod-insmod", "ntfs not registered to begin with");
    return;
  }
  if (rmmod("ntfs") != 0) {
    fail("rmmod-insmod", "delete_module(ntfs) failed");
    return;
  }
  struct modrow rows[64];
  int n = read_modules(rows, 64);
  if (n > 0 && find_mod(rows, n, "ntfs")) {
    fail("rmmod-insmod", "ntfs still listed after unload");
    return;
  }
  if (file_contains_word("/proc/filesystems", "ntfs")) {
    fail("rmmod-insmod", "ntfs filesystem type survived the unload");
    return;
  }
  if (insmod_path(MODDIR "/ntfs.ko", "") != 0) {
    fail("rmmod-insmod", "reload failed");
    return;
  }
  n = read_modules(rows, 64);
  if (n <= 0 || !find_mod(rows, n, "ntfs")) {
    fail("rmmod-insmod", "ntfs missing after reload");
    return;
  }
  if (!file_contains_word("/proc/filesystems", "ntfs")) {
    fail("rmmod-insmod", "filesystem type not re-registered");
    return;
  }
  ok("rmmod-insmod");
}

static void t_refcount(void) {
  struct modrow rows[64];
  int n = read_modules(rows, 64);
  const struct modrow *ipv6 = n > 0 ? find_mod(rows, n, "ipv6") : 0;
  const struct modrow *ndp = n > 0 ? find_mod(rows, n, "ndp") : 0;
  if (!ipv6 || !ndp) {
    fail("refcount", "ipv6/ndp not loaded");
    return;
  }
  if (ipv6->used < 1) {
    fail("refcount", "ipv6 use count is zero although ndp depends on it");
    return;
  }
  if (strstr(ndp->deps, "ipv6") == 0) {
    fail("refcount", "ndp does not list ipv6 as a dependency");
    return;
  }
  char rc[64];
  if (read_first_line("/sys/module/ipv6/refcnt", rc, sizeof(rc)) != 0) {
    fail("refcount", "no /sys/module/ipv6/refcnt");
    return;
  }
  if ((int)strtol(rc, 0, 10) != ipv6->used) {
    fail("refcount", "sysfs refcnt disagrees with /proc/modules");
    return;
  }
  errno = 0;
  if (rmmod("ipv6") == 0) {
    fail("refcount", "a referenced module was removable");
    return;
  }
  if (errno != EBUSY) {
    fail("refcount", "removing a referenced module did not report EBUSY");
    return;
  }
  ok("refcount");
}

static void t_dup_load(void) {
  errno = 0;
  if (insmod_path(MODDIR "/isofs.ko", "") == 0) {
    fail("dup-load", "loading an already-loaded module succeeded");
    return;
  }
  if (errno != EEXIST) {
    fail("dup-load", "duplicate load did not report EEXIST");
    return;
  }
  ok("dup-load");
}

static void t_vermagic_reject(void) {
  size_t size = 0;
  char *buf = slurp(MODDIR "/btrfs.ko", &size);
  if (!buf) {
    fail("vermagic-reject", "cannot read btrfs.ko");
    return;
  }
  /* Corrupt the vermagic value in place. The blob is a run of NUL-terminated
   * "key=value" records, so this is a byte edit, not a re-link. */
  size_t milen = 0;
  const char *mi = find_modinfo(buf, size, &milen);
  const char *vm = mi ? modinfo_value(mi, milen, "vermagic") : 0;
  if (!vm) {
    fail("vermagic-reject", "no vermagic tag to corrupt");
    free(buf);
    return;
  }
  char *mut = (char *)vm;
  mut[0] = (mut[0] == '9') ? '8' : '9';

  if (rmmod("btrfs") != 0) {
    fail("vermagic-reject", "could not unload btrfs first");
    free(buf);
    return;
  }
  errno = 0;
  long rc = syscall(SYS_init_module, buf, (unsigned long)size, "");
  int saved = errno;
  if (rc == 0) {
    fail("vermagic-reject", "a module built for another kernel was accepted");
    free(buf);
    return;
  }
  if (saved != ENOEXEC) {
    fail("vermagic-reject", "wrong errno for a vermagic mismatch");
    free(buf);
    return;
  }
  free(buf);
  /* The intact original must still load, proving the rejection was about the
   * corrupted byte and not about the module. */
  if (insmod_path(MODDIR "/btrfs.ko", "") != 0) {
    fail("vermagic-reject", "the intact module no longer loads");
    return;
  }
  if (!file_contains_word("/proc/filesystems", "btrfs")) {
    fail("vermagic-reject", "btrfs did not re-register after reload");
    return;
  }
  ok("vermagic-reject");
}

static void t_init_module(void) {
  /* The raw init_module(2) path: the image comes from this process's memory,
   * not from a descriptor. Unload isofs, load it back this way, and check the
   * filesystem type reappears. */
  size_t size = 0;
  char *buf = slurp(MODDIR "/isofs.ko", &size);
  if (!buf) {
    fail("init-module", "cannot read isofs.ko");
    return;
  }
  if (rmmod("isofs") != 0) {
    fail("init-module", "could not unload isofs");
    free(buf);
    return;
  }
  if (file_contains_word("/proc/filesystems", "iso9660")) {
    fail("init-module", "iso9660 type survived the unload");
    free(buf);
    return;
  }
  long rc = syscall(SYS_init_module, buf, (unsigned long)size, "");
  free(buf);
  if (rc != 0) {
    fail("init-module", "init_module failed");
    return;
  }
  if (!file_contains_word("/proc/filesystems", "iso9660")) {
    fail("init-module", "iso9660 type not restored");
    return;
  }
  ok("init-module");
}

static void t_unpriv(void) {
  pid_t pid = fork();
  if (pid < 0) {
    fail("unpriv", "fork failed");
    return;
  }
  if (pid == 0) {
    if (setgid(65534) != 0 || setuid(65534) != 0)
      _exit(2);
    errno = 0;
    long rc = syscall(SYS_delete_module, "isofs", 0);
    if (rc == 0)
      _exit(3); /* unprivileged removal succeeded — a hole */
    _exit(errno == EPERM ? 0 : 4);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
    fail("unpriv", "child did not exit normally");
    return;
  }
  int code = WEXITSTATUS(status);
  if (code == 2) {
    fail("unpriv", "could not drop privileges");
    return;
  }
  if (code == 3) {
    fail("unpriv", "an unprivileged process removed a module");
    return;
  }
  if (code != 0) {
    fail("unpriv", "unprivileged delete_module did not report EPERM");
    return;
  }
  /* And the module is still there. */
  struct modrow rows[64];
  int n = read_modules(rows, 64);
  if (n <= 0 || !find_mod(rows, n, "isofs")) {
    fail("unpriv", "the module disappeared anyway");
    return;
  }
  ok("unpriv");
}

int main(void) {
  t_proc_modules();
  t_modinfo();
  t_fs_modules();
  t_sound_module();
  t_rmmod_insmod();
  t_refcount();
  t_dup_load();
  t_vermagic_reject();
  t_init_module();
  t_unpriv();

  if (failures == 0)
    printf("M95-SMOKE: done\n");
  else
    printf("M95-SMOKE: done with %d failure(s)\n", failures);
  return failures ? 1 : 0;
}
