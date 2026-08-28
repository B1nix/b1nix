/* M109 — per-process namespaces for unshare(1)/nsenter(1).
 *
 * Four kinds, all real:
 *
 *   UTS   — each namespace owns a hostname and a domainname. Every reader in
 *           the tree (uname(2), /proc/sys/kernel/hostname, /sys/kernel/hostname)
 *           already went through kernel_hostname_get(), so routing that one
 *           function through the caller's namespace makes all of them agree.
 *   MOUNT — a namespace is a private copy of the VFS mount table. Table entries
 *           carry a namespace id and every scan skips the ones that do not
 *           belong to the caller, so a mount made inside a namespace is
 *           invisible outside it (kernel/fs/vfs.c).
 *
 *   PID   — a TRANSLATION over the kernel's one flat task-id space. A task
 *           created into a namespace is numbered from 1 there (and in every
 *           namespace between it and the initial one) and is invisible to any
 *           namespace that is not an ancestor of its own. As on Linux,
 *           unshare(CLONE_NEWPID) does not move the caller — it chooses the
 *           namespace its future children are born into.
 *   NET   — interfaces, routes, neighbours and socket bindings each carry the
 *           id of the namespace they belong to, and every enumeration and
 *           lookup filters on the caller's (kernel/net/). A veth pair is how a
 *           frame crosses from one namespace to another.
 *
 * Which namespaces a task is in lives in a side table keyed by task id —
 * struct task must not grow (see kernel/mm/eviction.c for the same
 * constraint). A task with no row is in the initial namespace of every kind,
 * which is every task until something unshares, so a normal boot allocates no
 * rows and namespace_active() keeps the VFS fast paths free of any lookup.
 */

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/namespace.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <string.h>

#define NS_MAX_TASKS 64 /* tasks that are NOT in the initial namespaces */
#define NS_MAX_UTS 8    /* slot 0 is the initial UTS namespace */
/* Mount namespaces. Eight was enough while the only caller was `unshare -m`
 * from a shell; a systemd machine puts every sandboxed unit and every
 * generator in one of its own, and runs out during the boot -- at which point
 * clone(CLONE_NEWNS) answers ENOSPC and the unit fails to start for a reason
 * that has nothing to do with the unit. The cost is one int per slot here plus
 * whatever mount entries a namespace actually holds. */
#define NS_MAX_MNT 64   /* slot 0 is the initial mount namespace */
#define NS_MAX_PID 8    /* slot 0 is the initial pid namespace */
/* Tasks a non-initial pid namespace can number at once. */
#define PIDNS_MAX_TASKS 64

#define NS_NAME_MAX 65

struct ns_row {
  int used;
  usize pid;
  u32 id[NS_KIND_COUNT];
  /* Linux's pid_ns_for_children: unshare/setns of a PID namespace takes effect
   * on the tasks this one goes on to create, never on itself. */
  u32 pid_children;
  /* Namespaces the NEXT child is born into, prepared by clone(2) before the
   * fork and consumed by namespace_fork_inherit. 0 in a slot means "inherit".
   *
   * It is done ahead of the fork, not after it, because the child is runnable
   * the moment the fork returns: a namespace handed to it afterwards is a
   * namespace it may already have left the kernel without. Cloning a mount
   * table also allocates and takes VFS locks, which the fork's tail — running
   * with interrupts disabled — is no place for. */
  u32 child_ns[NS_KIND_COUNT];
};

/* One entry per task numbered in a non-initial pid namespace.
 *
 * A dead entry keeps its numbers. It has to: waitpid(2) reaps the task and
 * only then translates the pid it is about to return, so deleting the mapping
 * at reap time made every wait for a specific child inside a namespace answer
 * 0. Keeping it is safe because the kernel's task ids are handed out by a
 * monotonic counter and never recycled, so a stale pair can never come to name
 * a different task. The slot is reclaimed when the table has no free one. */
struct pidns_slot {
  usize vnr;  /* the number this namespace knows the task by; 0 = free */
  usize gid;  /* the kernel's own task id */
  u8 dead;    /* the task is gone; the number is kept for the reaper */
};

struct pid_ns {
  int used;
  u32 parent;     /* enclosing namespace; 0 is the initial one */
  usize next_vnr; /* next number to hand out */
  struct pidns_slot map[PIDNS_MAX_TASKS];
};

struct uts_ns {
  int used;
  char host[NS_NAME_MAX];
  char domain[NS_NAME_MAX];
};

static struct ns_row ns_rows[NS_MAX_TASKS];
static struct uts_ns uts_ns[NS_MAX_UTS];
static int mnt_ns_used[NS_MAX_MNT];
static struct pid_ns pid_ns[NS_MAX_PID];
static int net_ns_used[NS_MAX_NET];

/* Non-zero once at least one row exists. Read without the lock on the VFS fast
 * path: the worst a stale zero can do is one lookup that predates an unshare
 * the calling task has not returned from yet. */
static int ns_any;

static spinlock_t ns_lock = SPINLOCK_INIT;
static int ns_ready;

/* The pid-namespace map, defined further down beside the rest of the pid
 * machinery; the task lifetime hooks above it have to reach them. */
static void pidns_enter_locked(u32 ns, usize gid);
static void pidns_leave_locked(usize gid);

static void ns_copy_name(char *dst, usize len, const char *src) {
  if (!dst || len == 0)
    return;
  usize i = 0;
  if (src)
    for (; src[i] && i + 1 < len; i++)
      dst[i] = src[i];
  dst[i] = '\0';
}

/* Initialise the initial namespaces on first use — there is no ordering
 * guarantee between the scheduler and an early sethostname(). */
static void ns_ensure_init(void) {
  if (ns_ready)
    return;
  ns_ready = 1;
  uts_ns[0].used = 1;
  ns_copy_name(uts_ns[0].host, sizeof(uts_ns[0].host), "b1nix");
  ns_copy_name(uts_ns[0].domain, sizeof(uts_ns[0].domain), "(none)");
  mnt_ns_used[0] = 1;
  pid_ns[0].used = 1;
  pid_ns[0].parent = 0;
  pid_ns[0].next_vnr = 1;
  net_ns_used[0] = 1;
}

const char *namespace_kind_name(int kind) {
  switch (kind) {
  case NS_UTS:
    return "uts";
  case NS_MNT:
    return "mnt";
  case NS_PID:
    return "pid";
  case NS_NET:
    return "net";
  default:
    return "";
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

int namespace_active(void) { return ns_any; }

/* ── the side table ─────────────────────────────────────────────────────── */

static struct ns_row *ns_find_locked(usize pid) {
  for (usize i = 0; i < NS_MAX_TASKS; i++)
    if (ns_rows[i].used && ns_rows[i].pid == pid)
      return &ns_rows[i];
  return 0;
}

static struct ns_row *ns_get_or_add_locked(usize pid) {
  struct ns_row *r = ns_find_locked(pid);
  if (r)
    return r;
  for (usize i = 0; i < NS_MAX_TASKS; i++) {
    if (ns_rows[i].used)
      continue;
    ns_rows[i].used = 1;
    ns_rows[i].pid = pid;
    for (int k = 0; k < NS_KIND_COUNT; k++)
      ns_rows[i].id[k] = 0;
    ns_rows[i].pid_children = 0;
    ns_any = 1;
    return &ns_rows[i];
  }
  return 0;
}

/* Is any live row still in namespace `id` of kind `kind`? */
static int ns_id_referenced_locked(int kind, u32 id) {
  for (usize i = 0; i < NS_MAX_TASKS; i++)
    if (ns_rows[i].used && ns_rows[i].id[kind] == id)
      return 1;
  return 0;
}

static void ns_recompute_any_locked(void) {
  for (usize i = 0; i < NS_MAX_TASKS; i++)
    if (ns_rows[i].used)
      return;
  ns_any = 0;
}

/* Release namespaces nothing points at any more. Called from the paths that
 * allocate (unshare/setns) rather than from task exit: tearing a mount
 * namespace down means dropping VFS references, which must not happen on the
 * exit path where the caller is already unwinding. Must be called with the
 * lock NOT held — vfs_mnt_ns_destroy() takes VFS locks. */
/* Is namespace `id` of kind `kind` still wanted by a live row? A pid
 * namespace also counts as referenced while a row is still aiming its future
 * children at it — an unshare that has not forked yet must not be collected. */
static int ns_kind_referenced_locked(int kind, u32 id) {
  if (ns_id_referenced_locked(kind, id))
    return 1;
  if (kind == NS_PID)
    for (usize i = 0; i < NS_MAX_TASKS; i++)
      if (ns_rows[i].used && ns_rows[i].pid_children == id)
        return 1;
  return 0;
}

static void ns_gc(void) {
  u32 dead[NS_MAX_MNT];
  int ndead = 0;
  u32 dead_net[NS_MAX_NET];
  int ndead_net = 0;

  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  ns_ensure_init();
  for (u32 i = 1; i < NS_MAX_UTS; i++)
    if (uts_ns[i].used && !ns_id_referenced_locked(NS_UTS, i))
      uts_ns[i].used = 0;
  for (u32 i = 1; i < NS_MAX_MNT; i++) {
    if (!mnt_ns_used[i] || ns_id_referenced_locked(NS_MNT, i))
      continue;
    mnt_ns_used[i] = 0;
    dead[ndead++] = i;
  }
  for (u32 i = 1; i < NS_MAX_PID; i++) {
    if (!pid_ns[i].used || ns_kind_referenced_locked(NS_PID, i))
      continue;
    memset(&pid_ns[i], 0, sizeof(pid_ns[i]));
  }
  for (u32 i = 1; i < NS_MAX_NET; i++) {
    if (!net_ns_used[i] || ns_id_referenced_locked(NS_NET, i))
      continue;
    net_ns_used[i] = 0;
    dead_net[ndead_net++] = i;
  }
  spin_unlock_irqrestore(&ns_lock, flags);

  for (int i = 0; i < ndead; i++)
    vfs_mnt_ns_destroy(dead[i]);
  /* Interfaces, routes and neighbours outlive their namespace otherwise, and a
   * later namespace reusing the slot would inherit them. */
  for (int i = 0; i < ndead_net; i++)
    net_ns_destroy(dead_net[i]);
}

u32 namespace_id_of(usize pid, int kind) {
  if (kind < 0 || kind >= NS_KIND_COUNT || !ns_any)
    return 0;
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  struct ns_row *r = ns_find_locked(pid);
  u32 id = r ? r->id[kind] : 0;
  spin_unlock_irqrestore(&ns_lock, flags);
  return id;
}

u32 namespace_current_id(int kind) {
  if (!ns_any)
    return 0;
  return namespace_id_of(scheduler_get_pid(), kind);
}

void namespace_fork_inherit(usize parent_pid, usize child_pid) {
  if (!ns_any)
    return;
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  struct ns_row *p = ns_find_locked(parent_pid);
  if (p) {
    u32 copy[NS_KIND_COUNT];
    for (int k = 0; k < NS_KIND_COUNT; k++)
      copy[k] = p->id[k];
    /* A pending CLONE_NEWPID applies here and only here: the child is born in
     * it, the parent stays where it was, and the child's own children follow
     * the child. That is what makes the first child pid 1. */
    if (p->pid_children)
      copy[NS_PID] = p->pid_children;
    /* Namespaces clone(CLONE_NEW*) prepared for exactly this child. */
    for (int k = 0; k < NS_KIND_COUNT; k++)
      if (p->child_ns[k]) {
        copy[k] = p->child_ns[k];
        p->child_ns[k] = 0; /* one child, not every child after it */
      }
    struct ns_row *c = ns_get_or_add_locked(child_pid);
    if (c) {
      for (int k = 0; k < NS_KIND_COUNT; k++)
        c->id[k] = copy[k];
      c->pid_children = 0;
      for (int k = 0; k < NS_KIND_COUNT; k++)
        c->child_ns[k] = 0;
      if (copy[NS_PID])
        pidns_enter_locked(copy[NS_PID], child_pid);
    }
  }
  spin_unlock_irqrestore(&ns_lock, flags);
}

/* The task's slot has been released. Its number is only MARKED dead here, not
 * dropped: waitpid(2) reaps the task and only then translates the pid it is
 * about to return, so a number deleted at reap time is already gone by the
 * time the answer needs it. See struct pidns_slot. */
void namespace_task_reaped(usize pid) {
  /* Release any receive-context slot the task still holds. Push and pop are
   * paired inside one call frame, so this should never find one — but a pid is
   * reused, and a slot left behind would answer for its next owner. */
  namespace_net_release(pid);
  if (!ns_ready)
    return;
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  pidns_leave_locked(pid);
  spin_unlock_irqrestore(&ns_lock, flags);
}

void namespace_task_exit(usize pid) {
  if (!ns_any)
    return;
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  struct ns_row *r = ns_find_locked(pid);
  if (r) {
    r->used = 0;
    r->pid = 0;
    r->pid_children = 0;
    ns_recompute_any_locked();
  }
  spin_unlock_irqrestore(&ns_lock, flags);
}

/* ── PID namespaces ───────────────────────────────────────────────────────
 *
 * The kernel numbers tasks once, globally. A pid namespace is a translation
 * table over that: `map` pairs the number this namespace uses (`vnr`) with the
 * kernel's own id (`gid`). The initial namespace needs no table — there the
 * two are the same number, which is what makes an unnamespaced boot free.
 *
 * A task is entered into its own namespace AND into every namespace between
 * that one and the initial namespace, because an ancestor must be able to name
 * (and signal, and wait for) a task inside a namespace it created. A namespace
 * that is not an ancestor has no entry, and therefore no way to name it — that
 * is the isolation. */

static int pidns_is_ancestor_locked(u32 ancestor, u32 of) {
  if (ancestor >= NS_MAX_PID || of >= NS_MAX_PID)
    return 0;
  for (u32 n = of;; n = pid_ns[n].parent) {
    if (n == ancestor)
      return 1;
    if (n == 0)
      return 0;
    if (!pid_ns[n].used)
      return 0;
  }
}

/* Number `gid` in `ns` and in every namespace up to (but not including) the
 * initial one. Best effort: a namespace whose table is full simply cannot name
 * the task, which is reported as "no such process" rather than as a wrong one. */
static void pidns_enter_locked(u32 ns, usize gid) {
  for (u32 n = ns; n != 0 && n < NS_MAX_PID && pid_ns[n].used;
       n = pid_ns[n].parent) {
    struct pid_ns *pn = &pid_ns[n];
    int taken = 0;
    for (usize i = 0; i < PIDNS_MAX_TASKS; i++)
      if (pn->map[i].vnr && pn->map[i].gid == gid)
        taken = 1;
    if (taken)
      continue;
    usize slot = PIDNS_MAX_TASKS;
    for (usize i = 0; i < PIDNS_MAX_TASKS; i++) {
      if (!pn->map[i].vnr) {
        slot = i;
        break;
      }
    }
    /* Nothing free: take the oldest number belonging to a task that has been
     * reaped. Only now does a dead entry stop being answerable, which is late
     * enough that no waiter is still holding its number. */
    if (slot == PIDNS_MAX_TASKS) {
      usize oldest = 0;
      for (usize i = 0; i < PIDNS_MAX_TASKS; i++) {
        if (!pn->map[i].dead)
          continue;
        if (slot == PIDNS_MAX_TASKS || pn->map[i].vnr < oldest) {
          slot = i;
          oldest = pn->map[i].vnr;
        }
      }
    }
    if (slot == PIDNS_MAX_TASKS)
      continue; /* full of live tasks — the task is simply not nameable here */
    pn->map[slot].vnr = pn->next_vnr++;
    pn->map[slot].gid = gid;
    pn->map[slot].dead = 0;
  }
}

static void pidns_leave_locked(usize gid) {
  for (u32 n = 1; n < NS_MAX_PID; n++)
    for (usize i = 0; i < PIDNS_MAX_TASKS; i++)
      if (pid_ns[n].map[i].vnr && pid_ns[n].map[i].gid == gid)
        pid_ns[n].map[i].dead = 1;
}

static usize pidns_vnr_locked(u32 ns, usize gid) {
  if (ns == 0)
    return gid;
  if (ns >= NS_MAX_PID || !pid_ns[ns].used)
    return 0;
  for (usize i = 0; i < PIDNS_MAX_TASKS; i++)
    if (pid_ns[ns].map[i].vnr && pid_ns[ns].map[i].gid == gid)
      return pid_ns[ns].map[i].vnr;
  return 0;
}

static usize pidns_gid_locked(u32 ns, usize vnr) {
  if (ns == 0)
    return vnr;
  if (ns >= NS_MAX_PID || !pid_ns[ns].used)
    return 0;
  for (usize i = 0; i < PIDNS_MAX_TASKS; i++)
    if (pid_ns[ns].map[i].vnr == vnr)
      return pid_ns[ns].map[i].gid;
  return 0;
}

/* The pid namespace of the task `pid` belongs to. */
static u32 pidns_of_locked(usize pid) {
  struct ns_row *r = ns_find_locked(pid);
  return r ? r->id[NS_PID] : 0;
}

usize namespace_pid_to_user(usize kernel_pid) {
  if (!ns_any || kernel_pid == 0)
    return kernel_pid;
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  u32 ns = pidns_of_locked(scheduler_get_pid());
  usize v = ns == 0 ? kernel_pid : pidns_vnr_locked(ns, kernel_pid);
  spin_unlock_irqrestore(&ns_lock, flags);
  return v;
}

usize namespace_pid_from_user(usize user_pid) {
  if (!ns_any || user_pid == 0)
    return user_pid;
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  u32 ns = pidns_of_locked(scheduler_get_pid());
  usize g = ns == 0 ? user_pid : pidns_gid_locked(ns, user_pid);
  spin_unlock_irqrestore(&ns_lock, flags);
  return g;
}

int namespace_pid_visible(usize kernel_pid) {
  return namespace_pid_to_user(kernel_pid) != 0;
}

int namespace_pid_visible_from(usize observer_pid, usize kernel_pid) {
  if (!ns_any)
    return 1;
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  u32 ns = pidns_of_locked(observer_pid);
  int vis = ns == 0 ? 1 : (pidns_vnr_locked(ns, kernel_pid) != 0);
  spin_unlock_irqrestore(&ns_lock, flags);
  return vis;
}

/* ── network namespaces ───────────────────────────────────────────────────
 *
 * The net layer needs two different answers to "which namespace is this?".
 * A socket call, an ioctl or a netlink message belongs to the caller. A frame
 * being demultiplexed belongs to the interface it arrived on, which may be in
 * a namespace no running task is currently in — so the receive path pushes a
 * context around the delivery, exactly as g_receiving_netdev already does.
 *
 * That context is PER TASK, not one word for the machine. Two CPUs demultiplex
 * frames at the same time, and a delivery can sleep (a reply generated inside
 * it waits for ARP), so a single global would let one task's receive context
 * become another task's answer to "which namespace am I in?" — which is a
 * frame stamped with, or delivered to, the wrong namespace's address.
 *
 * Each slot is claimed by its owner and read by nobody else, so the only
 * shared step is claiming a free one. A task with no slot is not inside a
 * receive path and gets the namespace it belongs to. */

/* Keyed by the task's own id, the same number namespace_task_reaped() releases
 * with: scheduler_get_pid() is the thread-group id, so keying on it would give
 * every thread of a process one shared entry and hand one thread's receive
 * context to another -- exactly the sharing this table replaced. */
#define NS_RX_SLOTS 32
struct ns_rx_slot {
  usize key; /* pid + 1, so that 0 means "free" */
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

u32 namespace_net_current(void) {
  if (!ns_any)
    return 0;
  return namespace_id_of(scheduler_get_pid(), NS_NET);
}

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
   * task's own namespace, which is what it did before this table existed. */
  return 0;
}

/* Give up the slot of a task that is gone. */
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
  if (ns == 0)
    return 1;
  if (ns >= NS_MAX_NET)
    return 0;
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  int live = net_ns_used[ns];
  spin_unlock_irqrestore(&ns_lock, flags);
  return live;
}

/* ── UTS ────────────────────────────────────────────────────────────────── */

static struct uts_ns *uts_current_locked(void) {
  ns_ensure_init();
  u32 id = 0;
  if (ns_any) {
    struct ns_row *r = ns_find_locked(scheduler_get_pid());
    if (r)
      id = r->id[NS_UTS];
  }
  if (id >= NS_MAX_UTS || !uts_ns[id].used)
    id = 0;
  return &uts_ns[id];
}

void namespace_uts_get_host(char *buf, usize len) {
  if (!buf || len == 0)
    return;
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  ns_copy_name(buf, len, uts_current_locked()->host);
  spin_unlock_irqrestore(&ns_lock, flags);
}

void namespace_uts_get_domain(char *buf, usize len) {
  if (!buf || len == 0)
    return;
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  ns_copy_name(buf, len, uts_current_locked()->domain);
  spin_unlock_irqrestore(&ns_lock, flags);
}

int namespace_uts_set_host(const char *name) {
  if (!name)
    return -EFAULT;
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  struct uts_ns *u = uts_current_locked();
  ns_copy_name(u->host, sizeof(u->host), name);
  spin_unlock_irqrestore(&ns_lock, flags);
  return 0;
}

int namespace_uts_set_domain(const char *name) {
  if (!name)
    return -EFAULT;
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  struct uts_ns *u = uts_current_locked();
  ns_copy_name(u->domain, sizeof(u->domain), name);
  spin_unlock_irqrestore(&ns_lock, flags);
  return 0;
}

/* ── unshare(2) / setns(2) ──────────────────────────────────────────────── */

static int ns_unshare_uts(usize pid) {
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  u32 slot = 0;
  for (u32 i = 1; i < NS_MAX_UTS; i++) {
    if (uts_ns[i].used)
      continue;
    slot = i;
    break;
  }
  if (!slot) {
    spin_unlock_irqrestore(&ns_lock, flags);
    return -ENOSPC;
  }
  struct ns_row *r = ns_get_or_add_locked(pid);
  if (!r) {
    spin_unlock_irqrestore(&ns_lock, flags);
    return -ENOSPC;
  }
  /* A new UTS namespace starts as a copy of the one being left, exactly as
   * Linux does — `unshare -u` with no sethostname still reports the old
   * name. */
  struct uts_ns *old = uts_current_locked();
  uts_ns[slot].used = 1;
  ns_copy_name(uts_ns[slot].host, sizeof(uts_ns[slot].host), old->host);
  ns_copy_name(uts_ns[slot].domain, sizeof(uts_ns[slot].domain), old->domain);
  r->id[NS_UTS] = slot;
  spin_unlock_irqrestore(&ns_lock, flags);
  return 0;
}

static int ns_unshare_mnt(usize pid) {
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  u32 slot = 0;
  for (u32 i = 1; i < NS_MAX_MNT; i++) {
    if (mnt_ns_used[i])
      continue;
    slot = i;
    break;
  }
  if (!slot) {
    spin_unlock_irqrestore(&ns_lock, flags);
    return -ENOSPC;
  }
  struct ns_row *r = ns_get_or_add_locked(pid);
  if (!r) {
    spin_unlock_irqrestore(&ns_lock, flags);
    return -ENOSPC;
  }
  u32 from = r->id[NS_MNT];
  mnt_ns_used[slot] = 1; /* claimed, so no concurrent unshare takes it */
  spin_unlock_irqrestore(&ns_lock, flags);

  /* The copy takes VFS locks and must not run under ns_lock. */
  int rc = vfs_mnt_ns_clone(from, slot);

  spin_lock_irqsave(&ns_lock, &flags);
  if (rc != 0) {
    mnt_ns_used[slot] = 0;
    spin_unlock_irqrestore(&ns_lock, flags);
    vfs_mnt_ns_destroy(slot);
    return rc;
  }
  r = ns_get_or_add_locked(pid);
  if (r)
    r->id[NS_MNT] = slot;
  spin_unlock_irqrestore(&ns_lock, flags);
  return 0;
}

/* unshare(CLONE_NEWPID). Linux does not move the caller into the new
 * namespace — it records it as the namespace the caller's future children are
 * created in, which is why `unshare -p` needs a fork before anything is
 * numbered from 1. Doing the same here keeps the caller's own pid, and every
 * pid it already knows, meaningful. */
static int ns_unshare_pid(usize pid) {
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  ns_ensure_init();
  u32 slot = 0;
  for (u32 i = 1; i < NS_MAX_PID; i++) {
    if (pid_ns[i].used)
      continue;
    slot = i;
    break;
  }
  if (!slot) {
    spin_unlock_irqrestore(&ns_lock, flags);
    return -ENOSPC;
  }
  struct ns_row *r = ns_get_or_add_locked(pid);
  if (!r) {
    spin_unlock_irqrestore(&ns_lock, flags);
    return -ENOSPC;
  }
  memset(&pid_ns[slot], 0, sizeof(pid_ns[slot]));
  pid_ns[slot].used = 1;
  pid_ns[slot].parent = r->pid_children ? r->pid_children : r->id[NS_PID];
  pid_ns[slot].next_vnr = 1;
  r->pid_children = slot;
  spin_unlock_irqrestore(&ns_lock, flags);
  return 0;
}

static int ns_unshare_net(usize pid) {
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  ns_ensure_init();
  u32 slot = 0;
  for (u32 i = 1; i < NS_MAX_NET; i++) {
    if (net_ns_used[i])
      continue;
    slot = i;
    break;
  }
  if (!slot) {
    spin_unlock_irqrestore(&ns_lock, flags);
    return -ENOSPC;
  }
  struct ns_row *r = ns_get_or_add_locked(pid);
  if (!r) {
    spin_unlock_irqrestore(&ns_lock, flags);
    return -ENOSPC;
  }
  net_ns_used[slot] = 1;
  r->id[NS_NET] = slot;
  spin_unlock_irqrestore(&ns_lock, flags);
  /* A fresh network namespace starts empty, exactly as Linux's does: no
   * interfaces (not even the physical one), no routes, no neighbours. The
   * loopback datapath is per-namespace in the sense that it never leaves it. */
  return 0;
}

int namespace_unshare(u64 flags) {
  /* Namespaces b1nix does not have. Refusing is the honest answer: a task that
   * believed unshare(CLONE_NEWUSER) had worked would go on to trust an id
   * mapping that does not exist. */
  if (flags & (B1NIX_CLONE_NEWUSER | B1NIX_CLONE_NEWIPC | B1NIX_CLONE_NEWCGROUP))
    return -EINVAL;
  u64 want = flags & (B1NIX_CLONE_NEWNS | B1NIX_CLONE_NEWUTS |
                      B1NIX_CLONE_NEWPID | B1NIX_CLONE_NEWNET);
  if (!want)
    return 0; /* CLONE_FS/FILES/SYSVSEM: this task already owns all of them */

  ns_gc();
  usize pid = scheduler_get_pid();

  if (want & B1NIX_CLONE_NEWUTS) {
    int rc = ns_unshare_uts(pid);
    if (rc != 0)
      return rc;
  }
  if (want & B1NIX_CLONE_NEWNS) {
    int rc = ns_unshare_mnt(pid);
    if (rc != 0)
      return rc;
  }
  if (want & B1NIX_CLONE_NEWPID) {
    int rc = ns_unshare_pid(pid);
    if (rc != 0)
      return rc;
  }
  if (want & B1NIX_CLONE_NEWNET) {
    int rc = ns_unshare_net(pid);
    if (rc != 0)
      return rc;
  }
  return 0;
}

/* clone(CLONE_NEW*): build the namespaces the next child is born into.
 *
 * Called by the PARENT, before the fork, with interrupts enabled — cloning a
 * mount table allocates and takes VFS locks. namespace_fork_inherit then
 * stamps the child with what was prepared here, under the same lock that makes
 * the child visible, so there is no window in which the child exists outside
 * the namespaces it asked for.
 *
 * Ignoring the flags instead — which is what happened before this existed —
 * is not a smaller version of the feature, it is the wrong answer: a process
 * that asked for a private mount namespace and quietly got the shared one goes
 * on to remount things "for itself", and every one of those mounts is
 * everyone's. */
int namespace_child_prepare(u64 flags) {
  u64 want = flags & (B1NIX_CLONE_NEWNS | B1NIX_CLONE_NEWUTS |
                      B1NIX_CLONE_NEWNET);
  if (!want)
    return 0;

  ns_gc();
  usize pid = scheduler_get_pid();
  u32 prepared[NS_KIND_COUNT] = {0};

  u64 lf;
  spin_lock_irqsave(&ns_lock, &lf);
  ns_ensure_init();
  struct ns_row *r = ns_get_or_add_locked(pid);
  if (!r) {
    spin_unlock_irqrestore(&ns_lock, lf);
    return -ENOSPC;
  }
  u32 from_mnt = r->id[NS_MNT];

  if (want & B1NIX_CLONE_NEWUTS) {
    u32 slot = 0;
    for (u32 i = 1; i < NS_MAX_UTS; i++)
      if (!uts_ns[i].used) { slot = i; break; }
    if (!slot) {
      spin_unlock_irqrestore(&ns_lock, lf);
      return -ENOSPC;
    }
    struct uts_ns *old = uts_current_locked();
    uts_ns[slot].used = 1;
    ns_copy_name(uts_ns[slot].host, sizeof(uts_ns[slot].host), old->host);
    ns_copy_name(uts_ns[slot].domain, sizeof(uts_ns[slot].domain), old->domain);
    prepared[NS_UTS] = slot;
  }
  if (want & B1NIX_CLONE_NEWNET) {
    u32 slot = 0;
    for (u32 i = 1; i < NS_MAX_NET; i++)
      if (!net_ns_used[i]) { slot = i; break; }
    if (!slot) {
      if (prepared[NS_UTS])
        uts_ns[prepared[NS_UTS]].used = 0;
      spin_unlock_irqrestore(&ns_lock, lf);
      return -ENOSPC;
    }
    net_ns_used[slot] = 1;
    prepared[NS_NET] = slot;
  }
  u32 mnt_slot = 0;
  if (want & B1NIX_CLONE_NEWNS) {
    for (u32 i = 1; i < NS_MAX_MNT; i++)
      if (!mnt_ns_used[i]) { mnt_slot = i; break; }
    if (!mnt_slot) {
      if (prepared[NS_UTS])
        uts_ns[prepared[NS_UTS]].used = 0;
      if (prepared[NS_NET])
        net_ns_used[prepared[NS_NET]] = 0;
      spin_unlock_irqrestore(&ns_lock, lf);
      return -ENOSPC;
    }
    mnt_ns_used[mnt_slot] = 1; /* claimed, so no concurrent unshare takes it */
  }
  spin_unlock_irqrestore(&ns_lock, lf);

  /* The mount-table copy takes VFS locks and must not run under ns_lock. */
  if (mnt_slot) {
    int rc = vfs_mnt_ns_clone(from_mnt, mnt_slot);
    if (rc != 0) {
      spin_lock_irqsave(&ns_lock, &lf);
      mnt_ns_used[mnt_slot] = 0;
      if (prepared[NS_UTS])
        uts_ns[prepared[NS_UTS]].used = 0;
      if (prepared[NS_NET])
        net_ns_used[prepared[NS_NET]] = 0;
      spin_unlock_irqrestore(&ns_lock, lf);
      vfs_mnt_ns_destroy(mnt_slot);
      return rc;
    }
    prepared[NS_MNT] = mnt_slot;
  }

  spin_lock_irqsave(&ns_lock, &lf);
  r = ns_get_or_add_locked(pid);
  if (r)
    for (int k = 0; k < NS_KIND_COUNT; k++)
      if (prepared[k])
        r->child_ns[k] = prepared[k];
  spin_unlock_irqrestore(&ns_lock, lf);
  return 0;
}

/* The fork failed, so nothing will ever be born into what was prepared. */
void namespace_child_prepare_abort(void) {
  if (!ns_any)
    return;
  usize pid = scheduler_get_pid();
  u32 drop_mnt = 0;
  u64 lf;
  spin_lock_irqsave(&ns_lock, &lf);
  struct ns_row *r = ns_find_locked(pid);
  if (r) {
    if (r->child_ns[NS_UTS])
      uts_ns[r->child_ns[NS_UTS]].used = 0;
    if (r->child_ns[NS_NET])
      net_ns_used[r->child_ns[NS_NET]] = 0;
    drop_mnt = r->child_ns[NS_MNT];
    if (drop_mnt)
      mnt_ns_used[drop_mnt] = 0;
    for (int k = 0; k < NS_KIND_COUNT; k++)
      r->child_ns[k] = 0;
  }
  spin_unlock_irqrestore(&ns_lock, lf);
  if (drop_mnt)
    vfs_mnt_ns_destroy(drop_mnt);
}

int namespace_setns(int kind, u32 target) {
  if (kind < 0 || kind >= NS_KIND_COUNT)
    return -EINVAL;

  usize pid = scheduler_get_pid();
  u64 flags;
  spin_lock_irqsave(&ns_lock, &flags);
  ns_ensure_init();
  if (target != 0) {
    int live;
    switch (kind) {
    case NS_UTS: live = target < NS_MAX_UTS && uts_ns[target].used; break;
    case NS_MNT: live = target < NS_MAX_MNT && mnt_ns_used[target]; break;
    case NS_PID: live = target < NS_MAX_PID && pid_ns[target].used; break;
    default:     live = target < NS_MAX_NET && net_ns_used[target]; break;
    }
    if (!live) {
      spin_unlock_irqrestore(&ns_lock, flags);
      return -EINVAL;
    }
  }
  /* setns(CLONE_NEWPID) is the same deal as unshare: the caller keeps its own
   * number and its children are born in the namespace it joined. A caller may
   * only aim at its own namespace or one descended from it — going the other
   * way would hand it numbers it is not entitled to name. */
  if (kind == NS_PID) {
    struct ns_row *pr = ns_find_locked(pid);
    u32 mine = pr ? pr->id[NS_PID] : 0;
    if (!pidns_is_ancestor_locked(mine, target)) {
      spin_unlock_irqrestore(&ns_lock, flags);
      return -EINVAL;
    }
    if (!pr)
      pr = ns_get_or_add_locked(pid);
    if (!pr) {
      spin_unlock_irqrestore(&ns_lock, flags);
      return -ENOSPC;
    }
    pr->pid_children = (target == mine) ? 0 : target;
    spin_unlock_irqrestore(&ns_lock, flags);
    ns_gc();
    return 0;
  }
  struct ns_row *r = ns_find_locked(pid);
  if (!r && target == 0) {
    spin_unlock_irqrestore(&ns_lock, flags);
    return 0; /* already in the initial namespace of this kind */
  }
  if (!r)
    r = ns_get_or_add_locked(pid);
  if (!r) {
    spin_unlock_irqrestore(&ns_lock, flags);
    return -ENOSPC;
  }
  r->id[kind] = target;
  /* Joining a network namespace re-points every later socket, ioctl and
   * netlink message at that namespace's interfaces; nothing already open is
   * rewritten, which matches Linux (a socket keeps the namespace it was
   * created in). */
  int drop = r->pid_children ? 0 : 1;
  for (int k = 0; k < NS_KIND_COUNT; k++)
    if (r->id[k])
      drop = 0;
  if (drop) {
    r->used = 0;
    r->pid = 0;
    ns_recompute_any_locked();
  }
  spin_unlock_irqrestore(&ns_lock, flags);

  ns_gc();
  return 0;
}
