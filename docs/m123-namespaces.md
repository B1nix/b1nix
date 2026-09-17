# M123: namespaces for containers

What each namespace kind owns, the mount-table model containers needed, and
what is still missing. Smoke markers: `M123-SMOKE: ok *`
(`userspace/bin/smoke/m123_smoke.c`), `M123-TOOLS: ok *` and
`M123-PODMAN: ok rootless-run` (`userspace/rootfs-overlay/etc/m123-*-smoke.sh`,
posix lane), `DEBIAN-SMOKE: ok nspawn` (`tools/images/debian-stage.sh`).

## Core (`kernel/sched/namespace.c`)

- Eight kinds: UTS, mount, PID, network, user, IPC, cgroup, time. Each
  namespace is a refcounted slot with an owning user namespace and an nsfs
  inode number; `/proc/<pid>/ns/*` links, `NS_GET_USERNS`/`NS_GET_PARENT`/
  `NS_GET_NSTYPE`/`NS_GET_OWNER_UID`, and `setns` on nsfs handles or pidfds.
- Teardown of mount, network, IPC and cgroup state runs on the `ns-reaper`
  thread once the last reference (task, open handle, mount) is gone.

## User namespaces (`kernel/sched/user_namespace.c`)

- One kernel id space; ids are translated at the syscall boundary
  (`make_kuid`/`from_kuid_munged`, overflow id 65534). uid/gid are 32-bit.
- `uid_map`/`gid_map`/`setgroups` follow Linux's write rules (one write, the
  writer's capabilities over the parent, `setgroups` must be `deny` for an
  unprivileged gid map). `newuidmap`/`newgidmap` from shadow-subids work.
- Capabilities use Linux's numbers and are relative to a namespace
  (`ns_capable_cred`): full set in an owned child namespace, nothing outside it.
  Guarded resources check the owner of the namespace they belong to: mount
  namespace for mounts, network namespace for interface configuration and
  its sysctls, the target's user namespace for ptrace, kill and `prlimit`.
- Mounts from a user namespace: only filesystem types marked
  `VFS_FS_USERNS_MOUNT`, `MS_NODEV` forced, access flags inherited into a less
  privileged mount namespace are locked (`locked_flags`), shared mounts become
  slaves.

## IPC, cgroup and time

- SysV shm/sem/msg objects carry their IPC namespace; POSIX message queues are
  the `mqueue` filesystem, one instance per IPC namespace, with priorities,
  timeouts and `mq_notify` (`SIGEV_SIGNAL` and `SIGEV_THREAD`).
- A cgroup namespace roots `/proc/<pid>/cgroup` and a new `cgroup2` mount at the
  creator's cgroup.
- Time namespaces offset `CLOCK_MONOTONIC*`/`CLOCK_BOOTTIME*`
  (`/proc/<pid>/timens_offsets`, writable until a task enters); tasks in one
  get a vDSO data page that forces the system-call path.

## PID namespaces (`kernel/sched/pid_namespace.c`)

- `CLONE_NEWPID` on `clone`/`clone3`/`unshare`; per-namespace numbers for every
  pid a process sees, including `set_tid_address` and `CLONE_*_SETTID`.
- The first process is the namespace's init: it reaps orphans (after the
  nearest `PR_SET_CHILD_SUBREAPER`), ignores signals it has no handler for
  from inside, and its exit kills the namespace.
- procfs is per mount: each mount shows the mounting task's PID namespace.

## Mounts as containers use them (`kernel/fs/vfs.c`)

- Each mount entry records the mount it is attached to (`parent_seq`, set by
  the path walk). A bind shares nodes with its source, so the node alone cannot
  say which tree a walk is in.
- `MS_BIND|MS_REC` copies the submounts; remount changes only the top mount at
  a path; `umount2(MNT_DETACH)` takes the subtree; `pivot_root(".", ".")` stacks
  the old root over the new one (`pivot_group`) for the detach that follows.
- An open file keeps the flags of the mount it was opened through, reported in
  `fstatfs` `f_flags` and passed on by `/proc/<pid>/fd/*` and `/proc/<pid>/exe`
  (now a magic link), which is what runc/crun's cloned-binary check needs.
- `statx` `STATX_ATTR_MOUNT_ROOT` and mount ids come from the walk, and
  descriptor paths are relative to the caller's root under `chroot`.

## Known gaps

- Detaching a mount namespace's own root is `EINVAL` (Linux allows it).
- No `MNT_LOCKED` for child mounts; no idmapped mounts
  (`mount_setattr` with `userns_fd` is `EOPNOTSUPP`).
- devpts instances share one pty numbering.
- No cgroup v1: `systemd-nspawn` running a tree without systemd needs
  `SYSTEMD_NSPAWN_UNIFIED_HIERARCHY=1`.
- ICMP datagram sockets are not implemented (`ping_group_range` only gates them).
- Robust futex lists skip priority-inheritance entries.
- Proven configurations only: podman with `--rootfs`, the `vfs` storage driver,
  `--network none` and `--cgroups disabled`; nspawn without `--private-users`.
  Rootless networking (pasta), overlay storage and the unprivileged Chromium
  sandbox are not tested.
