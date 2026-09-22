/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_TRACEPOINT_H
#define B1NIX_TRACEPOINT_H

#include <b1nix/types.h>

/*
 * Static tracepoints, and the tracefs tree that names them.
 *
 * A tracepoint is a named place in the kernel that a program can ask to be told
 * about: `perf stat -e sched:sched_switch`, `perf record -e
 * raw_syscalls:sys_enter`, a bpftrace probe. The names and the file layout are
 * Linux's, because the tools read them rather than ask: /sys/kernel/tracing
 * holds one directory per event with an `id` (what perf_event_open puts in
 * attr.config) and an `enable` (what turns the site on).
 *
 * Every site is off until something enables it, and an off site costs one
 * relaxed load -- which is why the check is a macro here and the work is a
 * function call away.
 */

enum b1nix_tracepoint_id {
  /* The ids are the numbers userspace reads from tracefs and hands back in
   * attr.config, so they are assigned here and never renumbered. */
  TP_SCHED_SWITCH = 1,
  TP_SCHED_PROCESS_EXIT,
  TP_SCHED_PROCESS_FORK,
  TP_SYS_ENTER,
  TP_SYS_EXIT,
  TP_BLOCK_RQ_ISSUE,
  TP_BLOCK_RQ_COMPLETE,
  TP_PAGE_FAULT_USER,
  TP_IO_URING_SUBMIT,
  TP_IO_URING_COMPLETE,
  TP_KPROBE_BASE = 256, /* dynamic probes are numbered from here */
  TP_ID_MAX = 512,
};

struct b1nix_tracepoint {
  const char *group;
  const char *name;
  /* The format line tracefs publishes, so `perf` can decode the raw payload
   * this site writes. Three fields, named for what the site passes. */
  const char *fields[3];
  u16 id;
  /* The gate the sites read. It is on when tracefs was told to turn the event
   * on, or while at least one perf event is open on it -- `perf stat -e
   * sched:sched_switch` never writes tracefs at all, and an event nobody reads
   * must go back off when the last reader leaves. */
  volatile u8 enabled;
  u8 fs_on;    /* tracefs `enable` says 1 */
  u16 refs;    /* perf events open on this site */
  u64 hits;
};

/* Is anything listening at this site? The fast path of every tracepoint. */
int tracepoint_enabled(u16 id);
/* Tell whoever is listening. `a`, `b` and `c` are the site's own three words. */
void tracepoint_fire_slow(u16 id, u64 a, u64 b, u64 c);

#define TRACEPOINT_FIRE(id, a, b, c)                                           \
  do {                                                                         \
    if (tracepoint_enabled(id))                                                \
      tracepoint_fire_slow((id), (u64)(a), (u64)(b), (u64)(c));                 \
  } while (0)

/* The registry, for tracefs and for perf_event_open. */
struct b1nix_tracepoint *tracepoint_by_id(u16 id);
struct b1nix_tracepoint *tracepoint_by_name(const char *group, const char *name);
struct b1nix_tracepoint *tracepoint_nth(usize n); /* NULL past the end */
int tracepoint_set_enabled(u16 id, int on); /* what tracefs `enable` writes */
/* A reader arrives or leaves (perf_event_open on this site). */
int tracepoint_ref_get(u16 id);
void tracepoint_ref_put(u16 id);

/* A dynamic probe: a kprobe named by symbol. Returns its id, or -errno. */
int tracepoint_kprobe_add(const char *name, const char *symbol, int is_return);
int tracepoint_kprobe_remove(const char *name);
void tracepoint_kprobe_clear_all(void);

/* Build the /sys/kernel/tracing tree. Called once, after sysfs is mounted. */
void tracefs_init(void);
/* Create the files for any event that does not have them yet (a dynamic probe
 * has just been added). */
void tracefs_refresh_events(void);
/* Put a formatted line in the small ring `cat trace` reads. */
void tracefs_record(u16 id, u64 a, u64 b, u64 c);

#endif /* B1NIX_TRACEPOINT_H */
