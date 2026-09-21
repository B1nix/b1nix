/* SPDX-License-Identifier: GPL-2.0-only */
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
#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

/* Opcodes and flags newer than the uapi header this is compiled against. The
 * values are the ABI's, taken from the same enum the kernel's copy holds. */
#define IOU_OP_SETXATTR         42
/* Zero-copy receive: it needs a NIC-driven refill ring (see the roadmap), so
 * it is the opcode this kernel genuinely does not have. */
#define IOU_OP_RECV_ZC          58
#define IOU_OP_SOCKET           45
#define IOU_OP_SEND_ZC          47
#define IOU_OP_WAITID           50
#define IOU_OP_FIXED_FD_INSTALL 54
#define IOU_OP_FTRUNCATE        55
#define IOU_OP_BIND             56
#define IOU_OP_LISTEN           57
#define IOU_OP_PIPE             62

#ifndef IORING_SETUP_NO_SQARRAY
#define IORING_SETUP_NO_SQARRAY (1U << 16)
#endif
/* The opcodes and register commands this kernel implements that the
 * distribution's header is older than. Their numbers are ABI. */
#ifndef IORING_OP_FSETXATTR
#define IORING_OP_FSETXATTR 41
#define IORING_OP_SETXATTR 42
#define IORING_OP_FGETXATTR 43
#define IORING_OP_GETXATTR 44
#endif
#ifndef IORING_OP_URING_CMD
#define IORING_OP_URING_CMD 46
#endif
#ifndef IORING_OP_FUTEX_WAIT
#define IORING_OP_FUTEX_WAIT 51
#define IORING_OP_FUTEX_WAKE 52
#define IORING_OP_FUTEX_WAITV 53
#endif
#ifndef IORING_OP_READV_FIXED
#define IORING_OP_READV_FIXED 60
#define IORING_OP_WRITEV_FIXED 61
#endif
#ifndef IORING_REGISTER_PERSONALITY
#define IORING_REGISTER_PERSONALITY 9
#define IORING_UNREGISTER_PERSONALITY 10
#endif
#ifndef IORING_REGISTER_RING_FDS
#define IORING_REGISTER_RING_FDS 20
#define IORING_UNREGISTER_RING_FDS 21
#endif
#ifndef IORING_ENTER_REGISTERED_RING
#define IORING_ENTER_REGISTERED_RING (1U << 4)
#endif
#ifndef SOCKET_URING_OP_SIOCINQ
#define SOCKET_URING_OP_SIOCINQ 0
#define SOCKET_URING_OP_SIOCOUTQ 1
#define SOCKET_URING_OP_GETSOCKOPT 2
#define SOCKET_URING_OP_SETSOCKOPT 3
#endif

#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif
#ifndef IORING_REGISTER_PBUF_RING
#define IORING_REGISTER_PBUF_RING 22
#define IORING_UNREGISTER_PBUF_RING 23
#endif
#ifndef IORING_REGISTER_SYNC_CANCEL
#define IORING_REGISTER_SYNC_CANCEL 24
#endif
#ifndef IORING_REGISTER_FILE_ALLOC_RANGE
#define IORING_REGISTER_FILE_ALLOC_RANGE 25
#endif
#ifndef IORING_REGISTER_PBUF_STATUS
#define IORING_REGISTER_PBUF_STATUS 26
#endif

/* struct io_uring_buf_status, which the 6.6 uapi header does not carry. */
struct iou_buf_status {
  unsigned int buf_group;
  unsigned int head;
  unsigned int resv[8];
};
#ifndef IORING_FILE_INDEX_ALLOC
#define IORING_FILE_INDEX_ALLOC (~0U)
#endif
#ifndef IORING_ACCEPT_MULTISHOT
#define IORING_ACCEPT_MULTISHOT (1U << 0)
#endif
#ifndef IORING_RECV_MULTISHOT
#define IORING_RECV_MULTISHOT (1U << 1)
#endif
#ifndef IORING_CQE_BUFFER_SHIFT
#define IORING_CQE_BUFFER_SHIFT 16
#endif

/* struct io_uring_buf / io_uring_buf_ring, in the shape the ABI fixes: the
 * ring tail is overlaid on the first entry's resv field. */
struct iou_buf_ent {
  unsigned long long addr;
  unsigned int len;
  unsigned short bid;
  unsigned short resv;
};
#define IOU_PBUF_TAIL(base) (*(volatile unsigned short *)((char *)(base) + 14))

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
   * then mishandled. IORING_SETUP_SQ_AFF asks for the submission thread to be
   * pinned to sq_thread_cpu, which this kernel cannot promise. */
  struct io_uring_params p;

  memset(&p, 0, sizeof(p));
  p.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
  int fd = io_uring_setup_(8, &p);

  judge("refuses-sq-aff", fd < 0 && errno == EINVAL,
        "IORING_SETUP_SQ_AFF was accepted", (long)fd);
  if (fd >= 0)
    close(fd);

  memset(&p, 0, sizeof(p));
  p.flags = IORING_SETUP_ATTACH_WQ;
  fd = io_uring_setup_(8, &p);
  judge("refuses-attach-wq", fd < 0 && errno == EINVAL,
        "IORING_SETUP_ATTACH_WQ was accepted", (long)fd);
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
  int nop_ok = 0, read_ok = 0, openat_ok = 0, statx_ok = 0, pbuf_ok = 0;
  int xattr_ok = 0;
  int zc_reported = 1;

  for (unsigned i = 0; i < p->ops_len; i++) {
    if (p->ops[i].op == IORING_OP_NOP)
      nop_ok = (p->ops[i].flags & IO_URING_OP_SUPPORTED) != 0;
    if (p->ops[i].op == IORING_OP_READ)
      read_ok = (p->ops[i].flags & IO_URING_OP_SUPPORTED) != 0;
    if (p->ops[i].op == IORING_OP_OPENAT)
      openat_ok = (p->ops[i].flags & IO_URING_OP_SUPPORTED) != 0;
    if (p->ops[i].op == IORING_OP_STATX)
      statx_ok = (p->ops[i].flags & IO_URING_OP_SUPPORTED) != 0;
    if (p->ops[i].op == IORING_OP_PROVIDE_BUFFERS)
      pbuf_ok = (p->ops[i].flags & IO_URING_OP_SUPPORTED) != 0;
    /* The xattr opcodes are implemented, so the probe must say so... */
    if (p->ops[i].op == IOU_OP_SETXATTR)
      xattr_ok = (p->ops[i].flags & IO_URING_OP_SUPPORTED) != 0;
    /* ... and zero-copy receive is not, so it must say that too. */
    if (p->ops[i].op == IOU_OP_RECV_ZC)
      zc_reported = (p->ops[i].flags & IO_URING_OP_SUPPORTED) == 0;
  }
  judge("probe",
        p->ops_len > 0 && nop_ok && read_ok && openat_ok && statx_ok &&
            pbuf_ok && xattr_ok && zc_reported,
        "the probe does not tell the truth about which opcodes work",
        (long)p->ops_len);

  /* And an opcode the probe calls unsupported must say so rather than do
   * something. */
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IOU_OP_RECV_ZC;
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

/* ---- the checks added when the refused features were implemented -------- */

/* A ring shape helper that also reports the CQE stride, so IORING_SETUP_CQE32
 * can be walked correctly. */
static struct io_uring_cqe *cqe_at(struct ring *r, unsigned idx,
                                   size_t stride) {
  return (struct io_uring_cqe *)((char *)r->cqes +
                                 (size_t)(idx & *r->cq_mask) * stride);
}

static void check_sqpoll(void) {
  struct io_uring_params p;
  struct ring r;
  int i;

  memset(&r, 0, sizeof(r));
  memset(&p, 0, sizeof(p));
  p.flags = IORING_SETUP_SQPOLL;
  p.sq_thread_idle = 50;
  r.p = p;
  r.fd = io_uring_setup_(8, &r.p);
  if (r.fd < 0) {
    bad("sqpoll-setup", "IORING_SETUP_SQPOLL was refused", (long)r.fd);
    return;
  }
  r.sq_sz = r.p.sq_off.array + r.p.sq_entries * sizeof(unsigned);
  size_t cq_sz =
      r.p.cq_off.cqes + r.p.cq_entries * sizeof(struct io_uring_cqe);

  if (cq_sz > r.sq_sz)
    r.sq_sz = cq_sz;
  r.sq_ptr = mmap(0, r.sq_sz, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_POPULATE, r.fd, IORING_OFF_SQ_RING);
  r.sqes_sz = r.p.sq_entries * sizeof(struct io_uring_sqe);
  r.sqes = mmap(0, r.sqes_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                r.fd, IORING_OFF_SQES);
  if (r.sq_ptr == MAP_FAILED || r.sqes == MAP_FAILED) {
    bad("sqpoll-setup", "the SQPOLL ring would not map", -1);
    close(r.fd);
    return;
  }
  char *b = (char *)r.sq_ptr;

  r.sq_head = (unsigned *)(b + r.p.sq_off.head);
  r.sq_tail = (unsigned *)(b + r.p.sq_off.tail);
  r.sq_mask = (unsigned *)(b + r.p.sq_off.ring_mask);
  r.sq_entries = (unsigned *)(b + r.p.sq_off.ring_entries);
  r.sq_flags = (unsigned *)(b + r.p.sq_off.flags);
  r.sq_dropped = (unsigned *)(b + r.p.sq_off.dropped);
  r.sq_array = (unsigned *)(b + r.p.sq_off.array);
  r.cq_head = (unsigned *)(b + r.p.cq_off.head);
  r.cq_tail = (unsigned *)(b + r.p.cq_off.tail);
  r.cq_mask = (unsigned *)(b + r.p.cq_off.ring_mask);
  r.cq_entries = (unsigned *)(b + r.p.cq_off.ring_entries);
  r.cq_overflow = (unsigned *)(b + r.p.cq_off.overflow);
  r.cqes = (struct io_uring_cqe *)(b + r.p.cq_off.cqes);
  ok("sqpoll-setup");

  /* The whole point: a submission with NO io_uring_enter at all. */
  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_NOP;
  sqe->user_data = 0x59504cull;

  struct io_uring_cqe cqe;
  int got = 0;

  for (i = 0; i < 4000 && !got; i++) {
    got = cq_get(&r, &cqe);
    if (!got)
      usleep(1000);
  }
  judge("sqpoll-submits-without-enter",
        got && cqe.user_data == 0x59504cull && cqe.res == 0,
        "the submission thread did not consume the queue on its own",
        got ? (long)cqe.res : -1);

  /* After sq_thread_idle with nothing to do the thread must publish
   * IORING_SQ_NEED_WAKEUP, and a wakeup through io_uring_enter must get it
   * working again. */
  int saw_need_wakeup = 0;

  for (i = 0; i < 4000 && !saw_need_wakeup; i++) {
    if (__atomic_load_n(r.sq_flags, __ATOMIC_ACQUIRE) & IORING_SQ_NEED_WAKEUP)
      saw_need_wakeup = 1;
    else
      usleep(1000);
  }
  judge("sqpoll-need-wakeup", saw_need_wakeup,
        "IORING_SQ_NEED_WAKEUP was never published after the idle period", 0);

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_NOP;
  sqe->user_data = 0x59504dull;
  int rc = io_uring_enter_(r.fd, 1, 0, IORING_ENTER_SQ_WAKEUP, 0, 0);

  got = 0;
  for (i = 0; i < 4000 && !got; i++) {
    got = cq_get(&r, &cqe);
    if (!got)
      usleep(1000);
  }
  judge("sqpoll-wakeup", rc >= 0 && got && cqe.user_data == 0x59504dull,
        "IORING_ENTER_SQ_WAKEUP did not restart the submission thread",
        got ? (long)cqe.user_data : (long)rc);

  /* And it must move real data, which means it is running in the owner's
   * address space and can reach the owner's descriptors. */
  int pfd[2];

  if (pipe(pfd) == 0) {
    static char src[64] = "sqpoll-carried-this";
    char dst[64];

    memset(dst, 0, sizeof(dst));
    sqe = sq_get(&r);
    sqe->opcode = IORING_OP_WRITE;
    sqe->fd = pfd[1];
    sqe->addr = (unsigned long long)(uintptr_t)src;
    sqe->len = 20;
    sqe->off = (unsigned long long)-1;
    sqe->user_data = 0x77;
    io_uring_enter_(r.fd, 1, 0, IORING_ENTER_SQ_WAKEUP, 0, 0);
    got = 0;
    for (i = 0; i < 4000 && !got; i++) {
      got = cq_get(&r, &cqe);
      if (!got)
        usleep(1000);
    }
    int n = (int)read(pfd[0], dst, sizeof(dst));

    judge("sqpoll-moves-data",
          got && cqe.res == 20 && n == 20 && memcmp(dst, src, 20) == 0,
          "the submission thread could not reach the owner's memory or fds",
          got ? (long)cqe.res : -1);
    close(pfd[0]);
    close(pfd[1]);
  } else {
    bad("sqpoll-moves-data", "pipe failed", -1);
  }

  munmap(r.sqes, r.sqes_sz);
  munmap(r.sq_ptr, r.sq_sz);
  close(r.fd);
}

static void check_iopoll(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  const char *path = "/tmp/m125-iopoll";
  char buf[64], in[64];
  int fd;

  if (ring_make(&r, 8, IORING_SETUP_IOPOLL, 0) < 0) {
    bad("iopoll", "IORING_SETUP_IOPOLL was refused", -1);
    return;
  }
  fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    bad("iopoll", "no file", -1);
    ring_free(&r);
    return;
  }
  memset(buf, 0, sizeof(buf));
  strcpy(buf, "iopoll-data");
  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_WRITE;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(uintptr_t)buf;
  sqe->len = 16;
  sqe->off = 0;
  sqe->user_data = 0x10;
  int rc = submit_wait(&r, 1, &cqe);
  int wrote = (rc == 0 && cqe.res == 16);

  memset(in, 0, sizeof(in));
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_READ;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(uintptr_t)in;
  sqe->len = 16;
  sqe->off = 0;
  sqe->user_data = 0x11;
  rc = submit_wait(&r, 1, &cqe);
  judge("iopoll",
        wrote && rc == 0 && cqe.res == 16 && memcmp(in, buf, 16) == 0,
        "an IOPOLL ring did not move data reaped from GETEVENTS",
        rc == 0 ? (long)cqe.res : (long)rc);
  close(fd);
  unlink(path);
  ring_free(&r);
}

static void check_provided_buffers(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  static char pool[4 * 64];
  int sv[2];

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("provide-buffers", "no ring", 0);
    return;
  }
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    bad("provide-buffers", "socketpair failed", -1);
    ring_free(&r);
    return;
  }
  memset(pool, 0, sizeof(pool));

  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_PROVIDE_BUFFERS;
  sqe->fd = 4;                    /* how many */
  sqe->addr = (unsigned long long)(uintptr_t)pool;
  sqe->len = 64;                  /* each */
  sqe->off = 100;                 /* first buffer id */
  sqe->buf_group = 7;
  sqe->user_data = 0x20;
  int rc = submit_wait(&r, 1, &cqe);

  judge("provide-buffers", rc == 0 && cqe.res == 0,
        "IORING_OP_PROVIDE_BUFFERS did not take the buffers",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* A receive that chooses one of them. */
  const char *msg = "chosen-buffer";

  write(sv[1], msg, 14);
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_RECV;
  sqe->fd = sv[0];
  sqe->len = 0; /* take the whole buffer */
  sqe->flags = IOSQE_BUFFER_SELECT;
  sqe->buf_group = 7;
  sqe->user_data = 0x21;
  rc = submit_wait(&r, 1, &cqe);
  int bid = (int)(cqe.flags >> IORING_CQE_BUFFER_SHIFT);
  int have_flag = (cqe.flags & IORING_CQE_F_BUFFER) != 0;

  judge("buffer-select",
        rc == 0 && cqe.res == 14 && have_flag && bid == 100 &&
            memcmp(pool, msg, 14) == 0,
        "the chosen buffer was not reported or not the one written into",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* The next one must be a different buffer. */
  write(sv[1], "second", 7);
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_RECV;
  sqe->fd = sv[0];
  sqe->len = 0;
  sqe->flags = IOSQE_BUFFER_SELECT;
  sqe->buf_group = 7;
  sqe->user_data = 0x22;
  rc = submit_wait(&r, 1, &cqe);
  judge("buffer-select-advances",
        rc == 0 && cqe.res == 7 &&
            (int)(cqe.flags >> IORING_CQE_BUFFER_SHIFT) == 101 &&
            memcmp(pool + 64, "second", 7) == 0,
        "the second selection did not move on to the next buffer",
        rc == 0 ? (long)(cqe.flags >> IORING_CQE_BUFFER_SHIFT) : (long)rc);

  /* Take the remaining two away and prove the group is then empty. */
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_REMOVE_BUFFERS;
  sqe->fd = 2;
  sqe->buf_group = 7;
  sqe->user_data = 0x23;
  rc = submit_wait(&r, 1, &cqe);
  int removed = (rc == 0 && cqe.res == 2);

  write(sv[1], "third", 6);
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_RECV;
  sqe->fd = sv[0];
  sqe->len = 0;
  sqe->flags = IOSQE_BUFFER_SELECT;
  sqe->buf_group = 7;
  sqe->user_data = 0x24;
  rc = submit_wait(&r, 1, &cqe);
  judge("remove-buffers", removed && rc == 0 && cqe.res == -ENOBUFS,
        "an empty buffer group did not answer -ENOBUFS",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* Drain what is still queued on the socket so the next check starts clean. */
  char sink[64];

  recv(sv[0], sink, sizeof(sink), MSG_DONTWAIT);
  close(sv[0]);
  close(sv[1]);
  ring_free(&r);
}

static void check_buffer_ring(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  int sv[2];
  void *bring;
  static char pool[4 * 64];
  const unsigned entries = 4;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("pbuf-ring", "no ring", 0);
    return;
  }
  bring = mmap(0, 4096, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (bring == MAP_FAILED) {
    bad("pbuf-ring", "no memory for the buffer ring", -1);
    ring_free(&r);
    return;
  }
  memset(bring, 0, 4096);
  memset(pool, 0, sizeof(pool));

  struct io_uring_buf_reg reg;

  memset(&reg, 0, sizeof(reg));
  reg.ring_addr = (unsigned long long)(uintptr_t)bring;
  reg.ring_entries = entries;
  reg.bgid = 9;
  int rc = io_uring_register_(r.fd, IORING_REGISTER_PBUF_RING, &reg, 1);

  judge("pbuf-ring-register", rc == 0,
        "IORING_REGISTER_PBUF_RING was refused", (long)rc);
  if (rc < 0) {
    munmap(bring, 4096);
    ring_free(&r);
    return;
  }

  /* Publish two buffers the way the ABI says: write the entries, then the
   * tail. */
  struct iou_buf_ent *ents = (struct iou_buf_ent *)bring;

  for (unsigned i = 0; i < 2; i++) {
    ents[i].addr = (unsigned long long)(uintptr_t)(pool + i * 64);
    ents[i].len = 64;
    ents[i].bid = (unsigned short)(200 + i);
  }
  __atomic_store_n(&IOU_PBUF_TAIL(bring), (unsigned short)2, __ATOMIC_RELEASE);

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    bad("pbuf-ring-recv", "socketpair failed", -1);
    munmap(bring, 4096);
    ring_free(&r);
    return;
  }
  write(sv[1], "ring-buffer", 12);

  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_RECV;
  sqe->fd = sv[0];
  sqe->len = 0;
  sqe->flags = IOSQE_BUFFER_SELECT;
  sqe->buf_group = 9;
  sqe->user_data = 0x30;
  rc = submit_wait(&r, 1, &cqe);
  judge("pbuf-ring-recv",
        rc == 0 && cqe.res == 12 && (cqe.flags & IORING_CQE_F_BUFFER) &&
            (int)(cqe.flags >> IORING_CQE_BUFFER_SHIFT) == 200 &&
            memcmp(pool, "ring-buffer", 12) == 0,
        "the receive did not take the first entry of the buffer ring",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* The kernel's head must have moved by exactly one. */
  struct iou_buf_status st;

  memset(&st, 0, sizeof(st));
  st.buf_group = 9;
  rc = io_uring_register_(r.fd, IORING_REGISTER_PBUF_STATUS, &st, 1);
  judge("pbuf-ring-status", rc == 0 && st.head == 1,
        "the buffer ring head is not where the one consumed buffer left it",
        rc == 0 ? (long)st.head : (long)rc);

  rc = io_uring_register_(r.fd, IORING_UNREGISTER_PBUF_RING, &reg, 1);
  judge("pbuf-ring-unregister", rc == 0,
        "IORING_UNREGISTER_PBUF_RING was refused", (long)rc);

  close(sv[0]);
  close(sv[1]);
  munmap(bring, 4096);
  ring_free(&r);
}

static void check_multishot(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  int pfd[2];

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("multishot-poll", "no ring", 0);
    return;
  }
  if (pipe(pfd) < 0) {
    bad("multishot-poll", "pipe failed", -1);
    ring_free(&r);
    return;
  }
  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_POLL_ADD;
  sqe->fd = pfd[0];
  sqe->poll32_events = POLLIN;
  sqe->len = IORING_POLL_ADD_MULTI;
  sqe->user_data = 0x40;
  int rc = io_uring_enter_(r.fd, 1, 0, 0, 0, 0);
  int reports = 0, all_more = 1;
  char c;

  for (int round = 0; round < 2; round++) {
    write(pfd[1], "x", 1);
    if (io_uring_enter_(r.fd, 0, 1, IORING_ENTER_GETEVENTS, 0, 0) < 0)
      break;
    if (!cq_get(&r, &cqe))
      break;
    if (cqe.user_data != 0x40 || !(cqe.res & POLLIN))
      break;
    if (!(cqe.flags & IORING_CQE_F_MORE))
      all_more = 0;
    reports++;
    /* Consume the readiness so the next write is a fresh edge. */
    if (read(pfd[0], &c, 1) != 1)
      break;
    /* Let the sweep see the pipe empty again. */
    io_uring_enter_(r.fd, 0, 0, 0, 0, 0);
  }
  judge("multishot-poll", rc >= 0 && reports == 2 && all_more,
        "a multishot poll did not report twice with IORING_CQE_F_MORE",
        (long)reports);

  /* Cancelling it ends the series. */
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_ASYNC_CANCEL;
  sqe->addr = 0x40;
  sqe->user_data = 0x41;
  rc = io_uring_enter_(r.fd, 1, 2, IORING_ENTER_GETEVENTS, 0, 0);
  int saw_cancel = 0, saw_final = 0;

  while (cq_get(&r, &cqe)) {
    if (cqe.user_data == 0x41 && cqe.res == 0)
      saw_cancel = 1;
    if (cqe.user_data == 0x40 && cqe.res == -ECANCELED &&
        !(cqe.flags & IORING_CQE_F_MORE))
      saw_final = 1;
  }
  judge("multishot-poll-cancel", rc >= 0 && saw_cancel && saw_final,
        "cancelling a multishot poll did not end it without CQE_F_MORE",
        (long)rc);
  close(pfd[0]);
  close(pfd[1]);
  ring_free(&r);
}

static void check_multishot_recv(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  static char pool[4 * 32];
  int sv[2];

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("multishot-recv", "no ring", 0);
    return;
  }
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    bad("multishot-recv", "socketpair failed", -1);
    ring_free(&r);
    return;
  }
  memset(pool, 0, sizeof(pool));

  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_PROVIDE_BUFFERS;
  sqe->fd = 4;
  sqe->addr = (unsigned long long)(uintptr_t)pool;
  sqe->len = 32;
  sqe->off = 0;
  sqe->buf_group = 11;
  sqe->user_data = 0x50;
  int rc = submit_wait(&r, 1, &cqe);

  if (rc < 0 || cqe.res != 0) {
    bad("multishot-recv", "the buffers would not go in",
        rc == 0 ? (long)cqe.res : (long)rc);
    close(sv[0]);
    close(sv[1]);
    ring_free(&r);
    return;
  }

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_RECV;
  sqe->fd = sv[0];
  sqe->len = 0;
  sqe->ioprio = IORING_RECV_MULTISHOT;
  sqe->flags = IOSQE_BUFFER_SELECT;
  sqe->buf_group = 11;
  sqe->user_data = 0x51;
  io_uring_enter_(r.fd, 1, 0, 0, 0, 0);

  int got = 0, all_more = 1;
  int bids[2] = {-1, -1};

  for (int round = 0; round < 2; round++) {
    char m[8];

    snprintf(m, sizeof(m), "ms%d", round);
    write(sv[1], m, 4);
    if (io_uring_enter_(r.fd, 0, 1, IORING_ENTER_GETEVENTS, 0, 0) < 0)
      break;
    if (!cq_get(&r, &cqe) || cqe.user_data != 0x51 || cqe.res != 4)
      break;
    if (!(cqe.flags & IORING_CQE_F_MORE))
      all_more = 0;
    bids[round] = (int)(cqe.flags >> IORING_CQE_BUFFER_SHIFT);
    got++;
  }
  judge("multishot-recv",
        got == 2 && all_more && bids[0] == 0 && bids[1] == 1 &&
            memcmp(pool, "ms0", 3) == 0 && memcmp(pool + 32, "ms1", 3) == 0,
        "a multishot receive did not deliver twice into two chosen buffers",
        (long)got);
  close(sv[0]);
  close(sv[1]);
  ring_free(&r);
}

static void check_multishot_accept(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct sockaddr_un sa;
  const char *sock = "/tmp/m125-msaccept";
  int srv;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("multishot-accept", "no ring", 0);
    return;
  }
  unlink(sock);
  srv = socket(AF_UNIX, SOCK_STREAM, 0);
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  strncpy(sa.sun_path, sock, sizeof(sa.sun_path) - 1);
  if (srv < 0 || bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
      listen(srv, 4) < 0) {
    bad("multishot-accept", "the listening socket would not come up", -1);
    if (srv >= 0)
      close(srv);
    ring_free(&r);
    return;
  }

  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_ACCEPT;
  sqe->fd = srv;
  sqe->ioprio = IORING_ACCEPT_MULTISHOT;
  sqe->user_data = 0x60;
  io_uring_enter_(r.fd, 1, 0, 0, 0, 0);

  int accepted = 0, all_more = 1;
  int cfd[2] = {-1, -1};

  for (int i = 0; i < 2; i++) {
    cfd[i] = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cfd[i] < 0 || connect(cfd[i], (struct sockaddr *)&sa, sizeof(sa)) < 0)
      break;
    if (io_uring_enter_(r.fd, 0, 1, IORING_ENTER_GETEVENTS, 0, 0) < 0)
      break;
    if (!cq_get(&r, &cqe) || cqe.user_data != 0x60 || cqe.res < 0)
      break;
    if (!(cqe.flags & IORING_CQE_F_MORE))
      all_more = 0;
    /* Prove the accepted descriptor is a working connection. */
    {
      char m[8] = "hi";
      char in[8];

      memset(in, 0, sizeof(in));
      if (write(cfd[i], m, 3) == 3 && read(cqe.res, in, 3) == 3 &&
          memcmp(in, "hi", 3) == 0)
        accepted++;
    }
    close(cqe.res);
  }
  judge("multishot-accept", accepted == 2 && all_more,
        "a multishot accept did not take two connections with CQE_F_MORE",
        (long)accepted);
  for (int i = 0; i < 2; i++)
    if (cfd[i] >= 0)
      close(cfd[i]);
  close(srv);
  unlink(sock);
  ring_free(&r);
}

static void check_fs_opcodes(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe;
  const char *dir = "/tmp/m125-fs";
  const char *file = "/tmp/m125-fs/a";
  const char *file2 = "/tmp/m125-fs/b";
  const char *link = "/tmp/m125-fs/l";
  int rc;

  if (ring_make(&r, 16, 0, 0) < 0) {
    bad("op-mkdirat", "no ring", 0);
    return;
  }
  unlink(link);
  unlink(file);
  unlink(file2);
  rmdir(dir);

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_MKDIRAT;
  sqe->fd = AT_FDCWD;
  sqe->addr = (unsigned long long)(uintptr_t)dir;
  sqe->len = 0755;
  sqe->user_data = 1;
  rc = submit_wait(&r, 1, &cqe);
  struct stat stbuf;

  judge("op-mkdirat",
        rc == 0 && cqe.res == 0 && stat(dir, &stbuf) == 0 &&
            S_ISDIR(stbuf.st_mode),
        "IORING_OP_MKDIRAT did not make the directory",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* OPENAT creating a file, then a write through the descriptor it returned. */
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_OPENAT;
  sqe->fd = AT_FDCWD;
  sqe->addr = (unsigned long long)(uintptr_t)file;
  sqe->open_flags = O_RDWR | O_CREAT | O_TRUNC;
  sqe->len = 0644;
  sqe->user_data = 2;
  rc = submit_wait(&r, 1, &cqe);
  int ofd = (rc == 0) ? cqe.res : -1;
  int wrote = 0;

  if (ofd >= 0)
    wrote = (write(ofd, "opened-by-ring", 15) == 15);
  judge("op-openat", ofd >= 0 && wrote,
        "IORING_OP_OPENAT did not return a usable descriptor",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* STATX on the same path must report the size just written. */
  struct statx sx;

  memset(&sx, 0, sizeof(sx));
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_STATX;
  sqe->fd = AT_FDCWD;
  sqe->addr = (unsigned long long)(uintptr_t)file;
  sqe->len = STATX_BASIC_STATS;
  sqe->off = (unsigned long long)(uintptr_t)&sx;
  sqe->user_data = 3;
  rc = submit_wait(&r, 1, &cqe);
  judge("op-statx", rc == 0 && cqe.res == 0 && sx.stx_size == 15,
        "IORING_OP_STATX did not report the file it was asked about",
        rc == 0 ? (long)sx.stx_size : (long)rc);

  /* FTRUNCATE, then STATX again. */
  sqe = sq_get(&r);
  sqe->opcode = IOU_OP_FTRUNCATE;
  sqe->fd = ofd;
  sqe->off = 8;
  sqe->user_data = 4;
  rc = submit_wait(&r, 1, &cqe);
  int truncated = (rc == 0 && cqe.res == 0 && stat(file, &stbuf) == 0 &&
                   stbuf.st_size == 8);

  judge("op-ftruncate", truncated,
        "IORING_OP_FTRUNCATE did not shorten the file",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* FALLOCATE grows it back. */
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_FALLOCATE;
  sqe->fd = ofd;
  sqe->off = 0;
  sqe->addr = 4096;
  sqe->len = 0;
  sqe->user_data = 5;
  rc = submit_wait(&r, 1, &cqe);
  judge("op-fallocate",
        rc == 0 && cqe.res == 0 && stat(file, &stbuf) == 0 &&
            stbuf.st_size == 4096,
        "IORING_OP_FALLOCATE did not allocate the range",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* RENAMEAT, LINKAT, SYMLINKAT, UNLINKAT. */
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_RENAMEAT;
  sqe->fd = AT_FDCWD;
  sqe->addr = (unsigned long long)(uintptr_t)file;
  sqe->len = AT_FDCWD;
  sqe->addr2 = (unsigned long long)(uintptr_t)file2;
  sqe->user_data = 6;
  rc = submit_wait(&r, 1, &cqe);
  judge("op-renameat",
        rc == 0 && cqe.res == 0 && stat(file2, &stbuf) == 0 &&
            stat(file, &stbuf) < 0,
        "IORING_OP_RENAMEAT did not move the file",
        rc == 0 ? (long)cqe.res : (long)rc);

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_LINKAT;
  sqe->fd = AT_FDCWD;
  sqe->addr = (unsigned long long)(uintptr_t)file2;
  sqe->len = AT_FDCWD;
  sqe->addr2 = (unsigned long long)(uintptr_t)file;
  sqe->user_data = 7;
  rc = submit_wait(&r, 1, &cqe);
  judge("op-linkat",
        rc == 0 && cqe.res == 0 && stat(file, &stbuf) == 0 &&
            stbuf.st_nlink == 2,
        "IORING_OP_LINKAT did not make a second name for the file",
        rc == 0 ? (long)cqe.res : (long)rc);

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_SYMLINKAT;
  sqe->fd = AT_FDCWD;
  sqe->addr = (unsigned long long)(uintptr_t)"b";
  sqe->addr2 = (unsigned long long)(uintptr_t)link;
  sqe->user_data = 8;
  rc = submit_wait(&r, 1, &cqe);
  char lbuf[8];
  ssize_t ln = readlink(link, lbuf, sizeof(lbuf));

  judge("op-symlinkat",
        rc == 0 && cqe.res == 0 && ln == 1 && lbuf[0] == 'b',
        "IORING_OP_SYMLINKAT did not make the link it was asked for",
        rc == 0 ? (long)cqe.res : (long)rc);

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_UNLINKAT;
  sqe->fd = AT_FDCWD;
  sqe->addr = (unsigned long long)(uintptr_t)file;
  sqe->user_data = 9;
  rc = submit_wait(&r, 1, &cqe);
  judge("op-unlinkat", rc == 0 && cqe.res == 0 && stat(file, &stbuf) < 0,
        "IORING_OP_UNLINKAT did not remove the name",
        rc == 0 ? (long)cqe.res : (long)rc);

  if (ofd >= 0)
    close(ofd);
  unlink(link);
  unlink(file2);
  rmdir(dir);
  ring_free(&r);
}

static void check_net_opcodes(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe;
  struct sockaddr_un sa;
  const char *sock = "/tmp/m125-net";
  int rc;

  if (ring_make(&r, 16, 0, 0) < 0) {
    bad("op-socket", "no ring", 0);
    return;
  }
  unlink(sock);

  /* SOCKET, BIND and LISTEN, all through the ring. */
  sqe = sq_get(&r);
  sqe->opcode = IOU_OP_SOCKET;
  sqe->fd = AF_UNIX;
  sqe->off = SOCK_STREAM;
  sqe->len = 0;
  sqe->user_data = 1;
  rc = submit_wait(&r, 1, &cqe);
  int srv = (rc == 0) ? cqe.res : -1;

  judge("op-socket", srv >= 0, "IORING_OP_SOCKET did not return a socket",
        rc == 0 ? (long)cqe.res : (long)rc);
  if (srv < 0) {
    ring_free(&r);
    return;
  }
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  strncpy(sa.sun_path, sock, sizeof(sa.sun_path) - 1);

  sqe = sq_get(&r);
  sqe->opcode = IOU_OP_BIND;
  sqe->fd = srv;
  sqe->addr = (unsigned long long)(uintptr_t)&sa;
  sqe->addr2 = sizeof(sa);
  sqe->user_data = 2;
  rc = submit_wait(&r, 1, &cqe);
  int bound = (rc == 0 && cqe.res == 0);

  sqe = sq_get(&r);
  sqe->opcode = IOU_OP_LISTEN;
  sqe->fd = srv;
  sqe->len = 4;
  sqe->user_data = 3;
  rc = submit_wait(&r, 1, &cqe);
  int listening = (rc == 0 && cqe.res == 0);

  int cli = socket(AF_UNIX, SOCK_STREAM, 0);
  int connected = (cli >= 0 &&
                   connect(cli, (struct sockaddr *)&sa, sizeof(sa)) == 0);

  judge("op-bind-listen", bound && listening && connected,
        "a socket bound and listened through the ring did not accept a client",
        (long)(bound * 4 + listening * 2 + connected));

  /* SHUTDOWN on the client end: the server's accepted socket then reads EOF. */
  int acc = accept(srv, 0, 0);

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_SHUTDOWN;
  sqe->fd = cli;
  sqe->len = SHUT_WR;
  sqe->user_data = 4;
  rc = submit_wait(&r, 1, &cqe);
  char sink[4];
  int eof = (acc >= 0 && read(acc, sink, sizeof(sink)) == 0);

  judge("op-shutdown", rc == 0 && cqe.res == 0 && eof,
        "IORING_OP_SHUTDOWN did not close the writing direction",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* SENDMSG and RECVMSG over a fresh pair. */
  int sv[2];

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
    char out[] = "msghdr-carried";
    char in[32];
    struct iovec iov;
    struct msghdr mh;

    memset(in, 0, sizeof(in));
    iov.iov_base = out;
    iov.iov_len = 15;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;

    sqe = sq_get(&r);
    sqe->opcode = IORING_OP_SENDMSG;
    sqe->fd = sv[1];
    sqe->addr = (unsigned long long)(uintptr_t)&mh;
    sqe->user_data = 5;
    rc = submit_wait(&r, 1, &cqe);
    int sent = (rc == 0 && cqe.res == 15);

    struct iovec riov;
    struct msghdr rmh;

    riov.iov_base = in;
    riov.iov_len = sizeof(in);
    memset(&rmh, 0, sizeof(rmh));
    rmh.msg_iov = &riov;
    rmh.msg_iovlen = 1;
    sqe = sq_get(&r);
    sqe->opcode = IORING_OP_RECVMSG;
    sqe->fd = sv[0];
    sqe->addr = (unsigned long long)(uintptr_t)&rmh;
    sqe->user_data = 6;
    rc = submit_wait(&r, 1, &cqe);
    judge("op-sendmsg-recvmsg",
          sent && rc == 0 && cqe.res == 15 && memcmp(in, out, 15) == 0,
          "a message did not survive SENDMSG into RECVMSG",
          rc == 0 ? (long)cqe.res : (long)rc);

    /* SEND_ZC posts two completions: the transfer with CQE_F_MORE and the
     * notification. */
    sqe = sq_get(&r);
    sqe->opcode = IOU_OP_SEND_ZC;
    sqe->fd = sv[1];
    sqe->addr = (unsigned long long)(uintptr_t)out;
    sqe->len = 15;
    sqe->user_data = 7;
    rc = io_uring_enter_(r.fd, 1, 2, IORING_ENTER_GETEVENTS, 0, 0);
    int data_cqe = 0, notif_cqe = 0;

    while (cq_get(&r, &cqe)) {
      if (cqe.user_data != 7)
        continue;
      if (cqe.flags & IORING_CQE_F_NOTIF)
        notif_cqe = 1;
      else if (cqe.res == 15 && (cqe.flags & IORING_CQE_F_MORE))
        data_cqe = 1;
    }
    memset(in, 0, sizeof(in));
    int back = (int)read(sv[0], in, sizeof(in));

    judge("op-send-zc",
          rc >= 0 && data_cqe && notif_cqe && back == 15 &&
              memcmp(in, out, 15) == 0,
          "SEND_ZC did not post both completions or did not send the data",
          (long)(data_cqe * 2 + notif_cqe));
    close(sv[0]);
    close(sv[1]);
  } else {
    bad("op-sendmsg-recvmsg", "socketpair failed", -1);
    bad("op-send-zc", "socketpair failed", -1);
  }

  if (acc >= 0)
    close(acc);
  close(cli);
  close(srv);
  unlink(sock);
  ring_free(&r);
}

static void check_misc_opcodes(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe;
  int rc;

  if (ring_make(&r, 16, 0, 0) < 0) {
    bad("op-splice", "no ring", 0);
    return;
  }

  /* SPLICE: a pipe into a file. */
  {
    const char *path = "/tmp/m125-splice";
    int pfd[2], fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);

    if (pipe(pfd) == 0 && fd >= 0) {
      char check[32];

      write(pfd[1], "spliced-bytes", 14);
      sqe = sq_get(&r);
      sqe->opcode = IORING_OP_SPLICE;
      sqe->splice_fd_in = pfd[0];
      sqe->splice_off_in = (unsigned long long)-1;
      sqe->fd = fd;
      sqe->off = 0;
      sqe->len = 14;
      sqe->user_data = 1;
      rc = submit_wait(&r, 1, &cqe);
      memset(check, 0, sizeof(check));
      int n = (int)pread(fd, check, 14, 0);

      judge("op-splice",
            rc == 0 && cqe.res == 14 && n == 14 &&
                memcmp(check, "spliced-bytes", 14) == 0,
            "IORING_OP_SPLICE did not move the pipe's bytes into the file",
            rc == 0 ? (long)cqe.res : (long)rc);
      close(pfd[0]);
      close(pfd[1]);
    } else {
      bad("op-splice", "no pipe or file", -1);
    }
    if (fd >= 0)
      close(fd);
    unlink(path);
  }

  /* EPOLL_CTL: add a pipe read end and prove epoll_wait then sees it. */
  {
    int ep = epoll_create1(0);
    int pfd[2];
    struct epoll_event ev;
    struct epoll_event out[2];

    if (ep >= 0 && pipe(pfd) == 0) {
      memset(&ev, 0, sizeof(ev));
      ev.events = EPOLLIN;
      ev.data.u64 = 0xe0ull;
      sqe = sq_get(&r);
      sqe->opcode = IORING_OP_EPOLL_CTL;
      sqe->fd = ep;
      sqe->len = EPOLL_CTL_ADD;
      sqe->off = (unsigned long long)pfd[0];
      sqe->addr = (unsigned long long)(uintptr_t)&ev;
      sqe->user_data = 2;
      rc = submit_wait(&r, 1, &cqe);
      write(pfd[1], "e", 1);
      int n = epoll_wait(ep, out, 2, 1000);

      judge("op-epoll-ctl",
            rc == 0 && cqe.res == 0 && n == 1 && out[0].data.u64 == 0xe0ull,
            "IORING_OP_EPOLL_CTL did not register the descriptor",
            rc == 0 ? (long)cqe.res : (long)rc);
      close(pfd[0]);
      close(pfd[1]);
    } else {
      bad("op-epoll-ctl", "no epoll or pipe", -1);
    }
    if (ep >= 0)
      close(ep);
  }

  /* MSG_RING: a completion posted into a second ring. */
  {
    struct ring r2;

    if (ring_make(&r2, 8, 0, 0) == 0) {
      sqe = sq_get(&r);
      sqe->opcode = IORING_OP_MSG_RING;
      sqe->fd = r2.fd;
      sqe->addr = 0; /* IORING_MSG_DATA */
      sqe->len = 0x1234;
      sqe->off = 0xfeedull;
      sqe->user_data = 3;
      rc = submit_wait(&r, 1, &cqe);
      int sender_ok = (rc == 0 && cqe.res == 0);
      struct io_uring_cqe c2;
      int got = cq_get(&r2, &c2);

      judge("op-msg-ring",
            sender_ok && got && c2.user_data == 0xfeedull &&
                c2.res == 0x1234,
            "IORING_OP_MSG_RING did not deliver into the other ring",
            got ? (long)c2.res : (long)rc);
      ring_free(&r2);
    } else {
      bad("op-msg-ring", "no second ring", -1);
    }
  }

  /* WAITID: reap a child that has already exited. */
  {
    pid_t pid = fork();

    if (pid == 0)
      _exit(7);
    if (pid > 0) {
      siginfo_t si;

      memset(&si, 0, sizeof(si));
      sqe = sq_get(&r);
      sqe->opcode = IOU_OP_WAITID;
      sqe->fd = pid;
      sqe->len = P_PID;
      sqe->file_index = WEXITED;
      sqe->addr2 = (unsigned long long)(uintptr_t)&si;
      sqe->user_data = 4;
      rc = submit_wait(&r, 1, &cqe);
      judge("op-waitid",
            rc == 0 && cqe.res == 0 && si.si_pid == pid && si.si_status == 7,
            "IORING_OP_WAITID did not report the child's exit",
            rc == 0 ? (long)cqe.res : (long)rc);
    } else {
      bad("op-waitid", "fork failed", -1);
    }
  }

  /* MADVISE and FADVISE: accepted for a sane advice, refused for nonsense. */
  {
    void *m = mmap(0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    sqe = sq_get(&r);
    sqe->opcode = IORING_OP_MADVISE;
    sqe->addr = (unsigned long long)(uintptr_t)m;
    sqe->off = 4096;
    sqe->fadvise_advice = MADV_WILLNEED;
    sqe->user_data = 5;
    rc = submit_wait(&r, 1, &cqe);
    int madv_ok = (rc == 0 && cqe.res == 0);
    long madv_res = (rc == 0) ? (long)cqe.res : (long)rc;

    int fd = open("/tmp/m125-fadvise", O_RDWR | O_CREAT | O_TRUNC, 0644);

    sqe = sq_get(&r);
    sqe->opcode = IORING_OP_FADVISE;
    sqe->fd = fd;
    sqe->off = 0;
    sqe->addr = 4096;
    sqe->fadvise_advice = POSIX_FADV_SEQUENTIAL;
    sqe->user_data = 6;
    rc = submit_wait(&r, 1, &cqe);
    int fadv_ok = (rc == 0 && cqe.res == 0);

    sqe = sq_get(&r);
    sqe->opcode = IORING_OP_FADVISE;
    sqe->fd = fd;
    sqe->fadvise_advice = 99;
    sqe->user_data = 7;
    rc = submit_wait(&r, 1, &cqe);
    int fadv_bad = (rc == 0 && cqe.res == -EINVAL);

    judge("op-madvise", madv_ok, "IORING_OP_MADVISE did not accept MADV_WILLNEED",
          (long)madv_res);
    judge("op-fadvise", fadv_ok && fadv_bad,
          "IORING_OP_FADVISE did not accept sane advice or refuse nonsense",
          (long)(fadv_ok * 2 + fadv_bad));
    if (m != MAP_FAILED)
      munmap(m, 4096);
    if (fd >= 0)
      close(fd);
    unlink("/tmp/m125-fadvise");
  }

  /* PIPE: two working descriptors out of one SQE. */
  {
    int fds[2] = {-1, -1};

    sqe = sq_get(&r);
    sqe->opcode = IOU_OP_PIPE;
    sqe->addr = (unsigned long long)(uintptr_t)fds;
    sqe->user_data = 8;
    rc = submit_wait(&r, 1, &cqe);
    char in[8];
    int worked = 0;

    if (rc == 0 && cqe.res == 0 && fds[0] >= 0 && fds[1] >= 0) {
      memset(in, 0, sizeof(in));
      worked = (write(fds[1], "pp", 3) == 3 && read(fds[0], in, 3) == 3 &&
                memcmp(in, "pp", 3) == 0);
      close(fds[0]);
      close(fds[1]);
    }
    judge("op-pipe", worked, "IORING_OP_PIPE did not produce a working pipe",
          rc == 0 ? (long)cqe.res : (long)rc);
  }

  ring_free(&r);
}

static void check_direct_descriptors(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe;
  const char *path = "/tmp/m125-direct";
  int rc;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("direct-openat", "no ring", 0);
    return;
  }
  /* A sparse registered set to install into. */
  struct io_uring_rsrc_register rr;

  memset(&rr, 0, sizeof(rr));
  rr.nr = 4;
  rr.flags = IORING_RSRC_REGISTER_SPARSE;
  rc = io_uring_register_(r.fd, IORING_REGISTER_FILES2, &rr, sizeof(rr));
  if (rc < 0) {
    bad("direct-openat", "the sparse file set was refused", (long)rc);
    ring_free(&r);
    return;
  }

  unlink(path);
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_OPENAT;
  sqe->fd = AT_FDCWD;
  sqe->addr = (unsigned long long)(uintptr_t)path;
  sqe->open_flags = O_RDWR | O_CREAT | O_TRUNC;
  sqe->len = 0644;
  sqe->file_index = 2; /* slot 1, biased by one */
  sqe->user_data = 1;
  rc = submit_wait(&r, 1, &cqe);
  int opened_direct = (rc == 0 && cqe.res == 0);

  /* The slot must now be usable through IOSQE_FIXED_FILE. */
  static char msg[] = "direct-write";
  char back[32];

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_WRITE;
  sqe->flags = IOSQE_FIXED_FILE;
  sqe->fd = 1;
  sqe->addr = (unsigned long long)(uintptr_t)msg;
  sqe->len = 13;
  sqe->off = 0;
  sqe->user_data = 2;
  rc = submit_wait(&r, 1, &cqe);
  memset(back, 0, sizeof(back));
  int fd = open(path, O_RDONLY);
  int n = (fd >= 0) ? (int)read(fd, back, sizeof(back)) : -1;

  judge("direct-openat",
        opened_direct && rc == 0 && cqe.res == 13 && n == 13 &&
            memcmp(back, msg, 13) == 0,
        "a direct open did not land in the registered slot it named",
        rc == 0 ? (long)cqe.res : (long)rc);
  if (fd >= 0)
    close(fd);

  /* IORING_FILE_INDEX_ALLOC picks the slot itself and reports which. */
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_OPENAT;
  sqe->fd = AT_FDCWD;
  sqe->addr = (unsigned long long)(uintptr_t)path;
  sqe->open_flags = O_RDONLY;
  sqe->file_index = IORING_FILE_INDEX_ALLOC;
  sqe->user_data = 3;
  rc = submit_wait(&r, 1, &cqe);
  int slot = (rc == 0) ? cqe.res : -1;

  judge("direct-alloc", slot >= 0 && slot < 4 && slot != 1,
        "IORING_FILE_INDEX_ALLOC did not report a free slot",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* FIXED_FD_INSTALL turns a slot back into an ordinary descriptor. */
  sqe = sq_get(&r);
  sqe->opcode = IOU_OP_FIXED_FD_INSTALL;
  sqe->flags = IOSQE_FIXED_FILE;
  sqe->fd = 1;
  sqe->user_data = 4;
  rc = submit_wait(&r, 1, &cqe);
  int newfd = (rc == 0) ? cqe.res : -1;

  memset(back, 0, sizeof(back));
  n = (newfd >= 0) ? (int)pread(newfd, back, 13, 0) : -1;
  judge("fixed-fd-install",
        newfd >= 0 && n == 13 && memcmp(back, msg, 13) == 0,
        "IORING_OP_FIXED_FD_INSTALL did not hand back a working descriptor",
        rc == 0 ? (long)cqe.res : (long)rc);
  if (newfd >= 0)
    close(newfd);

  unlink(path);
  ring_free(&r);
}

static void check_register_extras(void) {
  struct ring r;
  int rc;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("register-buffers2", "no ring", 0);
    return;
  }
  /* BUFFERS2 in its sparse form, then BUFFERS_UPDATE filling a slot, then a
   * WRITE_FIXED that uses it. */
  struct io_uring_rsrc_register rr;

  memset(&rr, 0, sizeof(rr));
  rr.nr = 2;
  rr.flags = IORING_RSRC_REGISTER_SPARSE;
  rc = io_uring_register_(r.fd, IORING_REGISTER_BUFFERS2, &rr, sizeof(rr));
  int sparse_ok = (rc == 0);

  static char buf[64];
  struct iovec iov;
  struct io_uring_rsrc_update2 up;

  memset(buf, 0, sizeof(buf));
  strcpy(buf, "registered-later");
  iov.iov_base = buf;
  iov.iov_len = sizeof(buf);
  memset(&up, 0, sizeof(up));
  up.offset = 1;
  up.nr = 1;
  up.data = (unsigned long long)(uintptr_t)&iov;
  rc = io_uring_register_(r.fd, IORING_REGISTER_BUFFERS_UPDATE, &up,
                          sizeof(up));
  int update_ok = (rc == 1);

  const char *path = "/tmp/m125-buf2";
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe = sq_get(&r);

  sqe->opcode = IORING_OP_WRITE_FIXED;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(uintptr_t)buf;
  sqe->len = 17;
  sqe->off = 0;
  sqe->buf_index = 1;
  sqe->user_data = 1;
  rc = submit_wait(&r, 1, &cqe);
  char back[32];

  memset(back, 0, sizeof(back));
  int n = (fd >= 0) ? (int)pread(fd, back, 17, 0) : -1;

  judge("register-buffers2",
        sparse_ok && update_ok && rc == 0 && cqe.res == 17 && n == 17 &&
            memcmp(back, buf, 17) == 0,
        "a buffer registered by BUFFERS2 + BUFFERS_UPDATE did not work",
        rc == 0 ? (long)cqe.res : (long)rc);
  if (fd >= 0)
    close(fd);
  unlink(path);

  /* IOWQ_MAX_WORKERS reports the previous values. */
  {
    unsigned vals[2] = {4, 4};

    rc = io_uring_register_(r.fd, IORING_REGISTER_IOWQ_MAX_WORKERS, vals, 2);
    int first = (rc == 0);
    unsigned again[2] = {8, 8};

    rc = io_uring_register_(r.fd, IORING_REGISTER_IOWQ_MAX_WORKERS, again, 2);
    judge("iowq-max-workers",
          first && rc == 0 && again[0] == 4 && again[1] == 4,
          "IORING_REGISTER_IOWQ_MAX_WORKERS did not report the old values",
          rc == 0 ? (long)again[0] : (long)rc);
  }

  /* SYNC_CANCEL takes down an armed poll from the register path. */
  {
    int pfd[2];

    if (pipe(pfd) == 0) {
      struct io_uring_sqe *s = sq_get(&r);

      s->opcode = IORING_OP_POLL_ADD;
      s->fd = pfd[0];
      s->poll32_events = POLLIN;
      s->user_data = 0x515;
      io_uring_enter_(r.fd, 1, 0, 0, 0, 0);

      struct io_uring_sync_cancel_reg sc;

      memset(&sc, 0, sizeof(sc));
      sc.addr = 0x515;
      sc.fd = -1;
      sc.timeout.tv_sec = -1;
      sc.timeout.tv_nsec = -1;
      rc = io_uring_register_(r.fd, IORING_REGISTER_SYNC_CANCEL, &sc, 1);
      struct io_uring_cqe c;
      int got = 0;

      io_uring_enter_(r.fd, 0, 0, 0, 0, 0);
      while (cq_get(&r, &c))
        if (c.user_data == 0x515 && c.res == -ECANCELED)
          got = 1;
      judge("sync-cancel", rc == 0 && got,
            "IORING_REGISTER_SYNC_CANCEL did not cancel the armed poll",
            (long)rc);
      close(pfd[0]);
      close(pfd[1]);
    } else {
      bad("sync-cancel", "pipe failed", -1);
    }
  }
  ring_free(&r);

  /* RESTRICTIONS on a disabled ring: only what was named is allowed. */
  {
    struct ring rd;

    if (ring_make(&rd, 8, IORING_SETUP_R_DISABLED, 0) == 0) {
      struct io_uring_restriction res[2];

      memset(res, 0, sizeof(res));
      res[0].opcode = IORING_RESTRICTION_SQE_OP;
      res[0].sqe_op = IORING_OP_NOP;
      res[1].opcode = IORING_RESTRICTION_SQE_FLAGS_ALLOWED;
      res[1].sqe_flags = IOSQE_IO_LINK;
      rc = io_uring_register_(rd.fd, IORING_REGISTER_RESTRICTIONS, res, 2);
      int set_ok = (rc == 0);

      rc = io_uring_register_(rd.fd, IORING_REGISTER_ENABLE_RINGS, 0, 0);
      int enabled = (rc == 0);

      struct io_uring_cqe c;
      struct io_uring_sqe *s = sq_get(&rd);

      s->opcode = IORING_OP_NOP;
      s->user_data = 1;
      rc = submit_wait(&rd, 1, &c);
      int nop_allowed = (rc == 0 && c.res == 0);

      s = sq_get(&rd);
      s->opcode = IORING_OP_READ;
      s->fd = 0;
      s->user_data = 2;
      rc = submit_wait(&rd, 1, &c);
      int read_refused = (rc == 0 && c.res == -EACCES);

      judge("restrictions",
            set_ok && enabled && nop_allowed && read_refused,
            "a restricted ring did not allow exactly what it was told to",
            (long)(set_ok * 8 + enabled * 4 + nop_allowed * 2 + read_refused));
      ring_free(&rd);
    } else {
      bad("restrictions", "no disabled ring", -1);
    }
  }
}

static void check_ring_shapes(void) {
  struct io_uring_params p;
  struct ring r;
  int rc;

  /* IORING_SETUP_CQE32: the CQE stride is 32 bytes and the ring still works. */
  memset(&r, 0, sizeof(r));
  memset(&p, 0, sizeof(p));
  p.flags = IORING_SETUP_CQE32;
  r.p = p;
  r.fd = io_uring_setup_(8, &r.p);
  if (r.fd < 0) {
    bad("cqe32", "IORING_SETUP_CQE32 was refused", (long)r.fd);
  } else {
    size_t sz = r.p.cq_off.cqes + (size_t)r.p.cq_entries * 32;

    if (r.p.sq_off.array + r.p.sq_entries * sizeof(unsigned) > sz)
      sz = r.p.sq_off.array + r.p.sq_entries * sizeof(unsigned);
    r.sq_sz = sz;
    r.sq_ptr = mmap(0, r.sq_sz, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, r.fd, IORING_OFF_SQ_RING);
    r.sqes_sz = r.p.sq_entries * sizeof(struct io_uring_sqe);
    r.sqes = mmap(0, r.sqes_sz, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_POPULATE, r.fd, IORING_OFF_SQES);
    if (r.sq_ptr == MAP_FAILED || r.sqes == MAP_FAILED) {
      bad("cqe32", "the CQE32 ring would not map", -1);
    } else {
      char *b = (char *)r.sq_ptr;

      r.sq_head = (unsigned *)(b + r.p.sq_off.head);
      r.sq_tail = (unsigned *)(b + r.p.sq_off.tail);
      r.sq_mask = (unsigned *)(b + r.p.sq_off.ring_mask);
      r.sq_entries = (unsigned *)(b + r.p.sq_off.ring_entries);
      r.sq_array = (unsigned *)(b + r.p.sq_off.array);
      r.cq_head = (unsigned *)(b + r.p.cq_off.head);
      r.cq_tail = (unsigned *)(b + r.p.cq_off.tail);
      r.cq_mask = (unsigned *)(b + r.p.cq_off.ring_mask);
      r.cq_entries = (unsigned *)(b + r.p.cq_off.ring_entries);
      r.cqes = (struct io_uring_cqe *)(b + r.p.cq_off.cqes);

      struct io_uring_sqe *s = sq_get(&r);

      s->opcode = IORING_OP_NOP;
      s->user_data = 0xc32ull;
      rc = io_uring_enter_(r.fd, 1, 1, IORING_ENTER_GETEVENTS, 0, 0);
      unsigned head = __atomic_load_n(r.cq_head, __ATOMIC_RELAXED);
      unsigned tail = __atomic_load_n(r.cq_tail, __ATOMIC_ACQUIRE);
      struct io_uring_cqe *c = cqe_at(&r, head, 32);

      judge("cqe32",
            rc >= 0 && tail - head == 1 && c->user_data == 0xc32ull &&
                c->res == 0,
            "a 32-byte-CQE ring did not deliver a completion at the right "
            "stride",
            (long)rc);
      munmap(r.sqes, r.sqes_sz);
      munmap(r.sq_ptr, r.sq_sz);
    }
    close(r.fd);
  }

  /* IORING_SETUP_SQE128: the SQE stride is 128 bytes. */
  memset(&r, 0, sizeof(r));
  memset(&p, 0, sizeof(p));
  p.flags = IORING_SETUP_SQE128;
  r.p = p;
  r.fd = io_uring_setup_(8, &r.p);
  if (r.fd < 0) {
    bad("sqe128", "IORING_SETUP_SQE128 was refused", (long)r.fd);
  } else {
    r.sq_sz = r.p.cq_off.cqes +
              (size_t)r.p.cq_entries * sizeof(struct io_uring_cqe);
    void *sqp = mmap(0, r.sq_sz, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, r.fd, IORING_OFF_SQ_RING);
    size_t ssz = (size_t)r.p.sq_entries * 128;
    char *sqes = mmap(0, ssz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                      r.fd, IORING_OFF_SQES);

    if (sqp == MAP_FAILED || sqes == MAP_FAILED) {
      bad("sqe128", "the SQE128 ring would not map", -1);
    } else {
      char *b = (char *)sqp;
      unsigned *sqt = (unsigned *)(b + r.p.sq_off.tail);
      unsigned *sqa = (unsigned *)(b + r.p.sq_off.array);
      unsigned *cqh = (unsigned *)(b + r.p.cq_off.head);
      unsigned *cqt = (unsigned *)(b + r.p.cq_off.tail);
      unsigned *cqm = (unsigned *)(b + r.p.cq_off.ring_mask);
      struct io_uring_cqe *cqes = (struct io_uring_cqe *)(b + r.p.cq_off.cqes);
      struct io_uring_sqe *s = (struct io_uring_sqe *)(sqes + 0 * 128);

      memset(sqes, 0, ssz);
      s->opcode = IORING_OP_NOP;
      s->user_data = 0x128ull;
      sqa[0] = 0;
      __atomic_store_n(sqt, 1u, __ATOMIC_RELEASE);
      rc = io_uring_enter_(r.fd, 1, 1, IORING_ENTER_GETEVENTS, 0, 0);
      unsigned h = __atomic_load_n(cqh, __ATOMIC_RELAXED);
      unsigned t = __atomic_load_n(cqt, __ATOMIC_ACQUIRE);

      judge("sqe128",
            rc >= 0 && t - h == 1 && cqes[h & *cqm].user_data == 0x128ull,
            "a 128-byte-SQE ring did not read the entry at the right stride",
            (long)rc);
      munmap(sqes, ssz);
      munmap(sqp, r.sq_sz);
    }
    close(r.fd);
  }

  /* IORING_SETUP_NO_SQARRAY: the SQ head indexes the SQEs directly. */
  memset(&r, 0, sizeof(r));
  memset(&p, 0, sizeof(p));
  p.flags = IORING_SETUP_NO_SQARRAY;
  r.p = p;
  r.fd = io_uring_setup_(8, &r.p);
  if (r.fd < 0) {
    bad("no-sqarray", "IORING_SETUP_NO_SQARRAY was refused", (long)r.fd);
  } else {
    size_t sz = r.p.cq_off.cqes +
                (size_t)r.p.cq_entries * sizeof(struct io_uring_cqe);
    char *b = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                   r.fd, IORING_OFF_SQ_RING);
    size_t ssz = (size_t)r.p.sq_entries * sizeof(struct io_uring_sqe);
    struct io_uring_sqe *sqes = mmap(0, ssz, PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_POPULATE, r.fd,
                                     IORING_OFF_SQES);

    if (b == MAP_FAILED || sqes == MAP_FAILED) {
      bad("no-sqarray", "the NO_SQARRAY ring would not map", -1);
    } else {
      unsigned *sqt = (unsigned *)(b + r.p.sq_off.tail);
      unsigned *cqh = (unsigned *)(b + r.p.cq_off.head);
      unsigned *cqt = (unsigned *)(b + r.p.cq_off.tail);
      unsigned *cqm = (unsigned *)(b + r.p.cq_off.ring_mask);
      struct io_uring_cqe *cqes = (struct io_uring_cqe *)(b + r.p.cq_off.cqes);

      memset(&sqes[0], 0, sizeof(sqes[0]));
      sqes[0].opcode = IORING_OP_NOP;
      sqes[0].user_data = 0x5a5aull;
      __atomic_store_n(sqt, 1u, __ATOMIC_RELEASE);
      rc = io_uring_enter_(r.fd, 1, 1, IORING_ENTER_GETEVENTS, 0, 0);
      unsigned h = __atomic_load_n(cqh, __ATOMIC_RELAXED);
      unsigned t = __atomic_load_n(cqt, __ATOMIC_ACQUIRE);

      judge("no-sqarray",
            rc >= 0 && t - h == 1 && cqes[h & *cqm].user_data == 0x5a5aull,
            "a ring without the indirection array did not read the entry",
            (long)rc);
      munmap(sqes, ssz);
      munmap(b, sz);
    }
    close(r.fd);
  }

  /* IORING_SETUP_DEFER_TASKRUN, which Linux couples to SINGLE_ISSUER. */
  {
    struct ring rd;
    struct io_uring_cqe c;

    if (ring_make(&rd, 8,
                  IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SINGLE_ISSUER,
                  0) < 0) {
      bad("defer-taskrun", "the flag pair was refused", -1);
    } else {
      struct io_uring_sqe *s = sq_get(&rd);

      s->opcode = IORING_OP_NOP;
      s->user_data = 0xdefull;
      rc = submit_wait(&rd, 1, &c);
      judge("defer-taskrun", rc == 0 && c.user_data == 0xdefull,
            "a DEFER_TASKRUN ring did not complete its work",
            (long)rc);
      ring_free(&rd);
    }
    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_DEFER_TASKRUN;
    int fd = io_uring_setup_(8, &p);

    judge("defer-taskrun-needs-single-issuer", fd < 0 && errno == EINVAL,
          "DEFER_TASKRUN without SINGLE_ISSUER was accepted", (long)fd);
    if (fd >= 0)
      close(fd);
  }
}


/* ── the last of the opcode surface (M125 completion) ───────────────────── */

static void check_xattr_opcodes(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe;
  const char *path = "/tmp/m125-xattr";
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  char value[64];
  int rc;

  if (ring_make(&r, 8, 0, 0) < 0 || fd < 0) {
    bad("op-xattr", "no ring or file", (long)fd);
    if (fd >= 0)
      close(fd);
    return;
  }

  /* SETXATTR by path, then GETXATTR reads back exactly what was set. */
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_SETXATTR;
  sqe->addr = (unsigned long long)(uintptr_t) "user.b1nix";
  sqe->addr2 = (unsigned long long)(uintptr_t)path;
  sqe->addr3 = (unsigned long long)(uintptr_t) "through-the-ring";
  sqe->len = 16;
  sqe->user_data = 1;
  rc = submit_wait(&r, 1, &cqe);
  int set_ok = rc == 0 && cqe.res >= 0;

  memset(value, 0, sizeof(value));
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_GETXATTR;
  sqe->addr = (unsigned long long)(uintptr_t) "user.b1nix";
  sqe->addr2 = (unsigned long long)(uintptr_t)path;
  sqe->addr3 = (unsigned long long)(uintptr_t)value;
  sqe->len = sizeof(value);
  sqe->user_data = 2;
  rc = submit_wait(&r, 1, &cqe);
  judge("op-xattr",
        set_ok && rc == 0 && cqe.res == 16 &&
            memcmp(value, "through-the-ring", 16) == 0,
        "SETXATTR/GETXATTR did not round-trip an attribute",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* The f-forms, on the open descriptor. */
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_FSETXATTR;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(uintptr_t) "user.byfd";
  sqe->addr3 = (unsigned long long)(uintptr_t) "fd-form";
  sqe->len = 7;
  sqe->user_data = 3;
  rc = submit_wait(&r, 1, &cqe);
  int fset_ok = rc == 0 && cqe.res >= 0;

  memset(value, 0, sizeof(value));
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_FGETXATTR;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(uintptr_t) "user.byfd";
  sqe->addr3 = (unsigned long long)(uintptr_t)value;
  sqe->len = sizeof(value);
  sqe->user_data = 4;
  rc = submit_wait(&r, 1, &cqe);
  judge("op-fxattr",
        fset_ok && rc == 0 && cqe.res == 7 && memcmp(value, "fd-form", 7) == 0,
        "FSETXATTR/FGETXATTR did not round-trip an attribute on a descriptor",
        rc == 0 ? (long)cqe.res : (long)rc);

  close(fd);
  unlink(path);
  ring_free(&r);
}

static void check_vectored_fixed(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe;
  static char buf[8192];
  struct iovec reg = {.iov_base = buf, .iov_len = sizeof(buf)};
  struct iovec seg[2];
  const char *path = "/tmp/m125-vfixed";
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  int rc;

  if (ring_make(&r, 8, 0, 0) < 0 || fd < 0) {
    bad("op-writev-fixed", "no ring or file", (long)fd);
    if (fd >= 0)
      close(fd);
    return;
  }
  if (io_uring_register_(r.fd, IORING_REGISTER_BUFFERS, &reg, 1) < 0) {
    bad("op-writev-fixed", "registering the buffer", -1);
    close(fd);
    ring_free(&r);
    return;
  }

  memcpy(buf, "AAAABBBB", 8);
  seg[0].iov_base = buf;
  seg[0].iov_len = 4;
  seg[1].iov_base = buf + 4;
  seg[1].iov_len = 4;

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_WRITEV_FIXED;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(uintptr_t)seg;
  sqe->len = 2;
  sqe->off = 0;
  sqe->buf_index = 0;
  sqe->user_data = 1;
  rc = submit_wait(&r, 1, &cqe);
  int wrote = (rc == 0) ? cqe.res : -1;

  /* Read it back into two segments of the same registered buffer. */
  memset(buf + 16, 0, 16);
  seg[0].iov_base = buf + 16;
  seg[0].iov_len = 4;
  seg[1].iov_base = buf + 20;
  seg[1].iov_len = 4;
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_READV_FIXED;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(uintptr_t)seg;
  sqe->len = 2;
  sqe->off = 0;
  sqe->buf_index = 0;
  sqe->user_data = 2;
  rc = submit_wait(&r, 1, &cqe);
  judge("op-writev-fixed",
        wrote == 8 && rc == 0 && cqe.res == 8 &&
            memcmp(buf + 16, "AAAABBBB", 8) == 0,
        "the vectored fixed-buffer forms did not move the bytes",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* A segment outside the registration must be refused, which is the whole
   * point of registering. */
  seg[0].iov_base = (char *)buf - 4096;
  seg[0].iov_len = 4;
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_READV_FIXED;
  sqe->fd = fd;
  sqe->addr = (unsigned long long)(uintptr_t)seg;
  sqe->len = 1;
  sqe->off = 0;
  sqe->buf_index = 0;
  sqe->user_data = 3;
  rc = submit_wait(&r, 1, &cqe);
  judge("op-vfixed-bounds", rc == 0 && cqe.res == -EFAULT,
        "a segment outside the registered buffer was accepted",
        rc == 0 ? (long)cqe.res : (long)rc);

  close(fd);
  unlink(path);
  ring_free(&r);
}

static void check_futex_opcodes(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe;
  static volatile unsigned int word;
  int rc;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("op-futex", "no ring", 0);
    return;
  }

  /* A wait whose word already differs is EAGAIN, exactly as futex(2) says --
   * and it is the case a lock's fast path takes. */
  word = 5;
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_FUTEX_WAIT;
  sqe->addr = (unsigned long long)(uintptr_t)&word;
  sqe->off = 99; /* expect 99, the word holds 5 */
  sqe->user_data = 1;
  rc = submit_wait(&r, 1, &cqe);
  judge("op-futex-eagain", rc == 0 && cqe.res == -EAGAIN,
        "a futex wait on a word that had already moved did not report EAGAIN",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* A real wait: submit it, change the word from this thread, and the ring
   * completes it. */
  word = 1;
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_FUTEX_WAIT;
  sqe->addr = (unsigned long long)(uintptr_t)&word;
  sqe->off = 1;
  sqe->user_data = 2;
  if (io_uring_enter_(r.fd, 1, 0, 0, 0, 0) != 1) {
    bad("op-futex", "submitting the wait", -1);
    ring_free(&r);
    return;
  }
  /* Nothing has changed yet: the ring must have nothing to report. */
  int early = cq_get(&r, &cqe); /* 1 if something was already there */

  word = 2; /* the wake condition */
  rc = io_uring_enter_(r.fd, 0, 1, IORING_ENTER_GETEVENTS, 0, 0);
  int got = cq_get(&r, &cqe);

  judge("op-futex-wait",
        early == 0 && rc >= 0 && got == 1 && cqe.user_data == 2 &&
            cqe.res == 0,
        "a futex wait did not complete when its word changed",
        got == 1 ? (long)cqe.res : (long)got);

  /* FUTEX_WAKE reports how many it woke -- zero here, because nothing is
   * parked on that word, and that is a number not an error. */
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_FUTEX_WAKE;
  sqe->addr = (unsigned long long)(uintptr_t)&word;
  sqe->off = 1;
  sqe->user_data = 3;
  rc = submit_wait(&r, 1, &cqe);
  judge("op-futex-wake", rc == 0 && cqe.res >= 0,
        "FUTEX_WAKE did not report a count", rc == 0 ? (long)cqe.res : (long)rc);

  /* WAITV over two words, completing with the INDEX that moved. */
  static volatile unsigned int w2[2];
  struct {
    unsigned long long val;
    unsigned long long uaddr;
    unsigned int flags;
    unsigned int reserved;
  } wv[2];

  w2[0] = 10;
  w2[1] = 20;
  memset(wv, 0, sizeof(wv));
  wv[0].val = 10;
  wv[0].uaddr = (unsigned long long)(uintptr_t)&w2[0];
  wv[1].val = 20;
  wv[1].uaddr = (unsigned long long)(uintptr_t)&w2[1];

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_FUTEX_WAITV;
  sqe->addr = (unsigned long long)(uintptr_t)wv;
  sqe->len = 2;
  sqe->user_data = 4;
  if (io_uring_enter_(r.fd, 1, 0, 0, 0, 0) != 1) {
    bad("op-futex-waitv", "submitting", -1);
    ring_free(&r);
    return;
  }
  w2[1] = 21; /* the second one moves */
  io_uring_enter_(r.fd, 0, 1, IORING_ENTER_GETEVENTS, 0, 0);
  got = cq_get(&r, &cqe);
  judge("op-futex-waitv", got == 1 && cqe.user_data == 4 && cqe.res == 1,
        "FUTEX_WAITV did not report which word moved",
        got == 1 ? (long)cqe.res : (long)got);
  ring_free(&r);
}

static void check_uring_cmd(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe;
  int sv[2];
  int rc;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("op-uring-cmd", "no ring", 0);
    return;
  }
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    bad("op-uring-cmd", "socketpair", -1);
    ring_free(&r);
    return;
  }
  /* Put bytes in, then ask the socket how many are waiting. */
  if (write(sv[1], "0123456789", 10) != 10) {
    bad("op-uring-cmd", "write", -1);
    close(sv[0]);
    close(sv[1]);
    ring_free(&r);
    return;
  }
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_URING_CMD;
  sqe->fd = sv[0];
  sqe->cmd_op = SOCKET_URING_OP_SIOCINQ;
  sqe->user_data = 1;
  rc = submit_wait(&r, 1, &cqe);
  judge("op-uring-cmd", rc == 0 && cqe.res == 10,
        "SOCKET_URING_OP_SIOCINQ did not report the queued bytes",
        rc == 0 ? (long)cqe.res : (long)rc);

  /* And a socket option through the same door. */
  int bufsz = 0;
  socklen_t bl = sizeof(bufsz);

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_URING_CMD;
  sqe->fd = sv[0];
  sqe->cmd_op = SOCKET_URING_OP_GETSOCKOPT;
  /* level and optname overlay `addr` as two u32s in the ABI, and the
   * distribution's header is older than those names. */
  sqe->addr = ((unsigned long long)SO_TYPE << 32) | (unsigned)SOL_SOCKET;
  sqe->addr3 = (unsigned long long)(uintptr_t)&bufsz;
  sqe->len = bl;
  sqe->user_data = 2;
  rc = submit_wait(&r, 1, &cqe);
  judge("op-uring-cmd-getsockopt",
        rc == 0 && cqe.res >= 0 && bufsz == SOCK_STREAM,
        "SOCKET_URING_OP_GETSOCKOPT did not read the socket's type",
        rc == 0 ? (long)cqe.res : (long)rc);

  close(sv[0]);
  close(sv[1]);
  ring_free(&r);
}

static void check_drain_and_rings(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe;
  int pfd[2];
  int rc;

  if (ring_make(&r, 16, 0, 0) < 0) {
    bad("io-drain", "no ring", 0);
    return;
  }
  if (pipe(pfd) != 0) {
    bad("io-drain", "pipe", -1);
    ring_free(&r);
    return;
  }

  /* A read that cannot complete yet, then a NOP with IOSQE_IO_DRAIN behind
   * it. The drain must NOT complete while the read is outstanding -- that is
   * the whole promise of the flag. */
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_READ;
  sqe->fd = pfd[0];
  {
    static char rbuf[8];

    sqe->addr = (unsigned long long)(uintptr_t)rbuf;
  }
  sqe->len = 8;
  sqe->off = (unsigned long long)-1;
  sqe->user_data = 100;

  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_NOP;
  sqe->flags = IOSQE_IO_DRAIN;
  sqe->user_data = 200;

  if (io_uring_enter_(r.fd, 2, 0, 0, 0, 0) != 2) {
    bad("io-drain", "submitting", -1);
    close(pfd[0]);
    close(pfd[1]);
    ring_free(&r);
    return;
  }
  io_uring_enter_(r.fd, 0, 0, IORING_ENTER_GETEVENTS, 0, 0);
  int early = cq_get(&r, &cqe);
  int early_was_drain = (early == 1 && cqe.user_data == 200);

  /* Let the read finish; the drain may then run, and must come second. */
  write(pfd[1], "12345678", 8);
  rc = io_uring_enter_(r.fd, 0, 2, IORING_ENTER_GETEVENTS, 0, 0);
  struct io_uring_cqe first, second;
  int g1 = cq_get(&r, &first);
  int g2 = cq_get(&r, &second);

  judge("io-drain",
        !early_was_drain && rc >= 0 && g1 == 1 && g2 == 1 &&
            first.user_data == 100 && second.user_data == 200,
        "IOSQE_IO_DRAIN did not wait for the requests before it",
        g1 == 1 ? (long)first.user_data : (long)g1);
  close(pfd[0]);
  close(pfd[1]);

  /* A registered ring descriptor: enter by index instead of by fd. */
  {
    struct io_uring_rsrc_update upd;

    memset(&upd, 0, sizeof(upd));
    upd.data = (unsigned long long)r.fd;
    rc = io_uring_register_(r.fd, IORING_REGISTER_RING_FDS, &upd, 1);
    if (rc != 1) {
      bad("registered-ring", "IORING_REGISTER_RING_FDS", (long)rc);
    } else {
      sqe = sq_get(&r);
      sqe->opcode = IORING_OP_NOP;
      sqe->user_data = 300;
      int n = io_uring_enter_((int)upd.offset, 1, 1,
                              IORING_ENTER_GETEVENTS |
                                  IORING_ENTER_REGISTERED_RING,
                              0, 0);
      int got = cq_get(&r, &cqe);

      judge("registered-ring",
            n == 1 && got == 1 && cqe.user_data == 300,
            "entering by a registered ring index did not work", (long)n);
      io_uring_register_(r.fd, IORING_UNREGISTER_RING_FDS, &upd, 1);
    }
  }
  ring_free(&r);
}

/* A ring full of drains, which is what liburing's defer.t submits: 64 NOPs
 * each carrying IOSQE_IO_DRAIN, so every one of them waits for all the ones
 * before it. The shape is a chain of completions, and the thing being checked
 * is that the kernel walks it rather than recursing through it. */
static void check_drain_storm(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe;
  const unsigned n = 64;
  unsigned got = 0;
  int rc;

  if (ring_make(&r, 128, 0, 0) < 0) {
    bad("drain-storm", "no ring", 0);
    return;
  }
  for (unsigned i = 0; i < n; i++) {
    sqe = sq_get(&r);
    sqe->opcode = IORING_OP_NOP;
    sqe->flags = IOSQE_IO_DRAIN;
    sqe->user_data = 1000 + i;
  }
  rc = io_uring_enter_(r.fd, n, n, IORING_ENTER_GETEVENTS, 0, 0);
  for (unsigned i = 0; i < n; i++) {
    if (!cq_get(&r, &cqe))
      break;
    if (cqe.user_data != 1000 + got)
      break; /* drains must complete in submission order */
    got++;
  }
  judge("drain-storm", rc >= 0 && got == n,
        "64 drained NOPs did not all complete in order", (long)got);
  ring_free(&r);
}

/* liburing's defer.t in miniature: overflow the completion queue hundreds of
 * times, then submit drained requests behind the pile. The drain has to
 * consider every request still on the ring, and the completions have to be
 * walked rather than recursed through -- the machine panicked with a kernel
 * stack overflow on exactly this shape. */
static void check_drain_after_overflow(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe;
  int rc;
  unsigned reaped = 0;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("drain-overflow", "no ring", 0);
    return;
  }
  /* Submit far more than the completion queue holds, reaping nothing. */
  for (int round = 0; round < 40; round++) {
    for (int i = 0; i < 8; i++) {
      sqe = sq_get(&r);
      if (!sqe)
        break;
      sqe->opcode = IORING_OP_NOP;
      sqe->user_data = 5000 + round * 8 + i;
    }
    if (io_uring_enter_(r.fd, 8, 0, 0, 0, 0) < 0)
      break;
  }
  /* Then ten drained NOPs behind the pile. */
  for (int i = 0; i < 10; i++) {
    sqe = sq_get(&r);
    if (!sqe)
      break;
    sqe->opcode = IORING_OP_NOP;
    sqe->flags = IOSQE_IO_DRAIN;
    sqe->user_data = 9000 + i;
  }
  rc = io_uring_enter_(r.fd, 10, 1, IORING_ENTER_GETEVENTS, 0, 0);

  /* Drain the ring: every completion must eventually come out, and the
   * machine must still be alive to hand them over. */
  for (int guard = 0; guard < 2000; guard++) {
    if (cq_get(&r, &cqe)) {
      reaped++;
      continue;
    }
    if (io_uring_enter_(r.fd, 0, 0, IORING_ENTER_GETEVENTS, 0, 0) < 0)
      break;
    if (!cq_get(&r, &cqe))
      break;
    reaped++;
  }
  judge("drain-overflow", rc >= 0 && reaped > 100,
        "drained requests behind an overflowed completion queue did not come "
        "back",
        (long)reaped);
  ring_free(&r);
}

/* The same drains, on the two ring shapes defer.t also runs them on: a
 * kernel-submitted (SQPOLL) ring and an IOPOLL one. A drain is a rule about
 * ORDER, and the order is decided in a different place when the submitting
 * thread is the kernel's own. */
static void check_drain_other_rings(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe;
  int ok_sq = 0, ok_poll = 0;

  if (ring_make(&r, 32, IORING_SETUP_SQPOLL, 0) == 0) {
    unsigned got = 0;

    for (unsigned i = 0; i < 16; i++) {
      sqe = sq_get(&r);
      if (!sqe)
        break;
      sqe->opcode = IORING_OP_NOP;
      sqe->flags = IOSQE_IO_DRAIN;
      sqe->user_data = 7000 + i;
    }
    /* The kernel thread picks them up; ask it to, then wait. */
    io_uring_enter_(r.fd, 16, 0, IORING_ENTER_SQ_WAKEUP, 0, 0);
    for (int spin = 0; spin < 200 && got < 16; spin++) {
      io_uring_enter_(r.fd, 0, 1, IORING_ENTER_GETEVENTS, 0, 0);
      while (cq_get(&r, &cqe))
        got++;
    }
    ok_sq = (got == 16);
    ring_free(&r);
  } else {
    ok_sq = 1; /* no SQPOLL on this machine: nothing to prove */
  }

  if (ring_make(&r, 32, IORING_SETUP_IOPOLL, 0) == 0) {
    unsigned got = 0;

    for (unsigned i = 0; i < 16; i++) {
      sqe = sq_get(&r);
      if (!sqe)
        break;
      sqe->opcode = IORING_OP_NOP;
      sqe->flags = IOSQE_IO_DRAIN;
      sqe->user_data = 8000 + i;
    }
    io_uring_enter_(r.fd, 16, 0, 0, 0, 0);
    for (int spin = 0; spin < 200 && got < 16; spin++) {
      io_uring_enter_(r.fd, 0, 1, IORING_ENTER_GETEVENTS, 0, 0);
      while (cq_get(&r, &cqe))
        got++;
    }
    ok_poll = (got == 16);
    ring_free(&r);
  } else {
    ok_poll = 1;
  }

  judge("drain-sqpoll-iopoll", ok_sq && ok_poll,
        "drained requests did not all complete on an SQPOLL or IOPOLL ring",
        (long)((ok_sq ? 2 : 0) + (ok_poll ? 1 : 0)));
}

static void check_personality(void) {
  struct ring r;
  struct io_uring_cqe cqe;
  struct io_uring_sqe *sqe;
  int id, rc;

  if (ring_make(&r, 8, 0, 0) < 0) {
    bad("personality", "no ring", 0);
    return;
  }
  id = io_uring_register_(r.fd, IORING_REGISTER_PERSONALITY, NULL, 0);
  if (id <= 0) {
    bad("personality", "IORING_REGISTER_PERSONALITY", (long)id);
    ring_free(&r);
    return;
  }

  /* A request that runs under the registered identity. Registered as root and
   * run as root, so what is proved here is that the id is accepted, applied
   * and released -- and that an id nobody registered is refused. */
  const char *path = "/tmp/m125-pers";

  unlink(path);
  sqe = sq_get(&r);
  sqe->opcode = IORING_OP_OPENAT;
  sqe->fd = AT_FDCWD;
  sqe->addr = (unsigned long long)(uintptr_t)path;
  sqe->open_flags = O_CREAT | O_RDWR;
  sqe->len = 0644;
  sqe->personality = (unsigned short)id;
  sqe->user_data = 1;
  rc = submit_wait(&r, 1, &cqe);
  int opened = (rc == 0 && cqe.res >= 0);

  if (opened)
    close(cqe.res);
  unlink(path);

  int bad_unreg = io_uring_register_(r.fd, IORING_UNREGISTER_PERSONALITY, NULL,
                                    (unsigned)id + 7);
  int good_unreg =
      io_uring_register_(r.fd, IORING_UNREGISTER_PERSONALITY, NULL, (unsigned)id);

  judge("personality", opened && bad_unreg < 0 && good_unreg == 0,
        "a registered personality was not applied and released",
        (long)(opened ? good_unreg : -1));
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
  check_sqpoll();
  check_iopoll();
  check_provided_buffers();
  check_buffer_ring();
  check_multishot();
  check_multishot_recv();
  check_multishot_accept();
  check_fs_opcodes();
  check_net_opcodes();
  check_misc_opcodes();
  check_direct_descriptors();
  check_register_extras();
  check_ring_shapes();
  check_xattr_opcodes();
  check_vectored_fixed();
  check_futex_opcodes();
  check_uring_cmd();
  check_drain_and_rings();
  check_drain_storm();
  check_drain_after_overflow();
  check_drain_other_rings();
  check_personality();

  printf("M125-SMOKE: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
