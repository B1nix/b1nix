#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <signal.h>
#include "types.h"
#include "syscall.h"

#define B1NIX_SIGINT 9
#define B1NIX_SIGKILL 10
#define B1NIX_SIGTERM 15

#define FD_CLOEXEC 1

#define WIFEXITED(status) (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WIFSIGNALED(status) (((status) & 0x7f) != 0)
#define WTERMSIG(status) ((status) & 0x7f)

// Helper: print string directly to stdout using raw write to avoid libc buffering issues during fork/exec
void print_marker(const char *marker) {
  write(1, marker, strlen(marker));
}

int main(int argc, char **argv) {
  // If invoked with check-close-on-exec argument:
  if (argc > 2 && strcmp(argv[1], "check-close-on-exec") == 0) {
    int fd = atoi(argv[2]);
    char buf[10];
    isize rc = syscall(SYS_READ, fd, buf, sizeof(buf));
    if (rc < 0) {
      // Failed to read, which is expected since it was closed on exec!
      return 0;
    }
    // If it succeeded or returned 0, then close-on-exec did not work.
    return 1;
  }

  print_marker("M12-SMOKE: start\n");

  // 1. Spawn a basic command
  int pid = syscall(SYS_SPAWN, "/bin/true", 0, NULL);
  if (pid >= 0) {
    print_marker("M12-SMOKE: ok spawn\n");
    int status = 0;
    int wr = syscall(SYS_WAITPID, pid, &status, 0);
    if (wr == pid) {
      print_marker("M12-SMOKE: ok waitpid\n");
    } else {
      print_marker("M12-SMOKE: fail waitpid\n");
    }
  } else {
    print_marker("M12-SMOKE: fail spawn\n");
  }

  // 2. Fork + Execve
  int fork_pid = syscall(SYS_FORK);
  if (fork_pid == 0) {
    char *exec_argv[] = {"/bin/true", NULL};
    char *exec_envp[] = {NULL};
    syscall(SYS_EXECVE, "/bin/true", exec_argv, exec_envp);
    // If execve fails
    syscall(SYS_EXIT, 99);
  } else if (fork_pid > 0) {
    int status = 0;
    int wr = syscall(SYS_WAITPID, fork_pid, &status, 0);
    if (wr == fork_pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      print_marker("M12-SMOKE: ok execve\n");
    } else {
      print_marker("M12-SMOKE: fail execve\n");
    }
  } else {
    print_marker("M12-SMOKE: fail fork\n");
  }

  // 3. Child exit status propagation
  int fork_pid2 = syscall(SYS_FORK);
  if (fork_pid2 == 0) {
    syscall(SYS_EXIT, 42);
  } else if (fork_pid2 > 0) {
    int status = 0;
    int wr = syscall(SYS_WAITPID, fork_pid2, &status, 0);
    if (wr == fork_pid2 && WIFEXITED(status) && WEXITSTATUS(status) == 42) {
      print_marker("M12-SMOKE: ok status-prop\n");
    } else {
      print_marker("M12-SMOKE: fail status-prop\n");
    }
  } else {
    print_marker("M12-SMOKE: fail fork2\n");
  }

  // 4. Repeated spawn/wait stress
  int stress_ok = 1;
  for (int i = 0; i < 20; i++) {
    int sp_pid = syscall(SYS_SPAWN, "/bin/true", 0, NULL);
    if (sp_pid < 0) {
      stress_ok = 0;
      break;
    }
    int status = 0;
    int wr = syscall(SYS_WAITPID, sp_pid, &status, 0);
    if (wr != sp_pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      stress_ok = 0;
      break;
    }
  }
  if (stress_ok) {
    print_marker("M12-SMOKE: ok stress\n");
  } else {
    print_marker("M12-SMOKE: fail stress\n");
  }

  // 5. Zombie reaping behavior
  int fork_pid3 = syscall(SYS_FORK);
  if (fork_pid3 == 0) {
    // exit immediately to become a zombie
    syscall(SYS_EXIT, 0);
  } else if (fork_pid3 > 0) {
    // yield a few times to let the child exit and become a zombie
    for (int i = 0; i < 5; i++) {
      syscall(SYS_YIELD);
    }
    // now wait on it (reap it)
    int status = 0;
    int wr = syscall(SYS_WAITPID, fork_pid3, &status, 0);
    if (wr == fork_pid3 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      print_marker("M12-SMOKE: ok zombie\n");
    } else {
      print_marker("M12-SMOKE: fail zombie\n");
    }
  } else {
    print_marker("M12-SMOKE: fail fork3\n");
  }

  // 6. File descriptor inheritance
  int inh_fd = open("/tmp/m12_inherit.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (inh_fd >= 0) {
    int fork_pid4 = syscall(SYS_FORK);
    if (fork_pid4 == 0) {
      syscall(SYS_DUP2, inh_fd, 1);
      close(inh_fd);
      write(1, "Hello from B1NIX\n", 17);
      syscall(SYS_EXIT, 0);
    } else if (fork_pid4 > 0) {
      close(inh_fd);
      int status = 0;
      syscall(SYS_WAITPID, fork_pid4, &status, 0);
      int read_fd = open("/tmp/m12_inherit.txt", O_RDONLY);
      if (read_fd >= 0) {
        char buf[128];
        memset(buf, 0, sizeof(buf));
        read(read_fd, buf, sizeof(buf) - 1);
        close(read_fd);
        if (strstr(buf, "Hello from B1NIX") != NULL) {
          print_marker("M12-SMOKE: ok fd-inheritance\n");
        } else {
          print_marker("M12-SMOKE: fail fd-inheritance\n");
        }
      } else {
        print_marker("M12-SMOKE: fail fd-inheritance open\n");
      }
    } else {
      print_marker("M12-SMOKE: fail fork4\n");
    }
  } else {
    print_marker("M12-SMOKE: fail inherit open\n");
  }

  // 7. dup2 behavior
  int dup_fd = syscall(SYS_DUP2, 1, 10);
  if (dup_fd == 10) {
    write(10, "", 0); // test writing empty string to make sure it doesn't fail
    print_marker("M12-SMOKE: ok dup2\n");
    close(10);
  } else {
    print_marker("M12-SMOKE: fail dup2\n");
  }

  // 8. close-on-exec
  int coe_fd = open("/tmp/coe.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (coe_fd >= 0) {
    int fl = syscall(SYS_FCNTL, coe_fd, F_GETFD, 0);
    int set_rc = syscall(SYS_FCNTL, coe_fd, F_SETFD, fl | FD_CLOEXEC);
    int fl2 = syscall(SYS_FCNTL, coe_fd, F_GETFD, 0);
    close(coe_fd);
    if (fl >= 0 && set_rc == 0 && (fl2 & FD_CLOEXEC)) {
      print_marker("M12-SMOKE: ok close-on-exec\n");
    } else {
      print_marker("M12-SMOKE: fail close-on-exec\n");
    }
  } else {
    print_marker("M12-SMOKE: fail coe open\n");
  }

  // 9. brk growth/shrink sanity
  u64 start_brk = syscall(SYS_BRK, 0);
  if (start_brk > 0) {
    u64 new_brk = syscall(SYS_BRK, start_brk + 4096);
    if (new_brk == start_brk + 4096) {
      // Write to new memory
      volatile char *ptr = (volatile char *)(start_brk);
      ptr[0] = 'X';
      if (ptr[0] == 'X') {
        u64 shrunk_brk = syscall(SYS_BRK, start_brk);
        if (shrunk_brk == start_brk) {
          print_marker("M12-SMOKE: ok brk\n");
        } else {
          print_marker("M12-SMOKE: fail brk shrink\n");
        }
      } else {
        print_marker("M12-SMOKE: fail brk write\n");
      }
    } else {
      print_marker("M12-SMOKE: fail brk grow\n");
    }
  } else {
    print_marker("M12-SMOKE: fail brk get\n");
  }

  // 10. mmap/munmap mapping lifecycle
  void *map_ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (map_ptr != MAP_FAILED) {
    volatile char *m = (volatile char *)map_ptr;
    m[0] = 'Y';
    if (m[0] == 'Y') {
      int rc = munmap(map_ptr, 4096);
      if (rc == 0) {
        print_marker("M12-SMOKE: ok mmap\n");
      } else {
        print_marker("M12-SMOKE: fail munmap\n");
      }
    } else {
      print_marker("M12-SMOKE: fail mmap write\n");
    }
  } else {
    print_marker("M12-SMOKE: fail mmap\n");
  }

  // 11. Invalid pointer/argument handling
  isize rc_fault = syscall(SYS_WRITE, 1, (void *)-1, 10);
  isize rc_badf = syscall(SYS_READ, -1, NULL, 10);
  if (rc_fault == -EFAULT && rc_badf == -EBADF) {
    print_marker("M12-SMOKE: ok invalid-args\n");
  } else {
    print_marker("M12-SMOKE: fail invalid-args\n");
  }

  // 12. kill / signals basic
  int fork_pid6 = syscall(SYS_FORK);
  if (fork_pid6 == 0) {
    // Child: loop sleeping/yielding
    while (1) {
      syscall(SYS_YIELD);
    }
    syscall(SYS_EXIT, 0);
  } else if (fork_pid6 > 0) {
    // kill the child
    int rc_kill = syscall(SYS_KILL, fork_pid6, B1NIX_SIGKILL);
    if (rc_kill == 0) {
      int status = 0;
      int wr = syscall(SYS_WAITPID, fork_pid6, &status, 0);
      if (wr == fork_pid6 && WIFSIGNALED(status) && WTERMSIG(status) == B1NIX_SIGKILL) {
        print_marker("M12-SMOKE: ok kill\n");
      } else {
        print_marker("M12-SMOKE: fail kill status\n");
      }
    } else {
      print_marker("M12-SMOKE: fail kill syscall\n");
    }
  } else {
    print_marker("M12-SMOKE: fail fork6\n");
  }

  // 13. sigaction (ignoring a signal)
  struct sigaction act;
  struct sigaction old;
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_IGN;
  int rc_sig = syscall(SYS_SIGNAL, B1NIX_SIGTERM, &act, &old);
  if (rc_sig == 0) {
    // Send SIGTERM to ourselves; it should be ignored!
    int self_pid = syscall(SYS_GETPID);
    syscall(SYS_KILL, self_pid, B1NIX_SIGTERM);
    // Yield a few times to let it process
    for (int i = 0; i < 5; i++) {
      syscall(SYS_YIELD);
    }
    // Restore default action
    act.sa_handler = SIG_DFL;
    syscall(SYS_SIGNAL, B1NIX_SIGTERM, &act, NULL);
    print_marker("M12-SMOKE: ok sigaction\n");
  } else {
    print_marker("M12-SMOKE: fail sigaction\n");
  }

  // 14. setsid / process groups
  int old_pgrp = syscall(SYS_GETPGRP);
  (void)old_pgrp;
  int new_sid = syscall(SYS_SETSID);
  if (new_sid >= 0) {
    int new_pgrp = syscall(SYS_GETPGRP);
    // After setsid, pgrp should match sid
    if (new_pgrp == new_sid) {
      print_marker("M12-SMOKE: ok setsid-pgrp\n");
    } else {
      print_marker("M12-SMOKE: fail setsid-pgrp mismatch\n");
    }
  } else {
    // setsid might return EPERM if we are already group leader. 
    // In our test, init spawns us, so we are a new process and not a group leader.
    print_marker("M12-SMOKE: fail setsid-pgrp setsid\n");
  }

  // 15. uid/gid getters/setters sanity
  int uid = syscall(SYS_GETUID);
  int gid = syscall(SYS_GETGID);
  int euid = syscall(SYS_GETEUID);
  int egid = syscall(SYS_GETEGID);
  // Try to set to same uid/gid (should be allowed)
  int rc_uid = syscall(SYS_SETUID, uid);
  int rc_gid = syscall(SYS_SETGID, gid);
  if (uid >= 0 && gid >= 0 && euid >= 0 && egid >= 0 && rc_uid == 0 && rc_gid == 0) {
    print_marker("M12-SMOKE: ok uid-gid\n");
  } else {
    print_marker("M12-SMOKE: fail uid-gid\n");
  }

  print_marker("M12-SMOKE: done\n");
  return 0;
}
