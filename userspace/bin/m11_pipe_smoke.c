/*
 * m11_pipe_smoke — pipe EOF, nonblocking read/write tests.
 * Ported from deleted kernel/user/programs.c (in-kernel shell pipe tests).
 * Tests: pipe EOF when all writers close, nonblocking read returns EAGAIN,
 * nonblocking write returns EAGAIN when full, and named pipes (mkfifo): node
 * type, the ENXIO rule for a writer with no reader, the blocking open
 * rendezvous, data transfer and EOF.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FIFO_PATH "/run/m11.fifo"

static void marker(const char *t) { write(1, t, strlen(t)); }

int main(void) {
  marker("M11-SMOKE: start\n");

  /* Test 1: pipe EOF — reader gets 0 when all writers close. */
  {
    int pfd[2];
    if (pipe(pfd) < 0) { marker("M11-SMOKE: fail pipe\n"); return 1; }
    /* Write something then close write end. */
    write(pfd[1], "x", 1);
    close(pfd[1]);
    char buf[8];
    ssize_t n = read(pfd[0], buf, sizeof(buf));
    close(pfd[0]);
    if (n == 1 && buf[0] == 'x')
      marker("M11-SMOKE: ok pipe-eof\n");
    else
      marker("M11-SMOKE: fail pipe-eof\n");
  }

  /* Test 2: nonblocking read on empty pipe returns EAGAIN. */
  {
    int pfd[2];
    if (pipe(pfd) < 0) { marker("M11-SMOKE: fail pipe\n"); return 1; }
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    char buf[8];
    ssize_t n = read(pfd[0], buf, sizeof(buf));
    close(pfd[0]); close(pfd[1]);
    if (n < 0 && errno == EAGAIN)
      marker("M11-SMOKE: ok pipe-nonblock-read\n");
    else
      marker("M11-SMOKE: fail pipe-nonblock-read\n");
  }

  /* Test 3: nonblocking write to full pipe returns EAGAIN. */
  {
    int pfd[2];
    if (pipe(pfd) < 0) { marker("M11-SMOKE: fail pipe\n"); return 1; }
    fcntl(pfd[1], F_SETFL, O_NONBLOCK);
    /* Fill the pipe buffer. */
    char buf[4096];
    memset(buf, 'A', sizeof(buf));
    int fills = 0;
    while (write(pfd[1], buf, sizeof(buf)) > 0)
      fills++;
    /* Next write should fail with EAGAIN. */
    ssize_t n = write(pfd[1], "x", 1);
    close(pfd[0]); close(pfd[1]);
    if (n < 0 && errno == EAGAIN && fills > 0)
      marker("M11-SMOKE: ok pipe-nonblock-write\n");
    else
      marker("M11-SMOKE: fail pipe-nonblock-write\n");
  }

  /* Test 4: mkfifo creates a FIFO node that stat() reports as one. /run is the
   * volatile in-memory directory an init system uses for its control FIFO. */
  {
    unlink(FIFO_PATH);
    struct stat st;
    if (mkfifo(FIFO_PATH, 0600) == 0 && stat(FIFO_PATH, &st) == 0 &&
        S_ISFIFO(st.st_mode))
      marker("M11-SMOKE: ok fifo-mkfifo\n");
    else
      marker("M11-SMOKE: fail fifo-mkfifo\n");
  }

  /* Test 5: O_WRONLY | O_NONBLOCK with no reader present fails with ENXIO
   * (POSIX), instead of blocking or silently succeeding. */
  {
    int fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (fd < 0 && errno == ENXIO)
      marker("M11-SMOKE: ok fifo-nonblock-enxio\n");
    else
      marker("M11-SMOKE: fail fifo-nonblock-enxio\n");
    if (fd >= 0)
      close(fd);
  }

  /* Test 6: the blocking open rendezvous — the reader's open() returns only
   * once a writer arrives — plus data transfer and EOF when the writer closes.
   * This is exactly the path openrc-init and sysvinit use for their control
   * FIFO, so it is checked end to end across a fork. */
  {
    pid_t pid = fork();
    if (pid == 0) {
      int r = open(FIFO_PATH, O_RDONLY);
      if (r < 0)
        _exit(2);
      char buf[16];
      ssize_t n = read(r, buf, sizeof(buf));
      ssize_t eof = read(r, buf + 8, 1);
      close(r);
      if (n != 5)
        _exit(4);
      if (memcmp(buf, "hello", 5) != 0)
        _exit(5);
      if (eof != 0)
        _exit(6);
      _exit(0);
    } else if (pid > 0) {
      int w = open(FIFO_PATH, O_WRONLY);
      int werr = w < 0 ? errno : 0;
      ssize_t wn = w >= 0 ? write(w, "hello", 5) : -werr;
      if (w >= 0)
        close(w);
      int status = -1;
      waitpid(pid, &status, 0);
      if (wn == 5 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        marker("M11-SMOKE: ok fifo-rendezvous\n");
        marker("M11-SMOKE: ok fifo-eof\n");
      } else {
        /* Name the broken half: wn is the writer's byte count (negative errno
         * when its open failed), code the reader's exit status. */
        char msg[96];
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        snprintf(msg, sizeof(msg),
                 "M11-SMOKE: fail fifo-rendezvous wn=%d reader=%d\n", (int)wn,
                 code);
        marker(msg);
      }
    } else {
      marker("M11-SMOKE: fail fifo-rendezvous\n");
    }
    unlink(FIFO_PATH);
  }

  marker("M11-SMOKE: done\n");
  return 0;
}
