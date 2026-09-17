/*
 * Memory protection keys (x86 PKU).
 *
 * A user page carries a 4-bit key in its entry (bits 62:59), and each thread's
 * PKRU register holds two bits per key: access-disable and write-disable. The
 * CPU checks them on every data access to a user page, from ring 3 and from the
 * kernel alike, so the kernel keeps each thread's PKRU loaded while it runs,
 * user value and all, as Linux does.
 *
 * What is kept here:
 *   - per thread: the PKRU value, saved and loaded at every context switch;
 *   - per address space: which keys are allocated, and the key execute-only
 *     mappings share (Linux's mm->context.pkey_allocation_map and
 *     execute_only_pkey), keyed by the address space's root table.
 *
 * The kernel copies to and from user memory without an exception fixup, so a
 * key the copy has not been cleared for would fault in kernel mode. The copy
 * helpers check the keys first and fail with EFAULT, exactly the result Linux's
 * fixup gives; the only access that can still fault is one racing another
 * thread's pkey_mprotect, and that one is let through (arch_pkru_kernel_fault)
 * with the thread's own value put back before it returns to user mode.
 */
#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/pkeys.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <string.h>

#define PKRU_AD 0x1u
#define PKRU_WD 0x2u
#define PKEYS_MAX 16
/* Every key but 0 access-disabled: what a new program starts with. */
#define PKRU_INIT 0x55555554u

static int g_pku;

int arch_pkeys_enabled(void) { return g_pku; }

static inline u32 rdpkru(void) {
  u32 eax, edx;
  __asm__ volatile("rdpkru" : "=a"(eax), "=d"(edx) : "c"(0));
  (void)edx;
  return eax;
}

static inline void wrpkru(u32 v) {
  __asm__ volatile("wrpkru" : : "a"(v), "c"(0), "d"(0));
}

/* Per CPU, like every CR4 bit: all of them or none, which the control-register
 * census after bring-up checks. */
void x86_enable_pku(int bsp) {
  u32 a, b, c, d;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
  if (a < 7)
    return;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
  if (!(c & (1u << 3)))
    return;
  if (!bsp && !g_pku)
    return; /* the boot CPU did not: keep the cores alike */
  u64 cr4;
  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  cr4 |= 1ULL << 22; /* PKE */
  __asm__ volatile("movq %0, %%cr4" : : "r"(cr4) : "memory");
  wrpkru(0);
  if (bsp) {
    g_pku = 1;
    console_write("pku: protection keys enabled\n");
  }
}

/* ── per thread ─────────────────────────────────────────────────────── */

struct pkru_slot {
  u32 live;
  /* Set while the kernel runs with keys cleared for a racing copy; `user` is
   * the value to put back before the thread returns to user mode. */
  u32 user;
  u8 relaxed;
};
static struct pkru_slot *g_pkru;
static usize g_pkru_slots;

/* The table is sized from the task table on first use in process context;
 * `create` is 0 on the context-switch path, which must not allocate. Until it
 * exists every thread's value is the reset value, 0. */
static struct pkru_slot *pkru_slot_in(struct task *t, int create) {
  if (!g_pkru && create) {
    usize n = scheduler_max_task_slots();
    struct pkru_slot *s = kzalloc(n * sizeof(*s));
    if (s && !__atomic_compare_exchange_n(&g_pkru, &(struct pkru_slot *){0}, s,
                                          0, __ATOMIC_ACQUIRE,
                                          __ATOMIC_RELAXED))
      kfree(s);
    else if (s)
      g_pkru_slots = n;
  }
  usize i = t ? scheduler_task_index(t) : (usize)-1;
  return (g_pkru && i < g_pkru_slots) ? &g_pkru[i] : 0;
}

static struct pkru_slot *pkru_slot(struct task *t) { return pkru_slot_in(t, 1); }

void arch_pkru_switch(struct task *prev, struct task *next) {
  if (!g_pku)
    return;
  struct pkru_slot *p = pkru_slot_in(prev, 0), *n = pkru_slot_in(next, 0);
  u32 cur = rdpkru();
  if (p)
    p->live = cur;
  u32 want = n ? n->live : 0;
  if (want != cur)
    wrpkru(want);
}

u32 arch_pkru_user_get(void) {
  if (!g_pku)
    return 0;
  struct pkru_slot *s = pkru_slot(current_task);
  return (s && s->relaxed) ? s->user : rdpkru();
}

void arch_pkru_user_set(u32 v) {
  if (!g_pku)
    return;
  struct pkru_slot *s = pkru_slot(current_task);
  if (s)
    s->relaxed = 0;
  wrpkru(v);
}

int arch_pkru_kernel_fault(void) {
  if (!g_pku || !current_task)
    return 0;
  struct pkru_slot *s = pkru_slot(current_task);
  if (!s)
    return 0;
  u32 cur = rdpkru();
  if (!s->relaxed) {
    s->user = cur;
    s->relaxed = 1;
  }
  if (cur == 0)
    return 0; /* nothing left to clear: a genuine fault */
  wrpkru(0);
  return 1;
}

void arch_pkru_return_to_user(void) {
  if (!g_pku)
    return;
  struct pkru_slot *s = pkru_slot(current_task);
  if (s && s->relaxed) {
    s->relaxed = 0;
    wrpkru(s->user);
  }
}

void arch_pkru_fork(struct task *parent, struct task *child) {
  if (!g_pku)
    return;
  struct pkru_slot *c = pkru_slot(child);
  if (c) {
    c->live = parent == current_task ? arch_pkru_user_get()
                                     : (pkru_slot(parent) ? pkru_slot(parent)->live
                                                          : PKRU_INIT);
    c->relaxed = 0;
  }
}

/* ── per address space ─────────────────────────────────────────────── */

struct pkey_mm {
  u64 root;
  u16 allocated;
  i8 exec_only; /* -1: none yet */
};
static spinlock_t g_mm_lock = SPINLOCK_INIT;
static struct pkey_mm *g_mm;
static usize g_mm_cap;

/* The entry for `root`, created when `create`. Called with g_mm_lock held;
 * grows the table only from a caller that may allocate. */
static struct pkey_mm *mm_find(u64 root) {
  for (usize i = 0; i < g_mm_cap; i++)
    if (g_mm[i].root == root)
      return &g_mm[i];
  return 0;
}

static struct pkey_mm *mm_get(u64 root, u64 *flags) {
  if (!root)
    return 0; /* a kernel thread has no user address space */
  for (;;) {
    spin_lock_irqsave(&g_mm_lock, flags);
    struct pkey_mm *m = mm_find(root);
    if (m)
      return m;
    struct pkey_mm *free_slot = mm_find(0);
    if (free_slot) {
      free_slot->root = root;
      free_slot->allocated = 1; /* key 0 is every mapping's default */
      free_slot->exec_only = -1;
      return free_slot;
    }
    usize want = g_mm_cap ? g_mm_cap * 2 : 64;
    spin_unlock_irqrestore(&g_mm_lock, *flags);
    struct pkey_mm *grown = kzalloc(want * sizeof(*grown));
    if (!grown)
      return 0;
    struct pkey_mm *old = 0;
    spin_lock_irqsave(&g_mm_lock, flags);
    if (g_mm_cap < want) {
      memcpy(grown, g_mm, g_mm_cap * sizeof(*grown));
      old = g_mm;
      g_mm = grown;
      g_mm_cap = want;
      grown = 0;
    }
    spin_unlock_irqrestore(&g_mm_lock, *flags);
    kfree(grown ? grown : old);
  }
}

static u64 cur_root(void) {
  return current_task ? current_task->pml4_phys : 0;
}

void arch_pkeys_mm_release(u64 root) {
  if (!g_pku || !root)
    return;
  u64 flags;
  spin_lock_irqsave(&g_mm_lock, &flags);
  struct pkey_mm *m = mm_find(root);
  if (m)
    memset(m, 0, sizeof(*m));
  spin_unlock_irqrestore(&g_mm_lock, flags);
}

/* fork: the child's address space starts with the parent's keys. */
void arch_pkeys_mm_clone(u64 parent_root, u64 child_root) {
  if (!g_pku || !parent_root || !child_root)
    return;
  u64 flags;
  spin_lock_irqsave(&g_mm_lock, &flags);
  struct pkey_mm *p = mm_find(parent_root);
  struct pkey_mm copy = p ? *p : (struct pkey_mm){0};
  spin_unlock_irqrestore(&g_mm_lock, flags);
  arch_pkeys_mm_release(child_root);
  if (!p)
    return;
  struct pkey_mm *c = mm_get(child_root, &flags);
  if (c) {
    c->allocated = copy.allocated;
    c->exec_only = copy.exec_only;
    spin_unlock_irqrestore(&g_mm_lock, flags);
  }
}

void arch_pkeys_signal_rights(void) {
  if (g_pku)
    arch_pkru_user_set(PKRU_INIT);
}

/* execve: a new program starts with only key 0 and the initial PKRU. */
void arch_pkeys_exec(void) {
  if (!g_pku)
    return;
  arch_pkeys_mm_release(cur_root());
  arch_pkru_user_set(PKRU_INIT);
}

static void set_access(int pkey, u32 rights) {
  u32 shift = (u32)pkey * 2;
  u32 bits = 0;
  if (rights & PKEY_DISABLE_ACCESS)
    bits |= PKRU_AD;
  if (rights & PKEY_DISABLE_WRITE)
    bits |= PKRU_WD;
  u32 v = arch_pkru_user_get();
  v &= ~((PKRU_AD | PKRU_WD) << shift);
  arch_pkru_user_set(v | (bits << shift));
}

int arch_pkey_alloc(u32 rights) {
  if (!g_pku)
    return -ENOSPC;
  u64 flags;
  struct pkey_mm *m = mm_get(cur_root(), &flags);
  if (!m)
    return -ENOMEM;
  if (m->allocated == 0xffff) {
    spin_unlock_irqrestore(&g_mm_lock, flags);
    return -ENOSPC;
  }
  int k = __builtin_ctz((u32)(u16)~m->allocated);
  m->allocated |= (u16)(1u << k);
  spin_unlock_irqrestore(&g_mm_lock, flags);
  set_access(k, rights);
  return k;
}

static int allocated_locked(struct pkey_mm *m, int pkey) {
  if (pkey < 0 || pkey >= PKEYS_MAX)
    return 0;
  if (pkey == m->exec_only)
    return 1;
  return (m->allocated >> pkey) & 1;
}

int arch_pkey_is_allocated(int pkey) {
  if (!g_pku)
    return 0;
  u64 flags;
  struct pkey_mm *m = mm_get(cur_root(), &flags);
  if (!m)
    return 0;
  int r = allocated_locked(m, pkey);
  spin_unlock_irqrestore(&g_mm_lock, flags);
  return r;
}

int arch_pkey_free(int pkey) {
  if (!g_pku)
    return -EINVAL;
  u64 flags;
  struct pkey_mm *m = mm_get(cur_root(), &flags);
  if (!m)
    return -ENOMEM;
  int r = -EINVAL;
  if (allocated_locked(m, pkey)) {
    m->allocated &= (u16)~(1u << pkey);
    r = 0;
  }
  spin_unlock_irqrestore(&g_mm_lock, flags);
  return r;
}

/* The key PROT_EXEC-only mappings share: allocated on first use, and made
 * unreadable for the calling thread (Linux's __execute_only_pkey). -1 when
 * none can be had. */
int arch_execute_only_pkey(void) {
  if (!g_pku)
    return -1;
  u64 flags;
  struct pkey_mm *m = mm_get(cur_root(), &flags);
  if (!m)
    return -1;
  int k = m->exec_only;
  int fresh = 0;
  if (k < 0) {
    if (m->allocated == 0xffff) {
      spin_unlock_irqrestore(&g_mm_lock, flags);
      return -1;
    }
    k = __builtin_ctz((u32)(u16)~m->allocated);
    m->allocated |= (u16)(1u << k);
    m->exec_only = (i8)k;
    fresh = 1;
  }
  spin_unlock_irqrestore(&g_mm_lock, flags);
  if (fresh || !((arch_pkru_user_get() >> (k * 2)) & PKRU_AD))
    set_access(k, PKEY_DISABLE_ACCESS);
  return k;
}

int arch_pkey_is_exec_only(int pkey) {
  if (!g_pku || pkey <= 0)
    return 0;
  u64 flags;
  struct pkey_mm *m = mm_get(cur_root(), &flags);
  if (!m)
    return 0;
  int r = m->exec_only == pkey;
  spin_unlock_irqrestore(&g_mm_lock, flags);
  return r;
}

/* May the current thread read (or write) a user page carrying `pkey`? */
int arch_pkey_allows(int pkey, int write) {
  if (!g_pku)
    return 1;
  u32 bits = (arch_pkru_user_get() >> (pkey * 2)) & 3u;
  if (bits & PKRU_AD)
    return 0;
  return !(write && (bits & PKRU_WD));
}
