/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * cgroup v2 — the unified hierarchy.
 *
 * systemd is the reason this exists. It mounts "cgroup2" on /sys/fs/cgroup
 * before it starts a single unit, makes one directory per unit, and moves the
 * unit's processes into it by writing their pid to that directory's
 * cgroup.procs. Everything it later does with a unit's processes — listing
 * them, learning that the last one has gone, killing what is left — is a read
 * or a write of a file in this tree. Without the filesystem, PID 1 falls back
 * to the v1 hierarchy, fails to mount that either, and freezes.
 *
 * Shape
 * -----
 * A mount is a directory tree in the VFS's own in-memory node graph, the same
 * way tmpfs is. Each directory IS a cgroup and carries the control files that
 * describe it; mkdir(2) creates a cgroup and rmdir(2) destroys one, which is
 * the whole of the cgroup v2 creation API.
 *
 * Membership is a side table keyed by task id (struct task must not grow — see
 * kernel/sched/namespace.c for the same constraint). "No entry" means the root
 * cgroup, so a machine that never mounts cgroup2 allocates nothing and every
 * task reports "0::/", which is what a kernel with the hierarchy unmounted
 * reports too.
 *
 * Controllers
 * -----------
 * Four, and every one of them is enforced. A controller is advertised only
 * when it is, because advertising one means accepting a write to memory.max or
 * cpu.weight, and accepting a limit that nothing enforces is a lie told to the
 * process that set it.
 *
 * pids    A fork that would exceed a pids.max anywhere between the new task's
 *         cgroup and the root fails with EAGAIN, which is what Linux's pids
 *         controller returns.
 *
 * memory  memory.current is the resident memory of the member address spaces:
 *         every page a member process has mapped and present, anonymous and
 *         file-backed alike, counted once per address space so threads do not
 *         multiply it. It is measured, not modelled -- the same page-table walk
 *         /proc/<pid>/status reports VmRSS with. memory.max is enforced from
 *         the page-fault path: each fault charged to a limited cgroup advances
 *         a running estimate, and the moment the estimate crosses the limit the
 *         cgroup is measured exactly. Only that exact figure is allowed to
 *         decide anything, so the kernel never kills on a drifting counter.
 *         Over the limit means: reclaim, measure again, and if it is still over
 *         kill the worst task INSIDE the cgroup -- by oom_score_adj and
 *         resident size, exactly as the machine-wide killer chooses. Every one
 *         of those steps shows up in memory.events.
 *
 * cpu     cpu.weight rides the stride scheduler that already implements nice
 *         (M117). A cgroup's effective weight is the product of the weights
 *         down to it, and each of its tasks is given a stride of
 *         base * tasks_in_cgroup * 100 / effective_weight -- so the group's
 *         share of the machine is its weight, however many tasks it splits it
 *         between, which is what systemd's CPUWeight= means. cpu.max is a
 *         quota per period: the tick charges each task's CPU time to its
 *         cgroups, and a cgroup that has spent its quota has its tasks passed
 *         over by the scheduler until the period rolls over. A task inside a
 *         system call is never passed over -- it may be holding a lock the rest
 *         of the machine needs.
 *
 * io      io.stat counts the bytes and the device commands that really went to
 *         a device on a member's behalf, charged where the block layer
 *         serialises them. io.max is a ceiling on those two rates, enforced by
 *         making the next command wait.
 *
 * What is NOT here: per-cgroup pressure files (memory.pressure and friends).
 * PSI is measured machine-wide in kernel/mm/psi.c and published under
 * /proc/pressure; a per-cgroup file would have to be a copy of the global one,
 * which is worse than the honest absence a kernel without CONFIG_PSI_PER_CGROUP
 * presents.
 */

#include <b1nix/cgroup.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/inotify.h>
#include <b1nix/ktime.h>
#include <b1nix/mm.h>
#include <b1nix/namespace.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <string.h>

/* ── controllers ─────────────────────────────────────────────────────────── */

#define CG_CTRL_CPU 0x1u
#define CG_CTRL_IO 0x2u
#define CG_CTRL_MEMORY 0x4u
#define CG_CTRL_PIDS 0x8u
#define CG_CTRL_ALL (CG_CTRL_CPU | CG_CTRL_IO | CG_CTRL_MEMORY | CG_CTRL_PIDS)

/* Listed in the order Linux lists them, because a few readers compare the
 * string rather than parsing it. */
static const struct {
  const char *name;
  u32 bit;
} cg_controllers[] = {
    {"cpu", CG_CTRL_CPU},
    {"io", CG_CTRL_IO},
    {"memory", CG_CTRL_MEMORY},
    {"pids", CG_CTRL_PIDS},
};

#define CG_PIDS_MAX_UNSET 0xFFFFFFFFu
/* "max" for the 64-bit limits (memory.max, cpu quota, io.max). */
#define CG_LIM_MAX (~0ull)

/* cpu.weight: the cgroup v2 range, and its default. */
#define CG_CPU_WEIGHT_MIN 1u
#define CG_CPU_WEIGHT_MAX 10000u
#define CG_CPU_WEIGHT_DEF 100u
#define CG_CPU_PERIOD_DEF_US 100000ull

const char *cgroup_available_controllers(void) { return "cpu io memory pids"; }

/* ── the tree ────────────────────────────────────────────────────────────── */

/* How many block devices one cgroup keeps io statistics and limits for. A unit
 * touches one or two; a slot is never reclaimed, so past the fourth a cgroup
 * stops accounting new devices rather than losing the ones it has. */
#define CG_IO_DEVS 4

struct cg_io_dev {
  u32 devno; /* blk_devno(): (major << 8) | minor. 0 == free slot. */
  u64 rbytes, wbytes, rios, wios; /* io.stat, since the slot was taken */
  u64 rbps, wbps, riops, wiops;   /* io.max, CG_LIM_MAX == "max" */
  /* The one-second window io.max is expressed against. */
  u64 win_start_ns, win_rbytes, win_wbytes, win_rios, win_wios;
};

struct cgroup {
  struct cgroup *parent;
  struct cgroup *next; /* global list, for descendant walks */
  struct vfs_node *dir;
  struct vfs_node *events_node; /* cgroup.events, for the inotify edge */
  u32 subtree_control;
  u32 pids_max;
  u64 pids_denied;      /* pids.events: max */
  u32 max_depth;        /* cgroup.max.depth, CG_PIDS_MAX_UNSET = "max" */
  u32 max_descendants;  /* cgroup.max.descendants */

  /* ── memory ──
   * Limits are in pages; CG_LIM_MAX is "max". mem_exact is the last exact
   * measurement and mem_delta the faults charged since, so mem_exact+mem_delta
   * is the running estimate that decides WHEN to measure again -- never what
   * to do about it. */
  u64 mem_max;
  u64 mem_high;
  u64 mem_low;
  u64 mem_min;
  u64 mem_swap_max;
  u64 mem_exact;
  u64 mem_delta;
  u64 mem_peak;
  u64 mem_pgfault;      /* memory.stat: pages charged here */
  u64 mem_pgmajfault;   /* memory.stat: pages faulted back in from swap */
  u64 mem_pgscan;       /* memory.stat: pages the reclaim hand looked at */
  u64 mem_pgsteal;      /* memory.stat: pages it actually wrote out */
  /* memory.swap.current, in pages. mem_swap_cur counts this cgroup AND its
   * descendants, which is what the file reports; mem_swap_own counts only the
   * pages charged with this cgroup's own id, which is what has to move to the
   * parent if the cgroup is removed while its pages are still out. */
  u64 mem_swap_cur;
  u64 mem_swap_own;
  u64 mem_swap_ev_max;  /* memory.swap.events: max */
  u64 mem_swap_ev_fail; /* memory.swap.events: fail */
  u16 id;               /* index into cg_ids, 0 = the root / untracked */
  u64 mem_ev_low, mem_ev_high, mem_ev_max, mem_ev_oom, mem_ev_oom_kill;
  int mem_oom_group;
  /* When this cgroup was last taken over its memory.max or memory.high. Both
   * paths cost an exact measurement, and a cgroup that stays over its limit
   * crosses it again on the very next fault -- so there is a floor on how
   * often either may run. */
  u64 mem_oom_at_ns;
  u64 mem_high_at_ns;
  /* When reclaim last found nothing to take. A cgroup whose pages are all hot,
   * or whose swap is full, would otherwise sweep the whole eviction ring on
   * every fault -- a page-table walk per page of RAM, inside a page fault. */
  u64 mem_reclaim_dry_at_ns;

  /* ── cpu ── */
  u32 cpu_weight;             /* 1..10000 */
  u64 cpu_quota_us;           /* cpu.max, CG_LIM_MAX = "max" */
  u64 cpu_period_us;
  u64 cpu_period_start_ns;
  u64 cpu_period_used_ns;
  u64 cpu_usage_ns, cpu_user_ns, cpu_sys_ns; /* cpu.stat, since creation */
  u64 cpu_nr_periods, cpu_nr_throttled, cpu_throttled_ns;
  int cpu_throttled;
  /* Members in this cgroup alone, refreshed once per sweep. Counting it per
   * task inside the publish loop made the sweep quadratic in the task table. */
  u32 cpu_nr_tasks;

  /* ── io ──
   * Per device, because that is how cgroup v2 expresses both sides of it:
   * io.stat prints one line per device and io.max is written as
   * "MAJ:MIN rbps=... wiops=...". A slot is taken the first time a cgroup
   * touches a device or names one in io.max. */
  u32 io_weight;
  struct cg_io_dev io_dev[CG_IO_DEVS];
  int populated;        /* last value published in cgroup.events */
  int scratch;          /* cg_events_refresh's single-pass accumulator */
  int is_root;
  /* cgroup namespaces rooted here. A removed cgroup a namespace still names
   * stays allocated (off every list, with no directory) until they let go. */
  int ns_refs;
  int removed;
};

static struct cgroup *cg_root;
static struct cgroup *cg_all; /* singly linked list of every live cgroup */
static void cg_sync_children_controller_files(struct cgroup *cg);
/* Live mounts of the hierarchy: one superblock, however many places. */
static int cg_mounts;
static spinlock_t cg_lock = SPINLOCK_INIT;

/* ── membership ──────────────────────────────────────────────────────────── */

/* One entry per task that is NOT in the root cgroup. 4096 is the kernel's own
 * task ceiling (TASK_CHUNK_SIZE * TASK_MAX_CHUNKS), so the table can always
 * hold every task that exists; the hash keeps the common lookup to one probe.
 * Deletion shifts the probe chain back rather than leaving a tombstone, so a
 * long boot cannot fill the table with debris. */
#define CG_SLOTS 8192u

struct cg_member {
  usize pid; /* 0 = free */
  struct cgroup *cg;
};

static struct cg_member cg_members[CG_SLOTS];

/* ── cgroup ids ──────────────────────────────────────────────────────────────
 *
 * A swapped page is charged to an id rather than to a pointer, because the
 * page outlives everything else: the slot it sits in remembers two bytes and
 * nothing more, and the cgroup may be removed before the page comes back.
 *
 * An entry stays allocated while any swapped page still names it. When the
 * cgroup it belongs to is removed with pages still out, the entry is pointed
 * at the parent -- the charge moves up rather than disappearing, which is what
 * Linux does at css_offline -- and it is freed once the last of those pages
 * has been read back. `refs` is exactly that count.
 *
 * 1024 ids: systemd creates a cgroup per unit and a handful per session, and
 * an id is held only for as long as a cgroup exists or has pages in swap. A
 * machine that runs out gets id 0 for the next cgroup, whose swapped pages are
 * then simply not attributed -- the count stays honest by being absent rather
 * than wrong, and the console says so once. */
#define CG_IDS 1024u

struct cg_id_slot {
  struct cgroup *cg; /* 0 = free */
  u64 refs;          /* swapped pages still charged to this id */
  int orphaned;      /* the cgroup is gone; the entry is only a redirect */
};

static struct cg_id_slot cg_ids[CG_IDS];
static int cg_ids_exhausted_said;

/* Caller holds cg_lock. */
static u16 cg_id_alloc(struct cgroup *cg) {
  for (u16 i = 1; i < (u16)CG_IDS; i++) {
    if (!cg_ids[i].cg) {
      cg_ids[i].cg = cg;
      cg_ids[i].refs = 0;
      cg_ids[i].orphaned = 0;
      return i;
    }
  }
  return 0;
}

/* Caller holds cg_lock. The cgroup is going away: hand its id, and the pages
 * still charged to it, to its parent. */
static void cg_id_reparent(struct cgroup *cg) {
  if (!cg->id || cg->id >= CG_IDS)
    return;
  struct cg_id_slot *sl = &cg_ids[cg->id];

  if (sl->cg != cg)
    return;
  if (sl->refs == 0) { /* nothing out there names it */
    sl->cg = 0;
    sl->orphaned = 0;
    return;
  }
  if (cg->parent) {
    cg->parent->mem_swap_own += cg->mem_swap_own;
    sl->cg = cg->parent;
    sl->orphaned = 1;
  } else {
    sl->cg = 0;
    sl->orphaned = 0;
  }
  cg->mem_swap_own = 0;
  /* An entry already redirected AT this cgroup has to follow it up, or the
   * next uncharge would walk a freed struct. */
  for (u16 i = 1; i < (u16)CG_IDS; i++)
    if (cg_ids[i].cg == cg && i != cg->id)
      cg_ids[i].cg = cg->parent;
}

static inline u32 cg_hash(usize pid) {
  u64 h = (u64)pid * 0x9E3779B97F4A7C15ull;
  return (u32)((h >> 32) & (CG_SLOTS - 1));
}

/* Caller holds cg_lock. */
static struct cgroup *cg_member_get(usize pid) {
  u32 i = cg_hash(pid);
  for (u32 n = 0; n < CG_SLOTS; n++) {
    u32 s = (i + n) & (CG_SLOTS - 1);
    if (cg_members[s].pid == 0)
      return 0;
    if (cg_members[s].pid == pid)
      return cg_members[s].cg;
  }
  return 0;
}

/* Caller holds cg_lock. cg == NULL removes the entry (back to the root). */
static void cg_member_set(usize pid, struct cgroup *cg) {
  u32 i = cg_hash(pid);
  u32 found = CG_SLOTS;
  for (u32 n = 0; n < CG_SLOTS; n++) {
    u32 s = (i + n) & (CG_SLOTS - 1);
    if (cg_members[s].pid == pid) {
      found = s;
      break;
    }
    if (cg_members[s].pid == 0)
      break;
  }

  if (!cg) {
    if (found == CG_SLOTS)
      return;
    /* Backward-shift deletion: close the hole by pulling forward any entry
     * whose ideal slot is at or before it, so no probe chain is broken. */
    u32 hole = found;
    cg_members[hole].pid = 0;
    cg_members[hole].cg = 0;
    for (u32 n = 1; n < CG_SLOTS; n++) {
      u32 s = (hole + n) & (CG_SLOTS - 1);
      if (cg_members[s].pid == 0)
        break;
      u32 ideal = cg_hash(cg_members[s].pid);
      /* Is `ideal` cyclically within (hole, s]? If not, the entry may move. */
      u32 d_hole = (s - hole) & (CG_SLOTS - 1);
      u32 d_ideal = (s - ideal) & (CG_SLOTS - 1);
      if (d_ideal >= d_hole) {
        cg_members[hole] = cg_members[s];
        cg_members[s].pid = 0;
        cg_members[s].cg = 0;
        hole = s;
      }
    }
    return;
  }

  if (found != CG_SLOTS) {
    cg_members[found].cg = cg;
    return;
  }
  for (u32 n = 0; n < CG_SLOTS; n++) {
    u32 s = (i + n) & (CG_SLOTS - 1);
    if (cg_members[s].pid == 0) {
      cg_members[s].pid = pid;
      cg_members[s].cg = cg;
      return;
    }
  }
}

/* The cgroup a task belongs to, never NULL once a hierarchy is mounted. */
static struct cgroup *cg_of(usize pid) {
  struct cgroup *cg = cg_member_get(pid);
  return cg ? cg : cg_root;
}

static int cg_is_ancestor(const struct cgroup *anc, const struct cgroup *cg) {
  for (const struct cgroup *c = cg; c; c = c->parent)
    if (c == anc)
      return 1;
  return 0;
}

/* ── cgroup namespaces ───────────────────────────────────────────────────── */

/* The cgroup the calling task's cgroup namespace is rooted at. Caller holds
 * cg_lock. */
static struct cgroup *cg_ns_root_locked(void) {
  struct cgroup *r = namespace_cgroup_root(namespace_current_id(NS_CGROUP));
  return (r && !r->removed) ? r : cg_root;
}

void *cgroup_ns_root_get(usize pid) {
  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup *cg = cg_root ? cg_of(pid) : 0;
  if (cg == cg_root)
    cg = 0; /* the real root needs no reference */
  if (cg)
    cg->ns_refs++;
  spin_unlock_irqrestore(&cg_lock, flags);
  return cg;
}

void cgroup_ns_root_put(void *root) {
  struct cgroup *cg = root;
  if (!cg)
    return;
  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  int free_it = --cg->ns_refs == 0 && cg->removed;
  if (free_it)
    cg_id_reparent(cg);
  spin_unlock_irqrestore(&cg_lock, flags);
  if (free_it)
    kfree(cg);
}

/* ── task counting ───────────────────────────────────────────────────────── */

static int cg_task_live(const struct task *t) {
  return t && t->id && t->state != TASK_UNUSED && t->state != TASK_DEAD &&
         t->state != TASK_REAPING;
}

/* Tasks in `cg` and, when `recurse`, everything below it. Caller holds
 * cg_lock. */
static usize cg_count_tasks(struct cgroup *cg, int recurse) {
  usize n = 0;
  usize slots = scheduler_task_slots();
  for (usize i = 0; i < slots; i++) {
    struct task *t = scheduler_task_slot(i);
    if (!cg_task_live(t))
      continue;
    struct cgroup *tc = cg_of(t->id);
    if (tc == cg || (recurse && cg_is_ancestor(cg, tc)))
      n++;
  }
  return n;
}

static usize cg_count_descendants(struct cgroup *cg) {
  usize n = 0;
  for (struct cgroup *c = cg_all; c; c = c->next)
    if (c != cg && cg_is_ancestor(cg, c))
      n++;
  return n;
}

static u32 cg_depth(const struct cgroup *cg) {
  u32 d = 0;
  for (const struct cgroup *c = cg; c && c->parent; c = c->parent)
    d++;
  return d;
}

/* The kernel's own task ceiling (TASK_CHUNK_SIZE * TASK_MAX_CHUNKS), which is
 * what scheduler_task_index() is bounded by. The two side tables below are
 * indexed by it rather than by pid, because the tick reads them once per task
 * per tick and a hash probe there would be the most expensive thing in it. */
#define CG_TASK_SLOTS 4096
/* Total CPU nanoseconds each task had burned when the tick last looked, and
 * how much of that was user time. The difference is what gets charged. */
static u64 cg_task_cpu_seen[CG_TASK_SLOTS];
static u64 cg_task_cpu_user_seen[CG_TASK_SLOTS];

/* ── memory: measurement, charging, and the kill ─────────────────────────── */

/* How many member tasks one measurement or one kill decision may look at.
 * Every slice systemd makes is far below this; a cgroup with more members than
 * this is measured from the first CG_MEM_TASKS of them, which can only
 * under-count and so can only under-enforce. */
#define CG_MEM_TASKS 512
/* Distinct address spaces remembered while de-duplicating threads. */
#define CG_MEM_SPACES 256

/* Snapshot the live tasks in `cg` and everything below it. Caller holds
 * cg_lock; the array is read after the lock is dropped, which is safe because
 * a task slot is never freed, only recycled (and a recycled slot fails the
 * liveness re-check below). Returns how many were stored. */
static int cg_collect_tasks(struct cgroup *cg, struct task **out, int cap) {
  int n = 0;
  usize slots = scheduler_task_slots();

  for (usize i = 0; i < slots && n < cap; i++) {
    struct task *t = scheduler_task_slot(i);

    if (!cg_task_live(t))
      continue;
    struct cgroup *tc = cg_of(t->id);
    if (tc != cg && !cg_is_ancestor(cg, tc))
      continue;
    out[n++] = t;
  }
  return n;
}

/* The resident memory of a set of tasks, in pages, counting each address space
 * once so a threaded process is not multiplied by its thread count.
 *
 * Called WITHOUT cg_lock: the walk it performs is the one /proc/<pid>/status
 * does for VmRSS, which takes tens of thousands of table reads for a large
 * process and must not run with interrupts off. */
static u64 cg_mem_measure_tasks(struct task **tasks, int n) {
  u64 seen[CG_MEM_SPACES];
  int nseen = 0;
  u64 pages = 0;

  for (int i = 0; i < n; i++) {
    struct task *t = tasks[i];

    if (!cg_task_live(t) || !t->pml4_phys)
      continue;
    int dup = 0;
    for (int k = 0; k < nseen; k++)
      if (seen[k] == t->pml4_phys) {
        dup = 1;
        break;
      }
    if (dup)
      continue;
    if (nseen < CG_MEM_SPACES)
      seen[nseen++] = t->pml4_phys;
    pages += task_rss_current_pages(t);
  }
  return pages;
}

/* Measure `cg` exactly and publish the result, clearing the running estimate's
 * delta. Returns the measurement in pages. Must be called in task context. */
static u64 cg_mem_refresh(struct cgroup *cg) {
  struct task *tasks[CG_MEM_TASKS];
  u64 flags;
  int n;

  spin_lock_irqsave(&cg_lock, &flags);
  n = cg_collect_tasks(cg, tasks, CG_MEM_TASKS);
  spin_unlock_irqrestore(&cg_lock, flags);

  u64 pages = cg_mem_measure_tasks(tasks, n);

  spin_lock_irqsave(&cg_lock, &flags);
  cg->mem_exact = pages;
  cg->mem_delta = 0;
  if (pages > cg->mem_peak)
    cg->mem_peak = pages;
  spin_unlock_irqrestore(&cg_lock, flags);
  return pages;
}

/* Linux's badness, in the shape that matters here: resident pages, shifted by
 * oom_score_adj as a thousandth of the machine. -1000 means never. */
static u64 cg_oom_badness(struct task *t, int *immune) {
  int adj = scheduler_oom_score_adj(t->id);

  *immune = 0;
  if (adj <= -1000 || t->id == 1) {
    *immune = 1;
    return 0;
  }
  u64 rss = task_rss_current_pages(t);
  u64 total = pmm_total_usable_memory() / PAGE_SIZE;
  i64 bias = total ? ((i64)adj * (i64)total) / 1000 : 0;
  i64 points = (i64)rss + bias;

  return points > 0 ? (u64)points : 1;
}

/* The task the OOM killer should choose. `within` is a struct cgroup (0 for the
 * whole machine). Exported through cgroup.h so the machine-wide killer in the
 * page allocator picks by the same rule. */
usize cgroup_oom_victim(void *within) {
  struct cgroup *cg = within;
  struct task *tasks[CG_MEM_TASKS];
  int n = 0;
  u64 flags;

  if (cg) {
    spin_lock_irqsave(&cg_lock, &flags);
    n = cg_collect_tasks(cg, tasks, CG_MEM_TASKS);
    spin_unlock_irqrestore(&cg_lock, flags);
  } else {
    usize slots = scheduler_task_slots();

    for (usize i = 0; i < slots && n < CG_MEM_TASKS; i++) {
      struct task *t = scheduler_task_slot(i);

      if (cg_task_live(t) && t->pml4_phys)
        tasks[n++] = t;
    }
  }

  struct task *best = 0;
  u64 best_points = 0;

  for (int i = 0; i < n; i++) {
    struct task *t = tasks[i];
    int immune = 0;

    if (!cg_task_live(t) || !t->pml4_phys)
      continue;
    /* A task already condemned is not a candidate: choosing it again would
     * make the killer think it had made progress when it had not. */
    if (t->pending_signals & (1ULL << (SIGKILL - 1)))
      continue;
    u64 points = cg_oom_badness(t, &immune);
    if (immune)
      continue;
    if (!best || points > best_points) {
      best = t;
      best_points = points;
    }
  }
  return best ? best->id : 0;
}

/* How long a cgroup is left alone after it has been taken over memory.max or
 * memory.high. A cgroup that is over stays over until the process that took it
 * there dies, and the fault after the kill crosses the limit again: without a
 * floor, every one of those faults ran a fresh measurement, which is a
 * page-table walk of every member -- the machine stopped making progress
 * altogether and a spinlock lockup was reported somewhere unrelated. */
#define CG_MEM_ACTION_COOLDOWN_NS (100ull * 1000000ull)

/* Pages one reclaim attempt may take, and how many attempts one crossing of
 * memory.max may make. See cg_reclaim and cg_mem_over_limit. */
#define CG_RECLAIM_BATCH 32u
/* Batches the kill path may reclaim before it decides that killing is the only
 * answer left. It runs at most once per cooldown, has already paid for an
 * exact measurement, and is the last thing between a cgroup and a dead
 * process -- so it tries considerably harder than a fault does. */
#define CG_RECLAIM_ROUNDS 16u
/* How many batches one crossing of memory.max may reclaim before giving up and
 * letting the kill path decide. 32 x 8 = 256 pages, a megabyte, per fault. */
#define CG_RELIEVE_ROUNDS 8

/* ── swap accounting and cgroup-targeted reclaim ─────────────────────────── */

u16 cgroup_id_of_task(usize pid) {
  if (!cg_root || !pid)
    return 0;
  u64 flags;

  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup *cg = cg_of(pid);
  u16 id = cg ? cg->id : 0;
  spin_unlock_irqrestore(&cg_lock, flags);
  return id;
}

int cgroup_swap_charge(u16 id, u64 pages) {
  if (!id || id >= CG_IDS || !pages)
    return 0;
  u64 flags;

  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup *cg = cg_ids[id].cg;
  if (!cg) {
    spin_unlock_irqrestore(&cg_lock, flags);
    return 0; /* the id outlived everything it named */
  }
  /* memory.swap.max is a limit on the subtree, so every ancestor gets a veto
   * and the whole charge is refused if any of them says no. Refusing is the
   * point: Linux stops swapping for that cgroup and lets the memory pressure
   * fall back on memory.max, which ends in reclaim or a kill. */
  for (struct cgroup *a = cg; a; a = a->parent) {
    if (a->mem_swap_max != CG_LIM_MAX &&
        a->mem_swap_cur + pages > a->mem_swap_max) {
      a->mem_swap_ev_max++;
      a->mem_swap_ev_fail++;
      spin_unlock_irqrestore(&cg_lock, flags);
      return -1;
    }
  }
  for (struct cgroup *a = cg; a; a = a->parent)
    a->mem_swap_cur += pages;
  cg->mem_swap_own += pages;
  cg_ids[id].refs += pages;
  spin_unlock_irqrestore(&cg_lock, flags);
  return 0;
}

void cgroup_swap_uncharge(u16 id, u64 pages) {
  if (!id || id >= CG_IDS || !pages)
    return;
  u64 flags;

  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup *cg = cg_ids[id].cg;
  if (cg) {
    for (struct cgroup *a = cg; a; a = a->parent)
      a->mem_swap_cur = a->mem_swap_cur > pages ? a->mem_swap_cur - pages : 0;
    cg->mem_swap_own = cg->mem_swap_own > pages ? cg->mem_swap_own - pages : 0;
  }
  if (cg_ids[id].refs > pages)
    cg_ids[id].refs -= pages;
  else {
    cg_ids[id].refs = 0;
    /* The last page charged to a cgroup that is already gone: the id has
     * nothing left to redirect and can be handed out again. */
    if (cg_ids[id].orphaned) {
      cg_ids[id].cg = 0;
      cg_ids[id].orphaned = 0;
    }
  }
  spin_unlock_irqrestore(&cg_lock, flags);
}

/* Reclaim inside a cgroup: write its own pages out to swap until it is back
 * under `target_pages`, and never touch anybody else's.
 *
 * This is what makes memory.max a limit rather than a death sentence. A
 * cgroup over its limit with anonymous memory that is not being touched now
 * loses that memory to swap and carries on, which is what the process that set
 * the limit asked for; only a cgroup that cannot give anything back reaches
 * the kill below. With no swap device attached there is nowhere to put the
 * pages, the scan frees nothing, and the behaviour is exactly what it was. */
static usize cg_reclaim(struct cgroup *cg, u64 over_pages) {
  if (!swap_active() || !over_pages)
    return 0;

  u64 now = ktime_monotonic_ns();
  u64 flags;

  spin_lock_irqsave(&cg_lock, &flags);
  u64 dry = cg->mem_reclaim_dry_at_ns;
  spin_unlock_irqrestore(&cg_lock, flags);
  /* Nothing to take a moment ago means nothing to take now: the pages this
   * cgroup owns are all in use, or swap is full. Either way the answer does
   * not change between two faults, and looking for it again costs a sweep of
   * the ring -- which is what made a cgroup with a full swap device stop the
   * machine rather than merely stop itself. */
  if (dry && now - dry < CG_MEM_ACTION_COOLDOWN_NS)
    return 0;

  /* How much one crossing of the limit may reclaim. Linux reclaims in
   * clusters (SWAP_CLUSTER_MAX, 32 pages) for the same reason: the allocation
   * that crossed the limit needs a little room now, not the whole excess, and
   * the next allocation will come back here in a moment anyway. Asking for the
   * whole excess made a single fault write megabytes out. */
  if (over_pages > CG_RECLAIM_BATCH)
    over_pages = CG_RECLAIM_BATCH;

  /* The members, taken once: reclaim walks each task's own pages, so the list
   * is the whole of what this cgroup may take from. A task that exits while we
   * work is handled by the ring, which forgets its pages on the way out. */
  struct task *tasks[CG_MEM_TASKS];
  int n;

  spin_lock_irqsave(&cg_lock, &flags);
  n = cg_collect_tasks(cg, tasks, CG_MEM_TASKS);
  spin_unlock_irqrestore(&cg_lock, flags);

  usize scanned = 0;
  usize freed = 0;

  for (int i = 0; i < n && freed < (usize)over_pages; i++)
    freed += eviction_reclaim_task(tasks[i], (usize)over_pages - freed,
                                   &scanned);

  spin_lock_irqsave(&cg_lock, &flags);
  for (struct cgroup *a = cg; a; a = a->parent) {
    a->mem_pgscan += scanned;
    a->mem_pgsteal += freed;
    /* The pages are gone from this cgroup's resident set, so the running
     * estimate has to lose them too -- otherwise the next fault reads an
     * estimate that still counts them, decides the cgroup is over, and
     * reclaims again on evidence that reclaim itself has already answered. */
    a->mem_delta = a->mem_delta > freed ? a->mem_delta - freed : 0;
  }
  cg->mem_reclaim_dry_at_ns = freed ? 0 : now;
  spin_unlock_irqrestore(&cg_lock, flags);
  return freed;
}


/* Reclaim until the cgroup's own estimate says it is back inside its limit, or
 * until reclaim stops making progress.
 *
 * This is the shape of Linux's try_charge loop, and the reason for it is that
 * one batch per fault is not a limit: a task that allocates faster than 32
 * pages per fault stays over the limit however long it runs, and the kill
 * comes for a cgroup that had cold pages it could have given back. Each round
 * is small so the fault does not disappear for a long time, and the estimate
 * -- which cg_reclaim keeps honest by subtracting what it freed -- is what
 * decides when to stop, because measuring exactly costs a walk of every
 * member's page tables and this runs inside a page fault.
 */
static void cg_mem_relieve(struct cgroup *cg, u64 max_pages) {
  for (int round = 0; round < CG_RELIEVE_ROUNDS; round++) {
    u64 flags;

    spin_lock_irqsave(&cg_lock, &flags);
    u64 est = cg->mem_exact + cg->mem_delta;
    spin_unlock_irqrestore(&cg_lock, flags);

    if (est <= max_pages)
      return;
    if (cg_reclaim(cg, CG_RECLAIM_BATCH) == 0)
      return; /* nothing left to take: the kill path decides from here */
  }
}

/* Over memory.max: measure exactly, reclaim inside the cgroup, and kill inside
 * it only if that was not enough. Called in task context with no cgroup lock
 * held. Returns the pid killed, or 0 when nothing had to die -- because the
 * exact measurement disagreed with the estimate, or because reclaim brought
 * the cgroup back under its limit.
 *
 * Reclaim first, as Linux does. What it may take is restricted to this
 * cgroup's own pages: freeing another cgroup's memory to keep this one inside
 * its limit is not what memory.max means, and the charge for every page it
 * writes out lands on this cgroup's memory.swap.current. */
static usize cg_mem_over_limit(struct cgroup *cg, u64 max_pages) {
  u64 now = cg_mem_refresh(cg);

  if (now <= max_pages)
    return 0;

  /* Reclaim in rounds until the cgroup is back inside its limit or there is
   * nothing left to take. Each round is a bounded slice of the eviction ring;
   * between rounds the excess is tracked by subtracting what was freed rather
   * than by measuring again, because measuring walks the page tables of every
   * member and doing that per round would cost more than the reclaim. */
  usize total = 0;

  for (usize round = 0; round < CG_RECLAIM_ROUNDS && now > max_pages; round++) {
    usize freed = cg_reclaim(cg, now - max_pages > CG_RECLAIM_BATCH
                                     ? CG_RECLAIM_BATCH
                                     : (usize)(now - max_pages));

    if (!freed)
      break;
    total += freed;
    now = now > freed ? now - freed : 0;
  }
  if (total) {
    now = cg_mem_refresh(cg);
    if (now <= max_pages)
      return 0; /* it gave the memory back instead of dying */
  }

  usize victim = cgroup_oom_victim(cg);
  u64 flags;

  spin_lock_irqsave(&cg_lock, &flags);
  cg->mem_ev_max++;
  cg->mem_ev_oom++;
  if (victim)
    cg->mem_ev_oom_kill++;
  struct vfs_node *ev = cg->events_node;
  spin_unlock_irqrestore(&cg_lock, flags);

  if (victim) {
    console_write("[OOM-KILL] cgroup over memory.max — killing pid ");
    console_write_dec(victim);
    console_write("\n");
    scheduler_kill(victim, SIGKILL);
  }
  if (ev)
    vfs_inotify_notify(ev, IN_MODIFY, 0);
  return victim;
}

void cgroup_mem_note_majfault(void) {
  if (!cg_root || !current_task)
    return;
  u64 flags;

  spin_lock_irqsave(&cg_lock, &flags);
  for (struct cgroup *a = cg_of(current_task->id); a; a = a->parent)
    a->mem_pgmajfault++;
  spin_unlock_irqrestore(&cg_lock, flags);
}

/* memory.high: reclaim, and throttle the allocator while it happens.
 *
 * Linux reclaims inside the cgroup and puts the allocating task to sleep in
 * proportion to how far past memory.high it is. Both halves are here: the
 * reclaim takes this cgroup's own cold pages out to swap, and the sleep is
 * what is left when there is nothing to take -- a cgroup past its soft limit
 * allocates more slowly than one inside it, and is never stopped. One tick per
 * crossing, and crossings are rate-limited. */
static void cg_mem_check_high(struct cgroup *cg) {
  u64 flags;
  u64 high;

  spin_lock_irqsave(&cg_lock, &flags);
  high = cg->mem_high;
  spin_unlock_irqrestore(&cg_lock, flags);
  if (high == CG_LIM_MAX)
    return;

  u64 now = cg_mem_refresh(cg);

  if (now <= high)
    return;
  spin_lock_irqsave(&cg_lock, &flags);
  cg->mem_ev_high++;
  spin_unlock_irqrestore(&cg_lock, flags);
  if (cg_reclaim(cg, now - high) > 0 && cg_mem_refresh(cg) <= high)
    return; /* back inside the soft limit: no reason to hold the task up */
  scheduler_sleep_ticks(1);
}

/* How many cgroups want memory accounting -- a limit, or the controller
 * enabled. Zero is the normal case and makes the fault hook a load and a
 * branch; the machine that never mounts cgroup2 never pays anything. */
static volatile int cg_mem_limited;

void cgroup_mem_fault_charge(u64 fault_addr) {
  if (fault_addr >= USER_SPACE_LIMIT)
    return; /* a kernel page: nothing charges kernel memory to a cgroup here */
  /* The charge can reclaim, and reclaim must not take the page this fault has
   * just installed -- the instruction that faulted is about to touch it. */
  eviction_protect_begin(current_task, fault_addr);
  cgroup_mem_charge_pages(1);
  eviction_protect_end();
}

void cgroup_mem_charge_pages(u64 npages) {
  if (!cg_root || !npages ||
      !__atomic_load_n(&cg_mem_limited, __ATOMIC_RELAXED))
    return;
  struct task *cur = current_task;
  if (!cur || !cur->pml4_phys)
    return;

  struct cgroup *over = 0, *high = 0;
  u64 over_max = 0;
  u64 flags;

  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup *cg = cg_of(cur->id);
  for (struct cgroup *a = cg; a; a = a->parent) {
    a->mem_pgfault += npages;
    a->mem_delta += npages;
    u64 est = a->mem_exact + a->mem_delta;

    if (est > a->mem_peak)
      a->mem_peak = est;
    if (!high && a->mem_high != CG_LIM_MAX && est > a->mem_high)
      high = a;
    if (a->mem_max == CG_LIM_MAX)
      continue;
    if (!over && est > a->mem_max) {
      over = a;
      over_max = a->mem_max;
    }
  }
  spin_unlock_irqrestore(&cg_lock, flags);

  u64 now = (high || over) ? ktime_monotonic_ns() : 0;

  if (high && high != over) {
    int act;

    spin_lock_irqsave(&cg_lock, &flags);
    act = high->mem_high_at_ns == 0 ||
          now - high->mem_high_at_ns >= CG_MEM_ACTION_COOLDOWN_NS;
    if (act)
      high->mem_high_at_ns = now;
    spin_unlock_irqrestore(&cg_lock, flags);
    if (act)
      cg_mem_check_high(high);
  }
  if (!over)
    return;

  /* Reclaim first, and on every fault that crosses the limit -- not once per
   * cooldown. The cooldown below exists because measuring a cgroup exactly and
   * killing inside it are expensive and must not happen per fault; reclaim is
   * neither. One small batch per crossing is what keeps a cgroup that has cold
   * pages inside its limit instead of killing it, and it is self-limiting: a
   * cgroup that is not over does not come here at all.
   *
   * It also throttles, which is the other half of what Linux does here: the
   * task that went over the limit pays for the reclaim in its own fault. */
  cg_mem_relieve(over, over_max);

  {
    int act;

    spin_lock_irqsave(&cg_lock, &flags);
    act = over->mem_oom_at_ns == 0 ||
          now - over->mem_oom_at_ns >= CG_MEM_ACTION_COOLDOWN_NS;
    if (act)
      over->mem_oom_at_ns = now;
    spin_unlock_irqrestore(&cg_lock, flags);
    if (!act)
      return;
  }
  /* The fault itself is never failed, not even when the task that just faulted
   * is the one chosen to die. Failing it turned a clean SIGKILL into an
   * unhandled SIGSEGV reported against a page the kernel had in fact just
   * installed: the process died either way, but of the wrong signal and with a
   * fault report that named nothing real. The victim carries a pending SIGKILL
   * and dies on its next return to ring 3. */
  (void)cg_mem_over_limit(over, over_max);
}

/* ── cpu: weights on the stride scheduler, and the quota ─────────────────── */

/* cpu.weight.nice <-> cpu.weight, the same table Linux maps them through: the
 * scheduler weight of each nice value, rescaled so that nice 0 is 100. */
static const u32 cg_prio_to_weight[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
    9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
    1024,  820,   655,   526,   423,   335,   272,   215,   172,   137,
    110,   87,    70,    56,    45,    36,    29,    23,    18,    15,
};

static u32 cg_nice_to_weight(int nice) {
  if (nice < -20)
    nice = -20;
  if (nice > 19)
    nice = 19;
  u64 w = ((u64)cg_prio_to_weight[nice + 20] * CG_CPU_WEIGHT_DEF + 512) / 1024;

  if (w < CG_CPU_WEIGHT_MIN)
    w = CG_CPU_WEIGHT_MIN;
  if (w > CG_CPU_WEIGHT_MAX)
    w = CG_CPU_WEIGHT_MAX;
  return (u32)w;
}

/* The nice value whose weight is nearest to `w` — what Linux reports back. */
static int cg_weight_to_nice(u32 w) {
  int best = 0;
  u64 best_err = ~0ull;

  for (int n = -20; n <= 19; n++) {
    u32 cand = cg_nice_to_weight(n);
    u64 err = cand > w ? cand - w : w - cand;

    if (err < best_err) {
      best_err = err;
      best = n;
    }
  }
  return best;
}

/* The weight a cgroup really carries: the product of the weights from the root
 * down to it, in units where 100 is "unweighted". Caller holds cg_lock. */
static u64 cg_cpu_effective_weight(struct cgroup *cg) {
  u64 w = CG_CPU_WEIGHT_DEF;

  for (struct cgroup *c = cg; c && !c->is_root; c = c->parent) {
    if (!c->parent || !(c->parent->subtree_control & CG_CTRL_CPU))
      continue;
    w = (w * c->cpu_weight) / CG_CPU_WEIGHT_DEF;
    if (w < 1)
      w = 1;
  }
  return w ? w : 1;
}

/* How often the stride weights are recomputed. A weight change takes effect
 * within this, which is well below the window any CPUWeight test measures. */
#define CG_SWEEP_MS 100

/* Set when any cgroup has the cpu controller enabled, and separately when one
 * has a cpu.max. Without a quota nothing can change between sweeps, so the
 * tick does nothing at all ninety-nine times in a hundred; a machine that
 * never touches the controller pays one load. */
static volatile int cg_cpu_active;
static volatile int cg_cpu_quota;

/* Take the scheduler's cgroup weighting off every task. Called when the
 * hierarchy goes away: cgroup_tick is what publishes those values and it stops
 * running with the hierarchy, so a throttled task would stay throttled for
 * ever. Caller holds no cgroup lock. */
static void cg_sched_clear_all(void) {
  usize slots = scheduler_task_slots();

  for (usize i = 0; i < slots; i++) {
    struct task *t = scheduler_task_slot(i);

    if (!t || t->state == TASK_UNUSED)
      continue;
    sched_set_cgroup_stride_pct(t, 0);
    sched_set_cgroup_throttled(t, 0);
  }
}

void cgroup_tick(void) {
  if (!cg_root)
    return;

  u64 now = ktime_monotonic_ns();
  u64 flags;
  static u64 last_sweep_ns;
  int sweep;

  spin_lock_irqsave(&cg_lock, &flags);
  sweep = (last_sweep_ns == 0 ||
           now - last_sweep_ns >= (u64)CG_SWEEP_MS * 1000000ull);
  if (sweep)
    last_sweep_ns = now;

  /* Between sweeps there is only one thing that can change: a cgroup spending
   * its cpu.max. Without a quota anywhere, the tick has nothing to do. */
  if (!sweep && !__atomic_load_n(&cg_cpu_quota, __ATOMIC_RELAXED)) {
    spin_unlock_irqrestore(&cg_lock, flags);
    return;
  }

  /* 1. Roll the cpu.max periods over. */
  for (struct cgroup *c = cg_all; c; c = c->next) {
    if (c->cpu_quota_us == CG_LIM_MAX)
      continue;
    u64 period_ns = c->cpu_period_us * 1000ull;
    if (!period_ns)
      period_ns = CG_CPU_PERIOD_DEF_US * 1000ull;
    if (c->cpu_period_start_ns == 0) {
      c->cpu_period_start_ns = now;
      c->cpu_nr_periods++; /* the first one counts, or nr_throttled outruns it */
      continue;
    }
    if (now - c->cpu_period_start_ns < period_ns)
      continue;
    c->cpu_period_start_ns = now;
    c->cpu_period_used_ns = 0;
    c->cpu_nr_periods++;
    c->cpu_throttled = 0;
  }

  /* 2. Charge the CPU time every task burned since the last tick to its
   *    cgroups. One pass over the task table, the same shape as the alarm
   *    sweep the tick already does. */
  if (__atomic_load_n(&cg_cpu_active, __ATOMIC_RELAXED)) {
    usize slots = scheduler_task_slots();

    for (usize i = 0; i < slots; i++) {
      struct task *t = scheduler_task_slot(i);
      usize idx;

      if (!t || t->state == TASK_UNUSED)
        continue;
      idx = scheduler_task_index(t);
      if (idx >= CG_TASK_SLOTS)
        continue;
      u64 u = task_utime_ns(t), sy = task_stime_ns(t);
      u64 total = u + sy;
      u64 prev = cg_task_cpu_seen[idx];
      u64 prev_u = cg_task_cpu_user_seen[idx];

      cg_task_cpu_seen[idx] = total;
      cg_task_cpu_user_seen[idx] = u;
      if (total <= prev)
        continue; /* a recycled slot, or no time since the last tick */
      if (prev == 0) {
        /* First sight of this slot. Whatever it has burned so far is not time
         * its cgroup spent since the last tick -- charging it would hand a
         * fresh cgroup the whole history of every task moved into it. */
        continue;
      }
      u64 dt = total - prev;
      u64 du = u > prev_u ? u - prev_u : 0;

      if (du > dt)
        du = dt;

      for (struct cgroup *a = cg_of(t->id); a; a = a->parent) {
        a->cpu_usage_ns += dt;
        a->cpu_user_ns += du;
        a->cpu_sys_ns += dt - du;
        if (a->cpu_quota_us == CG_LIM_MAX)
          continue;
        a->cpu_period_used_ns += dt;
        if (!a->cpu_throttled &&
            a->cpu_period_used_ns >= a->cpu_quota_us * 1000ull) {
          a->cpu_throttled = 1;
          a->cpu_nr_throttled++;
        }
        if (a->cpu_throttled)
          a->cpu_throttled_ns += dt;
      }
    }
  }

  /* 3. Publish, for every task, the stride scale and the throttle flag the
   *    scheduler reads. Only on the sweep: these change when a control file is
   *    written or a cgroup's membership changes, not tick by tick. Throttling
   *    is republished every tick, because a quota can be spent mid-period. */
  usize slots = scheduler_task_slots();
  if (sweep) {
    /* Members per cgroup, once, rather than once per task: the weight each
     * task is given divides its cgroup's weight by how many share it, and
     * asking for that count inside the loop made the sweep quadratic in the
     * task table.
     *
     * RUNNABLE members, not live ones. A cgroup's weight is divided among the
     * tasks competing for the CPU right now, which is what CFS group
     * scheduling divides it among; counting every thread instead would have a
     * compositor with fifty mostly-idle threads give its one busy thread a
     * fiftieth of a share, and the desktop lanes would have paid for the
     * controller being enabled at all. */
    for (struct cgroup *c = cg_all; c; c = c->next)
      c->cpu_nr_tasks = 0;
    for (usize i = 0; i < slots; i++) {
      struct task *t = scheduler_task_slot(i);

      if (cg_task_live(t) &&
          (t->state == TASK_READY || t->state == TASK_RUNNING))
        cg_of(t->id)->cpu_nr_tasks++;
    }
  }
  for (usize i = 0; i < slots; i++) {
    struct task *t = scheduler_task_slot(i);

    if (!cg_task_live(t))
      continue;
    struct cgroup *cg = cg_of(t->id);
    int throttled = 0;

    for (struct cgroup *a = cg; a; a = a->parent)
      if (a->cpu_throttled) {
        throttled = 1;
        break;
      }
    sched_set_cgroup_throttled(t, throttled);

    if (!sweep)
      continue;
    u32 pct = 0; /* 0 == "no cgroup weighting", the scheduler's default */
    if (!cg->is_root && cg->parent &&
        (cg->parent->subtree_control & CG_CTRL_CPU)) {
      u64 w = cg_cpu_effective_weight(cg);
      u64 nr = cg->cpu_nr_tasks;

      if (nr < 1)
        nr = 1;
      u64 p = (10000ull * nr) / w;
      if (p < 1)
        p = 1;
      if (p > 1000000ull)
        p = 1000000ull;
      pct = (u32)p;
    }
    sched_set_cgroup_stride_pct(t, pct);
  }
  spin_unlock_irqrestore(&cg_lock, flags);
}

/* ── io: accounting and the rate ceilings ────────────────────────────────── */

/* One second of history is what io.max is expressed against. */
#define CG_IO_WINDOW_NS 1000000000ull

static volatile int cg_io_limited;

/* The slot a cgroup keeps for one device, taken on first use. Caller holds
 * cg_lock. NULL when every slot is spoken for by another device. */
static struct cg_io_dev *cg_io_slot(struct cgroup *cg, u32 devno, int create) {
  for (int i = 0; i < CG_IO_DEVS; i++)
    if (cg->io_dev[i].devno == devno)
      return &cg->io_dev[i];
  if (!create)
    return 0;
  for (int i = 0; i < CG_IO_DEVS; i++)
    if (cg->io_dev[i].devno == 0) {
      struct cg_io_dev *d = &cg->io_dev[i];

      d->devno = devno;
      d->rbps = d->wbps = d->riops = d->wiops = CG_LIM_MAX;
      return d;
    }
  return 0;
}

/* Drop the window once a second has passed. Caller holds cg_lock. */
static void cg_io_window_roll(struct cg_io_dev *d, u64 now) {
  if (d->win_start_ns && now - d->win_start_ns < CG_IO_WINDOW_NS)
    return;
  d->win_start_ns = now;
  d->win_rbytes = 0;
  d->win_wbytes = 0;
  d->win_rios = 0;
  d->win_wios = 0;
}

void cgroup_io_account(u32 devno, u64 bytes, int write) {
  if (!cg_root || !current_task || !devno)
    return;
  u64 now = ktime_monotonic_ns();
  u64 flags;

  spin_lock_irqsave(&cg_lock, &flags);
  for (struct cgroup *a = cg_of(current_task->id); a; a = a->parent) {
    struct cg_io_dev *d = cg_io_slot(a, devno, 1);

    if (!d)
      continue;
    cg_io_window_roll(d, now);
    if (write) {
      d->wbytes += bytes;
      d->wios++;
      d->win_wbytes += bytes;
      d->win_wios++;
    } else {
      d->rbytes += bytes;
      d->rios++;
      d->win_rbytes += bytes;
      d->win_rios++;
    }
  }
  spin_unlock_irqrestore(&cg_lock, flags);
}

/* How long this request has to wait to keep its cgroup inside io.max.
 *
 * A rate limit of R per second over a window that has already carried U means
 * the window may not end before (U + this request)/R seconds after it began;
 * the wait is the remainder of that. The longest wait any one request can be
 * given is the window itself, so a limit can slow a stream down but never
 * wedge it. */
static u64 cg_io_wait_for(u64 used, u64 limit, u64 want, u64 elapsed_ns) {
  if (limit == CG_LIM_MAX || limit == 0)
    return 0;
  u64 need_ns = ((used + want) * CG_IO_WINDOW_NS) / limit;

  if (need_ns <= elapsed_ns)
    return 0;
  u64 wait = need_ns - elapsed_ns;

  return wait > CG_IO_WINDOW_NS ? CG_IO_WINDOW_NS : wait;
}

u64 cgroup_io_delay_ns(u32 devno, u64 bytes, int write) {
  if (!cg_root || !devno || !current_task ||
      !__atomic_load_n(&cg_io_limited, __ATOMIC_RELAXED))
    return 0;
  u64 now = ktime_monotonic_ns();
  u64 worst = 0;
  u64 flags;

  spin_lock_irqsave(&cg_lock, &flags);
  for (struct cgroup *a = cg_of(current_task->id); a; a = a->parent) {
    struct cg_io_dev *d = cg_io_slot(a, devno, 0);

    if (!d)
      continue;
    cg_io_window_roll(d, now);
    u64 elapsed = now - d->win_start_ns;
    u64 w;

    if (write) {
      w = cg_io_wait_for(d->win_wbytes, d->wbps, bytes, elapsed);
      if (w > worst)
        worst = w;
      w = cg_io_wait_for(d->win_wios, d->wiops, 1, elapsed);
    } else {
      w = cg_io_wait_for(d->win_rbytes, d->rbps, bytes, elapsed);
      if (w > worst)
        worst = w;
      w = cg_io_wait_for(d->win_rios, d->riops, 1, elapsed);
    }
    if (w > worst)
      worst = w;
  }
  spin_unlock_irqrestore(&cg_lock, flags);
  return worst;
}

/* Recount the "is anything limited at all?" flags after a control-file write.
 * Caller holds cg_lock. */
static void cg_limits_recount(void) {
  int mem = 0, io = 0, cpu = 0, quota = 0;

  for (struct cgroup *c = cg_all; c; c = c->next) {
    /* A limit is not the only reason to charge. memory.current is measured on
     * demand, but memory.stat's pgfault is a count, and a counter kept only
     * while a limit happens to be set reads zero for every cgroup that asked
     * for accounting alone -- which is most of what systemd asks for. Enabling
     * the controller is the ask. */
    if (c->mem_max != CG_LIM_MAX || c->mem_high != CG_LIM_MAX ||
        (c->subtree_control & CG_CTRL_MEMORY))
      mem++;
    for (int i = 0; i < CG_IO_DEVS; i++) {
      struct cg_io_dev *d = &c->io_dev[i];

      if (d->devno && (d->rbps != CG_LIM_MAX || d->wbps != CG_LIM_MAX ||
                       d->riops != CG_LIM_MAX || d->wiops != CG_LIM_MAX))
        io++;
    }
    if (c->subtree_control & CG_CTRL_CPU)
      cpu++;
    if (c->cpu_quota_us != CG_LIM_MAX) {
      cpu++;
      quota++;
    }
  }
  __atomic_store_n(&cg_mem_limited, mem, __ATOMIC_RELAXED);
  __atomic_store_n(&cg_io_limited, io, __ATOMIC_RELAXED);
  __atomic_store_n(&cg_cpu_active, cpu, __ATOMIC_RELAXED);
  __atomic_store_n(&cg_cpu_quota, quota, __ATOMIC_RELAXED);
}

/* ── cgroup.events: the edge systemd waits on ────────────────────────────── */

/* systemd watches cgroup.events with inotify to learn that a unit's last
 * process has exited. Recompute every cgroup's "populated" and report the ones
 * that changed; without the notification a service that has finished stays
 * "deactivating" until its stop timeout fires. */
static void cg_events_refresh(void) {
  struct vfs_node *changed[32];
  int nchanged = 0;

  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  /* One pass over the task table, not one per cgroup: this runs on every fork
   * and every exit, with interrupts off. */
  for (struct cgroup *c = cg_all; c; c = c->next)
    c->scratch = 0;
  usize slots = scheduler_task_slots();
  for (usize i = 0; i < slots; i++) {
    struct task *t = scheduler_task_slot(i);
    if (!cg_task_live(t))
      continue;
    for (struct cgroup *c = cg_of(t->id); c; c = c->parent)
      c->scratch = 1;
  }
  for (struct cgroup *c = cg_all; c; c = c->next) {
    if (c->is_root)
      continue;
    if (c->scratch != c->populated) {
      c->populated = c->scratch;
      if (c->events_node && nchanged < (int)(sizeof(changed) / sizeof(changed[0])))
        changed[nchanged++] = c->events_node;
    }
  }
  spin_unlock_irqrestore(&cg_lock, flags);

  for (int i = 0; i < nchanged; i++)
    vfs_inotify_notify(changed[i], IN_MODIFY, 0);
}

/* ── control files ───────────────────────────────────────────────────────── */

enum cg_file {
  CGF_PROCS = 1,
  CGF_THREADS,
  CGF_CONTROLLERS,
  CGF_SUBTREE_CONTROL,
  CGF_EVENTS,
  CGF_TYPE,
  CGF_STAT,
  CGF_MAX_DEPTH,
  CGF_MAX_DESCENDANTS,
  CGF_PIDS_CURRENT,
  CGF_PIDS_MAX,
  CGF_PIDS_EVENTS,
  CGF_PIDS_PEAK,
  CGF_MEM_CURRENT,
  CGF_MEM_PEAK,
  CGF_MEM_MIN,
  CGF_MEM_LOW,
  CGF_MEM_HIGH,
  CGF_MEM_MAX,
  CGF_MEM_EVENTS,
  CGF_MEM_EVENTS_LOCAL,
  CGF_MEM_STAT,
  CGF_MEM_SWAP_CURRENT,
  CGF_MEM_SWAP_MAX,
  CGF_MEM_SWAP_EVENTS,
  CGF_MEM_OOM_GROUP,
  CGF_CPU_STAT,
  CGF_CPU_WEIGHT,
  CGF_CPU_WEIGHT_NICE,
  CGF_CPU_MAX,
  CGF_IO_STAT,
  CGF_IO_MAX,
  CGF_IO_WEIGHT,
};

struct cg_filenode {
  struct cgroup *cg;
  enum cg_file kind;
};

/* Copy the slice of `src` that a read at `offset` asked for. */
static isize cg_emit(const char *src, usize len, u64 offset, char *out,
                     usize size) {
  if (offset >= len)
    return 0;
  usize avail = len - (usize)offset;
  usize n = size < avail ? size : avail;
  memcpy(out, src + offset, n);
  return (isize)n;
}

static usize cg_append(char *buf, usize cap, usize len, const char *s) {
  while (*s && len + 1 < cap)
    buf[len++] = *s++;
  if (len < cap)
    buf[len] = '\0';
  return len;
}

static usize cg_append_u64(char *buf, usize cap, usize len, u64 v) {
  char tmp[24];
  int n = 0;
  if (v == 0)
    tmp[n++] = '0';
  while (v) {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while (n > 0 && len + 1 < cap)
    buf[len++] = tmp[--n];
  if (len < cap)
    buf[len] = '\0';
  return len;
}

/* The v2 path of a cgroup as seen from `root` — "/" for the root itself, and,
 * as Linux prints it, a path through ".." for a cgroup outside that subtree. */
static int cg_path(struct cgroup *cg, struct cgroup *root, char *buf,
                   usize len) {
  const char *parts[64];
  int n = 0;
  /* Climb from `root` to the nearest common ancestor, one ".." per step. */
  struct cgroup *common = root;
  while (common && !common->is_root && !cg_is_ancestor(common, cg) && n < 32) {
    parts[n++] = "..";
    common = common->parent;
  }
  const char *down[32];
  int nd = 0;
  for (struct cgroup *c = cg; c && c != common && !c->is_root && nd < 32;
       c = c->parent)
    down[nd++] = c->dir ? c->dir->name : "?";
  for (int i = nd - 1; i >= 0 && n < 64; i--)
    parts[n++] = down[i];
  if (n == 0) {
    if (len < 2)
      return -ENAMETOOLONG;
    buf[0] = '/';
    buf[1] = '\0';
    return 1;
  }
  usize pos = 0;
  for (int i = 0; i < n; i++) {
    usize pl = strlen(parts[i]);
    if (pos + 1 + pl + 1 > len)
      return -ENAMETOOLONG;
    buf[pos++] = '/';
    memcpy(buf + pos, parts[i], pl);
    pos += pl;
  }
  buf[pos] = '\0';
  return (int)pos;
}

/* Render one control file into `buf`; returns the length written. */
static usize cg_render(struct cgroup *cg, enum cg_file kind, char *buf,
                       usize cap) {
  usize len = 0;
  u64 flags;

  switch (kind) {
  case CGF_PROCS:
  case CGF_THREADS: {
    spin_lock_irqsave(&cg_lock, &flags);
    usize slots = scheduler_task_slots();
    for (usize i = 0; i < slots; i++) {
      struct task *t = scheduler_task_slot(i);
      if (!cg_task_live(t))
        continue;
      if (cg_of(t->id) != cg)
        continue;
      /* cgroup.procs lists processes, cgroup.threads lists every thread. */
      if (kind == CGF_PROCS && task_tgid(t) != t->id)
        continue;
      /* In the reader's PID namespace; a task it cannot name is not listed. */
      usize vpid = namespace_pid_to_user(t->id);
      if (!vpid)
        continue;
      len = cg_append_u64(buf, cap, len, (u64)vpid);
      len = cg_append(buf, cap, len, "\n");
    }
    spin_unlock_irqrestore(&cg_lock, flags);
    break;
  }
  case CGF_CONTROLLERS: {
    /* A cgroup's available controllers are the ones its parent enabled for its
     * children; the root's are everything the kernel implements. */
    spin_lock_irqsave(&cg_lock, &flags);
    u32 avail = cg->is_root ? CG_CTRL_ALL
                            : (cg->parent ? cg->parent->subtree_control : 0);
    spin_unlock_irqrestore(&cg_lock, flags);
    int first = 1;
    for (usize i = 0; i < sizeof(cg_controllers) / sizeof(cg_controllers[0]); i++)
      if (avail & cg_controllers[i].bit) {
        if (!first)
          len = cg_append(buf, cap, len, " ");
        len = cg_append(buf, cap, len, cg_controllers[i].name);
        first = 0;
      }
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_SUBTREE_CONTROL: {
    spin_lock_irqsave(&cg_lock, &flags);
    u32 en = cg->subtree_control;
    spin_unlock_irqrestore(&cg_lock, flags);
    int first = 1;
    for (usize i = 0; i < sizeof(cg_controllers) / sizeof(cg_controllers[0]); i++)
      if (en & cg_controllers[i].bit) {
        if (!first)
          len = cg_append(buf, cap, len, " ");
        len = cg_append(buf, cap, len, cg_controllers[i].name);
        first = 0;
      }
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_EVENTS: {
    spin_lock_irqsave(&cg_lock, &flags);
    int pop = cg_count_tasks(cg, 1) > 0;
    cg->populated = pop;
    spin_unlock_irqrestore(&cg_lock, flags);
    len = cg_append(buf, cap, len, "populated ");
    len = cg_append_u64(buf, cap, len, (u64)pop);
    len = cg_append(buf, cap, len, "\nfrozen 0\n");
    break;
  }
  case CGF_TYPE:
    len = cg_append(buf, cap, len, "domain\n");
    break;
  case CGF_STAT: {
    spin_lock_irqsave(&cg_lock, &flags);
    usize nd = cg_count_descendants(cg);
    spin_unlock_irqrestore(&cg_lock, flags);
    len = cg_append(buf, cap, len, "nr_descendants ");
    len = cg_append_u64(buf, cap, len, (u64)nd);
    len = cg_append(buf, cap, len, "\nnr_dying_descendants 0\n");
    break;
  }
  case CGF_MAX_DEPTH:
  case CGF_MAX_DESCENDANTS: {
    spin_lock_irqsave(&cg_lock, &flags);
    u32 v = (kind == CGF_MAX_DEPTH) ? cg->max_depth : cg->max_descendants;
    spin_unlock_irqrestore(&cg_lock, flags);
    if (v == CG_PIDS_MAX_UNSET)
      len = cg_append(buf, cap, len, "max\n");
    else {
      len = cg_append_u64(buf, cap, len, (u64)v);
      len = cg_append(buf, cap, len, "\n");
    }
    break;
  }
  case CGF_PIDS_CURRENT:
  case CGF_PIDS_PEAK: {
    spin_lock_irqsave(&cg_lock, &flags);
    usize n = cg_count_tasks(cg, 1);
    spin_unlock_irqrestore(&cg_lock, flags);
    len = cg_append_u64(buf, cap, len, (u64)n);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_PIDS_MAX: {
    spin_lock_irqsave(&cg_lock, &flags);
    u32 v = cg->pids_max;
    spin_unlock_irqrestore(&cg_lock, flags);
    if (v == CG_PIDS_MAX_UNSET)
      len = cg_append(buf, cap, len, "max\n");
    else {
      len = cg_append_u64(buf, cap, len, (u64)v);
      len = cg_append(buf, cap, len, "\n");
    }
    break;
  }
  case CGF_PIDS_EVENTS: {
    spin_lock_irqsave(&cg_lock, &flags);
    u64 d = cg->pids_denied;
    spin_unlock_irqrestore(&cg_lock, flags);
    len = cg_append(buf, cap, len, "max ");
    len = cg_append_u64(buf, cap, len, d);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  /* ── memory ── */
  case CGF_MEM_CURRENT:
  case CGF_MEM_PEAK: {
    /* Measured on the spot. The running estimate the fault path keeps is a
     * trigger, never an answer: a reader asking what a cgroup uses gets the
     * same page-table walk /proc/<pid>/status answers VmRSS with. */
    u64 pages = cg_mem_refresh(cg);

    if (kind == CGF_MEM_PEAK) {
      spin_lock_irqsave(&cg_lock, &flags);
      pages = cg->mem_peak;
      spin_unlock_irqrestore(&cg_lock, flags);
    }
    len = cg_append_u64(buf, cap, len, pages * PAGE_SIZE);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_MEM_MIN:
  case CGF_MEM_LOW:
  case CGF_MEM_HIGH:
  case CGF_MEM_MAX:
  case CGF_MEM_SWAP_MAX: {
    spin_lock_irqsave(&cg_lock, &flags);
    u64 v = kind == CGF_MEM_MIN    ? cg->mem_min
            : kind == CGF_MEM_LOW  ? cg->mem_low
            : kind == CGF_MEM_HIGH ? cg->mem_high
            : kind == CGF_MEM_MAX  ? cg->mem_max
                                   : cg->mem_swap_max;
    spin_unlock_irqrestore(&cg_lock, flags);
    if (v == CG_LIM_MAX)
      len = cg_append(buf, cap, len, "max\n");
    else {
      len = cg_append_u64(buf, cap, len, v * PAGE_SIZE);
      len = cg_append(buf, cap, len, "\n");
    }
    break;
  }
  case CGF_MEM_SWAP_CURRENT: {
    /* The subtree's pages that are out in swap. At the root the question is
     * the machine's, and the swap layer already counts it exactly -- reporting
     * the sum of the children there would miss every page belonging to a task
     * in no cgroup at all. */
    u64 pages;

    if (cg->is_root) {
      u64 total = 0, used = 0;
      pages = swap_stats(&total, &used) == 0 ? used : 0;
    } else {
      spin_lock_irqsave(&cg_lock, &flags);
      pages = cg->mem_swap_cur;
      spin_unlock_irqrestore(&cg_lock, flags);
    }
    len = cg_append_u64(buf, cap, len, pages * PAGE_SIZE);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_MEM_SWAP_EVENTS: {
    spin_lock_irqsave(&cg_lock, &flags);
    u64 mx = cg->mem_swap_ev_max, fl = cg->mem_swap_ev_fail;
    spin_unlock_irqrestore(&cg_lock, flags);
    /* Linux prints high, max and fail. There is no memory.swap.high here, so
     * printing a "high" counter that nothing can ever increment would be
     * describing a limit this kernel does not have. */
    len = cg_append(buf, cap, len, "max ");
    len = cg_append_u64(buf, cap, len, mx);
    len = cg_append(buf, cap, len, "\nfail ");
    len = cg_append_u64(buf, cap, len, fl);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_MEM_OOM_GROUP: {
    spin_lock_irqsave(&cg_lock, &flags);
    int g = cg->mem_oom_group;
    spin_unlock_irqrestore(&cg_lock, flags);
    len = cg_append_u64(buf, cap, len, (u64)g);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_MEM_EVENTS:
  case CGF_MEM_EVENTS_LOCAL: {
    spin_lock_irqsave(&cg_lock, &flags);
    u64 lo = cg->mem_ev_low, hi = cg->mem_ev_high, mx = cg->mem_ev_max;
    u64 oom = cg->mem_ev_oom, ok = cg->mem_ev_oom_kill;
    spin_unlock_irqrestore(&cg_lock, flags);
    len = cg_append(buf, cap, len, "low ");
    len = cg_append_u64(buf, cap, len, lo);
    len = cg_append(buf, cap, len, "\nhigh ");
    len = cg_append_u64(buf, cap, len, hi);
    len = cg_append(buf, cap, len, "\nmax ");
    len = cg_append_u64(buf, cap, len, mx);
    len = cg_append(buf, cap, len, "\noom ");
    len = cg_append_u64(buf, cap, len, oom);
    len = cg_append(buf, cap, len, "\noom_kill ");
    len = cg_append_u64(buf, cap, len, ok);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_MEM_STAT: {
    /* Only what is really counted. Linux lists thirty-odd keys here, most of
     * them breakdowns this kernel does not keep; printing them as zero would
     * say "no file pages" about a cgroup full of them. A reader parses keys,
     * so the honest set is a short one. */
    spin_lock_irqsave(&cg_lock, &flags);
    u64 pf = cg->mem_pgfault, mf = cg->mem_pgmajfault;
    u64 sc = cg->mem_pgscan, st = cg->mem_pgsteal, sw = cg->mem_swap_cur;
    spin_unlock_irqrestore(&cg_lock, flags);
    len = cg_append(buf, cap, len, "swapcached 0\nswap ");
    len = cg_append_u64(buf, cap, len, sw * PAGE_SIZE);
    len = cg_append(buf, cap, len, "\npgfault ");
    len = cg_append_u64(buf, cap, len, pf);
    len = cg_append(buf, cap, len, "\npgmajfault ");
    len = cg_append_u64(buf, cap, len, mf);
    /* What the cgroup's own reclaim did, and only that: these count the pages
     * the scan above looked at and the ones it wrote out, not the machine-wide
     * eviction that runs on everyone's behalf. */
    len = cg_append(buf, cap, len, "\npgscan ");
    len = cg_append_u64(buf, cap, len, sc);
    len = cg_append(buf, cap, len, "\npgsteal ");
    len = cg_append_u64(buf, cap, len, st);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  /* ── cpu ── */
  case CGF_CPU_STAT: {
    spin_lock_irqsave(&cg_lock, &flags);
    u64 us = cg->cpu_usage_ns / 1000, uu = cg->cpu_user_ns / 1000;
    u64 sy = cg->cpu_sys_ns / 1000, np = cg->cpu_nr_periods;
    u64 nt = cg->cpu_nr_throttled, tu = cg->cpu_throttled_ns / 1000;
    spin_unlock_irqrestore(&cg_lock, flags);
    len = cg_append(buf, cap, len, "usage_usec ");
    len = cg_append_u64(buf, cap, len, us);
    len = cg_append(buf, cap, len, "\nuser_usec ");
    len = cg_append_u64(buf, cap, len, uu);
    len = cg_append(buf, cap, len, "\nsystem_usec ");
    len = cg_append_u64(buf, cap, len, sy);
    len = cg_append(buf, cap, len, "\nnr_periods ");
    len = cg_append_u64(buf, cap, len, np);
    len = cg_append(buf, cap, len, "\nnr_throttled ");
    len = cg_append_u64(buf, cap, len, nt);
    len = cg_append(buf, cap, len, "\nthrottled_usec ");
    len = cg_append_u64(buf, cap, len, tu);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_CPU_WEIGHT: {
    spin_lock_irqsave(&cg_lock, &flags);
    u32 w = cg->cpu_weight;
    spin_unlock_irqrestore(&cg_lock, flags);
    len = cg_append_u64(buf, cap, len, (u64)w);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_CPU_WEIGHT_NICE: {
    spin_lock_irqsave(&cg_lock, &flags);
    u32 w = cg->cpu_weight;
    spin_unlock_irqrestore(&cg_lock, flags);
    int nice = cg_weight_to_nice(w);
    if (nice < 0) {
      len = cg_append(buf, cap, len, "-");
      nice = -nice;
    }
    len = cg_append_u64(buf, cap, len, (u64)nice);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_CPU_MAX: {
    spin_lock_irqsave(&cg_lock, &flags);
    u64 q = cg->cpu_quota_us, per = cg->cpu_period_us;
    spin_unlock_irqrestore(&cg_lock, flags);
    if (q == CG_LIM_MAX)
      len = cg_append(buf, cap, len, "max");
    else
      len = cg_append_u64(buf, cap, len, q);
    len = cg_append(buf, cap, len, " ");
    len = cg_append_u64(buf, cap, len, per);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  /* ── io ── */
  case CGF_IO_WEIGHT: {
    spin_lock_irqsave(&cg_lock, &flags);
    u32 w = cg->io_weight;
    spin_unlock_irqrestore(&cg_lock, flags);
    len = cg_append(buf, cap, len, "default ");
    len = cg_append_u64(buf, cap, len, (u64)w);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_IO_STAT:
  case CGF_IO_MAX: {
    struct cg_io_dev snap[CG_IO_DEVS];

    spin_lock_irqsave(&cg_lock, &flags);
    memcpy(snap, cg->io_dev, sizeof(snap));
    spin_unlock_irqrestore(&cg_lock, flags);
    for (int i = 0; i < CG_IO_DEVS; i++) {
      struct cg_io_dev *d = &snap[i];

      if (!d->devno)
        continue;
      if (kind == CGF_IO_MAX && d->rbps == CG_LIM_MAX &&
          d->wbps == CG_LIM_MAX && d->riops == CG_LIM_MAX &&
          d->wiops == CG_LIM_MAX)
        continue;
      len = cg_append_u64(buf, cap, len, (u64)(d->devno >> 8));
      len = cg_append(buf, cap, len, ":");
      len = cg_append_u64(buf, cap, len, (u64)(d->devno & 0xff));
      if (kind == CGF_IO_STAT) {
        len = cg_append(buf, cap, len, " rbytes=");
        len = cg_append_u64(buf, cap, len, d->rbytes);
        len = cg_append(buf, cap, len, " wbytes=");
        len = cg_append_u64(buf, cap, len, d->wbytes);
        len = cg_append(buf, cap, len, " rios=");
        len = cg_append_u64(buf, cap, len, d->rios);
        len = cg_append(buf, cap, len, " wios=");
        len = cg_append_u64(buf, cap, len, d->wios);
      } else {
        static const char *const keys[4] = {" rbps=", " wbps=", " riops=",
                                            " wiops="};
        u64 vals[4] = {d->rbps, d->wbps, d->riops, d->wiops};

        for (int k = 0; k < 4; k++) {
          len = cg_append(buf, cap, len, keys[k]);
          if (vals[k] == CG_LIM_MAX)
            len = cg_append(buf, cap, len, "max");
          else
            len = cg_append_u64(buf, cap, len, vals[k]);
        }
      }
      len = cg_append(buf, cap, len, "\n");
    }
    break;
  }
  default:
    break;
  }
  return len;
}

static isize cg_read_cb(struct vfs_node *node, u64 offset, char *buffer,
                        usize size, int flags) {
  (void)flags;
  struct cg_filenode *fn = node->inode ? node->inode->data : 0;
  if (!fn || !fn->cg)
    return -EIO;

  /* cgroup.procs of the root can name every task in the machine, so the buffer
   * is sized for the task table rather than guessed. */
  usize cap = 4096 + scheduler_task_slots() * 12;
  char *buf = kmalloc(cap);
  if (!buf)
    return -ENOMEM;
  buf[0] = '\0';
  usize len = cg_render(fn->cg, fn->kind, buf, cap);
  isize r = cg_emit(buf, len, offset, buffer, size);
  kfree(buf);
  return r;
}

/* ── writes ──────────────────────────────────────────────────────────────── */

static u64 cg_parse_u64(const char *s, usize len, int *ok) {
  u64 v = 0;
  usize i = 0;
  int digits = 0;
  while (i < len && (s[i] == ' ' || s[i] == '\t'))
    i++;
  while (i < len && s[i] >= '0' && s[i] <= '9') {
    v = v * 10 + (u64)(s[i] - '0');
    i++;
    digits++;
  }
  *ok = digits > 0;
  return v;
}

/* A cgroup v2 limit file: a decimal number, or the literal "max". Returns 0 and
 * fills *out, or -EINVAL. */
static int cg_parse_limit(const char *s, usize len, u64 *out) {
  usize i = 0;

  while (i < len && (s[i] == ' ' || s[i] == '\t'))
    i++;
  if (len - i >= 3 && strncmp(s + i, "max", 3) == 0) {
    *out = CG_LIM_MAX;
    return 0;
  }
  int ok = 0;
  u64 v = cg_parse_u64(s + i, len - i, &ok);

  if (!ok)
    return -EINVAL;
  *out = v;
  return 0;
}

/* Move every thread of `pid`'s thread group into `cg`. */
static int cg_attach_process(struct cgroup *cg, usize pid, int threads_only) {
  struct task *t = scheduler_task_by_pid(pid);
  if (!cg_task_live(t))
    return -ESRCH;
  usize tgid = threads_only ? 0 : task_tgid(t);

  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  if (threads_only) {
    cg_member_set(pid, cg->is_root ? 0 : cg);
  } else {
    usize slots = scheduler_task_slots();
    for (usize i = 0; i < slots; i++) {
      struct task *o = scheduler_task_slot(i);
      if (cg_task_live(o) && task_tgid(o) == tgid)
        cg_member_set(o->id, cg->is_root ? 0 : cg);
    }
  }
  spin_unlock_irqrestore(&cg_lock, flags);
  cg_events_refresh();
  return 0;
}

static isize cg_write_cb(struct vfs_node *node, u64 offset, const char *buffer,
                         usize size, int flags) {
  (void)offset;
  (void)flags;
  struct cg_filenode *fn = node->inode ? node->inode->data : 0;
  if (!fn || !fn->cg)
    return -EIO;
  if (size == 0)
    return 0;
  struct cgroup *cg = fn->cg;
  u64 lock_flags;

  switch (fn->kind) {
  case CGF_PROCS:
  case CGF_THREADS: {
    int ok = 0;
    u64 upid = cg_parse_u64(buffer, size, &ok);
    if (!ok)
      return -EINVAL;
    /* 0 is the writer itself, as on Linux. */
    usize pid = upid ? namespace_pid_from_user((usize)upid)
                     : (fn->kind == CGF_THREADS ? scheduler_current_task_id()
                                                : scheduler_get_pid());
    if (!pid)
      return -ESRCH;
    /* A task inside a cgroup namespace may only move tasks between cgroups
     * of its own subtree: the source and the destination both have to be
     * below its namespace root. */
    spin_lock_irqsave(&cg_lock, &lock_flags);
    struct cgroup *nsroot = cg_ns_root_locked();
    int inside = cg_is_ancestor(nsroot, cg) &&
                 cg_is_ancestor(nsroot, cg_of(pid));
    spin_unlock_irqrestore(&cg_lock, lock_flags);
    if (!inside)
      return -ENOENT;
    int r = cg_attach_process(cg, pid, fn->kind == CGF_THREADS);
    return r < 0 ? r : (isize)size;
  }
  case CGF_SUBTREE_CONTROL: {
    /* "+pids -memory": a list of signed controller names. An unknown or
     * unavailable controller is ENOENT, as on Linux. */
    usize i = 0;
    u32 add = 0, del = 0;
    while (i < size) {
      while (i < size && (buffer[i] == ' ' || buffer[i] == '\n' ||
                          buffer[i] == '\t'))
        i++;
      if (i >= size)
        break;
      char sign = buffer[i];
      if (sign != '+' && sign != '-')
        return -EINVAL;
      i++;
      usize start = i;
      while (i < size && buffer[i] != ' ' && buffer[i] != '\n' &&
             buffer[i] != '\t')
        i++;
      usize nlen = i - start;
      u32 bit = 0;
      for (usize c = 0; c < sizeof(cg_controllers) / sizeof(cg_controllers[0]);
           c++)
        if (strlen(cg_controllers[c].name) == nlen &&
            strncmp(buffer + start, cg_controllers[c].name, nlen) == 0)
          bit = cg_controllers[c].bit;
      if (!bit)
        return -ENOENT;
      /* Only a controller this cgroup itself has available may be delegated. */
      spin_lock_irqsave(&cg_lock, &lock_flags);
      u32 avail = cg->is_root ? CG_CTRL_ALL
                              : (cg->parent ? cg->parent->subtree_control : 0);
      spin_unlock_irqrestore(&cg_lock, lock_flags);
      if (!(avail & bit))
        return -ENOENT;
      if (sign == '+')
        add |= bit;
      else
        del |= bit;
    }
    spin_lock_irqsave(&cg_lock, &lock_flags);
    cg->subtree_control = (cg->subtree_control | add) & ~del;
    cg_limits_recount();
    spin_unlock_irqrestore(&cg_lock, lock_flags);
    /* The children's interface files exist exactly while their parent
     * delegates the controller, so enabling one has to create them in every
     * child that already exists -- systemd writes cgroup.subtree_control after
     * it has made the slice's children, not before. */
    cg_sync_children_controller_files(cg);
    return (isize)size;
  }
  case CGF_PIDS_MAX:
  case CGF_MAX_DEPTH:
  case CGF_MAX_DESCENDANTS: {
    u32 v;
    usize i = 0;
    while (i < size && (buffer[i] == ' ' || buffer[i] == '\t'))
      i++;
    if (size - i >= 3 && strncmp(buffer + i, "max", 3) == 0) {
      v = CG_PIDS_MAX_UNSET;
    } else {
      int ok = 0;
      u64 n = cg_parse_u64(buffer + i, size - i, &ok);
      if (!ok)
        return -EINVAL;
      v = (n > 0xFFFFFFFEull) ? CG_PIDS_MAX_UNSET : (u32)n;
    }
    spin_lock_irqsave(&cg_lock, &lock_flags);
    if (fn->kind == CGF_PIDS_MAX)
      cg->pids_max = v;
    else if (fn->kind == CGF_MAX_DEPTH)
      cg->max_depth = v;
    else
      cg->max_descendants = v;
    spin_unlock_irqrestore(&cg_lock, lock_flags);
    return (isize)size;
  }
  /* ── memory: the byte limits ── */
  case CGF_MEM_MIN:
  case CGF_MEM_LOW:
  case CGF_MEM_HIGH:
  case CGF_MEM_MAX:
  case CGF_MEM_SWAP_MAX: {
    u64 v;
    int r = cg_parse_limit(buffer, size, &v);

    if (r < 0)
      return r;
    /* Written in bytes, kept in pages: a limit that is not a whole number of
     * pages rounds down, so the cgroup can never exceed what was asked for. */
    if (v != CG_LIM_MAX)
      v /= PAGE_SIZE;
    spin_lock_irqsave(&cg_lock, &lock_flags);
    if (fn->kind == CGF_MEM_MIN)
      cg->mem_min = v;
    else if (fn->kind == CGF_MEM_LOW)
      cg->mem_low = v;
    else if (fn->kind == CGF_MEM_HIGH)
      cg->mem_high = v;
    else if (fn->kind == CGF_MEM_MAX)
      cg->mem_max = v;
    else
      cg->mem_swap_max = v;
    /* A fresh limit must be judged against a fresh measurement, not against
     * whatever estimate the old one left behind. */
    cg->mem_delta = 0;
    cg->mem_exact = 0;
    cg_limits_recount();
    spin_unlock_irqrestore(&cg_lock, lock_flags);
    /* Setting a limit below what the cgroup already uses is the one moment a
     * limit can be exceeded without a single new page being faulted. Linux
     * reclaims and, failing that, kills, right here. */
    if (fn->kind == CGF_MEM_MAX && v != CG_LIM_MAX &&
        cg_mem_refresh(cg) > v)
      cg_mem_over_limit(cg, v);
    return (isize)size;
  }
  case CGF_MEM_OOM_GROUP: {
    int ok = 0;
    u64 v = cg_parse_u64(buffer, size, &ok);

    if (!ok || v > 1)
      return -EINVAL;
    spin_lock_irqsave(&cg_lock, &lock_flags);
    cg->mem_oom_group = (int)v;
    spin_unlock_irqrestore(&cg_lock, lock_flags);
    return (isize)size;
  }
  /* ── cpu ── */
  case CGF_CPU_WEIGHT: {
    int ok = 0;
    u64 v = cg_parse_u64(buffer, size, &ok);

    if (!ok || v < CG_CPU_WEIGHT_MIN || v > CG_CPU_WEIGHT_MAX)
      return -ERANGE;
    spin_lock_irqsave(&cg_lock, &lock_flags);
    cg->cpu_weight = (u32)v;
    spin_unlock_irqrestore(&cg_lock, lock_flags);
    return (isize)size;
  }
  case CGF_CPU_WEIGHT_NICE: {
    int nice = 0;
    usize i = 0;
    int neg = 0, digits = 0;

    while (i < size && (buffer[i] == ' ' || buffer[i] == '\t'))
      i++;
    if (i < size && (buffer[i] == '-' || buffer[i] == '+'))
      neg = buffer[i++] == '-';
    while (i < size && buffer[i] >= '0' && buffer[i] <= '9') {
      nice = nice * 10 + (buffer[i++] - '0');
      digits++;
      if (nice > 1000)
        break;
    }
    if (!digits)
      return -EINVAL;
    if (neg)
      nice = -nice;
    if (nice < -20 || nice > 19)
      return -ERANGE;
    spin_lock_irqsave(&cg_lock, &lock_flags);
    cg->cpu_weight = cg_nice_to_weight(nice);
    spin_unlock_irqrestore(&cg_lock, lock_flags);
    return (isize)size;
  }
  case CGF_CPU_MAX: {
    /* "QUOTA [PERIOD]", QUOTA either a number of microseconds or "max". */
    usize i = 0;
    u64 quota, period = CG_CPU_PERIOD_DEF_US;

    while (i < size && (buffer[i] == ' ' || buffer[i] == '\t'))
      i++;
    if (size - i >= 3 && strncmp(buffer + i, "max", 3) == 0) {
      quota = CG_LIM_MAX;
      i += 3;
    } else {
      int ok = 0;
      quota = cg_parse_u64(buffer + i, size - i, &ok);
      if (!ok)
        return -EINVAL;
      while (i < size && buffer[i] >= '0' && buffer[i] <= '9')
        i++;
    }
    while (i < size && (buffer[i] == ' ' || buffer[i] == '\t'))
      i++;
    if (i < size && buffer[i] >= '0' && buffer[i] <= '9') {
      int ok = 0;
      u64 p = cg_parse_u64(buffer + i, size - i, &ok);
      if (!ok || p == 0)
        return -EINVAL;
      period = p;
    }
    spin_lock_irqsave(&cg_lock, &lock_flags);
    cg->cpu_quota_us = quota;
    cg->cpu_period_us = period;
    cg->cpu_period_used_ns = 0;
    cg->cpu_period_start_ns = 0;
    cg->cpu_throttled = 0;
    cg_limits_recount();
    spin_unlock_irqrestore(&cg_lock, lock_flags);
    return (isize)size;
  }
  /* ── io ── */
  case CGF_IO_WEIGHT: {
    usize i = 0;
    int ok = 0;

    /* "default 100" or "100", as Linux accepts both. */
    while (i < size && (buffer[i] == ' ' || buffer[i] == '\t'))
      i++;
    if (size - i >= 7 && strncmp(buffer + i, "default", 7) == 0)
      i += 7;
    u64 v = cg_parse_u64(buffer + i, size - i, &ok);
    if (!ok || v < 1 || v > 10000)
      return -ERANGE;
    spin_lock_irqsave(&cg_lock, &lock_flags);
    cg->io_weight = (u32)v;
    spin_unlock_irqrestore(&cg_lock, lock_flags);
    return (isize)size;
  }
  case CGF_IO_MAX: {
    /* "MAJ:MIN rbps=... wbps=... riops=... wiops=...", any subset, each value
     * a number or "max". */
    usize i = 0;
    int ok = 0;

    while (i < size && (buffer[i] == ' ' || buffer[i] == '\t'))
      i++;
    u64 major = cg_parse_u64(buffer + i, size - i, &ok);
    if (!ok)
      return -EINVAL;
    while (i < size && buffer[i] >= '0' && buffer[i] <= '9')
      i++;
    if (i >= size || buffer[i] != ':')
      return -EINVAL;
    i++;
    u64 minor = cg_parse_u64(buffer + i, size - i, &ok);
    if (!ok)
      return -EINVAL;
    while (i < size && buffer[i] >= '0' && buffer[i] <= '9')
      i++;
    u32 devno = (u32)((major << 8) | (minor & 0xff));
    if (!devno)
      return -EINVAL;

    u64 vals[4] = {CG_LIM_MAX, CG_LIM_MAX, CG_LIM_MAX, CG_LIM_MAX};
    int seen[4] = {0, 0, 0, 0};
    static const char *const keys[4] = {"rbps", "wbps", "riops", "wiops"};

    while (i < size) {
      while (i < size && (buffer[i] == ' ' || buffer[i] == '\t' ||
                          buffer[i] == '\n'))
        i++;
      if (i >= size)
        break;
      usize start = i;
      while (i < size && buffer[i] != '=' && buffer[i] != ' ' &&
             buffer[i] != '\n')
        i++;
      if (i >= size || buffer[i] != '=')
        return -EINVAL;
      usize klen = i - start;
      i++;
      int which = -1;
      for (int k = 0; k < 4; k++)
        if (strlen(keys[k]) == klen && strncmp(buffer + start, keys[k], klen) == 0)
          which = k;
      if (which < 0)
        return -EINVAL;
      if (size - i >= 3 && strncmp(buffer + i, "max", 3) == 0) {
        vals[which] = CG_LIM_MAX;
        i += 3;
      } else {
        int vok = 0;
        vals[which] = cg_parse_u64(buffer + i, size - i, &vok);
        if (!vok)
          return -EINVAL;
        while (i < size && buffer[i] >= '0' && buffer[i] <= '9')
          i++;
      }
      seen[which] = 1;
    }

    spin_lock_irqsave(&cg_lock, &lock_flags);
    struct cg_io_dev *d = cg_io_slot(cg, devno, 1);
    if (!d) {
      spin_unlock_irqrestore(&cg_lock, lock_flags);
      return -ENOSPC;
    }
    if (seen[0])
      d->rbps = vals[0];
    if (seen[1])
      d->wbps = vals[1];
    if (seen[2])
      d->riops = vals[2];
    if (seen[3])
      d->wiops = vals[3];
    cg_limits_recount();
    spin_unlock_irqrestore(&cg_lock, lock_flags);
    return (isize)size;
  }
  default:
    return -EACCES;
  }
}

/* ── node construction ───────────────────────────────────────────────────── */

static int cg_statfs(struct vfs_node *node, struct b1nix_statfs *st) {
  (void)node;
  if (!st)
    return -EINVAL;
  memset(st, 0, sizeof(*st));
  st->f_type = 0x63677270; /* CGROUP2_SUPER_MAGIC — what systemd looks for */
  st->f_bsize = 4096;
  st->f_namelen = 255;
  return 0;
}

static struct vfs_node *cg_find_child(struct vfs_node *dir, const char *name) {
  for (struct vfs_node *c = dir->first_child; c; c = c->next_sibling)
    if (!c->deleted && strcmp(c->name, name) == 0)
      return c;
  return 0;
}

static struct vfs_node *cg_mkfile(struct cgroup *cg, const char *name,
                                  enum cg_file kind, int writable) {
  if (cg_find_child(cg->dir, name))
    return 0;
  struct vfs_node *n = vfs_create_node(VFS_DEVICE);
  if (!n)
    return 0;
  usize nl = strlen(name);
  if (nl > VFS_NAME_MAX - 1)
    nl = VFS_NAME_MAX - 1;
  memcpy(n->name, name, nl);
  n->name[nl] = '\0';
  n->inode->mode = writable ? 0644 : 0444;
  n->inode->nlink = 1;
  n->inode->uid = 0;
  n->inode->gid = 0;
  n->inode->flags |= VFS_NODE_PSEUDO_REG;
  struct cg_filenode *fn = kzalloc(sizeof(*fn));
  if (!fn) {
    n->deleted = 1;
    vfs_node_put(n);
    return 0;
  }
  fn->cg = cg;
  fn->kind = kind;
  n->inode->data = fn;
  n->inode->read_cb = cg_read_cb;
  if (writable)
    n->inode->write_cb = cg_write_cb;
  n->parent = cg->dir;
  n->refcount++;
  vfs_attach_child(cg->dir, n);
  return n;
}

static void cg_rmfile(struct cgroup *cg, const char *name) {
  struct vfs_node *n = cg_find_child(cg->dir, name);
  if (!n)
    return;
  vfs_detach_child(cg->dir, n);
  if (n->inode && n->inode->data) {
    kfree(n->inode->data);
    n->inode->data = 0;
  }
  n->deleted = 1;
  vfs_node_put(n);
}

/* Create or remove the per-controller interface files, which exist in a cgroup
 * exactly when its parent has that controller in cgroup.subtree_control. */
static void cg_sync_controller_files(struct cgroup *cg) {
  if (cg->is_root)
    return;
  u32 avail = cg->parent ? cg->parent->subtree_control : 0;
  if (avail & CG_CTRL_PIDS) {
    cg_mkfile(cg, "pids.current", CGF_PIDS_CURRENT, 0);
    cg_mkfile(cg, "pids.peak", CGF_PIDS_PEAK, 0);
    cg_mkfile(cg, "pids.max", CGF_PIDS_MAX, 1);
    cg_mkfile(cg, "pids.events", CGF_PIDS_EVENTS, 0);
  } else {
    cg_rmfile(cg, "pids.current");
    cg_rmfile(cg, "pids.peak");
    cg_rmfile(cg, "pids.max");
    cg_rmfile(cg, "pids.events");
  }
  if (avail & CG_CTRL_MEMORY) {
    cg_mkfile(cg, "memory.current", CGF_MEM_CURRENT, 0);
    cg_mkfile(cg, "memory.peak", CGF_MEM_PEAK, 0);
    cg_mkfile(cg, "memory.min", CGF_MEM_MIN, 1);
    cg_mkfile(cg, "memory.low", CGF_MEM_LOW, 1);
    cg_mkfile(cg, "memory.high", CGF_MEM_HIGH, 1);
    cg_mkfile(cg, "memory.max", CGF_MEM_MAX, 1);
    cg_mkfile(cg, "memory.events", CGF_MEM_EVENTS, 0);
    cg_mkfile(cg, "memory.events.local", CGF_MEM_EVENTS_LOCAL, 0);
    cg_mkfile(cg, "memory.stat", CGF_MEM_STAT, 0);
    cg_mkfile(cg, "memory.swap.current", CGF_MEM_SWAP_CURRENT, 0);
    cg_mkfile(cg, "memory.swap.max", CGF_MEM_SWAP_MAX, 1);
    cg_mkfile(cg, "memory.swap.events", CGF_MEM_SWAP_EVENTS, 0);
    cg_mkfile(cg, "memory.oom.group", CGF_MEM_OOM_GROUP, 1);
  } else {
    cg_rmfile(cg, "memory.current");
    cg_rmfile(cg, "memory.peak");
    cg_rmfile(cg, "memory.min");
    cg_rmfile(cg, "memory.low");
    cg_rmfile(cg, "memory.high");
    cg_rmfile(cg, "memory.max");
    cg_rmfile(cg, "memory.events");
    cg_rmfile(cg, "memory.events.local");
    cg_rmfile(cg, "memory.stat");
    cg_rmfile(cg, "memory.swap.current");
    cg_rmfile(cg, "memory.swap.max");
    cg_rmfile(cg, "memory.swap.events");
    cg_rmfile(cg, "memory.oom.group");
  }
  if (avail & CG_CTRL_CPU) {
    cg_mkfile(cg, "cpu.stat", CGF_CPU_STAT, 0);
    cg_mkfile(cg, "cpu.weight", CGF_CPU_WEIGHT, 1);
    cg_mkfile(cg, "cpu.weight.nice", CGF_CPU_WEIGHT_NICE, 1);
    cg_mkfile(cg, "cpu.max", CGF_CPU_MAX, 1);
  } else {
    cg_rmfile(cg, "cpu.stat");
    cg_rmfile(cg, "cpu.weight");
    cg_rmfile(cg, "cpu.weight.nice");
    cg_rmfile(cg, "cpu.max");
  }
  if (avail & CG_CTRL_IO) {
    cg_mkfile(cg, "io.stat", CGF_IO_STAT, 0);
    cg_mkfile(cg, "io.max", CGF_IO_MAX, 1);
    cg_mkfile(cg, "io.weight", CGF_IO_WEIGHT, 1);
  } else {
    cg_rmfile(cg, "io.stat");
    cg_rmfile(cg, "io.max");
    cg_rmfile(cg, "io.weight");
  }
}

static void cg_populate(struct cgroup *cg) {
  cg_mkfile(cg, "cgroup.procs", CGF_PROCS, 1);
  cg_mkfile(cg, "cgroup.threads", CGF_THREADS, 1);
  cg_mkfile(cg, "cgroup.controllers", CGF_CONTROLLERS, 0);
  cg_mkfile(cg, "cgroup.subtree_control", CGF_SUBTREE_CONTROL, 1);
  cg_mkfile(cg, "cgroup.stat", CGF_STAT, 0);
  if (!cg->is_root) {
    cg->events_node = cg_mkfile(cg, "cgroup.events", CGF_EVENTS, 0);
    cg_mkfile(cg, "cgroup.type", CGF_TYPE, 0);
    cg_mkfile(cg, "cgroup.max.depth", CGF_MAX_DEPTH, 1);
    cg_mkfile(cg, "cgroup.max.descendants", CGF_MAX_DESCENDANTS, 1);
  }
  cg_sync_controller_files(cg);
}

static int cg_mkdir_cb(struct vfs_node *dir, const char *name, u32 mode);
static int cg_rmdir_cb(struct vfs_node *dir, const char *name);
static int cg_unlink_cb(struct vfs_node *dir, const char *name);

/* Rebuild the controller files of every direct child of `cg`, after its
 * cgroup.subtree_control changed. Takes no cgroup lock: cg_mkfile and
 * cg_rmfile work on the VFS tree, which has its own, and the child list is
 * read under cg_lock only long enough to copy it. */
static void cg_sync_children_controller_files(struct cgroup *cg) {
  struct cgroup *kids[64];
  int n = 0;
  u64 flags;

  spin_lock_irqsave(&cg_lock, &flags);
  for (struct cgroup *c = cg_all; c && n < 64; c = c->next)
    if (c->parent == cg && !c->removed && c->dir)
      kids[n++] = c;
  spin_unlock_irqrestore(&cg_lock, flags);
  for (int i = 0; i < n; i++)
    cg_sync_controller_files(kids[i]);
}

static void cg_dir_init(struct cgroup *cg, struct vfs_node *dir) {
  cg->dir = dir;
  dir->inode->data = cg;
  dir->inode->statfs_cb = cg_statfs;
  dir->inode->mkdir_cb = cg_mkdir_cb;
  dir->inode->rmdir_cb = cg_rmdir_cb;
  dir->inode->unlink_cb = cg_unlink_cb;
  /* Its control files are the filesystem's, not the directory's contents:
   * rmdir must not see them as "not empty" (see vfs_remove_child_locked). */
  dir->inode->flags |= VFS_NODE_CTRL_CHILDREN;
}

static struct cgroup *cg_new(struct cgroup *parent, struct vfs_node *dir) {
  struct cgroup *cg = kzalloc(sizeof(*cg));
  if (!cg)
    return 0;
  cg->parent = parent;
  cg->pids_max = CG_PIDS_MAX_UNSET;
  cg->max_depth = CG_PIDS_MAX_UNSET;
  cg->max_descendants = CG_PIDS_MAX_UNSET;
  cg->mem_max = CG_LIM_MAX;
  cg->mem_high = CG_LIM_MAX;
  cg->mem_swap_max = CG_LIM_MAX;
  cg->cpu_weight = CG_CPU_WEIGHT_DEF;
  cg->cpu_quota_us = CG_LIM_MAX;
  cg->cpu_period_us = CG_CPU_PERIOD_DEF_US;
  cg->io_weight = CG_CPU_WEIGHT_DEF;
  cg->is_root = parent ? 0 : 1;
  cg_dir_init(cg, dir);

  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  cg->next = cg_all;
  cg_all = cg;
  cg->id = parent ? cg_id_alloc(cg) : 0; /* the root is id 0 by definition */
  int none_left = parent && !cg->id && !cg_ids_exhausted_said;
  if (none_left)
    cg_ids_exhausted_said = 1;
  spin_unlock_irqrestore(&cg_lock, flags);
  if (none_left)
    console_write("cgroup: out of swap-accounting ids — new cgroups will not "
                  "report memory.swap.current\n");
  return cg;
}

static int cg_mkdir_cb(struct vfs_node *dir, const char *name, u32 mode) {
  (void)mode;
  struct cgroup *parent = dir->inode ? dir->inode->data : 0;
  if (!parent)
    return -EIO;

  /* cgroup.max.depth / cgroup.max.descendants, enforced on the ancestor that
   * set them — the only moment either limit means anything. */
  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  for (struct cgroup *a = parent; a; a = a->parent) {
    if (a->max_depth != CG_PIDS_MAX_UNSET &&
        cg_depth(parent) + 1 - cg_depth(a) > a->max_depth) {
      spin_unlock_irqrestore(&cg_lock, flags);
      return -EAGAIN;
    }
    if (a->max_descendants != CG_PIDS_MAX_UNSET &&
        cg_count_descendants(a) + 1 > a->max_descendants) {
      spin_unlock_irqrestore(&cg_lock, flags);
      return -EAGAIN;
    }
  }
  spin_unlock_irqrestore(&cg_lock, flags);

  struct vfs_node *child = cg_find_child(dir, name);
  if (!child)
    return -EIO;
  child->inode->mode = 0755;
  child->inode->nlink = 2;
  struct cgroup *cg = cg_new(parent, child);
  if (!cg)
    return -ENOMEM;
  cg_populate(cg);
  return 0;
}

static int cg_unlink_cb(struct vfs_node *dir, const char *name) {
  (void)dir;
  (void)name;
  /* A control file cannot be removed; only the directory it describes can. */
  return -EPERM;
}

static void cg_forget(struct cgroup *cg) {
  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup **pp = &cg_all;
  while (*pp) {
    if (*pp == cg) {
      *pp = cg->next;
      break;
    }
    pp = &(*pp)->next;
  }
  cg_limits_recount(); /* its limits are gone with it */
  /* Anything still pointing here belongs to the parent now, exactly as Linux
   * refuses the rmdir until the cgroup is empty and then has nothing to move. */
  for (u32 i = 0; i < CG_SLOTS; i++)
    if (cg_members[i].pid && cg_members[i].cg == cg)
      cg_members[i].cg = cg->parent && !cg->parent->is_root ? cg->parent : 0;
  spin_unlock_irqrestore(&cg_lock, flags);
}

static int cg_rmdir_cb(struct vfs_node *dir, const char *name) {
  struct vfs_node *child = cg_find_child(dir, name);
  if (!child)
    return -ENOENT;
  struct cgroup *cg = child->inode ? child->inode->data : 0;
  if (!cg)
    return -EIO;

  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  if (cg_count_descendants(cg) > 0) {
    spin_unlock_irqrestore(&cg_lock, flags);
    return -ENOTEMPTY;
  }
  if (cg_count_tasks(cg, 1) > 0) {
    spin_unlock_irqrestore(&cg_lock, flags);
    return -EBUSY;
  }
  spin_unlock_irqrestore(&cg_lock, flags);

  /* Drop the control files, so the directory the VFS is about to unlink really
   * has no children left. */
  for (struct vfs_node *c = child->first_child; c;) {
    struct vfs_node *next = c->next_sibling;
    vfs_detach_child(child, c);
    if (c->inode && c->inode->data) {
      kfree(c->inode->data);
      c->inode->data = 0;
    }
    c->deleted = 1;
    vfs_node_put(c);
    c = next;
  }
  child->inode->data = 0;
  cg_forget(cg);
  /* A cgroup namespace rooted here keeps the struct (never the directory):
   * its members still need somewhere to measure their paths from. */
  spin_lock_irqsave(&cg_lock, &flags);
  cg->dir = 0;
  cg->removed = 1;
  int keep = cg->ns_refs > 0;
  if (!keep)
    cg_id_reparent(cg);
  spin_unlock_irqrestore(&cg_lock, flags);
  if (!keep)
    kfree(cg);
  return 0;
}

/* ── mount ───────────────────────────────────────────────────────────────── */

static struct vfs_node *cg_mount_cb(const char *source, u64 flags, void *data) {
  (void)source;
  (void)flags;
  (void)data;
  /* One unified hierarchy, as on Linux: every mount after the first shows the
   * same tree, rooted where the mounting task's cgroup namespace is. */
  u64 lf;
  spin_lock_irqsave(&cg_lock, &lf);
  if (cg_root) {
    struct cgroup *r = cg_ns_root_locked();
    struct vfs_node *dir = r->dir ? r->dir : cg_root->dir;
    vfs_node_get(dir);
    cg_mounts++;
    spin_unlock_irqrestore(&cg_lock, lf);
    return dir;
  }
  spin_unlock_irqrestore(&cg_lock, lf);
  struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
  if (!root)
    return ERR_PTR(-ENOMEM);
  root->inode->mode = 0755;
  root->inode->uid = 0;
  root->inode->gid = 0;
  root->inode->nlink = 2;
  struct cgroup *cg = cg_new(0, root);
  if (!cg) {
    root->deleted = 1;
    vfs_node_put(root);
    return ERR_PTR(-ENOMEM);
  }
  cg_root = cg;
  cg_mounts = 1;
  cg_populate(cg);
  return root;
}

static int cg_umount_cb(struct vfs_node *root_node) {
  (void)root_node;
  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  if (--cg_mounts > 0) {
    spin_unlock_irqrestore(&cg_lock, flags);
    return 0;
  }
  for (u32 i = 0; i < CG_SLOTS; i++) {
    cg_members[i].pid = 0;
    cg_members[i].cg = 0;
  }
  struct cgroup *c = cg_all;
  cg_all = 0;
  cg_root = 0;
  /* What a namespace still roots itself at survives, detached. */
  struct cgroup *to_free = 0;
  while (c) {
    struct cgroup *next = c->next;
    c->dir = 0;
    c->removed = 1;
    if (c->ns_refs == 0) {
      c->next = to_free;
      to_free = c;
    }
    c = next;
  }
  spin_unlock_irqrestore(&cg_lock, flags);
  while (to_free) {
    struct cgroup *next = to_free->next;
    kfree(to_free);
    to_free = next;
  }
  {
    u64 rf;

    spin_lock_irqsave(&cg_lock, &rf);
    cg_limits_recount();
    spin_unlock_irqrestore(&cg_lock, rf);
  }
  cg_sched_clear_all();
  return 0;
}

static struct vfs_fs cgroup2_fs = {
    .name = "cgroup2", .mount = cg_mount_cb, .umount = cg_umount_cb,
    .flags = VFS_FS_NODEV | VFS_FS_USERNS_MOUNT};

void cgroup_init(void) { vfs_register_fs(&cgroup2_fs); }

/* ── task lifetime ───────────────────────────────────────────────────────── */

void cgroup_fork_inherit(usize parent_pid, usize child_pid) {
  if (!cg_root)
    return;
  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup *cg = cg_member_get(parent_pid);
  if (cg)
    cg_member_set(child_pid, cg);
  spin_unlock_irqrestore(&cg_lock, flags);
  if (cg)
    cg_events_refresh();
}

void cgroup_task_exit(usize pid) {
  if (!cg_root)
    return;
  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup *cg = cg_member_get(pid);
  if (cg)
    cg_member_set(pid, 0);
  spin_unlock_irqrestore(&cg_lock, flags);
  if (cg)
    cg_events_refresh();
}

/* clone3(CLONE_INTO_CGROUP): the child is born in the cgroup this descriptor
 * names. See the header for why a kernel that refuses the flag cannot start a
 * systemd service at all.
 *
 * The descriptor has to be a cgroup2 DIRECTORY: those are the only nodes whose
 * inode data is a struct cgroup, and a descriptor on one of the control files
 * inside it names a file, not a group. */
int cgroup_attach_pid_at_fd(int fd, usize pid) {
  if (!cg_root)
    return -EINVAL;
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h || h->kind != VFS_HANDLE_NODE || !h->node)
    return -EBADF;
  struct vfs_node *dir = h->node;
  if (!dir->inode || dir->inode->type != VFS_DIRECTORY)
    return -ENOTDIR;
  /* Only a node this filesystem made carries a struct cgroup in inode->data,
   * and its mkdir hook is what says it is one of ours -- a plain directory on
   * some other filesystem would otherwise have its inode data read as a
   * cgroup pointer. */
  if (dir->inode->mkdir_cb != cg_mkdir_cb)
    return -EINVAL;
  struct cgroup *cg = (struct cgroup *)dir->inode->data;
  if (!cg)
    return -EINVAL;
  return cg_attach_process(cg, pid, 0);
}

int cgroup_fork_allowed(usize parent_pid) {
  if (!cg_root)
    return 0;
  u64 flags;
  int denied = 0;
  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup *cg = cg_member_get(parent_pid);
  for (struct cgroup *a = cg; a; a = a->parent) {
    if (a->pids_max == CG_PIDS_MAX_UNSET)
      continue;
    if (cg_count_tasks(a, 1) >= a->pids_max) {
      a->pids_denied++;
      denied = 1;
      break;
    }
  }
  spin_unlock_irqrestore(&cg_lock, flags);
  return denied ? -EAGAIN : 0;
}

int cgroup_path_of(usize pid, char *buf, usize len) {
  if (!buf || len < 2)
    return -EINVAL;
  if (!cg_root) {
    buf[0] = '/';
    buf[1] = '\0';
    return 1;
  }
  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup *cg = cg_of(pid);
  /* Seen from the reader's cgroup namespace, as Linux renders it. */
  int r = cg_path(cg, cg_ns_root_locked(), buf, len);
  spin_unlock_irqrestore(&cg_lock, flags);
  return r;
}
