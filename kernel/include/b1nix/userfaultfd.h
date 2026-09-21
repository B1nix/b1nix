/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * userfaultfd(2) — page faults handled in userspace (M126).
 *
 * A process opens a descriptor, registers a range of its own address space
 * with it, and from then on a fault in that range does not allocate a page:
 * the faulting thread stops and a message appears on the descriptor. Another
 * thread — the monitor — reads the message, decides what the page should
 * contain, and fills it in with UFFDIO_COPY or UFFDIO_ZEROPAGE, which wakes
 * the faulter.
 *
 * That is how CRIU restores a process without reading its whole address space
 * back from disk first, how a live migration moves a guest's memory while it
 * runs, and how a garbage collector can watch writes without mprotect's
 * signal handler. The ABI is Linux's, because every one of those programs is
 * written against it.
 *
 * The constants below are Linux's UAPI values; they are ABI and cannot be
 * chosen differently.
 */
#ifndef B1NIX_USERFAULTFD_H
#define B1NIX_USERFAULTFD_H

#include <b1nix/types.h>

/* userfaultfd(2) flags. */
#define UFFD_CLOEXEC 0x80000  /* O_CLOEXEC */
#define UFFD_NONBLOCK 0x800   /* O_NONBLOCK */
#define UFFD_USER_MODE_ONLY 1

/* The API handshake. */
#define UFFD_API 0xAAul
#define UFFD_API_FEATURES                                                      \
  (UFFD_FEATURE_THREAD_ID | UFFD_FEATURE_PAGEFAULT_FLAG_WP |                   \
   UFFD_FEATURE_MISSING_SHMEM | UFFD_FEATURE_WP_ASYNC)

#define UFFD_FEATURE_PAGEFAULT_FLAG_WP (1ull << 0)
#define UFFD_FEATURE_EVENT_FORK (1ull << 1)
#define UFFD_FEATURE_EVENT_REMAP (1ull << 2)
#define UFFD_FEATURE_EVENT_REMOVE (1ull << 3)
#define UFFD_FEATURE_MISSING_HUGETLBFS (1ull << 4)
#define UFFD_FEATURE_MISSING_SHMEM (1ull << 5)
#define UFFD_FEATURE_EVENT_UNMAP (1ull << 6)
#define UFFD_FEATURE_SIGBUS (1ull << 7)
#define UFFD_FEATURE_THREAD_ID (1ull << 8)
#define UFFD_FEATURE_MINOR_HUGETLBFS (1ull << 9)
#define UFFD_FEATURE_MINOR_SHMEM (1ull << 10)
#define UFFD_FEATURE_EXACT_ADDRESS (1ull << 11)
#define UFFD_FEATURE_WP_HUGETLBFS_SHMEM (1ull << 12)
#define UFFD_FEATURE_WP_UNPOPULATED (1ull << 13)
#define UFFD_FEATURE_POISON (1ull << 14)
#define UFFD_FEATURE_WP_ASYNC (1ull << 15)

/* Registration modes. */
#define UFFDIO_REGISTER_MODE_MISSING (1ull << 0)
#define UFFDIO_REGISTER_MODE_WP (1ull << 1)
#define UFFDIO_REGISTER_MODE_MINOR (1ull << 2)

/* Which ioctls a registration supports, reported back by UFFDIO_REGISTER. */
#define _UFFDIO_WAKE (0x02)
#define _UFFDIO_COPY (0x03)
#define _UFFDIO_ZEROPAGE (0x04)
#define _UFFDIO_WRITEPROTECT (0x06)
#define _UFFDIO_CONTINUE (0x07)

#define UFFD_API_RANGE_IOCTLS                                                  \
  ((1ull << _UFFDIO_WAKE) | (1ull << _UFFDIO_COPY) |                           \
   (1ull << _UFFDIO_ZEROPAGE) | (1ull << _UFFDIO_WRITEPROTECT))

/* The ioctl numbers themselves (_IOWR('\xAA', n, ...) in Linux's terms). */
#define UFFDIO_API 0xC018AA3Ful
#define UFFDIO_REGISTER 0xC020AA00ul
#define UFFDIO_UNREGISTER 0x8010AA01ul
#define UFFDIO_WAKE 0x8010AA02ul
#define UFFDIO_COPY 0xC028AA03ul
#define UFFDIO_ZEROPAGE 0xC020AA04ul
#define UFFDIO_WRITEPROTECT 0xC018AA06ul
#define UFFDIO_CONTINUE 0xC020AA07ul

/* Message types, as read(2) returns them. */
#define UFFD_EVENT_PAGEFAULT 0x12
#define UFFD_EVENT_FORK 0x13
#define UFFD_EVENT_REMAP 0x14
#define UFFD_EVENT_REMOVE 0x15
#define UFFD_EVENT_UNMAP 0x16

/* Flags on a pagefault message. */
#define UFFD_PAGEFAULT_FLAG_WRITE (1ull << 0)
#define UFFD_PAGEFAULT_FLAG_WP (1ull << 1)
#define UFFD_PAGEFAULT_FLAG_MINOR (1ull << 2)

/* Copy and write-protect modifiers. */
#define UFFDIO_COPY_MODE_DONTWAKE (1ull << 0)
#define UFFDIO_COPY_MODE_WP (1ull << 1)
#define UFFDIO_ZEROPAGE_MODE_DONTWAKE (1ull << 0)
#define UFFDIO_WRITEPROTECT_MODE_WP (1ull << 0)
#define UFFDIO_WRITEPROTECT_MODE_DONTWAKE (1ull << 1)

struct uffdio_api {
  u64 api;
  u64 features;
  u64 ioctls;
};

struct uffdio_range {
  u64 start;
  u64 len;
};

struct uffdio_register {
  struct uffdio_range range;
  u64 mode;
  u64 ioctls;
};

struct uffdio_copy {
  u64 dst;
  u64 src;
  u64 len;
  u64 mode;
  i64 copy;
};

struct uffdio_zeropage {
  struct uffdio_range range;
  u64 mode;
  i64 zeropage;
};

struct uffdio_writeprotect {
  struct uffdio_range range;
  u64 mode;
};

/* The message read(2) hands the monitor. Linux's layout, 32 bytes. */
struct uffd_msg {
  u8 event;
  u8 reserved1;
  u16 reserved2;
  u32 reserved3;
  union {
    struct {
      u64 flags;
      u64 address;
      union {
        u32 ptid;
      } feat;
    } pagefault;
    struct {
      u32 ufd;
    } fork;
    struct {
      u64 from;
      u64 to;
      u64 len;
    } remap;
    struct {
      u64 start;
      u64 end;
    } remove;
    struct {
      u64 reserved1;
      u64 reserved2;
      u64 reserved3;
    } reserved;
  } arg;
};

/* ── what the rest of the kernel calls ─────────────────────────────────── */

/* The system-call hook: returns 1 and sets *ret when `nr` is userfaultfd's. */
int userfaultfd_syscall(u64 nr, u64 a0, u64 *ret);

/*
 * A fault on an address that may belong to a registered range.
 *
 * `write` says whether the access was a store, and `present` whether a page is
 * already mapped there (which distinguishes a missing fault from a
 * write-protect one). Returns 1 when a monitor was told and has since answered
 * -- the caller must then retry the access rather than service it itself -- and
 * 0 when no userfaultfd covers the address and the ordinary path should run.
 *
 * Called with no VM lock held: it blocks the faulting thread.
 */
int uffd_handle_fault(u64 fault_addr, int write, int present);

/* A task is going away: release anything it was blocked on. */
struct task;
void uffd_task_exit(struct task *t);

#endif
