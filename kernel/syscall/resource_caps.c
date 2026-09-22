/* SPDX-License-Identifier: GPL-2.0-only */
/* M77: Global Resource Caps — runtime-tunable hard caps.
 *
 * TCP connection slots, VFS pipe buffers, System V shared-memory segment size
 * (SHMMAX) and the core-dump byte cap were compile-time #defines. They now live
 * in `g_resource_caps`, sized from usable RAM at boot and adjustable at runtime
 * through the writable /proc/sys/kernel/{tcp-max-conns,pipe-max-count,shmmax,
 * coredump-max-bytes} sysctl entries (see kernel/fs/proc/procfs.c).
 *
 * The write helpers clamp to [MIN, CEIL] and reject values outside the range
 * with -EINVAL; a cap reduction takes effect against FUTURE allocations only,
 * mirroring how Linux RLIMIT reduction behaves (no rollback of in-flight
 * resources). All helpers are called from the procfs write_cb (single writer
 * at a time), so no locking is needed on the caps themselves.
 */

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/resource_caps.h>

struct resource_caps g_resource_caps;

void resource_caps_init(void) {
  u64 ram_bytes = pmm_total_usable_memory();
  u64 ram_gb = ram_bytes / (1024ULL * 1024ULL * 1024ULL);

  /* TCP connection slots: 1024 at 1 GiB, +512 per extra GiB. Backing is a
   * compile-time array sized to CAP_TCP_CEIL; only this many are used.
   *
   * A thousand and not a hundred: a single test can hold a thousand at once --
   * liburing's accept opens 128 listeners and then connects 32 clients through
   * one of them per round, and
   * accept opens 32 clients and accepts 32 of them on one listener, twice over,
   * while the previous round's connections are still in TIME_WAIT -- and a table
   * that runs out does not fail the accept, it simply stops handing connections
   * over, which reads as a hang rather than as an error. */
  u32 tcp = 1024 + (u32)(ram_gb * 512);
  if (tcp < 1024) tcp = 1024;
  if (tcp > CAP_TCP_CEIL) tcp = CAP_TCP_CEIL;
  g_resource_caps.tcp_max_conns = tcp;

  /* VFS pipe buffers: 1024 at 1 GiB, +512 per extra GiB, clamped to
   * [1024, 4096].
   *
   * The old floor was 128, which is below what a program is entitled to ask
   * for: Linux's own soft limit is fs.pipe-user-pages-soft, 16384 pages, i.e.
   * 1024 pipes of the default 64 KiB -- and liburing's poll-mshot-update opens
   * exactly that many, so a thousand-and-first pipe is where this kernel used
   * to answer ENFILE while Linux answers with a pipe. */
  u32 pipes = 1024 + (u32)(ram_gb * 512);
  if (pipes < 1024) pipes = 1024;
  if (pipes > CAP_PIPES_CEIL) pipes = CAP_PIPES_CEIL;
  g_resource_caps.max_pipes = pipes;

  /* SHMMAX: the graphics budget (32 MiB) is the floor — enough for a full
   * 1280x800x4 framebuffer + a couple of windows. Scale up with RAM. */
  u64 shm = 32ULL * 1024 * 1024;
  u64 shm_extra = ram_gb * (8ULL * 1024 * 1024);
  if (shm + shm_extra > (u64)CAP_SHMMAX_CEIL_MB * 1024 * 1024)
    shm = (u64)CAP_SHMMAX_CEIL_MB * 1024 * 1024;
  else
    shm += shm_extra;
  g_resource_caps.shmmax_bytes = shm;

  g_resource_caps.coredump_max_bytes = 1024 * 1024; /* 1 MiB */

  console_write("resource caps: tcp_max=");
  console_write_dec(g_resource_caps.tcp_max_conns);
  console_write(" pipes=");
  console_write_dec(g_resource_caps.max_pipes);
  console_write(" shmmax=");
  console_write_dec(shm / 1024);
  console_write(" KB coredump=");
  console_write_dec(g_resource_caps.coredump_max_bytes);
  console_write(" bytes\n");
}

int resource_caps_set_tcp_max(u32 v) {
  if (v < CAP_TCP_MIN || v > CAP_TCP_CEIL)
    return -EINVAL;
  g_resource_caps.tcp_max_conns = v;
  return 0;
}

int resource_caps_set_max_pipes(u32 v) {
  if (v < CAP_PIPES_MIN || v > CAP_PIPES_CEIL)
    return -EINVAL;
  g_resource_caps.max_pipes = v;
  return 0;
}

int resource_caps_set_shmmax(u64 v) {
  u64 min = (u64)CAP_SHMMAX_MIN_MB * 1024 * 1024;
  u64 max = (u64)CAP_SHMMAX_CEIL_MB * 1024 * 1024;
  if (v < min || v > max)
    return -EINVAL;
  g_resource_caps.shmmax_bytes = v;
  return 0;
}

int resource_caps_set_coredump_max(u64 v) {
  if (v < CAP_COREDUMP_MIN || v > CAP_COREDUMP_CEIL)
    return -EINVAL;
  g_resource_caps.coredump_max_bytes = v;
  return 0;
}
