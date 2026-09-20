/* SPDX-License-Identifier: GPL-2.0-only */
/* selfhost_build — replay the host kernel build inside b1nix (M26).
 *
 * /mnt/build/cmds.txt holds one compile per line and /mnt/build/link.txt the
 * final link, each a tab-separated argv produced from the host build by
 * tests/selfhost/build-selfhost-module.sh. Paths in them are relative to
 * /mnt/build/src, and the toolchain is Alpine's clang and ld.lld under
 * /mnt/build/usr, whose libraries are found through LD_LIBRARY_PATH. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char *const g_env[] = {
    "LD_LIBRARY_PATH=/mnt/build/lib:/mnt/build/usr/lib:/mnt/build/usr/lib/llvm17/lib",
    "PATH=/mnt/build/usr/bin",
    "TMPDIR=/tmp",
    0,
};

static void emit(const char *s) { (void)!write(1, s, strlen(s)); }

static char *slurp(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return 0;
  }
  char *buf = malloc((size_t)st.st_size + 1);
  size_t got = 0;
  while (buf && got < (size_t)st.st_size) {
    ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
    if (r <= 0)
      break;
    got += (size_t)r;
  }
  close(fd);
  if (!buf)
    return 0;
  buf[got] = 0;
  return buf;
}

/* Run one tab-separated command line in place; returns the wait status. With
 * `log`, the command's stdout and stderr go to that file. */
static int run_line(char *line, const char *log) {
  /* Sized from the line: the final link alone has more than 675 arguments. */
  size_t max = 2;
  for (const char *c = line; *c; c++)
    if (*c == '\t')
      max++;
  char **argv = malloc(max * sizeof(*argv));
  int argc = 0;
  if (!argv)
    return -1;
  for (char *tok = strtok(line, "\t"); tok; tok = strtok(0, "\t"))
    argv[argc++] = tok;
  argv[argc] = 0;
  if (argc == 0) {
    free(argv);
    return 0;
  }
  pid_t pid = fork();
  if (pid == 0) {
    if (log) {
      int lfd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (lfd >= 0) {
        dup2(lfd, 1);
        dup2(lfd, 2);
        close(lfd);
      }
    }
    execve(argv[0], argv, g_env);
    char msg[320];
    snprintf(msg, sizeof(msg), "M26-SELFHOST-USER: exec %s: %s\n", argv[0],
             strerror(errno));
    emit(msg);
    _exit(127);
  }
  free(argv);
  if (pid < 0)
    return -1;
  int st = 0;
  waitpid(pid, &st, 0);
  return st;
}

static const char *out_of(const char *line) {
  const char *o = strstr(line, "\t-o\t");
  return o ? o + 4 : line;
}

int main(void) {
  char msg[512];

  emit("M26-SELFHOST-USER: start\n");
  if (chdir("/mnt/build/src") != 0) {
    emit("M26-SELFHOST-USER: fail no-src\n");
    return 1;
  }
  char *cmds = slurp("/mnt/build/cmds.txt");
  char *link = slurp("/mnt/build/link.txt");
  if (!cmds || !link) {
    emit("M26-SELFHOST-USER: fail no-commands\n");
    return 1;
  }

  int total = 0, failed = 0;
  for (char *p = cmds, *next; *p; p = next) {
    char *eol = strchr(p, '\n');
    if (eol) {
      *eol = 0;
      next = eol + 1;
    } else {
      next = p + strlen(p);
    }
    char label[256];
    snprintf(label, sizeof(label), "%.200s", out_of(p));
    char *tab = strchr(label, '\t');
    if (tab)
      *tab = 0;
    int st = run_line(p, 0);
    total++;
    if (st != 0) {
      failed++;
      snprintf(msg, sizeof(msg), "M26-SELFHOST-USER: cc-fail %s status=%d\n",
               label, st);
      emit(msg);
    }
    if ((total % 25) == 0) {
      snprintf(msg, sizeof(msg), "M26-SELFHOST-USER: progress %d compiled\n",
               total);
      emit(msg);
    }
  }
  snprintf(msg, sizeof(msg), "M26-SELFHOST-USER: compiled %d/%d (failed=%d)\n",
           total - failed, total, failed);
  emit(msg);
  if (failed != 0 || total == 0) {
    emit("M26-SELFHOST-USER: fail compile\n");
    return 1;
  }
  emit("M26-SELFHOST-USER: ok compile\n");

  char *nl = strchr(link, '\n');
  if (nl)
    *nl = 0;


  int lst = run_line(link, "/mnt/build/link.log");
  sync();
  if (lst != 0) {
    char *log = slurp("/mnt/build/link.log");
    if (log) {
      log[log[0] && strlen(log) > 2000 ? 2000 : strlen(log)] = 0;
      emit("M26-SELFHOST-USER: link output:\n");
      emit(log);
      emit("\n");
    }
  }
  unsigned char mag[4] = {0};
  int fd = open("/mnt/build/kernel.elf", O_RDONLY);
  if (fd >= 0) {
    (void)!read(fd, mag, sizeof(mag));
    close(fd);
  }
  snprintf(msg, sizeof(msg),
           "M26-SELFHOST-USER: link exit=%d magic=%02x%02x%02x%02x\n", lst,
           mag[0], mag[1], mag[2], mag[3]);
  emit(msg);
  if (lst == 0 && mag[0] == 0x7f && mag[1] == 'E' && mag[2] == 'L' &&
      mag[3] == 'F')
    emit("M26-SELFHOST-USER: ok kernel-elf\n");
  else
    emit("M26-SELFHOST-USER: fail link\n");
  return 0;
}
