/* M125 — io_uring.
 *
 * Drives the rings directly rather than through liburing: the point of the
 * check is that the ABI a library is compiled against is the one the kernel
 * implements, so the ring head/tail protocol, the SQE layout and the mmap
 * offsets are exercised by hand here. liburing's own suite runs in the Debian
 * lane (tools/image/debian-stage.sh).
 *
 * Every marker is printed only after the operation's result has been checked.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register 427
#endif

static int fails;

static void ok(const char *what) { printf("M125-SMOKE: ok %s\n", what); fflush(stdout); }

static void bad(const char *what, const char *why, long v) {
  printf("M125-SMOKE: FAIL %s — %s (%ld, errno=%d)\n", what, why, v, errno);
  fflush(stdout);
  fails++;
}

static void judge(const char *what, int good, const char *why, long v) {
  if (good)
    ok(what);
  else
    bad(what, why, v);
}

static int io_uring_setup_(unsigned entries, struct io_uring_params *p) {
  return (int)syscall(__NR_io_uring_setup, entries, p);
}
static int io_uring_enter_(int fd, unsigned to_submit, unsigned min_complete,
                           unsigned flags, void *arg, size_t argsz) {
  return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags,
                      arg, argsz);
}
static int io_uring_register_(int fd, unsigned op, void *arg, unsigned nr) {
  return (int)syscall(__NR_io_uring_register, fd, op, arg, nr);
}

/* The mapped ring, as this program sees it. */
struct ring {
  int fd;
  struct io_uring_params p;
  void *sq_ptr;
  size_t sq_sz;
  struct io_uring_sqe *sqes;
  size_t sqes_sz;

  unsigned *sq_head, *sq_tail, *sq_mask, *sq_entries, *sq_flags, *sq_dropped;
  unsigned *sq_array;
  unsigned *cq_head, *cq_tail, *cq_mask, *cq_entries, *cq_overflow;
  struct io_uring_cqe *cqes;
};

static int ring_make(struct ring *r, unsigned entries, unsigned flags,
                     unsigned cq_entries) {
  memset(r, 0, sizeof(*r));
  r->p.flags = flags;
  if (cq_entries) {
    r->p.flags |= IORING_SETUP_CQSIZE;
    r->p.cq_entries = cq_entries;
  }
  r->fd = io_uring_setup_(entries, &r->p);
  if (r->fd < 0)
    return -1;

  r->sq_sz = r->p.sq_off.array + r->p.sq_entries * sizeof(unsigned);
  size_t cq_sz = r->p.cq_off.cqes + r->p.cq_entries * sizeof(struct io_uring_cqe);

  if (cq_sz > r->sq_sz)
    r->sq_sz = cq_sz;

  r->sq_ptr = mmap(0, r->sq_sz, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
  if (r->sq_ptr == MAP_FAILED)
    return -2;
  r->sqes_sz = r->p.sq_entries * sizeof(struct io_uring_sqe);
  r->sqes = mmap(0, r->sqes_sz, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQES);
  if (r->sqes == MAP_FAILED)
    return -3;

  char *b = (char *)r->sq_ptr;

  r->sq_head = (unsigned *)(b + r->p.sq_off.head);
  r->sq_tail = (unsigned *)(b + r->p.sq_off.tail);
  r->sq_mask = (unsigned *)(b + r->p.sq_off.ring_mask);
  r->sq_entries = (unsigned *)(b + r->p.sq_off.ring_entries);
  r->sq_flags = (unsigned *)(b + r->p.sq_off.flags);
  r->sq_dropped = (unsigned *)(b + r->p.sq_off.dropped);
  r->sq_array = (unsigned *)(b + r->p.sq_off.array);
  r->cq_head = (unsigned *)(b + r->p.cq_off.head);
  r->cq_tail = (unsigned *)(b + r->p.cq_off.tail);
  r->cq_mask = (unsigned *)(b + r->p.cq_off.ring_mask);
  r->cq_entries = (unsigned *)(b + r->p.cq_off.ring_entries);
  r->cq_overflow = (unsigned *)(b + r->p.cq_off.overflow);
  r->cqes = (struct io_uring_cqe *)(b + r->p.cq_off.cqes);
  return 0;
}

static void ring_free(struct ring *r) {
  if (r->sqes && r->sqes != MAP_FAILED)
    munmap(r->sqes, r->sqes_sz);
  if (r->sq_ptr && r->sq_ptr != MAP_FAILED)
    munmap(r->sq_ptr, r->sq_sz);
  if (r->fd >= 0)
    close(r->fd);
  r->fd = -1;
  r->sqes = 0;
  r->sq_ptr = 0;
}

/* Take the next submission slot and put its index in the ring array. */
static struct io_uring_sqe *sq_get(struct ring *r) {
  unsigned tail = __atomic_load_n(r->sq_tail, __ATOMIC_RELAXED);
  unsigned head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);

  if (tail - head >= *r->sq_entries)
    return 0;
  unsigned idx = tail & *r->sq_mask;
  struct io_uring_sqe *sqe = &r->sqes[idx];

  memset(sqe, 0, sizeof(*sqe));
  r->sq_array[idx] = idx;
  __atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
  return sqe;
}

static int cq_get(struct ring *r, struct io_uring_cqe *out) {
  unsigned head = __atomic_load_n(r->cq_head, __ATOMIC_RELAXED);
  unsigned tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);

  if (head == tail)
    return 0;
  *out = r->cqes[head & *r->cq_mask];
  __atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
  return 1;
}

/* Submit everything queued and wait for one completion. */
static int submit_wait(struct ring *r, unsigned nr, struct io_uring_cqe *out) {
  int rc = io_uring_enter_(r->fd, nr, 1, IORING_ENTER_GETEVENTS, 0, 0);

  if (rc < 0)
    return rc;
  if (!cq_get(r, out))
    return -1;
  return 0;
}

/* ---- the checks --------------------------------------------------------- */

static void check_setup(void) {
  struct ring r;
  int rc = ring_make(&r, 8, 0, 0);

  if (rc < 0) {
    bad("setup", rc == -1 ? "io_uring_setup refused" : "the rings would not map",
        rc);
    return;
  }
  int good = r.p.sq_entries == 8 && r.p.cq_entries >= 8 &&
             *r.sq_entries == r.p.sq_entries &&
             *r.sq_mask == r.p.sq_entries - 1 &&
             *r.cq_entries == r.p.cq_entries &&
             *r.cq_mask == r.p.cq_entries - 1 && *r.sq_head == 0 &&
             *r.sq_tail == 0 && *r.cq_head == 0 && *r.cq_tail == 0;

  judge("setup", good, "the mapped rings do not describe themselves",
        (long)r.p.sq_entries);
  judge("features",
        (r.p.features & IORING_FEAT_NODROP) &&
            (r.p.features & IORING_FEAT_SUBMIT_STABLE) &&
            (r.p.features & IORING_FEAT_SINGLE_MMAP),
        "the advertised feature set is missing something it must have",
        (long)r.p.features);
  ring_free(&r);

  /* A flag the kernel cannot honour must be refused here, not accepted and
   * then mishandled. */
  struct io_uring_params p;

  memset(&p, 0, sizeof(p));
  p.flags = IORING_SETUP_SQPOLL;
  int fd = io_uring_setup_(8, &p);

  judge("refuses-sqpoll", fd < 0 && errno == EINVAL,
        "IORING_SETUP_SQPOLL was accepted", (long)fd);
  if (fd >= 0)
    close(fd);

  memset(&p, 0, sizeof(p));
  p.flags = IORING_SETUP_IOPOLL;
  fd = io_uring_setup_(8, &p);
  judge("refuses-iopoll", fd < 0 && errno == EINVAL,
        "IORING_SETUP_IOPOLL was accepted", (long)fd);
  if (fd >= 0)
    close(fd);

  memset(&p, 0, sizeof(p));
  fd = io_uring_setup_(0, &p);
  judge("refuses-zero-entries", fd < 0 && errno == EINVAL,
        "a zero-entry ring was created", (long)fd);
  if (fd >= 0)
    close(fd);
}

static void check_nop(void) {
  struct ring r;
  struct io_uring_cqe cqe;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("nop", "no ring", 0);
    return;
  }
  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_NOP;
  sqe->user_data = 0x4e4f50ull;
  int rc = submit_wait(&r, 1, &cqe);

  judge("nop", rc == 0 && cqe.user_data == 0x4e4f50ull && cqe.res == 0,
        "the no-op did not come back", rc == 0 ? (long)cqe.res : (long)rc);
  ring_free(&r);
}

static void check_file_rw(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  const char *path = "/tmp/m125-rw";
  char out[64], in[64];
  int fd;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("file-write", "no ring", 0);
    return;
  }
  fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    bad("file-write", "the scratch file would not open", fd);
    ring_free(&r);
    return;
  }
  memset(out, 'A', sizeof(out));
  memcpy(out, "io_uring wrote this", 19);

  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_WRITE;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(unsigned long)out;
  sqe->len = sizeof(out);
  sqe->off = 0;
  sqe->user_data = 1;
  int rc = submit_wait(&r, 1, &cqe);
  int wrote = (rc == 0 && cqe.user_data == 1) ? cqe.res : -1;

  judge("file-write", wrote == (int)sizeof(out),
        "the write did not report the whole buffer", (long)wrote);

  /* The host's own read must see it: the CQE is only half the claim. */
  memset(in, 0, sizeof(in));
  ssize_t direct = pread(fd, in, sizeof(in), 0);

  judge("file-write-lands",
        direct == (ssize_t)sizeof(out) && memcmp(in, out, sizeof(out)) == 0,
        "read(2) does not see what the ring wrote", (long)direct);

  memset(in, 0, sizeof(in));
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_READ;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(unsigned long)in;
  sqe->len = sizeof(in);
  sqe->off = 0;
  sqe->user_data = 2;
  rc = submit_wait(&r, 1, &cqe);
  judge("file-read",
        rc == 0 && cqe.user_data == 2 && cqe.res == (int)sizeof(in) &&
            memcmp(in, out, sizeof(out)) == 0,
        "the ring read back something else",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* Vectored, at an offset. */
  char v1[8], v2[8];
  struct iovec iov[2] = {{v1, sizeof(v1)}, {v2, sizeof(v2)}};

  memset(v1, 0, sizeof(v1));
  memset(v2, 0, sizeof(v2));
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_READV;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(unsigned long)iov;
  sqe->len = 2;
  sqe->off = 0;
  sqe->user_data = 3;
  rc = submit_wait(&r, 1, &cqe);
  judge("file-readv",
        rc == 0 && cqe.res == 16 && memcmp(v1, out, 8) == 0 &&
            memcmp(v2, out + 8, 8) == 0,
        "the vectored read did not fill both buffers",
        rc == 0 ? (long)cqe.res : (long)rc);

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_FSYNC;
  sqe->fd = fd;
  sqe->user_data = 4;
  rc = submit_wait(&r, 1, &cqe);
  judge("file-fsync", rc == 0 && cqe.user_data == 4 && cqe.res == 0,
        "fsync through the ring failed", rc == 0 ? (long)cqe.res : (long)rc);

  close(fd);
  unlink(path);
  ring_free(&r);
}

/* A read on a pipe with nothing in it must not block the submitter: it is
 * armed on the pipe's readiness and completes once something is written. */
static void check_pipe_async(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  int pfd[2];
  char buf[16];

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("pipe-async", "no ring", 0);
    return;
  }
  if (pipe(pfd) < 0) {
    bad("pipe-async", "no pipe", -1);
    ring_free(&r);
    return;
  }
  memset(buf, 0, sizeof(buf));
  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_READ;
  sqe->fd = pfd[0];
  sqe->addr = (unsigned long long)(unsigned long)buf;
  sqe->len = sizeof(buf);
  sqe->off = (unsigned long long)-1;
  sqe->user_data = 11;

  /* Submit with no wait. The submitter must come straight back. */
  int rc = io_uring_enter_(r.fd, 1, 0, 0, 0, 0);

  judge("pipe-submit-does-not-block", rc == 1,
        "submitting a read on an empty pipe did not return 1", (long)rc);

  struct io_uring_cqe peek;

  judge("pipe-not-complete-yet", cq_get(&r, &peek) == 0,
        "a read on an empty pipe completed anyway", 0);

  if (write(pfd[1], "hello", 5) != 5) {
    bad("pipe-async", "the write end refused", -1);
    close(pfd[0]);
    close(pfd[1]);
    ring_free(&r);
    return;
  }
  rc = io_uring_enter_(r.fd, 0, 1, IORING_ENTER_GETEVENTS, 0, 0);
  int got = (rc >= 0 && cq_get(&r, &cqe));

  judge("pipe-async",
        got && cqe.user_data == 11 && cqe.res == 5 &&
            memcmp(buf, "hello", 5) == 0,
        "the armed pipe read did not complete with the data",
        got ? (long)cqe.res : (long)rc);

  /* POLL_ADD on the same pipe, now empty again. */
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_POLL_ADD;
  sqe->fd = pfd[0];
  sqe->poll32_events = POLLIN;
  sqe->user_data = 12;
  rc = io_uring_enter_(r.fd, 1, 0, 0, 0, 0);
  int early = cq_get(&r, &peek);

  if (write(pfd[1], "x", 1) != 1)
    bad("poll-add", "the write end refused", -1);
  rc = io_uring_enter_(r.fd, 0, 1, IORING_ENTER_GETEVENTS, 0, 0);
  got = (rc >= 0 && cq_get(&r, &cqe));
  judge("poll-add",
        !early && got && cqe.user_data == 12 && (cqe.res & POLLIN),
        "POLL_ADD did not report readability when data arrived",
        got ? (long)cqe.res : (long)rc);

  close(pfd[0]);
  close(pfd[1]);
  ring_free(&r);
}

static void check_timeout_and_cancel(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct __kernel_timespec ts;
  int pfd[2];

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("timeout", "no ring", 0);
    return;
  }
  ts.tv_sec = 0;
  ts.tv_nsec = 50 * 1000 * 1000;
  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_TIMEOUT;
  sqe->addr = (unsigned long long)(unsigned long)&ts;
  sqe->len = 1;
  sqe->off = 0;
  sqe->user_data = 21;

  struct timespec t0, t1;

  clock_gettime(CLOCK_MONOTONIC, &t0);
  int rc = submit_wait(&r, 1, &cqe);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  long long elapsed_ms = (long long)(t1.tv_sec - t0.tv_sec) * 1000 +
                         (t1.tv_nsec - t0.tv_nsec) / 1000000;

  judge("timeout",
        rc == 0 && cqe.user_data == 21 && cqe.res == -ETIME &&
            elapsed_ms >= 40,
        "the timeout did not expire with -ETIME after its interval",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* Cancel a poll that will never fire. */
  if (pipe(pfd) < 0) {
    bad("cancel", "no pipe", -1);
    ring_free(&r);
    return;
  }
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_POLL_ADD;
  sqe->fd = pfd[0];
  sqe->poll32_events = POLLIN;
  sqe->user_data = 22;
  io_uring_enter_(r.fd, 1, 0, 0, 0, 0);

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_ASYNC_CANCEL;
  sqe->addr = 22;
  sqe->user_data = 23;
  rc = io_uring_enter_(r.fd, 1, 2, IORING_ENTER_GETEVENTS, 0, 0);

  struct io_uring_cqe a, b;
  int n = 0;

  if (cq_get(&r, &a))
    n++;
  if (cq_get(&r, &b))
    n++;
  int cancelled = 0, acked = 0;

  if (n == 2) {
    struct io_uring_cqe *poll = (a.user_data == 22) ? &a : &b;
    struct io_uring_cqe *canc = (a.user_data == 22) ? &b : &a;

    cancelled = (poll->user_data == 22 && poll->res == -ECANCELED);
    acked = (canc->user_data == 23 && canc->res == 0);
  }
  judge("cancel", n == 2 && cancelled && acked,
        "cancelling a poll did not produce -ECANCELED and an acknowledgement",
        (long)n);

  close(pfd[0]);
  close(pfd[1]);
  ring_free(&r);
}

static void check_links(void) {
  struct ring r;
  struct io_uring_cqe c1, c2;
  const char *path = "/tmp/m125-link";
  char out[8] = "linked!";
  char in[8];
  int fd;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("link", "no ring", 0);
    return;
  }
  fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    bad("link", "no scratch file", fd);
    ring_free(&r);
    return;
  }
  memset(in, 0, sizeof(in));

  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_WRITE;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(unsigned long)out;
  sqe->len = sizeof(out);
  sqe->off = 0;
  sqe->flags = IOSQE_IO_LINK;
  sqe->user_data = 31;

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_READ;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(unsigned long)in;
  sqe->len = sizeof(in);
  sqe->off = 0;
  sqe->user_data = 32;

  int rc = io_uring_enter_(r.fd, 2, 2, IORING_ENTER_GETEVENTS, 0, 0);
  int n = 0;

  if (cq_get(&r, &c1))
    n++;
  if (cq_get(&r, &c2))
    n++;
  judge("link-ordered",
        rc == 2 && n == 2 && c1.user_data == 31 && c2.user_data == 32 &&
            c1.res == (int)sizeof(out) && c2.res == (int)sizeof(in) &&
            memcmp(in, out, sizeof(out)) == 0,
        "the linked read did not run after the write it was linked to",
        n == 2 ? (long)c2.res : (long)rc);

  /* A link whose head fails must cancel the rest. */
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_READ;
  sqe->fd = -1; /* fails with EBADF */
  sqe->addr = (unsigned long long)(unsigned long)in;
  sqe->len = 4;
  sqe->flags = IOSQE_IO_LINK;
  sqe->user_data = 33;

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_NOP;
  sqe->user_data = 34;

  rc = io_uring_enter_(r.fd, 2, 2, IORING_ENTER_GETEVENTS, 0, 0);
  n = 0;
  if (cq_get(&r, &c1))
    n++;
  if (cq_get(&r, &c2))
    n++;
  int head_failed = 0, tail_cancelled = 0;

  if (n == 2) {
    struct io_uring_cqe *h = (c1.user_data == 33) ? &c1 : &c2;
    struct io_uring_cqe *t = (c1.user_data == 33) ? &c2 : &c1;

    head_failed = (h->res == -EBADF);
    tail_cancelled = (t->user_data == 34 && t->res == -ECANCELED);
  }
  judge("link-broken", n == 2 && head_failed && tail_cancelled,
        "a failed link head did not cancel the rest of the chain", (long)n);

  close(fd);
  unlink(path);
  ring_free(&r);
}

static void check_registration(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  const char *path = "/tmp/m125-reg";
  char out[32], in[32];
  int fd, rc;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("register-files", "no ring", 0);
    return;
  }
  fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    bad("register-files", "no scratch file", fd);
    ring_free(&r);
    return;
  }
  memset(out, 'R', sizeof(out));
  if (write(fd, out, sizeof(out)) != (ssize_t)sizeof(out)) {
    bad("register-files", "could not seed the file", -1);
    close(fd);
    ring_free(&r);
    return;
  }

  int fds[1] = {fd};

  rc = io_uring_register_(r.fd, IORING_REGISTER_FILES, fds, 1);
  if (rc < 0) {
    bad("register-files", "IORING_REGISTER_FILES refused", rc);
    close(fd);
    unlink(path);
    ring_free(&r);
    return;
  }

  /* Closing the descriptor must not take the registration with it: holding
   * the open file, not the number, is the whole point of registering. */
  close(fd);

  memset(in, 0, sizeof(in));
  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_READ;
  sqe->fd = 0; /* the index, not a descriptor */
  sqe->flags = IOSQE_FIXED_FILE;
  sqe->addr = (unsigned long long)(unsigned long)in;
  sqe->len = sizeof(in);
  sqe->off = 0;
  sqe->user_data = 41;
  rc = submit_wait(&r, 1, &cqe);
  judge("register-files",
        rc == 0 && cqe.res == (int)sizeof(in) &&
            memcmp(in, out, sizeof(out)) == 0,
        "a registered file did not survive its descriptor being closed",
        rc == 0 ? (long)cqe.res : (long)rc);

  rc = io_uring_register_(r.fd, IORING_UNREGISTER_FILES, 0, 0);
  judge("unregister-files", rc == 0, "IORING_UNREGISTER_FILES failed", rc);
  unlink(path);
  ring_free(&r);
}

static void check_registered_buffers(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  const char *path = "/tmp/m125-buf";
  static char big[4096];
  struct iovec iov = {big, sizeof(big)};
  int fd, rc;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("register-buffers", "no ring", 0);
    return;
  }
  fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    bad("register-buffers", "no scratch file", fd);
    ring_free(&r);
    return;
  }
  memset(big, 'B', sizeof(big));
  rc = io_uring_register_(r.fd, IORING_REGISTER_BUFFERS, &iov, 1);
  if (rc < 0) {
    bad("register-buffers", "IORING_REGISTER_BUFFERS refused", rc);
    close(fd);
    unlink(path);
    ring_free(&r);
    return;
  }

  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_WRITE_FIXED;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(unsigned long)big;
  sqe->len = sizeof(big);
  sqe->off = 0;
  sqe->buf_index = 0;
  sqe->user_data = 51;
  rc = submit_wait(&r, 1, &cqe);
  int wrote = (rc == 0) ? cqe.res : -1;

  memset(big, 0, sizeof(big));
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_READ_FIXED;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(unsigned long)big;
  sqe->len = sizeof(big);
  sqe->off = 0;
  sqe->buf_index = 0;
  sqe->user_data = 52;
  rc = submit_wait(&r, 1, &cqe);

  int allB = 1;

  for (size_t i = 0; i < sizeof(big); i++)
    if (big[i] != 'B') {
      allB = 0;
      break;
    }
  judge("register-buffers",
        wrote == (int)sizeof(big) && rc == 0 && cqe.res == (int)sizeof(big) &&
            allB,
        "a fixed write and read did not round-trip the registered buffer",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* A fixed operation outside the registered range is -EFAULT, not silent. */
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_READ_FIXED;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(unsigned long)big + 4000;
  sqe->len = 4096;
  sqe->off = 0;
  sqe->buf_index = 0;
  sqe->user_data = 53;
  rc = submit_wait(&r, 1, &cqe);
  judge("fixed-buffer-bounds", rc == 0 && cqe.res == -EFAULT,
        "a fixed read past the registered range was allowed",
        rc == 0 ? (long)cqe.res : (long)rc);

  io_uring_register_(r.fd, IORING_UNREGISTER_BUFFERS, 0, 0);
  close(fd);
  unlink(path);
  ring_free(&r);
}

static void check_probe(void) {
  struct ring r;
  char blob[sizeof(struct io_uring_probe) +
            256 * sizeof(struct io_uring_probe_op)];
  struct io_uring_probe *p = (struct io_uring_probe *)blob;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("probe", "no ring", 0);
    return;
  }
  memset(blob, 0, sizeof(blob));
  int rc = io_uring_register_(r.fd, IORING_REGISTER_PROBE, blob, 256);

  if (rc < 0) {
    bad("probe", "IORING_REGISTER_PROBE refused", rc);
    ring_free(&r);
    return;
  }
  int nop_ok = 0, read_ok = 0, madvise_reported = 1;

  for (unsigned i = 0; i < p->ops_len; i++) {
    if (p->ops[i].op == IORING_OP_NOP)
      nop_ok = (p->ops[i].flags & IO_URING_OP_SUPPORTED) != 0;
    if (p->ops[i].op == IORING_OP_READ)
      read_ok = (p->ops[i].flags & IO_URING_OP_SUPPORTED) != 0;
    if (p->ops[i].op == IORING_OP_MADVISE)
      madvise_reported = (p->ops[i].flags & IO_URING_OP_SUPPORTED) == 0;
  }
  judge("probe", p->ops_len > 0 && nop_ok && read_ok && madvise_reported,
        "the probe does not tell the truth about which opcodes work",
        (long)p->ops_len);

  /* And an opcode the probe calls unsupported must say so rather than do
   * something. */
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_MADVISE;
  sqe->fd = -1;
  sqe->user_data = 61;
  rc = submit_wait(&r, 1, &cqe);
  judge("unsupported-op-is-einval",
        rc == 0 && (cqe.res == -EINVAL || cqe.res == -EBADF),
        "an unimplemented opcode did not fail cleanly",
        rc == 0 ? (long)cqe.res : (long)rc);
  ring_free(&r);
}

static void check_socket(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  int sv[2];
  char msg[] = "ring-socket";
  char in[32];

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("socket-send-recv", "no ring", 0);
    return;
  }
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    bad("socket-send-recv", "socketpair failed", -1);
    ring_free(&r);
    return;
  }

  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_SEND;
  sqe->fd = sv[0];
  sqe->addr = (unsigned long long)(unsigned long)msg;
  sqe->len = sizeof(msg);
  sqe->user_data = 71;
  int rc = submit_wait(&r, 1, &cqe);
  int sent = (rc == 0) ? cqe.res : -1;

  memset(in, 0, sizeof(in));
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_RECV;
  sqe->fd = sv[1];
  sqe->addr = (unsigned long long)(unsigned long)in;
  sqe->len = sizeof(in);
  sqe->user_data = 72;
  rc = submit_wait(&r, 1, &cqe);

  judge("socket-send-recv",
        sent == (int)sizeof(msg) && rc == 0 && cqe.res == (int)sizeof(msg) &&
            memcmp(in, msg, sizeof(msg)) == 0,
        "send and recv through the ring did not carry the message",
        rc == 0 ? (long)cqe.res : (long)sent);

  close(sv[0]);
  close(sv[1]);
  ring_free(&r);
}

/* accept and connect, over an AF_UNIX listening socket. The accept is armed
 * before anything connects, which is the case that has to work. */
static void check_accept_connect(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct sockaddr_un sa;
  const char *sockpath = "/tmp/m125.sock";
  int lfd, cfd, rc;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("accept-connect", "no ring", 0);
    return;
  }
  unlink(sockpath);
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  strncpy(sa.sun_path, sockpath, sizeof(sa.sun_path) - 1);

  lfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
      listen(lfd, 4) < 0) {
    bad("accept-connect", "could not set up the listening socket", -1);
    if (lfd >= 0)
      close(lfd);
    ring_free(&r);
    return;
  }

  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_ACCEPT;
  sqe->fd = lfd;
  sqe->addr = 0;
  sqe->addr2 = 0;
  sqe->user_data = 81;
  rc = io_uring_enter_(r.fd, 1, 0, 0, 0, 0);
  if (rc != 1) {
    bad("accept-connect", "the accept would not submit", rc);
    close(lfd);
    ring_free(&r);
    return;
  }

  cfd = socket(AF_UNIX, SOCK_STREAM, 0);
  rc = connect(cfd, (struct sockaddr *)&sa, sizeof(sa));
  if (rc < 0) {
    bad("accept-connect", "connect(2) refused", rc);
    close(cfd);
    close(lfd);
    ring_free(&r);
    return;
  }

  rc = io_uring_enter_(r.fd, 0, 1, IORING_ENTER_GETEVENTS, 0, 0);
  int got = (rc >= 0 && cq_get(&r, &cqe));
  int afd = (got && cqe.user_data == 81) ? cqe.res : -1;

  if (afd >= 0) {
    /* The accepted descriptor has to be a working socket. */
    char pong[8];

    if (write(cfd, "pong", 4) != 4 || read(afd, pong, 4) != 4 ||
        memcmp(pong, "pong", 4) != 0)
      afd = -1;
  }
  judge("accept-connect", afd >= 0,
        "the armed accept did not produce a usable connection",
        got ? (long)cqe.res : (long)rc);
  if (afd >= 0)
    close(afd);
  close(cfd);
  close(lfd);
  unlink(sockpath);
  ring_free(&r);
}

/* More completions than the ring can hold must be kept, not dropped: that is
 * what IORING_FEAT_NODROP promises. */
static void check_overflow(void) {
  struct ring r;
  struct io_uring_cqe cqe;

  if (ring_make(&r, 4, 0, 4) < 0) {
    bad("cq-overflow", "no ring", 0);
    return;
  }
  unsigned want = *r.cq_entries + 4;
  unsigned posted = 0;

  for (unsigned i = 0; i < want; i++) {
    struct io_uring_sqe *sqe = sq_get(&r);

    if (!sqe) {
      /* SQ full: drain it and carry on. */
      io_uring_enter_(r.fd, 4, 0, 0, 0, 0);
      sqe = sq_get(&r);
      if (!sqe)
        break;
    }
    sqe->opcode = IORING_OP_NOP;
    sqe->user_data = 1000 + i;
    io_uring_enter_(r.fd, 1, 0, 0, 0, 0);
    posted++;
  }

  unsigned seen = 0;

  for (int round = 0; round < 64 && seen < posted; round++) {
    while (cq_get(&r, &cqe)) {
      if (cqe.user_data == 1000 + seen)
        seen++;
      else
        seen = posted + 1000; /* out of order: fail below */
    }
    io_uring_enter_(r.fd, 0, 0, IORING_ENTER_GETEVENTS, 0, 0);
  }
  judge("cq-overflow", posted > *r.cq_entries && seen == posted,
        "completions past the ring's size were not all delivered in order",
        (long)seen);
  ring_free(&r);
}

int main(void) {
  printf("M125-SMOKE: start\n");
  fflush(stdout);

  /* If the call is absent, say so once and stop: every check below would just
   * repeat it. */
  struct io_uring_params probe;

  memset(&probe, 0, sizeof(probe));
  int fd = io_uring_setup_(1, &probe);

  if (fd < 0 && errno == ENOSYS) {
    printf("M125-SMOKE: FAIL setup — io_uring_setup is ENOSYS\n");
    printf("M125-SMOKE: done\n");
    fflush(stdout);
    return 1;
  }
  if (fd >= 0)
    close(fd);

  check_setup();
  check_nop();
  check_file_rw();
  check_pipe_async();
  check_timeout_and_cancel();
  check_links();
  check_registration();
  check_registered_buffers();
  check_probe();
  check_socket();
  check_accept_connect();
  check_overflow();

  printf("M125-SMOKE: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
