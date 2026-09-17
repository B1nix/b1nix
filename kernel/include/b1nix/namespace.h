#ifndef B1NIX_NAMESPACE_H
#define B1NIX_NAMESPACE_H

#include <b1nix/types.h>

struct task;
struct cred;
struct vfs_node;

/* Per-task namespaces (M109, completed in M123).
 *
 * Every Linux namespace kind exists: UTS, mount, PID, network, user, IPC,
 * cgroup and time. A namespace is a small id per kind (0 is the initial one)
 * with a reference count, an owning user namespace and an nsfs inode number.
 * The subsystems that hold per-namespace state (the mount table, the net layer,
 * the IPC tables, the cgroup tree) key it by that id.
 *
 * Membership is per TASK, the way Linux's nsproxy is: unshare(2) and setns(2)
 * move the calling thread, and a new thread or process starts in its creator's
 * namespaces. It lives in a side table indexed by the task's slot, because
 * struct task must not grow (see kernel/mm/eviction.c).
 *
 * A namespace lives while anything references it: a member task, a task that
 * will create its children in it (pid_for_children, time_for_children), an
 * open /proc/<pid>/ns handle or a mount of one, a child user or PID namespace,
 * or a namespace it owns. Releasing a mount or network namespace drops VFS and
 * socket state, which a reaper thread does outside the path that let go. */

/* Linux CLONE_NEW* bits. CLONE_NEWTIME shares its value with CSIGNAL's range,
 * so clone(2) cannot carry it: only clone3(2), unshare(2) and setns(2) can. */
#define B1NIX_CLONE_NEWTIME   0x00000080
#define B1NIX_CLONE_NEWNS     0x00020000
#define B1NIX_CLONE_NEWCGROUP 0x02000000
#define B1NIX_CLONE_NEWUTS    0x04000000
#define B1NIX_CLONE_NEWIPC    0x08000000
#define B1NIX_CLONE_NEWUSER   0x10000000
#define B1NIX_CLONE_NEWPID    0x20000000
#define B1NIX_CLONE_NEWNET    0x40000000
#define B1NIX_CLONE_NS_ALL                                                     \
  (B1NIX_CLONE_NEWTIME | B1NIX_CLONE_NEWNS | B1NIX_CLONE_NEWCGROUP |           \
   B1NIX_CLONE_NEWUTS | B1NIX_CLONE_NEWIPC | B1NIX_CLONE_NEWUSER |             \
   B1NIX_CLONE_NEWPID | B1NIX_CLONE_NEWNET)

enum ns_kind {
  NS_UTS = 0,
  NS_MNT = 1,
  NS_PID = 2,
  NS_NET = 3,
  NS_USER = 4,
  NS_IPC = 5,
  NS_CGROUP = 6,
  NS_TIME = 7,
  NS_KIND_COUNT
};

/* How many namespaces of each kind can exist at once, slot 0 being the initial
 * one. The subsystems holding per-namespace tables size them from these. */
#define NS_MAX_UTS 64
#define NS_MAX_MNT 128
#define NS_MAX_PID 64
#define NS_MAX_NET 16
#define NS_MAX_USER 64
#define NS_MAX_IPC 64
#define NS_MAX_CGROUP 64
#define NS_MAX_TIME 32

/* Nesting limit of user and PID namespaces, as Linux's MAX_USER_NS_LEVEL /
 * MAX_PID_NS_LEVEL. */
#define NS_MAX_LEVEL 32

/* "uts", "mnt", "pid", "net", "user", "ipc", "cgroup", "time" — the
 * /proc/<pid>/ns/ names. */
const char *namespace_kind_name(int kind);
int namespace_kind_from_name(const char *name);
/* The CLONE_NEW* bit of a kind, and back (-1 for anything else). */
u64 namespace_kind_flag(int kind);
int namespace_kind_from_flag(u64 flag);

/* Non-zero once any task has left an initial namespace. The VFS uses it to skip
 * per-lookup namespace resolution entirely on a normal boot. */
int namespace_active(void);

/* Namespace ids (0 = the initial namespace of that kind). */
u32 namespace_id_of(usize pid, int kind);
u32 namespace_task_id(const struct task *t, int kind);
u32 namespace_current_id(int kind);
/* The namespace a task's future children are created in: differs from its own
 * for PID and time namespaces after unshare(2)/setns(2). */
u32 namespace_task_children_id(const struct task *t, int kind);

/* The nsfs inode number of a namespace: what readlink("/proc/<pid>/ns/<kind>")
 * reports and what st_ino of an open handle is. Never reused. */
u64 namespace_inum(int kind, u32 id);
/* The user namespace that owns a namespace (for a user namespace: its parent). */
u32 namespace_owner(int kind, u32 id);
/* Take / drop a reference. namespace_get fails (-EINVAL) on a dead namespace. */
int namespace_get(int kind, u32 id);
void namespace_put(int kind, u32 id);

/* ── task lifetime (called from the scheduler) ───────────────────────────── */

/* clone(CLONE_NEW*): build the namespaces the caller's NEXT child is born into,
 * in the parent, before the fork — the child is runnable the moment the fork
 * returns, and a mount-table copy allocates and takes VFS locks. Checks the
 * flag combinations and privileges Linux checks. */
int namespace_fork_prepare(u64 clone_flags);
/* The fork failed; release what was prepared for a child that will not exist. */
void namespace_fork_prepare_abort(void);
/* May the caller create a task at all? A PID namespace whose init has exited
 * accepts no new members (ENOMEM, as Linux). */
int namespace_fork_allowed(void);
/* Stamp the child with its creator's namespaces (or the prepared ones). The
 * child's credentials must already exist: a new user namespace is recorded in
 * them. */
void namespace_fork_inherit(struct task *parent, struct task *child,
                            u64 clone_flags);
/* The task has exited: drop its namespace references. */
void namespace_task_exit(struct task *t);
/* The task's slot is being released (after its parent reaped it). */
void namespace_task_reaped(struct task *t);

/* unshare(2) / setns(2) for the calling task; both check privileges. `fd` for
 * setns is a /proc/<pid>/ns handle or a pidfd. */
int namespace_unshare(u64 flags);
int namespace_setns(int fd, int nstype);

/* ── nsfs ────────────────────────────────────────────────────────────────── */

/* A node standing for namespace (kind, id), holding a reference on it for as
 * long as the node lives. Returns a referenced node or an ERR_PTR. This is what
 * /proc/<pid>/ns/<kind> resolves to and what NS_GET_USERNS hands out. */
struct vfs_node *nsfs_node(int kind, u32 id);
/* Which namespace an nsfs node stands for. 0, or -EINVAL for any other node. */
int nsfs_node_ns(struct vfs_node *node, int *kind, u32 *id);
/* The NS_GET_* ioctls on an nsfs handle. */
int nsfs_ioctl(struct vfs_node *node, u64 request, u64 arg);

/* ── UTS ─────────────────────────────────────────────────────────────────── */

/* uname(2), /proc/sys/kernel/hostname and /sys/kernel/hostname read these, so
 * all of them see the caller's namespace. */
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
 * The first task numbered in a namespace is its init. Orphans inside the
 * namespace are reparented to it, it receives from inside its namespace only
 * the signals it has handlers for, and when it exits every other member is
 * killed and the namespace accepts no new task.
 *
 * Both directions return 0 for "no such task in this namespace", which the
 * syscall layer turns into ESRCH. With no namespaces in use both are a compare
 * and return of their argument. */
usize namespace_pid_to_user(usize kernel_pid);
usize namespace_pid_from_user(usize user_pid);
/* The same translations for a namespace named explicitly (a procfs instance,
 * an SCM_CREDENTIALS receiver). */
usize namespace_pid_to_ns(u32 pidns, usize kernel_pid);
usize namespace_pid_from_ns(u32 pidns, usize user_pid);
/* Is `kernel_pid` numbered in the caller's PID namespace at all? */
int namespace_pid_visible(usize kernel_pid);
int namespace_pid_visible_from(usize observer_pid, usize kernel_pid);
/* The nesting level of a PID namespace (0 for the initial one), and the numbers
 * a task has from its own namespace up to the initial one, innermost LAST — the
 * order of /proc/<pid>/status NSpid. Returns how many were written. */
u32 namespace_pid_level(u32 pidns);
int namespace_pid_chain(usize kernel_pid, u32 from_ns, usize *out, int max);
/* The kernel id of the init of `pidns` (0 for the initial namespace, whose init
 * is the machine's). */
usize namespace_pid_reaper(u32 pidns);
/* Where an orphan of `exiting` goes: the init of the orphan's PID namespace, or
 * 0 for "the machine's init". */
usize namespace_pid_orphan_reaper(usize orphan_pid);
/* A signal from the calling task to `target`: may it be delivered? False for
 * the init of the caller's own PID namespace when the signal has no handler, or
 * is SIGKILL/SIGSTOP (which only an ancestor namespace can send). */
int namespace_pid_signal_allowed(const struct task *target, int sig,
                                 int has_handler);

/* ── network namespace ────────────────────────────────────────────────────
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
/* Release the receive context of a task that has been reaped. */
void namespace_net_release(usize pid);
/* Does namespace `ns` exist (0 always does)? */
int namespace_net_live(u32 ns);

/* ── time namespace ───────────────────────────────────────────────────────
 *
 * A time namespace shifts CLOCK_MONOTONIC and CLOCK_BOOTTIME (and their _RAW,
 * _COARSE and _ALARM variants) by a fixed offset. CLOCK_REALTIME is global. */
#define TIMENS_CLOCK_MONOTONIC 1
#define TIMENS_CLOCK_BOOTTIME 7
/* The offsets of the caller's namespace, in nanoseconds (signed). */
i64 namespace_time_offset(int clock);
/* Offsets of the time namespace a task's children are born into, as written to
 * /proc/<pid>/timens_offsets. Settable only before any task has entered it. */
int namespace_time_offsets_render(const struct task *t, char *buf, usize len);
int namespace_time_offsets_write(struct task *t, const char *buf, usize len);

/* ── hooks the namespace core calls ───────────────────────────────────────── */

/* Mount namespaces (kernel/fs/vfs.c). */
int vfs_mnt_ns_clone(u32 from_ns, u32 to_ns);
void vfs_mnt_ns_destroy(u32 ns);
/* Network namespaces (kernel/net/net.c): destroy every interface, route and
 * neighbour belonging to `ns`. */
void net_ns_destroy(u32 ns);
/* IPC namespaces (kernel/ipc/): drop every SysV object and message queue. */
void ipc_ns_destroy(u32 ns);
/* cgroup namespaces (kernel/fs/cgroup/cgroup.c): the root of a new namespace is
 * the creator's cgroup; the core records and releases it through these. */
void *cgroup_ns_root_get(usize pid);
void cgroup_ns_root_put(void *root);
void *namespace_cgroup_root(u32 cgns);

#endif
