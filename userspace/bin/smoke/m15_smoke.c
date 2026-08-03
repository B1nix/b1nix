/*
 * m15_smoke — IPC, signals, permissions, POSIX timers, audit tests.
 * Rewritten to use POSIX API (no b1nix raw syscalls).
 */
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <signal.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile int g_sigusr1_hits = 0;

static void marker(const char *text) { write(1, text, strlen(text)); }

static void sigusr1_handler(int sig) {
  (void)sig;
  g_sigusr1_hits++;
}

/* M74: RT-signal queueing handler. */
static volatile int g_rt_hits;
static void rt_handler(int sig) {
  (void)sig;
  g_rt_hits++;
}

/* M74: SA_SIGINFO RT handler — records si_value payloads. */
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
  {
    struct sigaction act;
    struct sigaction old_act;
    memset(&act, 0, sizeof(act));
    memset(&old_act, 0, sizeof(old_act));
    act.sa_handler = sigusr1_handler;
    act.sa_flags = 0;
    if (sigaction(SIGUSR1, &act, &old_act) == 0) {
      marker("M15-SMOKE: ok signal-baseline\n");
    } else {
      marker("M15-SMOKE: fail signal-baseline\n");
    }
  }

  /* 2) Signal handler delivery: kill(self, SIGUSR1) + yield loop. */
  {
    pid_t self_pid = getpid();
    int kill_rc = kill(self_pid, SIGUSR1);
    for (int i = 0; i < 16 && g_sigusr1_hits == 0; i++)
      usleep(1000);
    if (kill_rc == 0 && g_sigusr1_hits > 0) {
      marker("M15-SMOKE: ok signal-handler\n");
    } else {
      marker("M15-SMOKE: fail signal-handler\n");
    }
  }

  /* 2.1) sigprocmask: blocked signal must remain pending until unblocked. */
  {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    int mask_ok = 1;
    int before_hits = g_sigusr1_hits;
    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0)
      mask_ok = 0;
    pid_t self = getpid();
    if (kill(self, SIGUSR1) != 0)
      mask_ok = 0;
    usleep(50000);
    if (g_sigusr1_hits != before_hits)
      mask_ok = 0;
    if (sigprocmask(SIG_UNBLOCK, &set, NULL) != 0)
      mask_ok = 0;
    for (int i = 0; i < 16 && g_sigusr1_hits == before_hits; i++)
      usleep(1000);
    if (g_sigusr1_hits == before_hits)
      mask_ok = 0;
    if (mask_ok)
      marker("M15-SMOKE: ok signal-mask\n");
    else
      marker("M15-SMOKE: fail signal-mask\n");
  }

  /* 3) SIG_IGN behavior. */
  {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_IGN;
    if (sigaction(SIGTERM, &act, NULL) == 0) {
      pid_t self = getpid();
      kill(self, SIGTERM);
      usleep(50000);
      marker("M15-SMOKE: ok signal-ignore\n");
    } else {
      marker("M15-SMOKE: fail signal-ignore setup\n");
    }
  }

  /* 4) IPC message queue roundtrip. */
  {
    mqd_t mq = mq_open("/m15_q", O_CREAT | O_RDWR, 0666, NULL);
    if (mq != (mqd_t)-1) {
      const char *msg = "m15-test-msg";
      int send_rc = mq_send(mq, msg, strlen(msg), 0);
      char buf[32];
      unsigned int rx_prio = 1;
      memset(buf, 0, sizeof(buf));
      /* mq_receive returns the message byte count; the 4th arg is the priority
       * (0 here, since mq_send used priority 0), NOT the length. */
      int recv_rc = mq_receive(mq, buf, sizeof(buf), &rx_prio);
      mq_close(mq);
      int unlink_rc = mq_unlink("/m15_q");
      if (send_rc == 0 && recv_rc == (int)strlen(msg) && rx_prio == 0 &&
          strcmp(buf, msg) == 0 && unlink_rc == 0) {
        marker("M15-SMOKE: ok ipc-mq\n");
      } else {
        marker("M15-SMOKE: fail ipc-mq\n");
      }
    } else {
      marker("M15-SMOKE: fail ipc-mq open\n");
    }
  }

  /* 5) Shared memory semantics. */
  {
    int shmid = shmget(0x9999, 4096, IPC_CREAT | 0666);
    if (shmid >= 0) {
      int *shm = (int *)shmat(shmid, NULL, 0);
      if (shm != (void *)-1) {
        shm[0] = 42;
        int seen = shm[0];
        int dt_rc = shmdt(shm);
        int rm_rc = shmctl(shmid, IPC_RMID, NULL);
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
  }

  /* 5b) shm cleanup on exit: child attaches and exits WITHOUT shmdt. */
  {
    int shmid2 = shmget(0x9998, 4096, IPC_CREAT | 0666);
    if (shmid2 >= 0) {
      pid_t cpid = fork();
      if (cpid == 0) {
        int *s = (int *)shmat(shmid2, NULL, 0);
        if (s != (void *)-1)
          s[0] = 7;
        _exit(0);
      } else if (cpid > 0) {
        int status = 0;
        waitpid(cpid, &status, 0);
        int rm = shmctl(shmid2, IPC_RMID, NULL);
        if (rm == 0)
          marker("M15-SMOKE: ok shm-exit-cleanup\n");
        else
          marker("M15-SMOKE: fail shm-exit-cleanup\n");
      } else {
        marker("M15-SMOKE: fail shm-exit-cleanup fork\n");
      }
    } else {
      marker("M15-SMOKE: fail shm-exit-cleanup shmget\n");
    }
  }

  /* 5c) shm cleanup when child is SIGKILL'd + fork nattch accounting. */
  {
    int shmid3 = shmget(0x9997, 4096, IPC_CREAT | 0666);
    if (shmid3 >= 0) {
      volatile int *sp = (volatile int *)shmat(shmid3, NULL, 0);
      if (sp != (volatile int *)-1) {
        sp[0] = 0;
        pid_t kpid = fork();
        if (kpid == 0) {
          sp[0] = 1;
          for (;;)
            usleep(100000);
        } else if (kpid > 0) {
          while (sp[0] == 0)
            usleep(1000);
          kill(kpid, SIGKILL);
          int status = 0;
          waitpid(kpid, &status, 0);
          shmdt((void *)sp);
          int rm = shmctl(shmid3, IPC_RMID, NULL);
          if (rm == 0)
            marker("M15-SMOKE: ok shm-kill-cleanup\n");
          else
            marker("M15-SMOKE: fail shm-kill-cleanup\n");
        } else {
          marker("M15-SMOKE: fail shm-kill-cleanup fork\n");
        }
      } else {
        marker("M15-SMOKE: fail shm-kill-cleanup attach\n");
      }
    } else {
      marker("M15-SMOKE: fail shm-kill-cleanup shmget\n");
    }
  }

  /* 6) Cooperative userspace semaphore baseline. */
  {
    sem_t sem;
    int sem_ok = 0;
    if (sem_init(&sem, 0, 0) == 0) {
      if (sem_post(&sem) == 0 && sem_wait(&sem) == 0 &&
          sem_destroy(&sem) == 0)
        sem_ok = 1;
    }
    marker(sem_ok ? "M15-SMOKE: ok semaphore\n"
                  : "M15-SMOKE: fail semaphore\n");
  }

  /* 7) Clocks and timers. */
  {
    struct timespec t1 = {0, 0};
    struct timespec t2 = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &t1) == 0) {
      struct timespec req = {0, 150000000};
      nanosleep(&req, NULL);
      clock_gettime(CLOCK_MONOTONIC, &t2);
      long diff_ms = (t2.tv_sec - t1.tv_sec) * 1000 +
                     (t2.tv_nsec - t1.tv_nsec) / 1000000;
      if (diff_ms >= 100 && diff_ms <= 350)
        marker("M15-SMOKE: ok clock-timer\n");
      else
        marker("M15-SMOKE: fail clock-timer\n");
    } else {
      marker("M15-SMOKE: fail clock_gettime\n");
    }
  }

  /* 8) Permissions: chmod effect and enforcement for non-root uid. */
  {
    int fd_perm =
        open("/tmp/m15_perm.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd_perm >= 0) {
      write(fd_perm, "test", 4);
      close(fd_perm);
      if (chmod("/tmp/m15_perm.txt", 0400) == 0)
        marker("M15-SMOKE: ok permissions-chmod\n");
      else
        marker("M15-SMOKE: fail permissions-chmod\n");

      int deny_ok = 0;
      pid_t pid = fork();
      if (pid == 0) {
        int su = setuid(1000);
        int rfd = open("/tmp/m15_perm.txt", O_RDONLY);
        if (su == 0 && rfd < 0)
          _exit(0);
        if (rfd >= 0)
          close(rfd);
        _exit(1);
      } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
          deny_ok = 1;
      }

      unlink("/tmp/m15_perm.txt");
      if (deny_ok)
        marker("M15-SMOKE: ok permissions-enforcement\n");
      else
        marker("M15-SMOKE: fail permissions-enforcement\n");
    } else {
      marker("M15-SMOKE: fail permissions file setup\n");
    }
  }

  /* 8b) Audit logging: the privileged chmod + setuid above make the kernel emit
   * "audit: chmod called" / "audit: setuid called" to the kernel log. Read the
   * ring buffer via klogctl(SYSLOG_ACTION_READ_ALL) and confirm both appear. */
  {
    extern int klogctl(int, char *, int);
    char kbuf[8192];
    int kn = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, kbuf, sizeof(kbuf) - 1);
    if (kn > 0) {
      kbuf[kn] = '\0';
      if (strstr(kbuf, "audit: setuid called") &&
          strstr(kbuf, "audit: chmod called"))
        marker("M15-SMOKE: ok audit-logging\n");
      else
        marker("M15-SMOKE: fail audit-logging\n");
    } else {
      marker("M15-SMOKE: fail audit-logging\n");
    }
  }

  /* M74: RT-signal queueing — block SIGRTMIN, raise 3x, unblock, 3 deliveries. */
  {
    struct sigaction rtact;
    struct sigaction rtold;
    memset(&rtact, 0, sizeof(rtact));
    memset(&rtold, 0, sizeof(rtold));
    rtact.sa_handler = rt_handler;
    int rt_ok = 0;
    if (sigaction(SIGRTMIN, &rtact, &rtold) == 0) {
      sigset_t rtset;
      sigemptyset(&rtset);
      sigaddset(&rtset, SIGRTMIN);
      pid_t self = getpid();
      sigprocmask(SIG_BLOCK, &rtset, NULL);
      g_rt_hits = 0;
      kill(self, SIGRTMIN);
      kill(self, SIGRTMIN);
      kill(self, SIGRTMIN);
      sigprocmask(SIG_UNBLOCK, &rtset, NULL);
      for (int i = 0; i < 64 && g_rt_hits < 3; i++)
        usleep(1000);
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

  /* M74: sigqueue payload via SA_SIGINFO — FIFO order verification. */
  {
    struct sigaction sa;
    struct sigaction old;
    memset(&sa, 0, sizeof(sa));
    memset(&old, 0, sizeof(old));
    sa.sa_sigaction = rt_si_handler;
    sa.sa_flags = SA_SIGINFO;
    int ok = 0;
    if (sigaction(SIGRTMIN + 1, &sa, &old) == 0) {
      sigset_t s;
      sigemptyset(&s);
      sigaddset(&s, SIGRTMIN + 1);
      pid_t self = getpid();
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
        usleep(1000);
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

  /* M74: POSIX interval timer — 20 ms periodic SIGRTMIN+2. */
  {
    struct sigaction sa;
    struct sigaction old;
    memset(&sa, 0, sizeof(sa));
    memset(&old, 0, sizeof(old));
    sa.sa_sigaction = rt_si_handler;
    sa.sa_flags = SA_SIGINFO;
    int ok = 0;
    if (sigaction(SIGRTMIN + 2, &sa, &old) == 0) {
      struct sigevent sev;
      memset(&sev, 0, sizeof(sev));
      sev.sigev_notify = SIGEV_SIGNAL;
      sev.sigev_signo = SIGRTMIN + 2;
      sev.sigev_value.sival_int = 99;
      timer_t tid;
      if (timer_create(CLOCK_MONOTONIC, &sev, &tid) == 0) {
        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        its.it_value.tv_nsec = 20000000;
        its.it_interval.tv_nsec = 20000000;
        g_rt_n = 0;
        if (timer_settime(tid, 0, &its, NULL) == 0) {
          for (int i = 0; i < 80 && g_rt_n < 3; i++) {
            struct timespec ts = {0, 10000000};
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
