/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Static tracepoints and the tracefs tree that names them.
 *
 * WHAT A TRACEPOINT IS HERE
 *
 * A named place in the kernel with three words of payload. `perf stat -e
 * sched:sched_switch` counts how often it is reached; `perf record -e
 * raw_syscalls:sys_enter` takes a sample at each one. The sites are compiled in
 * and off: an off site is one relaxed load of a byte (see TRACEPOINT_FIRE), so
 * putting one on the system-call path costs nothing measurable when nobody is
 * listening.
 *
 * WHY THE FILE LAYOUT IS LINUX'S
 *
 * Because the tools read it rather than ask. perf walks
 * /sys/kernel/tracing/events/<group>/<event>/ for an `id` -- the number that
 * goes into perf_event_attr.config with PERF_TYPE_TRACEPOINT -- and writes
 * `enable` to turn the site on; it falls back to /sys/kernel/debug/tracing, so
 * both paths exist. `available_events` is what `perf list` reads to know what
 * this kernel has at all. A tool that finds none of this concludes the kernel
 * has no tracing and offers the user nothing.
 *
 * DYNAMIC PROBES
 *
 * kprobe_events takes Linux's syntax (`p:name symbol`, `r:name symbol`, and a
 * `-:name` to remove one) and creates an event under the `kprobes` group. The
 * site itself is an int3 patched over the symbol's first byte: see
 * kernel/trace/kprobe.c.
 */

#include <b1nix/tracepoint.h>

#include <b1nix/errno.h>
#include <b1nix/kprobe.h>
#include <b1nix/perf_event.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/vfs.h>

#include <stdio.h>
#include <string.h>

/* ── the registry ────────────────────────────────────────────────────────── */

/* The static sites. The ids are what userspace reads and hands back, so the
 * order here is an ABI of sorts: an entry is added at the end, never renumbered.
 *
 * The field names are the ones tracefs publishes in `format`, so a tool can say
 * what the three words mean. They are the site's own words, not a struct: this
 * kernel does not generate per-event structures, and three u64s cover what the
 * sites have to say. */
static struct b1nix_tracepoint g_tp[] = {
    {"sched", "sched_switch", {"prev_pid", "next_pid", "prev_state"},
     TP_SCHED_SWITCH, 0, 0, 0, 0},
    {"sched", "sched_process_exit", {"pid", "exit_code", "reserved"},
     TP_SCHED_PROCESS_EXIT, 0, 0, 0, 0},
    {"sched", "sched_process_fork", {"parent_pid", "child_pid", "reserved"},
     TP_SCHED_PROCESS_FORK, 0, 0, 0, 0},
    {"raw_syscalls", "sys_enter", {"id", "arg0", "arg1"}, TP_SYS_ENTER, 0, 0, 0, 0},
    {"raw_syscalls", "sys_exit", {"id", "ret", "reserved"}, TP_SYS_EXIT, 0, 0, 0, 0},
    {"block", "block_rq_issue", {"dev", "sector", "nr_sectors"},
     TP_BLOCK_RQ_ISSUE, 0, 0, 0, 0},
    {"block", "block_rq_complete", {"dev", "sector", "error"},
     TP_BLOCK_RQ_COMPLETE, 0, 0, 0, 0},
    {"exceptions", "page_fault_user", {"address", "error_code", "pid"},
     TP_PAGE_FAULT_USER, 0, 0, 0, 0},
    {"io_uring", "io_uring_submit_req", {"opcode", "user_data", "flags"},
     TP_IO_URING_SUBMIT, 0, 0, 0, 0},
    {"io_uring", "io_uring_complete", {"user_data", "res", "cflags"},
     TP_IO_URING_COMPLETE, 0, 0, 0, 0},
};
#define TP_STATIC_N (sizeof(g_tp) / sizeof(g_tp[0]))

/* Dynamic probes, numbered from TP_KPROBE_BASE. A row is free when `name[0]`
 * is NUL. */
#define TP_DYN_MAX 32
struct tp_dyn {
  char name[32];
  char symbol[64];
  struct b1nix_tracepoint tp;
  int is_return;
  int used;
};
static struct tp_dyn g_dyn[TP_DYN_MAX];
static spinlock_t g_tp_lock = SPINLOCK_INIT;

struct b1nix_tracepoint *tracepoint_by_id(u16 id) {
  for (usize i = 0; i < TP_STATIC_N; i++)
    if (g_tp[i].id == id)
      return &g_tp[i];
  for (usize i = 0; i < TP_DYN_MAX; i++)
    if (g_dyn[i].used && g_dyn[i].tp.id == id)
      return &g_dyn[i].tp;
  return 0;
}

struct b1nix_tracepoint *tracepoint_by_name(const char *group,
                                            const char *name) {
  if (!group || !name)
    return 0;
  for (usize i = 0; i < TP_STATIC_N; i++)
    if (strcmp(g_tp[i].group, group) == 0 && strcmp(g_tp[i].name, name) == 0)
      return &g_tp[i];
  for (usize i = 0; i < TP_DYN_MAX; i++)
    if (g_dyn[i].used && strcmp(g_dyn[i].tp.group, group) == 0 &&
        strcmp(g_dyn[i].tp.name, name) == 0)
      return &g_dyn[i].tp;
  return 0;
}

struct b1nix_tracepoint *tracepoint_nth(usize n) {
  if (n < TP_STATIC_N)
    return &g_tp[n];
  n -= TP_STATIC_N;
  for (usize i = 0; i < TP_DYN_MAX; i++) {
    if (!g_dyn[i].used)
      continue;
    if (n == 0)
      return &g_dyn[i].tp;
    n--;
  }
  return 0;
}

int tracepoint_enabled(u16 id) {
  struct b1nix_tracepoint *tp = tracepoint_by_id(id);

  return tp ? __atomic_load_n(&tp->enabled, __ATOMIC_RELAXED) : 0;
}

/* Turn the site's gate on or off to match fs_on|refs, patching a dynamic
 * probe's instruction on the way. Called with g_tp_lock NOT held: arming talks
 * to the kprobe layer, which takes its own lock. */
static int tracepoint_regate(struct b1nix_tracepoint *tp) {
  int want = (tp->fs_on || tp->refs) ? 1 : 0;

  if (want == (int)__atomic_load_n(&tp->enabled, __ATOMIC_RELAXED))
    return 0;
  if (tp->id >= TP_KPROBE_BASE) {
    for (usize i = 0; i < TP_DYN_MAX; i++) {
      int rc;

      if (!g_dyn[i].used || g_dyn[i].tp.id != tp->id)
        continue;
      rc = want ? kprobe_arm(g_dyn[i].symbol, tp->id, g_dyn[i].is_return)
                : kprobe_disarm(tp->id);
      if (rc < 0)
        return rc;
      break;
    }
  }
  __atomic_store_n(&tp->enabled, (u8)want, __ATOMIC_RELEASE);
  return 0;
}

int tracepoint_set_enabled(u16 id, int on) {
  struct b1nix_tracepoint *tp = tracepoint_by_id(id);
  int rc;
  u8 was;

  if (!tp)
    return -ENOENT;
  was = tp->fs_on;
  tp->fs_on = on ? 1 : 0;
  rc = tracepoint_regate(tp);
  if (rc < 0)
    tp->fs_on = was; /* the site did not change, so neither does the file */
  return rc;
}

int tracepoint_ref_get(u16 id) {
  struct b1nix_tracepoint *tp = tracepoint_by_id(id);
  int rc;

  if (!tp)
    return -ENOENT;
  tp->refs++;
  rc = tracepoint_regate(tp);
  if (rc < 0)
    tp->refs--;
  return rc;
}

void tracepoint_ref_put(u16 id) {
  struct b1nix_tracepoint *tp = tracepoint_by_id(id);

  if (!tp || !tp->refs)
    return;
  tp->refs--;
  tracepoint_regate(tp);
}

void tracepoint_fire_slow(u16 id, u64 a, u64 b, u64 c) {
  struct b1nix_tracepoint *tp = tracepoint_by_id(id);

  if (!tp)
    return;
  __atomic_add_fetch(&tp->hits, 1, __ATOMIC_RELAXED);
  /* perf decides whether anybody wanted this one, and whether it is a count or
   * a sample. Called with whatever context the site has: a tracepoint fires
   * from interrupt context as readily as from a system call, so nothing here
   * allocates or sleeps. */
  perf_tracepoint_hit(id, a, b, c);
  /* And the small text ring a human reads with `cat trace`. */
  tracefs_record(id, a, b, c);
}

/* ── dynamic probes ──────────────────────────────────────────────────────── */

int tracepoint_kprobe_add(const char *name, const char *symbol,
                          int is_return) {
  u64 flags;
  int slot = -1;

  if (!name || !*name || !symbol || !*symbol)
    return -EINVAL;
  if (strlen(name) >= sizeof(g_dyn[0].name) ||
      strlen(symbol) >= sizeof(g_dyn[0].symbol))
    return -ENAMETOOLONG;
  if (!kprobe_symbol_ok(symbol))
    return -ENOENT;
  spin_lock_irqsave(&g_tp_lock, &flags);
  for (usize i = 0; i < TP_DYN_MAX; i++) {
    if (g_dyn[i].used && strcmp(g_dyn[i].name, name) == 0) {
      spin_unlock_irqrestore(&g_tp_lock, flags);
      return -EEXIST;
    }
    if (!g_dyn[i].used && slot < 0)
      slot = (int)i;
  }
  if (slot < 0) {
    spin_unlock_irqrestore(&g_tp_lock, flags);
    return -ENOSPC;
  }
  struct tp_dyn *d = &g_dyn[slot];

  memset(d, 0, sizeof(*d));
  strncpy(d->name, name, sizeof(d->name) - 1);
  strncpy(d->symbol, symbol, sizeof(d->symbol) - 1);
  d->is_return = is_return;
  d->tp.group = "kprobes";
  d->tp.name = d->name;
  d->tp.fields[0] = "ip";
  d->tp.fields[1] = "arg0";
  d->tp.fields[2] = "arg1";
  d->tp.id = (u16)(TP_KPROBE_BASE + slot);
  d->tp.enabled = 0;
  d->tp.fs_on = 0;
  d->tp.refs = 0;
  d->tp.hits = 0;
  d->used = 1;
  spin_unlock_irqrestore(&g_tp_lock, flags);
  return (int)d->tp.id;
}

int tracepoint_kprobe_remove(const char *name) {
  u64 flags;
  int rc = -ENOENT;

  if (!name)
    return -EINVAL;
  spin_lock_irqsave(&g_tp_lock, &flags);
  for (usize i = 0; i < TP_DYN_MAX; i++) {
    if (!g_dyn[i].used || strcmp(g_dyn[i].name, name) != 0)
      continue;
    u16 id = g_dyn[i].tp.id;

    spin_unlock_irqrestore(&g_tp_lock, flags);
    kprobe_disarm(id);
    spin_lock_irqsave(&g_tp_lock, &flags);
    memset(&g_dyn[i], 0, sizeof(g_dyn[i]));
    rc = 0;
    break;
  }
  spin_unlock_irqrestore(&g_tp_lock, flags);
  return rc;
}

void tracepoint_kprobe_clear_all(void) {
  for (usize i = 0; i < TP_DYN_MAX; i++)
    if (g_dyn[i].used)
      tracepoint_kprobe_remove(g_dyn[i].name);
}
