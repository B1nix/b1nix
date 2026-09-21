/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * userfaultfd(2) — the fault handler is a program.
 *
 * See <b1nix/userfaultfd.h> for what this is and who wants it. The shape here
 * is the one the ABI forces:
 *
 *   The FAULTING thread, deep in the page-fault handler, finds that the
 *   address belongs to a registered range. It posts a message, then sleeps on
 *   the context until somebody says the page is there. It holds no VM lock
 *   while it sleeps -- it cannot, because the thread that will wake it has to
 *   take those locks to install the page.
 *
 *   The MONITOR thread reads the message from the descriptor, works out what
 *   the page should contain, and calls UFFDIO_COPY or UFFDIO_ZEROPAGE. Those
 *   install the page into the faulting address space and wake everyone waiting
 *   on the context; each waiter re-checks whether ITS page arrived and either
 *   returns to retry the access or goes back to sleep.
 *
 * Two details are worth stating because they are easy to get wrong:
 *
 *   A fault that finds no monitor listening must not hang. If the descriptor
 *   is closed while a thread waits on it, or the monitor dies, the waiters are
 *   released and the fault falls through to the ordinary zero-fill -- a
 *   program whose monitor has gone gets its memory, not a freeze.
 *
 *   The monitor's source buffer is read in the MONITOR's address space and the
 *   page is installed in the FAULTER's, and those are usually but not always
 *   the same. The copy therefore goes through a freshly allocated frame
 *   addressed through the direct map, never through the faulter's mapping.
 */
#include <b1nix/userfaultfd.h>

#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <b1nix/tlb.h>
#include <b1nix/user.h>
#include <b1nix/uidgid.h>
#include <b1nix/vfs.h>

#include <string.h>

/* userfaultfd(2): 323 on x86_64, 282 on aarch64. */
#if defined(__aarch64__)
#define UFFD_NR_open 282
#else
#define UFFD_NR_open 323
#endif

/* Ranges one context may watch, and messages it may hold before a fault has to
 * wait for the monitor to catch up. A monitor that does not read is a monitor
 * that has stopped working, and the queue is bounded so that it cannot take
 * the machine's memory with it. */
/* The frame bits of a leaf page-table entry: the physical address, with the
 * flags above and below it masked off. */
#define UFFD_PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

#define UFFD_MAX_RANGES 32
#define UFFD_MAX_MSGS 64

struct uffd_range {
  u64 start, end; /* page aligned, end exclusive */
  u64 mode;       /* UFFDIO_REGISTER_MODE_* */
  int used;
};

struct uffd_ctx {
  /* The address space this context watches. A monitor in another process is
   * allowed, which is why the pml4 is remembered rather than assumed. */
  u64 pml4_phys;
  usize owner_tgid;

  u64 features; /* agreed at UFFDIO_API */
  int api_done;
  int nonblock;
  int dead; /* the descriptor is closing: release every waiter */

  struct uffd_range ranges[UFFD_MAX_RANGES];

  struct uffd_msg msgs[UFFD_MAX_MSGS];
  u32 msg_head, msg_tail; /* head == tail: empty */

  /* References: the descriptor holds one, and every thread that is inside a
   * fault on this context holds one. The last one out frees it -- a faulter
   * sleeping on a context whose descriptor is being closed would otherwise
   * wake up inside freed memory, and "wait a bit and hope" is not a lifetime
   * rule. */
  int refs;

  spinlock_t lock;
  /* Two channels: one the monitor sleeps on waiting for a message, one the
   * faulters sleep on waiting for their page. */
  char msg_chan;
  char fault_chan;
  int waiters;
};

/* Every live context, so a fault can find the one that covers its address
 * without walking descriptor tables. */
#define UFFD_MAX_CTX 64
static struct uffd_ctx *g_ctx[UFFD_MAX_CTX];
static spinlock_t g_ctx_lock = SPINLOCK_INIT;

static void uffd_ctx_link(struct uffd_ctx *c) {
  u64 flags;

  spin_lock_irqsave(&g_ctx_lock, &flags);
  for (int i = 0; i < UFFD_MAX_CTX; i++) {
    if (!g_ctx[i]) {
      g_ctx[i] = c;
      break;
    }
  }
  spin_unlock_irqrestore(&g_ctx_lock, flags);
}

static void uffd_ctx_unlink(struct uffd_ctx *c) {
  u64 flags;

  spin_lock_irqsave(&g_ctx_lock, &flags);
  for (int i = 0; i < UFFD_MAX_CTX; i++)
    if (g_ctx[i] == c)
      g_ctx[i] = 0;
  spin_unlock_irqrestore(&g_ctx_lock, flags);
}

/* Drop a reference. The caller must not touch the context afterwards. */
static void uffd_put(struct uffd_ctx *c) {
  if (!c)
    return;
  if (__atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL) == 0)
    kfree(c);
}

/* ── ranges ──────────────────────────────────────────────────────────────── */

/* The registration covering `addr`, or 0. Caller holds the context lock. */
static struct uffd_range *uffd_find(struct uffd_ctx *c, u64 addr) {
  for (int i = 0; i < UFFD_MAX_RANGES; i++) {
    if (c->ranges[i].used && addr >= c->ranges[i].start &&
        addr < c->ranges[i].end)
      return &c->ranges[i];
  }
  return 0;
}

static int uffd_add_range(struct uffd_ctx *c, u64 start, u64 end, u64 mode) {
  u64 flags;
  int rc = -ENOMEM;

  spin_lock_irqsave(&c->lock, &flags);
  /* An existing registration of the same range just changes mode, which is
   * what a monitor enabling write-protect on a range it already watches does. */
  for (int i = 0; i < UFFD_MAX_RANGES; i++) {
    if (c->ranges[i].used && c->ranges[i].start == start &&
        c->ranges[i].end == end) {
      c->ranges[i].mode = mode;
      rc = 0;
      goto out;
    }
  }
  for (int i = 0; i < UFFD_MAX_RANGES; i++) {
    if (!c->ranges[i].used) {
      c->ranges[i].start = start;
      c->ranges[i].end = end;
      c->ranges[i].mode = mode;
      c->ranges[i].used = 1;
      rc = 0;
      goto out;
    }
  }
out:
  spin_unlock_irqrestore(&c->lock, flags);
  return rc;
}

static int uffd_del_range(struct uffd_ctx *c, u64 start, u64 end) {
  u64 flags;
  int found = 0;

  spin_lock_irqsave(&c->lock, &flags);
  for (int i = 0; i < UFFD_MAX_RANGES; i++) {
    if (!c->ranges[i].used)
      continue;
    /* Unregistering a part of a registration leaves the rest registered; the
     * common cases -- all of it, or its head or tail -- are handled exactly,
     * and a hole punched in the middle takes a second slot. */
    u64 rs = c->ranges[i].start, re = c->ranges[i].end;

    if (end <= rs || start >= re)
      continue;
    found = 1;
    if (start <= rs && end >= re) {
      c->ranges[i].used = 0;
    } else if (start <= rs) {
      c->ranges[i].start = end;
    } else if (end >= re) {
      c->ranges[i].end = start;
    } else {
      u64 mode = c->ranges[i].mode;

      c->ranges[i].end = start;
      for (int j = 0; j < UFFD_MAX_RANGES; j++) {
        if (!c->ranges[j].used) {
          c->ranges[j].start = end;
          c->ranges[j].end = re;
          c->ranges[j].mode = mode;
          c->ranges[j].used = 1;
          break;
        }
      }
    }
  }
  spin_unlock_irqrestore(&c->lock, flags);
  return found ? 0 : -ENOENT;
}

/* ── the message queue ───────────────────────────────────────────────────── */

/* Caller holds the context lock. Returns 0, or -1 when the queue is full --
 * which means the monitor has stopped reading. */
static int uffd_post(struct uffd_ctx *c, const struct uffd_msg *m) {
  u32 next = (c->msg_tail + 1) % UFFD_MAX_MSGS;

  if (next == c->msg_head)
    return -1;
  c->msgs[c->msg_tail] = *m;
  c->msg_tail = next;
  return 0;
}

static int uffd_pop(struct uffd_ctx *c, struct uffd_msg *out) {
  if (c->msg_head == c->msg_tail)
    return -1;
  *out = c->msgs[c->msg_head];
  c->msg_head = (c->msg_head + 1) % UFFD_MAX_MSGS;
  return 0;
}

/* ── the fault path ──────────────────────────────────────────────────────── */

/* Has the page arrived? Asked by a waiter each time it is woken, because a
 * wake is broadcast to every waiter on the context and only one of them is
 * usually the one whose page was filled. */
static int uffd_page_present(u64 pml4, u64 addr, int for_write) {
  if (!paging_user_frame(pml4, addr))
    return 0;
  /* For a write, the page must also be writable again -- which is what lifting
   * a write-protection means, and what the waiting store is waiting for. The
   * question goes to the arch: the bit that carries the answer is not the same
   * one on both. */
  if (for_write && !paging_user_writable(pml4, addr))
    return 0;
  return 1;
}

int uffd_handle_fault(u64 fault_addr, int write, int present) {
  struct task *cur = current_task;

  if (!cur || !cur->pml4_phys)
    return 0;

  u64 addr = fault_addr & ~(u64)(PAGE_SIZE - 1);
  struct uffd_ctx *c = 0;
  struct uffd_range *r = 0;
  u64 flags;
  u64 want_mode = present ? UFFDIO_REGISTER_MODE_WP
                          : UFFDIO_REGISTER_MODE_MISSING;

  /* A write-protect fault only belongs to a monitor if the range asked for
   * write-protect notifications; a read fault on a present page is not a
   * userfaultfd event at all. */
  if (present && !write)
    return 0;

  spin_lock_irqsave(&g_ctx_lock, &flags);
  for (int i = 0; i < UFFD_MAX_CTX && !c; i++) {
    struct uffd_ctx *cand = g_ctx[i];

    if (!cand || cand->dead || cand->pml4_phys != cur->pml4_phys)
      continue;
    r = uffd_find(cand, addr);
    if (r && (r->mode & want_mode)) {
      c = cand;
      /* Taken under the registry lock, which is what stops the descriptor's
       * close from freeing the context between finding it and using it. */
      __atomic_add_fetch(&c->refs, 1, __ATOMIC_ACQ_REL);
    }
  }
  spin_unlock_irqrestore(&g_ctx_lock, flags);
  if (!c)
    return 0;

  struct uffd_msg m;

  memset(&m, 0, sizeof(m));
  m.event = UFFD_EVENT_PAGEFAULT;
  m.arg.pagefault.address = addr;
  if (write)
    m.arg.pagefault.flags |= UFFD_PAGEFAULT_FLAG_WRITE;
  if (present)
    m.arg.pagefault.flags |= UFFD_PAGEFAULT_FLAG_WP;
  if (c->features & UFFD_FEATURE_THREAD_ID)
    m.arg.pagefault.feat.ptid = (u32)cur->id;

  spin_lock_irqsave(&c->lock, &flags);
  int posted = uffd_post(c, &m);

  c->waiters++;
  spin_unlock_irqrestore(&c->lock, flags);
  if (posted != 0) {
    /* The monitor is not keeping up. Servicing the fault ourselves is the
     * honest failure: the program gets a zero page instead of the contents the
     * monitor would have supplied, which is wrong for a CRIU restore but is
     * what Linux does too when the queue overflows -- it delivers SIGBUS. This
     * kernel prefers the fall-through, and says so here rather than hanging. */
    spin_lock_irqsave(&c->lock, &flags);
    c->waiters--;
    spin_unlock_irqrestore(&c->lock, flags);
    uffd_put(c);
    return 0;
  }
  scheduler_wake_all(&c->msg_chan);

  /* Wait for the page. Each wake is a broadcast, so the condition is checked
   * again every time; the timeout is a safety net for a monitor that dies
   * without closing its descriptor, not part of the protocol. */
  for (int spins = 0; spins < 10000; spins++) {
    if (c->dead)
      break;
    if (uffd_page_present(cur->pml4_phys, addr, write))
      break;
    if (cur->pending_signals)
      break; /* a signal is due: let the fault restart from the top */
    scheduler_wait_prepare_timeout(&c->fault_chan, 10);
    if (uffd_page_present(cur->pml4_phys, addr, write)) {
      scheduler_wait_cancel();
      break;
    }
    scheduler_wait_commit();
  }

  spin_lock_irqsave(&c->lock, &flags);
  c->waiters--;
  spin_unlock_irqrestore(&c->lock, flags);

  /* Whether or not the monitor answered, the fault is retried: if the page is
   * there the access simply succeeds, and if it is not the ordinary path fills
   * it in. Returning 1 says "do not service this here". */
  int served = uffd_page_present(cur->pml4_phys, addr, write);

  uffd_put(c);
  return served;
}

void uffd_task_exit(struct task *t) {
  u64 flags;

  if (!t)
    return;
  /* The PROCESS dying is what releases the waiters, not a thread of it: the
   * monitor is usually a thread, and the thread that answers the faults
   * finishing its work must not take the descriptor down with it -- that made
   * an idle descriptor report POLLERR and read(2) return end-of-file to the
   * program that was still using it. */
  if (task_tgid(t) != t->id)
    return;
  spin_lock_irqsave(&g_ctx_lock, &flags);
  for (int i = 0; i < UFFD_MAX_CTX; i++) {
    struct uffd_ctx *c = g_ctx[i];

    if (c && c->owner_tgid == task_tgid(t)) {
      c->dead = 1;
      scheduler_wake_all(&c->fault_chan);
      scheduler_wake_all(&c->msg_chan);
    }
  }
  spin_unlock_irqrestore(&g_ctx_lock, flags);
}

/* ── the descriptor ──────────────────────────────────────────────────────── */

static isize uffd_read(struct vfs_handle *h, char *buf, usize len) {
  struct uffd_ctx *c = h ? (struct uffd_ctx *)h->private_data : 0;
  struct uffd_msg m;
  u64 flags;

  if (!c)
    return -EBADF;
  if (!c->api_done)
    return -EINVAL; /* Linux: read before UFFDIO_API is EINVAL */
  if (len < sizeof(m))
    return -EINVAL;

  for (;;) {
    spin_lock_irqsave(&c->lock, &flags);
    int got = uffd_pop(c, &m);

    spin_unlock_irqrestore(&c->lock, flags);
    if (got == 0)
      break;
    if (c->dead)
      return 0;
    if (c->nonblock || (h->flags & B1NIX_O_NONBLOCK))
      return -EAGAIN;
    if (current_task && current_task->pending_signals)
      return -EINTR;
    scheduler_wait_prepare_timeout(&c->msg_chan, 10);
    spin_lock_irqsave(&c->lock, &flags);
    int now = (c->msg_head != c->msg_tail);

    spin_unlock_irqrestore(&c->lock, flags);
    if (now)
      scheduler_wait_cancel();
    else
      scheduler_wait_commit();
  }
  memcpy(buf, &m, sizeof(m));
  return (isize)sizeof(m);
}

static int uffd_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct uffd_ctx *c = h ? (struct uffd_ctx *)h->private_data : 0;
  u64 flags;

  pfd->revents = 0;
  if (!c)
    return -EBADF;
  spin_lock_irqsave(&c->lock, &flags);
  if (c->msg_head != c->msg_tail)
    pfd->revents |= B1NIX_POLLIN;
  spin_unlock_irqrestore(&c->lock, flags);
  if (c->dead)
    pfd->revents |= B1NIX_POLLERR;
  return 0;
}

/* Install one page into the watched address space. `src` is read from the
 * CALLER's address space; `dst` is a page in the context's. */
static int uffd_install(struct uffd_ctx *c, u64 dst, const void *src,
                        int write_protect) {
  u64 frame = pmm_alloc_frame();

  if (!frame)
    return -ENOMEM;

  u64 direct = vmm_direct_map_base();
  void *kpage = (void *)(usize)(frame + direct);

  if (src) {
    if (syscall_copyin(kpage, src, PAGE_SIZE) < 0) {
      pmm_free_frame(frame);
      return -EFAULT;
    }
  } else {
    memset(kpage, 0, PAGE_SIZE);
  }

  u64 pflags = VMM_PRESENT | VMM_USER;

  if (!write_protect)
    pflags |= VMM_WRITABLE;
  paging_set_page_in_space(c->pml4_phys, dst, frame, pflags);
  /* Another CPU may still have the absent entry cached from the fault it took
   * a moment ago. */
  tlb_shootdown_page(dst);
  return 0;
}

/* Change the writability of a range already mapped in the watched space.
 *
 * Through the arch helper, not by editing the leaf here: "writable" is one bit
 * on x86_64 and a two-bit AP field on aarch64, and a caller that clears bit 1
 * of an aarch64 descriptor has cleared the bit that says it is a page. */
static int uffd_writeprotect(struct uffd_ctx *c, u64 start, u64 end, int on) {
  for (u64 a = start; a < end; a += PAGE_SIZE) {
    if (paging_set_writable_in_space(c->pml4_phys, a, !on))
      tlb_shootdown_page(a);
  }
  return 0;
}

static int uffd_range_ok(u64 start, u64 len, u64 *end_out) {
  if (!len || (start & (PAGE_SIZE - 1)) || (len & (PAGE_SIZE - 1)))
    return -EINVAL;
  if (start + len < start || start + len > USER_SPACE_LIMIT)
    return -EINVAL;
  *end_out = start + len;
  return 0;
}

static int uffd_ioctl(struct vfs_handle *h, u64 cmd, void *argp) {
  unsigned long arg = (unsigned long)(usize)argp;
  struct uffd_ctx *c = h ? (struct uffd_ctx *)h->private_data : 0;

  if (!c)
    return -EBADF;

  switch (cmd) {
  case UFFDIO_API: {
    struct uffdio_api api;

    if (syscall_copyin(&api, (const void *)(usize)arg, sizeof(api)) < 0)
      return -EFAULT;
    if (api.api != UFFD_API || c->api_done)
      return -EINVAL;
    if (api.features & ~(u64)UFFD_API_FEATURES) {
      /* Linux answers an unsupported feature request by reporting what IS
       * available and failing, so the caller can ask again for less. */
      api.features = UFFD_API_FEATURES;
      api.ioctls = UFFD_API_RANGE_IOCTLS;
      (void)syscall_copyout((void *)(usize)arg, &api, sizeof(api));
      return -EINVAL;
    }
    c->features = api.features;
    c->api_done = 1;
    api.features = UFFD_API_FEATURES;
    api.ioctls = (1ull << _UFFDIO_WAKE) | (1ull << _UFFDIO_COPY) |
                 (1ull << _UFFDIO_ZEROPAGE) | (1ull << _UFFDIO_WRITEPROTECT);
    return syscall_copyout((void *)(usize)arg, &api, sizeof(api)) == 0 ? 0
                                                                       : -EFAULT;
  }
  case UFFDIO_REGISTER: {
    struct uffdio_register reg;
    u64 end;

    if (!c->api_done)
      return -EINVAL;
    if (syscall_copyin(&reg, (const void *)(usize)arg, sizeof(reg)) < 0)
      return -EFAULT;
    if (uffd_range_ok(reg.range.start, reg.range.len, &end) < 0)
      return -EINVAL;
    if (!reg.mode ||
        (reg.mode & ~(u64)(UFFDIO_REGISTER_MODE_MISSING |
                           UFFDIO_REGISTER_MODE_WP)))
      /* MINOR needs a page cache behind the mapping to have something to show
       * the monitor; refused rather than accepted and never delivered. */
      return -EINVAL;
    {
      /* The range must be mapped memory of this address space, or a fault in
       * it would never reach the handler that consults us. */
      struct task *t = current_task;

      if (!t || t->pml4_phys != c->pml4_phys)
        return -EINVAL;
    }
    int rc = uffd_add_range(c, reg.range.start, end, reg.mode);

    if (rc < 0)
      return rc;
    reg.ioctls = (1ull << _UFFDIO_WAKE) | (1ull << _UFFDIO_COPY) |
                 (1ull << _UFFDIO_ZEROPAGE);
    if (reg.mode & UFFDIO_REGISTER_MODE_WP)
      reg.ioctls |= 1ull << _UFFDIO_WRITEPROTECT;
    return syscall_copyout((void *)(usize)arg, &reg, sizeof(reg)) == 0
               ? 0
               : -EFAULT;
  }
  case UFFDIO_UNREGISTER: {
    struct uffdio_range range;
    u64 end;

    if (syscall_copyin(&range, (const void *)(usize)arg, sizeof(range)) < 0)
      return -EFAULT;
    if (uffd_range_ok(range.start, range.len, &end) < 0)
      return -EINVAL;
    int rc = uffd_del_range(c, range.start, end);

    /* Whoever was waiting in that range is now waiting for a monitor that no
     * longer watches it. */
    scheduler_wake_all(&c->fault_chan);
    return rc;
  }
  case UFFDIO_WAKE: {
    struct uffdio_range range;
    u64 end;

    if (syscall_copyin(&range, (const void *)(usize)arg, sizeof(range)) < 0)
      return -EFAULT;
    if (uffd_range_ok(range.start, range.len, &end) < 0)
      return -EINVAL;
    scheduler_wake_all(&c->fault_chan);
    return 0;
  }
  case UFFDIO_COPY: {
    struct uffdio_copy cp;
    u64 end;

    if (syscall_copyin(&cp, (const void *)(usize)arg, sizeof(cp)) < 0)
      return -EFAULT;
    if (uffd_range_ok(cp.dst, cp.len, &end) < 0)
      return -EINVAL;
    if (cp.mode & ~(u64)(UFFDIO_COPY_MODE_DONTWAKE | UFFDIO_COPY_MODE_WP))
      return -EINVAL;

    i64 done = 0;
    int rc = 0;

    for (u64 a = cp.dst, s = cp.src; a < end; a += PAGE_SIZE, s += PAGE_SIZE) {
      rc = uffd_install(c, a, (const void *)(usize)s,
                        (cp.mode & UFFDIO_COPY_MODE_WP) != 0);
      if (rc < 0)
        break;
      done += (i64)PAGE_SIZE;
    }
    cp.copy = done ? done : (i64)rc;
    (void)syscall_copyout((void *)(usize)arg, &cp, sizeof(cp));
    if (!(cp.mode & UFFDIO_COPY_MODE_DONTWAKE))
      scheduler_wake_all(&c->fault_chan);
    if (!done)
      return rc;
    return 0;
  }
  case UFFDIO_ZEROPAGE: {
    struct uffdio_zeropage zp;
    u64 end;

    if (syscall_copyin(&zp, (const void *)(usize)arg, sizeof(zp)) < 0)
      return -EFAULT;
    if (uffd_range_ok(zp.range.start, zp.range.len, &end) < 0)
      return -EINVAL;

    i64 done = 0;
    int rc = 0;

    for (u64 a = zp.range.start; a < end; a += PAGE_SIZE) {
      rc = uffd_install(c, a, 0, 0);
      if (rc < 0)
        break;
      done += (i64)PAGE_SIZE;
    }
    zp.zeropage = done ? done : (i64)rc;
    (void)syscall_copyout((void *)(usize)arg, &zp, sizeof(zp));
    if (!(zp.mode & UFFDIO_ZEROPAGE_MODE_DONTWAKE))
      scheduler_wake_all(&c->fault_chan);
    if (!done)
      return rc;
    return 0;
  }
  case UFFDIO_WRITEPROTECT: {
    struct uffdio_writeprotect wp;
    u64 end;

    if (syscall_copyin(&wp, (const void *)(usize)arg, sizeof(wp)) < 0)
      return -EFAULT;
    if (uffd_range_ok(wp.range.start, wp.range.len, &end) < 0)
      return -EINVAL;
    if (wp.mode & ~(u64)(UFFDIO_WRITEPROTECT_MODE_WP |
                         UFFDIO_WRITEPROTECT_MODE_DONTWAKE))
      return -EINVAL;
    uffd_writeprotect(c, wp.range.start, end,
                      (wp.mode & UFFDIO_WRITEPROTECT_MODE_WP) != 0);
    if (!(wp.mode & UFFDIO_WRITEPROTECT_MODE_DONTWAKE))
      scheduler_wake_all(&c->fault_chan);
    return 0;
  }
  default:
    return -EINVAL;
  }
}

static void uffd_release(struct vfs_handle *h) {
  struct uffd_ctx *c = h ? (struct uffd_ctx *)h->private_data : 0;

  if (!c)
    return;
  h->private_data = 0;
  c->dead = 1;
  /* Release the faulters before the context goes: their fault then falls
   * through to the ordinary zero-fill rather than waiting for a monitor that
   * has closed its descriptor. */
  scheduler_wake_all(&c->fault_chan);
  scheduler_wake_all(&c->msg_chan);
  uffd_ctx_unlink(c);
  if (h->node) {
    vfs_node_put(h->node);
    h->node = 0;
  }
  /* The descriptor's own reference. A faulter still inside the context keeps
   * it alive until it is finished with it. */
  uffd_put(c);
}

static const struct vfs_file_ops uffd_file_ops = {
    .read = uffd_read,
    .poll = uffd_poll,
    .ioctl = uffd_ioctl,
    .release = uffd_release,
};

static isize uffd_create(u64 flags) {
  struct task *cur = current_task;

  if (flags & ~(u64)(UFFD_CLOEXEC | UFFD_NONBLOCK | UFFD_USER_MODE_ONLY))
    return -EINVAL;
  if (!cur || !cur->pml4_phys)
    return -EINVAL;

  struct uffd_ctx *c = kzalloc(sizeof(*c));

  if (!c)
    return -ENOMEM;
  c->pml4_phys = cur->pml4_phys;
  c->owner_tgid = task_tgid(cur);
  c->refs = 1; /* the descriptor's */
  c->nonblock = (flags & UFFD_NONBLOCK) != 0;
  c->lock = SPINLOCK_INIT;

  struct vfs_node *node = vfs_create_node(VFS_DEVICE);

  if (!node) {
    kfree(c);
    return -ENOMEM;
  }
  strncpy(node->name, "userfaultfd", sizeof(node->name) - 1);
  node->name[sizeof(node->name) - 1] = 0;
  node->deleted = 1;
  node->inode->nlink = 0;
  node->inode->mode = 0600;

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_NODE);

  if (!h) {
    vfs_node_put(node);
    kfree(c);
    return -ENFILE;
  }
  h->node = node;
  h->private_data = c;
  h->ops = &uffd_file_ops;
  h->flags = B1NIX_O_RDWR | (c->nonblock ? B1NIX_O_NONBLOCK : 0);

  uffd_ctx_link(c);

  int fd = scheduler_fd_alloc(h);

  if (fd < 0) {
    uffd_ctx_unlink(c);
    vfs_handle_release(h);
    uffd_put(c);
    return fd;
  }
  if (flags & UFFD_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

int userfaultfd_syscall(u64 nr, u64 a0, u64 *ret) {
  if (nr != UFFD_NR_open)
    return 0;
  *ret = (u64)uffd_create(a0);
  return 1;
}
