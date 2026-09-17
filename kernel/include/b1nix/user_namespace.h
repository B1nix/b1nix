#ifndef B1NIX_USER_NAMESPACE_H
#define B1NIX_USER_NAMESPACE_H

#include <b1nix/types.h>

struct cred;
struct task;

/* User namespaces (M123).
 *
 * The kernel keeps ONE id space: every uid and gid stored in a credential, an
 * inode or an IPC object is a kernel id (kuid/kgid). A user namespace is a
 * translation between that space and the ids a process inside it sees, given by
 * /proc/<pid>/uid_map and gid_map. The initial namespace (0) is the identity
 * over all of [0, 2^32-2].
 *
 * Every id crossing the syscall boundary is translated with the caller's
 * namespace: inbound with make_kuid/make_kgid (unmapped → EINVAL), outbound
 * with from_kuid_munged/from_kgid_munged (unmapped → the overflow id 65534).
 *
 * Capabilities are relative to a namespace. A task holds its capability sets
 * in its own user namespace; in a descendant namespace it holds every
 * capability if it owns that namespace (or an ancestor of it on the way down)
 * and nothing otherwise; in any other namespace, nothing. Every global
 * resource is guarded by the namespace that owns it: a mount namespace's owner
 * for mount(2), a network namespace's owner for interface configuration. */

#define UID_INVALID 0xFFFFFFFFu
#define GID_INVALID 0xFFFFFFFFu
#define UID_OVERFLOW 65534u
#define GID_OVERFLOW 65534u

/* Extents per map. Linux allows 340; the distributions' newuidmap writes one
 * line per /etc/subuid range, and a container runtime a handful. */
#define USERNS_MAP_EXTENTS 64

/* The user namespace a credential lives in. */
u32 cred_userns(const struct cred *c);

/* Namespace-local id → kernel id, UID_INVALID when unmapped. */
u32 make_kuid(u32 ns, u32 uid);
u32 make_kgid(u32 ns, u32 gid);
/* Kernel id → namespace-local id, UID_INVALID when unmapped. */
u32 from_kuid(u32 ns, u32 kuid);
u32 from_kgid(u32 ns, u32 kgid);
/* The same, reporting the overflow id for an unmapped kernel id. */
u32 from_kuid_munged(u32 ns, u32 kuid);
u32 from_kgid_munged(u32 ns, u32 kgid);
int kuid_has_mapping(u32 ns, u32 kuid);
int kgid_has_mapping(u32 ns, u32 kgid);

/* The same for the calling task's namespace. */
u32 current_make_kuid(u32 uid);
u32 current_make_kgid(u32 gid);
u32 current_from_kuid(u32 kuid);
u32 current_from_kgid(u32 kgid);

/* Is `ancestor` the namespace `ns` or one of its ancestors? */
int userns_is_ancestor(u32 ancestor, u32 ns);

/* Does `c` hold `cap` in user namespace `ns`? */
int ns_capable_cred(const struct cred *c, u32 ns, int cap);
/* The calling task. */
int ns_capable(u32 ns, int cap);
/* CAP_* over an inode: held in the caller's own namespace, and the inode's
 * owner and group both mapped there (Linux's capable_wrt_inode_uidgid). */
int capable_wrt_inode_uidgid(const struct cred *c, u32 kuid, u32 kgid, int cap);
/* The owner test chown/chmod/utimes/sticky directories use: the fsuid owns the
 * inode, or the caller has CAP_FOWNER over it. */
int cred_inode_owner_or_capable(const struct cred *c, u32 kuid, u32 kgid);

/* setgroups(2) is allowed in `ns` only once its gid_map is written and its
 * /proc/<pid>/setgroups did not say "deny". */
int userns_may_setgroups(u32 ns);

/* unshare(CLONE_NEWUSER): create a child of the credential's namespace owned by
 * its euid/egid. Returns the new id (holding one reference) or a negative
 * errno. */
int userns_create(const struct cred *creator);

/* /proc/<pid>/{uid_map,gid_map,projid_map,setgroups}. */
#define USERNS_UID_MAP 0
#define USERNS_GID_MAP 1
#define USERNS_PROJID_MAP 2
int userns_map_render(const struct task *target, int which, char *buf,
                      usize len);
isize userns_map_write(const struct task *target, int which, const char *buf,
                       usize len);
int userns_setgroups_render(const struct task *target, char *buf, usize len);
isize userns_setgroups_write(const struct task *target, const char *buf,
                             usize len);

/* The owner of a user namespace, as a kernel uid. */
u32 userns_owner_kuid(u32 ns);

#endif
