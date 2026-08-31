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

/* Controllers this kernel implements, space separated ("pids"). Backs
 * /proc/cgroups as well as the cgroup.controllers file. */
const char *cgroup_available_controllers(void);

#endif /* B1NIX_CGROUP_H */
