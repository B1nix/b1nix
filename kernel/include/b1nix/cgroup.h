/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_CGROUP_H
#define B1NIX_CGROUP_H

#include <b1nix/types.h>

/*
 * cgroup v2 — the unified hierarchy (kernel/fs/cgroup/cgroup.c).
 *
 * systemd will not boot without it: it mounts "cgroup2" on /sys/fs/cgroup,
 * creates a directory per unit, and moves each unit's processes into it by
 * writing their pid to cgroup.procs. Everything it does with the hierarchy —
 * naming a unit's processes, knowing when the last one has gone, killing what
 * is left — is a read or a write of a file in that tree.
 *
 * Membership lives in a side table keyed by task id, not in struct task: that
 * struct must not grow (see kernel/sched/namespace.c for the same constraint).
 */

/* Register the "cgroup2" filesystem type. Called once from vfs_init. */
void cgroup_init(void);

/* A new task starts in its parent's cgroup, as on Linux. */
void cgroup_fork_inherit(usize parent_pid, usize child_pid);

/* Drop a task's membership. Called from the exit path. */
void cgroup_task_exit(usize pid);

/* clone3(CLONE_INTO_CGROUP): put a task straight into the cgroup a directory
 * descriptor names, instead of into its parent's.
 *
 * The point of doing it at clone time rather than by writing cgroup.procs
 * afterwards is that there is no window in which the child is accounted to the
 * wrong cgroup -- which matters to anything that limits or counts by cgroup.
 * glibc's posix_spawn() uses it for POSIX_SPAWN_SETCGROUP, and systemd 254 and
 * later spawn every single unit through exactly that path: refusing the flag
 * made posix_spawn answer EINVAL, and not one service on the machine could be
 * started ("Failed to spawn executor: Invalid argument").
 *
 * `fd` must be a descriptor on a cgroup2 directory. Returns 0, or -EBADF /
 * -EINVAL when it is not one. */
int cgroup_attach_pid_at_fd(int fd, usize pid);

/* pids.max: may `parent_pid` create another task? Returns 0 when it may, and
 * -EAGAIN when a cgroup between the parent and the root is at its limit —
 * which is the errno Linux's pids controller makes fork(2) return. */
int cgroup_fork_allowed(usize parent_pid);

/* The v2 path of a task's cgroup ("/" for the root), for /proc/<pid>/cgroup.
 * Writes at most `len` bytes including the NUL and returns the length, or a
 * negative errno. Answers "/" for a task with no membership, which is what a
 * kernel with no cgroup2 mounted reports. */
int cgroup_path_of(usize pid, char *buf, usize len);

/* Controllers this kernel implements, space separated. Backs /proc/cgroups as
 * well as the cgroup.controllers file. */
const char *cgroup_available_controllers(void);

/* ── the controllers ────────────────────────────────────────────────────────
 *
 * Four: pids, memory, cpu and io. A controller is advertised only when it is
 * enforced, because advertising one means accepting a limit, and accepting a
 * limit that nothing enforces is a lie told to the process that set it --
 * systemd in particular, whose whole resource model is these files.
 *
 * Each hook below is called from the subsystem that owns the resource, and
 * each is a no-op with one predictable branch until a cgroup actually sets a
 * limit: a machine that never writes memory.max pays nothing for the memory
 * controller. */

/* Called once per timer tick from the BSP. Rolls cpu.max periods over, charges
 * the CPU time every task burned since the last tick to its cgroup, and once
 * every sweep interval recomputes the stride weights the scheduler applies. */
void cgroup_tick(void);

/* A page fault has just installed a page in the current task's address space.
 *
 * `fault_addr` decides whether it counts: what matters is that the page is a
 * USERSPACE page, not that the access came from ring 3. A kernel that fills a
 * user buffer -- read(2) into a freshly mapped page, which is how a program
 * that reads /dev/zero into a growing buffer takes all the memory there is --
 * faults from ring 0 on a user address, and charging only ring-3 faults let
 * exactly that program eat the machine inside a cgroup with a memory.max.
 *
 * Charges the page to the task's cgroups and, when the running estimate
 * crosses a memory.max anywhere up the chain, measures that cgroup exactly and
 * -- if it really is over -- kills inside it. The fault is never failed, not
 * even when the caller is the task chosen to die: the page was installed, and
 * the victim dies of its pending SIGKILL on the way back to ring 3. */
void cgroup_mem_fault_charge(u64 fault_addr);

/* The same charge for a path that maps pages without faulting: brk(2) grows
 * the break by allocating and mapping every page itself, so the fault hook
 * never sees the largest allocation a glibc process makes. */
void cgroup_mem_charge_pages(u64 npages);

/* Would charging `npages` to the running task's cgroup take it past a
 * memory.max? Asked before a transparent huge page is installed, because a
 * block commits 512 pages at once and is not reclaimable while it is a block:
 * a cgroup that took one at its limit would have nothing to give back and be
 * killed holding memory it was never going to touch. A refusal here is a
 * fallback to ordinary 4 KiB pages, which reclaim can take. */
int cgroup_mem_would_exceed(u64 npages);

/* A major fault: the current task waited for a page to come back from swap.
 * Counted in memory.stat's pgmajfault for its cgroups. */
void cgroup_mem_note_majfault(void);

/* io: one device command has completed for the current task. `devno` is the
 * device's (major << 8) | minor, `bytes` the transfer size. Feeds io.stat and
 * the io.max windows. */
void cgroup_io_account(u32 devno, u64 bytes, int write);

/* io.max: how many nanoseconds the current task must wait before its next
 * command to this device, so that its cgroup stays inside its bytes-per-second
 * and IOs-per-second ceilings. 0 when nothing limits it. */
u64 cgroup_io_delay_ns(u32 devno, u64 bytes, int write);

/* The oom_score_adj of a task, and the task the OOM killer should choose.
 * `within` is a cgroup (as an opaque pointer, 0 for the whole machine); the
 * victim is the live task with the highest badness that is not pid 1 and has
 * not set oom_score_adj to -1000. Returns 0 when there is nothing to kill. */
usize cgroup_oom_victim(void *within);

/* ── swap accounting ────────────────────────────────────────────────────────
 *
 * memory.swap.current is the pages of this cgroup's memory that are out in
 * swap, and it can only be that if a page carries its owner with it: the task
 * that faulted it may be asleep, moved, or dead by the time the page comes
 * back. So the eviction path asks for the owner's id here, the swap layer
 * stores the id beside the slot (kernel/mm/swap.c), and the charge is released
 * when the slot is freed -- by whoever frees it, in whatever context.
 *
 * The id is small and stable: a cgroup gets one when it is created and keeps
 * it until both the cgroup is gone and its last swapped page has come back.
 * A cgroup removed while it still has pages out hands its id to its parent,
 * which is where Linux's charges go too (the "reparent" in mem_cgroup_css_
 * offline), so the count can never be attributed to a cgroup that no longer
 * exists and can never simply vanish. */

/* The id of a task's cgroup. 0 for the root, for a task with no membership,
 * and on a machine with no cgroup2: everywhere the charge has nowhere more
 * specific to go. */
u16 cgroup_id_of_task(usize pid);

/* Charge `pages` to the cgroup `id` names and to every ancestor. Returns 0, or
 * -1 when an ancestor is at its memory.swap.max -- in which case nothing was
 * charged and the caller must not swap the page out. */
int cgroup_swap_charge(u16 id, u64 pages);

/* Release a charge taken above. Called from the swap layer when a slot is
 * freed: a major fault, an address space being torn down, or swapoff. */
void cgroup_swap_uncharge(u16 id, u64 pages);

#endif /* B1NIX_CGROUP_H */
