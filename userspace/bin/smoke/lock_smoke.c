/*
 * lock_smoke — POSIX fcntl file locking tests.
 * Ported from deleted kernel/user/programs.c lock_smoke_main().
 * Tests: exclusive lock, nonblock conflict (EAGAIN), blocking wait (F_SETLKW),
 * wake-on-close, and child exit status propagation.
 */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void marker(const char *t) { write(1, t, strlen(t)); }

int main(void) {
  marker("LOCK-SMOKE: start\n");

  int fd = open("/tmp/lock-smoke.dat", O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (fd < 0) { marker("LOCK-SMOKE: fail open\n"); return 1; }

  /* Parent takes an exclusive write lock on the whole file. */
  struct flock lk;
  memset(&lk, 0, sizeof(lk));
  lk.l_type = F_WRLCK;
  if (fcntl(fd, F_SETLK, &lk) < 0) {
    marker("LOCK-SMOKE: fail parent-setlk\n");
    close(fd); return 1;
  }

  /* Handshake pipe: the parent must not release its lock until the child has
   * actually attempted the non-blocking lock, otherwise the child observes a
   * free file and F_SETLK legitimately succeeds. */
  int sync[2];
  if (pipe(sync) < 0) { marker("LOCK-SMOKE: fail pipe\n"); close(fd); return 1; }

  pid_t pid = fork();
  if (pid < 0) {
    marker("LOCK-SMOKE: fail fork\n");
    close(sync[0]); close(sync[1]); close(fd);
    return 1;
  }

  if (pid == 0) {
    close(sync[0]);
    /* Child: try non-blocking lock → must get EAGAIN. */
    struct flock clk;
    memset(&clk, 0, sizeof(clk));
    clk.l_type = F_WRLCK;
    int rc = fcntl(fd, F_SETLK, &clk);
    if (rc != -1 || errno != EAGAIN) {
      char d[96];
      snprintf(d, sizeof(d),
               "LOCK-SMOKE: detail nonblock-conflict rc=%d errno=%d\n", rc,
               errno);
      marker(d);
      marker("LOCK-SMOKE: fail nonblock-conflict\n");
      _exit(2);
    }
    marker("LOCK-SMOKE: ok nonblock-conflict\n");

    /* Tell the parent it may release now; the blocking acquire below then has
     * a real conflict to wait on. */
    if (write(sync[1], "x", 1) != 1) {
      marker("LOCK-SMOKE: fail sync\n");
      _exit(4);
    }
    close(sync[1]);

    /* Child: blocking lock — will succeed once parent closes. */
    if (fcntl(fd, F_SETLKW, &clk) < 0) {
      marker("LOCK-SMOKE: fail setlkw\n");
      _exit(3);
    }
    marker("LOCK-SMOKE: ok wake-on-close\n");
    close(fd);
    _exit(0);
  }

  /* Parent: block until the child reports it has tried the non-blocking lock
   * (or died, which closes the pipe), then release. */
  close(sync[1]);
  char ack;
  (void)read(sync[0], &ack, 1);
  close(sync[0]);
  close(fd);

  int st = 0;
  waitpid(pid, &st, 0);
  if (WEXITSTATUS(st) != 0) {
    marker("LOCK-SMOKE: fail child-status\n");
    return 1;
  }

  marker("LOCK-SMOKE: done\n");
  unlink("/tmp/lock-smoke.dat");
  return 0;
}
