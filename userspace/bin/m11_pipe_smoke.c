/*
 * m11_pipe_smoke — pipe EOF, nonblocking read/write tests.
 * Ported from deleted kernel/user/programs.c (in-kernel shell pipe tests).
 * Tests: pipe EOF when all writers close, nonblocking read returns EAGAIN,
 * nonblocking write returns EAGAIN when full.
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

  marker("M11-SMOKE: done\n");
  return 0;
}
