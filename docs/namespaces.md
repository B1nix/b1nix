# Namespaces (M109)

b1nix implements all four kinds `unshare(1)` and `nsenter(1)` ask for: UTS,
mount, PID and network. This document is the implementation note; the roadmap
carries the one-line summary.

## Where the state lives

`kernel/sched/namespace.c` keeps a side table keyed by task id
(`struct ns_row`). **`struct task` must not grow** — an M29 LAPIC page-table
invariant depends on its size, and there are static asserts on two of its field
offsets in `kernel/sched/scheduler.c`. A task with no row is in the initial
namespace of every kind, which is every task until something unshares, so a
normal boot allocates no rows at all and `namespace_active()` keeps every fast
path free of a lookup.

Namespace lifetime differs from Linux in one deliberate way: a namespace lives
as long as a *task* is in it, not as long as a descriptor names it. An open
`/proc/<pid>/ns` handle does not keep an otherwise-empty namespace alive, so
`setns(2)` on a handle whose last member has exited fails with `EINVAL` instead
of resurrecting it. Collection happens on the next `unshare`/`setns`, never on
the exit path — tearing a mount namespace down means dropping VFS references,
which must not happen while a task is already unwinding.

## PID

The kernel numbers tasks once, globally (`next_task_id` in
`kernel/sched/scheduler.c`); a PID namespace is a **translation** over that
numbering, not a second one. `struct pid_ns` pairs the number a namespace uses
(`vnr`) with the kernel's own id, and a task is entered into its own namespace
*and every namespace between that one and the initial one* — an ancestor must
be able to name, signal and wait for a task inside a namespace it created. A
namespace that is not an ancestor has no entry and therefore no way to name the
task. That absence is the isolation.

The translation is applied at the **syscall boundary**
(`ns_pid_in`/`ns_pid_out` in `kernel/syscall/syscall.c`), not in the scheduler,
which would otherwise have to carry a namespace alongside every id. Covered:
`fork`, `clone`, `wait`, `waitpid`, `waitid`, `getpid`, `gettid`, `getppid`,
`getpgrp`, `getpgid`, `getsid`, `setpgid`, `kill`, `tkill`, `tgkill`,
`sigqueue`, `rt_tgsigqueueinfo`, `setpriority`, `getpriority`,
`set_tid_address` and `prlimit64`, on both the native and the Linux-ABI paths.
A number that names nothing in the caller's namespace is `ESRCH` (or `ECHILD`
for the wait family).

As on Linux, `unshare(CLONE_NEWPID)` does **not** move the caller: it records
the namespace the caller's future children are born into, which is why the
first such child is pid 1 there and why `unshare -p` needs a fork before
anything is numbered from 1. `setns(CLONE_NEWPID)` behaves the same way, and
may only target the caller's own namespace or one descended from it.

`/proc` reports the PID namespace it was **mounted** in — again exactly Linux,
where `unshare -p` without `--mount-proc` leaves `/proc` showing the parent's
numbering. b1nix mounts `/proc` once, in the initial namespace, so it shows
global ids; `/proc/self` still resolves to the caller.

## Network

Interfaces (`struct netdev::netns`), routes (`struct route_entry::ns`),
neighbours (`struct arp_entry::ns`) and socket bindings
(`struct vfs_socket_state::netns`) each carry the id of the namespace they
belong to. Everything userspace can reach filters on it: `netdev_by_index`,
`netdev_index_by_name`, the netlink link/addr/route/neigh dumps, the `SIOCGIF*`
ioctls, `/proc/net/dev`, `/proc/net/route` and AF_PACKET delivery. Because
`netdev_by_index` is the chokepoint, an ifindex belonging to another namespace
resolves to nothing rather than to a device the caller merely cannot see.

Two contexts answer "which namespace is this?" (`kernel/sched/namespace.c`):

* `namespace_net_current()` — the calling task's, for a socket call, an ioctl
  or a netlink message.
* `namespace_net_context()` — the same, except inside a receive path, where it
  is the namespace of the interface the frame arrived on.
  `net_deliver_frame()` and `net_poll()` push it, the same way
  `g_receiving_netdev` is already pushed.

A socket keeps the namespace it was created in for life, as on Linux: joining
another namespace re-points what a task creates *next*, never what it already
holds open.

One known limitation: the receive-side context is a single global, not a
per-CPU value — the same shape as `g_receiving_netdev`, which it sits beside.
While a frame for a non-initial namespace is in flight on one CPU, another
CPU's lookup can briefly resolve in that namespace instead of its own. The
consequence is a dropped packet, not a misdelivered one (a lookup filtered on
the wrong namespace finds nothing), and the window exists only while
namespaced traffic is being delivered. Making it per-CPU is the fix if this
ever matters.

### veth

`kernel/net/veth.c`. A two-ended cable made of software: what one end
transmits, the other receives, byte for byte. It exists because a namespace is
otherwise sealed — the pair is created in one namespace and one end moved into
another with `ip link set <dev> netns <pid>` (rtnetlink `IFLA_NET_NS_PID` /
`IFLA_NET_NS_FD`, handled by `nl_do_netns`), leaving a single link that crosses
the boundary. The peer receives through `net_deliver_frame()` rather than its
own `->transmit`, so the frame enters the receive path (packet sockets, bridge
hand-off, protocol demux) and is not sent a second time.

Moving an interface is refused (`EBUSY`) for the device carrying the initial
namespace's L3 configuration, and for anything stacked — a bridge port, a
VLAN's lower device or a bond slave would forward frames across the boundary,
which is the one thing the boundary is for.

### What is not there

A namespace has **no private IPv4 configuration**. `kernel/net/net.c` holds one
`local_ip`/`gateway_ip`/`netmask`, and `ipv4_send_tx` stamps every packet's
source address from it; UDP demultiplexing matches on the destination port, and
TCP connections are keyed without an interface. So a namespaced interface
carries frames — AF_PACKET, veth, and the routes that select an output
interface — but not addresses, and there is no `ping` between two namespaces.
Making that work means threading a namespace through the address state in
`net.c` and through `ipv4.c`/`udp.c`/`tcp.c`; the roadmap carries it as the one
remaining `planned` line.

## Coverage

`userspace/bin/smoke/m109_smoke.c`, checked in `tests/smoke.sh`:

| marker | what it proves |
|---|---|
| `uts-namespace`, `ns-handles`, `setns-uts` | a private hostname, and `setns` in both directions |
| `mount-namespace` | a mount made under `CLONE_NEWNS` is absent outside it |
| `pid-namespace` | the first child reports `getpid()==1` and `getppid()==0` itself; its own child gets 2; `waitpid` inside reports that 2 |
| `pid-ns-isolation` | a pid from outside names nothing inside — `kill()` is `ESRCH` |
| `pid-ns-handles` | `/proc/<pid>/ns/pid` differs inside and is *unchanged* for the task that unshared |
| `veth-pair`, `veth-carries-frame` | two interfaces, both in `/proc/net/dev`; a frame sent on one arrives on the other as a *received* frame |
| `net-namespace` | a fresh namespace has no interfaces at all |
| `veth-crosses-namespace` | an end moved away vanishes here and receives there |
| `net-ns-routes` | a route added inside is in its own `/proc/net/route` and not in the initial namespace's |
