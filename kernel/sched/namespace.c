/* SPDX-License-Identifier: GPL-2.0-only */
/* Per-task namespaces: the core (M109, completed in M123).
 *
 * Eight kinds, all real:
 *
 *   UTS    — a hostname and a domainname.
 *   MOUNT  — a private copy of the VFS mount table (kernel/fs/vfs.c).
 *   PID    — a numbering over the kernel's flat task ids, with its own init
 *            (kernel/sched/pid_namespace.c).
 *   NET    — interfaces, routes, neighbours and socket bindings (kernel/net/).
 *   USER   — uid/gid maps and capabilities relative to a namespace
 *            (kernel/sched/user_namespace.c). Carried in the task's cred.
 *   IPC    — SysV IPC objects and POSIX message queues (kernel/ipc/).
 *   CGROUP — the cgroup a task's view of the hierarchy is rooted at.
 *   TIME   — offsets of CLOCK_MONOTONIC and CLOCK_BOOTTIME.
 *
 * This file holds what every kind shares: the id tables with their reference
 * counts and owners, per-task membership, the reaper that tears down the kinds
 * whose release takes locks a dying task must not take, clone/unshare/setns,
 * and nsfs — the objects /proc/<pid>/ns/<kind> resolves to.
 */

#include <b1nix/cgroup.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/vdso.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <string.h>

#include "ns_internal.h"

/* Linux CLONE_* bits unshare(2) accepts besides CLONE_NEW*. */
#define LX_CLONE_VM      0x00000100ULL
#define LX_CLONE_FS      0x00000200ULL
#define LX_CLONE_FILES   0x00000400ULL
#define LX_CLONE_SIGHAND 0x00000800ULL
#define LX_CLONE_PARENT  0x00008000ULL
#define LX_CLONE_THREAD  0x00010000ULL
#define LX_CLONE_SYSVSEM 0x00040000ULL

spinlock_t ns_lock = SPINLOCK_INIT;
int ns_any;

static struct ns_slot uts_slots[NS_MAX_UTS];
static struct ns_slot mnt_slots[NS_MAX_MNT];
static struct ns_slot pid_slots[NS_MAX_PID];
static struct ns_slot net_slots[NS_MAX_NET];
static struct ns_slot user_slots[NS_MAX_USER];
static struct ns_slot ipc_slots[NS_MAX_IPC];
static struct ns_slot cgroup_slots[NS_MAX_CGROUP];
static struct ns_slot time_slots[NS_MAX_TIME];

struct ns_slot *const ns_slots[NS_KIND_COUNT] = {
    [NS_UTS] = uts_slots,       [NS_MNT] = mnt_slots,
    [NS_PID] = pid_slots,       [NS_NET] = net_slots,
    [NS_USER] = user_slots,     [NS_IPC] = ipc_slots,
    [NS_CGROUP] = cgroup_slots, [NS_TIME] = time_slots,
};

const u32 ns_max[NS_KIND_COUNT] = {
    [NS_UTS] = NS_MAX_UTS,       [NS_MNT] = NS_MAX_MNT,
    [NS_PID] = NS_MAX_PID,       [NS_NET] = NS_MAX_NET,
    [NS_USER] = NS_MAX_USER,     [NS_IPC] = NS_MAX_IPC,
    [NS_CGROUP] = NS_MAX_CGROUP, [NS_TIME] = NS_MAX_TIME,
};

#define NS_NAME_MAX 65

struct uts_data {
  char host[NS_NAME_MAX];
  char domain[NS_NAME_MAX];
};
static struct uts_data uts_data[NS_MAX_UTS];

struct time_data {
  i64 monotonic;
  i64 boottime;
};
static struct time_data time_data[NS_MAX_TIME];

/* The cgroup each cgroup namespace is rooted at (a referenced struct cgroup,
 * opaque here). Slot 0 is the real root and stays NULL. */
static void *cgroup_roots[NS_MAX_CGROUP];

static struct ns_row ns_rows[SCHED_MAX_TASKS];

/* nsfs inode numbers of the initial namespaces are Linux's fixed ones, so a
 * program that compares against them (they are well known) gets the answer it
 * expects. Namespaces created later count up from past them. */
static const u64 ns_initial_inum[NS_KIND_COUNT] = {
    [NS_IPC] = 0xEFFFFFFFu,    [NS_UTS] = 0xEFFFFFFEu,
    [NS_USER] = 0xEFFFFFFDu,   [NS_PID] = 0xEFFFFFFCu,
    [NS_CGROUP] = 0xEFFFFFFBu, [NS_TIME] = 0xEFFFFFFAu,
    [NS_NET] = 0xF0000000u,    [NS_MNT] = 0xF0000001u,
};
static u64 ns_next_inum = 0xF0000002u;

static int ns_ready;

static void ns_copy_name(char *dst, usize len, const char *src) {
  if (!dst || len == 0)
    return;
  usize i = 0;
  if (src)
    for (; src[i] && i + 1 < len; i++)
      dst[i] = src[i];
  dst[i] = '\0';
}

/* Initial namespaces exist from the first use: there is no ordering guarantee
 * between the scheduler and an early sethostname(). */
static void ns_ensure_init_locked(void) {
  if (ns_ready)
    return;
  ns_ready = 1;
  for (int k = 0; k < NS_KIND_COUNT; k++) {
    ns_slots[k][0].used = 1;
    ns_slots[k][0].entered = 1;
    ns_slots[k][0].refs = 1; /* never released */
    ns_slots[k][0].owner = 0;
    ns_slots[k][0].inum = ns_initial_inum[k];
  }
  ns_copy_name(uts_data[0].host, sizeof(uts_data[0].host), "b1nix");
  ns_copy_name(uts_data[0].domain, sizeof(uts_data[0].domain), "(none)");
  pidns_init_locked(0, 0);
}

const char *namespace_kind_name(int kind) {
  switch (kind) {
  case NS_UTS: return "uts";
  case NS_MNT: return "mnt";
  case NS_PID: return "pid";
  case NS_NET: return "net";
  case NS_USER: return "user";
  case NS_IPC: return "ipc";
  case NS_CGROUP: return "cgroup";
  case NS_TIME: return "time";
  default: return "";
  }
}

int namespace_kind_from_name(const char *name) {
  if (!name)
    return -1;
  for (int k = 0; k < NS_KIND_COUNT; k++)
    if (strcmp(name, namespace_kind_name(k)) == 0)
      return k;
  return -1;
}

u64 namespace_kind_flag(int kind) {
  switch (kind) {
  case NS_UTS: return B1NIX_CLONE_NEWUTS;
  case NS_MNT: return B1NIX_CLONE_NEWNS;
  case NS_PID: return B1NIX_CLONE_NEWPID;
  case NS_NET: return B1NIX_CLONE_NEWNET;
  case NS_USER: return B1NIX_CLONE_NEWUSER;
  case NS_IPC: return B1NIX_CLONE_NEWIPC;
  case NS_CGROUP: return B1NIX_CLONE_NEWCGROUP;
  case NS_TIME: return B1NIX_CLONE_NEWTIME;
  default: return 0;
  }
}

int namespace_kind_from_flag(u64 flag) {
  for (int k = 0; k < NS_KIND_COUNT; k++)
    if (namespace_kind_flag(k) == flag)
      return k;
  return -1;
}

int namespace_active(void) { return ns_any; }

/* ── reference counting ─────────────────────────────────────────────────── */

/* Mount, network, IPC and cgroup namespaces cannot be torn down where their
 * last reference is usually dropped — an exiting task, with the VFS and socket
 * locks off limits. They become zombies and this thread finishes them. */
static void *ns_reaper_chan = (void *)&ns_reaper_chan;
static int ns_reaper_started;
static volatile int ns_reaper_pending;

static int ns_kind_deferred(int kind) {
  return kind == NS_MNT || kind == NS_NET || kind == NS_IPC ||
         kind == NS_CGROUP;
}

u32 ns_alloc_locked(int kind, u32 owner) {
  ns_ensure_init_locked();
  struct ns_slot *s = ns_slots[kind];
  for (u32 i = 1; i < ns_max[kind]; i++) {
    if (s[i].used)
      continue;
    s[i].used = 1;
    s[i].zombie = 0;
    s[i].entered = 0;
    s[i].refs = 1;
    s[i].owner = owner;
    s[i].inum = ns_next_inum++;
    /* A user namespace's owner is its parent; either way the reference keeps
     * the owner alive for as long as this namespace is. */
    ns_get_locked(NS_USER, owner);
    ns_any = 1;
    return i;
  }
  return 0;
}

void ns_get_locked(int kind, u32 id) {
  if (id == 0 || kind < 0 || kind >= NS_KIND_COUNT || id >= ns_max[kind])
    return;
  ns_slots[kind][id].refs++;
}

void ns_put_locked(int kind, u32 id) {
  if (id == 0 || kind < 0 || kind >= NS_KIND_COUNT || id >= ns_max[kind])
    return;
  struct ns_slot *s = &ns_slots[kind][id];
  if (!s->used || s->refs == 0) {
    console_write("namespace: reference underflow on ");
    console_write(namespace_kind_name(kind));
    console_write(" namespace ");
    console_write_dec(id);
    console_write("\n");
    return;
  }
  if (--s->refs != 0)
    return;
  if (ns_kind_deferred(kind)) {
    s->zombie = 1;
    ns_reaper_pending = 1;
    return;
  }
  u32 owner = s->owner;
  switch (kind) {
  case NS_PID:
    pidns_release_locked(id);
    break;
  case NS_USER:
    userns_release_locked(id);
    break;
  case NS_UTS:
    memset(&uts_data[id], 0, sizeof(uts_data[id]));
    break;
  case NS_TIME:
    memset(&time_data[id], 0, sizeof(time_data[id]));
    break;
  default:
    break;
  }
  s->used = 0;
  ns_put_locked(NS_USER, owner);
}

static int ns_live_locked(int kind, u32 id) {
  if (kind < 0 || kind >= NS_KIND_COUNT || id >= ns_max[kind])
    return 0;
  if (id == 0)
    return 1;
  return ns_slots[kind][id].used && !ns_slots[kind][id].zombie;
}

static void ns_kick_reaper(void);

int namespace_get(int kind, u32 id) {
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  ns_ensure_init_locked();
  int ok = ns_live_locked(kind, id);
  if (ok)
    ns_get_locked(kind, id);
  spin_unlock_irqrestore(&ns_lock, f);
  return ok ? 0 : -EINVAL;
}

void namespace_put(int kind, u32 id) {
  if (id == 0)
    return;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  ns_put_locked(kind, id);
  spin_unlock_irqrestore(&ns_lock, f);
  ns_kick_reaper();
}

u64 namespace_inum(int kind, u32 id) {
  if (kind < 0 || kind >= NS_KIND_COUNT || id >= ns_max[kind])
    return 0;
  if (id == 0)
    return ns_initial_inum[kind];
  return ns_slots[kind][id].inum;
}

u32 namespace_owner(int kind, u32 id) {
  if (kind < 0 || kind >= NS_KIND_COUNT || id >= ns_max[kind] || id == 0)
    return 0;
  return ns_slots[kind][id].owner;
}

static void ns_reaper_thread(void *arg) {
  (void)arg;
  for (;;) {
    u64 f;
    spin_lock_irqsave(&ns_lock, &f);
    if (!ns_reaper_pending) {
      scheduler_wait_prepare(ns_reaper_chan);
      spin_unlock_irqrestore(&ns_lock, f);
      scheduler_wait_commit();
      continue;
    }
    ns_reaper_pending = 0;
    /* Collect under the lock, tear down outside it, then free the slots: the
     * ids must stay allocated until nothing in the VFS or the net layer can
     * still carry them, or a new namespace would inherit the leftovers. */
    struct {
      int kind;
      u32 id;
      void *cg;
    } dead[32];
    int n = 0;
    for (int k = 0; k < NS_KIND_COUNT && n < (int)(sizeof(dead) / sizeof(dead[0])); k++) {
      if (!ns_kind_deferred(k))
        continue;
      for (u32 i = 1; i < ns_max[k] && n < (int)(sizeof(dead) / sizeof(dead[0])); i++) {
        struct ns_slot *s = &ns_slots[k][i];
        if (!s->used || !s->zombie || s->zombie == 2)
          continue;
        s->zombie = 2; /* being torn down */
        dead[n].kind = k;
        dead[n].id = i;
        dead[n].cg = 0;
        if (k == NS_CGROUP) {
          dead[n].cg = cgroup_roots[i];
          cgroup_roots[i] = 0;
        }
        n++;
      }
    }
    if (n == (int)(sizeof(dead) / sizeof(dead[0])))
      ns_reaper_pending = 1; /* more than one batch */
    spin_unlock_irqrestore(&ns_lock, f);

    for (int i = 0; i < n; i++) {
      switch (dead[i].kind) {
      case NS_MNT: vfs_mnt_ns_destroy(dead[i].id); break;
      case NS_NET: net_ns_destroy(dead[i].id); break;
      case NS_IPC: ipc_ns_destroy(dead[i].id); break;
      case NS_CGROUP: cgroup_ns_root_put(dead[i].cg); break;
      default: break;
      }
    }

    spin_lock_irqsave(&ns_lock, &f);
    for (int i = 0; i < n; i++) {
      struct ns_slot *s = &ns_slots[dead[i].kind][dead[i].id];
      u32 owner = s->owner;
      s->used = 0;
      s->zombie = 0;
      ns_put_locked(NS_USER, owner);
    }
    spin_unlock_irqrestore(&ns_lock, f);
  }
}

static void ns_kick_reaper(void) {
  if (!__atomic_load_n(&ns_reaper_pending, __ATOMIC_RELAXED))
    return;
  if (!ns_reaper_started) {
    if (!scheduler_can_block())
      return; /* the next kick from a task context starts it */
    ns_reaper_started = 1;
    if (kthread_create("ns-reaper", ns_reaper_thread, 0) < 0) {
      ns_reaper_started = 0;
      return;
    }
  }
  scheduler_wake_all(ns_reaper_chan);
}

/* ── per-task membership ────────────────────────────────────────────────── */

struct ns_row *ns_row_of(const struct task *t) {
  if (!t)
    return 0;
  usize idx = task_slot_index(t);
  if (idx >= SCHED_MAX_TASKS)
    return 0;
  return &ns_rows[idx];
}

u32 namespace_task_id(const struct task *t, int kind) {
  if (!t || kind < 0 || kind >= NS_KIND_COUNT)
    return 0;
  if (kind == NS_USER)
    return t->cred ? cred_userns(t->cred) : 0;
  if (!ns_any)
    return 0;
  const struct ns_row *r = ns_row_of(t);
  return (r && r->used) ? r->id[kind] : 0;
}

u32 namespace_task_children_id(const struct task *t, int kind) {
  if (!ns_any || !t)
    return namespace_task_id(t, kind);
  const struct ns_row *r = ns_row_of(t);
  if (!r || !r->used)
    return namespace_task_id(t, kind);
  if (kind == NS_PID)
    return r->pid_children;
  if (kind == NS_TIME)
    return r->time_children;
  return namespace_task_id(t, kind);
}

u32 namespace_current_id(int kind) {
  if (kind != NS_USER && !ns_any)
    return 0;
  return namespace_task_id(current_task, kind);
}

u32 namespace_id_of(usize pid, int kind) {
  if (kind != NS_USER && !ns_any)
    return 0;
  struct task *t = scheduler_task_by_pid(pid);
  return t ? namespace_task_id(t, kind) : 0;
}

/* Drop every reference a row holds and mark it unused. */
static void ns_row_clear_locked(struct ns_row *r) {
  if (!r->used)
    return;
  for (int k = 0; k < NS_KIND_COUNT; k++) {
    if (k != NS_USER)
      ns_put_locked(k, r->id[k]);
    r->id[k] = 0;
  }
  ns_put_locked(NS_PID, r->pid_children);
  ns_put_locked(NS_TIME, r->time_children);
  r->pid_children = 0;
  r->time_children = 0;
  if (r->child_valid)
    for (int k = 0; k < NS_KIND_COUNT; k++) {
      ns_put_locked(k, r->child[k]);
      r->child[k] = 0;
    }
  r->child_valid = 0;
  r->used = 0;
}

/* Does the row name anything but initial namespaces? */
static int ns_row_trivial(const struct ns_row *r) {
  for (int k = 0; k < NS_KIND_COUNT; k++)
    if (r->id[k])
      return 0;
  return !r->pid_children && !r->time_children && !r->child_valid;
}

void namespace_fork_inherit(struct task *parent, struct task *child,
                            u64 clone_flags) {
  struct ns_row *cr = ns_row_of(child);
  if (!cr)
    return;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  ns_ensure_init_locked();
  /* The slot may have been left by a task that never went through the exit
   * hooks (a kernel thread); nothing of that task may carry over. */
  ns_row_clear_locked(cr);

  struct ns_row *pr = ns_row_of(parent);
  int thread = (clone_flags & LX_CLONE_THREAD) != 0;
  u16 ids[NS_KIND_COUNT] = {0};
  u16 given[NS_KIND_COUNT] = {0}; /* references handed over, not taken */
  if (pr && pr->used) {
    for (int k = 0; k < NS_KIND_COUNT; k++)
      if (k != NS_USER)
        ids[k] = pr->id[k];
    if (!thread) {
      ids[NS_PID] = pr->pid_children;
      ids[NS_TIME] = pr->time_children;
    }
    if (pr->child_valid && !thread) {
      for (int k = 0; k < NS_KIND_COUNT; k++) {
        if (!pr->child[k])
          continue;
        if (k == NS_USER) {
          /* The child's credential is already a copy of the parent's; it moves
           * into the new namespace with the reference clone prepared. */
          if (child->cred)
            cred_enter_userns_locked(child->cred, pr->child[k]);
          else
            ns_put_locked(NS_USER, pr->child[k]);
        } else {
          ids[k] = pr->child[k];
          given[k] = 1;
        }
        pr->child[k] = 0;
      }
      pr->child_valid = 0;
    }
  }
  for (int k = 0; k < NS_KIND_COUNT; k++) {
    if (k == NS_USER)
      continue;
    cr->id[k] = ids[k];
    if (ids[k] && !given[k])
      ns_get_locked(k, ids[k]);
  }
  cr->pid_children = ids[NS_PID];
  ns_get_locked(NS_PID, ids[NS_PID]);
  cr->time_children = ids[NS_TIME];
  ns_get_locked(NS_TIME, ids[NS_TIME]);
  if (ids[NS_TIME])
    time_slots[ids[NS_TIME]].entered = 1;
  cr->used = !ns_row_trivial(cr);
  if (ids[NS_PID])
    pidns_enter_locked(ids[NS_PID], child, thread);
  u32 parent_time = (pr && pr->used) ? pr->id[NS_TIME] : 0;
  spin_unlock_irqrestore(&ns_lock, f);
  /* A child born into another time namespace must not read its clocks from
   * the page its parent's address space showed. */
  if (!thread && ids[NS_TIME] != parent_time)
    vdso_timens_update(child);
}

void namespace_task_exit(struct task *t) {
  struct ns_row *r = ns_row_of(t);
  if (!r || !ns_any)
    return;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  u32 pidns = r->used ? r->id[NS_PID] : 0;
  if (pidns)
    pidns_task_exit_locked(t, pidns);
  ns_row_clear_locked(r);
  spin_unlock_irqrestore(&ns_lock, f);
  if (pidns)
    pidns_run_zaps();
  ns_kick_reaper();
}

void namespace_task_reaped(struct task *t) {
  if (!t)
    return;
  /* Release any receive-context slot the task still holds: push and pop pair
   * inside one call frame, but a slot left behind would answer for the next
   * owner of the id. */
  namespace_net_release(t->id);
  if (!ns_any)
    return;
  /* A task can reach its reap without having gone through an exit path (a
   * fork that failed after the child was numbered): do what exit would. */
  namespace_task_exit(t);
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  pidns_task_reaped_locked(t->id);
  spin_unlock_irqrestore(&ns_lock, f);
}

/* ── creating namespaces ────────────────────────────────────────────────── */

/* A set of namespaces being built for unshare(2) or a clone(2) child. Each
 * non-zero entry holds one reference. */
struct ns_set {
  u32 id[NS_KIND_COUNT];
};

static void ns_set_release(struct ns_set *set) {
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  for (int k = 0; k < NS_KIND_COUNT; k++) {
    ns_put_locked(k, set->id[k]);
    set->id[k] = 0;
  }
  spin_unlock_irqrestore(&ns_lock, f);
  ns_kick_reaper();
}

static usize thread_group_size(void) {
  return scheduler_thread_group_count(current_task);
}

/* Build every namespace `flags` asks for, as the calling task would get them.
 * `userns` is the user namespace the new namespaces are owned by (a new one if
 * the set creates it). The caller checks privileges. */
static int ns_build(u64 flags, struct ns_set *set) {
  memset(set, 0, sizeof(*set));
  struct task *me = current_task;
  const struct cred *c = me ? me->cred : 0;
  if (!c)
    return -EPERM;

  if (flags & B1NIX_CLONE_NEWUSER) {
    int rc = userns_create(c);
    if (rc < 0)
      return rc;
    set->id[NS_USER] = (u32)rc;
  }
  u32 owner = set->id[NS_USER] ? set->id[NS_USER] : cred_userns(c);

  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  ns_ensure_init_locked();
  struct ns_row *r = ns_row_of(me);
  int used = r && r->used;
  u32 cur_uts = used ? r->id[NS_UTS] : 0;
  u32 cur_pid = used ? r->id[NS_PID] : 0;
  u32 cur_pid_children = used ? r->pid_children : 0;
  u32 cur_time_children = used ? r->time_children : 0;
  u32 cur_mnt = used ? r->id[NS_MNT] : 0;
  int rc = 0;

  if (flags & B1NIX_CLONE_NEWPID) {
    /* Linux copy_pid_ns: a task whose children already go to a namespace other
     * than its own cannot stack another one on top before it forks. */
    if (cur_pid_children != cur_pid) {
      rc = -EINVAL;
      goto fail_locked;
    }
    if (pidns_level_locked(cur_pid) + 1 >= NS_MAX_LEVEL) {
      rc = -ENOSPC;
      goto fail_locked;
    }
  }

  for (int k = 0; k < NS_KIND_COUNT; k++) {
    if (k == NS_USER || !(flags & namespace_kind_flag(k)))
      continue;
    u32 id = ns_alloc_locked(k, owner);
    if (!id) {
      rc = -ENOSPC;
      goto fail_locked;
    }
    set->id[k] = id;
    switch (k) {
    case NS_UTS:
      /* A new UTS namespace starts as a copy of the one being left. */
      uts_data[id] = uts_data[ns_live_locked(NS_UTS, cur_uts) ? cur_uts : 0];
      break;
    case NS_PID:
      pidns_init_locked(id, cur_pid);
      break;
    case NS_TIME:
      time_data[id] = time_data[cur_time_children];
      break;
    case NS_CGROUP:
      /* The root is taken outside the lock below. */
      break;
    default:
      break;
    }
  }
  spin_unlock_irqrestore(&ns_lock, f);

  if (set->id[NS_CGROUP]) {
    void *root = cgroup_ns_root_get(scheduler_get_pid());
    spin_lock_irqsave(&ns_lock, &f);
    cgroup_roots[set->id[NS_CGROUP]] = root;
    spin_unlock_irqrestore(&ns_lock, f);
  }
  /* The mount-table copy takes VFS locks and must not run under ns_lock. */
  if (set->id[NS_MNT]) {
    int mrc = vfs_mnt_ns_clone(cur_mnt, set->id[NS_MNT]);
    if (mrc != 0) {
      ns_set_release(set);
      return mrc;
    }
  }
  return 0;

fail_locked:
  spin_unlock_irqrestore(&ns_lock, f);
  ns_set_release(set);
  return rc;
}

/* The privilege Linux asks for before creating the non-user namespaces of a
 * set: CAP_SYS_ADMIN in the user namespace that will own them. A set that
 * creates that user namespace passes by construction — its creator holds every
 * capability there. */
static int ns_may_create(u64 flags) {
  if (!(flags & (B1NIX_CLONE_NS_ALL & ~(u64)B1NIX_CLONE_NEWUSER)))
    return 1;
  if (flags & B1NIX_CLONE_NEWUSER)
    return 1;
  const struct cred *c = scheduler_get_current_cred();
  return c && ns_capable_cred(c, cred_userns(c), CAP_SYS_ADMIN);
}

int namespace_fork_prepare(u64 flags) {
  u64 want = flags & B1NIX_CLONE_NS_ALL;
  /* CLONE_NEWTIME's bit is inside CSIGNAL on clone(2); the syscall layer only
   * passes it from clone3. */
  if ((flags & (B1NIX_CLONE_NEWNS | B1NIX_CLONE_NEWUSER)) &&
      (flags & LX_CLONE_FS))
    return -EINVAL;
  if ((flags & (B1NIX_CLONE_NEWUSER | B1NIX_CLONE_NEWPID)) &&
      (flags & (LX_CLONE_THREAD | LX_CLONE_PARENT)))
    return -EINVAL;
  if (!want)
    return 0;
  if (!ns_may_create(want))
    return -EPERM;

  struct ns_set set;
  int rc = ns_build(want, &set);
  if (rc < 0)
    return rc;

  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  struct ns_row *r = ns_row_of(current_task);
  if (!r) {
    spin_unlock_irqrestore(&ns_lock, f);
    ns_set_release(&set);
    return -EINVAL;
  }
  /* A previous prepare whose fork never ran (the caller was interrupted
   * between the two) must not leak into this child. */
  if (r->child_valid)
    for (int k = 0; k < NS_KIND_COUNT; k++) {
      ns_put_locked(k, r->child[k]);
      r->child[k] = 0;
    }
  for (int k = 0; k < NS_KIND_COUNT; k++)
    r->child[k] = (u16)set.id[k];
  r->child_valid = 1;
  r->used = 1;
  spin_unlock_irqrestore(&ns_lock, f);
  ns_kick_reaper();
  return 0;
}

void namespace_fork_prepare_abort(void) {
  if (!ns_any)
    return;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  struct ns_row *r = ns_row_of(current_task);
  if (r && r->child_valid) {
    for (int k = 0; k < NS_KIND_COUNT; k++) {
      ns_put_locked(k, r->child[k]);
      r->child[k] = 0;
    }
    r->child_valid = 0;
    if (ns_row_trivial(r))
      r->used = 0;
  }
  spin_unlock_irqrestore(&ns_lock, f);
  ns_kick_reaper();
}

int namespace_fork_allowed(void) {
  if (!ns_any)
    return 0;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  struct ns_row *r = ns_row_of(current_task);
  u32 target = 0;
  if (r && r->used)
    target = (r->child_valid && r->child[NS_PID]) ? r->child[NS_PID]
                                                   : r->pid_children;
  int dying = target ? pidns_dying_locked(target) : 0;
  spin_unlock_irqrestore(&ns_lock, f);
  return dying ? -ENOMEM : 0;
}

int namespace_unshare(u64 flags) {
  const u64 allowed = LX_CLONE_THREAD | LX_CLONE_FS | LX_CLONE_SIGHAND |
                      LX_CLONE_VM | LX_CLONE_FILES | LX_CLONE_SYSVSEM |
                      B1NIX_CLONE_NS_ALL;
  if (flags & ~allowed)
    return -EINVAL;
  /* Linux check_unshare_flags: a new user namespace implies a private
   * thread group and fs; a new mount namespace a private fs. */
  if (flags & B1NIX_CLONE_NEWUSER)
    flags |= LX_CLONE_THREAD | LX_CLONE_FS;
  if (flags & B1NIX_CLONE_NEWNS)
    flags |= LX_CLONE_FS;
  if ((flags & (LX_CLONE_THREAD | LX_CLONE_SIGHAND | LX_CLONE_VM)) &&
      thread_group_size() > 1)
    return -EINVAL;

  u64 want = flags & B1NIX_CLONE_NS_ALL;
  if (!want)
    return 0; /* CLONE_FS/FILES/SYSVSEM: this task already owns all of them */
  if (!ns_may_create(want))
    return -EPERM;

  struct ns_set set;
  int rc = ns_build(want, &set);
  if (rc < 0)
    return rc;

  struct task *me = current_task;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  struct ns_row *r = ns_row_of(me);
  if (!r) {
    spin_unlock_irqrestore(&ns_lock, f);
    ns_set_release(&set);
    return -EINVAL;
  }
  if (!r->used) {
    memset(r, 0, sizeof(*r));
    r->used = 1;
  }
  for (int k = 0; k < NS_KIND_COUNT; k++) {
    u32 id = set.id[k];
    if (!(want & namespace_kind_flag(k)))
      continue;
    switch (k) {
    case NS_USER:
      cred_enter_userns_locked(me->cred, id);
      break;
    case NS_PID:
      /* The caller stays where it is; its children are born in the new one. */
      ns_put_locked(NS_PID, r->pid_children);
      r->pid_children = (u16)id;
      break;
    case NS_TIME:
      ns_put_locked(NS_TIME, r->time_children);
      r->time_children = (u16)id;
      break;
    default:
      ns_put_locked(k, r->id[k]);
      r->id[k] = (u16)id;
      break;
    }
  }
  spin_unlock_irqrestore(&ns_lock, f);
  ns_kick_reaper();
  return 0;
}

/* ── setns(2) ───────────────────────────────────────────────────────────── */

/* Validate entering namespace (kind, id) — Linux's per-kind ->install checks.
 * Caller holds ns_lock. */
static int ns_install_check_locked(int kind, u32 id, const struct cred *c,
                                   const struct ns_row *r) {
  if (!ns_live_locked(kind, id))
    return -EINVAL;
  u32 mine = cred_userns(c);
  u32 owner = kind == NS_USER ? id : (id ? ns_slots[kind][id].owner : 0);
  switch (kind) {
  case NS_USER:
    /* Entering the namespace one is already in would re-grant the full
     * capability set; threaded processes may not change user namespace. */
    if (id == mine)
      return -EINVAL;
    if (thread_group_size() > 1)
      return -EINVAL;
    if (!ns_capable_cred(c, id, CAP_SYS_ADMIN))
      return -EPERM;
    return 0;
  case NS_MNT:
    if (!ns_capable_cred(c, owner, CAP_SYS_ADMIN) ||
        !ns_capable_cred(c, mine, CAP_SYS_CHROOT) ||
        !ns_capable_cred(c, mine, CAP_SYS_ADMIN))
      return -EPERM;
    return 0;
  case NS_PID: {
    u32 active = (r && r->used) ? r->id[NS_PID] : 0;
    if (!ns_capable_cred(c, owner, CAP_SYS_ADMIN) ||
        !ns_capable_cred(c, mine, CAP_SYS_ADMIN))
      return -EPERM;
    /* Only the active namespace or a descendant of it: going the other way
     * would hand the caller numbers it is not entitled to name. */
    if (!pidns_is_ancestor_locked(active, id))
      return -EINVAL;
    return 0;
  }
  case NS_TIME:
    if (thread_group_size() > 1)
      return -EUSERS;
    /* fall through */
  default:
    if (!ns_capable_cred(c, owner, CAP_SYS_ADMIN) ||
        !ns_capable_cred(c, mine, CAP_SYS_ADMIN))
      return -EPERM;
    return 0;
  }
}

/* Enter namespace (kind, id): the reference is taken here. */
static void ns_install_locked(struct task *me, struct ns_row *r, int kind,
                              u32 id) {
  if (!r->used) {
    memset(r, 0, sizeof(*r));
    r->used = 1;
  }
  switch (kind) {
  case NS_USER:
    ns_get_locked(NS_USER, id);
    cred_enter_userns_locked(me->cred, id);
    break;
  case NS_PID:
    ns_get_locked(NS_PID, id);
    ns_put_locked(NS_PID, r->pid_children);
    r->pid_children = (u16)id;
    break;
  case NS_TIME:
    /* setns into a time namespace moves the caller itself (its clocks change
     * at once) and its future children. */
    ns_get_locked(NS_TIME, id);
    ns_get_locked(NS_TIME, id);
    ns_put_locked(NS_TIME, r->time_children);
    ns_put_locked(NS_TIME, r->id[NS_TIME]);
    r->time_children = (u16)id;
    r->id[NS_TIME] = (u16)id;
    if (id)
      time_slots[id].entered = 1;
    break;
  default:
    ns_get_locked(kind, id);
    ns_put_locked(kind, r->id[kind]);
    r->id[kind] = (u16)id;
    break;
  }
  if (ns_row_trivial(r))
    r->used = 0;
}

int namespace_setns(int fd, int nstype) {
  struct task *me = current_task;
  const struct cred *c = me ? me->cred : 0;
  if (!c)
    return -EPERM;
  struct vfs_handle *h = vfs_handle_acquire(fd);
  if (!h)
    return -EBADF;

  u32 targets[NS_KIND_COUNT] = {0};
  u8 want[NS_KIND_COUNT] = {0};
  int rc = 0;
  int by_pidfd = h->kind == VFS_HANDLE_PIDFD;

  if (by_pidfd) {
    /* setns(pidfd, flags): every namespace named in `flags`, taken from the
     * process the pidfd refers to, entered as one operation. */
    u64 flags = (u64)(u32)nstype;
    if (!flags || (flags & ~(u64)B1NIX_CLONE_NS_ALL)) {
      vfs_handle_release(h);
      return -EINVAL;
    }
    struct task *t = scheduler_task_by_pid(vfs_pidfd_pid(h));
    if (!t || t->state == TASK_DEAD || t->state == TASK_REAPING ||
        t->state == TASK_UNUSED) {
      vfs_handle_release(h);
      return -ESRCH;
    }
    for (int k = 0; k < NS_KIND_COUNT; k++) {
      if (!(flags & namespace_kind_flag(k)))
        continue;
      want[k] = 1;
      targets[k] = namespace_task_id(t, k);
    }
  } else {
    int kind;
    u32 id;
    if (h->kind != VFS_HANDLE_NODE || nsfs_node_ns(h->node, &kind, &id) != 0) {
      vfs_handle_release(h);
      return -EINVAL;
    }
    /* A non-zero nstype is the caller telling us what it believes the handle
     * is; disagreeing with it is an error. */
    if (nstype != 0 && namespace_kind_flag(kind) != (u64)(u32)nstype) {
      vfs_handle_release(h);
      return -EINVAL;
    }
    want[kind] = 1;
    targets[kind] = id;
  }
  vfs_handle_release(h);

  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  ns_ensure_init_locked();
  struct ns_row *r = ns_row_of(me);
  if (!r) {
    spin_unlock_irqrestore(&ns_lock, f);
    return -EINVAL;
  }
  /* The user namespace first: every later check is made with the credential
   * the caller will have once it is inside, as Linux does. With a pidfd the
   * caller may already share the target's user namespace; that one is then
   * simply not re-entered. */
  if (want[NS_USER] && targets[NS_USER] == cred_userns(c) && by_pidfd)
    want[NS_USER] = 0;
  if (want[NS_USER]) {
    rc = ns_install_check_locked(NS_USER, targets[NS_USER], c, r);
    if (rc == 0)
      ns_install_locked(me, r, NS_USER, targets[NS_USER]);
  }
  for (int k = 0; k < NS_KIND_COUNT && rc == 0; k++) {
    if (k == NS_USER || !want[k])
      continue;
    rc = ns_install_check_locked(k, targets[k], me->cred, r);
  }
  for (int k = 0; k < NS_KIND_COUNT && rc == 0; k++) {
    if (k == NS_USER || !want[k])
      continue;
    ns_install_locked(me, r, k, targets[k]);
  }
  spin_unlock_irqrestore(&ns_lock, f);
  if (rc == 0 && want[NS_TIME])
    vdso_timens_update(me);
  ns_kick_reaper();
  return rc;
}

/* ── nsfs ───────────────────────────────────────────────────────────────── */

/* st_dev of every namespace handle, Linux's nsfs device (0:4). */
#define NSFS_DEV 4u
#define NSFS_PIN_VALID 0x80000000u

static void nsfs_release(struct vfs_node *node);
static int nsfs_ioctl_cb(struct vfs_node *node, u64 request, void *arg);

struct vfs_node *nsfs_node(int kind, u32 id) {
  if (kind < 0 || kind >= NS_KIND_COUNT)
    return ERR_PTR(-EINVAL);
  if (namespace_get(kind, id) != 0)
    return ERR_PTR(-ENOENT);
  struct vfs_node *n = vfs_create_node(VFS_FILE);
  if (!n) {
    namespace_put(kind, id);
    return ERR_PTR(-ENOMEM);
  }
  snprintf(n->name, sizeof(n->name), "%s:[%llu]", namespace_kind_name(kind),
           (unsigned long long)namespace_inum(kind, id));
  n->inode->ino = namespace_inum(kind, id);
  n->inode->dev = NSFS_DEV;
  n->inode->mode = 0444;
  n->inode->uid = 0;
  n->inode->gid = 0;
  n->inode->data = (void *)(usize)(NSFS_PIN_VALID | ((u32)kind << 24) | id);
  n->inode->release_cb = nsfs_release;
  n->inode->ioctl_cb = nsfs_ioctl_cb;
  /* No name in any directory: the node dies with its last reference, and that
   * is what releases the namespace. */
  n->deleted = 1;
  return n;
}

int nsfs_node_ns(struct vfs_node *node, int *kind, u32 *id) {
  if (!node || !node->inode || node->inode->release_cb != nsfs_release)
    return -EINVAL;
  u32 pin = (u32)(usize)node->inode->data;
  if (!(pin & NSFS_PIN_VALID))
    return -EINVAL;
  if (kind)
    *kind = (int)((pin >> 24) & 0x7F);
  if (id)
    *id = pin & 0xFFFFFF;
  return 0;
}

static void nsfs_release(struct vfs_node *node) {
  int kind;
  u32 id;
  if (nsfs_node_ns(node, &kind, &id) != 0)
    return;
  node->inode->data = 0;
  node->inode->nlink = 0; /* free the inode along with the node */
  namespace_put(kind, id);
}

/* <linux/nsfs.h> */
#define NS_GET_USERNS    0xb701
#define NS_GET_PARENT    0xb702
#define NS_GET_NSTYPE    0xb703
#define NS_GET_OWNER_UID 0xb704

int nsfs_ioctl(struct vfs_node *node, u64 request, u64 arg) {
  int kind;
  u32 id;
  if (nsfs_node_ns(node, &kind, &id) != 0)
    return -ENOTTY;
  switch (request) {
  case NS_GET_NSTYPE:
    return (int)namespace_kind_flag(kind);
  case NS_GET_USERNS:
  case NS_GET_PARENT: {
    int want_kind = NS_USER;
    u32 target;
    const struct cred *c = scheduler_get_current_cred();
    u32 mine = c ? cred_userns(c) : 0;
    u64 f;
    spin_lock_irqsave(&ns_lock, &f);
    if (request == NS_GET_USERNS) {
      target = kind == NS_USER ? userns_parent_locked(id)
                               : (id ? ns_slots[kind][id].owner : 0);
      if (kind == NS_USER && id == 0) {
        spin_unlock_irqrestore(&ns_lock, f);
        return -EPERM;
      }
    } else if (kind == NS_USER) {
      if (id == 0) {
        spin_unlock_irqrestore(&ns_lock, f);
        return -EPERM;
      }
      target = userns_parent_locked(id);
    } else if (kind == NS_PID) {
      if (id == 0) {
        spin_unlock_irqrestore(&ns_lock, f);
        return -EPERM;
      }
      want_kind = NS_PID;
      target = pidns_parent_locked(id);
    } else {
      spin_unlock_irqrestore(&ns_lock, f);
      return -EINVAL;
    }
    spin_unlock_irqrestore(&ns_lock, f);
    /* Linux will not hand out a namespace above the caller's own user
     * namespace: that would be a way out of it. */
    if (want_kind == NS_USER && !userns_is_ancestor(mine, target))
      return -EPERM;
    if (want_kind == NS_PID) {
      u32 active = namespace_current_id(NS_PID);
      u64 g;
      spin_lock_irqsave(&ns_lock, &g);
      int ok = pidns_is_ancestor_locked(active, target);
      spin_unlock_irqrestore(&ns_lock, g);
      if (!ok)
        return -EPERM;
    }
    struct vfs_node *n = nsfs_node(want_kind, target);
    if (IS_ERR(n))
      return (int)PTR_ERR(n);
    int fd = vfs_fd_for_node(n, B1NIX_O_RDONLY);
    vfs_node_put(n);
    if (fd >= 0)
      scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
    return fd;
  }
  case NS_GET_OWNER_UID: {
    if (kind != NS_USER)
      return -EINVAL;
    u32 uid = current_from_kuid(userns_owner_kuid(id));
    if (!arg)
      return -EFAULT;
    return syscall_copyout((void *)(usize)arg, &uid, sizeof(uid)) < 0 ? -EFAULT
                                                                      : 0;
  }
  default:
    return -ENOTTY;
  }
}

static int nsfs_ioctl_cb(struct vfs_node *node, u64 request, void *arg) {
  return nsfs_ioctl(node, request, (u64)(usize)arg);
}

/* ── UTS ────────────────────────────────────────────────────────────────── */

static struct uts_data *uts_current_locked(void) {
  ns_ensure_init_locked();
  u32 id = namespace_current_id(NS_UTS);
  if (!ns_live_locked(NS_UTS, id))
    id = 0;
  return &uts_data[id];
}

void namespace_uts_get_host(char *buf, usize len) {
  if (!buf || len == 0)
    return;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  ns_copy_name(buf, len, uts_current_locked()->host);
  spin_unlock_irqrestore(&ns_lock, f);
}

void namespace_uts_get_domain(char *buf, usize len) {
  if (!buf || len == 0)
    return;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  ns_copy_name(buf, len, uts_current_locked()->domain);
  spin_unlock_irqrestore(&ns_lock, f);
}

int namespace_uts_set_host(const char *name) {
  if (!name)
    return -EFAULT;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  struct uts_data *u = uts_current_locked();
  ns_copy_name(u->host, sizeof(u->host), name);
  spin_unlock_irqrestore(&ns_lock, f);
  return 0;
}

int namespace_uts_set_domain(const char *name) {
  if (!name)
    return -EFAULT;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  struct uts_data *u = uts_current_locked();
  ns_copy_name(u->domain, sizeof(u->domain), name);
  spin_unlock_irqrestore(&ns_lock, f);
  return 0;
}

/* ── network namespaces ───────────────────────────────────────────────────
 *
 * The net layer needs two different answers to "which namespace is this?".
 * A socket call, an ioctl or a netlink message belongs to the caller. A frame
 * being demultiplexed belongs to the interface it arrived on, which may be in
 * a namespace no running task is currently in — so the receive path pushes a
 * context around the delivery.
 *
 * That context is PER TASK, not one word for the machine: two CPUs demultiplex
 * frames at the same time, and a delivery can sleep (a reply generated inside
 * it waits for ARP). Keyed by the task's own id, not the thread group's, so
 * one thread's receive context never answers for another. */

#define NS_RX_SLOTS 32
struct ns_rx_slot {
  usize key; /* task id + 1, so that 0 means "free" */
  u32 ns;
};
static struct ns_rx_slot ns_rx[NS_RX_SLOTS];
/* Non-zero while any task is inside a receive path: the fast path for every
 * other caller, which is almost all of them. */
static int ns_rx_live;

static struct ns_rx_slot *ns_rx_find(usize key) {
  for (int i = 0; i < NS_RX_SLOTS; i++) {
    if (__atomic_load_n(&ns_rx[i].key, __ATOMIC_ACQUIRE) == key)
      return &ns_rx[i];
  }
  return 0;
}

u32 namespace_net_current(void) { return namespace_current_id(NS_NET); }

u32 namespace_net_context(void) {
  if (__atomic_load_n(&ns_rx_live, __ATOMIC_RELAXED)) {
    struct ns_rx_slot *slot = ns_rx_find(scheduler_current_task_id() + 1);
    if (slot) {
      u32 rx = __atomic_load_n(&slot->ns, __ATOMIC_RELAXED);
      if (rx)
        return rx;
    }
  }
  return namespace_net_current();
}

u32 namespace_net_push_context(u32 ns) {
  usize key = scheduler_current_task_id() + 1;
  struct ns_rx_slot *slot = ns_rx_find(key);
  if (slot) {
    /* Already inside a delivery — a bridge handing a frame back, or a reply
     * that re-enters the receive path. The caller keeps the previous value on
     * its stack and hands it back to the pop below. */
    u32 prev = __atomic_load_n(&slot->ns, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->ns, ns, __ATOMIC_RELAXED);
    return prev;
  }
  if (!ns)
    return 0; /* nothing to record: the initial namespace is the default */
  for (int i = 0; i < NS_RX_SLOTS; i++) {
    usize expect = 0;
    if (__atomic_compare_exchange_n(&ns_rx[i].key, &expect, key, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
      __atomic_store_n(&ns_rx[i].ns, ns, __ATOMIC_RELAXED);
      __atomic_fetch_add(&ns_rx_live, 1, __ATOMIC_RELAXED);
      return 0;
    }
  }
  /* Every slot taken. The delivery still happens; it resolves in the calling
   * task's own namespace. */
  return 0;
}

void namespace_net_release(usize pid) {
  if (!__atomic_load_n(&ns_rx_live, __ATOMIC_RELAXED))
    return;
  struct ns_rx_slot *slot = ns_rx_find(pid + 1);
  if (!slot)
    return;
  __atomic_store_n(&slot->ns, 0, __ATOMIC_RELAXED);
  __atomic_fetch_sub(&ns_rx_live, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&slot->key, (usize)0, __ATOMIC_RELEASE);
}

void namespace_net_pop_context(u32 saved) {
  usize key = scheduler_current_task_id() + 1;
  struct ns_rx_slot *slot = ns_rx_find(key);
  if (!slot)
    return;
  if (saved) {
    __atomic_store_n(&slot->ns, saved, __ATOMIC_RELAXED);
    return;
  }
  __atomic_store_n(&slot->ns, 0, __ATOMIC_RELAXED);
  __atomic_fetch_sub(&ns_rx_live, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&slot->key, (usize)0, __ATOMIC_RELEASE);
}

int namespace_net_live(u32 ns) {
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  int live = ns_live_locked(NS_NET, ns);
  spin_unlock_irqrestore(&ns_lock, f);
  return live;
}

/* ── time namespaces ────────────────────────────────────────────────────── */

i64 namespace_time_offset(int clock) {
  u32 id = namespace_current_id(NS_TIME);
  if (!id)
    return 0;
  if (clock == TIMENS_CLOCK_MONOTONIC)
    return time_data[id].monotonic;
  if (clock == TIMENS_CLOCK_BOOTTIME)
    return time_data[id].boottime;
  return 0;
}

i64 namespace_clock_offset(int clockid) {
  switch (clockid) {
  case 1: /* CLOCK_MONOTONIC */
  case 4: /* CLOCK_MONOTONIC_RAW */
  case 6: /* CLOCK_MONOTONIC_COARSE */
    return namespace_time_offset(TIMENS_CLOCK_MONOTONIC);
  case 7: /* CLOCK_BOOTTIME */
  case 9: /* CLOCK_BOOTTIME_ALARM */
    return namespace_time_offset(TIMENS_CLOCK_BOOTTIME);
  default:
    return 0;
  }
}

static void timens_fmt(char *buf, usize len, usize *pos, const char *name,
                       i64 off) {
  i64 sec = off / 1000000000LL;
  i64 nsec = off % 1000000000LL;
  if (nsec < 0) {
    nsec += 1000000000LL;
    sec -= 1;
  }
  if (*pos < len)
    *pos += (usize)snprintf(buf + *pos, len - *pos, "%s %lld %lld\n", name,
                            (long long)sec, (long long)nsec);
}

int namespace_time_offsets_render(const struct task *t, char *buf, usize len) {
  u32 id = namespace_task_children_id(t, NS_TIME);
  usize pos = 0;
  timens_fmt(buf, len, &pos, "monotonic", id ? time_data[id].monotonic : 0);
  timens_fmt(buf, len, &pos, "boottime", id ? time_data[id].boottime : 0);
  return (int)(pos < len ? pos : len);
}

/* Parse "<clock> <secs> <nsecs>" lines. The clock is a name or a clock id. */
int namespace_time_offsets_write(struct task *t, const char *buf, usize len) {
  u32 id = namespace_task_children_id(t, NS_TIME);
  if (!id)
    return -EACCES; /* the initial namespace's clocks are not movable */
  const struct cred *c = scheduler_get_current_cred();
  if (!c || !ns_capable_cred(c, namespace_owner(NS_TIME, id), CAP_SYS_TIME))
    return -EPERM;
  i64 mono = 0, boot = 0;
  int have_mono = 0, have_boot = 0;
  usize i = 0;
  while (i < len) {
    char line[96];
    usize n = 0;
    while (i < len && buf[i] != '\n' && n + 1 < sizeof(line))
      line[n++] = buf[i++];
    if (i < len && buf[i] != '\n')
      return -EINVAL;
    i++;
    line[n] = '\0';
    if (n == 0)
      continue;
    char *p = line;
    char name[16];
    usize nl = 0;
    while (*p && *p != ' ' && nl + 1 < sizeof(name))
      name[nl++] = *p++;
    name[nl] = '\0';
    int clk;
    if (strcmp(name, "monotonic") == 0 || strcmp(name, "1") == 0)
      clk = TIMENS_CLOCK_MONOTONIC;
    else if (strcmp(name, "boottime") == 0 || strcmp(name, "7") == 0)
      clk = TIMENS_CLOCK_BOOTTIME;
    else
      return -EINVAL;
    i64 vals[2];
    for (int v = 0; v < 2; v++) {
      while (*p == ' ')
        p++;
      int neg = 0;
      if (*p == '-') {
        neg = 1;
        p++;
      }
      if (*p < '0' || *p > '9')
        return -EINVAL;
      i64 x = 0;
      while (*p >= '0' && *p <= '9') {
        if (x > (i64)922337203685477580LL)
          return -ERANGE;
        x = x * 10 + (*p++ - '0');
      }
      vals[v] = neg ? -x : x;
    }
    while (*p == ' ')
      p++;
    if (*p)
      return -EINVAL;
    if (vals[1] < 0 || vals[1] >= 1000000000LL)
      return -EINVAL;
    /* Linux bounds a monotonic/boottime offset so the shifted clock can never
     * go negative: no further back than the clock itself has run. */
    i64 off = vals[0] * 1000000000LL + vals[1];
    if (clk == TIMENS_CLOCK_MONOTONIC) {
      mono = off;
      have_mono = 1;
    } else {
      boot = off;
      have_boot = 1;
    }
  }
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  int rc = 0;
  if (!ns_live_locked(NS_TIME, id))
    rc = -EINVAL;
  else if (time_slots[id].entered)
    rc = -EACCES; /* offsets are fixed once a task lives in the namespace */
  else {
    if (have_mono)
      time_data[id].monotonic = mono;
    if (have_boot)
      time_data[id].boottime = boot;
  }
  spin_unlock_irqrestore(&ns_lock, f);
  return rc < 0 ? rc : (int)len;
}

/* ── cgroup namespaces ──────────────────────────────────────────────────── */

void *namespace_cgroup_root(u32 cgns) {
  if (cgns == 0 || cgns >= NS_MAX_CGROUP)
    return 0;
  return cgroup_roots[cgns];
}
