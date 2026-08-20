#ifndef B1NIX_NAMESPACE_H
#define B1NIX_NAMESPACE_H

#include <b1nix/types.h>

/* M109 — per-process namespaces.
 *
 * b1nix carries the namespaces a task belongs to in a side table keyed by task
 * id (struct task must not grow — see kernel/mm/eviction.c for the same
 * pattern). A task with no entry is in the initial namespace of every kind,
 * which is what every task is until something calls unshare(2), so the table
 * stays empty on a normal boot and the lookups below cost nothing.
 *
 * All four kinds are real: UTS (hostname/domainname), mount (a private copy of
 * the mount table), PID (a private numbering, so a task in a new namespace
 * sees its children counted from 1) and network (a private set of interfaces,
 * routes, neighbours and socket bindings).
 *
 * One deliberate difference from Linux: a namespace lives as long as a TASK is
 * in it, not as long as a descriptor names it. An open /proc/<pid>/ns handle
 * does not keep an otherwise-empty namespace alive, so setns(2) on a handle
 * whose last member has exited fails with EINVAL instead of resurrecting it. */

/* Linux CLONE_NEW* bits, as unshare(1)/nsenter(1) pass them. */
#define B1NIX_CLONE_NEWNS     0x00020000
#define B1NIX_CLONE_NEWCGROUP 0x02000000
#define B1NIX_CLONE_NEWUTS    0x04000000
#define B1NIX_CLONE_NEWIPC    0x08000000
#define B1NIX_CLONE_NEWUSER   0x10000000
#define B1NIX_CLONE_NEWPID    0x20000000
#define B1NIX_CLONE_NEWNET    0x40000000

enum ns_kind {
  NS_UTS = 0,
  NS_MNT = 1,
  NS_PID = 2,
  NS_NET = 3,
  NS_KIND_COUNT
};

/* "uts", "mnt", "pid", "net" — the /proc/<pid>/ns/ file names. */
const char *namespace_kind_name(int kind);
int namespace_kind_from_name(const char *name);

/* Non-zero once any task has left an initial namespace. The VFS uses it to
 * skip the per-lookup namespace resolution entirely on a normal boot. */
int namespace_active(void);

/* Namespace ids of `pid` (0 = the initial namespace of that kind). */
u32 namespace_id_of(usize pid, int kind);
u32 namespace_current_id(int kind);

/* Task lifetime hooks, called from the scheduler. */
void namespace_fork_inherit(usize parent_pid, usize child_pid);
void namespace_task_exit(usize pid);
/* Called when the task's slot is released, which is later than its exit: a
 * zombie still has to be nameable by the parent that is about to wait for it. */
void namespace_task_reaped(usize pid);

/* unshare(2) / setns(2) backends. Both act on the calling task. */
int namespace_unshare(u64 flags);
/* Join namespace `id` of kind `kind` — the pair a /proc/<pid>/ns/<kind>
 * descriptor pinned when it was opened. */
int namespace_setns(int kind, u32 id);

/* UTS namespace contents. kernel_hostname_get()/_set() in the syscall layer
 * route through these, so uname(2), /proc/sys/kernel/hostname and
 * /sys/kernel/hostname all see the caller's namespace. */
void namespace_uts_get_host(char *buf, usize len);
void namespace_uts_get_domain(char *buf, usize len);
int namespace_uts_set_host(const char *name);
int namespace_uts_set_domain(const char *name);

/* ── PID namespace ────────────────────────────────────────────────────────
 *
 * The kernel keeps one flat, global task-id space; a PID namespace is a
 * TRANSLATION over it. A task created into namespace N is given a number in N
 * (counting from 1) and in every namespace between N and the initial one, and
 * is invisible to any namespace that is not an ancestor of N.
 *
 * As on Linux, unshare(CLONE_NEWPID) does not move the caller: it sets the
 * namespace the caller's future CHILDREN are created in, so the first such
 * child is pid 1 there.
 *
 * Both directions return 0 for "no such task in this namespace", which the
 * syscall layer turns into ESRCH. On a normal boot (no namespaces) both are a
 * compare-and-return of their argument. */
usize namespace_pid_to_user(usize kernel_pid);
usize namespace_pid_from_user(usize user_pid);
/* Is `kernel_pid` numbered in the caller's PID namespace at all? Used by
 * /proc, which must not list a task the caller cannot name. */
int namespace_pid_visible(usize kernel_pid);
/* The same question asked on behalf of another task, for the /proc/<pid>
 * renderers that run in one task while describing another. */
int namespace_pid_visible_from(usize observer_pid, usize kernel_pid);

/* ── network namespace ────────────────────────────────────────────────────
 *
 * A network namespace owns the interfaces registered into it, plus the routing
 * entries, neighbour entries and socket bindings stamped with its id. The net
 * layer asks for one of two contexts:
 *
 *   namespace_net_current()  the calling task's namespace — the right answer
 *                            for a socket call, an ioctl or a netlink message.
 *   namespace_net_context()  the same, except inside a receive path, where it
 *                            is the namespace of the interface the frame
 *                            arrived on (net_deliver_frame pushes it).
 */
u32 namespace_net_current(void);
u32 namespace_net_context(void);
/* Push/pop the receive-side context. Returns the previous value. */
u32 namespace_net_push_context(u32 ns);
void namespace_net_pop_context(u32 saved);
/* Does namespace `ns` exist (0 always does)? */
int namespace_net_live(u32 ns);

/* Mount-namespace hooks provided by the VFS (kernel/fs/vfs.c). */
int vfs_mnt_ns_clone(u32 from_ns, u32 to_ns);
void vfs_mnt_ns_destroy(u32 ns);

/* Network-namespace teardown hook provided by the net layer (kernel/net/net.c):
 * destroy every interface, route and neighbour belonging to `ns`. */
void net_ns_destroy(u32 ns);

#endif
