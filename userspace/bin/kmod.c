/* kmod — the module utilities (M95/M96), a single multi-call binary installed
 * as /bin/insmod, /bin/rmmod, /bin/lsmod, /bin/modinfo and /sbin/modprobe.
 *
 *   lsmod                     list /proc/modules
 *   insmod <file.ko> [k=v]... load one module image
 *   rmmod <name>              unload one module
 *   modinfo <name|file.ko>    print the module's .modinfo tags
 *   modprobe [-r] <name>      resolve aliases + modules.dep, then load/unload
 *
 * Everything reads the real kernel interfaces: /proc/modules, the
 * init_module/finit_module/delete_module syscalls, and the generated
 * /lib/modules/modules.{dep,alias} index files.
 */

#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MODULE_DIR "/lib/modules"

static int do_finit(int fd, const char *params) {
  return (int)syscall(SYS_finit_module, fd, params ? params : "", 0);
}

static int do_delete(const char *name, unsigned flags) {
  return (int)syscall(SYS_delete_module, name, flags);
}

static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

/* ── lsmod ───────────────────────────────────────────────────────────────── */
static int cmd_lsmod(void) {
  FILE *f = fopen("/proc/modules", "r");
  if (!f) {
    fprintf(stderr, "lsmod: /proc/modules: %s\n", strerror(errno));
    return 1;
  }
  printf("Module                  Size  Used by\n");
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char name[64], deps[192], state[32], addr[32];
    unsigned long size;
    int used;
    if (sscanf(line, "%63s %lu %d %191s %31s %31s", name, &size, &used, deps,
               state, addr) < 5)
      continue;
    printf("%-20s %7lu  %d %s\n", name, size, used,
           strcmp(deps, "-") == 0 ? "" : deps);
  }
  fclose(f);
  return 0;
}

/* ── insmod ──────────────────────────────────────────────────────────────── */
static int insmod_file(const char *path, const char *params) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "insmod: %s: %s\n", path, strerror(errno));
    return 1;
  }
  int rc = do_finit(fd, params);
  int saved = errno;
  close(fd);
  if (rc != 0) {
    fprintf(stderr, "insmod: %s: %s\n", path, strerror(saved));
    return 1;
  }
  return 0;
}

static int cmd_insmod(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: insmod <file.ko> [param=value]...\n");
    return 1;
  }
  char params[256];
  params[0] = '\0';
  for (int i = 2; i < argc; i++) {
    if (params[0])
      strncat(params, " ", sizeof(params) - strlen(params) - 1);
    strncat(params, argv[i], sizeof(params) - strlen(params) - 1);
  }
  return insmod_file(argv[1], params);
}

/* ── rmmod ───────────────────────────────────────────────────────────────── */
static int cmd_rmmod(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: rmmod <module>\n");
    return 1;
  }
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    char name[64];
    snprintf(name, sizeof(name), "%s", base_name(argv[i]));
    char *dot = strstr(name, ".ko");
    if (dot)
      *dot = '\0';
    if (do_delete(name, 0) != 0) {
      fprintf(stderr, "rmmod: %s: %s\n", name, strerror(errno));
      rc = 1;
    }
  }
  return rc;
}

/* ── modinfo ─────────────────────────────────────────────────────────────── */
/* Read the .modinfo section of a relocatable object and print its records. */
static int print_modinfo(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "modinfo: %s: %s\n", path, strerror(errno));
    return 1;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
    close(fd);
    fprintf(stderr, "modinfo: %s: not an object file\n", path);
    return 1;
  }
  char *buf = malloc((size_t)st.st_size);
  if (!buf) {
    close(fd);
    return 1;
  }
  ssize_t got = 0;
  while (got < st.st_size) {
    ssize_t n = read(fd, buf + got, (size_t)(st.st_size - got));
    if (n <= 0)
      break;
    got += n;
  }
  close(fd);
  if (got != st.st_size) {
    free(buf);
    fprintf(stderr, "modinfo: %s: short read\n", path);
    return 1;
  }

  Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_type != ET_REL) {
    free(buf);
    fprintf(stderr, "modinfo: %s: not a kernel module\n", path);
    return 1;
  }
  Elf64_Shdr *sh = (Elf64_Shdr *)(buf + eh->e_shoff);
  const char *shstr = buf + sh[eh->e_shstrndx].sh_offset;
  int found = 0;
  printf("filename:       %s\n", path);
  for (int i = 0; i < eh->e_shnum; i++) {
    if (strcmp(shstr + sh[i].sh_name, ".modinfo") != 0)
      continue;
    found = 1;
    const char *p = buf + sh[i].sh_offset;
    size_t left = (size_t)sh[i].sh_size;
    while (left > 0) {
      size_t len = strnlen(p, left);
      if (len == 0) {
        p++;
        left--;
        continue;
      }
      const char *eq = memchr(p, '=', len);
      if (eq)
        printf("%-15.*s %.*s\n", (int)(eq - p) + 1, p, (int)(len - (size_t)(eq - p) - 1),
               eq + 1);
      p += len;
      left -= len;
      if (left) {
        p++;
        left--;
      }
    }
    break;
  }
  free(buf);
  if (!found) {
    fprintf(stderr, "modinfo: %s: no .modinfo section\n", path);
    return 1;
  }
  return 0;
}

static int cmd_modinfo(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: modinfo <module|file.ko>\n");
    return 1;
  }
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    char path[256];
    if (strchr(argv[i], '/') || strstr(argv[i], ".ko"))
      snprintf(path, sizeof(path), "%s", argv[i]);
    else
      snprintf(path, sizeof(path), MODULE_DIR "/%s.ko", argv[i]);
    if (print_modinfo(path) != 0)
      rc = 1;
  }
  return rc;
}

/* ── modprobe ────────────────────────────────────────────────────────────── */
/* Look "key: rest-of-line" up in one of the generated index files. */
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

static int module_is_loaded(const char *name) {
  FILE *f = fopen("/proc/modules", "r");
  if (!f)
    return 0;
  char line[512];
  size_t n = strlen(name);
  int loaded = 0;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, name, n) == 0 && (line[n] == ' ' || line[n] == '\t')) {
      loaded = 1;
      break;
    }
  }
  fclose(f);
  return loaded;
}

static int modprobe_one(const char *name, const char *params, int depth);

static int modprobe_deps(const char *name, int depth) {
  char deps[256];
  if (index_lookup(MODULE_DIR "/modules.dep", name, deps, sizeof(deps)) != 0)
    return 0;
  char *save = 0;
  for (char *tok = strtok_r(deps, " \t", &save); tok;
       tok = strtok_r(0, " \t", &save)) {
    if (modprobe_one(tok, "", depth + 1) != 0)
      return 1;
  }
  return 0;
}

static int modprobe_one(const char *name, const char *params, int depth) {
  if (depth > 8) {
    fprintf(stderr, "modprobe: dependency loop at %s\n", name);
    return 1;
  }
  char real[64];
  snprintf(real, sizeof(real), "%s", name);

  char path[256];
  snprintf(path, sizeof(path), MODULE_DIR "/%s.ko", real);
  if (access(path, R_OK) != 0) {
    char aliased[64];
    if (index_lookup(MODULE_DIR "/modules.alias", real, aliased,
                     sizeof(aliased)) == 0) {
      snprintf(real, sizeof(real), "%s", aliased);
      snprintf(path, sizeof(path), MODULE_DIR "/%s.ko", real);
    }
  }
  if (module_is_loaded(real))
    return 0;
  if (modprobe_deps(real, depth) != 0)
    return 1;
  return insmod_file(path, params);
}

static int cmd_modprobe(int argc, char **argv) {
  int remove = 0;
  int i = 1;
  if (i < argc && (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--remove") == 0)) {
    remove = 1;
    i++;
  }
  if (i >= argc) {
    fprintf(stderr, "usage: modprobe [-r] <module|alias> [param=value]...\n");
    return 1;
  }
  const char *name = argv[i++];
  if (remove) {
    if (do_delete(name, 0) != 0) {
      fprintf(stderr, "modprobe: remove %s: %s\n", name, strerror(errno));
      return 1;
    }
    return 0;
  }
  char params[256];
  params[0] = '\0';
  for (; i < argc; i++) {
    if (params[0])
      strncat(params, " ", sizeof(params) - strlen(params) - 1);
    strncat(params, argv[i], sizeof(params) - strlen(params) - 1);
  }
  return modprobe_one(name, params, 0);
}

int main(int argc, char **argv) {
  const char *me = base_name(argv[0]);
  if (strcmp(me, "lsmod") == 0)
    return cmd_lsmod();
  if (strcmp(me, "insmod") == 0)
    return cmd_insmod(argc, argv);
  if (strcmp(me, "rmmod") == 0)
    return cmd_rmmod(argc, argv);
  if (strcmp(me, "modinfo") == 0)
    return cmd_modinfo(argc, argv);
  if (strcmp(me, "modprobe") == 0)
    return cmd_modprobe(argc, argv);

  /* Invoked as `kmod <subcommand> ...`. */
  if (argc > 1) {
    const char *sub = argv[1];
    if (strcmp(sub, "lsmod") == 0)
      return cmd_lsmod();
    if (strcmp(sub, "insmod") == 0)
      return cmd_insmod(argc - 1, argv + 1);
    if (strcmp(sub, "rmmod") == 0)
      return cmd_rmmod(argc - 1, argv + 1);
    if (strcmp(sub, "modinfo") == 0)
      return cmd_modinfo(argc - 1, argv + 1);
    if (strcmp(sub, "modprobe") == 0)
      return cmd_modprobe(argc - 1, argv + 1);
  }
  fprintf(stderr, "usage: kmod {lsmod|insmod|rmmod|modinfo|modprobe} ...\n");
  return 1;
}
