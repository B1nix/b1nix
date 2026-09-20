/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/uidgid.h>
#include <b1nix/mm.h>
#include <b1nix/namespace.h>
#include <b1nix/user_namespace.h>

/* ── Global user and group tables ── */
static struct user  users[MAX_USERS];
static int          user_count;

static struct group groups[MAX_GROUPS];
static int          group_count;

/* ── Initialization ── */

void uidgid_init(void)
{
    memset(users, 0, sizeof(users));
    memset(groups, 0, sizeof(groups));
    user_count = 0;
    group_count = 0;

    /* Create root user */
    user_add(ROOT_UID, ROOT_GID, "root");
    group_add(ROOT_GID, "root");
    group_add_member(ROOT_GID, ROOT_UID);

    /* Create default groups */
    group_add(5, "tty");
    group_add(6, "disk");
    group_add(7, "net");
    group_add(10, "wheel");
    group_add_member(10, ROOT_UID); /* Add root to wheel */

    /* Create default users */
    user_add(1, 1, "daemon");
    group_add(1, "daemon");
    group_add_member(1, 1);

    user_add(1000, 1000, "user");
    group_add(1000, "users");
    group_add_member(1000, 1000);

    console_write("uidgid: initialized (");
    console_write_dec(user_count);
    console_write(" users, ");
    console_write_dec(group_count);
    console_write(" groups)\n");
}

/* ── User management ── */

int user_add(u32 uid, u32 gid, const char *name)
{
    if (user_count >= MAX_USERS) return -1;
    if (user_find_by_uid(uid)) return -1;

    struct user *u = &users[user_count];
    u->uid = uid;
    u->gid = gid;
    usize len = strlen(name);
    if (len > 31) len = 31;
    memcpy(u->name, name, len);
    u->name[len] = '\0';
    user_count++;
    return 0;
}

const struct user *user_find_by_uid(u32 uid)
{
    for (int i = 0; i < user_count; i++) {
        if (users[i].uid == uid) return &users[i];
    }
    return 0;
}

const struct user *user_find_by_name(const char *name)
{
    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].name, name) == 0) return &users[i];
    }
    return 0;
}

/* ── Group management ── */

int group_add(u32 gid, const char *name)
{
    if (group_count >= MAX_GROUPS) return -1;
    if (group_find_by_gid(gid)) return -1;

    struct group *g = &groups[group_count];
    g->gid = gid;
    g->member_count = 0;
    usize len = strlen(name);
    if (len > 31) len = 31;
    memcpy(g->name, name, len);
    g->name[len] = '\0';
    group_count++;
    return 0;
}

int group_add_member(u32 gid, u32 uid)
{
    struct group *g = (struct group *)group_find_by_gid(gid);
    if (!g) return -1;
    if (g->member_count >= MAX_USERS) return -1;

    /* Check if already a member */
    for (int i = 0; i < g->member_count; i++) {
        if (g->members[i] == uid) return 0; /* Already a member */
    }

    g->members[g->member_count++] = uid;
    return 0;
}

const struct group *group_find_by_gid(u32 gid)
{
    for (int i = 0; i < group_count; i++) {
        if (groups[i].gid == gid) return &groups[i];
    }
    return 0;
}

/* ── Credential management ── */

struct cred *cred_create_default(void)
{
    struct cred *c = kzalloc(sizeof(struct cred));
    if (!c) return 0;

    /* Default to root */
    c->uid = ROOT_UID;
    c->euid = ROOT_UID;
    c->suid = ROOT_UID;
    c->gid = ROOT_GID;
    c->egid = ROOT_GID;
    c->sgid = ROOT_GID;
    c->ngroups = 0;
    c->umask = 0022;
    c->fsuid = ROOT_UID;
    c->fsgid = ROOT_GID;
    c->cap_bounding = CAP_FULL_SET;
    c->cap_effective = CAP_FULL_SET;
    c->cap_permitted = CAP_FULL_SET;
    c->cap_inheritable = 0;
    c->user_ns = 0;

    return c;
}

struct cred *cred_dup(const struct cred *src)
{
    if (!src) return cred_create_default();
    struct cred *c = kzalloc(sizeof(struct cred));
    if (!c) return 0;
    memcpy(c, src, sizeof(struct cred));
    /* The copy names the same user namespace, and holds its own reference. */
    if (c->user_ns && namespace_get(NS_USER, c->user_ns) != 0)
        c->user_ns = 0;
    return c;
}

void cred_free(struct cred *cred)
{
    if (!cred) return;
    namespace_put(NS_USER, cred->user_ns);
    kfree(cred);
}

/* The kernel uid that is root inside the credential's user namespace, or
 * UID_INVALID when nobody is. */
static u32 cred_root_kuid(const struct cred *c)
{
    return make_kuid(c->user_ns, 0);
}

static int priv_setuid(const struct cred *c)
{
    return ns_capable_cred(c, c->user_ns, CAP_SETUID);
}

static int priv_setgid(const struct cred *c)
{
    return ns_capable_cred(c, c->user_ns, CAP_SETGID);
}

/* Linux cap_emulate_setxuid(): the capability consequences of a uid change,
 * judged against root of the credential's own user namespace.
 *
 *  - leaving root in all of ruid, euid and suid drops permitted and effective
 *    (unless SECBIT_KEEP_CAPS) and always the ambient set;
 *  - the effective uid leaving root drops the effective set;
 *  - the effective uid becoming root restores effective from permitted. */
static void cred_fixup_setuid(struct cred *c, u32 old_ruid, u32 old_euid,
                              u32 old_suid)
{
    if (c->securebits & SECBIT(SECURE_NO_SETUID_FIXUP))
        return;
    u32 root = cred_root_kuid(c);
    int was_root = old_ruid == root || old_euid == root || old_suid == root;
    int now_root = c->uid == root || c->euid == root || c->suid == root;
    if (was_root && !now_root) {
        if (!(c->securebits & SECBIT(SECURE_KEEP_CAPS))) {
            c->cap_permitted = 0;
            c->cap_effective = 0;
        }
        c->cap_ambient = 0;
    }
    if (old_euid == root && c->euid != root)
        c->cap_effective = 0;
    if (old_euid != root && c->euid == root)
        c->cap_effective = c->cap_permitted;
}

/* Capabilities that only matter for file access. Linux clears these from the
 * effective set when fsuid moves away from root and restores them (from
 * permitted) when it comes back, so a server that lowers fsuid for one
 * operation really loses the ability to override file permissions during it. */
#define CAP_FS_MASK ((1ULL << CAP_CHOWN) | (1ULL << CAP_DAC_OVERRIDE) | \
                     (1ULL << CAP_DAC_READ_SEARCH) | (1ULL << CAP_FOWNER) | \
                     (1ULL << CAP_FSETID) | (1ULL << CAP_LINUX_IMMUTABLE) | \
                     (1ULL << CAP_MKNOD) | (1ULL << CAP_MAC_OVERRIDE))

static void cred_fixup_fsuid(struct cred *c, u32 old_fsuid)
{
    if (c->securebits & SECBIT(SECURE_NO_SETUID_FIXUP))
        return;
    u32 root = cred_root_kuid(c);
    if (old_fsuid == root && c->fsuid != root)
        c->cap_effective &= ~CAP_FS_MASK;
    if (old_fsuid != root && c->fsuid == root)
        c->cap_effective |= c->cap_permitted & CAP_FS_MASK;
}

void cred_sync_fsids(struct cred *cred)
{
    if (!cred) return;
    u32 old_fsuid = cred->fsuid;
    cred->fsuid = cred->euid;
    cred->fsgid = cred->egid;
    cred_fixup_fsuid(cred, old_fsuid);
}

/* Every uid below is a kernel id; the syscall layer translates from the
 * caller's user namespace and answers EINVAL for an unmapped one. */

/* setuid(2): with CAP_SETUID in the caller's user namespace, all of ruid, euid,
 * suid and fsuid; otherwise only the effective (and fs) uid, and only to the
 * real or saved one. */
int cred_set_uid(struct cred *cred, u32 uid)
{
    if (!cred) return -EINVAL;
    u32 or = cred->uid, oe = cred->euid, os = cred->suid;
    if (priv_setuid(cred)) {
        cred->uid = cred->suid = uid;
    } else if (uid != cred->uid && uid != cred->suid) {
        return -EPERM;
    }
    cred->euid = uid;
    cred_fixup_setuid(cred, or, oe, os);
    cred_sync_fsids(cred);
    return 0;
}

int cred_set_gid(struct cred *cred, u32 gid)
{
    if (!cred) return -EINVAL;
    if (priv_setgid(cred)) {
        cred->gid = cred->sgid = gid;
    } else if (gid != cred->gid && gid != cred->sgid) {
        return -EPERM;
    }
    cred->egid = gid;
    cred_sync_fsids(cred);
    return 0;
}

/* POSIX setreuid(): -1 leaves a field unchanged. Non-privileged callers may
 * set the real uid to {uid, euid} and the effective uid to {uid, euid, suid}.
 * If the real uid is set, or the effective uid is set to a value different
 * from the old real uid, the saved set-user-ID takes the new effective uid. */
int cred_setreuid(struct cred *cred, int ruid, int euid)
{
    if (!cred) return -EINVAL;
    int priv = priv_setuid(cred);
    u32 or = cred->uid, oe = cred->euid, os = cred->suid;
    if (ruid != -1 && !priv &&
        (u32)ruid != cred->uid && (u32)ruid != cred->euid)
        return -EPERM;
    if (euid != -1 && !priv &&
        (u32)euid != cred->uid && (u32)euid != cred->euid &&
        (u32)euid != cred->suid)
        return -EPERM;
    if (ruid != -1) cred->uid = (u32)ruid;
    if (euid != -1) cred->euid = (u32)euid;
    if (ruid != -1 || (euid != -1 && (u32)euid != or))
        cred->suid = cred->euid;
    cred_fixup_setuid(cred, or, oe, os);
    cred_sync_fsids(cred);
    return 0;
}

/* POSIX setregid() — mirror of cred_setreuid for the group ids. */
int cred_setregid(struct cred *cred, int rgid, int egid)
{
    if (!cred) return -EINVAL;
    int priv = priv_setgid(cred);
    u32 old_gid = cred->gid;
    if (rgid != -1 && !priv &&
        (u32)rgid != cred->gid && (u32)rgid != cred->egid)
        return -EPERM;
    if (egid != -1 && !priv &&
        (u32)egid != cred->gid && (u32)egid != cred->egid &&
        (u32)egid != cred->sgid)
        return -EPERM;
    if (rgid != -1) cred->gid = (u32)rgid;
    if (egid != -1) cred->egid = (u32)egid;
    if (rgid != -1 || (egid != -1 && (u32)egid != old_gid))
        cred->sgid = cred->egid;
    cred_sync_fsids(cred);
    return 0;
}

int cred_setresuid(struct cred *cred, int ruid, int euid, int suid)
{
    if (!cred) return -EINVAL;
    int priv = priv_setuid(cred);
    u32 or = cred->uid, oe = cred->euid, os = cred->suid;
    if (!priv) {
        if (ruid != -1 && (u32)ruid != cred->uid && (u32)ruid != cred->euid && (u32)ruid != cred->suid)
            return -EPERM;
        if (euid != -1 && (u32)euid != cred->uid && (u32)euid != cred->euid && (u32)euid != cred->suid)
            return -EPERM;
        if (suid != -1 && (u32)suid != cred->uid && (u32)suid != cred->euid && (u32)suid != cred->suid)
            return -EPERM;
    }
    if (ruid != -1) cred->uid = (u32)ruid;
    if (euid != -1) cred->euid = (u32)euid;
    if (suid != -1) cred->suid = (u32)suid;
    cred_fixup_setuid(cred, or, oe, os);
    cred_sync_fsids(cred);
    return 0;
}

int cred_setresgid(struct cred *cred, int rgid, int egid, int sgid)
{
    if (!cred) return -EINVAL;
    int priv = priv_setgid(cred);
    if (!priv) {
        if (rgid != -1 && (u32)rgid != cred->gid && (u32)rgid != cred->egid && (u32)rgid != cred->sgid)
            return -EPERM;
        if (egid != -1 && (u32)egid != cred->gid && (u32)egid != cred->egid && (u32)egid != cred->sgid)
            return -EPERM;
        if (sgid != -1 && (u32)sgid != cred->gid && (u32)sgid != cred->egid && (u32)sgid != cred->sgid)
            return -EPERM;
    }
    if (rgid != -1) cred->gid = (u32)rgid;
    if (egid != -1) cred->egid = (u32)egid;
    if (sgid != -1) cred->sgid = (u32)sgid;
    cred_sync_fsids(cred);
    return 0;
}

/* ── Permission checks ── */

int cred_can_access(const struct cred *cred, u32 file_uid, u32 file_gid, u16 file_mode, u32 access_mask)
{
    if (!cred) return 0;

    /* Filesystem access is checked against fsuid/fsgid, which normally mirror
     * euid/egid (see cred_sync_fsids). The mode bits only: what a capability
     * overrides is the caller's to decide (vfs_get_node_perm). */
    u16 perms;
    if (cred->fsuid == file_uid) {
        perms = (file_mode >> 6) & 7;
    } else {
        int in_group = cred->fsgid == file_gid;
        for (int i = 0; !in_group && i < cred->ngroups; i++)
            if (cred->groups[i] == file_gid)
                in_group = 1;
        perms = in_group ? ((file_mode >> 3) & 7) : (file_mode & 7);
    }
    return (perms & access_mask) == access_mask;
}

/* Recompute what the credential holds after its uids were rewritten wholesale
 * (a task created with a given identity): root of its user namespace holds
 * the bounding set, anyone else nothing (unless SECBIT_KEEP_CAPS). */
void cred_refresh_caps(struct cred *cred)
{
    if (!cred) return;
    if (cred->securebits & SECBIT(SECURE_NO_SETUID_FIXUP))
        return;
    int root = (cred->euid == cred_root_kuid(cred)) &&
               !(cred->securebits & SECBIT(SECURE_NOROOT));
    u64 want;
    if (root)
        want = CAP_FULL_SET;
    else if (cred->securebits & SECBIT(SECURE_KEEP_CAPS))
        want = cred->cap_permitted;
    else
        want = 0;
    cred->cap_permitted = want & cred->cap_bounding;
    cred->cap_effective = cred->cap_permitted;
    cred->cap_inheritable &= cred->cap_bounding;
}

/* Linux cap_bprm_creds_from_file() for a file with no file capabilities,
 * applied to `c` for an exec of a file with `file_mode`/`file_kuid`/
 * `file_kgid`. Set-user/group-ID applies only when `honour_setid` (no
 * no_new_privs, not a nosuid mount) and the file's owner and group both map
 * into the credential's user namespace. Returns whether the ids changed. */
int cred_exec_transform(struct cred *c, u16 file_mode, u32 file_kuid,
                        u32 file_kgid, int honour_setid)
{
    if (!c) return 0;
    u32 root = cred_root_kuid(c);
    int mapped = kuid_has_mapping(c->user_ns, file_kuid) &&
                 kgid_has_mapping(c->user_ns, file_kgid);
    if (honour_setid && mapped) {
        if (file_mode & 04000)
            c->euid = file_kuid;
        if ((file_mode & 02010) == 02010)
            c->egid = file_kgid;
    }
    int is_setid = c->euid != c->uid || c->egid != c->gid;

    u64 permitted = 0;
    int effective = 0;
    if (!(c->securebits & SECBIT(SECURE_NOROOT)) &&
        (c->euid == root || c->uid == root)) {
        permitted = c->cap_bounding | c->cap_inheritable;
        if (c->euid == root)
            effective = 1;
    }
    permitted &= c->cap_bounding;
    if (is_setid)
        c->cap_ambient = 0;
    permitted |= c->cap_ambient;

    c->cap_permitted = permitted;
    c->cap_effective = effective ? permitted : c->cap_ambient;
    c->suid = c->euid;
    c->sgid = c->egid;
    c->fsuid = c->euid;
    c->fsgid = c->egid;
    /* SECBIT_KEEP_CAPS does not survive an exec. */
    c->securebits &= ~SECBIT(SECURE_KEEP_CAPS);
    return is_setid;
}

u32 cred_get_securebits(const struct cred *cred)
{
    return cred ? cred->securebits : 0;
}

int cred_set_securebits(struct cred *cred, u32 bits)
{
    if (!cred) return -EINVAL;
    if (bits & ~(SECURE_ALL_BITS | SECURE_ALL_LOCKS))
        return -EINVAL;
    /* A lock, once set, can never be cleared. */
    if ((cred->securebits & SECURE_ALL_LOCKS) & ~bits)
        return -EPERM;
    /* Nor can the flag a lock guards be changed. */
    u32 changed = (cred->securebits ^ bits) & SECURE_ALL_BITS;
    if (changed & (cred->securebits >> 1) & SECURE_ALL_BITS)
        return -EPERM;
    if (!ns_capable_cred(cred, cred->user_ns, CAP_SETPCAP))
        return -EPERM;
    cred->securebits = bits;
    return 0;
}

/* setfsuid(2)/setfsgid(2): any of the task's own ids, or anything mapped with
 * CAP_SETUID/CAP_SETGID. The previous value is returned either way. */
u32 cred_set_fsuid(struct cred *cred, u32 fsuid)
{
    if (!cred) return 0;
    u32 prev = cred->fsuid;
    if (fsuid == cred->uid || fsuid == cred->euid || fsuid == cred->suid ||
        fsuid == cred->fsuid || priv_setuid(cred)) {
        cred->fsuid = fsuid;
        cred_fixup_fsuid(cred, prev);
    }
    return prev;
}

u32 cred_set_fsgid(struct cred *cred, u32 fsgid)
{
    if (!cred) return 0;
    u32 prev = cred->fsgid;
    if (fsgid == cred->gid || fsgid == cred->egid || fsgid == cred->sgid ||
        fsgid == cred->fsgid || priv_setgid(cred))
        cred->fsgid = fsgid;
    return prev;
}

int cred_capset(struct cred *cred, u64 eff, u64 perm, u64 inh)
{
    if (!cred) return -EINVAL;
    /* Capabilities can only be given up: the new permitted set must be a
     * subset of the old one, and effective a subset of permitted. */
    if (perm & ~cred->cap_permitted) return -EPERM;
    if (eff & ~perm) return -EPERM;
    if (inh & ~(cred->cap_inheritable | cred->cap_permitted)) return -EPERM;
    cred->cap_permitted = perm;
    cred->cap_effective = eff;
    cred->cap_inheritable = inh;
    /* A capability is ambient only while it is BOTH permitted and inheritable;
     * dropping it from either takes it out of the ambient set as well. */
    cred->cap_ambient &= (perm & inh);
    /* The bounding set is NOT touched here. On Linux only PR_CAPBSET_DROP
     * lowers it; capset(2) changes what the process holds, not the ceiling on
     * what it could ever hold. */
    return 0;
}

/* ── Capabilities ── */

/* capable(): the capability is in the EFFECTIVE set of a credential in the
 * initial user namespace. A task inside a user namespace holds nothing here —
 * what it may do to a resource a namespace owns is ns_capable_cred()'s answer. */
int cred_has_cap(const struct cred *cred, int cap)
{
    return ns_capable_cred(cred, 0, cap);
}

int cred_has_cap_effective(const struct cred *cred, int cap)
{
    if (!cred) return 0;
    if (cap < 0 || cap > CAP_LAST) return 0;
    return (cred->cap_effective & (1ULL << cap)) != 0;
}
