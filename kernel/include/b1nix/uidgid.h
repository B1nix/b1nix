#ifndef B1NIX_UIDGID_H
#define B1NIX_UIDGID_H

#include <b1nix/types.h>

/* ── Default users/groups ── */
#define ROOT_UID 0
#define ROOT_GID 0

#define MAX_USERS  64
#define MAX_GROUPS 32

/* ── User structure ── */
struct user {
    u16  uid;
    u16  gid;       /* Primary group */
    char name[32];
};

/* ── Group structure ── */
struct group {
    u16  gid;
    char name[32];
    u16  members[MAX_USERS];
    int  member_count;
};

/* ── Credentials (attached to each task) ── */
struct cred {
    u16  uid;       /* Real user ID */
    u16  euid;      /* Effective user ID (for permission checks) */
    u16  suid;      /* Saved set-user-ID */
    u16  gid;       /* Real group ID */
    u16  egid;      /* Effective group ID */
    u16  sgid;      /* Saved set-group-ID */
    u16  groups[MAX_GROUPS]; /* Supplementary groups */
    int  ngroups;
    u16  umask;
    /* Filesystem IDs. POSIX checks file access against these rather than the
     * effective IDs; they track euid/egid unless setfsuid(2)/setfsgid(2) moves
     * them, which is how a server drops filesystem privilege for one operation
     * while keeping the effective ID it needs for signals. */
    u16  fsuid;
    u16  fsgid;
    /* Capability sets. A root task starts with everything; a task can drop
     * capabilities it holds (capset), and dropping one is irreversible without
     * CAP_SETPCAP. Bit N corresponds to CAP_* value N. */
    u64  cap_effective;
    u64  cap_permitted;
    u64  cap_inheritable;
    /* Bounding set: the ceiling a task can ever hold. capset(2) lowers it and
     * nothing raises it, so a dropped capability stays dropped even across a
     * return to euid 0 (which otherwise re-grants the full set). */
    u64  cap_bounding;
    /* Ambient set (Linux 4.3): the capabilities a non-setuid execve keeps.
     * A capability may only be ambient while it is BOTH permitted and
     * inheritable, and losing either drops it from here too.
     *
     * It is not an optional refinement. systemd applies an ambient set before
     * every process it spawns, and `prctl(PR_CAP_AMBIENT, ...)` answering
     * EINVAL is reported as "Failed to apply the starting ambient set" -- a
     * fatal error for the spawn, so with no ambient set at all not one unit
     * on the machine can be started. */
    u64  cap_ambient;
    /* securebits(7): how the kernel adjusts capabilities across a uid change.
     * Zero is Linux's default and what this kernel did unconditionally before
     * the field existed. systemd reads them for every service it starts —
     * PR_GET_SECUREBITS reporting -EINVAL made it conclude the bits differed
     * from what it wanted, call PR_SET_SECUREBITS, get EINVAL from that too,
     * and fail the service at step SECUREBITS (status 213). */
    u32  securebits;
};

/* securebits(7) bit numbers, as Linux's <linux/securebits.h>. Each flag has a
 * LOCKED companion one bit above it: once the lock is set the flag can never
 * change again, not even by a process holding CAP_SETPCAP. */
#define SECURE_NOROOT                   0
#define SECURE_NOROOT_LOCKED            1
#define SECURE_NO_SETUID_FIXUP          2
#define SECURE_NO_SETUID_FIXUP_LOCKED   3
#define SECURE_KEEP_CAPS                4
#define SECURE_KEEP_CAPS_LOCKED         5
#define SECURE_NO_CAP_AMBIENT_RAISE     6
#define SECURE_NO_CAP_AMBIENT_RAISE_LOCKED 7
#define SECBIT(n) (1u << (n))
#define SECURE_ALL_BITS                                                        \
  (SECBIT(SECURE_NOROOT) | SECBIT(SECURE_NO_SETUID_FIXUP) |                    \
   SECBIT(SECURE_KEEP_CAPS) | SECBIT(SECURE_NO_CAP_AMBIENT_RAISE))
#define SECURE_ALL_LOCKS (SECURE_ALL_BITS << 1)

/* Every capability b1nix defines (CAP_CHOWN..CAP_AUDIT_CONTROL and the few
 * above them) — the set a root task holds. */
#define CAP_LAST 36
#define CAP_FULL_SET ((CAP_LAST >= 63) ? ~0ULL : ((1ULL << (CAP_LAST + 1)) - 1))

/* Recompute the capability sets after a UID change: a root task holds
 * everything within its bounding set, a non-root task holds nothing. */
void cred_refresh_caps(struct cred *cred);
/* Keep fsuid/fsgid in step with euid/egid (the default POSIX behaviour). */
void cred_sync_fsids(struct cred *cred);
/* setfsuid(2)/setfsgid(2): return the PREVIOUS value, changed or not. */
u16 cred_set_fsuid(struct cred *cred, u16 fsuid);
u16 cred_set_fsgid(struct cred *cred, u16 fsgid);
/* capset(2): install the three sets, bounded by what the task already holds. */
int cred_capset(struct cred *cred, u64 eff, u64 perm, u64 inh);

/* ── Ring / Privilege level ── */
enum ring_level {
    RING_KERNEL = 0,
    RING_SERVICE = 1,
    RING_USER = 3,
};

/* ── Capabilities (for fine-grained privilege control) ── */
#define CAP_CHOWN           0
#define CAP_DAC_OVERRIDE    1
#define CAP_DAC_READ_SEARCH 2
#define CAP_FOWNER          3
#define CAP_FSETID          4
#define CAP_KILL            5
#define CAP_SETGID          6
#define CAP_SETUID          7
#define CAP_SETPCAP         8
#define CAP_NET_BIND_SERVICE 9
#define CAP_NET_BROADCAST   10
#define CAP_NET_ADMIN       11
#define CAP_NET_RAW         12
#define CAP_IPC_LOCK        13
#define CAP_IPC_OWNER       14
#define CAP_SYS_MODULE      15
#define CAP_SYS_RAWIO       16
#define CAP_SYS_CHROOT      17
#define CAP_SYS_PTRACE      18
#define CAP_SYS_PACCT       19
#define CAP_SYS_ADMIN       20
#define CAP_SYS_BOOT        21
#define CAP_SYS_NICE        22
#define CAP_SYS_RESOURCE    23
#define CAP_SYS_TIME        24
#define CAP_SYS_TTY_CONFIG  25
#define CAP_MKNOD           26
#define CAP_LEASE           27
#define CAP_AUDIT_WRITE     28
#define CAP_AUDIT_CONTROL   29
#define CAP_SETFCAP         30
#define CAP_MAC_OVERRIDE    31
#define CAP_MAC_ADMIN       32
#define CAP_SYSLOG          33
#define CAP_WAKE_ALARM      34
#define CAP_BLOCK_SUSPEND   35
#define CAP_AUDIT_READ      36
#define CAP_LAST_CAP        36

#define CAP_MAX CAP_LAST_CAP

/* ── Capability set ── */
struct cap_data {
    u32 permitted;      /* Permitted capabilities */
    u32 inheritable;    /* Inheritable capabilities */
    u32 effective;      /* Effective capabilities */
};

/* ── API ── */

void uidgid_init(void);

/* User management */
int  user_add(u16 uid, u16 gid, const char *name);
const struct user *user_find_by_uid(u16 uid);
const struct user *user_find_by_name(const char *name);

/* Group management */
int  group_add(u16 gid, const char *name);
int  group_add_member(u16 gid, u16 uid);
const struct group *group_find_by_gid(u16 gid);

/* Credential management for tasks */
struct cred *cred_create_default(void);
struct cred *cred_dup(const struct cred *src);
void         cred_free(struct cred *cred);

int  cred_set_uid(struct cred *cred, u16 uid);
int  cred_set_euid(struct cred *cred, u16 euid);
int  cred_set_gid(struct cred *cred, u16 gid);
int  cred_set_egid(struct cred *cred, u16 egid);
int  cred_setreuid(struct cred *cred, int ruid, int euid);
int  cred_setregid(struct cred *cred, int rgid, int egid);
int  cred_setresuid(struct cred *cred, int ruid, int euid, int suid);
int  cred_setresgid(struct cred *cred, int rgid, int egid, int sgid);

/* Permission checks */
int  cred_can_access(const struct cred *cred, u16 file_uid, u16 file_gid, u16 file_mode, u32 access_mask);
int  cred_has_cap(const struct cred *cred, int cap);
int  cred_has_cap_effective(const struct cred *cred, int cap);

/* securebits(7). The setter enforces the LOCKED bits and requires CAP_SETPCAP,
 * exactly as Linux does, and the flags it stores really change what
 * cred_refresh_caps does on the next uid transition. */
int  cred_set_securebits(struct cred *cred, u32 bits);
u32  cred_get_securebits(const struct cred *cred);

#endif /* B1NIX_UIDGID_H */
