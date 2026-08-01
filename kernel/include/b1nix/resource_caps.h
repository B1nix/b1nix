#ifndef B1NIX_RESOURCE_CAPS_H
#define B1NIX_RESOURCE_CAPS_H

/* ── M77: Global Resource Caps ──
 *
 * Runtime-tunable hard caps for system-wide resources that historically were
 * baked-in #define constants. The limits now live in a single struct, sized at
 * init from usable RAM and adjustable at runtime through the writable
 * /proc/sys/kernel/ sysctl entries (see kernel/syscall/resource_caps.c).
 *
 * Design notes:
 *  - All limits carry both a "current" runtime value and a hard ceiling
 *    (the initial #define value). The sysctl write path rejects attempts to
 *    raise a limit above the ceiling, exactly like RLIMIT_* soft/hard pairs.
 *  - The lower bound is always a small positive minimum so a misconfigured
 *    system (sysctl writes a tiny value) cannot deadlock basic boot paths
 *    (e.g. TCP connections FEWER than the smoke suite needs would hang).
 *  - Caps struct is intentionally NOT touched from fast paths: i.e. tcp_alloc
 *    reads g_caps.tcp_max_conns once per allocation; it does NOT acquire a
 *    spinlock on the hot path. Writes are rare and serialised by the sysctl
 *    write_cb into the caps, but the kill-side has no rollback of in-flight
 *    allocations, consistent with Linux behaviour (reduction takes effect
 *    against future allocations only).
 */

#include <b1nix/types.h>

/* Initial ceilings (the compiled-in #defines that previously governed these).
 * A sysctl call cannot raise a cap above the corresponding ceiling. */
#define CAP_TCP_CEIL        256   /* was: 64, but scaling allows more on big hosts */
#define CAP_PIPES_CEIL      1024  /* was: 128 */
#define CAP_SHMMAX_CEIL_MB  256   /* upper bound for SHMMAX in MiB */
#define CAP_COREDUMP_CEIL   (8ULL * 1024 * 1024) /* 8 MiB */

/* Absolute minimums enforced by the write path. */
#define CAP_TCP_MIN         16
#define CAP_PIPES_MIN       16
#define CAP_SHMMAX_MIN_MB   4
#define CAP_COREDUMP_MIN    (64 * 1024)

struct resource_caps {
  /* Maximum number of simultaneous TCP connection slots. */
  u32 tcp_max_conns;
  /* Maximum number of VFS pipe (and FIFO) buffers. */
  u32 max_pipes;
  /* Maximum size (bytes) of a single System V shared memory segment. */
  u64 shmmax_bytes;
  /* Default core-dump file size cap (bytes), also exposed as the initial
   * RLIMIT_CORE soft limit for new processes. */
  u64 coredump_max_bytes;
};

extern struct resource_caps g_resource_caps;

/* Initialise the caps from usable RAM. Called once from kernel_main before
 * subsystems (TCP, pipes, shm) read the values back. */
void resource_caps_init(void);

/* sysctl write helpers — return 0 on success, -EINVAL on out-of-range. */
int resource_caps_set_tcp_max(u32 v);
int resource_caps_set_max_pipes(u32 v);
int resource_caps_set_shmmax(u64 v);
int resource_caps_set_coredump_max(u64 v);

/* Read-side helpers so callers never touch g_resource_caps directly on hot
 * paths (each is a single volatile read, no lock). */
static inline u32 resource_caps_tcp_max(void) {
  return g_resource_caps.tcp_max_conns ? g_resource_caps.tcp_max_conns
                                       : CAP_TCP_CEIL;
}
static inline u32 resource_caps_pipe_max(void) {
  return g_resource_caps.max_pipes ? g_resource_caps.max_pipes : CAP_PIPES_CEIL;
}

#endif /* B1NIX_RESOURCE_CAPS_H */
