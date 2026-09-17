# Isolation and security

Milestones M15, M31, M63, M77, M109 (namespaces) and M123; Landlock and keys
from M124 are described in `processes-and-system-calls.md`.

## Users, permissions and resource caps

Credentials carry real, effective, saved and filesystem ids with supplementary
groups. The VFS checks permissions the way Linux's generic permission code
does, including sticky directories and set-group-id stripping. Setuid
executables change credentials unless the mount is `nosuid` or the process has
`no_new_privs` (M15, M31). Capabilities use Linux's numbering and sets:
permitted, effective, inheritable, bounding and ambient, plus securebits.
Password checks go through `/etc/shadow` with SHA-512 and PAM. Global resource
caps (TCP connections, pipes, core dump size, SHMMAX) are tunable at runtime
under `/proc/sys/kernel` (M77).

## Sandboxing

seccomp-bpf filters stack. The most severe verdict wins, with Linux's
precedence, and strict mode is available. A filter can be installed only with
`no_new_privs` set or with `CAP_SYS_ADMIN` in the caller's user namespace
(M63).

## Namespaces

All eight namespace kinds exist: UTS, mount, PID, network, user, IPC, cgroup
and time (M109, M123). Each namespace is a reference-counted object with an
owning user namespace and an nsfs inode. `/proc/<pid>/ns/*` names it, the
`NS_GET_*` ioctls describe it, and `setns` enters it through such a handle or a
pidfd. The state of a mount, network, IPC or cgroup namespace is torn down on
the `ns-reaper` thread when its last task, handle or mount lets go.

**User namespaces.** The kernel keeps one space of ids, and a user namespace is
a translation into it, written through `uid_map` and `gid_map` under Linux's
rules. Ids are translated at the system-call boundary; an id with no mapping
reads as 65534. Capabilities count only in the namespace a task holds them in
and in the namespaces it owns below. Every guarded resource asks the namespace
that owns it:

- mounts ask the owner of the mount namespace;
- interface configuration asks the owner of the network namespace;
- ptrace, kill and `prlimit` ask the target's user namespace.

A mount made from a user namespace may use only filesystem types built for it
and always gets `nodev`. Flags inherited into a less privileged mount namespace
are locked, and shared mounts there become slaves. `newuidmap` and `newgidmap`
hand a user the ranges in `/etc/subuid` and `/etc/subgid`.

**IPC and cgroup.** SysV shared memory, semaphores and message queues belong to
an IPC namespace, and POSIX message queues are an `mqueue` filesystem instance
per IPC namespace. A cgroup namespace roots `/proc/<pid>/cgroup` and new
`cgroup2` mounts at its creator's cgroup.

**PID namespaces.** `CLONE_NEWPID` works on `clone`, `clone3` and `unshare`.
Every pid a process sees is its namespace's number, including the tids written
by `set_tid_address` and `CLONE_*_SETTID`. The first process is the namespace's
init:

- it adopts orphans, after the nearest `PR_SET_CHILD_SUBREAPER`;
- it ignores signals it has no handler for when they come from inside the
  namespace;
- its exit kills everything in the namespace.

procfs is per mount and shows the namespace of the task that mounted it.

**Time namespaces.** They offset the monotonic and boot clocks through
`/proc/<pid>/timens_offsets`, which can be written until the first task enters.
Processes in such a namespace read their clocks through the system call instead
of the vDSO page.

## Mounts the way container runtimes use them

Container runtimes lean on the mount table harder than anything else, and a
node-keyed table was not enough. Each mount records the mount it is attached
to, as seen by the path walk, because a bind mount shares its nodes with its
source and the node alone cannot say which tree a walk is in. Moving a mount
moves the mount the source path resolves to, not merely the first one with the
same root. The other operations follow Linux:

- A recursive bind copies the mounts below its source.
- A remount changes only the top mount at a path.
- `umount2(MNT_DETACH)` takes the whole subtree.
- `pivot_root(".", ".")` stacks the old root over the new one so that it can
  be detached right after.

An open file remembers the flags of the mount it was opened through, and
`fstatfs` reports them even after that mount has been detached. runc and crun
protect their own binary exactly that way: a read-only bind, detached at once,
then a check of `/proc/self/exe`. The mount-root attribute of `statx` and mount
ids come from the walk, and descriptor paths are relative to the process's root
after `chroot`, which is how systemd-nspawn works inside its container.

## What has been run on it

The posix smoke lane runs, as an unprivileged user, util-linux's
`unshare -Urpf` and `nsenter`, `bwrap --unshare-all`, and rootless `podman` with
`crun`. The container itself reports being uid 0 over the user's subordinate
range, pid 1, and having its own hostname and `/proc` (markers `M123-TOOLS` and
`M123-PODMAN`). The Debian lane starts a command with `systemd-nspawn`
(`DEBIAN-SMOKE: ok nspawn`).

Some things are not done yet:

- Detaching a mount namespace's own root answers EINVAL, where Linux allows it.
- Child mounts are not locked to their parents, and idmapped mounts are
  refused.
- All devpts instances share one pty numbering.
- There is no cgroup v1, so nspawn with a tree that contains no systemd needs
  `SYSTEMD_NSPAWN_UNIFIED_HIERARCHY=1`.
- podman has been run only with `--rootfs`, the vfs storage driver, no network
  and cgroups disabled.
- nspawn has not been run with `--private-users`.
- Rootless networking, overlay storage and Chromium's unprivileged sandbox are
  untested.
