/* SPDX-License-Identifier: GPL-2.0-only */
/* PID namespaces (M109, completed in M123).
 *
 * The kernel numbers tasks once, globally, with ids that are never reused. A
 * PID namespace is a translation over that: a task created into namespace N is
 * given a number in N and in every namespace between N and the initial one,
 * because an ancestor must be able to name (signal, wait for, trace) a task
 * inside a namespace it created. A namespace that is not an ancestor has no
 * number for the task, and therefore no way to name it — that is the isolation.
 * The initial namespace needs no table: there the two numbers are the same.
 *
 * The numbers live in one table of (namespace, kernel id, number) entries with
 * two hash chains, one per direction. A reaped task's entries are kept for a
 * while: waitpid(2) reaps and only then translates the pid it returns.
 *
 * The first process numbered in a namespace is its init (number 1). It adopts
 * the namespace's orphans, it is not killed by signals it has no handler for
 * unless an ancestor namespace sends SIGKILL or SIGSTOP, and when it exits
 * every other member is killed and the namespace refuses new ones.
 */

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/sched.h>
#include <string.h>

#include "ns_internal.h"

#define PIDMAP_ENTRIES 8192
#define PIDMAP_BUCKETS 4096
/* Reaped tasks whose numbers are still answerable. */
#define PIDMAP_DEAD_RING 1024

struct pidmap_ent {
  usize gid;  /* kernel id */
  usize vnr;  /* number in `ns` */
  u32 gen;    /* bumped on reuse, so the dead ring never frees a newer entry */
  u16 ns;
  u8 used;
  u8 dead;
  u16 next_g; /* chain links, index + 1 */
  u16 next_v;
};

struct pidns_data {
  u32 parent;
  u32 level;
  usize next_vnr;
  usize reaper;    /* kernel id of the namespace's init */
  u8 has_reaper;
  u8 reaper_gone;  /* init has exited */
  u8 dying;        /* no task may join any more */
};

static struct pidmap_ent pidmap[PIDMAP_ENTRIES];
static u16 head_g[PIDMAP_BUCKETS];
static u16 head_v[PIDMAP_BUCKETS];
static u16 free_head; /* index + 1, linked through next_g */
static int pidmap_ready;
static struct {
  u16 idx; /* index + 1 */
  u32 gen;
} dead_ring[PIDMAP_DEAD_RING];
static u32 dead_pos;

static struct pidns_data pidns[NS_MAX_PID];

/* Namespaces whose init just exited and whose members still have to be
 * killed — outside ns_lock. */
#define PIDNS_ZAP_MAX 8
static u32 zap_pending[PIDNS_ZAP_MAX];
static u32 zap_count;

static inline u32 hash_g(usize gid, u32 ns) {
  return (u32)((gid * 2654435761u) ^ (ns * 40503u)) & (PIDMAP_BUCKETS - 1);
}

static inline u32 hash_v(usize vnr, u32 ns) {
  return (u32)((vnr * 2246822519u) ^ (ns * 3266489917u)) &
         (PIDMAP_BUCKETS - 1);
}

static void pidmap_ensure_init_locked(void) {
  if (pidmap_ready)
    return;
  pidmap_ready = 1;
  for (u32 i = 0; i < PIDMAP_ENTRIES; i++)
    pidmap[i].next_g = (i + 1 < PIDMAP_ENTRIES) ? (u16)(i + 2) : 0;
  free_head = 1;
}

static void chain_unlink(u16 *head, u32 idx, int by_gid) {
  u16 *link = head;
  while (*link) {
    u32 cur = (u32)*link - 1;
    u16 *next = by_gid ? &pidmap[cur].next_g : &pidmap[cur].next_v;
    if (cur == idx) {
      *link = *next;
      return;
    }
    link = next;
  }
}

static void pidmap_free_locked(u32 idx) {
  struct pidmap_ent *e = &pidmap[idx];
  if (!e->used)
    return;
  chain_unlink(&head_g[hash_g(e->gid, e->ns)], idx, 1);
  chain_unlink(&head_v[hash_v(e->vnr, e->ns)], idx, 0);
  e->used = 0;
  e->dead = 0;
  e->next_v = 0;
  e->next_g = free_head;
  free_head = (u16)(idx + 1);
}

static int pidmap_add_locked(u32 ns, usize gid, usize vnr) {
  pidmap_ensure_init_locked();
  if (!free_head)
    return -ENOSPC;
  u32 idx = (u32)free_head - 1;
  struct pidmap_ent *e = &pidmap[idx];
  free_head = e->next_g;
  e->gid = gid;
  e->vnr = vnr;
  e->ns = (u16)ns;
  e->used = 1;
  e->dead = 0;
  e->gen++;
  u32 hg = hash_g(gid, ns), hv = hash_v(vnr, ns);
  e->next_g = head_g[hg];
  head_g[hg] = (u16)(idx + 1);
  e->next_v = head_v[hv];
  head_v[hv] = (u16)(idx + 1);
  return 0;
}

static struct pidmap_ent *find_by_gid_locked(u32 ns, usize gid) {
  if (!pidmap_ready)
    return 0;
  for (u16 l = head_g[hash_g(gid, ns)]; l; l = pidmap[l - 1].next_g) {
    struct pidmap_ent *e = &pidmap[l - 1];
    if (e->gid == gid && e->ns == ns)
      return e;
  }
  return 0;
}

static struct pidmap_ent *find_by_vnr_locked(u32 ns, usize vnr) {
  if (!pidmap_ready)
    return 0;
  for (u16 l = head_v[hash_v(vnr, ns)]; l; l = pidmap[l - 1].next_v) {
    struct pidmap_ent *e = &pidmap[l - 1];
    if (e->vnr == vnr && e->ns == ns)
      return e;
  }
  return 0;
}

/* ── namespace objects ──────────────────────────────────────────────────── */

void pidns_init_locked(u32 id, u32 parent) {
  pidmap_ensure_init_locked();
  struct pidns_data *d = &pidns[id];
  memset(d, 0, sizeof(*d));
  d->next_vnr = 1;
  if (id == 0)
    return;
  d->parent = parent;
  d->level = pidns[parent].level + 1;
  ns_get_locked(NS_PID, parent);
}

void pidns_release_locked(u32 id) {
  if (id == 0 || id >= NS_MAX_PID)
    return;
  for (u32 i = 0; i < PIDMAP_ENTRIES; i++)
    if (pidmap[i].used && pidmap[i].ns == id)
      pidmap_free_locked(i);
  u32 parent = pidns[id].parent;
  memset(&pidns[id], 0, sizeof(pidns[id]));
  ns_put_locked(NS_PID, parent);
}

u32 pidns_level_locked(u32 ns) {
  return ns < NS_MAX_PID ? pidns[ns].level : 0;
}

u32 pidns_parent_locked(u32 ns) {
  return ns < NS_MAX_PID ? pidns[ns].parent : 0;
}

int pidns_dying_locked(u32 ns) {
  return ns && ns < NS_MAX_PID && pidns[ns].dying;
}

int pidns_is_ancestor_locked(u32 ancestor, u32 of) {
  if (ancestor >= NS_MAX_PID || of >= NS_MAX_PID)
    return 0;
  for (int depth = 0; depth <= NS_MAX_LEVEL + 1; depth++) {
    if (of == ancestor)
      return 1;
    if (of == 0)
      return 0;
    of = pidns[of].parent;
  }
  return 0;
}

/* ── membership ─────────────────────────────────────────────────────────── */

void pidns_enter_locked(u32 ns, struct task *t, int is_thread) {
  if (!t || ns == 0 || ns >= NS_MAX_PID)
    return;
  usize gid = t->id;
  for (u32 n = ns; n != 0; n = pidns[n].parent) {
    if (find_by_gid_locked(n, gid))
      continue;
    usize vnr = pidns[n].next_vnr;
    if (pidmap_add_locked(n, gid, vnr) != 0) {
      console_write("pidns: number table full; task ");
      console_write_dec(gid);
      console_write(" has no number in a namespace\n");
      break;
    }
    pidns[n].next_vnr = vnr + 1;
    /* Only the innermost namespace's first process is its init. */
    if (n == ns && !is_thread && !pidns[n].has_reaper) {
      pidns[n].reaper = gid;
      pidns[n].has_reaper = 1;
    }
  }
}

void pidns_task_exit_locked(struct task *t, u32 ns) {
  if (!t || ns == 0 || ns >= NS_MAX_PID)
    return;
  struct pidns_data *d = &pidns[ns];
  if (!d->has_reaper || d->reaper != t->id || d->reaper_gone)
    return;
  d->reaper_gone = 1;
  d->dying = 1;
  /* Every descendant namespace goes with it: their members are members of
   * this one too, and are killed by the same pass. */
  for (u32 i = 1; i < NS_MAX_PID; i++)
    if (ns_slots[NS_PID][i].used && pidns_is_ancestor_locked(ns, i))
      pidns[i].dying = 1;
  if (zap_count < PIDNS_ZAP_MAX)
    zap_pending[zap_count++] = ns;
}

void pidns_task_reaped_locked(usize gid) {
  if (!pidmap_ready)
    return;
  for (u32 n = 1; n < NS_MAX_PID; n++) {
    if (!ns_slots[NS_PID][n].used)
      continue;
    struct pidmap_ent *e = find_by_gid_locked(n, gid);
    if (!e || e->dead)
      continue;
    e->dead = 1;
    u32 idx = (u32)(e - pidmap);
    /* The ring keeps the number answerable for the parent that is about to
     * translate it; the entry it pushes out is freed. */
    u32 slot = dead_pos++ % PIDMAP_DEAD_RING;
    if (dead_ring[slot].idx) {
      struct pidmap_ent *old = &pidmap[dead_ring[slot].idx - 1];
      if (old->used && old->dead && old->gen == dead_ring[slot].gen)
        pidmap_free_locked(dead_ring[slot].idx - 1u);
    }
    dead_ring[slot].idx = (u16)(idx + 1);
    dead_ring[slot].gen = e->gen;
  }
}

/* Kill every member of the namespaces whose init has exited. */
void pidns_run_zaps(void) {
  for (;;) {
    u64 f;
    spin_lock_irqsave(&ns_lock, &f);
    if (zap_count == 0) {
      spin_unlock_irqrestore(&ns_lock, f);
      return;
    }
    u32 ns = zap_pending[--zap_count];
    spin_unlock_irqrestore(&ns_lock, f);

    usize max = scheduler_max_task_slots();
    for (usize i = 0; i < max; i++) {
      struct task *t = scheduler_task_slot(i);
      if (!t || t->state == TASK_UNUSED || t->state == TASK_DEAD ||
          t->state == TASK_REAPING || t == current_task)
        continue;
      spin_lock_irqsave(&ns_lock, &f);
      struct ns_row *r = ns_row_of(t);
      u32 tns = (r && r->used) ? r->id[NS_PID] : 0;
      int member = tns && pidns_is_ancestor_locked(ns, tns);
      usize id = t->id;
      spin_unlock_irqrestore(&ns_lock, f);
      if (member)
        scheduler_kill(id, SIGKILL);
    }
  }
}

/* ── translation ────────────────────────────────────────────────────────── */

static u32 active_pidns(const struct task *t) {
  const struct ns_row *r = ns_row_of(t);
  return (r && r->used) ? r->id[NS_PID] : 0;
}

usize namespace_pid_to_ns(u32 ns, usize kernel_pid) {
  if (ns == 0 || kernel_pid == 0)
    return kernel_pid;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  struct pidmap_ent *e = find_by_gid_locked(ns, kernel_pid);
  usize v = e ? e->vnr : 0;
  spin_unlock_irqrestore(&ns_lock, f);
  return v;
}

usize namespace_pid_from_ns(u32 ns, usize user_pid) {
  if (ns == 0 || user_pid == 0)
    return user_pid;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  struct pidmap_ent *e = find_by_vnr_locked(ns, user_pid);
  usize g = e ? e->gid : 0;
  spin_unlock_irqrestore(&ns_lock, f);
  return g;
}

usize namespace_pid_to_user(usize kernel_pid) {
  if (!ns_any)
    return kernel_pid;
  return namespace_pid_to_ns(active_pidns(current_task), kernel_pid);
}

usize namespace_pid_from_user(usize user_pid) {
  if (!ns_any)
    return user_pid;
  return namespace_pid_from_ns(active_pidns(current_task), user_pid);
}

int namespace_pid_visible(usize kernel_pid) {
  return namespace_pid_to_user(kernel_pid) != 0;
}

int namespace_pid_visible_from(usize observer_pid, usize kernel_pid) {
  if (!ns_any)
    return 1;
  struct task *o = scheduler_task_by_pid(observer_pid);
  u32 ns = o ? active_pidns(o) : 0;
  return ns == 0 || namespace_pid_to_ns(ns, kernel_pid) != 0;
}

u32 namespace_pid_level(u32 ns) {
  return ns < NS_MAX_PID ? pidns[ns].level : 0;
}

int namespace_pid_chain(usize kernel_pid, u32 from_ns, usize *out, int max) {
  if (!out || max <= 0)
    return 0;
  struct task *t = scheduler_task_by_pid(kernel_pid);
  u32 own = t ? active_pidns(t) : 0;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  /* Walk from the task's own namespace up to the reader's, then emit in the
   * other order (outermost first). */
  u32 path[NS_MAX_LEVEL + 1];
  int n = 0;
  int reached = 0;
  for (u32 ns = own; n <= NS_MAX_LEVEL; ns = pidns[ns].parent) {
    path[n++] = ns;
    if (ns == from_ns) {
      reached = 1;
      break;
    }
    if (ns == 0)
      break;
  }
  int count = 0;
  if (reached) {
    for (int i = n - 1; i >= 0 && count < max; i--) {
      struct pidmap_ent *e =
          path[i] ? find_by_gid_locked(path[i], kernel_pid) : 0;
      usize v = path[i] ? (e ? e->vnr : 0) : kernel_pid;
      out[count++] = v;
    }
  }
  spin_unlock_irqrestore(&ns_lock, f);
  return count;
}

usize namespace_pid_reaper(u32 ns) {
  if (ns == 0 || ns >= NS_MAX_PID)
    return 0;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  usize r = (pidns[ns].has_reaper && !pidns[ns].reaper_gone) ? pidns[ns].reaper
                                                             : 0;
  spin_unlock_irqrestore(&ns_lock, f);
  return r;
}

usize namespace_pid_orphan_reaper(usize orphan_pid) {
  if (!ns_any)
    return 0;
  struct task *t = scheduler_task_by_pid(orphan_pid);
  if (!t)
    return 0;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  usize reaper = 0;
  for (u32 ns = active_pidns(t); ns != 0; ns = pidns[ns].parent) {
    struct pidns_data *d = &pidns[ns];
    if (d->has_reaper && !d->reaper_gone && d->reaper != orphan_pid) {
      reaper = d->reaper;
      break;
    }
  }
  spin_unlock_irqrestore(&ns_lock, f);
  return reaper;
}

int namespace_pid_signal_allowed(const struct task *target, int sig,
                                 int has_handler) {
  if (!ns_any || !target || sig == 0 || has_handler)
    return 1;
  u32 tns = active_pidns(target);
  if (tns == 0)
    return 1;
  usize leader = task_tgid(target);
  if (!leader)
    leader = target->id;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  int is_init = pidns[tns].has_reaper && pidns[tns].reaper == leader;
  u32 sender_ns = active_pidns(current_task);
  /* A sender with no number in the target's namespace is in an ancestor. */
  int from_ancestor =
      sender_ns != tns && pidns_is_ancestor_locked(sender_ns, tns);
  spin_unlock_irqrestore(&ns_lock, f);
  if (!is_init)
    return 1;
  return from_ancestor && (sig == SIGKILL || sig == SIGSTOP);
}
