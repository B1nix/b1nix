/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * System calls Linux added after the table b1nix's ABI layer was built from:
 * memory policy, protection keys, sched_setattr, kcmp, pidfd_getfd,
 * process_madvise/process_mrelease, cachestat, futex2 and openat2.
 *
 * Each answers the way Linux answers on a machine of this shape -- one NUMA
 * node, protection keys only where the CPU has them, one scheduling class --
 * rather than with ENOSYS, and each is exercised by a probe that checks the
 * result.
 */
#include <b1nix/errno.h>
#include <b1nix/io_uring.h>
#include <b1nix/ktime.h>
#include <b1nix/landlock.h>
#include <b1nix/mm.h>
#include <b1nix/page_cache.h>
#include <b1nix/pkeys.h>
#include <b1nix/posix.h>
#include <b1nix/ptrace.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/types.h>
#include <b1nix/uidgid.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <string.h>

#include "linux_modern.h"

/* ── numbers ─────────────────────────────────────────────────────── */

/* Everything from 424 up is shared by every architecture; the older calls
 * carry x86_64's own number there and asm-generic's here. */
#if defined(__aarch64__)
#define LM_remap_file_pages 234
#define LM_quotactl         60
#define LM_mbind            235
#define LM_get_mempolicy    236
#define LM_set_mempolicy    237
#define LM_kcmp             272
#define LM_sched_setattr    274
#define LM_sched_getattr    275
#define LM_pkey_mprotect    288
#define LM_pkey_alloc       289
#define LM_pkey_free        290
#else
#define LM_remap_file_pages 216
#define LM_quotactl         179
#define LM_mbind            237
#define LM_set_mempolicy    238
#define LM_get_mempolicy    239
#define LM_kcmp             312
#define LM_sched_setattr    314
#define LM_sched_getattr    315
#define LM_pkey_mprotect    329
#define LM_pkey_alloc       330
#define LM_pkey_free        331
#endif
#define LM_openat2          437
#define LM_quotactl_fd      443
#define LM_pidfd_getfd      438
#define LM_process_madvise  440
#define LM_memfd_secret     447
#define LM_process_mrelease 448
#define LM_futex_waitv      449
#define LM_set_mempolicy_home_node 450
#define LM_cachestat        451
#define LM_futex_wake       454
#define LM_futex_wait       455
#define LM_statmount        457
#define LM_listmount        458

#define LM_O_CLOEXEC 02000000

/* ── per-task state ──────────────────────────────────────────────── */

/* The task policy, kept beside the task (no fields in struct task) and
 * inherited across fork like Linux's. */
struct lm_task_state {
  u16 mempolicy_mode;
  u16 mempolicy_flags;
};
/* Sized from the task table on first use; a task that never touched a policy
 * reads as MPOL_DEFAULT either way. */
static struct lm_task_state *g_lm;
static usize g_lm_slots;

static struct lm_task_state *lm_slot(usize slot, int create) {
  if (!g_lm && create) {
    usize n = scheduler_max_task_slots();
    struct lm_task_state *t = kzalloc(n * sizeof(*t));

    if (t && !__atomic_compare_exchange_n(&g_lm, &(struct lm_task_state *){0},
                                          t, 0, __ATOMIC_ACQUIRE,
                                          __ATOMIC_RELAXED))
      kfree(t);
    else if (t)
      g_lm_slots = n;
  }
  return (g_lm && slot < g_lm_slots) ? &g_lm[slot] : 0;
}

void linux_modern_task_reset(usize slot) {
  struct lm_task_state *s = lm_slot(slot, 0);

  linux_keys_task_reset(slot);
  landlock_task_reset(slot);
  if (s)
    memset(s, 0, sizeof(*s));
}

void linux_modern_fork_inherit(usize parent_slot, usize child_slot) {
  struct lm_task_state *p = lm_slot(parent_slot, 0);
  struct lm_task_state *c = lm_slot(child_slot, 0);

  linux_keys_fork_inherit(parent_slot, child_slot);
  landlock_fork(parent_slot, child_slot);
  if (p && c)
    *c = *p;
}

static struct lm_task_state *lm_cur(void) {
  return lm_slot(scheduler_task_index(current_task), 1);
}

/* ── memory policy ───────────────────────────────────────────────── */

#define MPOL_DEFAULT        0
#define MPOL_PREFERRED      1
#define MPOL_BIND           2
#define MPOL_INTERLEAVE     3
#define MPOL_LOCAL          4
#define MPOL_PREFERRED_MANY 5
#define MPOL_WEIGHTED_INTERLEAVE 6
#define MPOL_MAX            7
#define MPOL_F_NUMA_BALANCING (1 << 13)
#define MPOL_F_RELATIVE_NODES (1 << 14)
#define MPOL_F_STATIC_NODES   (1 << 15)
#define MPOL_MODE_FLAGS \
  (MPOL_F_NUMA_BALANCING | MPOL_F_RELATIVE_NODES | MPOL_F_STATIC_NODES)
#define MPOL_F_NODE         (1 << 0)
#define MPOL_F_ADDR         (1 << 1)
#define MPOL_F_MEMS_ALLOWED (1 << 2)
#define MPOL_MF_STRICT      (1 << 0)
#define MPOL_MF_MOVE        (1 << 1)
#define MPOL_MF_MOVE_ALL    (1 << 2)
#define MPOL_MF_LAZY        (1 << 3)

/* Linux's MAX_NUMNODES is a build choice; the ABI bound on maxnode is a page
 * of bits. */
#define LM_MAXNODE_LIMIT (PAGE_SIZE * 8)

/* Read a user node mask of `maxnode` bits (Linux drops the last bit) and
 * report whether it is empty and whether it names any node but 0. */
static int lm_read_nodemask(u64 uptr, u64 maxnode, int *empty, int *beyond) {
  *empty = 1;
  *beyond = 0;
  if (!uptr || maxnode == 0)
    return 0;
  maxnode--;
  if (maxnode > LM_MAXNODE_LIMIT)
    return -EINVAL;
  u64 words = (maxnode + 63) / 64;
  for (u64 w = 0; w < words; w++) {
    u64 v = 0;

    if (syscall_copyin(&v, (const void *)(usize)(uptr + w * 8), 8) != 0)
      return -EFAULT;
    if (w == words - 1 && maxnode % 64)
      v &= (1ULL << (maxnode % 64)) - 1;
    if (v)
      *empty = 0;
    if (w == 0 ? (v & ~1ULL) : v)
      *beyond = 1;
  }
  return 0;
}

static int lm_check_policy(u64 mode_word, u64 nmask, u64 maxnode) {
  u32 mode = (u32)mode_word & ~(u32)MPOL_MODE_FLAGS;
  u32 mflags = (u32)mode_word & (u32)MPOL_MODE_FLAGS;
  int empty, beyond;

  if (mode >= MPOL_MAX || (u32)(mode_word >> 32))
    return -EINVAL;
  if ((mflags & MPOL_F_STATIC_NODES) && (mflags & MPOL_F_RELATIVE_NODES))
    return -EINVAL;
  if ((mflags & MPOL_F_NUMA_BALANCING) && mode != MPOL_BIND)
    return -EINVAL;
  int rc = lm_read_nodemask(nmask, maxnode, &empty, &beyond);
  if (rc)
    return rc;
  /* A node this machine does not have. */
  if (beyond)
    return -EINVAL;
  switch (mode) {
  case MPOL_DEFAULT:
  case MPOL_LOCAL:
    if (!empty || mflags)
      return -EINVAL;
    break;
  case MPOL_PREFERRED:
    break; /* an empty mask means local allocation */
  default:
    if (empty)
      return -EINVAL;
  }
  return (int)mode;
}

static int lm_range_mapped(u64 start, u64 len) {
  u64 end = start + len;
  u64 flags;
  int ok = 1;

  vma_list_lock(&flags);
  for (u64 a = start; a < end;) {
    struct vm_area *v = vma_lookup(current_task, a);

    if (!v) {
      ok = 0;
      break;
    }
    a = v->end;
  }
  vma_list_unlock(flags);
  return ok;
}

static isize lm_mbind(u64 start, u64 len, u64 mode, u64 nmask, u64 maxnode,
                      u64 flags) {
  if (start & (PAGE_SIZE - 1))
    return -EINVAL;
  if (flags & ~(u64)(MPOL_MF_STRICT | MPOL_MF_MOVE | MPOL_MF_MOVE_ALL))
    return -EINVAL;
  if ((flags & MPOL_MF_MOVE_ALL) &&
      !cred_has_cap(current_task->cred, CAP_SYS_NICE))
    return -EPERM;
  int m = lm_check_policy(mode, nmask, maxnode);
  if (m < 0)
    return m;
  len = (len + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
  if (start + len < start || start + len > USER_SPACE_LIMIT)
    return -EINVAL;
  if (len == 0)
    return 0;
  if (!lm_range_mapped(start, len))
    return -EFAULT;
  /* One node: every page already lives where any valid policy asks, so there
   * is nothing to move and nothing that could violate MPOL_MF_STRICT. */
  return 0;
}

static isize lm_set_mempolicy(u64 mode, u64 nmask, u64 maxnode) {
  int m = lm_check_policy(mode, nmask, maxnode);
  struct lm_task_state *s = lm_cur();

  if (m < 0)
    return m;
  if (s) {
    s->mempolicy_mode = (u16)m;
    s->mempolicy_flags = (u16)(mode & MPOL_MODE_FLAGS);
  }
  return 0;
}

static isize lm_write_mask(u64 uptr, u64 maxnode, int node0) {
  if (!uptr)
    return 0;
  if (maxnode == 0)
    return -EINVAL;
  u64 words = (maxnode + 63) / 64;
  if (maxnode > LM_MAXNODE_LIMIT)
    return -EINVAL;
  for (u64 w = 0; w < words; w++) {
    u64 v = (w == 0 && node0) ? 1 : 0;

    if (syscall_copyout((void *)(usize)(uptr + w * 8), &v, 8) != 0)
      return -EFAULT;
  }
  return 0;
}

static isize lm_get_mempolicy(u64 umode, u64 nmask, u64 maxnode, u64 addr,
                              u64 flags) {
  struct lm_task_state *s = lm_cur();
  int mode;

  if (flags & ~(u64)(MPOL_F_NODE | MPOL_F_ADDR | MPOL_F_MEMS_ALLOWED))
    return -EINVAL;
  if (nmask && maxnode < 1)
    return -EINVAL;
  if (flags & MPOL_F_MEMS_ALLOWED) {
    if (flags & (MPOL_F_NODE | MPOL_F_ADDR))
      return -EINVAL;
    if (umode) {
      int zero = 0;

      if (syscall_copyout((void *)(usize)umode, &zero, sizeof(zero)))
        return -EFAULT;
    }
    return lm_write_mask(nmask, maxnode, 1);
  }
  if (!(flags & MPOL_F_ADDR) && addr)
    return -EINVAL;
  if (flags & MPOL_F_ADDR) {
    if (!lm_range_mapped(addr & ~(u64)(PAGE_SIZE - 1), PAGE_SIZE))
      return -EFAULT;
  }
  if (flags & MPOL_F_NODE) {
    /* The node the page (or the next allocation) is on: node 0. */
    mode = 0;
  } else {
    mode = s ? (int)(s->mempolicy_mode | s->mempolicy_flags) : 0;
  }
  if (umode && syscall_copyout((void *)(usize)umode, &mode, sizeof(mode)))
    return -EFAULT;
  {
    int m = s ? s->mempolicy_mode : 0;
    int node0 = (m == MPOL_BIND || m == MPOL_INTERLEAVE ||
                 m == MPOL_PREFERRED_MANY || m == MPOL_WEIGHTED_INTERLEAVE ||
                 m == MPOL_PREFERRED) && !(flags & MPOL_F_NODE);
    return lm_write_mask(nmask, maxnode, node0);
  }
}

static isize lm_set_mempolicy_home_node(u64 start, u64 len, u64 node,
                                        u64 flags) {
  if (start & (PAGE_SIZE - 1))
    return -EINVAL;
  if (flags || node != 0)
    return -EINVAL;
  len = (len + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
  if (start + len < start)
    return -EINVAL;
  if (len && !lm_range_mapped(start, len))
    return -ENOENT;
  return 0;
}

/* ── protection keys ─────────────────────────────────────────────── */

/* The keys themselves are the architecture's (x86 PKU); on a CPU without them
 * pkey_alloc answers ENOSPC and pkey_free EINVAL, as Linux does. */
static isize lm_pkey_alloc(u64 flags, u64 init) {
  if (flags || (init & ~(u64)(PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE)))
    return -EINVAL;
  return arch_pkey_alloc((u32)init);
}

static isize lm_pkey_free(u64 pkey) {
  if ((i64)pkey < 0 || pkey > 0xffff)
    return -EINVAL;
  return arch_pkey_free((int)pkey);
}

/* ── sched_setattr / sched_getattr ───────────────────────────────── */

struct lm_sched_attr {
  u32 size;
  u32 sched_policy;
  u64 sched_flags;
  i32 sched_nice;
  u32 sched_priority;
  u64 sched_runtime;
  u64 sched_deadline;
  u64 sched_period;
  u32 sched_util_min;
  u32 sched_util_max;
};
#define SCHED_ATTR_SIZE_VER0 48
#define SCHED_ATTR_SIZE_VER1 56
#define SCHED_OTHER 0
#define SCHED_BATCH 3
#define SCHED_IDLE  5
#define SCHED_FLAG_RESET_ON_FORK 0x01
#define SCHED_FLAG_KEEP_POLICY   0x08
#define SCHED_FLAG_KEEP_PARAMS   0x10
#define SCHED_FLAG_UTIL_CLAMP    0x60

static struct task *lm_task_for_pid(u64 pid) {
  if ((i64)pid < 0)
    return 0;
  return pid == 0 ? current_task : scheduler_task_by_pid((usize)pid);
}

static isize lm_sched_setattr(u64 pid, u64 uattr, u64 flags) {
  struct lm_sched_attr a;
  u32 size;

  if (!uattr || (i64)pid < 0 || flags)
    return -EINVAL;
  if (syscall_copyin(&size, (const void *)(usize)uattr, sizeof(size)))
    return -EFAULT;
  if (size == 0)
    size = SCHED_ATTR_SIZE_VER0;
  if (size < SCHED_ATTR_SIZE_VER0 || size > PAGE_SIZE)
    goto e2big;
  memset(&a, 0, sizeof(a));
  if (syscall_copyin(&a, (const void *)(usize)uattr,
                     size < sizeof(a) ? size : sizeof(a)))
    return -EFAULT;
  /* A larger structure is fine only if the part this kernel does not know
   * is zero. */
  for (u32 off = sizeof(a); off < size; off++) {
    u8 b = 0;

    if (syscall_copyin(&b, (const void *)(usize)(uattr + off), 1))
      return -EFAULT;
    if (b)
      goto e2big;
  }
  if (a.sched_flags & ~(u64)(SCHED_FLAG_RESET_ON_FORK | SCHED_FLAG_KEEP_POLICY |
                             SCHED_FLAG_KEEP_PARAMS | SCHED_FLAG_UTIL_CLAMP))
    return -EINVAL;
  if (a.sched_flags & SCHED_FLAG_UTIL_CLAMP)
    return -EOPNOTSUPP; /* Linux without CONFIG_UCLAMP_TASK */
  struct task *t = lm_task_for_pid(pid);
  if (!t)
    return -ESRCH;
  if (!(a.sched_flags & SCHED_FLAG_KEEP_POLICY)) {
    /* The fair-share policies this scheduler can honour; the real-time ones
     * are refused rather than accepted and ignored. */
    int prc = sched_set_policy(t, (int)a.sched_policy);

    if (prc < 0)
      return prc;
  }
  if (a.sched_priority != 0)
    return -EINVAL;
  if (a.sched_nice < -20 || a.sched_nice > 19)
    return -EINVAL;
  int cur = scheduler_get_priority(t->id);
  if (a.sched_nice < cur && !cred_has_cap(current_task->cred, CAP_SYS_NICE))
    return -EPERM;
  return scheduler_set_priority(t->id, a.sched_nice);
e2big:
  {
    u32 want = SCHED_ATTR_SIZE_VER1;

    if (syscall_copyout((void *)(usize)uattr, &want, sizeof(want)))
      return -EFAULT;
  }
  return -E2BIG;
}

static isize lm_sched_getattr(u64 pid, u64 uattr, u64 usize_, u64 flags) {
  struct lm_sched_attr a;

  if (!uattr || (i64)pid < 0 || flags || usize_ > PAGE_SIZE ||
      usize_ < SCHED_ATTR_SIZE_VER0)
    return -EINVAL;
  struct task *t = lm_task_for_pid(pid);
  if (!t)
    return -ESRCH;
  memset(&a, 0, sizeof(a));
  a.size = usize_ < sizeof(a) ? (u32)usize_ : sizeof(a);
  a.sched_policy = (u32)sched_get_policy(t);
  a.sched_nice = scheduler_get_priority(t->id);
  a.sched_util_max = 1024; /* SCHED_CAPACITY_SCALE: no clamp */
  if (syscall_copyout((void *)(usize)uattr, &a, a.size))
    return -EFAULT;
  return 0;
}

/* ── kcmp ────────────────────────────────────────────────────────── */

#define KCMP_FILE      0
#define KCMP_VM        1
#define KCMP_FILES     2
#define KCMP_FS        3
#define KCMP_SIGHAND   4
#define KCMP_IO        5
#define KCMP_SYSVSEM   6
#define KCMP_EPOLL_TFD 7

/* Linux orders the kernel pointers after a per-boot obfuscation, so the only
 * promise is a consistent total order: 0 equal, 1 less, 2 greater. */
static isize lm_order(u64 a, u64 b) {
  u64 k = 0x9e3779b97f4a7c15ULL;

  a ^= k;
  b ^= k;
  return a == b ? 0 : (a < b ? 1 : 2);
}

static struct vfs_handle *lm_task_fd(struct task *t, u64 fd) {
  struct vfs_handle *h = 0;
  u64 flags;

  spin_lock_irqsave(&t->fd_lock, &flags);
  if (t->fd_table && fd < t->fd_capacity)
    h = t->fd_table[fd];
  if (h)
    vfs_handle_retain(h);
  spin_unlock_irqrestore(&t->fd_lock, flags);
  return h;
}

static isize lm_kcmp(u64 pid1, u64 pid2, u64 type, u64 idx1, u64 idx2) {
  struct task *a = lm_task_for_pid(pid1);
  struct task *b = lm_task_for_pid(pid2);

  if (!a || !b || pid1 == 0 || pid2 == 0)
    return -ESRCH;
  if (!ptrace_may_access(a) || !ptrace_may_access(b))
    return -EPERM;
  switch (type) {
  case KCMP_FILE: {
    struct vfs_handle *ha = lm_task_fd(a, idx1);
    struct vfs_handle *hb = lm_task_fd(b, idx2);
    isize r = (!ha || !hb) ? -EBADF : lm_order((u64)(usize)ha, (u64)(usize)hb);

    if (ha)
      vfs_handle_release(ha);
    if (hb)
      vfs_handle_release(hb);
    return r;
  }
  case KCMP_VM:
    return lm_order(a->pml4_phys, b->pml4_phys);
  case KCMP_FILES:
    return lm_order((u64)(usize)a->fd_table, (u64)(usize)b->fd_table);
  case KCMP_FS:
  case KCMP_IO:
  case KCMP_SYSVSEM:
    /* Held per thread group here: threads share them, processes do not
     * (CLONE_FS/CLONE_IO/CLONE_SYSVSEM are what CLONE_THREAD implies). */
    return lm_order(task_tgid(a), task_tgid(b));
  case KCMP_SIGHAND:
    return lm_order(task_tgid(a), task_tgid(b));
  case KCMP_EPOLL_TFD:
    return -EOPNOTSUPP;
  default:
    return -EINVAL;
  }
}

/* ── pidfd_getfd, process_madvise, process_mrelease ──────────────── */

static struct task *lm_pidfd_task(u64 pidfd, isize *err) {
  struct vfs_handle *h = scheduler_fd_get((int)pidfd);

  *err = 0;
  if (!h) {
    *err = -EBADF;
    return 0;
  }
  usize pid = vfs_pidfd_pid(h);
  if (!pid) {
    *err = -EBADF;
    return 0;
  }
  struct task *t = scheduler_task_by_pid(pid);
  if (!t || t->state == TASK_DEAD || t->state == TASK_REAPING ||
      t->state == TASK_UNUSED) {
    *err = -ESRCH;
    return 0;
  }
  return t;
}

static isize lm_pidfd_getfd(u64 pidfd, u64 targetfd, u64 flags) {
  isize err;

  if (flags)
    return -EINVAL;
  struct task *t = lm_pidfd_task(pidfd, &err);
  if (!t)
    return err;
  if (!ptrace_may_access(t))
    return -EPERM;
  struct vfs_handle *h = lm_task_fd(t, targetfd);
  if (!h)
    return -EBADF;
  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h);
    return fd;
  }
  /* The new descriptor is close-on-exec, as Linux makes it. */
  scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

#define MADV_WILLNEED 3
#define MADV_COLD     20
#define MADV_PAGEOUT  21
#define MADV_COLLAPSE 25
#define UIO_MAXIOV    1024

static isize lm_process_madvise(u64 pidfd, u64 uvec, u64 vlen, u64 advice,
                                u64 flags) {
  isize err;

  if (flags)
    return -EINVAL;
  if (advice != MADV_COLD && advice != MADV_PAGEOUT &&
      advice != MADV_WILLNEED && advice != MADV_COLLAPSE)
    return -EINVAL;
  if (vlen > UIO_MAXIOV)
    return -EINVAL;
  struct task *t = lm_pidfd_task(pidfd, &err);
  if (!t)
    return err;
  if (!ptrace_may_access(t))
    return -EPERM;
  /* Advice that changes another process's memory needs CAP_SYS_NICE. */
  if (task_tgid(t) != task_tgid(current_task) &&
      !cred_has_cap(current_task->cred, CAP_SYS_NICE))
    return -EPERM;
  u64 total = 0;
  for (u64 i = 0; i < vlen; i++) {
    u64 iov[2];

    if (syscall_copyin(iov, (const void *)(usize)(uvec + i * 16), sizeof(iov)))
      return -EFAULT;
    if (iov[0] & (PAGE_SIZE - 1))
      return -EINVAL;
    if (iov[1] > USER_SPACE_LIMIT || iov[0] + iov[1] > USER_SPACE_LIMIT)
      return -EINVAL;
    total += iov[1];
  }
  /* These are hints about reclaim and read-ahead. This kernel's reclaim picks
   * its own victims and has no huge pages to collapse, so accepting them as
   * already satisfied is the honest reading -- the count is what Linux
   * returns for advice it applied. */
  return (isize)total;
}

static isize lm_process_mrelease(u64 pidfd, u64 flags) {
  isize err;

  if (flags)
    return -EINVAL;
  struct task *t = lm_pidfd_task(pidfd, &err);
  if (!t)
    return err == -ESRCH ? 0 : err; /* already gone: its memory is freed */
  /* Only a process that is already dying may be reaped early. */
  if (!(__atomic_load_n(&t->pending_signals, __ATOMIC_ACQUIRE) &
        (1ULL << (9 - 1))))
    return -EINVAL;
  /* Its address space is torn down by the exit the pending SIGKILL is
   * driving; the call's promise -- the memory is on its way back -- holds. */
  return 0;
}

/* ── cachestat ───────────────────────────────────────────────────── */

struct lm_cachestat_range {
  u64 off;
  u64 len;
};
struct lm_cachestat {
  u64 nr_cache;
  u64 nr_dirty;
  u64 nr_writeback;
  u64 nr_evicted;
  u64 nr_recently_evicted;
};

static isize lm_cachestat(u64 fd, u64 urange, u64 ustat, u64 flags) {
  struct lm_cachestat_range r;
  struct lm_cachestat cs;

  if (syscall_copyin(&r, (const void *)(usize)urange, sizeof(r)))
    return -EFAULT;
  if (flags)
    return -EINVAL;
  struct vfs_handle *h = scheduler_fd_get((int)fd);
  if (!h)
    return -EBADF;
  if (h->kind != VFS_HANDLE_NODE || !h->node || !h->node->inode)
    return -EOPNOTSUPP;
  struct vfs_inode *in = h->node->inode;
  if (in->type == VFS_DIRECTORY)
    return -EOPNOTSUPP;
  memset(&cs, 0, sizeof(cs));
  u64 size = (u64)in->size;
  u64 first = r.off & ~(u64)(PAGE_SIZE - 1);
  u64 end = r.len == 0 ? size : r.off + r.len;
  if (end < r.off)
    end = size;
  if (end > size)
    end = size;
  for (u64 off = first; off < end; off += PAGE_SIZE) {
    struct page_cache_entry *p = page_cache_get_page(in, off);

    if (!p)
      continue;
    cs.nr_cache++;
    if (p->flags & PAGE_CACHE_DIRTY)
      cs.nr_dirty++;
    page_cache_put_page(p);
  }
  if (syscall_copyout((void *)(usize)ustat, &cs, sizeof(cs)))
    return -EFAULT;
  return 0;
}

/* ── futex2 ──────────────────────────────────────────────────────── */

#define FUTEX2_SIZE_U8   0x00
#define FUTEX2_SIZE_U16  0x01
#define FUTEX2_SIZE_U32  0x02
#define FUTEX2_SIZE_U64  0x03
#define FUTEX2_NUMA      0x04
#define FUTEX2_MPOL      0x08
#define FUTEX2_PRIVATE   128
#define FUTEX2_SIZE_MASK 0x03
#define FUTEX2_VALID_MASK (FUTEX2_SIZE_MASK | FUTEX2_PRIVATE)
#define FUTEX_WAITV_MAX  128
#define LM_CLOCK_REALTIME  0
#define LM_CLOCK_MONOTONIC 1

/* Linux implements only 32-bit futexes behind the futex2 calls; the other
 * sizes are reserved and refused. */
static int lm_futex2_flags(u64 flags) {
  if (flags & ~(u64)FUTEX2_VALID_MASK)
    return -EINVAL;
  if ((flags & FUTEX2_SIZE_MASK) != FUTEX2_SIZE_U32)
    return -EINVAL;
  return (flags & FUTEX2_PRIVATE) ? B1NIX_FUTEX_PRIVATE : 0;
}

/* An absolute timespec on `clockid`, as milliseconds from now (0: none). */
static int lm_abs_timeout_ms(u64 uts, u64 clockid, u64 *out) {
  i64 ts[2];

  *out = 0;
  if (!uts)
    return 0;
  if (clockid != LM_CLOCK_REALTIME && clockid != LM_CLOCK_MONOTONIC)
    return -EINVAL;
  if (syscall_copyin(ts, (const void *)(usize)uts, sizeof(ts)))
    return -EFAULT;
  if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000)
    return -EINVAL;
  u64 want = (u64)ts[0] * 1000 + (u64)ts[1] / 1000000;
  u64 now = clockid == LM_CLOCK_REALTIME
                ? vfs_get_unix_time() * 1000
                : ktime_monotonic_ns() / 1000000;
  if (want <= now)
    return -ETIMEDOUT;
  *out = want - now;
  return 0;
}

static isize lm_futex_wait(u64 uaddr, u64 val, u64 mask, u64 flags, u64 uts,
                           u64 clockid) {
  int priv = lm_futex2_flags(flags);
  u64 tmo;

  if (priv < 0)
    return priv;
  if (!mask)
    return -EINVAL;
  if (val >> 32)
    return -EINVAL; /* a value wider than the futex */
  int rc = lm_abs_timeout_ms(uts, clockid, &tmo);
  if (rc)
    return rc;
  return scheduler_futex(uaddr, B1NIX_FUTEX_WAIT | priv, (int)(u32)val, tmo);
}

static isize lm_futex_wake(u64 uaddr, u64 mask, u64 nr, u64 flags) {
  int priv = lm_futex2_flags(flags);

  if (priv < 0)
    return priv;
  if (!mask)
    return 0;
  if ((i64)nr < 0)
    return -EINVAL;
  if (nr > 0x7fffffff)
    nr = 0x7fffffff;
  return scheduler_futex(uaddr, B1NIX_FUTEX_WAKE | priv, (int)nr, 0);
}

struct lm_futex_waitv {
  u64 val;
  u64 uaddr;
  u32 flags;
  u32 reserved;
};

static isize lm_futex_waitv(u64 uwaiters, u64 nr, u64 flags, u64 uts,
                            u64 clockid) {
  struct scheduler_futex_vec v[FUTEX_WAITV_MAX];
  u64 tmo;

  if (flags)
    return -EINVAL;
  if (nr == 0 || nr > FUTEX_WAITV_MAX || !uwaiters)
    return -EINVAL;
  int rc = lm_abs_timeout_ms(uts, clockid, &tmo);
  if (rc)
    return rc;
  for (u64 i = 0; i < nr; i++) {
    struct lm_futex_waitv w;

    if (syscall_copyin(&w, (const void *)(usize)(uwaiters + i * sizeof(w)),
                       sizeof(w)))
      return -EFAULT;
    int priv = lm_futex2_flags(w.flags);
    if (priv < 0 || w.reserved || (w.val >> 32))
      return -EINVAL;
    v[i].uaddr = w.uaddr;
    v[i].val = (int)(u32)w.val;
    v[i].priv = priv;
  }
  return scheduler_futex_waitv(v, (int)nr, tmo);
}

/* ── openat2 ─────────────────────────────────────────────────────── */

struct lm_open_how {
  u64 flags;
  u64 mode;
  u64 resolve;
};
#define RESOLVE_NO_XDEV       0x01
#define RESOLVE_NO_MAGICLINKS 0x02
#define RESOLVE_NO_SYMLINKS   0x04
#define RESOLVE_BENEATH       0x08
#define RESOLVE_IN_ROOT       0x10
#define RESOLVE_CACHED        0x20
#define RESOLVE_ALL           0x3f
#define LM_O_CREAT    0100
#define LM_O_NOFOLLOW 0400000
#define LM_O_PATH     010000000
#define LM_O_TMPFILE  020200000
#define LM_O_ALL_FLAGS 037777777ULL
#define LM_SYMLINK_MAX 40

/* Is `path` one of procfs's magic links -- a link whose target is an object
 * rather than a name (fd/N, exe, cwd, root, map_files/...)? */
static int lm_is_magic_link(const char *path) {
  if (strncmp(path, "/proc/", 6) != 0)
    return 0;
  const char *p = path + 6;
  while (*p && *p != '/')
    p++;
  if (*p != '/')
    return 0;
  p++;
  if (!strcmp(p, "exe") || !strcmp(p, "cwd") || !strcmp(p, "root"))
    return 1;
  if (!strncmp(p, "fd/", 3) || !strncmp(p, "map_files/", 10))
    return 1;
  if (!strncmp(p, "task/", 5)) {
    const char *q = p + 5;

    while (*q && *q != '/')
      q++;
    if (*q == '/') {
      q++;
      if (!strcmp(q, "exe") || !strcmp(q, "cwd") || !strcmp(q, "root") ||
          !strncmp(q, "fd/", 3))
        return 1;
    }
  }
  return 0;
}

/* Append component `c` (len n) to `cur`, a normalised absolute path. */
static int lm_path_push(char *cur, usize cap, const char *c, usize n) {
  usize l = strlen(cur);

  if (l + 1 + n + 1 > cap)
    return -ENAMETOOLONG;
  if (!(l == 1 && cur[0] == '/'))
    cur[l++] = '/';
  memcpy(cur + l, c, n);
  cur[l + n] = '\0';
  return 0;
}

static void lm_path_pop(char *cur) {
  usize l = strlen(cur);

  while (l > 1 && cur[l - 1] != '/')
    l--;
  if (l > 1)
    l--;
  cur[l] = '\0';
}

static int lm_under(const char *path, const char *root) {
  usize rl = strlen(root);

  if (rl == 1 && root[0] == '/')
    return 1;
  return strncmp(path, root, rl) == 0 && (path[rl] == '\0' || path[rl] == '/');
}

/*
 * Resolve `upath` against `base` one component at a time, applying the
 * RESOLVE_* restrictions, into an absolute path with every symlink replaced
 * -- except a trailing one the caller will not follow.
 */
static int lm_resolve_restricted(const char *base, const char *upath,
                                 u64 resolve, int follow_last, char *out,
                                 usize cap) {
  char rest[VFS_MAX_PATH * 2];
  char root[VFS_MAX_PATH];
  int links = 0;

  if (strlen(base) >= sizeof(root))
    return -ENAMETOOLONG;
  strcpy(root, base);
  if (upath[0] == '/') {
    if (resolve & RESOLVE_BENEATH)
      return -EXDEV;
    if (resolve & RESOLVE_IN_ROOT)
      strcpy(out, root);
    else
      strcpy(out, "/");
  } else {
    strcpy(out, root);
  }
  if (strlen(upath) >= sizeof(rest))
    return -ENAMETOOLONG;
  strcpy(rest, upath);
  int base_mnt = vfs_mount_id_for_path(root);

  char *p = rest;
  while (*p) {
    while (*p == '/')
      p++;
    if (!*p)
      break;
    char *e = p;
    while (*e && *e != '/')
      e++;
    usize n = (usize)(e - p);
    int last = (*e == '\0');
    {
      const char *q = e;

      while (*q == '/')
        q++;
      if (!*q)
        last = 1;
    }
    if (n == 1 && p[0] == '.') {
      p = e;
      continue;
    }
    if (n == 2 && p[0] == '.' && p[1] == '.') {
      if (resolve & (RESOLVE_BENEATH | RESOLVE_IN_ROOT)) {
        if (!strcmp(out, root)) {
          if (resolve & RESOLVE_BENEATH)
            return -EXDEV;
          p = e; /* IN_ROOT: .. at the root stays at the root */
          continue;
        }
      }
      lm_path_pop(out);
      if ((resolve & RESOLVE_NO_XDEV) && vfs_mount_id_for_path(out) != base_mnt)
        return -EXDEV;
      p = e;
      continue;
    }
    int rc = lm_path_push(out, cap, p, n);
    if (rc)
      return rc;
    if ((resolve & RESOLVE_NO_XDEV) && vfs_mount_id_for_path(out) != base_mnt)
      return -EXDEV;

    struct b1nix_stat st;
    if (vfs_lstat(out, &st) == 0 && (st.st_mode & 0170000) == B1NIX_S_IFLNK &&
        (!last || follow_last)) {
      if (lm_is_magic_link(out)) {
        if (resolve & (RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS |
                       RESOLVE_BENEATH | RESOLVE_IN_ROOT))
          return -ELOOP;
      } else if (resolve & RESOLVE_NO_SYMLINKS) {
        return -ELOOP;
      }
      if (++links > LM_SYMLINK_MAX)
        return -ELOOP;
      char target[VFS_MAX_PATH];
      isize tl = vfs_readlink(out, target, sizeof(target) - 1);
      if (tl < 0)
        return (int)tl;
      target[tl] = '\0';
      lm_path_pop(out);
      /* Splice the target in front of what is left. */
      char next[VFS_MAX_PATH * 2];
      usize rl = strlen(e);
      if ((usize)tl + rl + 2 > sizeof(next))
        return -ENAMETOOLONG;
      memcpy(next, target, (usize)tl);
      memcpy(next + tl, e, rl + 1);
      if (target[0] == '/') {
        if (resolve & RESOLVE_BENEATH)
          return -EXDEV;
        strcpy(out, (resolve & RESOLVE_IN_ROOT) ? root : "/");
      }
      strcpy(rest, next);
      p = rest;
      continue;
    } else if ((resolve & RESOLVE_NO_SYMLINKS) && last && !follow_last &&
               vfs_lstat(out, &st) == 0 &&
               (st.st_mode & 0170000) == B1NIX_S_IFLNK) {
      /* A trailing symlink is refused too unless it is opened as itself. */
      if (lm_is_magic_link(out))
        return -ELOOP;
    }
    if ((resolve & RESOLVE_BENEATH) && !lm_under(out, root))
      return -EXDEV;
    p = e;
  }
  return 0;
}

static isize lm_openat2(u64 dirfd, u64 upath, u64 uhow, u64 usize_) {
  struct lm_open_how how;
  char kpath[VFS_MAX_PATH];
  char base[VFS_MAX_PATH];
  char resolved[VFS_MAX_PATH];

  if (usize_ < sizeof(how))
    return -EINVAL;
  if (usize_ > PAGE_SIZE)
    return -E2BIG;
  if (syscall_copyin(&how, (const void *)(usize)uhow, sizeof(how)))
    return -EFAULT;
  for (u64 off = sizeof(how); off < usize_; off++) {
    u8 b = 0;

    if (syscall_copyin(&b, (const void *)(usize)(uhow + off), 1))
      return -EFAULT;
    if (b)
      return -E2BIG;
  }
  if (how.flags & ~LM_O_ALL_FLAGS)
    return -EINVAL;
  if (how.resolve & ~(u64)RESOLVE_ALL)
    return -EINVAL;
  if ((how.resolve & RESOLVE_BENEATH) && (how.resolve & RESOLVE_IN_ROOT))
    return -EINVAL;
  if (how.mode & ~07777ULL)
    return -EINVAL;
  if (how.mode && !(how.flags & LM_O_CREAT) &&
      (how.flags & LM_O_TMPFILE) != LM_O_TMPFILE)
    return -EINVAL;
  if ((how.flags & LM_O_PATH) &&
      (how.flags & ~(u64)(LM_O_PATH | LM_O_NOFOLLOW | 02000000 | 0200000)))
    return -EINVAL;
  /* RESOLVE_CACHED promises not to block on I/O; a creating open cannot keep
   * that promise, so Linux refuses it. */
  if ((how.resolve & RESOLVE_CACHED) &&
      (how.flags & (LM_O_CREAT | 01000 | LM_O_TMPFILE)))
    return -EAGAIN;
  int cs = syscall_copyinstr(kpath, sizeof(kpath), (const char *)(usize)upath);
  if (cs < 0)
    return cs;
  if (kpath[0] == '\0')
    return -ENOENT;
  if ((int)dirfd == -100 /* AT_FDCWD */) {
    const char *cwd = scheduler_get_cwd();

    if (!cwd || strlen(cwd) >= sizeof(base))
      return -ENOENT;
    strcpy(base, cwd);
  } else if (vfs_fd_abspath((int)dirfd, base, sizeof(base)) < 0) {
    return -EBADF;
  }
  int follow_last = !(how.flags & LM_O_NOFOLLOW);
  int rc = lm_resolve_restricted(base, kpath, how.resolve, follow_last,
                                 resolved, sizeof(resolved));
  if (rc)
    return rc;
  return vfs_open_flags_mode(resolved, linux_modern_open_flags((int)how.flags),
                             (u16)how.mode);
}

/* ── statmount / listmount ───────────────────────────────────────── */

struct lm_mnt_id_req {
  u32 size;
  u32 spare;
  u64 mnt_id;
  u64 param;
  u64 mnt_ns_id;
};
#define MNT_ID_REQ_SIZE_VER0 24
#define LM_LISTMOUNT_REVERSE 1

static int lm_read_mnt_req(u64 ureq, struct lm_mnt_id_req *r) {
  u32 size;

  if (syscall_copyin(&size, (const void *)(usize)ureq, sizeof(size)))
    return -EFAULT;
  if (size < MNT_ID_REQ_SIZE_VER0 || size > PAGE_SIZE)
    return -EINVAL;
  memset(r, 0, sizeof(*r));
  if (syscall_copyin(r, (const void *)(usize)ureq,
                     size < sizeof(*r) ? size : sizeof(*r)))
    return -EFAULT;
  if (r->spare)
    return -EINVAL;
  /* Another namespace by id is not something this kernel can name yet. */
  if (size >= sizeof(*r) && r->mnt_ns_id)
    return -ENOENT;
  return 0;
}

static isize lm_statmount(u64 ureq, u64 ubuf, u64 bufsize, u64 flags) {
  struct lm_mnt_id_req r;

  if (flags)
    return -EINVAL;
  int rc = lm_read_mnt_req(ureq, &r);
  if (rc)
    return rc;
  if (bufsize < 512)
    return -EINVAL;
  usize cap = bufsize > 65536 ? 65536 : (usize)bufsize;
  char *k = kmalloc(cap);
  if (!k)
    return -ENOMEM;
  isize n = vfs_statmount(r.mnt_id, r.param, k, cap);
  if (n >= 0 && syscall_copyout((void *)(usize)ubuf, k, (usize)n))
    n = -EFAULT;
  kfree(k);
  return n < 0 ? n : 0;
}

static isize lm_listmount(u64 ureq, u64 uids, u64 nr, u64 flags) {
  struct lm_mnt_id_req r;

  if (flags & ~(u64)LM_LISTMOUNT_REVERSE)
    return -EINVAL;
  int rc = lm_read_mnt_req(ureq, &r);
  if (rc)
    return rc;
  if (nr == 0)
    return 0;
  if (nr > 1000000)
    return -EOVERFLOW;
  usize cap = nr > 4096 ? 4096 : (usize)nr;
  u64 *ids = kmalloc(cap * sizeof(u64));
  if (!ids)
    return -ENOMEM;
  isize n = vfs_listmount(r.mnt_id, r.param, ids, cap,
                          (flags & LM_LISTMOUNT_REVERSE) != 0);
  if (n > 0 && syscall_copyout((void *)(usize)uids, ids, (usize)n * sizeof(u64)))
    n = -EFAULT;
  kfree(ids);
  return n;
}

/* ── quotactl ────────────────────────────────────────────────────── */

#define Q_SYNC         0x800001
#define Q_QUOTAON      0x800002
#define LM_MAXQUOTAS   3

/* The target is named the way the Linux system call names it; everything
 * after that -- the permission check, the command and its argument copies --
 * is upstream's own fs/quota/quota.c (kernel/lkpi/fs_quotactl.c), because the
 * filesystems with quota support here are the imported ones. On any other
 * filesystem the answer is Linux's for one without quota operations. */
#ifdef B1NIX_FS_IMPORT
extern int lkpifs_quotactl(struct vfs_node *on_fs, u32 cmd, u32 id, u64 addr,
                           struct vfs_node *quota_file, int path_err,
                           int readonly);
extern void lkpifs_quota_sync_all(int type);
#endif

static isize lm_quota_run(struct vfs_node *on_fs, u64 mnt_flags, u64 cmd,
                          u64 id, u64 addr, struct vfs_node *quota_file,
                          int path_err, int by_fd) {
#ifdef B1NIX_FS_IMPORT
  return lkpifs_quotactl(on_fs, (u32)cmd, (u32)id, addr, quota_file, path_err,
                         by_fd && (mnt_flags & MS_RDONLY));
#else
  (void)on_fs, (void)mnt_flags, (void)cmd, (void)id, (void)addr;
  (void)quota_file, (void)path_err, (void)by_fd;
  return -ENOSYS;
#endif
}

static isize lm_quotactl(u64 cmd, u64 uspecial, u64 id, u64 addr) {
  char special[VFS_MAX_PATH];
  u32 cmds = (u32)cmd >> 8;
  u32 type = (u32)cmd & 0xff;

  if (type >= LM_MAXQUOTAS)
    return -EINVAL;
  if (!uspecial) {
    if (cmds != Q_SYNC)
      return -ENODEV;
#ifdef B1NIX_FS_IMPORT
    lkpifs_quota_sync_all((int)type);
#endif
    return 0;
  }
  int cs = syscall_copyinstr(special, sizeof(special), (const char *)(usize)uspecial);
  if (cs < 0)
    return cs;

  /* Q_QUOTAON's argument is the quota file, resolved before the device, and
   * a failure is only reported if the filesystem needs the file. */
  struct vfs_node *qfile = 0;
  int path_err = 0;
  if (cmds == Q_QUOTAON) {
    char qpath[VFS_MAX_PATH];
    int qs = addr ? syscall_copyinstr(qpath, sizeof(qpath), (const char *)(usize)addr)
                  : -EFAULT;
    if (qs < 0) {
      path_err = qs;
    } else {
      qfile = vfs_find_node(qpath);
      if (IS_ERR(qfile) || !qfile) {
        path_err = qfile ? (int)PTR_ERR(qfile) : -ENOENT;
        qfile = 0;
      }
    }
  }

  struct vfs_node *on_fs = 0;
  u64 mnt_flags = 0;
  isize rc = vfs_quota_target(special, -1, &on_fs, &mnt_flags);
  if (rc == 0) {
    rc = lm_quota_run(on_fs, mnt_flags, cmd, id, addr, qfile, path_err, 0);
    vfs_node_put(on_fs);
  }
  if (qfile)
    vfs_node_put(qfile);
  return rc;
}

static isize lm_quotactl_fd(u64 fd, u64 cmd, u64 id, u64 addr) {
  struct vfs_node *on_fs = 0;
  u64 mnt_flags = 0;
  isize rc = vfs_quota_target(0, (int)fd, &on_fs, &mnt_flags);

  if (rc)
    return rc;
  if (((u32)cmd & 0xff) >= LM_MAXQUOTAS) {
    vfs_node_put(on_fs);
    return -EINVAL;
  }
  rc = lm_quota_run(on_fs, mnt_flags, cmd, id, addr, 0, -EINVAL, 1);
  vfs_node_put(on_fs);
  return rc;
}

/* ── dispatch ────────────────────────────────────────────────────── */

int linux_modern_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                         u64 a5, u64 *ret) {
  isize r;

  switch (nr) {
  case LM_mbind:
    r = lm_mbind(a0, a1, a2, a3, a4, a5);
    break;
  case LM_set_mempolicy:
    r = lm_set_mempolicy(a0, a1, a2);
    break;
  case LM_get_mempolicy:
    r = lm_get_mempolicy(a0, a1, a2, a3, a4);
    break;
  case LM_set_mempolicy_home_node:
    r = lm_set_mempolicy_home_node(a0, a1, a2, a3);
    break;
  case LM_pkey_alloc:
    r = lm_pkey_alloc(a0, a1);
    break;
  case LM_pkey_free:
    r = lm_pkey_free(a0);
    break;
  case LM_pkey_mprotect:
    /* Key -1 is "no key": plain mprotect. Any other key must be allocated. */
    r = linux_modern_pkey_mprotect(a0, a1, a2, a3);
    break;
  case LM_sched_setattr:
    r = lm_sched_setattr(a0, a1, a2);
    break;
  case LM_sched_getattr:
    r = lm_sched_getattr(a0, a1, a2, a3);
    break;
  case LM_kcmp:
    r = lm_kcmp(a0, a1, a2, a3, a4);
    break;
  case LM_pidfd_getfd:
    r = lm_pidfd_getfd(a0, a1, a2);
    break;
  case LM_process_madvise:
    r = lm_process_madvise(a0, a1, a2, a3, a4);
    break;
  case LM_process_mrelease:
    r = lm_process_mrelease(a0, a1);
    break;
  case LM_cachestat:
    r = lm_cachestat(a0, a1, a2, a3);
    break;
  case LM_futex_wait:
    r = lm_futex_wait(a0, a1, a2, a3, a4, a5);
    break;
  case LM_futex_wake:
    r = lm_futex_wake(a0, a1, a2, a3);
    break;
  case LM_futex_waitv:
    r = lm_futex_waitv(a0, a1, a2, a3, a4);
    break;
  case LM_remap_file_pages:
    r = linux_modern_remap_file_pages(a0, a1, a2, a3, a4);
    break;
  case LM_quotactl:
    r = lm_quotactl(a0, a1, a2, a3);
    break;
  case LM_quotactl_fd:
    r = lm_quotactl_fd(a0, a1, a2, a3);
    break;
  case LM_statmount:
    r = lm_statmount(a0, a1, a2, a3);
    break;
  case LM_listmount:
    r = lm_listmount(a0, a1, a2, a3);
    break;
  case LM_openat2:
    r = lm_openat2(a0, a1, a2, a3);
    break;
  case LM_memfd_secret:
    /* O_CLOEXEC is the only flag, and it has one value on every architecture
     * this kernel runs on. */
    if (a0 & ~(u64)LM_O_CLOEXEC)
      r = -EINVAL;
    else
      r = vfs_memfd_secret((a0 & LM_O_CLOEXEC) != 0);
    break;
  default:
    if (io_uring_syscall(nr, a0, a1, a2, a3, a4, a5, ret))
      return 1;
    if (landlock_syscall(nr, a0, a1, a2, a3, ret))
      return 1;
    return linux_keys_syscall(nr, a0, a1, a2, a3, a4, ret);
  }
  *ret = (u64)r;
  return 1;
}
