/* SPDX-License-Identifier: GPL-2.0-only */
/* io_uring — the shared submission and completion rings (M125).
 *
 * WHAT THIS IS
 *
 * io_uring_setup(2) hands back a descriptor and three regions of kernel memory
 * the process maps: the submission ring, the completion ring and the array of
 * submission queue entries. Userspace writes an SQE, advances the SQ tail and
 * calls io_uring_enter(2); the kernel consumes the entries, performs them and
 * writes a CQE for each. The ABI — every structure, every flag value, the
 * head/tail protocol — is Linux's, taken verbatim from Linux 6.18.51 in
 * <b1nix/io_uring_abi.h>. liburing is compiled against those structures, so a
 * near-miss is worse than an absence.
 *
 * WHAT THIS IS NOT, AND WHY IT SAYS SO OUT LOUD
 *
 * Userspace probes for io_uring and switches strategy wholesale when it finds
 * it — the same lesson kernel/fs/mount_api.c records for the new mount API. So
 * everything this kernel cannot honour is refused where it can still be
 * refused cleanly:
 *
 *   - io_uring_setup() rejects, with -EINVAL, every IORING_SETUP_* flag it
 *     cannot implement (SQPOLL, IOPOLL, SQE128, CQE32, NO_MMAP, ATTACH_WQ,
 *     DEFER_TASKRUN, REGISTERED_FD_ONLY, NO_SQARRAY, ...), exactly as an older
 *     Linux does for a flag it has never heard of.
 *   - params->features advertises only what is true here.
 *   - An opcode that is not implemented completes with -EINVAL, and
 *     IORING_REGISTER_PROBE reports it as unsupported, which is the mechanism
 *     the ABI provides for exactly this question. liburing's
 *     io_uring_opcode_supported() reads that table, so a program asks rather
 *     than guesses.
 *
 * HOW A REQUEST MAKES PROGRESS
 *
 * There is no thread per request and no io-wq pool. A request is issued in the
 * context of the task that submitted it:
 *
 *   - If its file cannot block (a regular file, a device), it is performed
 *     inline and its CQE is posted before io_uring_enter returns.
 *   - If its file is pollable (socket, pipe, tty, eventfd, ...), the readiness
 *     of that file is tested first with the file's own ->poll op. Ready means
 *     perform it now; not ready means the request is ARMED and left on the
 *     ring, and io_uring_enter's wait loop re-tests it every time the global
 *     readiness channel vfs_poll_chan is woken — which is what every ISR,
 *     every socket receive path and the timer tick already do. That is the
 *     existing completion machinery (M70), used rather than duplicated, and it
 *     is what IORING_FEAT_FAST_POLL describes.
 *
 * The consequence, stated plainly because it is a real limit: an armed request
 * makes progress while its owner is inside io_uring_enter (or enters it again).
 * A program that submits and then blocks somewhere else entirely — select() on
 * an unrelated descriptor, say — does not get that completion until it comes
 * back. Every liburing and fio submission pattern does come back, because that
 * is what io_uring_enter(GETEVENTS) is for. SQPOLL, which would need a kernel
 * thread of its own, is refused rather than faked.
 *
 * WHY THE RINGS ARE A DEVICE NODE
 *
 * sys_mmap() resolves a descriptor through vfs_find_node_by_fd(), which insists
 * on a VFS_HANDLE_NODE handle with a node behind it, and maps kernel-owned
 * physical pages only for a VFS_DEVICE inode carrying
 * ->mmap_handle_page_phys_cb (the path drm uses). So the ring descriptor is a
 * node handle over an anonymous device node, with the ring context on
 * handle->private_data, and the magic offsets IORING_OFF_SQ_RING /
 * IORING_OFF_CQ_RING / IORING_OFF_SQES arrive at that callback unchanged.
 */

#include <b1nix/io_uring.h>

#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/ktime.h>
#include <b1nix/mm.h>
#include <b1nix/linux_abi.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/vfs.h>

#include <stdio.h>
#include <string.h>

#include "../syscall/linux_modern.h"

extern void *vfs_poll_chan;

/* ---- limits ------------------------------------------------------------- */

/* Linux's own ceilings are 32768 SQ / 65536 CQ entries. The rings here are
 * physically contiguous (one pmm_alloc_frames run each), so the ceiling is set
 * by what a contiguous allocation can be expected to satisfy rather than by the
 * ABI: 4096 SQEs is 256 KiB, the largest single run this asks for. A request
 * above the cap is clamped when IORING_SETUP_CLAMP is set and refused with
 * -EINVAL otherwise, which is what Linux does. */
#define IOU_MAX_SQ_ENTRIES 4096u
#define IOU_MAX_CQ_ENTRIES 8192u
/* A registered file set and a registered buffer set. Linux allows far more;
 * these are what a table of pointers costs here. */
#define IOU_MAX_FIXED_FILES 8192u
#define IOU_MAX_FIXED_BUFS  1024u
/* The bounce buffer a single copy through the kernel uses. File I/O larger
 * than this is done in several passes; a socket transfer is capped at it, as
 * the send/recv system calls are (SOCKET_IO_MAX in kernel/syscall/syscall.c). */
#define IOU_BOUNCE 65536u
/* Live requests per ring. A ring cannot have more outstanding than it has SQ
 * entries in flight plus the timeouts and poll registrations it added, so this
 * only bounds a pathological submitter. */
#define IOU_MAX_INFLIGHT 8192u

/* ---- the ring ----------------------------------------------------------- */

/* The fixed head of the mapped ring region. Offsets published in
 * io_sqring_offsets / io_cqring_offsets point into this and at the two arrays
 * that follow it, so the layout is ours to choose — what must be exact is that
 * the offsets describe it truthfully. */
struct iou_ring_hdr {
  u32 sq_head;
  u32 sq_tail;
  u32 sq_ring_mask;
  u32 sq_ring_entries;
  u32 sq_flags;
  u32 sq_dropped;
  u32 cq_head;
  u32 cq_tail;
  u32 cq_ring_mask;
  u32 cq_ring_entries;
  u32 cq_overflow;
  u32 cq_flags;
  u32 reserved[4];
};

#define IOU_OFF_SQ_HEAD      0u
#define IOU_OFF_SQ_TAIL      4u
#define IOU_OFF_SQ_MASK      8u
#define IOU_OFF_SQ_ENTRIES   12u
#define IOU_OFF_SQ_FLAGS     16u
#define IOU_OFF_SQ_DROPPED   20u
#define IOU_OFF_CQ_HEAD      24u
#define IOU_OFF_CQ_TAIL      28u
#define IOU_OFF_CQ_MASK      32u
#define IOU_OFF_CQ_ENTRIES   36u
#define IOU_OFF_CQ_OVERFLOW  40u
#define IOU_OFF_CQ_FLAGS     44u
#define IOU_OFF_SQ_ARRAY     64u

struct iou_fixed_file {
  struct vfs_handle *file; /* retained */
};

struct iou_fixed_buf {
  u64 addr;
  u64 len;
};

/* struct iovec, as userspace lays it out. */
struct iou_iovec {
  u64 base;
  u64 len;
};

/* A completion that had nowhere to go. IORING_FEAT_NODROP promises a CQE is
 * never lost, so an overflow is parked here and flushed into the ring the
 * moment userspace consumes one. */
struct iou_overflow {
  struct iou_overflow *next;
  struct io_uring_cqe cqe;
};

enum iou_state {
  IOU_ST_QUEUED = 0, /* in a link chain, not yet issued */
  IOU_ST_ARMED,      /* issued, waiting for readiness or a deadline */
};

struct iou_req {
  struct iou_req *next;      /* ctx->live */
  struct iou_req *link_next; /* the next request of this link chain */

  struct io_uring_sqe sqe;
  struct io_ring_ctx *ctx;

  struct vfs_handle *file; /* retained for the life of the request, or NULL */

  u8 state;
  u8 hardlink;    /* IOSQE_IO_HARDLINK: the chain survives a failure */
  u8 cqe_skip;    /* IOSQE_CQE_SKIP_SUCCESS */
  u8 has_deadline;
  u8 is_timeout;  /* IORING_OP_TIMEOUT */
  u8 is_linktmo;  /* IORING_OP_LINK_TIMEOUT */
  u8 timeout_etime_success;
  /* A request that was already doomed when it was built — a bad descriptor, a
   * flag combination this kernel refuses, a timespec that would not copy in.
   * It is STILL made into a request and still put on its link chain, because a
   * failure is one of the two things a chain reacts to: completing it out of
   * band left the rest of the chain running as if nothing had gone wrong. */
  u8 failed;
  i32 fail_res;

  u16 poll_mask; /* what readiness this request is armed on; 0 = none */

  /* Multishot: one SQE that reports many times. Every report but the last
   * carries IORING_CQE_F_MORE, and the request stays armed. */
  u8 multishot;
  /* A multishot POLL_ADD reports once per readiness EDGE. The file's ->poll is
   * level-triggered, so a socket left unread would otherwise produce a CQE on
   * every sweep until the ring overflowed; Linux reports once per wakeup,
   * which for an unconsumed readiness is once. This flag is what remembers
   * that the current readiness has already been reported. */
  u8 mshot_reported;
  u8 buf_select; /* IOSQE_BUFFER_SELECT: the buffer comes from a group */
  u8 is_poll_update; /* IORING_POLL_UPDATE_*: change a poll already armed */
  u16 buf_group;
  u32 sel_len; /* sqe->len as submitted; a buffer pick overwrites sqe->len */
  /* sqe->file_index: a direct (registered) descriptor slot to install the
   * result in, biased by one, or IORING_FILE_INDEX_ALLOC. 0 means "give the
   * process an ordinary descriptor". */
  u32 file_index;

  /* READV/WRITEV: the iovec array, copied at SUBMISSION.
   * IORING_FEAT_SUBMIT_STABLE says everything the SQE points at for the
   * description of the request is the kernel's once io_uring_enter returns,
   * and liburing's pipe-reuse test says it by overwriting the array with
   * rubbish the instant submit comes back. Reading it at execution time
   * therefore reads the rubbish. */
  struct iou_iovec *iov;
  u32 iovcnt;

  u64 deadline_ns;   /* CLOCK_MONOTONIC deadline when has_deadline */
  u64 timeout_target; /* IORING_OP_TIMEOUT: cq_posted value that satisfies it */
  u32 timeout_count;

  /* IORING_OP_LINK_TIMEOUT arms against the request before it in the chain. */
  struct iou_req *tmo_target;
  struct iou_req *tmo_armed; /* the link timeout watching THIS request */
};

struct io_ring_ctx {
  struct io_ring_ctx *next; /* g_ctx_list */
  volatile int refs;        /* the handle holds one, the inode holds one */
  volatile int lock;

  usize owner_tgid;

  u32 flags;
  u32 sq_entries, cq_entries;
  u32 sq_mask, cq_mask;
  int enabled; /* cleared by IORING_SETUP_R_DISABLED until ENABLE_RINGS */

  /* the two physically contiguous regions */
  u64 ring_phys;
  usize ring_pages;
  u64 sqes_phys;
  usize sqes_pages;

  struct iou_ring_hdr *hdr;
  u32 *sq_array;
  struct io_uring_cqe *cqes;
  struct io_uring_sqe *sqes;

  u32 sq_local_head; /* what the kernel has consumed */
  u64 cq_posted;     /* CQEs that count towards IORING_OP_TIMEOUT's count */
  /* How many IORING_OP_TIMEOUT requests have ever fired on this ring.
   *
   * A timeout firing ALWAYS returns the waiter to userspace, however many
   * completions it asked for: that is the whole point of arming one before a
   * long wait, and liburing's test_single_timeout_many submits a single
   * timeout and then waits for four completions on purpose. Comparing this
   * against the value the wait started with is how Linux says the same thing
   * (io_should_wake's cq_timeouts check). */
  u64 cq_timeouts;

  struct iou_req *live;
  u32 nr_live;

  struct iou_overflow *ovfl_head, *ovfl_tail;

  struct iou_fixed_file *files;
  u32 nr_files;
  struct iou_fixed_buf *bufs;
  u32 nr_bufs;

  struct vfs_handle *cq_eventfd; /* retained */
  int eventfd_async; /* IORING_REGISTER_EVENTFD_ASYNC */

  /* Provided buffers: the classic IORING_OP_PROVIDE_BUFFERS lists and the
   * IORING_REGISTER_PBUF_RING rings, both keyed by group id. */
  struct iou_bgroup *bgroups;

  /* IORING_SETUP_SQPOLL: a kernel thread of this ring's own that consumes the
   * submission queue without the owner entering the kernel. */
  int sq_tid;
  u32 sq_idle_ms;
  volatile int sq_stop;
  volatile int sq_alive;
  int sq_wait; /* the address is the channel the submission thread sleeps on */

  /* IORING_REGISTER_FILE_ALLOC_RANGE: where IORING_FILE_INDEX_ALLOC looks. */
  u32 falloc_off, falloc_len;

  /* IORING_REGISTER_RESTRICTIONS, which only has meaning before
   * IORING_REGISTER_ENABLE_RINGS. */
  u8 restricted;
  u8 restr_register[IORING_REGISTER_LAST];
  u8 restr_sqe[IORING_OP_LAST];
  u8 restr_flags_allowed;
  u8 restr_flags_required;

  /* IORING_REGISTER_IOWQ_MAX_WORKERS: bounded and reported truthfully — see
   * the note where it is handled. */
  u32 iowq_max[2];

  /* Ring shape. IORING_SETUP_CQE32 and IORING_SETUP_SQE128 double the entry
   * size, so every index into cqes/sqes is a byte offset rather than an array
   * subscript. */
  u32 cqe_size;
  u32 sqe_size;

  struct vfs_node *node;
};

/* ---- provided buffers ---------------------------------------------------
 *
 * Two shapes of the same idea: the program hands the kernel a set of buffers
 * under a group id, and an SQE marked IOSQE_BUFFER_SELECT names the group
 * instead of an address. The kernel picks one, uses it, and reports which in
 * the completion (IORING_CQE_F_BUFFER, buffer id in the top 16 bits).
 *
 *   - IORING_OP_PROVIDE_BUFFERS builds a list the kernel owns, consumed in
 *     buffer-id order and refilled by submitting the opcode again.
 *   - IORING_REGISTER_PBUF_RING registers a ring the PROGRAM owns and writes:
 *     the kernel reads the entries out of user memory and keeps only its own
 *     head. That is the whole point of the ring form, so the entries are read
 *     with syscall_copyin at selection time rather than cached.
 */
struct iou_pbuf {
  u64 addr;
  u32 len;
  u16 bid;
};

struct iou_bgroup {
  struct iou_bgroup *next;
  u16 bgid;

  /* The list form. */
  struct iou_pbuf *list;
  u32 nr, cap, head;

  /* The ring form. */
  int is_ring;
  u64 ring_addr;
  u32 ring_entries;
  u16 ring_mask;
  u16 ring_head;
};

/* The offset of io_uring_buf_ring::tail, which is overlaid on the first
 * entry's resv field: __u64 + __u32 + __u16. */
#define IOU_PBUF_RING_TAIL_OFF 14u

static struct io_ring_ctx *g_ctx_list;
static volatile int g_ctx_list_lock;

static void iou_list_lock(void) {
  while (__atomic_test_and_set(&g_ctx_list_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
}

static void iou_list_unlock(void) {
  __atomic_clear(&g_ctx_list_lock, __ATOMIC_RELEASE);
}

/* The per-ring lock is a plain spin-with-yield, like the rest of this file's
 * neighbours (aio.c, eventpoll.c). No interrupt handler ever takes it: a ring
 * is only ever touched by the task that owns it and by a teardown, so there is
 * nothing for spin_lock_irqsave to protect against here. Nothing sleeps while
 * it is held — the issue paths take it, drop it, do the I/O, and take it again
 * to post. */
static void iou_lock(struct io_ring_ctx *ctx) {
  while (__atomic_test_and_set(&ctx->lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
}

static void iou_unlock(struct io_ring_ctx *ctx) {
  __atomic_clear(&ctx->lock, __ATOMIC_RELEASE);
}

static void iou_trace(const char *what, u64 a, u64 b, isize rc) {
  static int on = -1;

  if (on < 0)
    on = bootinfo_has_flag("b1nix.trace-iouring") ? 1 : 0;
  if (!on)
    return;

  char line[160];
  snprintf(line, sizeof(line), "io_uring: %s a=%u b=%u -> %d", what, (u32)a,
           (u32)b, (int)rc);
  klog_info(line);
}

/* ---- ring memory -------------------------------------------------------- */

/* `sq_array_entries` is zero for IORING_SETUP_NO_SQARRAY, where the ring has
 * no indirection array at all and the SQ head/tail index the SQEs directly. */
static usize iou_ring_bytes(u32 sq_array_entries, u32 cq_entries, u32 cqe_size,
                            u32 *cqes_off) {
  usize off = IOU_OFF_SQ_ARRAY + (usize)sq_array_entries * sizeof(u32);

  off = (off + 63u) & ~(usize)63u;
  if (cqes_off)
    *cqes_off = (u32)off;
  return off + (usize)cq_entries * cqe_size;
}

static u64 iou_alloc_region(usize bytes, usize *out_pages) {
  usize pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  u64 phys;

  if (pages == 0)
    pages = 1;
  phys = pmm_alloc_frames(pages);
  if (!phys)
    return 0;
  *out_pages = pages;
  return phys;
}

static void iou_free_region(u64 phys, usize pages) {
  if (!phys)
    return;
  for (usize i = 0; i < pages; i++)
    pmm_free_frame(phys + (u64)i * PAGE_SIZE);
}

static void *iou_kva(u64 phys) {
  return (void *)(usize)(phys + vmm_direct_map_base());
}

/* A CQE by ring index. With IORING_SETUP_CQE32 the stride is 32 bytes, not
 * sizeof(struct io_uring_cqe), so every access goes through here. */
static struct io_uring_cqe *iou_cqe_at(struct io_ring_ctx *ctx, u32 idx) {
  return (struct io_uring_cqe *)((char *)ctx->cqes +
                                 (usize)(idx & ctx->cq_mask) * ctx->cqe_size);
}

/* An SQE by its index in the SQE array; the stride is 128 with
 * IORING_SETUP_SQE128. */
static const struct io_uring_sqe *iou_sqe_at(struct io_ring_ctx *ctx,
                                             u32 idx) {
  return (const struct io_uring_sqe *)((const char *)ctx->sqes +
                                       (usize)idx * ctx->sqe_size);
}

/* Write a completion into the ring slot, zeroing the two extra words a 32-byte
 * CQE carries so a stale value from an earlier lap cannot be read as data. */
static void iou_store_cqe(struct io_ring_ctx *ctx, u32 tail, u64 user_data,
                          i32 res, u32 cflags) {
  struct io_uring_cqe *cqe = iou_cqe_at(ctx, tail);

  cqe->user_data = user_data;
  cqe->res = res;
  cqe->flags = cflags;
  if (ctx->cqe_size >= 32) {
    u64 *big = (u64 *)(void *)(cqe + 1);

    big[0] = 0;
    big[1] = 0;
  }
}

/* The mmap hook. Offset is what the caller passed to mmap(2), unchanged, so it
 * is one of the magic constants plus a page index inside that region. */
static int iou_mmap_page_phys(struct vfs_handle *handle, u64 offset,
                              u64 *out_phys) {
  struct io_ring_ctx *ctx;
  u64 base, region_bytes, within;
  usize pages;

  if (!handle || !handle->private_data)
    return -EBADF;
  ctx = (struct io_ring_ctx *)handle->private_data;

  if (offset >= IORING_OFF_SQES) {
    base = ctx->sqes_phys;
    pages = ctx->sqes_pages;
    within = offset - IORING_OFF_SQES;
  } else if (offset >= IORING_OFF_CQ_RING) {
    /* IORING_FEAT_SINGLE_MMAP: one region holds both rings, and the CQ offsets
     * published in io_cqring_offsets are offsets into it. A caller that maps
     * IORING_OFF_CQ_RING anyway — liburing does when the feature bit is
     * absent, and an application may do it regardless — gets the same pages. */
    base = ctx->ring_phys;
    pages = ctx->ring_pages;
    within = offset - IORING_OFF_CQ_RING;
  } else {
    base = ctx->ring_phys;
    pages = ctx->ring_pages;
    within = offset;
  }

  region_bytes = (u64)pages * PAGE_SIZE;
  if (!base || (within & (PAGE_SIZE - 1)) || within >= region_bytes)
    return -EINVAL;
  *out_phys = base + within;
  return 0;
}

/* ---- completions -------------------------------------------------------- */

static void iou_signal_eventfd(struct io_ring_ctx *ctx) {
  struct vfs_handle *efd = ctx->cq_eventfd;
  u64 one = 1;

  if (!efd || !efd->ops || !efd->ops->write)
    return;
  /* IORING_CQ_EVENTFD_DISABLED is userspace's switch for exactly this. */
  if (__atomic_load_n(&ctx->hdr->cq_flags, __ATOMIC_ACQUIRE) &
      IORING_CQ_EVENTFD_DISABLED)
    return;
  efd->ops->write(efd, (const char *)&one, sizeof(one));
}

/* Move whatever the backlog holds into the ring, as far as it fits. */
static void iou_flush_overflow_locked(struct io_ring_ctx *ctx) {
  while (ctx->ovfl_head) {
    u32 tail = __atomic_load_n(&ctx->hdr->cq_tail, __ATOMIC_RELAXED);
    u32 head = __atomic_load_n(&ctx->hdr->cq_head, __ATOMIC_ACQUIRE);

    if (tail - head >= ctx->cq_entries)
      return;

    struct iou_overflow *o = ctx->ovfl_head;

    ctx->ovfl_head = o->next;
    if (!ctx->ovfl_head)
      ctx->ovfl_tail = 0;
    iou_store_cqe(ctx, tail, o->cqe.user_data, o->cqe.res, o->cqe.flags);
    __atomic_store_n(&ctx->hdr->cq_tail, tail + 1, __ATOMIC_RELEASE);
    kfree(o);
  }
  if (!ctx->ovfl_head) {
    u32 f = __atomic_load_n(&ctx->hdr->sq_flags, __ATOMIC_RELAXED);

    __atomic_store_n(&ctx->hdr->sq_flags, f & ~(u32)IORING_SQ_CQ_OVERFLOW,
                     __ATOMIC_RELEASE);
  }
}

/* `counts` is whether this completion counts towards IORING_OP_TIMEOUT's
 * "wake me after N completions". A timeout's OWN completion does not: two
 * timeouts armed together, each asked to fire after one completion, otherwise
 * satisfy each other — the first to expire posts a CQE, the second sees the
 * count met and reports success instead of the ETIME it owed. */
static void iou_post_cqe_locked(struct io_ring_ctx *ctx, u64 user_data,
                                i32 res, u32 cflags, int counts) {
  iou_flush_overflow_locked(ctx);

  u32 tail = __atomic_load_n(&ctx->hdr->cq_tail, __ATOMIC_RELAXED);
  u32 head = __atomic_load_n(&ctx->hdr->cq_head, __ATOMIC_ACQUIRE);

  if (counts)
    ctx->cq_posted++;

  if (!ctx->ovfl_head && tail - head < ctx->cq_entries) {
    iou_store_cqe(ctx, tail, user_data, res, cflags);
    __atomic_store_n(&ctx->hdr->cq_tail, tail + 1, __ATOMIC_RELEASE);
    return;
  }

  /* IORING_FEAT_NODROP: park it rather than lose it. */
  struct iou_overflow *o = kzalloc(sizeof(*o));

  if (!o) {
    /* The one case where a completion really cannot be kept. Count it where
     * the ABI says counts of lost completions go, so the number is visible
     * rather than silently absent. */
    u32 ov = __atomic_load_n(&ctx->hdr->cq_overflow, __ATOMIC_RELAXED);

    __atomic_store_n(&ctx->hdr->cq_overflow, ov + 1, __ATOMIC_RELEASE);
    return;
  }
  o->cqe.user_data = user_data;
  o->cqe.res = res;
  o->cqe.flags = cflags;
  o->next = 0;
  if (ctx->ovfl_tail)
    ctx->ovfl_tail->next = o;
  else
    ctx->ovfl_head = o;
  ctx->ovfl_tail = o;

  u32 ov = __atomic_load_n(&ctx->hdr->cq_overflow, __ATOMIC_RELAXED);

  __atomic_store_n(&ctx->hdr->cq_overflow, ov + 1, __ATOMIC_RELEASE);
  u32 f = __atomic_load_n(&ctx->hdr->sq_flags, __ATOMIC_RELAXED);

  __atomic_store_n(&ctx->hdr->sq_flags, f | IORING_SQ_CQ_OVERFLOW,
                   __ATOMIC_RELEASE);
}

static void iou_post_cqe_n(struct io_ring_ctx *ctx, u64 user_data, i32 res,
                           u32 cflags, int counts) {
  iou_trace("cqe", user_data, (u64)(u32)cflags, res);
  iou_lock(ctx);
  iou_post_cqe_locked(ctx, user_data, res, cflags, counts);
  iou_unlock(ctx);
  iou_signal_eventfd(ctx);
  scheduler_wake_all(ctx);
}

/* A completion for a request whose own kind decides whether it counts. */
static void iou_post_req_cqe(struct iou_req *req, i32 res, u32 cflags) {
  if (req->is_timeout)
    __atomic_add_fetch(&req->ctx->cq_timeouts, 1, __ATOMIC_RELEASE);
  iou_post_cqe_n(req->ctx, req->sqe.user_data, res, cflags,
                 (req->is_timeout || req->is_linktmo) ? 0 : 1);
}

static u32 iou_cq_ready(struct io_ring_ctx *ctx) {
  u32 tail = __atomic_load_n(&ctx->hdr->cq_tail, __ATOMIC_ACQUIRE);
  u32 head = __atomic_load_n(&ctx->hdr->cq_head, __ATOMIC_RELAXED);

  return tail - head;
}

/* ---- request bookkeeping ------------------------------------------------ */

static void iou_req_link(struct io_ring_ctx *ctx, struct iou_req *req) {
  iou_lock(ctx);
  req->next = ctx->live;
  ctx->live = req;
  ctx->nr_live++;
  iou_unlock(ctx);
}

static void iou_req_unlink(struct io_ring_ctx *ctx, struct iou_req *req) {
  struct iou_req **pp = &ctx->live;

  while (*pp) {
    if (*pp == req) {
      *pp = req->next;
      req->next = 0;
      if (ctx->nr_live)
        ctx->nr_live--;
      return;
    }
    pp = &(*pp)->next;
  }
}

static void iou_req_free(struct iou_req *req) {
  if (!req)
    return;
  if (req->file)
    vfs_handle_release(req->file);
  if (req->iov)
    kfree(req->iov);
  kfree(req);
}

/* ---- file resolution ---------------------------------------------------- */

/* Resolve the SQE's file. A fixed file is taken from the registered set, where
 * the ring holds its own reference — that is what registering a file buys, and
 * it keeps working after the descriptor that registered it is closed. A plain
 * descriptor is resolved once, here, and retained for the request's life so a
 * close(2) between submission and completion cannot pull it out. */
static struct vfs_handle *iou_get_file(struct io_ring_ctx *ctx,
                                       const struct io_uring_sqe *sqe,
                                       int *err) {
  struct vfs_handle *h;

  *err = 0;
  if (sqe->flags & IOSQE_FIXED_FILE) {
    u32 idx = (u32)sqe->fd;

    if (!ctx->files || idx >= ctx->nr_files || !ctx->files[idx].file) {
      *err = -EBADF;
      return 0;
    }
    h = ctx->files[idx].file;
    vfs_handle_retain(h);
    return h;
  }
  if (sqe->fd < 0) {
    *err = -EBADF;
    return 0;
  }
  h = scheduler_fd_get_retain(sqe->fd);
  if (!h) {
    *err = -EBADF;
    return 0;
  }
  return h;
}

/* Whether a descriptor's readiness is a real question. A regular file or a
 * device answers "always ready" and its read can only be as slow as the disk;
 * a socket, pipe or tty can have nothing there for hours, and that is the case
 * a request must be armed for rather than block the submitter in. */
static int iou_pollable(struct vfs_handle *h) {
  if (!h || !h->ops || !h->ops->poll)
    return 0;
  switch (h->kind) {
  case VFS_HANDLE_PIPE_READ:
  case VFS_HANDLE_PIPE_WRITE:
  case VFS_HANDLE_SOCKET:
  case VFS_HANDLE_PTY_MASTER:
  case VFS_HANDLE_PTY_SLAVE:
  case VFS_HANDLE_SERIAL_TTY:
  case VFS_HANDLE_INPUT:
  case VFS_HANDLE_EVENTFD:
  case VFS_HANDLE_TIMERFD:
  case VFS_HANDLE_SIGNALFD:
  case VFS_HANDLE_EPOLL:
  case VFS_HANDLE_INOTIFY:
  case VFS_HANDLE_PIDFD:
    return 1;
  default:
    return 0;
  }
}

static u16 iou_poll_now(struct vfs_handle *h, u16 want) {
  struct b1nix_pollfd pfd;

  if (!h || !h->ops || !h->ops->poll)
    return want; /* nothing to ask; treat as ready */
  pfd.fd = -1;
  pfd.events = (short)(want | B1NIX_POLLERR | B1NIX_POLLHUP);
  pfd.revents = 0;
  h->ops->poll(h, &pfd);
  return (u16)(unsigned short)pfd.revents;
}

/* Whether this request's sqe->off is a real offset.
 *
 * -1 means "the descriptor's own position" (IORING_FEAT_RW_CUR_POS), a file
 * with no position at all cannot take one, and O_APPEND overrides any offset
 * a caller passes — liburing's prep helpers put 0 in sqe->off for a plain
 * write, and honouring that on an O_APPEND descriptor writes every record over
 * the first one instead of appending them. */
static int iou_use_offset(const struct vfs_handle *h,
                          const struct io_uring_sqe *sqe) {
  if (sqe->off == (u64)-1)
    return 0;
  if (!h || h->kind != VFS_HANDLE_NODE)
    return 0;
  if (h->flags & B1NIX_O_APPEND)
    return 0;
  return 1;
}

/* The two *at() flags posix.h does not carry, with Linux's values. */
#define IOU_AT_REMOVEDIR      0x200
#define IOU_AT_SYMLINK_FOLLOW 0x400

/* accept4(2) flag values, as userspace passes them in sqe->accept_flags. */
#define IOU_SOCK_NONBLOCK 0x0800
#define IOU_SOCK_CLOEXEC  0x80000

/* ---- data movement ------------------------------------------------------ */

/* Whether another pass over a descriptor that can block is safe to make.
 *
 * A descriptor that cannot block is always yes. One that can — a pipe, a
 * socket — must be asked again between passes, because a transfer that
 * satisfied the first pass exactly says nothing about the second: liburing's
 * pipe-reuse test writes 8 KiB into a pipe and submits a 16-vector read of
 * 16 KiB, and the second vector's read parked the submitting task in the pipe
 * for ever. Short is the right answer there, and it is the answer read(2)
 * gives. */
static int iou_more_ok(struct vfs_handle *h, u16 mask) {
  if (!iou_pollable(h))
    return 1;
  return (iou_poll_now(h, mask) & (mask | B1NIX_POLLERR | B1NIX_POLLHUP)) != 0;
}

/* A read into a user buffer, in bounce-sized passes. `offp` is NULL for a read
 * at the descriptor's own position. */
static isize iou_read_user(struct vfs_handle *h, u64 uaddr, u32 len,
                           const u64 *offp) {
  char *bounce;
  isize done = 0;

  if (len == 0)
    return 0;
  bounce = kmalloc(len < IOU_BOUNCE ? len : IOU_BOUNCE);
  if (!bounce)
    return -ENOMEM;

  while ((u32)done < len) {
    usize chunk = len - (u32)done;
    isize n;

    if (chunk > IOU_BOUNCE)
      chunk = IOU_BOUNCE;
    if (done > 0 && !iou_more_ok(h, B1NIX_POLLIN))
      break;
    if (offp)
      n = vfs_pread_h(h, bounce, chunk, *offp + (u64)done);
    else
      n = vfs_handle_read(h, bounce, chunk);
    if (n < 0) {
      if (done > 0)
        break;
      kfree(bounce);
      return n;
    }
    if (n == 0)
      break;
    if (syscall_copyout((void *)(usize)(uaddr + (u64)done), bounce,
                        (usize)n) < 0) {
      kfree(bounce);
      return done > 0 ? done : -EFAULT;
    }
    done += n;
    if ((usize)n < chunk)
      break; /* short read: EOF, or all the device had */
  }
  kfree(bounce);
  return done;
}

static isize iou_write_user(struct vfs_handle *h, u64 uaddr, u32 len,
                            const u64 *offp) {
  char *bounce;
  isize done = 0;

  if (len == 0)
    return 0;
  bounce = kmalloc(len < IOU_BOUNCE ? len : IOU_BOUNCE);
  if (!bounce)
    return -ENOMEM;

  while ((u32)done < len) {
    usize chunk = len - (u32)done;
    isize n;

    if (chunk > IOU_BOUNCE)
      chunk = IOU_BOUNCE;
    if (done > 0 && !iou_more_ok(h, B1NIX_POLLOUT))
      break;
    if (syscall_copyin(bounce, (const void *)(usize)(uaddr + (u64)done),
                       chunk) < 0) {
      kfree(bounce);
      return done > 0 ? done : -EFAULT;
    }
    if (offp)
      n = vfs_pwrite_h(h, bounce, chunk, *offp + (u64)done);
    else
      n = vfs_handle_write(h, bounce, chunk);
    if (n < 0) {
      if (done > 0)
        break;
      kfree(bounce);
      return n;
    }
    if (n == 0)
      break;
    done += n;
    if ((usize)n < chunk)
      break;
  }
  kfree(bounce);
  return done;
}

static isize iou_rw_vectored(struct vfs_handle *h, const struct iou_iovec *iov,
                             u32 iovcnt, const u64 *offp, int is_write) {
  isize total = 0;
  u64 pos = offp ? *offp : 0;
  u16 mask = is_write ? B1NIX_POLLOUT : B1NIX_POLLIN;

  if (iovcnt == 0 || !iov)
    return 0;

  for (u32 i = 0; i < iovcnt; i++) {
    isize n;

    if (iov[i].len == 0)
      continue;
    if (iov[i].len > 0xffffffffull) {
      if (total == 0)
        total = -EINVAL;
      break;
    }
    if (total > 0 && !iou_more_ok(h, mask))
      break;
    if (is_write)
      n = iou_write_user(h, iov[i].base, (u32)iov[i].len, offp ? &pos : 0);
    else
      n = iou_read_user(h, iov[i].base, (u32)iov[i].len, offp ? &pos : 0);
    if (n < 0) {
      if (total == 0)
        total = n;
      break;
    }
    total += n;
    pos += (u64)n;
    if ((u64)n < iov[i].len)
      break; /* short: stop, as readv/writev do */
  }
  return total;
}

/* A registered buffer names a range the ring was told about. The request's
 * addr/len must lie inside it; otherwise the index is wrong or the program is
 * confused, and Linux answers -EFAULT. */
static int iou_fixed_range_ok(struct io_ring_ctx *ctx, u16 index, u64 addr,
                              u32 len) {
  struct iou_fixed_buf *b;

  if (!ctx->bufs || index >= ctx->nr_bufs)
    return 0;
  b = &ctx->bufs[index];
  if (!b->len)
    return 0;
  if (addr < b->addr)
    return 0;
  if (addr + (u64)len > b->addr + b->len)
    return 0;
  return 1;
}

/* ---- provided buffers --------------------------------------------------- */

static struct iou_bgroup *iou_bgroup_find(struct io_ring_ctx *ctx, u16 bgid) {
  for (struct iou_bgroup *g = ctx->bgroups; g; g = g->next)
    if (g->bgid == bgid)
      return g;
  return 0;
}

static struct iou_bgroup *iou_bgroup_get(struct io_ring_ctx *ctx, u16 bgid) {
  struct iou_bgroup *g = iou_bgroup_find(ctx, bgid);

  if (g)
    return g;
  g = kzalloc(sizeof(*g));
  if (!g)
    return 0;
  g->bgid = bgid;
  g->next = ctx->bgroups;
  ctx->bgroups = g;
  return g;
}

static void iou_bgroup_free_all(struct io_ring_ctx *ctx) {
  struct iou_bgroup *g = ctx->bgroups;

  ctx->bgroups = 0;
  while (g) {
    struct iou_bgroup *n = g->next;

    kfree(g->list);
    kfree(g);
    g = n;
  }
}

/* IORING_OP_PROVIDE_BUFFERS: sqe->addr is the first buffer, sqe->len the size
 * of each, sqe->fd how many, sqe->off the first buffer id, sqe->buf_group the
 * group. The buffers lie back to back in the caller's memory. */
static i32 iou_provide_buffers(struct io_ring_ctx *ctx,
                               const struct io_uring_sqe *sqe) {
  u32 nbufs = (u32)sqe->fd;
  u32 len = sqe->len;
  u64 addr = sqe->addr;
  u64 bid = sqe->off;
  struct iou_bgroup *g;

  if (sqe->fd <= 0 || nbufs > 0xffffu)
    return -EINVAL;
  if (bid > 0xffffull || bid + nbufs > 0x10000ull)
    return -EINVAL;
  if (len == 0)
    return -EINVAL;
  /* The range has to be one this process can reach; otherwise the -EFAULT
   * surfaces much later, inside whichever request happened to pick it. */
  {
    char probe;

    if (syscall_copyin(&probe, (const void *)(usize)addr, 1) < 0 ||
        syscall_copyin(&probe,
                       (const void *)(usize)(addr + (u64)nbufs * len - 1),
                       1) < 0)
      return -EFAULT;
  }

  iou_lock(ctx);
  g = iou_bgroup_get(ctx, sqe->buf_group);
  if (!g) {
    iou_unlock(ctx);
    return -ENOMEM;
  }
  if (g->is_ring) {
    iou_unlock(ctx);
    return -EINVAL; /* the group is a registered ring; the two forms exclude */
  }
  /* Compact what has already been consumed before growing. */
  if (g->head) {
    for (u32 i = g->head; i < g->nr; i++)
      g->list[i - g->head] = g->list[i];
    g->nr -= g->head;
    g->head = 0;
  }
  if (g->nr + nbufs > g->cap) {
    u32 cap = g->cap ? g->cap : 16;
    struct iou_pbuf *nl;

    while (cap < g->nr + nbufs)
      cap *= 2;
    nl = kzalloc(sizeof(*nl) * cap);
    if (!nl) {
      iou_unlock(ctx);
      return -ENOMEM;
    }
    for (u32 i = 0; i < g->nr; i++)
      nl[i] = g->list[i];
    kfree(g->list);
    g->list = nl;
    g->cap = cap;
  }
  for (u32 i = 0; i < nbufs; i++) {
    g->list[g->nr].addr = addr + (u64)i * len;
    g->list[g->nr].len = len;
    g->list[g->nr].bid = (u16)(bid + i);
    g->nr++;
  }
  iou_unlock(ctx);
  return 0;
}

/* IORING_OP_REMOVE_BUFFERS: drop up to sqe->fd buffers from the group; the
 * result is how many really went. */
static i32 iou_remove_buffers(struct io_ring_ctx *ctx,
                              const struct io_uring_sqe *sqe) {
  struct iou_bgroup *g;
  u32 want = (u32)sqe->fd;
  u32 got = 0;

  if (sqe->fd <= 0)
    return -EINVAL;
  iou_lock(ctx);
  g = iou_bgroup_find(ctx, sqe->buf_group);
  if (!g || g->is_ring) {
    iou_unlock(ctx);
    return -ENOENT;
  }
  while (got < want && g->head < g->nr) {
    g->head++;
    got++;
  }
  iou_unlock(ctx);
  return got ? (i32)got : -ENOENT;
}

/* Pick a buffer from a group. Returns 0 and fills *out, or -ENOBUFS. */
static int iou_buf_select(struct io_ring_ctx *ctx, u16 bgid,
                          struct iou_pbuf *out) {
  struct iou_bgroup *g;
  int rc = -ENOBUFS;

  iou_lock(ctx);
  g = iou_bgroup_find(ctx, bgid);
  if (!g) {
    iou_unlock(ctx);
    return -ENOBUFS;
  }
  if (g->is_ring) {
    u16 tail = 0;
    struct io_uring_buf ent;

    /* The tail is the program's; the head is ours. Both live where the ABI
     * says, and the entries are read out of the program's memory now rather
     * than cached, because the program may rewrite an entry it has not yet
     * published. */
    if (syscall_copyin(&tail,
                       (const void *)(usize)(g->ring_addr +
                                             IOU_PBUF_RING_TAIL_OFF),
                       sizeof(tail)) < 0) {
      iou_unlock(ctx);
      return -EFAULT;
    }
    if (g->ring_head == tail) {
      iou_unlock(ctx);
      return -ENOBUFS;
    }
    if (syscall_copyin(&ent,
                       (const void *)(usize)(g->ring_addr +
                                             (u64)(g->ring_head & g->ring_mask) *
                                                 sizeof(ent)),
                       sizeof(ent)) < 0) {
      iou_unlock(ctx);
      return -EFAULT;
    }
    g->ring_head++;
    out->addr = ent.addr;
    out->len = ent.len;
    out->bid = ent.bid;
    rc = 0;
  } else if (g->head < g->nr) {
    *out = g->list[g->head];
    g->head++;
    rc = 0;
  }
  iou_unlock(ctx);
  return rc;
}

/* Give a buffer back when the request that took it did nothing with it. */
static void iou_buf_recycle(struct io_ring_ctx *ctx, u16 bgid) {
  struct iou_bgroup *g;

  iou_lock(ctx);
  g = iou_bgroup_find(ctx, bgid);
  if (g) {
    if (g->is_ring)
      g->ring_head--;
    else if (g->head)
      g->head--;
  }
  iou_unlock(ctx);
}

/* ---- paths, descriptors and direct descriptors -------------------------- */

/* Resolve a dirfd-relative path the way the *at() opcodes define it: an
 * absolute path or AT_FDCWD is used as it stands, anything else is joined onto
 * the directory the descriptor names. Same rule as the *at() system calls in
 * kernel/syscall/syscall.c. */
static int iou_path_at(int dirfd, u64 user_path, char *out, usize outsz) {
  char kpath[VFS_MAX_PATH];
  char dirbuf[VFS_MAX_PATH];
  usize dlen;

  if (!user_path)
    return -EFAULT;
  if (syscall_copyinstr(kpath, sizeof(kpath), (const char *)(usize)user_path) <
      0)
    return -EFAULT;
  if (kpath[0] == '/' || dirfd == AT_FDCWD) {
    if (strlen(kpath) >= outsz)
      return -ENAMETOOLONG;
    strncpy(out, kpath, outsz - 1);
    out[outsz - 1] = 0;
    return 0;
  }
  if (vfs_fd_abspath(dirfd, dirbuf, sizeof(dirbuf)) < 0)
    return -EBADF;
  dlen = strlen(dirbuf);
  if (dlen && dirbuf[dlen - 1] == '/')
    dirbuf[--dlen] = 0;
  if (dlen + 1 + strlen(kpath) >= outsz)
    return -ENAMETOOLONG;
  snprintf(out, outsz, "%s/%s", dirbuf, kpath);
  return 0;
}

/* Several opcodes are exactly a system call that takes a descriptor number —
 * splice, fallocate, shutdown, epoll_ctl. io_uring resolved the file to a
 * handle (it may be a registered file with no descriptor at all), so the
 * handle is lent a descriptor for the length of the call rather than the
 * operation being written a second time against handles. */
static int iou_tmp_fd(struct vfs_handle *h) {
  int fd;

  if (!h)
    return -EBADF;
  vfs_handle_retain(h);
  fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h);
    return fd == -ENOMEM ? -ENOMEM : -EMFILE;
  }
  return fd;
}

static void iou_tmp_fd_put(int fd) {
  if (fd >= 0)
    scheduler_fd_close(fd);
}

/* Install a handle in the registered-file set. `file_index` is sqe->file_index:
 * a slot biased by one, or IORING_FILE_INDEX_ALLOC to take the first free slot
 * in the allocation range. Consumes the reference on success and returns the
 * slot for the ALLOC form, 0 otherwise. */
static i32 iou_install_direct(struct io_ring_ctx *ctx, struct vfs_handle *h,
                              u32 file_index) {
  i32 rc;

  iou_lock(ctx);
  if (!ctx->files || !ctx->nr_files) {
    iou_unlock(ctx);
    return -ENXIO;
  }
  if (file_index == IORING_FILE_INDEX_ALLOC) {
    u32 lo = ctx->falloc_len ? ctx->falloc_off : 0;
    u32 hi = ctx->falloc_len ? ctx->falloc_off + ctx->falloc_len
                             : ctx->nr_files;

    if (hi > ctx->nr_files)
      hi = ctx->nr_files;
    for (u32 i = lo; i < hi; i++)
      if (!ctx->files[i].file) {
        ctx->files[i].file = h;
        iou_unlock(ctx);
        return (i32)i;
      }
    iou_unlock(ctx);
    return -ENFILE;
  }
  if (file_index == 0 || file_index - 1 >= ctx->nr_files) {
    iou_unlock(ctx);
    return -EINVAL;
  }
  rc = 0;
  if (ctx->files[file_index - 1].file)
    vfs_handle_release(ctx->files[file_index - 1].file);
  ctx->files[file_index - 1].file = h;
  iou_unlock(ctx);
  return rc;
}

/* An opcode that produced an ordinary descriptor was asked for a direct one
 * instead: take the handle out of the descriptor and put it in a slot. */
static i32 iou_fd_to_direct(struct io_ring_ctx *ctx, int fd, u32 file_index) {
  struct vfs_handle *h = scheduler_fd_get_retain(fd);
  i32 rc;

  if (!h) {
    scheduler_fd_close(fd);
    return -EBADF;
  }
  rc = iou_install_direct(ctx, h, file_index);
  if (rc < 0)
    vfs_handle_release(h);
  scheduler_fd_close(fd);
  return rc;
}

/* ---- timeouts ----------------------------------------------------------- */

static int iou_timespec_in(u64 uaddr, u64 *out_ns) {
  struct __kernel_timespec ts;

  if (syscall_copyin(&ts, (const void *)(usize)uaddr, sizeof(ts)) < 0)
    return -EFAULT;
  if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000ll || ts.tv_sec < 0)
    return -EINVAL;
  *out_ns = (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
  return 0;
}

/* ---- issuing ------------------------------------------------------------ */

static void iou_complete(struct iou_req *req, i32 res, u32 cflags);
static int iou_is_ring_handle(const struct vfs_handle *h);
static int iou_cancel_by(struct io_ring_ctx *ctx, u64 user_data, int by_fd,
                         int fd, u32 cancel_flags);
static isize iou_files_update(struct io_ring_ctx *ctx, u32 off, u64 uaddr,
                              u32 nr);

/* IORING_TIMEOUT_UPDATE: move an armed timeout's expiry rather than cancelling
 * and re-arming it. `ltimeout` picks a link timeout instead of a plain one. */
static i32 iou_timeout_update(struct io_ring_ctx *ctx, u64 user_data, u64 ns,
                              int absolute, int ltimeout) {
  i32 rc = -ENOENT;

  iou_lock(ctx);
  for (struct iou_req *r = ctx->live; r; r = r->next) {
    if (r->state != IOU_ST_ARMED || r->sqe.user_data != user_data)
      continue;
    if (ltimeout ? !r->is_linktmo : !r->is_timeout)
      continue;
    r->has_deadline = 1;
    r->deadline_ns = absolute ? ns : ktime_monotonic_ns() + ns;
    rc = 0;
    break;
  }
  iou_unlock(ctx);
  return rc;
}

/* Perform the request now. The caller has already decided the file is ready.
 * Returns the CQE result. */
static i32 iou_perform_op(struct iou_req *req) {
  struct io_ring_ctx *ctx = req->ctx;
  const struct io_uring_sqe *sqe = &req->sqe;
  struct vfs_handle *h = req->file;
  isize r;

  switch (sqe->opcode) {
  case IORING_OP_NOP:
    if (sqe->nop_flags & IORING_NOP_INJECT_RESULT)
      return (i32)sqe->len;
    return 0;

  case IORING_OP_READ:
  case IORING_OP_READ_MULTISHOT:
  case IORING_OP_READ_FIXED: {
    u64 off = sqe->off;
    int use_off;

    if (!h)
      return -EBADF;
    if (sqe->opcode == IORING_OP_READ_FIXED &&
        !iou_fixed_range_ok(ctx, sqe->buf_index, sqe->addr, sqe->len))
      return -EFAULT;
    /* IORING_FEAT_RW_CUR_POS: -1 means the descriptor's own position. A
     * descriptor with no position at all (a socket, a pipe) is the same case. */
    use_off = iou_use_offset(h, sqe);
    r = iou_read_user(h, sqe->addr, sqe->len, use_off ? &off : 0);
    return (i32)r;
  }

  case IORING_OP_WRITE:
  case IORING_OP_WRITE_FIXED: {
    u64 off = sqe->off;
    int use_off;

    if (!h)
      return -EBADF;
    if (sqe->opcode == IORING_OP_WRITE_FIXED &&
        !iou_fixed_range_ok(ctx, sqe->buf_index, sqe->addr, sqe->len))
      return -EFAULT;
    use_off = iou_use_offset(h, sqe);
    r = iou_write_user(h, sqe->addr, sqe->len, use_off ? &off : 0);
    return (i32)r;
  }

  case IORING_OP_READV:
  case IORING_OP_WRITEV: {
    u64 off = sqe->off;
    int use_off;

    if (!h)
      return -EBADF;
    use_off = iou_use_offset(h, sqe);
    r = iou_rw_vectored(h, req->iov, req->iovcnt, use_off ? &off : 0,
                        sqe->opcode == IORING_OP_WRITEV);
    return (i32)r;
  }

  case IORING_OP_FSYNC:
  case IORING_OP_SYNC_FILE_RANGE:
    if (!h)
      return -EBADF;
    /* b1nix has no metadata-only writeback and no ranged writeback, so
     * IORING_FSYNC_DATASYNC, a range and a full fsync are all the same
     * operation — exactly as fdatasync(2) is aliased to fsync in
     * kernel/syscall/linux_abi.c. Flushing more than was asked for is what
     * sync_file_range(2) permits; flushing less is not. */
    return (i32)vfs_fsync_h(h);

  case IORING_OP_ACCEPT: {
    usize alen = 0;
    usize *alenp = 0;
    char addrbuf[128];
    int fd;

    if (!h)
      return -EBADF;
    if (sqe->addr2) {
      u32 ulen = 0;

      if (syscall_copyin(&ulen, (const void *)(usize)sqe->addr2,
                         sizeof(ulen)) < 0)
        return -EFAULT;
      alen = ulen > sizeof(addrbuf) ? sizeof(addrbuf) : ulen;
      alenp = &alen;
    }
    memset(addrbuf, 0, sizeof(addrbuf));
    fd = vfs_accept_h(h, sqe->addr ? addrbuf : 0, sqe->addr ? alenp : 0);
    if (fd < 0)
      return (i32)fd;
    if (sqe->addr && sqe->addr2) {
      u32 reported = (u32)alen;
      usize copy = alen > sizeof(addrbuf) ? sizeof(addrbuf) : alen;

      if (copy &&
          syscall_copyout((void *)(usize)sqe->addr, addrbuf, copy) < 0) {
        scheduler_fd_close(fd);
        return -EFAULT;
      }
      if (syscall_copyout((void *)(usize)sqe->addr2, &reported,
                          sizeof(reported)) < 0) {
        scheduler_fd_close(fd);
        return -EFAULT;
      }
    }
    /* accept4(2)'s flags, which is what sqe->accept_flags carries. */
    if (sqe->accept_flags & IOU_SOCK_CLOEXEC)
      scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
    if (sqe->accept_flags & IOU_SOCK_NONBLOCK) {
      struct vfs_handle *nh = scheduler_fd_get(fd);

      if (nh)
        nh->flags |= B1NIX_O_NONBLOCK;
    }
    /* A direct accept puts the connection straight into the registered set;
     * cqe->res is then the slot, not a descriptor. */
    if (req->file_index)
      return iou_fd_to_direct(ctx, fd, req->file_index);
    return fd;
  }

  case IORING_OP_CONNECT: {
    char addrbuf[128];
    usize alen = (usize)sqe->off; /* connect puts addrlen in off */

    if (!h)
      return -EBADF;
    if (!sqe->addr || alen == 0 || alen > sizeof(addrbuf))
      return -EINVAL;
    if (syscall_copyin(addrbuf, (const void *)(usize)sqe->addr, alen) < 0)
      return -EFAULT;
    return (i32)vfs_connect_h(h, addrbuf, alen);
  }

  case IORING_OP_SEND: {
    char *bounce;
    u32 len = sqe->len;

    if (!h)
      return -EBADF;
    if (len > IOU_BOUNCE)
      len = IOU_BOUNCE;
    bounce = kmalloc(len ? len : 1);
    if (!bounce)
      return -ENOMEM;
    if (len && syscall_copyin(bounce, (const void *)(usize)sqe->addr, len) < 0) {
      kfree(bounce);
      return -EFAULT;
    }
    r = vfs_socket_send_h(h, bounce, len, (int)sqe->msg_flags);
    kfree(bounce);
    return (i32)r;
  }

  case IORING_OP_RECV: {
    char *bounce;
    u32 len = sqe->len;

    if (!h)
      return -EBADF;
    if (len > IOU_BOUNCE)
      len = IOU_BOUNCE;
    bounce = kmalloc(len ? len : 1);
    if (!bounce)
      return -ENOMEM;
    r = vfs_socket_recv_h(h, bounce, len, (int)sqe->msg_flags);
    if (r > 0 &&
        syscall_copyout((void *)(usize)sqe->addr, bounce, (usize)r) < 0)
      r = -EFAULT;
    kfree(bounce);
    return (i32)r;
  }

  case IORING_OP_CLOSE: {
    struct vfs_handle *victim;

    if (sqe->flags & IOSQE_FIXED_FILE)
      return -EINVAL; /* Linux uses file_index for that; not supported here */
    if (sqe->fd < 0)
      return -EBADF;
    /* Not the ring this request is running on. Closing it here drops its last
     * reference in the middle of its own submission, and the teardown then
     * frees the request that is executing. Linux answers EBADF for the same
     * reason. */
    victim = scheduler_fd_get(sqe->fd);
    if (victim && victim->private_data == ctx)
      return -EBADF;
    return (i32)scheduler_fd_close(sqe->fd);
  }

  case IORING_OP_ASYNC_CANCEL: {
    int by_fd = (sqe->cancel_flags & IORING_ASYNC_CANCEL_FD) ? 1 : 0;
    int n = iou_cancel_by(ctx, sqe->addr, by_fd, (int)sqe->fd,
                          sqe->cancel_flags);

    if (!n)
      return -ENOENT;
    /* Linux reports 0 for the ordinary "one request cancelled" case and the
     * count only when the caller asked for all of them. */
    return (sqe->cancel_flags & (IORING_ASYNC_CANCEL_ALL |
                                 IORING_ASYNC_CANCEL_ANY))
               ? n
               : 0;
  }

  case IORING_OP_POLL_REMOVE:
    return iou_cancel_by(ctx, sqe->addr, 0, -1, 0) ? 0 : -ENOENT;

  case IORING_OP_TIMEOUT_REMOVE: {
    u32 fl = sqe->timeout_flags;

    if (sqe->buf_index || sqe->len || sqe->splice_fd_in)
      return -EINVAL;
    if (fl & IORING_TIMEOUT_UPDATE_MASK) {
      u64 ns = 0;
      int rc;

      /* The clock flags choose a clock this kernel does not keep timeouts on,
       * so they are refused rather than silently read as MONOTONIC. */
      if (fl & ~(u32)(IORING_TIMEOUT_UPDATE_MASK | IORING_TIMEOUT_ABS))
        return -EINVAL;
      rc = iou_timespec_in(sqe->addr2, &ns);
      if (rc < 0)
        return rc;
      return iou_timeout_update(ctx, sqe->addr, ns,
                                (fl & IORING_TIMEOUT_ABS) != 0,
                                (fl & IORING_LINK_TIMEOUT_UPDATE) != 0);
    }
    /* Plain removal takes no flags at all. */
    if (fl)
      return -EINVAL;
    return iou_cancel_by(ctx, sqe->addr, 0, -1, 0) ? 0 : -ENOENT;
  }

  case IORING_OP_FILES_UPDATE:
    /* The registered-file set changed from inside the ring rather than through
     * io_uring_register: offset in off, the descriptors at addr, how many in
     * len. The result is how many slots were written. */
    return (i32)iou_files_update(ctx, (u32)sqe->off, sqe->addr, sqe->len);

  case IORING_OP_SENDMSG:
  case IORING_OP_SENDMSG_ZC:
  case IORING_OP_RECVMSG: {
    int tfd, is_send = sqe->opcode != IORING_OP_RECVMSG;

    if (!h)
      return -EBADF;
    tfd = iou_tmp_fd(h);
    if (tfd < 0)
      return (i32)tfd;
    if (is_send)
      r = (isize)(i64)syscall_sendmsg_user(
          tfd, (const struct syscall_msghdr *)(usize)sqe->addr,
          (int)sqe->msg_flags);
    else
      r = (isize)(i64)syscall_recvmsg_user(
          tfd, (struct syscall_msghdr *)(usize)sqe->addr, (int)sqe->msg_flags);
    iou_tmp_fd_put(tfd);
    return (i32)r;
  }

  case IORING_OP_SEND_ZC: {
    /* There is no page-pinned zero-copy path here: the transfer is the same
     * copy IORING_OP_SEND makes. What the ABI requires of SEND_ZC beyond that
     * is the second, notification completion, and it is posted for real — see
     * iou_complete, which posts it once the data has left. A program that
     * waits for IORING_CQE_F_NOTIF therefore gets the answer it is owed. */
    char *bounce;
    u32 len = sqe->len;

    if (!h)
      return -EBADF;
    if (len > IOU_BOUNCE)
      len = IOU_BOUNCE;
    bounce = kmalloc(len ? len : 1);
    if (!bounce)
      return -ENOMEM;
    if (len && syscall_copyin(bounce, (const void *)(usize)sqe->addr, len) < 0) {
      kfree(bounce);
      return -EFAULT;
    }
    r = vfs_socket_send_h(h, bounce, len, (int)sqe->msg_flags);
    kfree(bounce);
    return (i32)r;
  }

  case IORING_OP_SHUTDOWN: {
    int tfd;

    if (!h)
      return -EBADF;
    tfd = iou_tmp_fd(h);
    if (tfd < 0)
      return (i32)tfd;
    r = vfs_shutdown(tfd, (int)sqe->len);
    iou_tmp_fd_put(tfd);
    return (i32)r;
  }

  case IORING_OP_SOCKET: {
    int fd;

    if (sqe->addr || sqe->len > 0xffff || sqe->rw_flags)
      return -EINVAL;
    fd = vfs_socket((int)sqe->fd, (int)sqe->off, (int)sqe->len);
    if (fd < 0)
      return (i32)fd;
    if (req->file_index)
      return iou_fd_to_direct(ctx, fd, req->file_index);
    return fd;
  }

  case IORING_OP_BIND: {
    char addrbuf[128];
    usize alen = (usize)sqe->addr2;
    int tfd;

    if (!h)
      return -EBADF;
    if (!sqe->addr || alen == 0 || alen > sizeof(addrbuf))
      return -EINVAL;
    if (syscall_copyin(addrbuf, (const void *)(usize)sqe->addr, alen) < 0)
      return -EFAULT;
    tfd = iou_tmp_fd(h);
    if (tfd < 0)
      return (i32)tfd;
    r = vfs_bind(tfd, addrbuf, alen);
    iou_tmp_fd_put(tfd);
    return (i32)r;
  }

  case IORING_OP_LISTEN: {
    int tfd;

    if (!h)
      return -EBADF;
    tfd = iou_tmp_fd(h);
    if (tfd < 0)
      return (i32)tfd;
    r = vfs_listen(tfd, (int)sqe->len);
    iou_tmp_fd_put(tfd);
    return (i32)r;
  }

  case IORING_OP_OPENAT:
  case IORING_OP_OPENAT2: {
    char path[VFS_MAX_PATH];
    int flags, fd;
    u16 mode;

    if (sqe->opcode == IORING_OP_OPENAT2) {
      /* struct open_how {u64 flags; u64 mode; u64 resolve;}. openat2(2) is
       * already implemented, RESOLVE_* and all, so the opcode is that call
       * rather than a second reading of the same structure. */
      u64 ret = 0;

      if (sqe->len != 24)
        return -EINVAL;
      if (!linux_modern_syscall(437, (u64)(i64)(i32)sqe->fd, sqe->addr,
                                sqe->off, sqe->len, 0, 0, &ret))
        return -EINVAL;
      fd = (int)(i64)ret;
    } else {
      int rc = iou_path_at((int)sqe->fd, sqe->addr, path, sizeof(path));

      if (rc < 0)
        return (i32)rc;
      flags = linux_modern_open_flags((int)sqe->open_flags);
      mode = (u16)(sqe->len & 07777);
      fd = vfs_open_flags_mode(path, flags, mode);
    }
    if (fd < 0)
      return (i32)fd;
    if (req->file_index)
      return iou_fd_to_direct(ctx, fd, req->file_index);
    return fd;
  }

  case IORING_OP_STATX: {
    char kpath[VFS_MAX_PATH];
    int rc;

    /* AT_EMPTY_PATH names the descriptor itself, which for a registered file
     * has no descriptor number — lend it one. */
    if (sqe->statx_flags & AT_EMPTY_PATH) {
      /* The descriptor itself. A registered file has no descriptor number, so
       * lend it one for the call. */
      struct vfs_handle *sh = h;
      int tfd;

      if (!sh) {
        int err = 0;

        sh = iou_get_file(ctx, sqe, &err);
        if (!sh)
          return (i32)err;
      } else {
        vfs_handle_retain(sh);
      }
      tfd = iou_tmp_fd(sh);
      vfs_handle_release(sh);
      if (tfd < 0)
        return (i32)tfd;
      rc = syscall_statx_kpath(tfd, "", (int)sqe->statx_flags, sqe->len,
                               (struct statx *)(usize)sqe->off);
      iou_tmp_fd_put(tfd);
      return (i32)rc;
    }
    rc = iou_path_at((int)sqe->fd, sqe->addr, kpath, sizeof(kpath));
    if (rc < 0)
      return (i32)rc;
    return (i32)syscall_statx_kpath(AT_FDCWD, kpath, (int)sqe->statx_flags,
                                    sqe->len,
                                    (struct statx *)(usize)sqe->off);
  }

  case IORING_OP_FALLOCATE: {
    int tfd;

    if (!h)
      return -EBADF;
    tfd = iou_tmp_fd(h);
    if (tfd < 0)
      return (i32)tfd;
    r = syscall_fallocate(tfd, (int)sqe->len, sqe->off, sqe->addr);
    iou_tmp_fd_put(tfd);
    return (i32)r;
  }

  case IORING_OP_FTRUNCATE: {
    int tfd;

    if (!h)
      return -EBADF;
    tfd = iou_tmp_fd(h);
    if (tfd < 0)
      return (i32)tfd;
    r = vfs_ftruncate(tfd, sqe->off);
    iou_tmp_fd_put(tfd);
    return (i32)r;
  }

  case IORING_OP_FADVISE:
    /* posix_fadvise(2) is advice, and this kernel's readahead is driven by the
     * access pattern it observes rather than by a hint (M14). Accepting the
     * advice and changing nothing is what the call promises; what must NOT be
     * accepted is nonsense, so the advice value is checked. */
    if (!h)
      return -EBADF;
    switch (sqe->fadvise_advice) {
    case 0: /* NORMAL */
    case 1: /* RANDOM */
    case 2: /* SEQUENTIAL */
    case 3: /* WILLNEED */
    case 4: /* DONTNEED */
    case 5: /* NOREUSE */
      return 0;
    default:
      return -EINVAL;
    }

  case IORING_OP_MADVISE:
    /* madvise(2) against this process's own memory, by the same implementation
     * the system call uses. It is called directly and not through
     * syscall_dispatch, because a task with the Linux personality has its
     * system call numbers translated on the way in and an io_uring opcode is
     * not a system call number. */
    return (i32)syscall_madvise((void *)(usize)sqe->addr, (usize)sqe->off,
                                (int)sqe->fadvise_advice);

  case IORING_OP_RENAMEAT: {
    char oldp[VFS_MAX_PATH], newp[VFS_MAX_PATH];
    int rc;

    if (sqe->rename_flags)
      return -EINVAL; /* RENAME_NOREPLACE/EXCHANGE are not implemented */
    rc = iou_path_at((int)sqe->fd, sqe->addr, oldp, sizeof(oldp));
    if (rc < 0)
      return (i32)rc;
    rc = iou_path_at((int)sqe->len, sqe->addr2, newp, sizeof(newp));
    if (rc < 0)
      return (i32)rc;
    return (i32)vfs_rename(oldp, newp);
  }

  case IORING_OP_UNLINKAT: {
    char path[VFS_MAX_PATH];
    int rc = iou_path_at((int)sqe->fd, sqe->addr, path, sizeof(path));

    if (rc < 0)
      return (i32)rc;
    if (sqe->unlink_flags & ~(u32)IOU_AT_REMOVEDIR)
      return -EINVAL;
    return (i32)((sqe->unlink_flags & IOU_AT_REMOVEDIR) ? vfs_rmdir(path)
                                                    : vfs_unlink(path));
  }

  case IORING_OP_MKDIRAT: {
    char path[VFS_MAX_PATH];
    int rc = iou_path_at((int)sqe->fd, sqe->addr, path, sizeof(path));

    if (rc < 0)
      return (i32)rc;
    return (i32)vfs_mkdir(path, sqe->len & 07777);
  }

  case IORING_OP_SYMLINKAT: {
    char target[VFS_MAX_PATH], link[VFS_MAX_PATH];
    int rc;

    /* The target of a symlink is a string, not a path to resolve. */
    if (syscall_copyinstr(target, sizeof(target),
                          (const char *)(usize)sqe->addr) < 0)
      return -EFAULT;
    rc = iou_path_at((int)sqe->fd, sqe->addr2, link, sizeof(link));
    if (rc < 0)
      return (i32)rc;
    return (i32)vfs_symlink(target, link);
  }

  case IORING_OP_LINKAT: {
    char oldp[VFS_MAX_PATH], newp[VFS_MAX_PATH];
    int rc;

    if (sqe->hardlink_flags & ~(u32)IOU_AT_SYMLINK_FOLLOW)
      return -EINVAL;
    rc = iou_path_at((int)sqe->fd, sqe->addr, oldp, sizeof(oldp));
    if (rc < 0)
      return (i32)rc;
    rc = iou_path_at((int)sqe->len, sqe->addr2, newp, sizeof(newp));
    if (rc < 0)
      return (i32)rc;
    return (i32)vfs_link(oldp, newp);
  }

  case IORING_OP_SPLICE:
  case IORING_OP_TEE: {
    struct vfs_handle *hin;
    int tfd_out, tfd_in;
    int err = 0;

    if (!h)
      return -EBADF;
    if (sqe->opcode == IORING_OP_TEE && (sqe->splice_off_in || sqe->off))
      return -EINVAL;
    if (sqe->splice_flags & ~(u32)(SPLICE_F_FD_IN_FIXED | 0xfu))
      return -EINVAL;
    if (sqe->splice_flags & SPLICE_F_FD_IN_FIXED) {
      u32 idx = (u32)sqe->splice_fd_in;

      iou_lock(ctx);
      hin = (ctx->files && idx < ctx->nr_files) ? ctx->files[idx].file : 0;
      if (hin)
        vfs_handle_retain(hin);
      iou_unlock(ctx);
      if (!hin)
        return -EBADF;
    } else {
      hin = scheduler_fd_get_retain(sqe->splice_fd_in);
      if (!hin)
        return -EBADF;
    }
    tfd_in = iou_tmp_fd(hin);
    vfs_handle_release(hin);
    if (tfd_in < 0)
      return (i32)tfd_in;
    tfd_out = iou_tmp_fd(h);
    if (tfd_out < 0) {
      iou_tmp_fd_put(tfd_in);
      return (i32)tfd_out;
    }
    /* splice(2)'s offsets are user pointers; io_uring carries the values, so
     * they are staged in kernel memory and handed over as pointers to it.
     * -1 means "the descriptor's own position", as everywhere else here. */
    {
      u64 off_in = sqe->splice_off_in, off_out = sqe->off;
      u64 *pin = (sqe->opcode == IORING_OP_TEE || off_in == (u64)-1) ? 0
                                                                     : &off_in;
      u64 *pout = (sqe->opcode == IORING_OP_TEE || off_out == (u64)-1)
                      ? 0
                      : &off_out;

      r = file_copy_range(tfd_in, pin, tfd_out, pout, sqe->len);
    }
    iou_tmp_fd_put(tfd_out);
    iou_tmp_fd_put(tfd_in);
    (void)err;
    return (i32)r;
  }

  case IORING_OP_EPOLL_CTL: {
    struct b1nix_epoll_event ev;
    int tfd, op = (int)sqe->len;

    if (!h)
      return -EBADF;
    memset(&ev, 0, sizeof(ev));
    /* EPOLL_CTL_DEL (2) reads no event structure. */
    if (op != 2 && sqe->addr &&
        syscall_copyin(&ev, (const void *)(usize)sqe->addr, sizeof(ev)) < 0)
      return -EFAULT;
    tfd = iou_tmp_fd(h);
    if (tfd < 0)
      return (i32)tfd;
    r = vfs_epoll_ctl(tfd, op, (int)sqe->off, op == 2 ? 0 : &ev);
    iou_tmp_fd_put(tfd);
    return (i32)r;
  }

  case IORING_OP_MSG_RING: {
    /* Post a completion into ANOTHER ring. The target is sqe->fd (a ring
     * descriptor, possibly a registered one), the data is sqe->len as the
     * result and sqe->off as the user_data. */
    struct io_ring_ctx *target;
    u32 tflags = 0;

    if (!h || !iou_is_ring_handle(h) || !h->private_data)
      return -EBADFD;
    if (sqe->msg_ring_flags & ~(u32)(IORING_MSG_RING_CQE_SKIP |
                                     IORING_MSG_RING_FLAGS_PASS))
      return -EINVAL;
    target = (struct io_ring_ctx *)h->private_data;
    if (sqe->addr == IORING_MSG_SEND_FD) {
      /* Move a registered file from this ring's set into the target's. */
      struct vfs_handle *src;
      u32 sidx = (u32)sqe->addr3;
      i32 rc;

      iou_lock(ctx);
      src = (ctx->files && sidx < ctx->nr_files) ? ctx->files[sidx].file : 0;
      if (src)
        vfs_handle_retain(src);
      iou_unlock(ctx);
      if (!src)
        return -EBADF;
      rc = iou_install_direct(target, src, (u32)sqe->file_index);
      if (rc < 0) {
        vfs_handle_release(src);
        return rc;
      }
      if (!(sqe->msg_ring_flags & IORING_MSG_RING_CQE_SKIP))
        iou_post_cqe_n(target, sqe->off, 0, 0, 1);
      return 0;
    }
    if (sqe->addr != IORING_MSG_DATA)
      return -EINVAL;
    if (sqe->msg_ring_flags & IORING_MSG_RING_FLAGS_PASS)
      tflags = sqe->file_index;
    iou_post_cqe_n(target, sqe->off, (i32)sqe->len, tflags, 1);
    return 0;
  }

  case IORING_OP_WAITID:
    /* io_uring is a Linux interface, so the siginfo it writes is Linux's —
     * the same rewrite the waitid(2) system call does, from the same place. */
    return (i32)syscall_waitid_linux(sqe->len, (u64)(u32)sqe->fd, sqe->addr2,
                                     (int)sqe->file_index);

  case IORING_OP_FIXED_FD_INSTALL: {
    /* Turn a registered file back into an ordinary descriptor. */
    int fd;

    if (!(sqe->flags & IOSQE_FIXED_FILE))
      return -EINVAL;
    if (sqe->install_fd_flags & ~(u32)IORING_FIXED_FD_NO_CLOEXEC)
      return -EINVAL;
    if (!h)
      return -EBADF;
    fd = iou_tmp_fd(h); /* a descriptor over the same handle; kept, not put */
    if (fd < 0)
      return (i32)fd;
    if (!(sqe->install_fd_flags & IORING_FIXED_FD_NO_CLOEXEC))
      scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
    return fd;
  }

  case IORING_OP_PIPE: {
    int fds[2];
    i32 rc;

    /* O_CLOEXEC and O_NONBLOCK are the only pipe2(2) flags there are. */
    if (sqe->pipe_flags & ~(u32)(B1NIX_O_CLOEXEC | B1NIX_O_NONBLOCK))
      return -EINVAL;
    rc = (i32)vfs_pipe(fds);
    if (rc < 0)
      return rc;
    for (int i = 0; i < 2; i++) {
      if (sqe->pipe_flags & B1NIX_O_CLOEXEC)
        scheduler_fd_flags_set(fds[i], B1NIX_FD_CLOEXEC);
      if (sqe->pipe_flags & B1NIX_O_NONBLOCK) {
        struct vfs_handle *ph = scheduler_fd_get(fds[i]);

        if (ph)
          ph->flags |= B1NIX_O_NONBLOCK;
      }
    }
    if (req->file_index) {
      /* Both ends as direct descriptors; file_index is the first slot. */
      i32 a = iou_fd_to_direct(ctx, fds[0], req->file_index);
      i32 b;

      if (a < 0) {
        scheduler_fd_close(fds[1]);
        return a;
      }
      b = iou_fd_to_direct(ctx, fds[1],
                           req->file_index == IORING_FILE_INDEX_ALLOC
                               ? IORING_FILE_INDEX_ALLOC
                               : req->file_index + 1);
      if (b < 0)
        return b;
      if (sqe->addr) {
        i32 slots[2] = {a, b};

        if (syscall_copyout((void *)(usize)sqe->addr, slots, sizeof(slots)) < 0)
          return -EFAULT;
      }
      return 0;
    }
    if (sqe->addr &&
        syscall_copyout((void *)(usize)sqe->addr, fds, sizeof(fds)) < 0) {
      scheduler_fd_close(fds[0]);
      scheduler_fd_close(fds[1]);
      return -EFAULT;
    }
    return 0;
  }

  case IORING_OP_PROVIDE_BUFFERS:
    return iou_provide_buffers(ctx, sqe);

  case IORING_OP_REMOVE_BUFFERS:
    return iou_remove_buffers(ctx, sqe);

  default:
    /* An opcode this kernel does not implement. IORING_REGISTER_PROBE says so
     * in advance; this is what a caller that asked anyway gets. */
    iou_trace("unsupported-op", sqe->opcode, sqe->user_data, -EINVAL);
    return -EINVAL;
  }
}

/* Whether this opcode reads into a buffer the ring may choose for it. */
static int iou_op_takes_buffer(u8 opcode) {
  switch (opcode) {
  case IORING_OP_READ:
  case IORING_OP_READV:
  case IORING_OP_RECV:
  case IORING_OP_RECVMSG:
  case IORING_OP_READ_MULTISHOT:
    return 1;
  default:
    return 0;
  }
}

/* Perform the request, choosing a provided buffer for it first if that is what
 * IOSQE_BUFFER_SELECT asked for. The buffer that was used is reported in the
 * completion's flags; one that turned out not to be needed goes back. */
static i32 iou_perform(struct iou_req *req, u32 *cflags) {
  struct iou_pbuf b;
  int selected = 0;
  i32 res;

  *cflags = 0;
  if (req->buf_select && iou_op_takes_buffer(req->sqe.opcode)) {
    int rc = iou_buf_select(req->ctx, req->buf_group, &b);

    if (rc < 0)
      return (i32)rc;
    selected = 1;
    req->sqe.addr = b.addr;
    req->sqe.len = (!req->sel_len || req->sel_len > b.len) ? b.len
                                                           : req->sel_len;
  }
  res = iou_perform_op(req);
  if (selected) {
    if (res <= 0)
      iou_buf_recycle(req->ctx, req->buf_group);
    else
      *cflags |= IORING_CQE_F_BUFFER |
                 ((u32)b.bid << IORING_CQE_BUFFER_SHIFT);
  }
  return res;
}

/* Cancel a chain from `req` onwards, posting -ECANCELED for each. */
static void iou_cancel_chain(struct iou_req *req) {
  while (req) {
    struct iou_req *next = req->link_next;

    iou_post_req_cqe(req, -ECANCELED, 0);
    if (req->tmo_armed) {
      struct iou_req *t = req->tmo_armed;

      req->tmo_armed = 0;
      t->tmo_target = 0;
      iou_lock(t->ctx);
      iou_req_unlink(t->ctx, t);
      iou_unlock(t->ctx);
      iou_post_req_cqe(t, -ECANCELED, 0);
      iou_req_free(t);
    }
    iou_lock(req->ctx);
    iou_req_unlink(req->ctx, req);
    iou_unlock(req->ctx);
    iou_req_free(req);
    req = next;
  }
}

static int iou_issue(struct iou_req *req);
static void iou_issue_chain(struct iou_req *head);

/* Finish a request: post its CQE, release the link timeout watching it, and
 * start the next link of the chain. */
static void iou_complete(struct iou_req *req, i32 res, u32 cflags) {
  struct iou_req *next = req->link_next;
  struct io_ring_ctx *ctx = req->ctx;
  int skip = req->cqe_skip && res >= 0;
  /* Whether the chain behind this request carries on. A timeout armed with
   * IORING_TIMEOUT_ETIME_SUCCESS reports -ETIME and is still a success as far
   * as the link is concerned — that is the only thing the flag does. */
  int link_ok = res >= 0 || (req->timeout_etime_success && res == -ETIME);
  /* IOSQE_IO_HARDLINK is a property of the link this request makes to the next
   * one, so it is read from THIS request, not from the one after it. Reading
   * the successor's flag made a hard link behave like a soft one whenever the
   * last SQE of the chain did not repeat the flag. */
  int hard = req->hardlink;
  /* A hard link survives a request that RAN and returned an error. It does not
   * survive one that could never run: a prep failure severs the chain whatever
   * the flag says, which is what liburing's submit-link-fail requires. */
  int prep_failed = req->failed;

  if (req->tmo_armed) {
    struct iou_req *t = req->tmo_armed;

    req->tmo_armed = 0;
    t->tmo_target = 0;
    iou_lock(ctx);
    iou_req_unlink(ctx, t);
    iou_unlock(ctx);
    /* Linux: a link timeout whose target finished first completes -ECANCELED. */
    iou_post_req_cqe(t, -ECANCELED, 0);
    iou_req_free(t);
  }

  /* IORING_OP_SEND_ZC promises two completions: the transfer, marked
   * IORING_CQE_F_MORE because another is coming, and the notification that
   * says the caller's buffer is free again. The buffer here is free as soon as
   * the copy is done, so the notification follows immediately — but it IS
   * posted, because a program that waits for IORING_CQE_F_NOTIF would
   * otherwise wait for ever. */
  int zc = (req->sqe.opcode == IORING_OP_SEND_ZC ||
            req->sqe.opcode == IORING_OP_SENDMSG_ZC) &&
           !req->failed;

  if (!skip)
    iou_post_req_cqe(req, res, cflags | (zc ? IORING_CQE_F_MORE : 0));
  if (zc)
    iou_post_cqe_n(ctx, req->sqe.user_data, 0, IORING_CQE_F_NOTIF, 1);

  iou_lock(ctx);
  iou_req_unlink(ctx, req);
  iou_unlock(ctx);
  req->link_next = 0;
  iou_req_free(req);

  if (!next)
    return;
  if (!link_ok && (prep_failed || !hard)) {
    iou_cancel_chain(next);
    return;
  }
  iou_issue(next);
}

/* One step of a request: perform it and post what it produced.
 *
 * Returns 1 when the request stays on the ring because more completions are
 * owed — that is what multishot means, and IORING_CQE_F_MORE is how each
 * completion but the last says so. A multishot request ends on the first
 * error, and a receive also ends on end-of-file, which is what Linux reports
 * and what a program uses to know the connection went away. */
static int iou_run_once(struct iou_req *req) {
  u32 cflags = 0;
  i32 res = iou_perform(req, &cflags);
  int last = res < 0;

  if (!last && res == 0)
    switch (req->sqe.opcode) {
    case IORING_OP_RECV:
    case IORING_OP_RECVMSG:
    case IORING_OP_READ_MULTISHOT:
      last = 1; /* end of file: nothing more will come */
      break;
    default:
      break;
    }
  if (req->multishot && !last) {
    iou_post_req_cqe(req, res, cflags | IORING_CQE_F_MORE);
    req->state = IOU_ST_ARMED;
    return 1;
  }
  iou_complete(req, res, cflags);
  return 0;
}

/* Start a completed link chain.
 *
 * A chain is submitted as a unit and it fails as one: if ANY member could not
 * be prepared, none of them runs. The refused member reports the error it was
 * refused with and every other member reports ECANCELED — which is exactly
 * what liburing's submit-link-fail asserts, from either end of the chain, and
 * what Linux's io_submit_fail_init arranges by failing the head as soon as a
 * member fails to prep. Issuing the head first and letting the failure
 * propagate cannot express it: a member that fails LATER in the chain has to
 * take the members BEFORE it down too. */
static void iou_issue_chain(struct iou_req *head) {
  struct iou_req *r;
  int poisoned = 0;

  for (r = head; r; r = r->link_next)
    if (r->failed) {
      poisoned = 1;
      break;
    }
  if (!poisoned) {
    iou_issue(head);
    return;
  }

  r = head;
  while (r) {
    struct iou_req *next = r->link_next;

    r->link_next = 0;
    if (r->tmo_armed) {
      r->tmo_armed->tmo_target = 0;
      r->tmo_armed = 0;
    }
    r->tmo_target = 0;
    iou_lock(r->ctx);
    iou_req_unlink(r->ctx, r);
    iou_unlock(r->ctx);
    iou_post_req_cqe(r, r->failed ? r->fail_res : -ECANCELED, 0);
    iou_req_free(r);
    r = next;
  }
}

/* Arm the link timeout that follows `req`, if the next link is one.
 *
 * Returns 1 when that timeout could not be prepared, in which case `req` is to
 * be cancelled: a link timeout is an instruction about the request in front of
 * it, and one the kernel refused cannot be silently dropped. Arming it anyway
 * is worse than either — it leaves a timeout with no deadline that never fires
 * and a request nothing ever completes, which is how liburing's link-timeout
 * test stopped answering rather than failing. */
static int iou_arm_link_timeout(struct iou_req *req) {
  struct iou_req *t = req->link_next;

  if (!t || !t->is_linktmo)
    return 0;
  req->link_next = t->link_next;
  t->link_next = 0;
  if (t->failed) {
    iou_lock(req->ctx);
    iou_req_unlink(req->ctx, t);
    iou_unlock(req->ctx);
    iou_post_req_cqe(t, t->fail_res, 0);
    iou_req_free(t);
    return 1;
  }
  t->tmo_target = req;
  req->tmo_armed = t;
  t->state = IOU_ST_ARMED;
  return 0;
}

/* IORING_POLL_ADD with an update flag: change the mask, the user_data, or both,
 * of a poll that is already armed. Returns 0 or -ENOENT, as Linux does. */
static i32 iou_poll_update(struct io_ring_ctx *ctx,
                           const struct io_uring_sqe *sqe) {
  i32 rc = -ENOENT;

  iou_lock(ctx);
  for (struct iou_req *r = ctx->live; r; r = r->next) {
    if (r->state != IOU_ST_ARMED || r->sqe.opcode != IORING_OP_POLL_ADD)
      continue;
    if (r->sqe.user_data != sqe->addr)
      continue;
    if (sqe->len & IORING_POLL_UPDATE_EVENTS) {
      r->poll_mask = (u16)(sqe->poll32_events & 0xffff);
      if (!r->poll_mask)
        r->poll_mask = B1NIX_POLLIN;
      r->mshot_reported = 0;
    }
    if (sqe->len & IORING_POLL_UPDATE_USER_DATA)
      r->sqe.user_data = sqe->off;
    rc = 0;
    break;
  }
  iou_unlock(ctx);
  return rc;
}

/* Issue a request. Returns 1 if it was armed (no CQE yet), 0 if it completed. */
static int iou_issue(struct iou_req *req) {
  struct io_ring_ctx *ctx = req->ctx;
  const struct io_uring_sqe *sqe = &req->sqe;

  if (iou_arm_link_timeout(req)) {
    iou_complete(req, -ECANCELED, 0);
    return 0;
  }

  if (req->failed) {
    iou_complete(req, req->fail_res, 0);
    return 0;
  }

  if (req->is_poll_update) {
    iou_complete(req, iou_poll_update(ctx, sqe), 0);
    return 0;
  }

  /* Requests that are nothing but a deadline or a registration. */
  if (req->is_timeout) {
    req->state = IOU_ST_ARMED;
    req->timeout_target = ctx->cq_posted + req->timeout_count;
    return 1;
  }
  if (sqe->opcode == IORING_OP_POLL_ADD) {
    req->state = IOU_ST_ARMED;
    return 1; /* the sweep below reports it, ready or not */
  }
  if (req->is_linktmo) {
    /* A LINK_TIMEOUT with nothing before it is meaningless. */
    iou_complete(req, -EINVAL, 0);
    return 0;
  }

  if (req->poll_mask && iou_pollable(req->file)) {
    u16 rev = iou_poll_now(req->file, req->poll_mask);

    if (!(rev & (req->poll_mask | B1NIX_POLLERR | B1NIX_POLLHUP))) {
      req->state = IOU_ST_ARMED;
      return 1;
    }
  }

  return iou_run_once(req);
}

/* ---- the progress sweep ------------------------------------------------- */

/* Re-test every armed request of this ring. Called at every entry to
 * io_uring_enter and each time round its wait loop, which is where the
 * vfs_poll_chan wakes land. Returns 1 if anything completed. */
static int iou_progress(struct io_ring_ctx *ctx) {
  int did = 0;
  u64 now = ktime_monotonic_ns();

  for (;;) {
    struct iou_req *victim = 0;
    i32 res = 0;
    int is_poll_report = 0;
    /* Expired timeouts complete in DEADLINE order, not in the order the list
     * happens to hold them. Several timeouts armed at once and reaped in one
     * go is exactly what liburing's test_multi_timeout checks, and a list walk
     * that takes the first expired one it meets gets them backwards. */
    struct iou_req *best_tmo = 0;
    i32 best_res = 0;

    iou_lock(ctx);
    for (struct iou_req *r = ctx->live; r; r = r->next) {
      if (r->state != IOU_ST_ARMED)
        continue;

      if (r->is_timeout) {
        if (r->timeout_count && ctx->cq_posted >= r->timeout_target) {
          if (!best_tmo || !best_tmo->has_deadline) {
            best_tmo = r;
            best_res = 0;
          }
          continue;
        }
        if (r->has_deadline && now >= r->deadline_ns) {
          if (!best_tmo || !best_tmo->has_deadline ||
              r->deadline_ns < best_tmo->deadline_ns) {
            best_tmo = r;
            /* IORING_TIMEOUT_ETIME_SUCCESS does NOT turn the result into 0: the
             * completion still says -ETIME, and what the flag changes is
             * whether the rest of a link chain is cancelled because of it. */
            best_res = -ETIME;
          }
        }
        continue;
      }

      if (r->is_linktmo) {
        if (r->has_deadline && now >= r->deadline_ns) {
          victim = r;
          res = -ETIME;
          break;
        }
        continue;
      }

      if (r->has_deadline && now >= r->deadline_ns) {
        victim = r;
        res = -ECANCELED;
        break;
      }

      if (r->sqe.opcode == IORING_OP_POLL_ADD) {
        u16 rev = iou_poll_now(r->file, r->poll_mask);

        /* Report only what was asked for, plus the two that cannot be masked
         * out. An eventfd is always writable, so an unmasked revents answered
         * POLLIN|POLLOUT to a poll that asked for POLLIN — 5 where the caller
         * requires 1. */
        rev &= (u16)(r->poll_mask | B1NIX_POLLERR | B1NIX_POLLHUP);
        if (rev) {
          /* A multishot poll reports once per readiness edge; see
           * iou_req::mshot_reported for why a level-triggered ->poll cannot be
           * reported on every sweep. */
          if (r->multishot && r->mshot_reported)
            continue;
          victim = r;
          res = (i32)rev;
          is_poll_report = 1;
          break;
        }
        r->mshot_reported = 0;
        continue;
      }

      if (r->poll_mask) {
        u16 rev = iou_poll_now(r->file, r->poll_mask);

        if (rev & (r->poll_mask | B1NIX_POLLERR | B1NIX_POLLHUP)) {
          victim = r;
          res = 1; /* marker: run it below */
          break;
        }
      }
    }
    if (best_tmo) {
      victim = best_tmo;
      res = best_res;
      is_poll_report = 0;
    }
    /* Claim it while the lock is still held. With IORING_SETUP_SQPOLL there
     * are two sweepers — the submission thread and whoever called
     * io_uring_enter — and an armed request left ARMED until after the unlock
     * could be picked, performed and freed by both. */
    if (victim)
      victim->state = IOU_ST_QUEUED;
    iou_unlock(ctx);

    if (!victim)
      break;

    did = 1;

    if (victim->is_linktmo) {
      /* The deadline beat the request it was watching: cancel that request and
       * report -ETIME for the timeout itself, which is what Linux does. */
      struct iou_req *target = victim->tmo_target;

      iou_trace("linktmo", victim->sqe.user_data,
                target ? target->sqe.user_data : 0xdead, 0);
      victim->tmo_target = 0;
      if (target)
        target->tmo_armed = 0;
      iou_lock(ctx);
      iou_req_unlink(ctx, victim);
      iou_unlock(ctx);
      iou_post_req_cqe(victim, -ETIME, 0);
      iou_req_free(victim);
      if (target) {
        struct iou_req *rest = target->link_next;

        target->link_next = 0;
        iou_lock(ctx);
        iou_req_unlink(ctx, target);
        iou_unlock(ctx);
        iou_post_req_cqe(target, -ECANCELED, 0);
        iou_req_free(target);
        if (rest)
          iou_cancel_chain(rest);
      }
      continue;
    }

    if (is_poll_report && victim->multishot) {
      victim->mshot_reported = 1;
      victim->state = IOU_ST_ARMED; /* it reports again on the next edge */
      iou_post_req_cqe(victim, res, IORING_CQE_F_MORE);
      continue;
    }

    if (victim->is_timeout || is_poll_report) {
      iou_complete(victim, res, 0);
      continue;
    }

    if (res == -ECANCELED) {
      iou_complete(victim, -ECANCELED, 0);
      continue;
    }

    /* Ready: it was claimed above, so perform it outside the lock. */
    iou_run_once(victim);
  }

  if (did)
    scheduler_wake_all(ctx);
  return did;
}

/* ---- cancellation ------------------------------------------------------- */

static int iou_cancel_by(struct io_ring_ctx *ctx, u64 user_data, int by_fd,
                         int fd, u32 cancel_flags) {
  int found = 0;

  for (;;) {
    struct iou_req *victim = 0;

    iou_lock(ctx);
    for (struct iou_req *r = ctx->live; r; r = r->next) {
      if (r->state != IOU_ST_ARMED)
        continue;
      if (cancel_flags & IORING_ASYNC_CANCEL_ANY) {
        victim = r;
        break;
      }
      if (by_fd) {
        if (r->sqe.fd == fd) {
          victim = r;
          break;
        }
        continue;
      }
      if (r->sqe.user_data == user_data) {
        victim = r;
        break;
      }
    }
    if (victim)
      iou_req_unlink(ctx, victim);
    iou_unlock(ctx);

    if (!victim)
      break;
    found++;

    if (victim->tmo_target)
      victim->tmo_target->tmo_armed = 0;
    struct iou_req *rest = victim->link_next;

    victim->link_next = 0;
    iou_post_req_cqe(victim, -ECANCELED, 0);
    iou_req_free(victim);
    if (rest)
      iou_cancel_chain(rest);

    if (!(cancel_flags & (IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_ANY)))
      break;
  }
  return found;
}

/* ---- submission --------------------------------------------------------- */

#define IOU_SQE_VALID_FLAGS                                                    \
  (IOSQE_FIXED_FILE | IOSQE_IO_LINK | IOSQE_IO_HARDLINK | IOSQE_ASYNC |        \
   IOSQE_CQE_SKIP_SUCCESS | IOSQE_IO_DRAIN | IOSQE_BUFFER_SELECT)

/* Which readiness, if any, a request has to wait for. */
static u16 iou_op_poll_mask(const struct io_uring_sqe *sqe) {
  switch (sqe->opcode) {
  case IORING_OP_READ:
  case IORING_OP_READV:
  case IORING_OP_READ_FIXED:
  case IORING_OP_READ_MULTISHOT:
  case IORING_OP_RECV:
  case IORING_OP_RECVMSG:
  case IORING_OP_ACCEPT:
    return B1NIX_POLLIN;
  case IORING_OP_WRITE:
  case IORING_OP_WRITEV:
  case IORING_OP_WRITE_FIXED:
  case IORING_OP_SEND:
  case IORING_OP_SEND_ZC:
  case IORING_OP_SENDMSG:
  case IORING_OP_SENDMSG_ZC:
    return B1NIX_POLLOUT;
  default:
    return 0;
  }
}

static int iou_op_needs_file(u8 opcode) {
  switch (opcode) {
  case IORING_OP_NOP:
  case IORING_OP_TIMEOUT:
  case IORING_OP_TIMEOUT_REMOVE:
  case IORING_OP_LINK_TIMEOUT:
  case IORING_OP_ASYNC_CANCEL:
  case IORING_OP_POLL_REMOVE:
  case IORING_OP_CLOSE:
  case IORING_OP_FILES_UPDATE:
  case IORING_OP_PROVIDE_BUFFERS:
  case IORING_OP_REMOVE_BUFFERS:
  case IORING_OP_MADVISE:
  case IORING_OP_SOCKET:
  case IORING_OP_WAITID:
  case IORING_OP_PIPE:
  /* The *at() opcodes put a DIRECTORY descriptor in sqe->fd, and AT_FDCWD is
   * not a descriptor at all. They resolve it themselves, with iou_path_at. */
  case IORING_OP_OPENAT:
  case IORING_OP_OPENAT2:
  case IORING_OP_STATX:
  case IORING_OP_RENAMEAT:
  case IORING_OP_UNLINKAT:
  case IORING_OP_MKDIRAT:
  case IORING_OP_SYMLINKAT:
  case IORING_OP_LINKAT:
    return 0;
  default:
    return 1;
  }
}

/* Whether this opcode may be asked to report more than once. */
static int iou_op_is_multishot(const struct io_uring_sqe *sqe) {
  switch (sqe->opcode) {
  case IORING_OP_POLL_ADD:
    return (sqe->len & IORING_POLL_ADD_MULTI) ? 1 : 0;
  case IORING_OP_ACCEPT:
    return (sqe->ioprio & IORING_ACCEPT_MULTISHOT) ? 1 : 0;
  case IORING_OP_RECV:
  case IORING_OP_RECVMSG:
    return (sqe->ioprio & IORING_RECV_MULTISHOT) ? 1 : 0;
  case IORING_OP_READ_MULTISHOT:
    return 1;
  default:
    return 0;
  }
}

/* Returns 0 when the SQE was accepted, 1 when it was accepted but is already
 * doomed (a bad descriptor, a flag this kernel refuses), or a negative errno
 * when submission itself must stop. The middle case is what
 * IORING_SETUP_SUBMIT_ALL is about: without that flag, Linux stops submitting
 * at the first SQE that fails to prepare, and a caller counts on the returned
 * number to know how far it got. */
static int iou_submit_one(struct io_ring_ctx *ctx,
                          const struct io_uring_sqe *sqe,
                          struct iou_req **chain_head,
                          struct iou_req **chain_tail) {
  struct iou_req *req;
  int err = 0;

  if (ctx->nr_live >= IOU_MAX_INFLIGHT)
    return -EBUSY;

  iou_trace("sqe", sqe->opcode, sqe->user_data, (isize)sqe->flags);
  req = kzalloc(sizeof(*req));
  if (!req)
    return -ENOMEM;
  req->ctx = ctx;
  req->sqe = *sqe; /* IORING_FEAT_SUBMIT_STABLE: the SQE is ours from here */
  req->state = IOU_ST_QUEUED;
  req->hardlink = (sqe->flags & IOSQE_IO_HARDLINK) ? 1 : 0;
  req->cqe_skip = (sqe->flags & IOSQE_CQE_SKIP_SUCCESS) ? 1 : 0;

  req->multishot = (u8)iou_op_is_multishot(sqe);
  req->file_index = sqe->file_index;
  req->sel_len = sqe->len;

  if (sqe->flags & ~(u8)IOU_SQE_VALID_FLAGS) {
    req->failed = 1;
    req->fail_res = -EINVAL;
  } else if (ctx->restricted && !ctx->restr_sqe[sqe->opcode % IORING_OP_LAST]) {
    /* IORING_REGISTER_RESTRICTIONS named the opcodes this ring may run, and
     * this is not one of them. */
    req->failed = 1;
    req->fail_res = -EACCES;
  } else if (ctx->restricted &&
             ((sqe->flags & ~ctx->restr_flags_allowed) ||
              (sqe->flags & ctx->restr_flags_required) !=
                  ctx->restr_flags_required)) {
    req->failed = 1;
    req->fail_res = -EACCES;
  } else if ((sqe->flags & IOSQE_BUFFER_SELECT) &&
             !iou_op_takes_buffer(sqe->opcode)) {
    /* A buffer can only be chosen for an operation that reads into one. */
    req->failed = 1;
    req->fail_res = -EOPNOTSUPP;
  } else if (sqe->opcode == IORING_OP_READ_MULTISHOT &&
             !(sqe->flags & IOSQE_BUFFER_SELECT)) {
    /* A multishot read has nowhere to put the second answer without a buffer
     * group to take it from. */
    req->failed = 1;
    req->fail_res = -EINVAL;
  } else if (req->multishot &&
             (sqe->opcode == IORING_OP_RECV ||
              sqe->opcode == IORING_OP_RECVMSG) &&
             !(sqe->flags & IOSQE_BUFFER_SELECT)) {
    req->failed = 1;
    req->fail_res = -EINVAL;
  } else if (sqe->opcode == IORING_OP_TIMEOUT) {
    u64 ns = 0;

    /* What kind of request this is has to be recorded BEFORE it can be
     * refused. A refusal that leaves the kind unset makes a doomed link
     * timeout look like an ordinary member of the chain, and the chain then
     * waits on a request in front of it that nothing will ever finish — which
     * is what liburing's link-timeout test stopped answering on. */
    req->is_timeout = 1;
    /* Linux validates the whole SQE, not just the parts it reads: a field
     * that has no meaning for this opcode is a caller who meant something
     * else. liburing sets one on purpose and requires EINVAL for it. */
    if ((sqe->timeout_flags & ~(u32)(IORING_TIMEOUT_ABS |
                                     IORING_TIMEOUT_ETIME_SUCCESS)) ||
        sqe->buf_index || sqe->len != 1 || sqe->splice_fd_in) {
      req->failed = 1;
      req->fail_res = -EINVAL;
      goto linked;
    }
    int tsrc = iou_timespec_in(sqe->addr, &ns);

    if (tsrc < 0) {
      /* EINVAL for a nonsense interval, EFAULT for a pointer that would not
       * copy: the two are different answers and a caller tells them apart. */
      req->failed = 1;
      req->fail_res = tsrc;
      goto linked;
    }
    req->timeout_count = (u32)sqe->off;
    req->has_deadline = 1;
    req->timeout_etime_success =
        (sqe->timeout_flags & IORING_TIMEOUT_ETIME_SUCCESS) ? 1 : 0;
    req->deadline_ns = (sqe->timeout_flags & IORING_TIMEOUT_ABS)
                           ? ns
                           : ktime_monotonic_ns() + ns;
  } else if (sqe->opcode == IORING_OP_LINK_TIMEOUT) {
    u64 ns = 0;

    int tsrc;

    req->is_linktmo = 1; /* see the note above IORING_OP_TIMEOUT */
    if (sqe->ioprio || sqe->buf_index || sqe->len != 1 || sqe->off ||
        sqe->splice_fd_in ||
        (sqe->timeout_flags & ~(u32)IORING_TIMEOUT_ABS)) {
      req->failed = 1;
      req->fail_res = -EINVAL;
      goto linked;
    }
    tsrc = iou_timespec_in(sqe->addr, &ns);
    if (tsrc < 0) {
      req->failed = 1;
      req->fail_res = tsrc;
      goto linked;
    }
    req->has_deadline = 1;
    req->deadline_ns = (sqe->timeout_flags & IORING_TIMEOUT_ABS)
                           ? ns
                           : ktime_monotonic_ns() + ns;
  } else if (sqe->opcode == IORING_OP_POLL_ADD) {
    u32 mask = sqe->poll32_events;

    if (sqe->len & ~(u32)(IORING_POLL_ADD_MULTI | IORING_POLL_UPDATE_EVENTS |
                          IORING_POLL_UPDATE_USER_DATA |
                          IORING_POLL_ADD_LEVEL)) {
      req->failed = 1;
      req->fail_res = -EINVAL;
      goto linked;
    }
    if (sqe->len & (IORING_POLL_UPDATE_EVENTS | IORING_POLL_UPDATE_USER_DATA)) {
      /* IORING_POLL_ADD as an UPDATE: sqe->addr names the poll to change by its
       * user_data, sqe->off carries the new user_data and poll32_events the new
       * mask. It is not a request of its own — it completes at once. */
      req->is_poll_update = 1;
      goto linked;
    }
    req->poll_mask = (u16)(mask & 0xffff);
    if (!req->poll_mask)
      req->poll_mask = B1NIX_POLLIN;
  } else {
    req->poll_mask = iou_op_poll_mask(sqe);
  }

  if (sqe->flags & IOSQE_BUFFER_SELECT) {
    req->buf_select = 1;
    req->buf_group = sqe->buf_group;
  }

  /* The iovec array is copied here, not when the request runs: see the note on
   * iou_req::iov. */
  if (!req->failed &&
      (sqe->opcode == IORING_OP_READV || sqe->opcode == IORING_OP_WRITEV)) {
    if (sqe->len > 1024) {
      req->failed = 1;
      req->fail_res = -EINVAL;
    } else if (sqe->len) {
      req->iov = kmalloc(sizeof(*req->iov) * sqe->len);
      if (!req->iov) {
        req->failed = 1;
        req->fail_res = -ENOMEM;
      } else if (syscall_copyin(req->iov, (const void *)(usize)sqe->addr,
                                sizeof(*req->iov) * sqe->len) < 0) {
        req->failed = 1;
        req->fail_res = -EFAULT;
      } else {
        req->iovcnt = sqe->len;
      }
    }
  }

  if (!req->failed &&
      (iou_op_needs_file(sqe->opcode) || sqe->opcode == IORING_OP_POLL_ADD)) {
    req->file = iou_get_file(ctx, sqe, &err);
    if (err) {
      req->failed = 1;
      req->fail_res = err;
    }
  }

linked:
  iou_req_link(ctx, req);

  /* Link chains. The SQE that carries IOSQE_IO_LINK says the NEXT one belongs
   * to the same chain, and the chain runs one request at a time.
   *
   * Nothing is issued until the chain is complete, and that is not a style
   * choice: issuing the head as soon as it arrives means the head can finish —
   * and be freed — before the second SQE of the batch is read, and the pointer
   * this function was about to append to is then a dangling one. So collect,
   * then issue. */
  if (*chain_head) {
    /* Read this BEFORE issuing: a chain that completes synchronously frees
     * every request in it, `req` included, on the way out of iou_issue. */
    int doomed = req->failed ? 1 : 0;

    (*chain_tail)->link_next = req;
    *chain_tail = req;
    if (!(sqe->flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK))) {
      struct iou_req *head = *chain_head;

      *chain_head = 0;
      *chain_tail = 0;
      iou_issue_chain(head);
    }
    return doomed;
  }
  if (sqe->flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK)) {
    *chain_head = req;
    *chain_tail = req;
    return req->failed ? 1 : 0;
  }
  {
    int doomed = req->failed ? 1 : 0;

    iou_issue(req);
    return doomed;
  }
}

static int iou_submit_sqes(struct io_ring_ctx *ctx, u32 to_submit) {
  u32 submitted = 0;
  struct iou_req *chain_head = 0, *chain_tail = 0;

  while (submitted < to_submit) {
    u32 tail = __atomic_load_n(&ctx->hdr->sq_tail, __ATOMIC_ACQUIRE);
    u32 head = ctx->sq_local_head;
    u32 idx;

    if (head == tail)
      break;
    /* IORING_SETUP_NO_SQARRAY: there is no indirection array, and the ring
     * head indexes the SQEs directly. */
    idx = ctx->sq_array ? ctx->sq_array[head & ctx->sq_mask]
                        : (head & ctx->sq_mask);
    ctx->sq_local_head = head + 1;
    __atomic_store_n(&ctx->hdr->sq_head, ctx->sq_local_head, __ATOMIC_RELEASE);

    if (idx >= ctx->sq_entries) {
      u32 d = __atomic_load_n(&ctx->hdr->sq_dropped, __ATOMIC_RELAXED);

      __atomic_store_n(&ctx->hdr->sq_dropped, d + 1, __ATOMIC_RELEASE);
      continue;
    }

    struct io_uring_sqe sqe = *iou_sqe_at(ctx, idx);
    int rc = iou_submit_one(ctx, &sqe, &chain_head, &chain_tail);

    if (rc < 0) {
      if (submitted == 0)
        return rc;
      break;
    }
    submitted++;
    /* One SQE could not be prepared. Unless the ring asked for every entry to
     * be attempted, that is where this submission stops — and it counts, so
     * the caller sees how far the batch got.
     *
     * Not in the middle of a link chain, though. A chain is submitted as a
     * unit and the rest of it is what receives the ECANCELED: stopping at the
     * doomed head left the following SQEs unread, so a caller that submitted
     * two and waited for two waited for ever. */
    if (rc == 1 && !chain_head && !(ctx->flags & IORING_SETUP_SUBMIT_ALL))
      break;
  }

  /* A chain the batch never terminated still has to run: Linux issues an
   * unterminated link when the submission batch ends. */
  if (chain_head)
    iou_issue_chain(chain_head);
  return (int)submitted;
}

/* ---- io_uring_enter ----------------------------------------------------- */

struct iou_getevents_arg {
  u64 sigmask;
  u32 sigmask_sz;
  u32 min_wait_usec;
  u64 ts;
};

static isize iou_wait_cqes(struct io_ring_ctx *ctx, u32 min_complete,
                           const u64 *deadline_ns) {
  u64 timeouts0 = __atomic_load_n(&ctx->cq_timeouts, __ATOMIC_ACQUIRE);

  for (;;) {
    iou_progress(ctx);

    iou_lock(ctx);
    iou_flush_overflow_locked(ctx);
    iou_unlock(ctx);

    if (iou_cq_ready(ctx) >= min_complete)
      return 0;
    /* A timeout fired: go back to userspace whatever min_complete said. */
    if (__atomic_load_n(&ctx->cq_timeouts, __ATOMIC_ACQUIRE) != timeouts0)
      return 0;
    if (deadline_ns && ktime_monotonic_ns() >= *deadline_ns) {
      /* The deadline is only an error when the wait produced nothing. Linux
       * ends io_cqring_wait with "return cq empty ? ret : 0", and a caller
       * relies on it: liburing's io_uring_wait_cqes asks for two completions
       * with one already in the ring, and a bare -ETIME there loses the
       * completion that WAS ready. */
      return iou_cq_ready(ctx) ? 0 : -ETIME;
    }

    /* Publish BLOCKED before the last look, the way sys_poll and epoll_wait do:
     * a readiness change on another CPU either lands in the scan above or sees
     * this task already blocked. A bounded block keeps deadlines honest
     * (an explicit wake clears wake_tick, so the timeout is re-armed each time
     * round). */
    u64 ticks = SCHED_MS_TO_TICKS(20);

    if (deadline_ns) {
      u64 now = ktime_monotonic_ns();
      u64 left_ms = (*deadline_ns > now) ? (*deadline_ns - now) / 1000000ull : 0;
      u64 t = SCHED_MS_TO_TICKS(left_ms);

      if (t < ticks)
        ticks = t;
    }
    if (ticks == 0)
      ticks = 1;

    scheduler_wait_prepare_timeout(vfs_poll_chan, ticks);
    if (iou_cq_ready(ctx) >= min_complete ||
        __atomic_load_n(&ctx->cq_timeouts, __ATOMIC_ACQUIRE) != timeouts0) {
      scheduler_wait_cancel();
      return 0;
    }
    if (scheduler_signal_pending()) {
      scheduler_wait_cancel();
      return -EINTR;
    }
    scheduler_wait_commit();
  }
}

/* ---- setup -------------------------------------------------------------- */

/* Every IORING_SETUP_* flag this kernel can honour. Anything else is refused
 * at setup, so a program that needs it learns so at the one moment it can
 * still choose a different strategy.
 *
 * IORING_SETUP_IOPOLL is in the list and means what it says here. Its contract
 * is that completions are reaped by io_uring_enter(IORING_ENTER_GETEVENTS) and
 * by nothing else — no interrupt posts them behind the program's back. b1nix's
 * block layer completes an I/O synchronously inside the request that issued
 * it, so a ring with IOPOLL set has every completion in the ring by the time
 * the submitting io_uring_enter returns, and a reap from GETEVENTS is the only
 * thing that hands one to the program. That satisfies the contract exactly; it
 * is not a shortcut, and there is no separate polling queue to build because
 * nothing is ever in flight to poll for.
 *
 * What is still refused: IORING_SETUP_SQ_AFF (there is no way to pin the
 * submission thread to sq_thread_cpu, and accepting the flag would be a
 * promise about placement that is not kept), ATTACH_WQ (no io-wq to attach
 * to), NO_MMAP, REGISTERED_FD_ONLY, HYBRID_IOPOLL and CQE_MIXED. */
#define IOU_SETUP_SUPPORTED                                                    \
  (IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP | IORING_SETUP_SUBMIT_ALL |        \
   IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG |                     \
   IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_R_DISABLED |                      \
   IORING_SETUP_IOPOLL | IORING_SETUP_SQPOLL | IORING_SETUP_SQE128 |           \
   IORING_SETUP_CQE32 | IORING_SETUP_NO_SQARRAY | IORING_SETUP_DEFER_TASKRUN)

/* What is true here, and nothing more. IORING_FEAT_SINGLE_MMAP because both
 * rings share one region; NODROP because an overflow is kept; SUBMIT_STABLE
 * because the SQE is copied at submission; RW_CUR_POS because an offset of -1
 * means the descriptor's own position; CUR_PERSONALITY because the request runs
 * as the submitter; FAST_POLL because a request that would block is armed on
 * the file's readiness rather than handed to a thread; POLL_32BITS because
 * poll32_events is read; EXT_ARG because io_uring_enter takes
 * io_uring_getevents_arg; CQE_SKIP because IOSQE_CQE_SKIP_SUCCESS works;
 * LINKED_FILE because a linked request resolves its file when it runs. */
#define IOU_FEATURES                                                           \
  (IORING_FEAT_SINGLE_MMAP | IORING_FEAT_NODROP | IORING_FEAT_SUBMIT_STABLE |  \
   IORING_FEAT_RW_CUR_POS | IORING_FEAT_CUR_PERSONALITY |                      \
   IORING_FEAT_FAST_POLL | IORING_FEAT_POLL_32BITS | IORING_FEAT_EXT_ARG |     \
   IORING_FEAT_CQE_SKIP | IORING_FEAT_LINKED_FILE |                            \
   IORING_FEAT_SQPOLL_NONFIXED | IORING_FEAT_NO_IOWAIT)

static u32 iou_roundup_pow2(u32 v) {
  u32 p = 1;

  while (p < v)
    p <<= 1;
  return p;
}

static void iou_ctx_get(struct io_ring_ctx *ctx) {
  __atomic_add_fetch(&ctx->refs, 1, __ATOMIC_RELAXED);
}

static void iou_ctx_put(struct io_ring_ctx *ctx) {
  if (__atomic_sub_fetch(&ctx->refs, 1, __ATOMIC_ACQ_REL) != 0)
    return;

  iou_list_lock();
  struct io_ring_ctx **pp = &g_ctx_list;

  while (*pp) {
    if (*pp == ctx) {
      *pp = ctx->next;
      break;
    }
    pp = &(*pp)->next;
  }
  iou_list_unlock();

  iou_free_region(ctx->ring_phys, ctx->ring_pages);
  iou_free_region(ctx->sqes_phys, ctx->sqes_pages);
  kfree(ctx->files);
  kfree(ctx->bufs);
  kfree(ctx);
}

/* ---- the submission thread (IORING_SETUP_SQPOLL) ------------------------
 *
 * A thread of the ring's own that consumes the submission queue, so a program
 * can submit by writing an SQE and advancing the tail and never enter the
 * kernel at all. It is a kernel thread that has adopted the owner's address
 * space and descriptor table — Linux builds the same thing with
 * create_io_thread(), a clone sharing VM and files — because a submission is
 * only useful if the data can be copied into the owner's buffers and
 * sqe->fd can be looked up in the owner's table.
 *
 * The handshake with userspace is the one the ABI defines: after
 * sq_thread_idle milliseconds with nothing to do, the thread publishes
 * IORING_SQ_NEED_WAKEUP in the ring's sq_flags and sleeps. A submitter that
 * sees that bit calls io_uring_enter(IORING_ENTER_SQ_WAKEUP), which wakes it.
 * The bit is published before the last look at the tail, so a submission that
 * raced the flag going up is still seen.
 */

static void iou_sq_need_wakeup(struct io_ring_ctx *ctx, int on) {
  u32 f = __atomic_load_n(&ctx->hdr->sq_flags, __ATOMIC_RELAXED);

  __atomic_store_n(&ctx->hdr->sq_flags,
                   on ? (f | IORING_SQ_NEED_WAKEUP)
                      : (f & ~(u32)IORING_SQ_NEED_WAKEUP),
                   __ATOMIC_RELEASE);
}

static int iou_sq_pending(struct io_ring_ctx *ctx) {
  return __atomic_load_n(&ctx->hdr->sq_tail, __ATOMIC_ACQUIRE) !=
         ctx->sq_local_head;
}

static void iou_sq_thread(void *arg) {
  struct io_ring_ctx *ctx = (struct io_ring_ctx *)arg;
  u64 last_work;

  if (scheduler_adopt_owner_context(ctx->owner_tgid) < 0) {
    __atomic_store_n(&ctx->sq_stop, 1, __ATOMIC_RELEASE);
    iou_ctx_put(ctx);
    return;
  }
  __atomic_store_n(&ctx->sq_alive, 1, __ATOMIC_RELEASE);
  last_work = ktime_monotonic_ns();

  while (!__atomic_load_n(&ctx->sq_stop, __ATOMIC_ACQUIRE)) {
    int did = 0;

    if (__atomic_load_n(&ctx->enabled, __ATOMIC_ACQUIRE)) {
      if (iou_sq_pending(ctx)) {
        u32 tail = __atomic_load_n(&ctx->hdr->sq_tail, __ATOMIC_ACQUIRE);

        iou_sq_need_wakeup(ctx, 0);
        if (iou_submit_sqes(ctx, tail - ctx->sq_local_head) > 0)
          did = 1;
      }
      if (iou_progress(ctx))
        did = 1;
    }
    if (did) {
      last_work = ktime_monotonic_ns();
      scheduler_yield();
      continue;
    }
    if (ktime_monotonic_ns() - last_work <
        (u64)ctx->sq_idle_ms * 1000000ull) {
      /* Still inside the idle window: keep looking, but from a one-tick sleep
       * rather than a spin, so a ring left open costs a timer wake and not a
       * core. */
      scheduler_wait_prepare_timeout(&ctx->sq_wait, 1);
      if (iou_sq_pending(ctx) || __atomic_load_n(&ctx->sq_stop,
                                                 __ATOMIC_ACQUIRE))
        scheduler_wait_cancel();
      else
        scheduler_wait_commit();
      continue;
    }

    iou_sq_need_wakeup(ctx, 1);
    /* Published; now look once more. A submitter that wrote the tail just
     * before the flag went up did not see it and will not call enter(). */
    if (iou_sq_pending(ctx) ||
        __atomic_load_n(&ctx->sq_stop, __ATOMIC_ACQUIRE)) {
      iou_sq_need_wakeup(ctx, 0);
      last_work = ktime_monotonic_ns();
      continue;
    }
    /* A bounded sleep: the wake from io_uring_enter is what normally ends it,
     * and the bound is what keeps an armed request's deadline honest. */
    scheduler_wait_prepare_timeout(&ctx->sq_wait, SCHED_MS_TO_TICKS(20));
    if (iou_sq_pending(ctx) || __atomic_load_n(&ctx->sq_stop, __ATOMIC_ACQUIRE))
      scheduler_wait_cancel();
    else
      scheduler_wait_commit();
    iou_sq_need_wakeup(ctx, 0);
    last_work = ktime_monotonic_ns();
  }

  iou_sq_need_wakeup(ctx, 0);
  __atomic_store_n(&ctx->sq_alive, 0, __ATOMIC_RELEASE);
  iou_ctx_put(ctx);
}

/* Stop the submission thread and wait for it to be gone. Everything after this
 * may free what the thread was reading. */
static void iou_sq_thread_stop(struct io_ring_ctx *ctx) {
  if (!ctx->sq_tid)
    return;
  __atomic_store_n(&ctx->sq_stop, 1, __ATOMIC_RELEASE);
  scheduler_wake_all(&ctx->sq_wait);
  while (__atomic_load_n(&ctx->sq_alive, __ATOMIC_ACQUIRE)) {
    scheduler_wake_all(&ctx->sq_wait);
    scheduler_yield();
  }
  ctx->sq_tid = 0;
}

/* Everything the ring still owns when its last descriptor goes away. */
static void iou_quiesce(struct io_ring_ctx *ctx) {
  struct iou_req *r;

  iou_sq_thread_stop(ctx);

  iou_lock(ctx);
  r = ctx->live;
  ctx->live = 0;
  ctx->nr_live = 0;
  iou_unlock(ctx);

  while (r) {
    struct iou_req *next = r->next;

    r->link_next = 0;
    r->tmo_armed = 0;
    r->tmo_target = 0;
    iou_req_free(r);
    r = next;
  }

  iou_lock(ctx);
  struct iou_overflow *o = ctx->ovfl_head;

  ctx->ovfl_head = ctx->ovfl_tail = 0;
  iou_unlock(ctx);
  while (o) {
    struct iou_overflow *n = o->next;

    kfree(o);
    o = n;
  }

  if (ctx->files) {
    for (u32 i = 0; i < ctx->nr_files; i++)
      if (ctx->files[i].file) {
        vfs_handle_release(ctx->files[i].file);
        ctx->files[i].file = 0;
      }
  }
  if (ctx->cq_eventfd) {
    vfs_handle_release(ctx->cq_eventfd);
    ctx->cq_eventfd = 0;
  }
  iou_lock(ctx);
  iou_bgroup_free_all(ctx);
  iou_unlock(ctx);
}

static void iou_handle_release(struct vfs_handle *h) {
  struct io_ring_ctx *ctx = (struct io_ring_ctx *)h->private_data;

  h->private_data = 0;
  if (ctx) {
    iou_quiesce(ctx);
    iou_ctx_put(ctx);
  }
  /* A handle of kind VFS_HANDLE_NODE with its own .release does not get the
   * default vfs_node_put, so do it here. */
  if (h->node) {
    vfs_node_put(h->node);
    h->node = 0;
  }
}

static int iou_handle_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct io_ring_ctx *ctx = (struct io_ring_ctx *)h->private_data;

  pfd->revents = 0;
  if (!ctx)
    return -EBADF;
  if (iou_cq_ready(ctx) > 0)
    pfd->revents |= B1NIX_POLLIN;
  return 0;
}

static const struct vfs_file_ops iou_file_ops = {
    .poll = iou_handle_poll,
    .release = iou_handle_release,
};

static int iou_is_ring_handle(const struct vfs_handle *h) {
  return h && h->ops == &iou_file_ops;
}

/* The inode outlives the descriptor when a mapping is still standing, so the
 * ring memory is only given back once both are gone. */
static void iou_inode_release(struct vfs_node *node) {
  struct io_ring_ctx *ctx = 0;

  iou_list_lock();
  for (struct io_ring_ctx *c = g_ctx_list; c; c = c->next)
    if (c->node == node) {
      ctx = c;
      break;
    }
  iou_list_unlock();
  if (ctx) {
    ctx->node = 0;
    iou_ctx_put(ctx);
  }
}

static isize iou_setup(u32 entries, u64 uparams) {
  struct io_uring_params p;
  struct io_ring_ctx *ctx;
  u32 cqes_off = 0;
  usize ring_bytes;

  if (syscall_copyin(&p, (const void *)(usize)uparams, sizeof(p)) < 0)
    return -EFAULT;
  for (int i = 0; i < 3; i++)
    if (p.resv[i])
      return -EINVAL;
  if (p.flags & ~(u32)IOU_SETUP_SUPPORTED)
    return -EINVAL;
  if ((p.flags & IORING_SETUP_TASKRUN_FLAG) &&
      !(p.flags & IORING_SETUP_COOP_TASKRUN))
    return -EINVAL;
  /* Linux couples these two: deferred task work only means anything when one
   * task owns the ring. */
  if ((p.flags & IORING_SETUP_DEFER_TASKRUN) &&
      !(p.flags & IORING_SETUP_SINGLE_ISSUER))
    return -EINVAL;
  if (entries == 0)
    return -EINVAL;
  /* The submission thread runs in the owner's address space, so there has to
   * be one: a ring set up from a kernel thread has nothing to adopt. */
  if ((p.flags & IORING_SETUP_SQPOLL) &&
      (!current_task || !current_task->pml4_phys))
    return -EINVAL;

  if (entries > IOU_MAX_SQ_ENTRIES) {
    if (!(p.flags & IORING_SETUP_CLAMP))
      return -EINVAL;
    entries = IOU_MAX_SQ_ENTRIES;
  }
  u32 sq_entries = iou_roundup_pow2(entries);
  u32 cq_entries;

  if (p.flags & IORING_SETUP_CQSIZE) {
    if (p.cq_entries == 0)
      return -EINVAL;
    cq_entries = iou_roundup_pow2(p.cq_entries);
    if (cq_entries > IOU_MAX_CQ_ENTRIES) {
      if (!(p.flags & IORING_SETUP_CLAMP))
        return -EINVAL;
      cq_entries = IOU_MAX_CQ_ENTRIES;
    }
    if (cq_entries < sq_entries)
      return -EINVAL;
  } else {
    cq_entries = sq_entries * 2;
    if (cq_entries > IOU_MAX_CQ_ENTRIES)
      cq_entries = IOU_MAX_CQ_ENTRIES;
  }

  ctx = kzalloc(sizeof(*ctx));
  if (!ctx)
    return -ENOMEM;
  ctx->refs = 1;
  ctx->flags = p.flags;
  ctx->sq_entries = sq_entries;
  ctx->cq_entries = cq_entries;
  ctx->sq_mask = sq_entries - 1;
  ctx->cq_mask = cq_entries - 1;
  ctx->enabled = (p.flags & IORING_SETUP_R_DISABLED) ? 0 : 1;
  ctx->owner_tgid = current_task ? task_tgid(current_task) : 0;
  ctx->cqe_size = (p.flags & IORING_SETUP_CQE32) ? 32u
                                                 : sizeof(struct io_uring_cqe);
  ctx->sqe_size = (p.flags & IORING_SETUP_SQE128)
                      ? 128u
                      : sizeof(struct io_uring_sqe);

  ring_bytes = iou_ring_bytes(
      (p.flags & IORING_SETUP_NO_SQARRAY) ? 0 : sq_entries, cq_entries,
      ctx->cqe_size, &cqes_off);
  ctx->ring_phys = iou_alloc_region(ring_bytes, &ctx->ring_pages);
  if (!ctx->ring_phys) {
    kfree(ctx);
    return -ENOMEM;
  }
  ctx->sqes_phys = iou_alloc_region((usize)sq_entries * ctx->sqe_size,
                                    &ctx->sqes_pages);
  if (!ctx->sqes_phys) {
    iou_free_region(ctx->ring_phys, ctx->ring_pages);
    kfree(ctx);
    return -ENOMEM;
  }

  ctx->hdr = (struct iou_ring_hdr *)iou_kva(ctx->ring_phys);
  ctx->sq_array = (p.flags & IORING_SETUP_NO_SQARRAY)
                      ? 0
                      : (u32 *)((char *)ctx->hdr + IOU_OFF_SQ_ARRAY);
  ctx->cqes = (struct io_uring_cqe *)((char *)ctx->hdr + cqes_off);
  ctx->sqes = (struct io_uring_sqe *)iou_kva(ctx->sqes_phys);

  /* pmm_alloc_frame(s) hands back zeroed memory, so the rings start empty. */
  ctx->hdr->sq_ring_mask = ctx->sq_mask;
  ctx->hdr->sq_ring_entries = sq_entries;
  ctx->hdr->cq_ring_mask = ctx->cq_mask;
  ctx->hdr->cq_ring_entries = cq_entries;
  /* Without IORING_SETUP_NO_SQARRAY the indirection array is real, and until
   * userspace fills it the identity mapping is what liburing assumes it may
   * leave alone. liburing writes it; filling it here makes a hand-written
   * submitter that does not work too. */
  if (ctx->sq_array)
    for (u32 i = 0; i < sq_entries; i++)
      ctx->sq_array[i] = i;

  struct vfs_node *node = vfs_create_node(VFS_DEVICE);

  if (!node) {
    iou_free_region(ctx->ring_phys, ctx->ring_pages);
    iou_free_region(ctx->sqes_phys, ctx->sqes_pages);
    kfree(ctx);
    return -ENOMEM;
  }
  strncpy(node->name, "io_uring", sizeof(node->name) - 1);
  node->name[sizeof(node->name) - 1] = 0;
  node->deleted = 1;
  node->inode->nlink = 0;
  node->inode->mode = 0600;
  {
    const struct cred *cred = scheduler_get_current_cred();

    node->inode->uid = cred ? cred->euid : ROOT_UID;
    node->inode->gid = cred ? cred->egid : ROOT_GID;
  }
  node->inode->mmap_handle_page_phys_cb = iou_mmap_page_phys;
  node->inode->release_cb = iou_inode_release;
  ctx->node = node;
  iou_ctx_get(ctx); /* the inode's reference */

  iou_list_lock();
  ctx->next = g_ctx_list;
  g_ctx_list = ctx;
  iou_list_unlock();

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_NODE);

  if (!h) {
    vfs_node_put(node); /* takes the inode reference with it */
    iou_ctx_put(ctx);
    return -ENFILE;
  }
  h->node = node;
  h->private_data = ctx;
  h->ops = &iou_file_ops;
  h->flags = B1NIX_O_RDWR;

  int fd = scheduler_fd_alloc(h);

  if (fd < 0) {
    vfs_handle_release(h);
    return fd == -ENOMEM ? -ENOMEM : -EMFILE;
  }
  /* io_uring_setup(2) returns a close-on-exec descriptor. */
  scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);

  if (p.flags & IORING_SETUP_SQPOLL) {
    /* sq_thread_idle is in milliseconds, as the ABI says; Linux's default when
     * the field is left at zero is one second. */
    ctx->sq_idle_ms = p.sq_thread_idle ? p.sq_thread_idle : 1000u;
    iou_ctx_get(ctx); /* the thread's reference */
    ctx->sq_tid = kthread_create("io_uring-sq", iou_sq_thread, ctx);
    if (ctx->sq_tid < 0) {
      iou_ctx_put(ctx);
      scheduler_fd_close(fd);
      return -EAGAIN;
    }
    /* The thread adopts the owner's address space before it does anything; if
     * that fails it clears sq_alive and stops, and the ring would then accept
     * submissions nothing ever consumes. Wait for the answer here, where it
     * can still be reported. */
    while (!__atomic_load_n(&ctx->sq_alive, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&ctx->sq_stop, __ATOMIC_ACQUIRE))
      scheduler_yield();
    if (!__atomic_load_n(&ctx->sq_alive, __ATOMIC_ACQUIRE)) {
      scheduler_fd_close(fd);
      return -EOWNERDEAD;
    }
  }

  p.sq_entries = sq_entries;
  p.cq_entries = cq_entries;
  p.features = IOU_FEATURES;
  p.sq_off.head = IOU_OFF_SQ_HEAD;
  p.sq_off.tail = IOU_OFF_SQ_TAIL;
  p.sq_off.ring_mask = IOU_OFF_SQ_MASK;
  p.sq_off.ring_entries = IOU_OFF_SQ_ENTRIES;
  p.sq_off.flags = IOU_OFF_SQ_FLAGS;
  p.sq_off.dropped = IOU_OFF_SQ_DROPPED;
  p.sq_off.array = IOU_OFF_SQ_ARRAY;
  p.sq_off.resv1 = 0;
  p.sq_off.user_addr = 0;
  p.cq_off.head = IOU_OFF_CQ_HEAD;
  p.cq_off.tail = IOU_OFF_CQ_TAIL;
  p.cq_off.ring_mask = IOU_OFF_CQ_MASK;
  p.cq_off.ring_entries = IOU_OFF_CQ_ENTRIES;
  p.cq_off.overflow = IOU_OFF_CQ_OVERFLOW;
  p.cq_off.cqes = cqes_off;
  p.cq_off.flags = IOU_OFF_CQ_FLAGS;
  p.cq_off.resv1 = 0;
  p.cq_off.user_addr = 0;

  if (syscall_copyout((void *)(usize)uparams, &p, sizeof(p)) < 0) {
    scheduler_fd_close(fd);
    return -EFAULT;
  }
  iou_trace("setup", entries, p.flags, fd);
  return fd;
}

/* ---- the ring from a descriptor ---------------------------------------- */

/* A descriptor that is not a ring and a descriptor that is not open are two
 * different answers: EOPNOTSUPP for the first, EBADF for the second. liburing's
 * io_uring_enter test checks both, and a kernel that says EBADF to a valid
 * descriptor is telling a program its file is gone. */
static struct io_ring_ctx *iou_ctx_from_fd(int fd, int *err) {
  struct vfs_handle *h = scheduler_fd_get(fd);

  if (!h) {
    *err = -EBADF;
    return 0;
  }
  if (h->ops != &iou_file_ops || !h->private_data) {
    *err = -EOPNOTSUPP;
    return 0;
  }
  *err = 0;
  return (struct io_ring_ctx *)h->private_data;
}

/* ---- io_uring_enter ----------------------------------------------------- */

static isize iou_enter(int fd, u32 to_submit, u32 min_complete, u32 flags,
                       u64 argp, usize argsz) {
  int ctxerr = 0;
  struct io_ring_ctx *ctx = iou_ctx_from_fd(fd, &ctxerr);
  u64 deadline = 0;
  int have_deadline = 0;
  isize submitted = 0;

  if (!ctx)
    return ctxerr;
  iou_trace("enter-in", to_submit, min_complete, (isize)flags);
  if (flags & ~(u32)(IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG |
                     IORING_ENTER_ABS_TIMER | IORING_ENTER_NO_IOWAIT |
                     IORING_ENTER_SQ_WAKEUP | IORING_ENTER_SQ_WAIT))
    return -EINVAL;
  if ((flags & (IORING_ENTER_SQ_WAKEUP | IORING_ENTER_SQ_WAIT)) &&
      !(ctx->flags & IORING_SETUP_SQPOLL))
    return -EINVAL;
  if (!ctx->enabled)
    return -EBADFD;

  if (flags & IORING_ENTER_EXT_ARG) {
    struct iou_getevents_arg arg;

    if (argsz != sizeof(arg))
      return -EINVAL;
    if (syscall_copyin(&arg, (const void *)(usize)argp, sizeof(arg)) < 0)
      return -EFAULT;
    if (arg.sigmask)
      return -EINVAL; /* no signal mask swap here; say so rather than ignore */
    if (arg.ts) {
      u64 ns = 0;

      if (iou_timespec_in(arg.ts, &ns) < 0)
        return -EFAULT;
      deadline = (flags & IORING_ENTER_ABS_TIMER) ? ns
                                                  : ktime_monotonic_ns() + ns;
      have_deadline = 1;
    }
  } else if (argp && argsz) {
    /* The old form passes a sigset_t. Refuse rather than silently ignore a
     * mask the caller is relying on. */
    return -EINVAL;
  }

  if (ctx->flags & IORING_SETUP_SQPOLL) {
    /* The submission thread owns the queue. What io_uring_enter does for an
     * SQPOLL ring is wake that thread and, with IORING_ENTER_SQ_WAIT, wait
     * until it has made room; the entries themselves are already published, so
     * they count as submitted. */
    if (!__atomic_load_n(&ctx->sq_alive, __ATOMIC_ACQUIRE))
      return -EOWNERDEAD;
    if (flags & IORING_ENTER_SQ_WAKEUP)
      scheduler_wake_all(&ctx->sq_wait);
    else if (__atomic_load_n(&ctx->hdr->sq_flags, __ATOMIC_ACQUIRE) &
             IORING_SQ_NEED_WAKEUP)
      scheduler_wake_all(&ctx->sq_wait);
    submitted = (isize)to_submit;
    if (flags & IORING_ENTER_SQ_WAIT) {
      u64 give_up = ktime_monotonic_ns() + 10ull * 1000000000ull;

      while (ktime_monotonic_ns() < give_up) {
        u32 tail = __atomic_load_n(&ctx->hdr->sq_tail, __ATOMIC_ACQUIRE);
        u32 head = __atomic_load_n(&ctx->hdr->sq_head, __ATOMIC_ACQUIRE);

        if (tail - head < ctx->sq_entries)
          break;
        if (scheduler_signal_pending())
          return -EINTR;
        scheduler_wake_all(&ctx->sq_wait);
        scheduler_yield();
      }
    }
  } else {
    iou_progress(ctx);

    if (to_submit) {
      int rc = iou_submit_sqes(ctx, to_submit);

      if (rc < 0)
        return rc;
      submitted = rc;
    }
  }

  if (flags & IORING_ENTER_GETEVENTS) {
    isize rc = iou_wait_cqes(ctx, min_complete, have_deadline ? &deadline : 0);

    if (rc == -EINTR && submitted == 0)
      return -EINTR;
    if (rc == -ETIME && submitted == 0 && min_complete)
      return -ETIME;
  } else {
    iou_progress(ctx);
  }

  iou_trace("enter", to_submit, min_complete, submitted);
  return submitted;
}

/* ---- io_uring_register -------------------------------------------------- */

/* A ring cannot hold itself. Registering one io_uring descriptor in another's
 * fixed-file set (or its own) makes a reference that nothing ever drops, so the
 * rings and every request they hold outlive the process. Linux refuses it for
 * the same reason, and liburing's test_partial_register_fail checks that a set
 * containing one is refused WHOLE — no descriptor left referenced. */

static isize iou_register_files(struct io_ring_ctx *ctx, u64 uaddr, u32 nr) {
  i32 *fds;

  if (ctx->files)
    return -EBUSY;
  if (nr == 0)
    return -EINVAL;
  /* Too many is EMFILE, as on Linux: a caller that asks for a set this kernel
   * cannot hold is told the set is too big, not that its arguments are wrong,
   * and liburing's huge-set test skips on exactly that distinction. */
  if (nr > IOU_MAX_FIXED_FILES)
    return -EMFILE;
  fds = kmalloc(sizeof(i32) * nr);
  if (!fds)
    return -ENOMEM;
  if (syscall_copyin(fds, (const void *)(usize)uaddr, sizeof(i32) * nr) < 0) {
    kfree(fds);
    return -EFAULT;
  }
  ctx->files = kzalloc(sizeof(struct iou_fixed_file) * nr);
  if (!ctx->files) {
    kfree(fds);
    return -ENOMEM;
  }
  ctx->nr_files = nr;
  for (u32 i = 0; i < nr; i++) {
    /* -1 is an empty slot, which the ABI allows and callers use to reserve
     * room. Any OTHER negative number is a descriptor that does not exist, and
     * the whole registration fails — treating every negative as a hole let a
     * set half full of -2 register successfully. */
    if (fds[i] == -1)
      continue;
    ctx->files[i].file = fds[i] < 0 ? 0 : scheduler_fd_get_retain(fds[i]);
    if (ctx->files[i].file && iou_is_ring_handle(ctx->files[i].file)) {
      vfs_handle_release(ctx->files[i].file);
      ctx->files[i].file = 0;
    }
    if (!ctx->files[i].file) {
      for (u32 j = 0; j < i; j++)
        if (ctx->files[j].file)
          vfs_handle_release(ctx->files[j].file);
      kfree(ctx->files);
      ctx->files = 0;
      ctx->nr_files = 0;
      kfree(fds);
      return -EBADF;
    }
  }
  kfree(fds);
  return 0;
}

/* IORING_REGISTER_FILES2: the same set, with a count and flags in a descriptor
 * of its own, plus the sparse form liburing's io_uring_register_files_sparse
 * uses to reserve slots it fills later. */
static isize iou_register_files2(struct io_ring_ctx *ctx, u64 uaddr,
                                 u32 nr_args) {
  struct io_uring_rsrc_register rr;

  if (nr_args != sizeof(rr))
    return -EINVAL;
  if (syscall_copyin(&rr, (const void *)(usize)uaddr, sizeof(rr)) < 0)
    return -EFAULT;
  if (rr.resv2)
    return -EINVAL;
  if (rr.flags & ~(u32)IORING_RSRC_REGISTER_SPARSE)
    return -EINVAL;
  if (rr.flags & IORING_RSRC_REGISTER_SPARSE) {
    /* Every slot empty. data must be absent: a sparse set names no files. */
    if (rr.data)
      return -EINVAL;
    if (ctx->files)
      return -EBUSY;
    if (rr.nr == 0)
      return -EINVAL;
    if (rr.nr > IOU_MAX_FIXED_FILES)
      return -EMFILE;
    ctx->files = kzalloc(sizeof(struct iou_fixed_file) * rr.nr);
    if (!ctx->files)
      return -ENOMEM;
    ctx->nr_files = rr.nr;
    return 0;
  }
  /* Tags are the resource-tag feature (IORING_FEAT_RSRC_TAGS), which is not
   * advertised, so a caller asking for them is asking for something that would
   * silently do nothing. */
  if (rr.tags)
    return -EINVAL;
  return iou_register_files(ctx, rr.data, rr.nr);
}

/* IORING_REGISTER_FILES_UPDATE: replace a run of slots. Returns how many were
 * updated, which is what the ABI says and what liburing checks. */
static isize iou_files_update(struct io_ring_ctx *ctx, u32 off, u64 uaddr,
                              u32 nr) {
  i32 *fds;
  u32 done = 0;

  if (!ctx->files)
    return -ENXIO;
  if (nr == 0)
    return -EINVAL;
  if (off > ctx->nr_files || nr > ctx->nr_files - off)
    return -EINVAL;
  fds = kmalloc(sizeof(i32) * nr);
  if (!fds)
    return -ENOMEM;
  if (syscall_copyin(fds, (const void *)(usize)uaddr, sizeof(i32) * nr) < 0) {
    kfree(fds);
    return -EFAULT;
  }
  for (u32 i = 0; i < nr; i++) {
    struct vfs_handle *nh = 0;

    if (fds[i] >= 0) {
      nh = scheduler_fd_get_retain(fds[i]);
      if (nh && iou_is_ring_handle(nh)) {
        vfs_handle_release(nh);
        nh = 0;
      }
      if (!nh) {
        kfree(fds);
        return done ? (isize)done : -EBADF;
      }
    } else if (fds[i] != -1) {
      /* Anything other than -1 (clear the slot) is not a descriptor. */
      kfree(fds);
      return done ? (isize)done : -EBADF;
    }
    if (ctx->files[off + i].file)
      vfs_handle_release(ctx->files[off + i].file);
    ctx->files[off + i].file = nh;
    done++;
  }
  kfree(fds);
  return (isize)done;
}

static isize iou_files_update2(struct io_ring_ctx *ctx, u64 uaddr,
                               u32 nr_args) {
  struct io_uring_rsrc_update2 up;

  if (nr_args != sizeof(up))
    return -EINVAL;
  if (syscall_copyin(&up, (const void *)(usize)uaddr, sizeof(up)) < 0)
    return -EFAULT;
  if (up.resv || up.resv2 || up.tags)
    return -EINVAL;
  return iou_files_update(ctx, up.offset, up.data, up.nr);
}

static isize iou_unregister_files(struct io_ring_ctx *ctx) {
  if (!ctx->files)
    return -ENXIO;
  for (u32 i = 0; i < ctx->nr_files; i++)
    if (ctx->files[i].file)
      vfs_handle_release(ctx->files[i].file);
  kfree(ctx->files);
  ctx->files = 0;
  ctx->nr_files = 0;
  return 0;
}

static isize iou_register_buffers(struct io_ring_ctx *ctx, u64 uaddr, u32 nr) {
  struct iou_iovec *iov;

  if (ctx->bufs)
    return -EBUSY;
  if (nr == 0 || nr > IOU_MAX_FIXED_BUFS)
    return -EINVAL;
  iov = kmalloc(sizeof(*iov) * nr);
  if (!iov)
    return -ENOMEM;
  if (syscall_copyin(iov, (const void *)(usize)uaddr, sizeof(*iov) * nr) < 0) {
    kfree(iov);
    return -EFAULT;
  }
  ctx->bufs = kzalloc(sizeof(struct iou_fixed_buf) * nr);
  if (!ctx->bufs) {
    kfree(iov);
    return -ENOMEM;
  }
  ctx->nr_bufs = nr;
  for (u32 i = 0; i < nr; i++) {
    /* The range has to be one this process can actually reach; a registration
     * that accepts a bad pointer only moves the -EFAULT to a stranger place. */
    if (iov[i].len) {
      char probe;

      if (syscall_copyin(&probe, (const void *)(usize)iov[i].base, 1) < 0 ||
          syscall_copyin(&probe,
                         (const void *)(usize)(iov[i].base + iov[i].len - 1),
                         1) < 0) {
        kfree(ctx->bufs);
        ctx->bufs = 0;
        ctx->nr_bufs = 0;
        kfree(iov);
        return -EFAULT;
      }
    }
    ctx->bufs[i].addr = iov[i].base;
    ctx->bufs[i].len = iov[i].len;
  }
  kfree(iov);
  return 0;
}

/* IORING_REGISTER_BUFFERS2: the same set as IORING_REGISTER_BUFFERS, described
 * by a struct of its own, plus the sparse form. */
static isize iou_register_buffers2(struct io_ring_ctx *ctx, u64 uaddr,
                                   u32 nr_args) {
  struct io_uring_rsrc_register rr;

  if (nr_args != sizeof(rr))
    return -EINVAL;
  if (syscall_copyin(&rr, (const void *)(usize)uaddr, sizeof(rr)) < 0)
    return -EFAULT;
  if (rr.resv2 || rr.tags)
    return -EINVAL;
  if (rr.flags & ~(u32)IORING_RSRC_REGISTER_SPARSE)
    return -EINVAL;
  if (rr.flags & IORING_RSRC_REGISTER_SPARSE) {
    if (rr.data)
      return -EINVAL;
    if (ctx->bufs)
      return -EBUSY;
    if (rr.nr == 0 || rr.nr > IOU_MAX_FIXED_BUFS)
      return -EINVAL;
    ctx->bufs = kzalloc(sizeof(struct iou_fixed_buf) * rr.nr);
    if (!ctx->bufs)
      return -ENOMEM;
    ctx->nr_bufs = rr.nr;
    return 0;
  }
  return iou_register_buffers(ctx, rr.data, rr.nr);
}

/* IORING_REGISTER_BUFFERS_UPDATE: replace a run of registered buffers. */
static isize iou_buffers_update(struct io_ring_ctx *ctx, u64 uaddr,
                                u32 nr_args) {
  struct io_uring_rsrc_update2 up;
  struct iou_iovec *iov;

  if (nr_args != sizeof(up))
    return -EINVAL;
  if (syscall_copyin(&up, (const void *)(usize)uaddr, sizeof(up)) < 0)
    return -EFAULT;
  if (up.resv || up.resv2 || up.tags)
    return -EINVAL;
  if (!ctx->bufs)
    return -ENXIO;
  if (up.nr == 0)
    return -EINVAL;
  if (up.offset > ctx->nr_bufs || up.nr > ctx->nr_bufs - up.offset)
    return -EINVAL;
  iov = kmalloc(sizeof(*iov) * up.nr);
  if (!iov)
    return -ENOMEM;
  if (syscall_copyin(iov, (const void *)(usize)up.data,
                     sizeof(*iov) * up.nr) < 0) {
    kfree(iov);
    return -EFAULT;
  }
  for (u32 i = 0; i < up.nr; i++) {
    if (iov[i].len) {
      char probe;

      if (syscall_copyin(&probe, (const void *)(usize)iov[i].base, 1) < 0 ||
          syscall_copyin(&probe,
                         (const void *)(usize)(iov[i].base + iov[i].len - 1),
                         1) < 0) {
        kfree(iov);
        return -EFAULT;
      }
    }
    ctx->bufs[up.offset + i].addr = iov[i].base;
    ctx->bufs[up.offset + i].len = iov[i].len;
  }
  kfree(iov);
  return (isize)up.nr;
}

/* IORING_REGISTER_PBUF_RING: a buffer ring the program owns and writes. */
static isize iou_register_pbuf_ring(struct io_ring_ctx *ctx, u64 uaddr,
                                    u32 nr_args) {
  struct io_uring_buf_reg reg;
  struct iou_bgroup *g;

  if (nr_args != 1)
    return -EINVAL;
  if (syscall_copyin(&reg, (const void *)(usize)uaddr, sizeof(reg)) < 0)
    return -EFAULT;
  for (int i = 0; i < 5; i++)
    if (reg.resv[i])
      return -EINVAL;
  /* IOU_PBUF_RING_MMAP would have the kernel allocate the ring and the program
   * map it at IORING_OFF_PBUF_RING; the ring here is the program's memory, so
   * the flag is refused rather than quietly ignored. IOU_PBUF_RING_INC, the
   * partial-consumption form, is refused for the same reason: half-using a
   * buffer needs a per-buffer cursor this does not keep. */
  if (reg.flags)
    return -EINVAL;
  if (!reg.ring_addr || (reg.ring_addr & 7u))
    return -EINVAL;
  if (!reg.ring_entries || (reg.ring_entries & (reg.ring_entries - 1)) ||
      reg.ring_entries > 0x8000u)
    return -EINVAL;
  {
    /* The ring has to be readable now; otherwise every selection from it would
     * report EFAULT from inside an unrelated request. */
    char probe;

    if (syscall_copyin(&probe, (const void *)(usize)reg.ring_addr, 1) < 0 ||
        syscall_copyin(&probe,
                       (const void *)(usize)(reg.ring_addr +
                                             (u64)reg.ring_entries * 16u - 1),
                       1) < 0)
      return -EFAULT;
  }

  iou_lock(ctx);
  if (iou_bgroup_find(ctx, reg.bgid)) {
    iou_unlock(ctx);
    return -EEXIST;
  }
  g = iou_bgroup_get(ctx, reg.bgid);
  if (!g) {
    iou_unlock(ctx);
    return -ENOMEM;
  }
  g->is_ring = 1;
  g->ring_addr = reg.ring_addr;
  g->ring_entries = reg.ring_entries;
  g->ring_mask = (u16)(reg.ring_entries - 1);
  g->ring_head = 0;
  iou_unlock(ctx);
  return 0;
}

static isize iou_unregister_pbuf_ring(struct io_ring_ctx *ctx, u64 uaddr,
                                      u32 nr_args) {
  struct io_uring_buf_reg reg;
  struct iou_bgroup **pp;
  isize rc = -EINVAL;

  if (nr_args != 1)
    return -EINVAL;
  if (syscall_copyin(&reg, (const void *)(usize)uaddr, sizeof(reg)) < 0)
    return -EFAULT;
  iou_lock(ctx);
  pp = &ctx->bgroups;
  while (*pp) {
    if ((*pp)->bgid == reg.bgid && (*pp)->is_ring) {
      struct iou_bgroup *g = *pp;

      *pp = g->next;
      kfree(g->list);
      kfree(g);
      rc = 0;
      break;
    }
    pp = &(*pp)->next;
  }
  iou_unlock(ctx);
  return rc;
}

static isize iou_pbuf_status(struct io_ring_ctx *ctx, u64 uaddr, u32 nr_args) {
  struct io_uring_buf_status st;
  struct iou_bgroup *g;

  if (nr_args != 1)
    return -EINVAL;
  if (syscall_copyin(&st, (const void *)(usize)uaddr, sizeof(st)) < 0)
    return -EFAULT;
  for (int i = 0; i < 8; i++)
    if (st.resv[i])
      return -EINVAL;
  iou_lock(ctx);
  g = iou_bgroup_find(ctx, (u16)st.buf_group);
  if (!g || !g->is_ring) {
    iou_unlock(ctx);
    return -ENOENT;
  }
  st.head = g->ring_head;
  iou_unlock(ctx);
  if (syscall_copyout((void *)(usize)uaddr, &st, sizeof(st)) < 0)
    return -EFAULT;
  return 0;
}

/* IORING_REGISTER_RESTRICTIONS: the list of things this ring will be allowed
 * to do once IORING_REGISTER_ENABLE_RINGS starts it. Only meaningful on a ring
 * created with IORING_SETUP_R_DISABLED, and only before it is enabled. */
static isize iou_register_restrictions(struct io_ring_ctx *ctx, u64 uaddr,
                                       u32 nr_args) {
  struct io_uring_restriction *r;
  isize rc = 0;

  if (ctx->enabled)
    return -EBADFD;
  if (ctx->restricted)
    return -EBUSY;
  if (nr_args == 0 || nr_args > 256)
    return -EINVAL;
  r = kzalloc(sizeof(*r) * nr_args);
  if (!r)
    return -ENOMEM;
  if (syscall_copyin(r, (const void *)(usize)uaddr, sizeof(*r) * nr_args) < 0) {
    kfree(r);
    return -EFAULT;
  }
  /* Nothing is allowed until something says so; that is what a restriction
   * list means. The flags allowed default to none as well, so a list that
   * names no flag restriction still permits a plain SQE. */
  memset(ctx->restr_register, 0, sizeof(ctx->restr_register));
  memset(ctx->restr_sqe, 0, sizeof(ctx->restr_sqe));
  ctx->restr_flags_allowed = 0;
  ctx->restr_flags_required = 0;
  for (u32 i = 0; i < nr_args; i++) {
    if (r[i].resv || r[i].resv2[0] || r[i].resv2[1] || r[i].resv2[2]) {
      rc = -EINVAL;
      break;
    }
    switch (r[i].opcode) {
    case IORING_RESTRICTION_REGISTER_OP:
      if (r[i].register_op >= IORING_REGISTER_LAST) {
        rc = -EINVAL;
        break;
      }
      ctx->restr_register[r[i].register_op] = 1;
      break;
    case IORING_RESTRICTION_SQE_OP:
      if (r[i].sqe_op >= IORING_OP_LAST) {
        rc = -EINVAL;
        break;
      }
      ctx->restr_sqe[r[i].sqe_op] = 1;
      break;
    case IORING_RESTRICTION_SQE_FLAGS_ALLOWED:
      ctx->restr_flags_allowed |= r[i].sqe_flags;
      break;
    case IORING_RESTRICTION_SQE_FLAGS_REQUIRED:
      ctx->restr_flags_required |= r[i].sqe_flags;
      break;
    default:
      rc = -EINVAL;
      break;
    }
    if (rc)
      break;
  }
  kfree(r);
  if (rc) {
    memset(ctx->restr_register, 0, sizeof(ctx->restr_register));
    memset(ctx->restr_sqe, 0, sizeof(ctx->restr_sqe));
    ctx->restr_flags_allowed = 0;
    ctx->restr_flags_required = 0;
    return rc;
  }
  /* A required flag is necessarily an allowed one. */
  ctx->restr_flags_allowed |= ctx->restr_flags_required;
  ctx->restricted = 1;
  return 0;
}

/* IORING_REGISTER_SYNC_CANCEL: cancel from the register path, with a deadline,
 * rather than by submitting IORING_OP_ASYNC_CANCEL. */
static isize iou_sync_cancel(struct io_ring_ctx *ctx, u64 uaddr, u32 nr_args) {
  struct io_uring_sync_cancel_reg reg;
  u64 deadline = 0;
  int have_deadline = 0;
  int total = 0;

  if (nr_args != 1)
    return -EINVAL;
  if (syscall_copyin(&reg, (const void *)(usize)uaddr, sizeof(reg)) < 0)
    return -EFAULT;
  for (int i = 0; i < 7; i++)
    if (reg.pad[i])
      return -EINVAL;
  for (int i = 0; i < 3; i++)
    if (reg.pad2[i])
      return -EINVAL;
  if (reg.flags & ~(u32)(IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_FD |
                         IORING_ASYNC_CANCEL_ANY |
                         IORING_ASYNC_CANCEL_FD_FIXED |
                         IORING_ASYNC_CANCEL_USERDATA))
    return -EINVAL;
  if (reg.timeout.tv_sec != -1 || reg.timeout.tv_nsec != -1) {
    if (reg.timeout.tv_nsec < 0 || reg.timeout.tv_nsec >= 1000000000ll ||
        reg.timeout.tv_sec < 0)
      return -EINVAL;
    deadline = ktime_monotonic_ns() + (u64)reg.timeout.tv_sec * 1000000000ull +
               (u64)reg.timeout.tv_nsec;
    have_deadline = 1;
  }
  for (;;) {
    int n = iou_cancel_by(ctx, reg.addr,
                          (reg.flags & IORING_ASYNC_CANCEL_FD) ? 1 : 0,
                          reg.fd, reg.flags);

    total += n;
    if (total)
      break;
    if (!have_deadline)
      return -ENOENT;
    if (ktime_monotonic_ns() >= deadline)
      return -ETIME;
    if (scheduler_signal_pending())
      return -EINTR;
    /* Wait for something to become cancellable, the way the ABI's timeout
     * promises: a request that has not been submitted yet may still arrive. */
    scheduler_wait_prepare_timeout(vfs_poll_chan, 1);
    scheduler_wait_commit();
  }
  return 0;
}

static isize iou_register_probe(struct io_ring_ctx *ctx, u64 uaddr,
                                u32 nr_args) {
  struct io_uring_probe probe;
  u32 n = nr_args;

  (void)ctx;
  /* nr_args is how many ops[] entries the caller has room for, and zero is a
   * legitimate question — "what is the last opcode?" — whose answer must leave
   * ops_len at zero. Filling it in regardless is how liburing's probe test
   * fails before it has asked anything. */
  if (n > IORING_OP_LAST)
    n = IORING_OP_LAST;
  if (syscall_copyin(&probe, (const void *)(usize)uaddr, sizeof(probe)) < 0)
    return -EFAULT;
  memset(&probe, 0, sizeof(probe));
  probe.last_op = IORING_OP_LAST - 1;
  probe.ops_len = (u8)n;

  for (u32 op = 0; op < n; op++) {
    struct io_uring_probe_op po;
    int supported;

    memset(&po, 0, sizeof(po));
    po.op = (u8)op;
    switch (op) {
    case IORING_OP_NOP:
    case IORING_OP_READV:
    case IORING_OP_WRITEV:
    case IORING_OP_FSYNC:
    case IORING_OP_READ_FIXED:
    case IORING_OP_WRITE_FIXED:
    case IORING_OP_POLL_ADD:
    case IORING_OP_POLL_REMOVE:
    case IORING_OP_TIMEOUT:
    case IORING_OP_TIMEOUT_REMOVE:
    case IORING_OP_ACCEPT:
    case IORING_OP_ASYNC_CANCEL:
    case IORING_OP_LINK_TIMEOUT:
    case IORING_OP_CONNECT:
    case IORING_OP_CLOSE:
    case IORING_OP_READ:
    case IORING_OP_WRITE:
    case IORING_OP_SEND:
    case IORING_OP_RECV:
    case IORING_OP_FILES_UPDATE:
    case IORING_OP_SYNC_FILE_RANGE:
    case IORING_OP_SENDMSG:
    case IORING_OP_RECVMSG:
    case IORING_OP_FALLOCATE:
    case IORING_OP_OPENAT:
    case IORING_OP_OPENAT2:
    case IORING_OP_STATX:
    case IORING_OP_FADVISE:
    case IORING_OP_MADVISE:
    case IORING_OP_EPOLL_CTL:
    case IORING_OP_SPLICE:
    case IORING_OP_PROVIDE_BUFFERS:
    case IORING_OP_REMOVE_BUFFERS:
    case IORING_OP_TEE:
    case IORING_OP_SHUTDOWN:
    case IORING_OP_RENAMEAT:
    case IORING_OP_UNLINKAT:
    case IORING_OP_MKDIRAT:
    case IORING_OP_SYMLINKAT:
    case IORING_OP_LINKAT:
    case IORING_OP_MSG_RING:
    case IORING_OP_SOCKET:
    case IORING_OP_SEND_ZC:
    case IORING_OP_SENDMSG_ZC:
    case IORING_OP_READ_MULTISHOT:
    case IORING_OP_WAITID:
    case IORING_OP_FIXED_FD_INSTALL:
    case IORING_OP_FTRUNCATE:
    case IORING_OP_BIND:
    case IORING_OP_LISTEN:
    case IORING_OP_PIPE:
      supported = 1;
      break;
    /* Not implemented, and the probe is the place that says so:
     * the extended-attribute four (no xattrs in this VFS), URING_CMD (no
     * driver takes a passthrough command), the three futex opcodes,
     * RECV_ZC/EPOLL_WAIT/READV_FIXED/WRITEV_FIXED. */
    default:
      supported = 0;
      break;
    }
    po.flags = supported ? IO_URING_OP_SUPPORTED : 0;
    if (syscall_copyout((void *)(usize)(uaddr + sizeof(probe) +
                                        (u64)op * sizeof(po)),
                        &po, sizeof(po)) < 0)
      return -EFAULT;
  }
  if (syscall_copyout((void *)(usize)uaddr, &probe, sizeof(probe)) < 0)
    return -EFAULT;
  return 0;
}

static isize iou_register(int fd, u32 opcode, u64 arg, u32 nr_args) {
  int ctxerr = 0;
  struct io_ring_ctx *ctx = iou_ctx_from_fd(fd, &ctxerr);
  isize rc;

  if (!ctx)
    return ctxerr;
  /* IORING_REGISTER_USE_REGISTERED_RING would take a registered ring index
   * instead of a descriptor; the registration it needs is not implemented, and
   * IORING_FEAT_REG_REG_RING is not advertised, so refuse the bit outright. */
  if (opcode & IORING_REGISTER_USE_REGISTERED_RING)
    return -EINVAL;
  /* Every operation but ENABLE_RINGS needs the ring to be running. */
  if (!ctx->enabled && opcode != IORING_REGISTER_ENABLE_RINGS &&
      opcode != IORING_REGISTER_RESTRICTIONS &&
      opcode != IORING_REGISTER_FILES && opcode != IORING_REGISTER_BUFFERS)
    return -EBADFD;
  /* A restricted ring only answers the register opcodes its restriction list
   * named. */
  if (ctx->restricted && ctx->enabled && opcode < IORING_REGISTER_LAST &&
      !ctx->restr_register[opcode])
    return -EACCES;

  switch (opcode) {
  case IORING_REGISTER_BUFFERS:
    rc = iou_register_buffers(ctx, arg, nr_args);
    break;
  case IORING_UNREGISTER_BUFFERS:
    if (!ctx->bufs)
      return -ENXIO;
    kfree(ctx->bufs);
    ctx->bufs = 0;
    ctx->nr_bufs = 0;
    rc = 0;
    break;
  case IORING_REGISTER_FILES:
    rc = iou_register_files(ctx, arg, nr_args);
    break;
  case IORING_REGISTER_FILES2:
    rc = iou_register_files2(ctx, arg, nr_args);
    break;
  case IORING_UNREGISTER_FILES:
    rc = iou_unregister_files(ctx);
    break;
  case IORING_REGISTER_FILES_UPDATE: {
    struct io_uring_rsrc_update up;

    if (syscall_copyin(&up, (const void *)(usize)arg, sizeof(up)) < 0)
      return -EFAULT;
    if (up.resv)
      return -EINVAL;
    rc = iou_files_update(ctx, up.offset, up.data, nr_args);
    break;
  }
  case IORING_REGISTER_FILES_UPDATE2:
    rc = iou_files_update2(ctx, arg, nr_args);
    break;
  case IORING_REGISTER_BUFFERS2:
    rc = iou_register_buffers2(ctx, arg, nr_args);
    break;
  case IORING_REGISTER_BUFFERS_UPDATE:
    rc = iou_buffers_update(ctx, arg, nr_args);
    break;
  case IORING_REGISTER_PBUF_RING:
    rc = iou_register_pbuf_ring(ctx, arg, nr_args);
    break;
  case IORING_UNREGISTER_PBUF_RING:
    rc = iou_unregister_pbuf_ring(ctx, arg, nr_args);
    break;
  case IORING_REGISTER_PBUF_STATUS:
    rc = iou_pbuf_status(ctx, arg, nr_args);
    break;
  case IORING_REGISTER_RESTRICTIONS:
    rc = iou_register_restrictions(ctx, arg, nr_args);
    break;
  case IORING_REGISTER_SYNC_CANCEL:
    rc = iou_sync_cancel(ctx, arg, nr_args);
    break;
  case IORING_REGISTER_FILE_ALLOC_RANGE: {
    struct io_uring_file_index_range range;

    if (nr_args)
      return -EINVAL;
    if (syscall_copyin(&range, (const void *)(usize)arg, sizeof(range)) < 0)
      return -EFAULT;
    if (range.resv)
      return -EINVAL;
    if (!ctx->files)
      return -ENXIO;
    if (range.off > ctx->nr_files || range.len > ctx->nr_files - range.off)
      return -EINVAL;
    ctx->falloc_off = range.off;
    ctx->falloc_len = range.len;
    rc = 0;
    break;
  }
  case IORING_REGISTER_IOWQ_MAX_WORKERS: {
    /* There is no io-wq: a request that would block is armed on its file's
     * readiness and retried from io_uring_enter (M70), so the number of
     * "workers" is not a thing this kernel has to tune. The ABI's contract is
     * that the call reports the PREVIOUS values and sets the new ones, and a
     * program uses it to cap concurrency. Both values are kept and reported
     * back truthfully; what they do not do is create threads, and the probe
     * and this comment are where that is said. */
    u32 vals[2];

    if (nr_args != 2)
      return -EINVAL;
    if (syscall_copyin(vals, (const void *)(usize)arg, sizeof(vals)) < 0)
      return -EFAULT;
    {
      u32 prev[2] = {ctx->iowq_max[0], ctx->iowq_max[1]};

      if (vals[0])
        ctx->iowq_max[0] = vals[0];
      if (vals[1])
        ctx->iowq_max[1] = vals[1];
      if (syscall_copyout((void *)(usize)arg, prev, sizeof(prev)) < 0)
        return -EFAULT;
    }
    rc = 0;
    break;
  }
  case IORING_REGISTER_EVENTFD:
  case IORING_REGISTER_EVENTFD_ASYNC: {
    i32 efd;

    if (ctx->cq_eventfd)
      return -EBUSY;
    if (nr_args != 1)
      return -EINVAL;
    if (syscall_copyin(&efd, (const void *)(usize)arg, sizeof(efd)) < 0)
      return -EFAULT;
    struct vfs_handle *h = scheduler_fd_get_retain(efd);

    if (!h)
      return -EBADF;
    if (h->kind != VFS_HANDLE_EVENTFD) {
      vfs_handle_release(h);
      return -EINVAL;
    }
    ctx->cq_eventfd = h;
    ctx->eventfd_async = (opcode == IORING_REGISTER_EVENTFD_ASYNC) ? 1 : 0;
    rc = 0;
    break;
  }
  case IORING_UNREGISTER_EVENTFD:
    if (!ctx->cq_eventfd)
      return -ENXIO;
    vfs_handle_release(ctx->cq_eventfd);
    ctx->cq_eventfd = 0;
    ctx->eventfd_async = 0;
    rc = 0;
    break;
  case IORING_REGISTER_PROBE:
    rc = iou_register_probe(ctx, arg, nr_args);
    break;
  case IORING_REGISTER_ENABLE_RINGS:
    if (ctx->enabled)
      return -EBADFD;
    ctx->enabled = 1;
    rc = 0;
    break;
  default:
    /* Personalities (this kernel runs every request as the submitter, so a
     * stored credential set would have nothing to switch to), registered ring
     * descriptors, io-wq affinity, NAPI busy-poll, the clock selection, buffer
     * cloning, ring resizing, memory regions and the query interface: none of
     * them exist here. -EINVAL is what a kernel without the opcode answers,
     * which is what liburing tests for, and IORING_REGISTER_PROBE plus the
     * absent feature bits say so in advance. */
    rc = -EINVAL;
    break;
  }
  iou_trace("register", opcode, nr_args, rc);
  return rc;
}

/* ---- the dispatcher hook ------------------------------------------------ */

/* 425-427 come from the shared range every architecture has used since 424, so
 * there is no per-architecture table here. */
#define IOU_NR_setup    425
#define IOU_NR_enter    426
#define IOU_NR_register 427

int io_uring_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5,
                     u64 *ret) {
  isize r;

  switch (nr) {
  case IOU_NR_setup:
    r = iou_setup((u32)a0, a1);
    break;
  case IOU_NR_enter:
    r = iou_enter((int)a0, (u32)a1, (u32)a2, (u32)a3, a4, (usize)a5);
    break;
  case IOU_NR_register:
    r = iou_register((int)a0, (u32)a1, a2, (u32)a3);
    break;
  default:
    return 0;
  }
  *ret = (u64)r;
  return 1;
}
