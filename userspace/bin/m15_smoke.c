#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h> /* struct itimerspec */
#include <time.h>
#include <unistd.h>

#include "syscall.h"
#include "types.h"

#define B1NIX_SIGTERM 15
#define B1NIX_SIGUSR1 19

#define WIFEXITED(status) (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)

static volatile int g_sigusr1_hits = 0;
static char g_dmesg_after[4096];

extern void __sig_restorer(void);

static void marker(const char *text) { write(1, text, strlen(text)); }

static void sigusr1_handler(int sig) {
  (void)sig;
  g_sigusr1_hits++;
}

/* M74: RT-signal queueing. A standard signal coalesces (N sends while blocked =
 * 1 delivery); an RT signal QUEUES (N sends = N deliveries). */
static volatile int g_rt_hits;
static void rt_handler(int sig) {
  (void)sig;
  g_rt_hits++;
}

/* M74: SA_SIGINFO RT handler — records the sigqueue payload (si_value) of each
 * delivery to verify FIFO order and that the payload survives delivery. */
static volatile int g_rt_vals[4];
static volatile int g_rt_n;
static void rt_si_handler(int sig, siginfo_t *si, void *uc) {
  (void)sig;
  (void)uc;
  if (si && g_rt_n < 4)
    g_rt_vals[g_rt_n++] = si->si_value.sival_int;
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  marker("M15-SMOKE: start\n");

  /* 1) Signals: baseline install path. */
  struct sigaction act;
  struct sigaction old_act;
  memset(&act, 0, sizeof(act));
  memset(&old_act, 0, sizeof(old_act));
  act.sa_handler = sigusr1_handler;
  act.sa_flags = 0;
  act.sa_restorer = __sig_restorer;
  if ((int)syscall(SYS_SIGNAL, B1NIX_SIGUSR1, &act, &old_act) == 0) {
    marker("M15-SMOKE: ok signal-baseline\n");
  } else {
    marker("M15-SMOKE: fail signal-baseline\n");
  }

  /* 2) Signal handler delivery: kill(self, SIGUSR1) + scheduler yield loop. */
  int self_pid = (int)syscall(SYS_GETPID);
  int kill_rc = (int)syscall(SYS_KILL, self_pid, B1NIX_SIGUSR1);
  for (int i = 0; i < 16 && g_sigusr1_hits == 0; i++) {
    syscall(SYS_YIELD);
  }
  if (kill_rc == 0 && g_sigusr1_hits > 0) {
    marker("M15-SMOKE: ok signal-handler\n");
  } else {
    marker("M15-SMOKE: fail signal-handler\n");
  }

  /* 2.1) sigprocmask: blocked signal must remain pending until unblocked. */
  sigset_t set = 0;
  sigemptyset(&set);
  sigaddset(&set, B1NIX_SIGUSR1);
  int mask_ok = 1;
  int before_hits = g_sigusr1_hits;
  if (sigprocmask(SIG_BLOCK, &set, NULL) != 0) {
    mask_ok = 0;
  }
  if ((int)syscall(SYS_KILL, self_pid, B1NIX_SIGUSR1) != 0) {
    mask_ok = 0;
  }
  for (int i = 0; i < 8; i++) {
    syscall(SYS_YIELD);
  }
  if (g_sigusr1_hits != before_hits) {
    mask_ok = 0;
  }
  if (sigprocmask(SIG_UNBLOCK, &set, NULL) != 0) {
    mask_ok = 0;
  }
  for (int i = 0; i < 16 && g_sigusr1_hits == before_hits; i++) {
    syscall(SYS_YIELD);
  }
  if (g_sigusr1_hits == before_hits) {
    mask_ok = 0;
  }
  if (mask_ok) {
    marker("M15-SMOKE: ok signal-mask\n");
  } else {
    marker("M15-SMOKE: fail signal-mask\n");
  }

  /* 3) SIG_IGN behavior. */
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_IGN;
  act.sa_restorer = __sig_restorer;
  if ((int)syscall(SYS_SIGNAL, B1NIX_SIGTERM, &act, NULL) == 0) {
    syscall(SYS_KILL, self_pid, B1NIX_SIGTERM);
    for (int i = 0; i < 5; i++) {
      syscall(SYS_YIELD);
    }
    marker("M15-SMOKE: ok signal-ignore\n");
  } else {
    marker("M15-SMOKE: fail signal-ignore setup\n");
  }

  /* 4) IPC message queue roundtrip. */
  int mq = (int)syscall(SYS_MQ_OPEN, "/m15_q");
  if (mq >= 0) {
    const char *msg = "m15-test-msg";
    int send_rc = (int)syscall(SYS_MQ_SEND, mq, msg, strlen(msg));
    char buf[32];
    unsigned int rx_len = 0;
    memset(buf, 0, sizeof(buf));
    int recv_rc = (int)syscall(SYS_MQ_RECEIVE, mq, buf, &rx_len);
    syscall(SYS_MQ_CLOSE, mq);
    int unlink_rc = (int)syscall(SYS_MQ_UNLINK, "/m15_q");
    if (send_rc == 0 && recv_rc == 0 && rx_len == strlen(msg) &&
        strcmp(buf, msg) == 0 && unlink_rc == 0) {
      marker("M15-SMOKE: ok ipc-mq\n");
    } else {
      marker("M15-SMOKE: fail ipc-mq\n");
    }
  } else {
    marker("M15-SMOKE: fail ipc-mq open\n");
  }

  /* 5) Shared memory semantics. */
  int shmid = (int)syscall(SYS_SHMGET, 0x9999, 4096, 0x1000 | 0666);
  if (shmid >= 0) {
    int *shm = (int *)syscall(SYS_SHMAT, shmid, NULL, 0);
    if (shm != (void *)-1) {
      shm[0] = 42;
      int seen = shm[0];
      int dt_rc = (int)syscall(SYS_SHMDT, shm);
      int rm_rc = (int)syscall(SYS_SHMCTL, shmid, 0, NULL);
      if (seen == 42 && dt_rc == 0 && rm_rc == 0) {
        marker("M15-SMOKE: ok shm\n");
      } else {
        marker("M15-SMOKE: fail shm\n");
      }
    } else {
      marker("M15-SMOKE: fail shm attach\n");
    }
  } else {
    marker("M15-SMOKE: fail shmget\n");
  }

  /* 5b) shm cleanup on exit: a child that attaches and exits WITHOUT shmdt
   * must still have its attachment released, so the creator can IPC_RMID.
   * Before the fix shm_nattch stayed 1 forever (leak) and IPC_RMID failed. */
  int shmid2 = (int)syscall(SYS_SHMGET, 0x9998, 4096, 0x1000 | 0666);
  if (shmid2 >= 0) {
    int cpid = (int)syscall(SYS_FORK);
    if (cpid == 0) {
      int *s = (int *)syscall(SYS_SHMAT, shmid2, NULL, 0);
      if (s != (void *)-1)
        s[0] = 7;
      syscall(SYS_EXIT, 0); /* no SHMDT — exercises shm_detach_all on exit */
    } else if (cpid > 0) {
      int status = 0;
      syscall(SYS_WAITPID, cpid, &status, 0);
      int rm = (int)syscall(SYS_SHMCTL, shmid2, 0, NULL); /* IPC_RMID */
      if (rm == 0) {
        marker("M15-SMOKE: ok shm-exit-cleanup\n");
      } else {
        marker("M15-SMOKE: fail shm-exit-cleanup\n");
      }
    } else {
      marker("M15-SMOKE: fail shm-exit-cleanup fork\n");
    }
  } else {
    marker("M15-SMOKE: fail shm-exit-cleanup shmget\n");
  }

  /* 5c) shm cleanup when a child is SIGKILL'd (OOM-killer path) + fork
   * nattch accounting. Parent attaches; the child inherits that attach across
   * fork (shm_fork_inherit bumps shm_nattch), signals via the shared page,
   * then loops until killed. After the child is reaped and the parent detaches
   * its own attach, IPC_RMID must succeed — proving the killed child's
   * attachment was accounted (it never called shmdt). */
  int shmid3 = (int)syscall(SYS_SHMGET, 0x9997, 4096, 0x1000 | 0666);
  if (shmid3 >= 0) {
    volatile int *sp = (volatile int *)syscall(SYS_SHMAT, shmid3, NULL, 0);
    if (sp != (volatile int *)-1) {
      sp[0] = 0;
      int kpid = (int)syscall(SYS_FORK);
      if (kpid == 0) {
        sp[0] = 1;                       /* inherited attach + running */
        for (;;) syscall(SYS_YIELD);     /* wait to be killed */
      } else if (kpid > 0) {
        while (sp[0] == 0) syscall(SYS_YIELD); /* child attached and ran */
        syscall(SYS_KILL, kpid, SIGKILL);
        int status = 0;
        syscall(SYS_WAITPID, kpid, &status, 0);
        syscall(SYS_SHMDT, (void *)sp);  /* drop the parent's own attach */
        int rm = (int)syscall(SYS_SHMCTL, shmid3, 0, NULL); /* IPC_RMID */
        if (rm == 0) {
          marker("M15-SMOKE: ok shm-kill-cleanup\n");
        } else {
          marker("M15-SMOKE: fail shm-kill-cleanup\n");
        }
      } else {
        marker("M15-SMOKE: fail shm-kill-cleanup fork\n");
      }
    } else {
      marker("M15-SMOKE: fail shm-kill-cleanup attach\n");
    }
  } else {
    marker("M15-SMOKE: fail shm-kill-cleanup shmget\n");
  }

  /* 6) Cooperative userspace semaphore baseline (no kernel futex yet). */
  int sem = 0;
  int sem_ok = 0;
  if (sem_init(&sem, 0, 0) == 0) {
    if (sem_post(&sem) == 0 && sem_wait(&sem) == 0 && sem == 0 &&
        sem_destroy(&sem) == 0) {
      sem_ok = 1;
    }
  }
  if (sem_ok) {
    marker("M15-SMOKE: ok semaphore\n");
  } else {
    marker("M15-SMOKE: fail semaphore\n");
  }

  /* 7) Clocks and timers. */
  struct timespec t1;
  struct timespec t2;
  t1.tv_sec = t1.tv_nsec = 0;
  t2.tv_sec = t2.tv_nsec = 0;
  if (clock_gettime(CLOCK_MONOTONIC, &t1) == 0) {
    struct timespec req = {0, 150000000};
    nanosleep(&req, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    long diff_ms =
        (t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_nsec - t1.tv_nsec) / 1000000;
    if (diff_ms >= 100 && diff_ms <= 350) {
      marker("M15-SMOKE: ok clock-timer\n");
    } else {
      marker("M15-SMOKE: fail clock-timer\n");
    }
  } else {
    marker("M15-SMOKE: fail clock_gettime\n");
  }

  /* 8) Permissions: chmod effect and real enforcement for non-root uid. */
  int fd_perm = open("/tmp/m15_perm.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (fd_perm >= 0) {
    write(fd_perm, "test", 4);
    close(fd_perm);
    int chmod_rc = (int)syscall(SYS_CHMOD, "/tmp/m15_perm.txt", 0400);
    if (chmod_rc == 0) {
      marker("M15-SMOKE: ok permissions-chmod\n");
    } else {
      marker("M15-SMOKE: fail permissions-chmod\n");
    }

    memset(g_dmesg_after, 0, sizeof(g_dmesg_after));
    int su0 = setuid(0);
    isize audit_n =
        syscall(SYS_DMESG, g_dmesg_after, sizeof(g_dmesg_after) - 1);
    if (su0 == 0 && audit_n > 0 &&
        strstr(g_dmesg_after, "audit: setuid called") != NULL &&
        strstr(g_dmesg_after, "audit: chmod called") != NULL) {
      marker("M15-SMOKE: ok audit-logging\n");
    } else {
      marker("M15-SMOKE: fail audit-logging\n");
    }

    int deny_ok = 0;
    int pid = (int)syscall(SYS_FORK);
    if (pid == 0) {
      int su = setuid(1000);
      int rfd = open("/tmp/m15_perm.txt", O_RDONLY);
      if (su == 0 && rfd < 0) {
        syscall(SYS_EXIT, 0);
      }
      if (rfd >= 0) {
        close(rfd);
      }
      syscall(SYS_EXIT, 1);
    } else if (pid > 0) {
      int status = 0;
      syscall(SYS_WAITPID, pid, &status, 0);
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        deny_ok = 1;
      }
    }

    syscall(SYS_UNLINK, "/tmp/m15_perm.txt");
    if (deny_ok) {
      marker("M15-SMOKE: ok permissions-enforcement\n");
    } else {
      marker("M15-SMOKE: fail permissions-enforcement\n");
    }
  } else {
    marker("M15-SMOKE: fail permissions file setup\n");
  }

  /* M74: RT-signal queueing — block SIGRTMIN, raise it 3x, unblock, and the
   * handler must run 3 times (a standard signal would coalesce to 1). */
  {
    struct sigaction rtact;
    struct sigaction rtold;
    memset(&rtact, 0, sizeof(rtact));
    memset(&rtold, 0, sizeof(rtold));
    rtact.sa_handler = rt_handler;
    rtact.sa_restorer = __sig_restorer;
    int rt_ok = 0;
    if ((int)syscall(SYS_SIGNAL, SIGRTMIN, &rtact, &rtold) == 0) {
      sigset_t rtset;
      sigemptyset(&rtset);
      sigaddset(&rtset, SIGRTMIN);
      int self = (int)syscall(SYS_GETPID);
      sigprocmask(SIG_BLOCK, &rtset, NULL);
      g_rt_hits = 0;
      syscall(SYS_KILL, self, SIGRTMIN);
      syscall(SYS_KILL, self, SIGRTMIN);
      syscall(SYS_KILL, self, SIGRTMIN);
      sigprocmask(SIG_UNBLOCK, &rtset, NULL);
      for (int i = 0; i < 64 && g_rt_hits < 3; i++)
        syscall(SYS_YIELD);
      rt_ok = (g_rt_hits == 3);
    }
    if (rt_ok) {
      marker("M74-SMOKE: ok rt-queue\n");
    } else {
      char b[64];
      snprintf(b, sizeof(b), "M74-SMOKE: fail rt-queue hits=%d\n", g_rt_hits);
      marker(b);
    }
  }

  /* M74: sigqueue payload via SA_SIGINFO. Queue three distinct payloads on a
   * blocked RT signal; the handler must receive them in FIFO order with the
   * correct si_value. */
  {
    struct sigaction sa;
    struct sigaction old;
    memset(&sa, 0, sizeof(sa));
    memset(&old, 0, sizeof(old));
    sa.sa_sigaction = rt_si_handler;
    sa.sa_flags = SA_SIGINFO;
    sa.sa_restorer = __sig_restorer;
    int ok = 0;
    if ((int)syscall(SYS_SIGNAL, SIGRTMIN + 1, &sa, &old) == 0) {
      sigset_t s;
      sigemptyset(&s);
      sigaddset(&s, SIGRTMIN + 1);
      int self = (int)syscall(SYS_GETPID);
      sigprocmask(SIG_BLOCK, &s, NULL);
      g_rt_n = 0;
      union sigval v;
      v.sival_int = 11;
      sigqueue(self, SIGRTMIN + 1, v);
      v.sival_int = 22;
      sigqueue(self, SIGRTMIN + 1, v);
      v.sival_int = 33;
      sigqueue(self, SIGRTMIN + 1, v);
      sigprocmask(SIG_UNBLOCK, &s, NULL);
      for (int i = 0; i < 64 && g_rt_n < 3; i++)
        syscall(SYS_YIELD);
      ok = (g_rt_n == 3 && g_rt_vals[0] == 11 && g_rt_vals[1] == 22 &&
            g_rt_vals[2] == 33);
    }
    if (ok) {
      marker("M74-SMOKE: ok rt-sigqueue\n");
    } else {
      char b[96];
      snprintf(b, sizeof(b), "M74-SMOKE: fail rt-sigqueue n=%d v=%d,%d,%d\n",
               g_rt_n, g_rt_vals[0], g_rt_vals[1], g_rt_vals[2]);
      marker(b);
    }
  }

  /* M74: POSIX interval timer — the validating CONSUMER of RT signals. A 20 ms
   * periodic timer raises SIGRTMIN+2 carrying sigev_value 99; the SA_SIGINFO
   * handler must fire repeatedly with that payload. */
  {
    struct sigaction sa;
    struct sigaction old;
    memset(&sa, 0, sizeof(sa));
    memset(&old, 0, sizeof(old));
    sa.sa_sigaction = rt_si_handler;
    sa.sa_flags = SA_SIGINFO;
    sa.sa_restorer = __sig_restorer;
    int ok = 0;
    if ((int)syscall(SYS_SIGNAL, SIGRTMIN + 2, &sa, &old) == 0) {
      struct sigevent sev;
      memset(&sev, 0, sizeof(sev));
      sev.sigev_notify = SIGEV_SIGNAL;
      sev.sigev_signo = SIGRTMIN + 2;
      sev.sigev_value.sival_int = 99;
      timer_t tid;
      if (timer_create(CLOCK_MONOTONIC, &sev, &tid) == 0) {
        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        its.it_value.tv_nsec = 20000000;    /* first fire in 20 ms */
        its.it_interval.tv_nsec = 20000000; /* then every 20 ms */
        g_rt_n = 0;
        if (timer_settime(tid, 0, &its, NULL) == 0) {
          for (int i = 0; i < 80 && g_rt_n < 3; i++) {
            struct timespec ts = {0, 10000000}; /* 10 ms */
            nanosleep(&ts, NULL);
          }
        }
        timer_delete(tid);
        ok = (g_rt_n >= 3 && g_rt_vals[0] == 99 && g_rt_vals[1] == 99 &&
              g_rt_vals[2] == 99);
      }
    }
    if (ok) {
      marker("M74-SMOKE: ok rt-timer\n");
    } else {
      char b[96];
      snprintf(b, sizeof(b), "M74-SMOKE: fail rt-timer n=%d v=%d\n", g_rt_n,
               g_rt_vals[0]);
      marker(b);
    }
  }

  marker("M15-SMOKE: done\n");
  return 0;
}
