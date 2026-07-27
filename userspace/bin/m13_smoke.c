/*
 * m13_smoke — userspace ABI, libc, stdio, execve, argv/envp, fd inheritance.
 * Rewritten to use POSIX API (no b1nix raw syscalls).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>

static void marker(const char *text) {
  write(1, text, strlen(text));
}

static int wait_exit_ok(pid_t pid, int expected) {
  int st = 0;
  pid_t wr = waitpid(pid, &st, 0);
  if (wr != pid || !WIFEXITED(st))
    return 0;
  return WEXITSTATUS(st) == expected;
}

static int wait_exit_status(pid_t pid, int *out_status) {
  int st = 0;
  pid_t wr = waitpid(pid, &st, 0);
  if (wr != pid || !WIFEXITED(st))
    return 0;
  if (out_status)
    *out_status = WEXITSTATUS(st);
  return 1;
}

static int read_all_file(const char *path, char *buf, int buf_sz) {
  if (!path || !buf || buf_sz <= 1)
    return -1;
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int n = read(fd, buf, (size_t)buf_sz - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = '\0';
  return n;
}

static int test_m24_errno(void) {
  marker("M24-SMOKE: start\n");

  /* 1. ENOENT */
  errno = 0;
  int fd = open("/tmp/nonexistent_file_xyz_123", O_RDONLY);
  if (fd >= 0 || errno != ENOENT) {
    marker("M24-SMOKE: fail ENOENT\n");
    return 1;
  }
  marker("M24-SMOKE: ok ENOENT\n");

  /* 2. EEXIST */
  unlink("/tmp/exists_test");
  fd = open("/tmp/exists_test", O_CREAT | O_RDWR, 0666);
  if (fd >= 0) {
    close(fd);
    errno = 0;
    int fd2 = open("/tmp/exists_test", O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd2 >= 0 || errno != EEXIST) {
      marker("M24-SMOKE: fail EEXIST\n");
      return 2;
    }
    marker("M24-SMOKE: ok EEXIST\n");
  } else {
    marker("M24-SMOKE: fail EEXIST setup\n");
    return 2;
  }

  /* 3. EINVAL */
  errno = 0;
  long rc = lseek(0, 0, 999);
  if (rc >= 0 || errno != EINVAL) {
    marker("M24-SMOKE: fail EINVAL\n");
    return 3;
  }
  marker("M24-SMOKE: ok EINVAL\n");

  /* 4. EBADF */
  errno = 0;
  rc = close(999);
  if (rc >= 0 || errno != EBADF) {
    marker("M24-SMOKE: fail EBADF\n");
    return 4;
  }
  marker("M24-SMOKE: ok EBADF\n");

  /* 5. ENOTDIR */
  errno = 0;
  fd = open("/tmp/exists_test/child", O_RDONLY);
  if (fd >= 0 || errno != ENOTDIR) {
    char d[96];
    snprintf(d, sizeof(d), "M24-SMOKE: fail ENOTDIR errno=%d\n", errno);
    marker(d);
    return 5;
  }
  marker("M24-SMOKE: ok ENOTDIR\n");

  /* 6. EISDIR */
  errno = 0;
  fd = open("/tmp", O_WRONLY);
  if (fd >= 0 || errno != EISDIR) {
    marker("M24-SMOKE: fail EISDIR\n");
    return 6;
  }
  marker("M24-SMOKE: ok EISDIR\n");

  /* 7. EACCES */
  unlink("/tmp/eacces_test");
  int fd_acc = open("/tmp/eacces_test", O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (fd_acc >= 0) {
    close(fd_acc);
    if (chmod("/tmp/eacces_test", 0400) != 0) {
      marker("M24-SMOKE: fail EACCES chmod\n");
      return 7;
    }
    pid_t acc_pid = fork();
    if (acc_pid == 0) {
      int su = setuid(1000);
      uid_t uid_after = getuid();
      errno = 0;
      int fd_child = open("/tmp/eacces_test", O_RDONLY);
      if (su == 0 && uid_after == 1000 && fd_child < 0 && errno == EACCES) {
        _exit(0);
      }
      if (fd_child >= 0 || errno != EACCES) {
        char d[96];
        snprintf(d, sizeof(d),
                 "M24-SMOKE: fail EACCES su=%d uid=%d errno=%d fd=%d\n", su,
                 (int)uid_after, errno, fd_child);
        marker(d);
      }
      _exit(1);
    } else if (acc_pid > 0) {
      int st = 0;
      waitpid(acc_pid, &st, 0);
      if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        marker("M24-SMOKE: fail EACCES\n");
        return 7;
      }
    } else {
      marker("M24-SMOKE: fail EACCES fork\n");
      return 7;
    }
  } else {
    marker("M24-SMOKE: fail EACCES setup\n");
    return 7;
  }

  marker("M24-SMOKE: ok EACCES\n");
  marker("M24-SMOKE: ok errno-mapping\n");
  marker("M24-SMOKE: ok diagnostics\n");
  return 0;
}

int main(int argc, char **argv, char **envp) {
  if (argc >= 2 && argv && argv[1] && strcmp(argv[1], "--m24") == 0)
    return test_m24_errno();
  if (argc >= 2 && argv && argv[1] && strcmp(argv[1], "check-exec-argv-env") == 0) {
    int env_ok = 0;
    if (envp) {
      for (int i = 0; envp[i]; i++) {
        if (strcmp(envp[i], "M13_TOKEN=ok") == 0) {
          env_ok = 1;
          break;
        }
      }
    }
    int argv_ok = (argc == 4 && argv && argv[0] &&
                   strcmp(argv[0], "m13-smoke") == 0 &&
                   strcmp(argv[1], "check-exec-argv-env") == 0 &&
                   strcmp(argv[2], "alpha") == 0 && strcmp(argv[3], "beta") == 0);
    if (!(env_ok && argv_ok)) {
      char d[160];
      snprintf(d, sizeof(d),
               "M13-SMOKE: detail exec-argv-env argc=%d a0=%s a1=%s a2=%s "
               "a3=%s env=%d\n",
               argc, (argc > 0 && argv[0]) ? argv[0] : "(null)",
               (argc > 1 && argv[1]) ? argv[1] : "(null)",
               (argc > 2 && argv[2]) ? argv[2] : "(null)",
               (argc > 3 && argv[3]) ? argv[3] : "(null)", env_ok);
      marker(d);
    }
    return (env_ok && argv_ok) ? 0 : 1;
  }
  if (argc >= 3 && argv && argv[1] && strcmp(argv[1], "check-fd-open") == 0) {
    int fd = atoi(argv[2]);
    char c = 0;
    int ok = (read(fd, &c, 1) >= 0);
    if (!ok) {
      char d[128];
      snprintf(d, sizeof(d),
               "M13-SMOKE: detail fd-open argc=%d a0=%s a1=%s a2=%s\n", argc,
               (argc > 0 && argv[0]) ? argv[0] : "(null)",
               (argc > 1 && argv[1]) ? argv[1] : "(null)",
               (argc > 2 && argv[2]) ? argv[2] : "(null)");
      marker(d);
    }
    return ok ? 0 : 1;
  }
  if (argc >= 3 && argv && argv[1] && strcmp(argv[1], "check-fd-closed") == 0) {
    int fd = atoi(argv[2]);
    char c = 0;
    int ok = (read(fd, &c, 1) < 0);
    if (!ok) {
      char d[128];
      snprintf(d, sizeof(d),
               "M13-SMOKE: detail fd-closed argc=%d a0=%s a1=%s a2=%s\n", argc,
               (argc > 0 && argv[0]) ? argv[0] : "(null)",
               (argc > 1 && argv[1]) ? argv[1] : "(null)",
               (argc > 2 && argv[2]) ? argv[2] : "(null)");
      marker(d);
    }
    return ok ? 0 : 1;
  }

  if (!(argc >= 1 && argv && argv[0] &&
        (strcmp(argv[0], "/bin/m13-smoke") == 0 ||
         strcmp(argv[0], "m13-smoke") == 0 ||
         strcmp(argv[0], "/bin/m13_smoke") == 0 ||
         strcmp(argv[0], "m13_smoke") == 0)))
    return 1;

  marker("M13-SMOKE: start\n");

  if (argc >= 1 && argv && argv[0] &&
      (strcmp(argv[0], "/bin/m13-smoke") == 0 ||
       strcmp(argv[0], "m13-smoke") == 0 ||
       strcmp(argv[0], "/bin/m13_smoke") == 0 ||
       strcmp(argv[0], "m13_smoke") == 0)) {
    marker("M13-SMOKE: ok argc-argv0\n");
  } else {
    marker("M13-SMOKE: fail argc-argv0\n");
  }

#ifdef __x86_64__
  unsigned long want_argv_align = 8;
#else
  unsigned long want_argv_align = 4;
#endif
  if (((unsigned long)argv & 0xF) == want_argv_align) {
    marker("M13-SMOKE: ok stack-align\n");
  } else {
    marker("M13-SMOKE: fail stack-align\n");
  }

  int fd = open("/tmp/m13_rw.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (fd >= 0) {
    const char *msg = "m13-io";
    int wr = write(fd, msg, 6);
    long off = lseek(fd, 0, SEEK_SET);
    char buf[8];
    memset(buf, 0, sizeof(buf));
    int rd = read(fd, buf, 6);
    close(fd);
    if (wr == 6 && off == 0 && rd == 6 && strcmp(buf, "m13-io") == 0) {
      marker("M13-SMOKE: ok libc-rw-open-close-lseek\n");
    } else {
      marker("M13-SMOKE: fail libc-rw-open-close-lseek\n");
    }
  } else {
    marker("M13-SMOKE: fail libc-rw-open-close-lseek\n");
  }

  pid_t pid = getpid();
  uid_t uid = getuid();
  gid_t gid = getgid();
  if (pid > 0 && uid >= 0 && gid >= 0) {
    marker("M13-SMOKE: ok getpid-uid-gid\n");
  } else {
    marker("M13-SMOKE: fail getpid-uid-gid\n");
  }

  void *brk0 = sbrk(0);
  void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
  int mu = (map != MAP_FAILED) ? munmap(map, 4096) : -1;
  if (brk0 != (void *)-1 && map != MAP_FAILED && mu == 0) {
    marker("M13-SMOKE: ok brk-mmap-munmap\n");
  } else {
    marker("M13-SMOKE: fail brk-mmap-munmap\n");
  }

  char sn[64];
  int sn_ok = snprintf(sn, sizeof(sn), "m13-%d", 13);
  puts("M13-SMOKE: ok puts");
  printf("M13-SMOKE: ok printf\n");
  if (sn_ok > 0 && strcmp(sn, "m13-13") == 0) {
    marker("M13-SMOKE: ok snprintf\n");
  } else {
    marker("M13-SMOKE: fail snprintf\n");
  }

  FILE *wf = fopen("/tmp/m13_stdio.txt", "w");
  if (wf) {
    const char *payload = "stdio-data";
    size_t wn = fwrite(payload, 1, strlen(payload), wf);
    fclose(wf);
    FILE *rf = fopen("/tmp/m13_stdio.txt", "r");
    if (rf) {
      char rb[32];
      memset(rb, 0, sizeof(rb));
      size_t rn = fread(rb, 1, strlen(payload), rf);
      fclose(rf);
      if (wn == strlen(payload) && rn == strlen(payload) &&
          strcmp(rb, payload) == 0) {
        marker("M13-SMOKE: ok stdio-file\n");
      } else {
        marker("M13-SMOKE: fail stdio-file\n");
      }
    } else {
      marker("M13-SMOKE: fail stdio-file\n");
    }
  } else {
    marker("M13-SMOKE: fail stdio-file\n");
  }

  /* fork+execve */
  {
    pid_t fe = fork();
    if (fe == 0) {
      char *a[] = {"echo", "alpha", "beta", NULL};
      char *e[] = {"M13_TOKEN=ok", "PATH=/bin", NULL};
      execve("/bin/echo", a, e);
      _exit(111);
    } else if (fe > 0) {
      if (wait_exit_ok(fe, 0)) {
        marker("M13-SMOKE: ok fork-execve-waitpid\n");
        marker("M13-SMOKE: ok builtin-exec\n");
      } else {
        marker("M13-SMOKE: fail fork-execve-waitpid\n");
        marker("M13-SMOKE: unsupported builtin-exec\n");
      }
    } else {
      marker("M13-SMOKE: fail fork-execve-waitpid\n");
      marker("M13-SMOKE: unsupported builtin-exec\n");
    }
  }

  /* execve fail deterministic */
  {
    pid_t fe = fork();
    if (fe == 0) {
      char *a[] = {"/bin/does-not-exist", NULL};
      char *e[] = {NULL};
      execve("/bin/does-not-exist", a, e);
      if (errno == ENOENT)
        _exit(0);
      _exit(112);
    } else if (fe > 0) {
      if (wait_exit_ok(fe, 0))
        marker("M13-SMOKE: ok execve-fail-deterministic\n");
      else
        marker("M13-SMOKE: fail execve-fail-deterministic\n");
    } else {
      marker("M13-SMOKE: fail execve-fail-deterministic\n");
    }
  }

  /* argv/envp round-trip via execve of self */
  {
    pid_t ae = fork();
    if (ae == 0) {
      char *a[] = {"m13-smoke", "check-exec-argv-env", "alpha", "beta", NULL};
      char *e[] = {"M13_TOKEN=ok", "PATH=/bin", NULL};
      execve("/bin/m13-smoke", a, e);
      _exit(121);
    } else if (ae > 0) {
      if (wait_exit_ok(ae, 0))
        marker("M13-SMOKE: ok execve-argv-env\n");
      else
        marker("M13-SMOKE: fail execve-argv-env\n");
    } else {
      marker("M13-SMOKE: fail execve-argv-env\n");
    }
  }

  /* sh -c with positional args */
  {
    pid_t sh = fork();
    if (sh == 0) {
      char *a[] = {"sh", "-c",
                   "[ $0 = m13sh ] && echo ok >/tmp/m13_sh_argv.ok; echo $1 $2 "
                   ">/tmp/m13_sh_args.ok",
                   "m13sh", "alpha", "beta", NULL};
      char *e[] = {NULL};
      execve("/bin/sh", a, e);
      _exit(111);
    } else if (sh > 0) {
      int st = 0;
      pid_t wr = waitpid(sh, &st, 0);
      if (wr == sh && WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        marker("M13-SMOKE: ok sh-c-status\n");
      } else {
        marker("M13-SMOKE: fail sh-c-status\n");
      }
      char sh0[16], shargs[32];
      int rn0 = read_all_file("/tmp/m13_sh_argv.ok", sh0, sizeof(sh0));
      int rn1 = read_all_file("/tmp/m13_sh_args.ok", shargs, sizeof(shargs));
      if (rn0 > 0 && rn1 > 0 && strcmp(sh0, "ok\n") == 0 &&
          strcmp(shargs, "alpha beta\n") == 0) {
        marker("M13-SMOKE: ok sh-c-argv\n");
      } else {
        marker("M13-SMOKE: fail sh-c-argv\n");
      }
    } else {
      marker("M13-SMOKE: fail sh-c-status\n");
      marker("M13-SMOKE: fail sh-c-argv\n");
    }
  }

  /* dup2 */
  {
    int src = open("/tmp/m13_dup_src.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);
    int dst = open("/tmp/m13_dup_dst.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (src >= 0 && dst >= 0) {
      int rc = dup2(src, dst);
      int wr = write(dst, "x", 1);
      if (rc == dst && wr == 1)
        marker("M13-SMOKE: ok dup2\n");
      else
        marker("M13-SMOKE: fail dup2\n");
    } else {
      marker("M13-SMOKE: fail dup2\n");
    }
    if (src >= 0) close(src);
    if (dst >= 0) close(dst);
  }

  /* parent intact after child exec fail */
  {
    int keep_fd = open("/tmp/m13_parent_alive.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (keep_fd >= 0) {
      pid_t child = fork();
      if (child == 0) {
        char *a[] = {"/bin/not-here", NULL};
        char *e[] = {NULL};
        execve("/bin/not-here", a, e);
        _exit(0);
      } else if (child > 0) {
        int ok = wait_exit_ok(child, 0);
        int wr = write(keep_fd, "p", 1);
        if (ok && wr == 1)
          marker("M13-SMOKE: ok parent-intact\n");
        else
          marker("M13-SMOKE: fail parent-intact\n");
      } else {
        marker("M13-SMOKE: fail parent-intact\n");
      }
      close(keep_fd);
    } else {
      marker("M13-SMOKE: fail parent-intact\n");
    }
  }

  /* fd-inherit across exec */
  {
    int inh_fd = open("/tmp/m13_inherit.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (inh_fd >= 0) {
      pid_t p = fork();
      if (p == 0) {
        char fd_s[16];
        snprintf(fd_s, sizeof(fd_s), "%d", inh_fd);
        char *a[] = {"m13-smoke", "check-fd-open", fd_s, NULL};
        char *e[] = {"PATH=/bin", NULL};
        execve("/bin/m13-smoke", a, e);
        _exit(122);
      } else if (p > 0) {
        int ec = -1;
        if (wait_exit_status(p, &ec) && ec == 0)
          marker("M13-SMOKE: ok fd-inherit-exec\n");
        else if (ec == 122)
          marker("M13-SMOKE: unsupported fd-inherit-exec\n");
        else
          marker("M13-SMOKE: fail fd-inherit-exec\n");
      } else {
        marker("M13-SMOKE: fail fd-inherit-exec\n");
      }
      close(inh_fd);
    } else {
      marker("M13-SMOKE: fail fd-inherit-exec\n");
    }
  }

  /* cloexec across exec */
  {
    int coe_fd =
        open("/tmp/m13_coe.txt", O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0666);
    if (coe_fd >= 0) {
      pid_t p = fork();
      if (p == 0) {
        char fd_s[16];
        snprintf(fd_s, sizeof(fd_s), "%d", coe_fd);
        char *a[] = {"m13-smoke", "check-fd-closed", fd_s, NULL};
        char *e[] = {"PATH=/bin", NULL};
        execve("/bin/m13-smoke", a, e);
        _exit(123);
      } else if (p > 0) {
        int ec = -1;
        if (wait_exit_status(p, &ec) && ec == 0)
          marker("M13-SMOKE: ok cloexec-exec\n");
        else if (ec == 123)
          marker("M13-SMOKE: unsupported cloexec-exec\n");
        else
          marker("M13-SMOKE: fail cloexec-exec\n");
      } else {
        marker("M13-SMOKE: fail cloexec-exec\n");
      }
      close(coe_fd);
    } else {
      marker("M13-SMOKE: fail cloexec-exec\n");
    }
  }

  /* errno negative */
  {
    int errfd = open("/tmp/m13_missing_file", O_RDONLY);
    if (errfd < 0)
      marker("M13-SMOKE: ok errno-negative\n");
    else {
      close(errfd);
      marker("M13-SMOKE: fail errno-negative\n");
    }
  }

  /* /bin/native_smoke is a bare-asm, raw-syscall ELF (no libc, no crt): it
   * prints its own "NATIVE-SMOKE: ok" line, but "done" means it also *exited
   * cleanly*, which only its parent can observe. The discovery loop can't
   * report that, so exec it here and report the status. */
  {
    pid_t np = fork();
    if (np == 0) {
      char *av[] = {(char *)"/bin/native_smoke", NULL};
      char *ev[] = {(char *)"PATH=/bin", NULL};
      execve(av[0], av, ev);
      _exit(127);
    }
    if (np > 0) {
      int st = 0;
      if (waitpid(np, &st, 0) == np && WIFEXITED(st) && WEXITSTATUS(st) == 0)
        marker("NATIVE-SMOKE: done\n");
      else
        marker("NATIVE-SMOKE: fail\n");
    }
  }

  marker("M13-SMOKE: done\n");

  /* M24 errno matrix. It used to be driven by the in-kernel dispatcher passing
   * "--m24"; nothing does that since the Ring 3 init took over test discovery
   * (which runs every /bin binary with no arguments), so run it inline here as
   * well. The explicit "--m24" entry point above stays for manual runs. */
  test_m24_errno();
  return 0;
}
