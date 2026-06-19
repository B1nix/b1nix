/* M57 smoke: multiprocess broker primitives.
 *
 * Audited (and proven correct here, not just asserted):
 *   fork-fdshare   fork() duplicates the fd table sharing the open-file
 *                  description, so parent and child share the file offset
 *                  (POSIX). A write in the child advances the offset the
 *                  parent observes.
 *   cloexec        FD_CLOEXEC set via fcntl(F_SETFD) (and O_CLOEXEC at open)
 *                  is closed across execve(); F_GETFD round-trips the flag.
 *   exec-inherit   A plain (non-CLOEXEC) fd survives execve() and a
 *                  dup2()-installed stdio fd survives, while the CLOEXEC fd is
 *                  gone — verified inside the exec'd image.
 *   fd-broker      socketpair() + SCM_RIGHTS hands a live fd from a parent to
 *                  a forked child over the pair; the child reads through the
 *                  passed fd and sees the parent's data.
 *   fd-broker-death an in-flight fd survives the sender closing its own copy
 *                  (the open-file description is kept alive by the SCM message
 *                  / receiver), and a closed peer is reported as a hangup.
 *   dupfd-cloexec  F_DUPFD_CLOEXEC duplicates to >= the requested fd with
 *                  FD_CLOEXEC set, while plain F_DUPFD leaves it clear.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail;

static void marker(const char *s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
}

static void ok(const char *name) {
  char buf[128];
  snprintf(buf, sizeof(buf), "M57-SMOKE: ok %s", name);
  marker(buf);
}

static void fail(const char *name, long a, long b) {
  char buf[160];
  snprintf(buf, sizeof(buf), "M57-SMOKE: FAIL %s got=%ld expected=%ld", name, a,
           b);
  marker(buf);
  g_fail = 1;
}

#define SCRATCH "/tmp/m57_scratch"

/* ---- exec-child path: report which fds it inherited -------------------- */
/* argv: m57-smoke execchild <plain_fd> <cloexec_fd>
 * Emits, via stdout (fd 1, which must itself have survived a dup2):
 *   the plain fd should be open (read returns the magic byte), the cloexec fd
 *   should be closed (fcntl F_GETFD -> EBADF). */
static int exec_child_main(int argc, char **argv) {
  if (argc < 4)
    return 2;
  int plain_fd = atoi(argv[2]);
  int cloexec_fd = atoi(argv[3]);

  /* stdout itself was a dup2 of a pipe write-end before exec; if that did not
   * survive, this write goes nowhere and the parent's check fails — proving
   * the negative as well as the positive. */
  int plain_open = (fcntl(plain_fd, F_GETFD) >= 0);
  int cloexec_open = (fcntl(cloexec_fd, F_GETFD) >= 0);

  /* Read the byte the parent wrote into the plain fd's file to confirm it is
   * the *same* open description, not just any open slot. */
  char c = 0;
  int read_ok = 0;
  if (plain_open) {
    lseek(plain_fd, 0, SEEK_SET);
    if (read(plain_fd, &c, 1) == 1 && c == 'Z')
      read_ok = 1;
  }

  /* Report a single decision byte back through stdout (the surviving dup2). */
  char verdict = (plain_open && read_ok && !cloexec_open) ? 'Y' : 'N';
  write(1, &verdict, 1);
  return 0;
}

/* ---- fork-fdshare: shared file offset across fork ---------------------- */
static void test_fork_fdshare(void) {
  int fd = open(SCRATCH, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    fail("fork-fdshare", errno, 0);
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    fail("fork-fdshare", errno, 0);
    close(fd);
    return;
  }
  if (pid == 0) {
    /* Child writes 4 bytes; the offset must advance in the shared OFD. */
    write(fd, "abcd", 4);
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  /* If the offset is shared, the parent's offset is now 4, so a fresh write
   * lands at byte 4, giving a 8-byte file. With a private offset it would be
   * 4 bytes. */
  off_t off = lseek(fd, 0, SEEK_CUR);
  write(fd, "efgh", 4);
  off_t size = lseek(fd, 0, SEEK_END);
  close(fd);
  if (off == 4 && size == 8)
    ok("fork-fdshare");
  else
    fail("fork-fdshare", (long)size, 8);
}

/* ---- cloexec: F_SETFD/F_GETFD round-trip ------------------------------- */
static void test_cloexec_flag(void) {
  int fd = open(SCRATCH, O_RDWR);
  if (fd < 0) {
    fail("cloexec", errno, 0);
    return;
  }
  if (fcntl(fd, F_GETFD) != 0) {
    fail("cloexec", fcntl(fd, F_GETFD), 0);
    close(fd);
    return;
  }
  if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 ||
      (fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0) {
    fail("cloexec", fcntl(fd, F_GETFD), FD_CLOEXEC);
    close(fd);
    return;
  }
  /* O_CLOEXEC at open() must set the flag too. */
  int fd2 = open(SCRATCH, O_RDONLY | O_CLOEXEC);
  int o = (fd2 >= 0) ? (fcntl(fd2, F_GETFD) & FD_CLOEXEC) : 0;
  if (fd2 >= 0)
    close(fd2);
  close(fd);
  if (o)
    ok("cloexec");
  else
    fail("cloexec", o, FD_CLOEXEC);
}

/* ---- exec-inherit: non-CLOEXEC survives, CLOEXEC closed, dup2 stdio ----- */
static void test_exec_inherit(const char *self) {
  /* Open a file, seed it with magic 'Z', keep it NON-cloexec. */
  int plain = open(SCRATCH, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (plain < 0) {
    fail("exec-inherit", errno, 0);
    return;
  }
  write(plain, "Z", 1);
  /* A second fd marked CLOEXEC — must vanish across exec. */
  int cloexec = dup(plain);
  fcntl(cloexec, F_SETFD, FD_CLOEXEC);

  /* Pipe whose write-end we dup2 onto the child's stdout, to capture the
   * child's verdict byte. The dup2 target (fd 1) must survive exec. */
  int pp[2];
  if (pipe(pp) < 0) {
    fail("exec-inherit", errno, 0);
    close(plain);
    close(cloexec);
    return;
  }

  char a_plain[16], a_cloexec[16];
  snprintf(a_plain, sizeof(a_plain), "%d", plain);
  snprintf(a_cloexec, sizeof(a_cloexec), "%d", cloexec);

  pid_t pid = fork();
  if (pid < 0) {
    fail("exec-inherit", errno, 0);
    close(plain); close(cloexec); close(pp[0]); close(pp[1]);
    return;
  }
  if (pid == 0) {
    dup2(pp[1], 1);     /* child stdout -> pipe write-end (survives exec) */
    close(pp[0]);
    close(pp[1]);
    char *cargv[] = {(char *)"m57-smoke", (char *)"execchild", a_plain,
                     a_cloexec, 0};
    execve(self, cargv, environ);
    _exit(127);         /* exec failed */
  }
  close(pp[1]);
  char verdict = '?';
  read(pp[0], &verdict, 1);
  close(pp[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  close(plain);
  close(cloexec);
  if (verdict == 'Y')
    ok("exec-inherit");
  else
    fail("exec-inherit", verdict, 'Y');
}

/* Send fd `payload` over the socket `sock` with a 1-byte data message. */
static int send_fd(int sock, int payload) {
  char data = 'x';
  struct iovec iov = {&data, 1};
  union {
    struct cmsghdr align;
    char buf[CMSG_SPACE(sizeof(int))];
  } u;
  memset(&u, 0, sizeof(u));
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = u.buf;
  msg.msg_controllen = sizeof(u.buf);
  struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &payload, sizeof(int));
  return (int)sendmsg(sock, &msg, 0);
}

/* Receive a single fd; returns the new fd or -1. */
static int recv_fd(int sock) {
  char data = 0;
  struct iovec iov = {&data, 1};
  union {
    struct cmsghdr align;
    char buf[CMSG_SPACE(sizeof(int))];
  } u;
  memset(&u, 0, sizeof(u));
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = u.buf;
  msg.msg_controllen = sizeof(u.buf);
  if (recvmsg(sock, &msg, 0) < 0)
    return -1;
  struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
  if (!c || c->cmsg_type != SCM_RIGHTS)
    return -1;
  int got = -1;
  memcpy(&got, CMSG_DATA(c), sizeof(int));
  return got;
}

/* ---- fd-broker: socketpair + SCM_RIGHTS hand an fd to a child ---------- */
static void test_fd_broker(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    fail("fd-broker", errno, 0);
    return;
  }
  /* Parent (the "broker") opens a resource and will hand it to the child. */
  int res = open(SCRATCH, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (res < 0) {
    fail("fd-broker", errno, 0);
    close(sv[0]); close(sv[1]);
    return;
  }
  write(res, "BROKER", 6);

  pid_t pid = fork();
  if (pid < 0) {
    fail("fd-broker", errno, 0);
    close(sv[0]); close(sv[1]); close(res);
    return;
  }
  if (pid == 0) {
    close(sv[0]);
    close(res);           /* child has no path to the resource except the pass */
    int got = recv_fd(sv[1]);
    char tmp[8] = {0};
    int good = 0;
    if (got >= 0) {
      lseek(got, 0, SEEK_SET);
      if (read(got, tmp, 6) == 6 && memcmp(tmp, "BROKER", 6) == 0)
        good = 1;
      close(got);
    }
    close(sv[1]);
    _exit(good ? 0 : 1);
  }
  close(sv[1]);
  if (send_fd(sv[0], res) < 0) {
    fail("fd-broker", errno, 0);
    close(sv[0]); close(res);
    return;
  }
  close(res);
  close(sv[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    ok("fd-broker");
  else
    fail("fd-broker", WIFEXITED(status) ? WEXITSTATUS(status) : -1, 0);
}

/* ---- fd-broker-death: in-flight fd survives sender-close; peer hangup --- */
static void test_fd_broker_death(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    fail("fd-broker-death", errno, 0);
    return;
  }
  int res = open(SCRATCH, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (res < 0) {
    fail("fd-broker-death", errno, 0);
    close(sv[0]); close(sv[1]);
    return;
  }
  write(res, "INFLIGHT", 8);

  /* Queue the fd, then close BOTH our copy of the resource fd. The open-file
   * description must stay alive because the SCM message holds a reference. */
  if (send_fd(sv[0], res) < 0) {
    fail("fd-broker-death", errno, 0);
    close(sv[0]); close(sv[1]); close(res);
    return;
  }
  close(res);

  int got = recv_fd(sv[1]);
  char tmp[8] = {0};
  int survived = 0;
  if (got >= 0) {
    lseek(got, 0, SEEK_SET);
    if (read(got, tmp, 8) == 8 && memcmp(tmp, "INFLIGHT", 8) == 0)
      survived = 1;
    close(got);
  }

  /* Now close one end of the pair; a recv on the other must report hangup
   * (0 bytes, not a hang) so a broker can clean up a dead peer. */
  close(sv[0]);
  char b = 0;
  ssize_t r = recv(sv[1], &b, 1, 0);
  close(sv[1]);

  if (survived && r == 0)
    ok("fd-broker-death");
  else
    fail("fd-broker-death", survived ? (long)r : -1, 0);
}

/* ---- dupfd-cloexec: F_DUPFD_CLOEXEC vs F_DUPFD ------------------------- */
static void test_dupfd_cloexec(void) {
  int fd = open(SCRATCH, O_RDONLY);
  if (fd < 0) {
    fail("dupfd-cloexec", errno, 0);
    return;
  }
  int d1 = fcntl(fd, F_DUPFD_CLOEXEC, 20);
  int d2 = fcntl(fd, F_DUPFD, 20);
  int ok1 = (d1 >= 20) && ((fcntl(d1, F_GETFD) & FD_CLOEXEC) != 0);
  int ok2 = (d2 >= 20) && ((fcntl(d2, F_GETFD) & FD_CLOEXEC) == 0);
  if (d1 >= 0) close(d1);
  if (d2 >= 0) close(d2);
  close(fd);
  if (ok1 && ok2)
    ok("dupfd-cloexec");
  else
    fail("dupfd-cloexec", ok1 ? 0 : d1, ok2 ? 0 : d2);
}

int main(int argc, char **argv) {
  if (argc >= 2 && argv[1] && strcmp(argv[1], "execchild") == 0)
    return exec_child_main(argc, argv);

  const char *self = (argc > 0 && argv[0] && argv[0][0] == '/') ? argv[0]
                                                                : "/bin/m57-smoke";

  test_fork_fdshare();
  test_cloexec_flag();
  test_exec_inherit(self);
  test_fd_broker();
  test_fd_broker_death();
  test_dupfd_cloexec();

  unlink(SCRATCH);

  /* The failure string must not contain "M57-SMOKE: done" — the host-side
   * check greps for that exact done marker as the all-passed signal. */
  marker(g_fail ? "M57-SMOKE: completed-with-failures" : "M57-SMOKE: done");
  return g_fail;
}
