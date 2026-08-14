/* mremap and MREMAP_MAYMOVE are GNU extensions in musl's headers. */
#define _GNU_SOURCE
/*
 * m12_smoke — POSIX-process and signal smoke test.
 * Verifies: fork, execve, waitpid, exit status, dup2, fcntl, brk,
 * mmap/munmap, kill, sigaction, setsid, getpgrp, uid/gid getters/setters.
 *
 * Rewritten to use POSIX API (no b1nix raw syscalls).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

static void marker(const char *text) {
  write(1, text, strlen(text));
}

int main(int argc, char **argv) {
  /* If invoked with check-close-on-exec argument: */
  if (argc > 2 && strcmp(argv[1], "check-close-on-exec") == 0) {
    int fd = atoi(argv[2]);
    char buf[10];
    ssize_t rc = read(fd, buf, sizeof(buf));
    /* Failed to read = expected (closed on exec) */
    return (rc < 0) ? 0 : 1;
  }

  marker("M12-SMOKE: start\n");

  /* 1. Spawn a basic command (fork+execve) */
  {
    pid_t pid = fork();
    if (pid == 0) {
      char *a[] = {"/bin/true", NULL};
      char *e[] = {NULL};
      execve("/bin/true", a, e);
      _exit(99);
    } else if (pid > 0) {
      int status = 0;
      pid_t wr = waitpid(pid, &status, 0);
      if (wr == pid) {
        marker("M12-SMOKE: ok spawn\n");
      } else {
        marker("M12-SMOKE: fail spawn waitpid\n");
      }
    } else {
      marker("M12-SMOKE: fail spawn fork\n");
    }
  }

  /* 2. Fork + Execve */
  {
    pid_t pid = fork();
    if (pid == 0) {
      char *a[] = {"/bin/true", NULL};
      char *e[] = {NULL};
      execve("/bin/true", a, e);
      _exit(99);
    } else if (pid > 0) {
      int status = 0;
      pid_t wr = waitpid(pid, &status, 0);
      if (wr == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        marker("M12-SMOKE: ok execve\n");
      } else {
        marker("M12-SMOKE: fail execve\n");
      }
    } else {
      marker("M12-SMOKE: fail fork\n");
    }
  }

  /* 3. Child exit status propagation */
  {
    pid_t pid = fork();
    if (pid == 0) {
      _exit(42);
    } else if (pid > 0) {
      int status = 0;
      pid_t wr = waitpid(pid, &status, 0);
      if (wr == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42) {
        marker("M12-SMOKE: ok status-prop\n");
      } else {
        marker("M12-SMOKE: fail status-prop\n");
      }
    } else {
      marker("M12-SMOKE: fail fork2\n");
    }
  }

  /* 3b. waitpid: targeted wait returns the right pid; ECHILD when no child
   * matches; WNOHANG returns 0 while the child is still alive. */
  {
    int wp_ok = 1;
    pid_t none = waitpid(-1, 0, WNOHANG);
    if (!(none == -1 && errno == ECHILD))
      wp_ok = 0; /* nothing outstanding: must be ECHILD */
    pid_t pid = fork();
    if (pid == 0) {
      for (volatile int i = 0; i < 2000000; i++)
        ;
      _exit(9);
    } else if (pid > 0) {
      int status = 0;
      /* WNOHANG on a live child: 0 (possibly already exited on SMP → pid). */
      pid_t nh = waitpid(pid, &status, WNOHANG);
      if (nh != 0 && nh != pid)
        wp_ok = 0;
      if (nh != pid) {
        pid_t wr = waitpid(pid, &status, 0);
        if (!(wr == pid && WIFEXITED(status) && WEXITSTATUS(status) == 9))
          wp_ok = 0;
      } else if (!(WIFEXITED(status) && WEXITSTATUS(status) == 9)) {
        wp_ok = 0;
      }
    } else {
      wp_ok = 0;
    }
    marker(wp_ok ? "M12-SMOKE: ok waitpid\n" : "M12-SMOKE: fail waitpid\n");
  }

  /* 4. Repeated spawn/wait stress */
  {
    int stress_ok = 1;
    for (int i = 0; i < 20; i++) {
      pid_t pid = fork();
      if (pid == 0) {
        _exit(0);
      } else if (pid > 0) {
        int status = 0;
        pid_t wr = waitpid(pid, &status, 0);
        if (wr != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
          stress_ok = 0;
          break;
        }
      } else {
        stress_ok = 0;
        break;
      }
    }
    marker(stress_ok ? "M12-SMOKE: ok stress\n" : "M12-SMOKE: fail stress\n");
  }

  /* 5. Zombie reaping behavior */
  {
    pid_t pid = fork();
    if (pid == 0) {
      _exit(0);
    } else if (pid > 0) {
      usleep(5000); /* let child become zombie */
      int status = 0;
      pid_t wr = waitpid(pid, &status, 0);
      if (wr == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        marker("M12-SMOKE: ok zombie\n");
      } else {
        marker("M12-SMOKE: fail zombie\n");
      }
    } else {
      marker("M12-SMOKE: fail fork3\n");
    }
  }

  /* 6. File descriptor inheritance */
  {
    int inh_fd = open("/tmp/m12_inherit.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (inh_fd >= 0) {
      pid_t pid = fork();
      if (pid == 0) {
        dup2(inh_fd, 1);
        close(inh_fd);
        write(1, "Hello from B1NIX\n", 17);
        _exit(0);
      } else if (pid > 0) {
        close(inh_fd);
        int status = 0;
        waitpid(pid, &status, 0);
        int read_fd = open("/tmp/m12_inherit.txt", O_RDONLY);
        if (read_fd >= 0) {
          char buf[128];
          memset(buf, 0, sizeof(buf));
          read(read_fd, buf, sizeof(buf) - 1);
          close(read_fd);
          if (strstr(buf, "Hello from B1NIX") != NULL) {
            marker("M12-SMOKE: ok fd-inheritance\n");
          } else {
            marker("M12-SMOKE: fail fd-inheritance\n");
          }
        } else {
          marker("M12-SMOKE: fail fd-inheritance open\n");
        }
      } else {
        marker("M12-SMOKE: fail fork4\n");
      }
    } else {
      marker("M12-SMOKE: fail inherit open\n");
    }
  }

  /* 7. dup2 behavior */
  {
    int dup_fd = dup2(1, 10);
    if (dup_fd == 10) {
      write(10, "", 0);
      marker("M12-SMOKE: ok dup2\n");
      close(10);
    } else {
      marker("M12-SMOKE: fail dup2\n");
    }
  }

  /* 8. close-on-exec */
  {
    int coe_fd = open("/tmp/coe.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (coe_fd >= 0) {
      int fl = fcntl(coe_fd, F_GETFD);
      int set_rc = fcntl(coe_fd, F_SETFD, fl | FD_CLOEXEC);
      int fl2 = fcntl(coe_fd, F_GETFD);
      close(coe_fd);
      if (fl >= 0 && set_rc == 0 && (fl2 & FD_CLOEXEC)) {
        marker("M12-SMOKE: ok close-on-exec\n");
      } else {
        marker("M12-SMOKE: fail close-on-exec\n");
      }
    } else {
      marker("M12-SMOKE: fail coe open\n");
    }
  }

  /* 9. brk growth/shrink sanity. musl's sbrk() deliberately fails for any
   * nonzero increment (its malloc never uses brk), so exercise the kernel's
   * brk through the raw syscall, which keeps Linux semantics: brk(addr)
   * returns the new break on success, the old break on failure. */
  {
    unsigned long start = (unsigned long)syscall(SYS_brk, 0);
    if (start != 0 && start != (unsigned long)-1) {
      unsigned long grown = (unsigned long)syscall(SYS_brk, start + 4096);
      if (grown >= start + 4096) {
        volatile char *ptr = (volatile char *)start;
        ptr[0] = 'X';
        if (ptr[0] == 'X') {
          syscall(SYS_brk, start);
          marker("M12-SMOKE: ok brk\n");
        } else {
          marker("M12-SMOKE: fail brk write\n");
        }
      } else {
        marker("M12-SMOKE: fail brk grow\n");
      }
    } else {
      marker("M12-SMOKE: fail brk get\n");
    }
  }

  /* 10. mmap/munmap mapping lifecycle */
  {
    void *map_ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map_ptr != MAP_FAILED) {
      volatile char *m = (volatile char *)map_ptr;
      m[0] = 'Y';
      if (m[0] == 'Y') {
        int rc = munmap(map_ptr, 4096);
        if (rc == 0) {
          marker("M12-SMOKE: ok mmap\n");
        } else {
          marker("M12-SMOKE: fail munmap\n");
        }
      } else {
        marker("M12-SMOKE: fail mmap write\n");
      }
    } else {
      marker("M12-SMOKE: fail mmap\n");
    }
  }

  /* 10b. mremap: growing a mapping keeps what was in it.
   *
   * musl's realloc calls this for every block big enough to have been mmap'd,
   * so a kernel without it cannot grow a large buffer — which is how a package
   * manager came to fail on exactly the packages over a megabyte. The check is
   * that the bytes survive the move, not merely that the call returns. */
  {
    size_t small = 64 * 1024, big = 512 * 1024;
    char *p = mmap(NULL, small, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (p == MAP_FAILED) {
      marker("M12-SMOKE: fail mremap-setup\n");
    } else {
      for (size_t i = 0; i < small; i++)
        p[i] = (char)(i & 0x7f);
      char *grown = mremap(p, small, big, MREMAP_MAYMOVE);
      if (grown == MAP_FAILED) {
        marker("M12-SMOKE: fail mremap-grow\n");
        munmap(p, small);
      } else {
        int intact = 1;
        for (size_t i = 0; i < small; i++)
          if (grown[i] != (char)(i & 0x7f))
            intact = 0;
        grown[big - 1] = 'Z';
        if (!intact)
          marker("M12-SMOKE: fail mremap-contents\n");
        else if (grown[big - 1] != 'Z')
          marker("M12-SMOKE: fail mremap-tail\n");
        else {
          char *shrunk = mremap(grown, big, small, 0);
          if (shrunk == MAP_FAILED || shrunk[0] != 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "M12-SMOKE: fail mremap-shrink (%d)\n",
                     shrunk == MAP_FAILED ? errno : -1);
            marker(msg);
          }
          else
            marker("M12-SMOKE: ok mremap\n");
          munmap(shrunk == MAP_FAILED ? grown : shrunk, small);
        }
      }
    }
  }

  /* 11. Invalid pointer/argument handling */
  {
    errno = 0;
    ssize_t rc_fault = write(1, (void *)-1, 10);
    int err_fault = errno;
    errno = 0;
    ssize_t rc_badf = read(-1, NULL, 10);
    int err_badf = errno;
    if (rc_fault == -1 && err_fault == EFAULT && rc_badf == -1 && err_badf == EBADF) {
      marker("M12-SMOKE: ok invalid-args\n");
    } else {
      marker("M12-SMOKE: fail invalid-args\n");
    }
  }

  /* 12. kill / signals basic */
  {
    pid_t pid = fork();
    if (pid == 0) {
      while (1) {
        usleep(100000); /* sleep 100ms, parent will kill us */
      }
      _exit(0);
    } else if (pid > 0) {
      int rc_kill = kill(pid, SIGKILL);
      if (rc_kill == 0) {
        int status = 0;
        pid_t wr = waitpid(pid, &status, 0);
        if (wr == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
          marker("M12-SMOKE: ok kill\n");
        } else {
          marker("M12-SMOKE: fail kill status\n");
        }
      } else {
        marker("M12-SMOKE: fail kill syscall\n");
      }
    } else {
      marker("M12-SMOKE: fail fork6\n");
    }
  }

  /* 13. sigaction (ignoring a signal) */
  {
    struct sigaction act;
    struct sigaction old;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_IGN;
    int rc_sig = sigaction(SIGTERM, &act, &old);
    if (rc_sig == 0) {
      pid_t self_pid = getpid();
      kill(self_pid, SIGTERM);
      usleep(50000); /* let signal process */
      act.sa_handler = SIG_DFL;
      sigaction(SIGTERM, &act, NULL);
      marker("M12-SMOKE: ok sigaction\n");
    } else {
      marker("M12-SMOKE: fail sigaction\n");
    }
  }

  /* 14. setsid / process groups */
  {
    pid_t new_sid = setsid();
    if (new_sid >= 0) {
      pid_t new_pgrp = getpgrp();
      if (new_pgrp == new_sid) {
        marker("M12-SMOKE: ok setsid-pgrp\n");
      } else {
        marker("M12-SMOKE: fail setsid-pgrp mismatch\n");
      }
    } else {
      marker("M12-SMOKE: fail setsid-pgrp setsid\n");
    }
  }

  /* 15. uid/gid getters/setters sanity */
  {
    uid_t uid = getuid();
    gid_t gid = getgid();
    uid_t euid = geteuid();
    gid_t egid = getegid();
    int rc_uid = setuid(uid);
    int rc_gid = setgid(gid);
    if (uid >= 0 && gid >= 0 && euid >= 0 && egid >= 0 &&
        rc_uid == 0 && rc_gid == 0) {
      marker("M12-SMOKE: ok uid-gid\n");
    } else {
      marker("M12-SMOKE: fail uid-gid\n");
    }
  }

  marker("M12-SMOKE: done\n");
  return 0;
}
