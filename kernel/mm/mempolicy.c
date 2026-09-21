/* SPDX-License-Identifier: GPL-2.0-only */
/* NUMA memory policy (M128; the calls themselves arrived with M124).
 *
 * A policy says which node a page should come from. There are two of them for
 * any allocation: the one the mapping carries (mbind) and the one the task
 * carries (set_mempolicy), and the mapping's wins — that is the order Linux
 * resolves them in, and programs written against numactl(1) depend on it.
 *
 * Until this milestone the machine had one node, so every policy was
 * satisfiable by definition and these calls only had to validate their
 * arguments. They now decide where memory comes from, which means two things
 * had to become real: the node mask is kept rather than checked and dropped,
 * and MPOL_MF_MOVE actually moves the pages that are already there. */

#include <b1nix/mempolicy.h>

#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/numa.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/tlb.h>
#include <b1nix/user.h>
#include <b1nix/uidgid.h>

#include <string.h>

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
#define MPOL_MAXNODE_LIMIT (PAGE_SIZE * 8)

/* ── per-task policy ─────────────────────────────────────────────────────── */

struct mempolicy_task {
  u8 mode;
  u8 pad;
  u16 flags;
  u16 nodes;     /* bitmap, NUMA_MAX_NODES wide */
  u16 il_next;   /* where MPOL_INTERLEAVE is up to */
};

static struct mempolicy_task *g_pol;
static usize g_pol_slots;

static struct mempolicy_task *pol_slot(usize slot, int create) {
  if (!g_pol && create) {
    usize n = scheduler_max_task_slots();
    struct mempolicy_task *t = kzalloc(n * sizeof(*t));

    if (t && !__atomic_compare_exchange_n(&g_pol,
                                          &(struct mempolicy_task *){0}, t, 0,
                                          __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      kfree(t);
    else if (t)
      g_pol_slots = n;
  }
  return (g_pol && slot < g_pol_slots) ? &g_pol[slot] : 0;
}

static struct mempolicy_task *pol_cur(int create) {
  return pol_slot(scheduler_task_index(current_task), create);
}

void mempolicy_task_reset(usize slot) {
  struct mempolicy_task *s = pol_slot(slot, 0);

  if (s)
    memset(s, 0, sizeof(*s));
}

void mempolicy_fork_inherit(usize parent_slot, usize child_slot) {
  struct mempolicy_task *p = pol_slot(parent_slot, 0);
  struct mempolicy_task *c = pol_slot(child_slot, 0);

  if (p && c)
    *c = *p;
}

/* ── node masks ──────────────────────────────────────────────────────────── */

/* Read a user node mask of `maxnode` bits (Linux drops the last bit) into a
 * bitmap of the nodes this machine has. `beyond` reports a bit set for a node
 * that does not exist, which is EINVAL to every caller here. */
static int read_nodemask(u64 uptr, u64 maxnode, u16 *mask, int *empty,
                         int *beyond) {
  u64 words;

  *mask = 0;
  *empty = 1;
  *beyond = 0;
  if (!uptr || maxnode == 0)
    return 0;
  maxnode--;
  if (maxnode > MPOL_MAXNODE_LIMIT)
    return -EINVAL;
  words = (maxnode + 63) / 64;
  for (u64 w = 0; w < words; w++) {
    u64 v = 0;

    if (syscall_copyin(&v, (const void *)(usize)(uptr + w * 8), 8) != 0)
      return -EFAULT;
    if (w == words - 1 && maxnode % 64)
      v &= (1ULL << (maxnode % 64)) - 1;
    if (v)
      *empty = 0;
    if (w == 0) {
      int nodes = numa_node_count();
      u64 known = nodes >= 64 ? ~0ULL : ((1ULL << nodes) - 1);

      *mask = (u16)(v & known & 0xffff);
      if (v & ~known)
        *beyond = 1;
    } else if (v) {
      *beyond = 1;
    }
  }
  return 0;
}

static isize write_nodemask(u64 uptr, u64 maxnode, u16 mask) {
  u64 words;

  if (!uptr)
    return 0;
  if (maxnode == 0)
    return -EINVAL;
  if (maxnode > MPOL_MAXNODE_LIMIT)
    return -EINVAL;
  words = (maxnode + 63) / 64;
  for (u64 w = 0; w < words; w++) {
    u64 v = (w == 0) ? (u64)mask : 0;

    if (syscall_copyout((void *)(usize)(uptr + w * 8), &v, 8) != 0)
      return -EFAULT;
  }
  return 0;
}

/* Validate a mode word plus its mask, and hand back both halves. */
static int check_policy(u64 mode_word, u64 nmask, u64 maxnode, u16 *mask_out) {
  u32 mode = (u32)mode_word & ~(u32)MPOL_MODE_FLAGS;
  u32 mflags = (u32)mode_word & (u32)MPOL_MODE_FLAGS;
  int empty, beyond, rc;
  u16 mask = 0;

  if (mode >= MPOL_MAX || (u32)(mode_word >> 32))
    return -EINVAL;
  if ((mflags & MPOL_F_STATIC_NODES) && (mflags & MPOL_F_RELATIVE_NODES))
    return -EINVAL;
  if ((mflags & MPOL_F_NUMA_BALANCING) && mode != MPOL_BIND)
    return -EINVAL;
  rc = read_nodemask(nmask, maxnode, &mask, &empty, &beyond);
  if (rc)
    return rc;
  if (beyond)
    return -EINVAL; /* a node this machine does not have */
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
  if (mask_out)
    *mask_out = mask;
  return (int)mode;
}

/* ── resolving a policy to a node ────────────────────────────────────────── */

static int first_node(u16 mask) {
  for (int i = 0; i < NUMA_MAX_NODES; i++)
    if (mask & (1u << i))
      return i;
  return -1;
}

/* Round-robin over the mask, for MPOL_INTERLEAVE. */
static int interleave_node(u16 mask, u16 *cursor) {
  int n = numa_node_count();

  for (int step = 0; step < n; step++) {
    int i = (*cursor + step) % n;

    if (mask & (1u << i)) {
      *cursor = (u16)((i + 1) % n);
      return i;
    }
  }
  return -1;
}

static int resolve(u8 mode, u16 mask, u16 *cursor, int *strict) {
  switch (mode) {
  case MPOL_BIND:
    if (strict)
      *strict = 1;
    return first_node(mask);
  case MPOL_PREFERRED:
  case MPOL_PREFERRED_MANY:
    return mask ? first_node(mask) : numa_node_here();
  case MPOL_INTERLEAVE:
  case MPOL_WEIGHTED_INTERLEAVE:
    return cursor ? interleave_node(mask, cursor) : first_node(mask);
  case MPOL_LOCAL:
    return numa_node_here();
  default:
    return -1;
  }
}

int mempolicy_node_for(struct vm_area *vma, u64 vaddr, int *strict) {
  struct mempolicy_task *t;

  (void)vaddr;
  if (strict)
    *strict = 0;
  if (numa_node_count() <= 1)
    return -1;
  if (vma && vma->mpol_mode) {
    u16 cursor = vma->mpol_nodes ? (u16)(vaddr / PAGE_SIZE) : 0;
    int node = resolve(vma->mpol_mode, vma->mpol_nodes, &cursor, strict);

    /* An interleaved mapping picks its node from the page's own offset, so
     * the same page always lands on the same node however many times it is
     * faulted, dropped and faulted again. */
    if (node >= 0)
      return node;
  }
  if (vma && vma->mpol_home)
    return (int)vma->mpol_home - 1;
  t = pol_cur(0);
  if (t && t->mode) {
    int node = resolve(t->mode, t->nodes, &t->il_next, strict);

    if (node >= 0)
      return node;
  }
  return -1; /* wherever this CPU is */
}

/* ── the range walk mbind needs ──────────────────────────────────────────── */

static int range_mapped(u64 start, u64 len) {
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

/* Move one present anonymous page onto `node`. Returns 1 when it moved, 0 when
 * it was already there or there was nothing to move, -1 when it could not be
 * moved (a shared page, or no memory on the target). */
static int migrate_page(struct task *t, struct vm_area *vma, u64 va, int node) {
  u64 pte = paging_leaf_pte(va);
  u64 old, fresh;

  if (!(pte & VMM_PRESENT))
    return 0;
  old = pte & 0x000ffffffffff000ULL;
  if (!old)
    return 0;
  if (numa_node_of_frame(old) == node)
    return 0;
  /* A page more than one mapping holds (a COW child, a shared file page) is
   * not this call's to move: the other holders would keep the old one and the
   * copy would silently stop being shared. */
  if (pmm_get_refcount(old) != 1)
    return -1;
  if (vma->node)
    return -1; /* file-backed: the page belongs to the page cache */
  fresh = pmm_alloc_frame_node(node, 1);
  if (!fresh)
    return -1;
  memcpy((void *)(usize)(fresh + DIRECT_MAP_BASE),
         (const void *)(usize)(old + DIRECT_MAP_BASE), PAGE_SIZE);
  paging_set_page_in_space(t->pml4_phys, va, fresh,
                           vmm_user_flags_from_prot((int)vma->prot) |
                               VMM_PKEY_BITS(vma->pkey));
  tlb_shootdown_current_mm();
  pmm_free_frame(old);
  return 1;
}

/* ── the system calls ────────────────────────────────────────────────────── */

isize mempolicy_mbind(u64 start, u64 len, u64 mode, u64 nmask, u64 maxnode,
                      u64 flags) {
  struct task *t = current_task;
  u16 mask = 0;
  int m;
  int misplaced = 0;
  u64 end;

  if (start & (PAGE_SIZE - 1))
    return -EINVAL;
  if (flags & ~(u64)(MPOL_MF_STRICT | MPOL_MF_MOVE | MPOL_MF_MOVE_ALL))
    return -EINVAL;
  if ((flags & MPOL_MF_MOVE_ALL) && !cred_has_cap(t->cred, CAP_SYS_NICE))
    return -EPERM;
  m = check_policy(mode, nmask, maxnode, &mask);
  if (m < 0)
    return m;
  len = (len + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
  if (start + len < start || start + len > USER_SPACE_LIMIT)
    return -EINVAL;
  if (len == 0)
    return 0;
  if (!range_mapped(start, len))
    return -EFAULT;
  end = start + len;

  /* Record the policy on every mapping the range covers, splitting the ones
   * it covers only partly — a policy that spilled onto the neighbours would
   * move memory the caller never named. */
  for (u64 a = start; a < end;) {
    struct vm_area *v = vma_lookup(t, a);

    if (!v)
      break;
    if (v->start < a)
      v = vma_split(t, v, a);
    if (!v)
      return -ENOMEM;
    if (v->end > end) {
      if (!vma_split(t, v, end))
        return -ENOMEM;
    }
    v->mpol_mode = (u8)m;
    v->mpol_flags = (u8)((mode & MPOL_MODE_FLAGS) >> 13);
    v->mpol_nodes = mask;
    a = v->end;
  }

  /* What is already there. MPOL_MF_MOVE moves it; MPOL_MF_STRICT without it
   * reports that pages sit outside the policy, which is what EIO means to
   * mbind(2). */
  if (numa_node_count() > 1 && (flags & (MPOL_MF_STRICT | MPOL_MF_MOVE))) {
    int target = -1;
    int dummy = 0;

    if (m == MPOL_BIND || m == MPOL_PREFERRED || m == MPOL_PREFERRED_MANY)
      target = first_node(mask);
    for (u64 a = start; a < end; a += PAGE_SIZE) {
      struct vm_area *v = vma_lookup(t, a);
      int node;

      if (!v)
        break;
      if (m == MPOL_INTERLEAVE || m == MPOL_WEIGHTED_INTERLEAVE) {
        u16 cursor = (u16)(a / PAGE_SIZE);

        target = interleave_node(mask, &cursor);
      }
      if (target < 0)
        continue;
      {
        /* Same reason as get_mempolicy above: a 2 MiB entry has no leaf. */
        u64 phys = paging_user_phys(t->pml4_phys, a);

        if (!phys)
          continue;
        node = numa_node_of_frame(phys & ~(u64)(PAGE_SIZE - 1));
      }
      if (node == target)
        continue;
      if (flags & MPOL_MF_MOVE) {
        if (migrate_page(t, v, a, target) < 0)
          misplaced = 1;
      } else {
        misplaced = 1;
      }
    }
    (void)dummy;
  }
  if (misplaced && (flags & MPOL_MF_STRICT))
    return -EIO;
  return 0;
}

isize mempolicy_set(u64 mode, u64 nmask, u64 maxnode) {
  u16 mask = 0;
  int m = check_policy(mode, nmask, maxnode, &mask);
  struct mempolicy_task *s;

  if (m < 0)
    return m;
  s = pol_cur(1);
  if (s) {
    s->mode = (u8)m;
    s->flags = (u16)(mode & MPOL_MODE_FLAGS);
    s->nodes = mask;
    s->il_next = 0;
  }
  return 0;
}

isize mempolicy_get(u64 umode, u64 nmask, u64 maxnode, u64 addr, u64 flags) {
  struct mempolicy_task *s = pol_cur(0);
  struct vm_area *v = 0;
  u8 mode = s ? s->mode : 0;
  u16 mask = s ? s->nodes : 0;
  u16 pflags = s ? s->flags : 0;
  int report;

  if (flags & ~(u64)(MPOL_F_NODE | MPOL_F_ADDR | MPOL_F_MEMS_ALLOWED))
    return -EINVAL;
  if (nmask && maxnode < 1)
    return -EINVAL;
  if (flags & MPOL_F_MEMS_ALLOWED) {
    int nodes = numa_node_count();
    u16 all = (u16)((nodes >= 16) ? 0xffff : ((1u << nodes) - 1));

    if (flags & (MPOL_F_NODE | MPOL_F_ADDR))
      return -EINVAL;
    if (umode) {
      int zero = 0;

      if (syscall_copyout((void *)(usize)umode, &zero, sizeof(zero)))
        return -EFAULT;
    }
    return write_nodemask(nmask, maxnode, all);
  }
  if (!(flags & MPOL_F_ADDR) && addr)
    return -EINVAL;
  if (flags & MPOL_F_ADDR) {
    u64 page = addr & ~(u64)(PAGE_SIZE - 1);

    if (!range_mapped(page, PAGE_SIZE))
      return -EFAULT;
    v = vma_lookup(current_task, page);
    if (v && v->mpol_mode) {
      mode = v->mpol_mode;
      mask = v->mpol_nodes;
      pflags = (u16)(v->mpol_flags << 13);
    }
  }
  if (flags & MPOL_F_NODE) {
    /* With MPOL_F_ADDR: the node the page is actually on. Without it, and on
     * an interleaving policy, the node the next allocation would use. */
    int node = 0;

    if (flags & MPOL_F_ADDR) {
      /* Through the huge-page-aware walk. paging_leaf_pte answers only for a
       * 4 KiB leaf and reports a transparent huge page as no mapping at all,
       * so get_mempolicy over one used to say EFAULT or name node 0 (M128). */
      u64 phys = paging_user_phys(
          current_task ? current_task->pml4_phys : 0, addr);

      if (!phys)
        return -EFAULT;
      node = numa_node_of_frame(phys & ~(u64)(PAGE_SIZE - 1));
    } else if (mode == MPOL_INTERLEAVE || mode == MPOL_WEIGHTED_INTERLEAVE) {
      u16 cursor = s ? s->il_next : 0;

      node = interleave_node(mask, &cursor);
      if (node < 0)
        node = 0;
    } else {
      return -EINVAL; /* Linux: MPOL_F_NODE without either is not defined */
    }
    report = node;
    if (umode && syscall_copyout((void *)(usize)umode, &report, sizeof(report)))
      return -EFAULT;
    return write_nodemask(nmask, maxnode, mask);
  }
  report = (int)(mode | pflags);
  if (umode && syscall_copyout((void *)(usize)umode, &report, sizeof(report)))
    return -EFAULT;
  return write_nodemask(nmask, maxnode, mask);
}

isize mempolicy_home_node(u64 start, u64 len, u64 node, u64 flags) {
  struct task *t = current_task;
  u64 end;

  if (start & (PAGE_SIZE - 1))
    return -EINVAL;
  if (flags)
    return -EINVAL;
  if ((int)node < 0 || (int)node >= numa_node_count())
    return -EINVAL;
  len = (len + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
  if (start + len < start)
    return -EINVAL;
  if (len == 0)
    return 0;
  if (!range_mapped(start, len))
    return -ENOENT;
  end = start + len;
  for (u64 a = start; a < end;) {
    struct vm_area *v = vma_lookup(t, a);

    if (!v)
      break;
    if (v->start < a)
      v = vma_split(t, v, a);
    if (!v)
      return -ENOMEM;
    if (v->end > end && !vma_split(t, v, end))
      return -ENOMEM;
    v->mpol_home = (u16)(node + 1);
    a = v->end;
  }
  return 0;
}
