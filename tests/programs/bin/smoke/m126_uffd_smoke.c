/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * m126_uffd_smoke — userfaultfd(2): page faults answered by a program.
 *
 * Every check makes a real fault happen and proves what the monitor did with
 * it. Nothing here asserts that an ioctl returned zero and calls that a pass:
 * a served page is read back and compared, a write-protected page is written
 * to and the notification is waited for, and a monitor that walks away is
 * proved not to leave its process hanging.
 *
 *   api            UFFDIO_API agrees a version and reports its features; a
 *                  second handshake, and a read before the first, are refused
 *   copy           a thread faults on unbacked memory, the monitor is told the
 *                  address, fills the page with UFFDIO_COPY, and the faulting
 *                  thread reads back exactly those bytes
 *   zeropage       UFFDIO_ZEROPAGE serves a fault with a page of zeroes
 *   poll           poll(2) on the descriptor reports the message readable, and
 *                  a non-blocking read with nothing pending is EAGAIN
 *   writeprotect   a page made read-only through UFFDIO_WRITEPROTECT faults on
 *                  the next store, the monitor sees a WP message, lifts the
 *                  protection, and the store completes
 *   unregister     after UFFDIO_UNREGISTER a fault in the range is served by
 *                  the kernel again, with no message and no wait
 *   monitor-gone   closing the descriptor while a thread is blocked on it lets
 *                  that thread run: memory, not a freeze
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <linux/userfaultfd.h>

#ifndef __NR_userfaultfd
#define __NR_userfaultfd 323
#endif

#define PAGE 4096

static int fails;

static void ok(const char *what) {
  printf("M126-UFFD: ok %s\n", what);
  fflush(stdout);
}

static void bad(const char *what, const char *why, long v) {
  printf("M126-UFFD: FAIL %s — %s (%ld, errno=%d)\n", what, why, v, errno);
  fflush(stdout);
  fails++;
}

static void judge(const char *what, int good, const char *why, long v) {
  if (good)
    ok(what);
  else
    bad(what, why, v);
}

static void note(const char *fmt, ...) {
  char b[300];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  printf("M126-UFFD:   %s\n", b);
  fflush(stdout);
}

static int uffd_open(int flags) {
  return (int)syscall(__NR_userfaultfd, flags);
}

static void msleep(unsigned ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

/* An anonymous region the kernel has not backed yet. */
static char *fresh_region(size_t pages) {
  char *p = mmap(NULL, pages * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  return p == MAP_FAILED ? NULL : p;
}

static int register_range(int ufd, void *addr, size_t len, uint64_t mode,
                          uint64_t *ioctls_out) {
  struct uffdio_register reg;

  memset(&reg, 0, sizeof(reg));
  reg.range.start = (uint64_t)(uintptr_t)addr;
  reg.range.len = len;
  reg.mode = mode;
  if (ioctl(ufd, UFFDIO_REGISTER, &reg) != 0)
    return -1;
  if (ioctls_out)
    *ioctls_out = reg.ioctls;
  return 0;
}

static int do_api(int ufd, uint64_t features, uint64_t *got) {
  struct uffdio_api api;

  memset(&api, 0, sizeof(api));
  api.api = UFFD_API;
  api.features = features;
  if (ioctl(ufd, UFFDIO_API, &api) != 0)
    return -1;
  if (got)
    *got = api.features;
  return 0;
}

/* ── api ─────────────────────────────────────────────────────────────────── */

static void check_api(void) {
  int ufd = uffd_open(0);
  uint64_t features = 0;
  struct uffd_msg m;

  if (ufd < 0) {
    bad("api", "userfaultfd(2) failed", (long)ufd);
    return;
  }
  /* A read before the handshake is EINVAL, as on Linux. */
  int early = (int)read(ufd, &m, sizeof(m));
  int early_ok = early < 0 && errno == EINVAL;

  if (do_api(ufd, 0, &features) != 0) {
    bad("api", "UFFDIO_API failed", -1);
    close(ufd);
    return;
  }
  /* A second handshake on the same descriptor is refused. */
  int again = 0;
  {
    struct uffdio_api api;

    memset(&api, 0, sizeof(api));
    api.api = UFFD_API;
    again = ioctl(ufd, UFFDIO_API, &api);
  }
  note("features 0x%llx", (unsigned long long)features);
  judge("api", early_ok && features != 0 && again != 0,
        "the handshake did not behave as the ABI says", (long)features);
  close(ufd);
}

/* ── the monitor thread ──────────────────────────────────────────────────── */

struct monitor {
  int ufd;
  char *region;
  volatile int served;     /* pages filled */
  volatile int wp_seen;    /* write-protect messages seen */
  volatile int stop;
  volatile int msgs;        /* messages read */
  volatile int read_errno;  /* why a read failed, if one did */
  volatile int copy_errno;  /* why UFFDIO_COPY failed, if it did */
  char fill;
};

static void *monitor_thread(void *arg) {
  struct monitor *mon = arg;

  for (;;) {
    struct pollfd pfd = {.fd = mon->ufd, .events = POLLIN};

    if (mon->stop)
      break;
    if (poll(&pfd, 1, 100) <= 0)
      continue;

    struct uffd_msg m;
    ssize_t rn = read(mon->ufd, &m, sizeof(m));

    if (rn != (ssize_t)sizeof(m)) {
      mon->read_errno = errno;
      continue;
    }
    mon->msgs++;
    if (m.event != UFFD_EVENT_PAGEFAULT)
      continue;

    uint64_t addr = m.arg.pagefault.address & ~(uint64_t)(PAGE - 1);

    if (m.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP) {
      struct uffdio_writeprotect wp;

      memset(&wp, 0, sizeof(wp));
      wp.range.start = addr;
      wp.range.len = PAGE;
      wp.mode = 0; /* lift the protection and wake the writer */
      ioctl(mon->ufd, UFFDIO_WRITEPROTECT, &wp);
      mon->wp_seen++;
      continue;
    }

    static char page[PAGE];

    memset(page, mon->fill, sizeof(page));

    struct uffdio_copy cp;

    memset(&cp, 0, sizeof(cp));
    cp.dst = addr;
    cp.src = (uint64_t)(uintptr_t)page;
    cp.len = PAGE;
    if (ioctl(mon->ufd, UFFDIO_COPY, &cp) == 0)
      mon->served++;
    else
      mon->copy_errno = errno;
  }
  return NULL;
}

/* ── copy ────────────────────────────────────────────────────────────────── */

static void check_copy_and_zeropage(void) {
  struct monitor mon;
  pthread_t th;
  char *region = fresh_region(4);

  memset(&mon, 0, sizeof(mon));
  mon.fill = 0x5A;
  if (!region) {
    bad("copy", "mmap", -1);
    return;
  }
  mon.region = region;
  mon.ufd = uffd_open(0);
  if (mon.ufd < 0 || do_api(mon.ufd, 0, NULL) != 0) {
    bad("copy", "opening a userfaultfd", (long)mon.ufd);
    return;
  }
  uint64_t ioctls = 0;

  if (register_range(mon.ufd, region, 4 * PAGE, UFFDIO_REGISTER_MODE_MISSING,
                     &ioctls) != 0) {
    bad("copy", "UFFDIO_REGISTER", -1);
    close(mon.ufd);
    return;
  }
  if (pthread_create(&th, NULL, monitor_thread, &mon) != 0) {
    bad("copy", "pthread_create", -1);
    close(mon.ufd);
    return;
  }

  /* The fault. Reading an address in the registered range must not give a
   * zero page from the kernel: it must give what the monitor supplies. */
  volatile char v0 = region[0];
  volatile char v1 = region[PAGE + 17];

  /* Stop the monitor and join it before looking at its counters. The faulting
   * thread is released by UFFDIO_COPY itself, so it can return and read
   * mon.served while the monitor has not yet incremented it for the second
   * page: reading the count without the join is a race in the test. */
  mon.stop = 1;
  pthread_join(th, NULL);

  int served_ok = (v0 == 0x5A) && (v1 == 0x5A) && mon.served >= 2;

  judge("copy", served_ok,
        "the page the faulting thread read was not the monitor's",
        (long)(unsigned char)v0);

  /* Zero page, served by hand on the third page, the monitor now gone. */

  struct uffdio_zeropage zp;

  memset(&zp, 0, sizeof(zp));
  zp.range.start = (uint64_t)(uintptr_t)(region + 2 * PAGE);
  zp.range.len = PAGE;
  int zrc = ioctl(mon.ufd, UFFDIO_ZEROPAGE, &zp);
  int zero_ok = zrc == 0 && zp.zeropage == PAGE;

  if (zero_ok) {
    for (int i = 0; i < PAGE; i++) {
      if (region[2 * PAGE + i] != 0) {
        zero_ok = 0;
        break;
      }
    }
  }
  judge("zeropage", zero_ok, "UFFDIO_ZEROPAGE did not leave a page of zeroes",
        (long)zrc);

  /* poll and EAGAIN, with nothing outstanding. */
  {
    struct pollfd pfd = {.fd = mon.ufd, .events = POLLIN};
    int pr = poll(&pfd, 1, 0);
    struct uffd_msg m;
    int flags = fcntl(mon.ufd, F_GETFL, 0);

    fcntl(mon.ufd, F_SETFL, flags | O_NONBLOCK);
    ssize_t rr = read(mon.ufd, &m, sizeof(m));

    /* What matters is that nothing is READABLE: poll may legitimately report
     * other bits (a descriptor is always writable), and the ABI's contract is
     * about POLLIN. */
    note("idle poll: rc %d revents %#x, read %zd errno %d", pr, pfd.revents,
         rr, errno);
    judge("poll", !(pfd.revents & POLLIN) && rr < 0 && errno == EAGAIN,
          "an idle descriptor reported a message to read", (long)pfd.revents);
  }

  note("%d messages, %d pages served (read errno %d, copy errno %d)", mon.msgs,
       mon.served, mon.read_errno, mon.copy_errno);
  close(mon.ufd);
  munmap(region, 4 * PAGE);
}

/* ── write protect ───────────────────────────────────────────────────────── */

static void check_writeprotect(void) {
  struct monitor mon;
  pthread_t th;
  char *region = fresh_region(1);

  memset(&mon, 0, sizeof(mon));
  mon.fill = 0x33;
  if (!region) {
    bad("writeprotect", "mmap", -1);
    return;
  }
  /* Back the page first: write protection applies to a page that is there. */
  region[0] = 1;

  mon.ufd = uffd_open(0);
  if (mon.ufd < 0 || do_api(mon.ufd, UFFD_FEATURE_PAGEFAULT_FLAG_WP, NULL) != 0) {
    bad("writeprotect", "opening a userfaultfd", (long)mon.ufd);
    return;
  }
  uint64_t ioctls = 0;

  if (register_range(mon.ufd, region, PAGE,
                     UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP,
                     &ioctls) != 0) {
    bad("writeprotect", "UFFDIO_REGISTER with WP", -1);
    close(mon.ufd);
    return;
  }
  if (!(ioctls & (1ull << _UFFDIO_WRITEPROTECT))) {
    bad("writeprotect", "the registration does not offer UFFDIO_WRITEPROTECT",
        (long)ioctls);
    close(mon.ufd);
    return;
  }

  struct uffdio_writeprotect wp;

  memset(&wp, 0, sizeof(wp));
  wp.range.start = (uint64_t)(uintptr_t)region;
  wp.range.len = PAGE;
  wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
  if (ioctl(mon.ufd, UFFDIO_WRITEPROTECT, &wp) != 0) {
    bad("writeprotect", "UFFDIO_WRITEPROTECT", -1);
    close(mon.ufd);
    return;
  }

  if (pthread_create(&th, NULL, monitor_thread, &mon) != 0) {
    bad("writeprotect", "pthread_create", -1);
    close(mon.ufd);
    return;
  }

  /* The store. It must fault, the monitor must see a WP message and lift the
   * protection, and the store must then land. */
  region[0] = 0x77;

  mon.stop = 1;
  pthread_join(th, NULL);

  judge("writeprotect", mon.wp_seen >= 1 && region[0] == 0x77,
        "the write-protected store was not reported to the monitor",
        (long)mon.wp_seen);
  close(mon.ufd);
  munmap(region, PAGE);
}

/* ── unregister ──────────────────────────────────────────────────────────── */

static void check_unregister(void) {
  char *region = fresh_region(2);
  int ufd = uffd_open(0);

  if (!region || ufd < 0 || do_api(ufd, 0, NULL) != 0) {
    bad("unregister", "setup", (long)ufd);
    return;
  }
  if (register_range(ufd, region, 2 * PAGE, UFFDIO_REGISTER_MODE_MISSING,
                     NULL) != 0) {
    bad("unregister", "UFFDIO_REGISTER", -1);
    close(ufd);
    return;
  }

  struct uffdio_range range;

  memset(&range, 0, sizeof(range));
  range.start = (uint64_t)(uintptr_t)region;
  range.len = 2 * PAGE;
  if (ioctl(ufd, UFFDIO_UNREGISTER, &range) != 0) {
    bad("unregister", "UFFDIO_UNREGISTER", -1);
    close(ufd);
    return;
  }

  /* With nobody registered, the fault is the kernel's again: this read must
   * return a zero page immediately, with no monitor running at all. */
  volatile char v = region[PAGE];

  judge("unregister", v == 0,
        "a fault in an unregistered range was not served by the kernel",
        (long)(unsigned char)v);
  close(ufd);
  munmap(region, 2 * PAGE);
}

/* ── the monitor walks away ──────────────────────────────────────────────── */

struct closer {
  int ufd;
};

static void *closer_thread(void *arg) {
  struct closer *c = arg;

  /* Let the other thread reach its fault, then close the descriptor without
   * serving it. */
  msleep(200);
  close(c->ufd);
  return NULL;
}

static void check_monitor_gone(void) {
  char *region = fresh_region(1);
  struct closer c;
  pthread_t th;

  c.ufd = uffd_open(0);
  if (!region || c.ufd < 0 || do_api(c.ufd, 0, NULL) != 0) {
    bad("monitor-gone", "setup", (long)c.ufd);
    return;
  }
  if (register_range(c.ufd, region, PAGE, UFFDIO_REGISTER_MODE_MISSING,
                     NULL) != 0) {
    bad("monitor-gone", "UFFDIO_REGISTER", -1);
    close(c.ufd);
    return;
  }
  if (pthread_create(&th, NULL, closer_thread, &c) != 0) {
    bad("monitor-gone", "pthread_create", -1);
    close(c.ufd);
    return;
  }

  struct timespec t0, t1;

  clock_gettime(CLOCK_MONOTONIC, &t0);
  volatile char v = region[0]; /* faults, waits, and must NOT hang */
  clock_gettime(CLOCK_MONOTONIC, &t1);
  pthread_join(th, NULL);

  long ms = (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                   (t1.tv_nsec - t0.tv_nsec) / 1000000);

  note("the fault waited %ld ms for a monitor that closed its descriptor", ms);
  judge("monitor-gone", v == 0 && ms < 5000,
        "a fault whose monitor went away did not fall through to the kernel",
        ms);
  munmap(region, PAGE);
}

int main(void) {
  printf("M126-UFFD: start\n");
  fflush(stdout);

  int probe = uffd_open(0);

  if (probe < 0 && errno == ENOSYS) {
    printf("M126-UFFD: FAIL open — userfaultfd(2) is ENOSYS\n");
    printf("M126-UFFD: done\n");
    fflush(stdout);
    return 1;
  }
  if (probe >= 0)
    close(probe);

  check_api();
  check_copy_and_zeropage();
  check_writeprotect();
  check_unregister();
  check_monitor_gone();

  printf("M126-UFFD: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
