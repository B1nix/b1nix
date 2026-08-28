#include <string.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/uidgid.h>
#include <b1nix/mm.h>

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

int user_add(u16 uid, u16 gid, const char *name)
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

const struct user *user_find_by_uid(u16 uid)
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

int group_add(u16 gid, const char *name)
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

int group_add_member(u16 gid, u16 uid)
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

const struct group *group_find_by_gid(u16 gid)
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

    return c;
}

struct cred *cred_dup(const struct cred *src)
{
    if (!src) return cred_create_default();
    struct cred *c = kzalloc(sizeof(struct cred));
    if (!c) return 0;
    memcpy(c, src, sizeof(struct cred));
    return c;
}

void cred_free(struct cred *cred)
{
    if (cred) kfree(cred);
}

int cred_set_uid(struct cred *cred, u16 uid)
{
    if (!cred) return -EINVAL;
    /* Only root can change real UID, or if we have CAP_SETUID */
    int is_privileged = (cred->euid == ROOT_UID || cred_has_cap(cred, CAP_SETUID));
    if (!is_privileged) {
        /* Non-root can only set uid to one of: euid, suid, or uid */
        if (uid != cred->euid && uid != cred->suid && uid != cred->uid) {
            return -EPERM;
        }
    }
    cred->uid = uid;
    if (is_privileged) {
        cred->euid = uid;
        cred->suid = uid;
    }
    cred_refresh_caps(cred);
    cred_sync_fsids(cred);
    return 0;
}

int cred_set_euid(struct cred *cred, u16 euid)
{
    if (!cred) return -EINVAL;
    if (!cred_has_cap(cred, CAP_SETUID)) {
        if (euid != cred->uid && euid != cred->suid && euid != cred->euid) {
            return -EPERM;
        }
    }
    cred->euid = euid;
    cred_refresh_caps(cred);
    cred_sync_fsids(cred);
    return 0;
}

int cred_set_gid(struct cred *cred, u16 gid)
{
    if (!cred) return -EINVAL;
    int is_privileged = (cred->euid == ROOT_UID || cred_has_cap(cred, CAP_SETGID));
    if (!is_privileged) {
        if (gid != cred->egid && gid != cred->sgid && gid != cred->gid) {
            return -EPERM;
        }
    }
    cred->gid = gid;
    if (is_privileged) {
        cred->egid = gid;
        cred->sgid = gid;
    }
    cred_refresh_caps(cred);
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
    int priv = (cred->euid == ROOT_UID || cred_has_cap(cred, CAP_SETUID));
    u16 old_uid = cred->uid;
    if (ruid != -1 && !priv &&
        (u16)ruid != cred->uid && (u16)ruid != cred->euid)
        return -EPERM;
    if (euid != -1 && !priv &&
        (u16)euid != cred->uid && (u16)euid != cred->euid &&
        (u16)euid != cred->suid)
        return -EPERM;
    if (ruid != -1) cred->uid = (u16)ruid;
    if (euid != -1) cred->euid = (u16)euid;
    if (ruid != -1 || (euid != -1 && (u16)euid != old_uid))
        cred->suid = cred->euid;
    cred_refresh_caps(cred);
    cred_sync_fsids(cred);
    return 0;
}

/* POSIX setregid() — mirror of cred_setreuid for the group ids. */
int cred_setregid(struct cred *cred, int rgid, int egid)
{
    if (!cred) return -EINVAL;
    int priv = (cred->euid == ROOT_UID || cred_has_cap(cred, CAP_SETGID));
    u16 old_gid = cred->gid;
    if (rgid != -1 && !priv &&
        (u16)rgid != cred->gid && (u16)rgid != cred->egid)
        return -EPERM;
    if (egid != -1 && !priv &&
        (u16)egid != cred->gid && (u16)egid != cred->egid &&
        (u16)egid != cred->sgid)
        return -EPERM;
    if (rgid != -1) cred->gid = (u16)rgid;
    if (egid != -1) cred->egid = (u16)egid;
    if (rgid != -1 || (egid != -1 && (u16)egid != old_gid))
        cred->sgid = cred->egid;
    cred_refresh_caps(cred);
    cred_sync_fsids(cred);
    return 0;
}

int cred_setresuid(struct cred *cred, int ruid, int euid, int suid)
{
    if (!cred) return -EINVAL;
    int priv = (cred->euid == ROOT_UID || cred_has_cap(cred, CAP_SETUID));
    if (!priv) {
        if (ruid != -1 && (u16)ruid != cred->uid && (u16)ruid != cred->euid && (u16)ruid != cred->suid)
            return -EPERM;
        if (euid != -1 && (u16)euid != cred->uid && (u16)euid != cred->euid && (u16)euid != cred->suid)
            return -EPERM;
        if (suid != -1 && (u16)suid != cred->uid && (u16)suid != cred->euid && (u16)suid != cred->suid)
            return -EPERM;
    }
    if (ruid != -1) cred->uid = (u16)ruid;
    if (euid != -1) cred->euid = (u16)euid;
    if (suid != -1) cred->suid = (u16)suid;
    cred_refresh_caps(cred);
    cred_sync_fsids(cred);
    return 0;
}

int cred_setresgid(struct cred *cred, int rgid, int egid, int sgid)
{
    if (!cred) return -EINVAL;
    int priv = (cred->euid == ROOT_UID || cred_has_cap(cred, CAP_SETGID));
    if (!priv) {
        if (rgid != -1 && (u16)rgid != cred->gid && (u16)rgid != cred->egid && (u16)rgid != cred->sgid)
            return -EPERM;
        if (egid != -1 && (u16)egid != cred->gid && (u16)egid != cred->egid && (u16)egid != cred->sgid)
            return -EPERM;
        if (sgid != -1 && (u16)sgid != cred->gid && (u16)sgid != cred->egid && (u16)sgid != cred->sgid)
            return -EPERM;
    }
    if (rgid != -1) cred->gid = (u16)rgid;
    if (egid != -1) cred->egid = (u16)egid;
    if (sgid != -1) cred->sgid = (u16)sgid;
    cred_refresh_caps(cred);
    cred_sync_fsids(cred);
    return 0;
}

int cred_set_egid(struct cred *cred, u16 egid)
{
    if (!cred) return -EINVAL;
    int is_privileged = (cred->euid == ROOT_UID || cred_has_cap(cred, CAP_SETGID));
    if (!is_privileged) {
        if (egid != cred->gid && egid != cred->sgid && egid != cred->egid) {
            return -EPERM;
        }
    }
    cred->egid = egid;
    cred_refresh_caps(cred);
    cred_sync_fsids(cred);
    return 0;
}

/* ── Permission checks ── */

int cred_can_access(const struct cred *cred, u16 file_uid, u16 file_gid, u16 file_mode, u32 access_mask)
{
    if (!cred) return 0;

    /* Filesystem access is checked against fsuid/fsgid, which normally mirror
     * euid/egid (see cred_sync_fsids). */
    if (cred->fsuid == ROOT_UID) return 1;

    u16 perms = 0;
    /* Check owner permissions */
    if (cred->fsuid == file_uid) {
        perms = (file_mode >> 6) & 7;
    }
    /* Check group permissions */
    else if (cred->fsgid == file_gid) {
        perms = (file_mode >> 3) & 7;
    }
    /* Check supplementary groups */
    else {
        int in_group = 0;
        for (int i = 0; i < cred->ngroups; i++) {
            if (cred->groups[i] == file_gid) {
                in_group = 1;
                break;
            }
        }
        if (in_group) {
            perms = (file_mode >> 3) & 7;
        } else {
            /* Check others permissions */
            perms = file_mode & 7;
        }
    }

    return (perms & access_mask) == access_mask;
}


void cred_refresh_caps(struct cred *cred)
{
    if (!cred) return;
    /* SECBIT_NO_SETUID_FIXUP: the kernel makes no capability adjustment at all
     * when the effective uid changes. */
    if (cred->securebits & SECBIT(SECURE_NO_SETUID_FIXUP))
        return;

    /* SECBIT_NOROOT: uid 0 is not special, so becoming root grants nothing. */
    int root = (cred->euid == ROOT_UID) &&
               !(cred->securebits & SECBIT(SECURE_NOROOT));
    u64 want;
    if (root)
        want = CAP_FULL_SET;
    else if (cred->securebits & SECBIT(SECURE_KEEP_CAPS))
        /* SECBIT_KEEP_CAPS: leaving uid 0 keeps the permitted set instead of
         * clearing it. This is what a service that drops to an unprivileged
         * user but keeps one capability relies on. */
        want = cred->cap_permitted;
    else
        want = 0;
    cred->cap_permitted = want & cred->cap_bounding;
    cred->cap_effective = cred->cap_permitted;
    cred->cap_inheritable &= cred->cap_bounding;
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
    if (!cred_has_cap(cred, CAP_SETPCAP))
        return -EPERM;
    cred->securebits = bits;
    cred_refresh_caps(cred);
    return 0;
}

void cred_sync_fsids(struct cred *cred)
{
    if (!cred) return;
    cred->fsuid = cred->euid;
    cred->fsgid = cred->egid;
}

/* Capabilities that only matter for file access. Linux clears these when fsuid
 * moves away from 0 and restores them when it comes back, so a server that
 * lowers fsuid for one operation really loses the ability to override file
 * permissions during it. */
#define CAP_FS_MASK ((1ULL << CAP_CHOWN) | (1ULL << CAP_DAC_OVERRIDE) | \
                     (1ULL << CAP_DAC_READ_SEARCH) | (1ULL << CAP_FOWNER) | \
                     (1ULL << CAP_FSETID) | (1ULL << CAP_MKNOD))

u16 cred_set_fsuid(struct cred *cred, u16 fsuid)
{
    if (!cred) return 0;
    u16 prev = cred->fsuid;
    /* A task may set fsuid to any of its own UIDs, or to anything at all with
     * CAP_SETUID. An unpermitted value leaves fsuid unchanged — and, as Linux
     * documents, is reported only by the returned previous value. */
    if (fsuid == cred->uid || fsuid == cred->euid || fsuid == cred->suid ||
        fsuid == cred->fsuid || cred_has_cap(cred, CAP_SETUID))
        cred->fsuid = fsuid;
    if (prev == ROOT_UID && cred->fsuid != ROOT_UID) {
        cred->cap_effective &= ~CAP_FS_MASK;
        cred->cap_permitted &= ~CAP_FS_MASK;
    } else if (prev != ROOT_UID && cred->fsuid == ROOT_UID) {
        u64 restore = CAP_FS_MASK & cred->cap_bounding &
                      ((cred->euid == ROOT_UID) ? CAP_FULL_SET : 0);
        cred->cap_permitted |= restore;
        cred->cap_effective |= restore;
    }
    return prev;
}

u16 cred_set_fsgid(struct cred *cred, u16 fsgid)
{
    if (!cred) return 0;
    u16 prev = cred->fsgid;
    if (fsgid == cred->gid || fsgid == cred->egid || fsgid == cred->sgid ||
        fsgid == cred->fsgid || cred_has_cap(cred, CAP_SETGID))
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
     * dropping it from either takes it out of the ambient set as well. Linux
     * enforces this invariant on every change, and it is the whole reason the
     * ambient set is safe: it can never carry something the process has just
     * given up. */
    cred->cap_ambient &= (perm & inh);
    /* The bounding set is NOT touched here. On Linux only PR_CAPBSET_DROP
     * lowers it; capset(2) changes what the process holds, not the ceiling on
     * what it could ever hold. Shrinking it here made a library that raises a
     * capability, does its work and puts the sets back (libcap's cap_set_proc,
     * which systemd uses around every bounding-set change) permanently narrow
     * the ceiling as a side effect. */
    return 0;
}

/* ── Capabilities ── */

/* A capability is held when it is in the PERMITTED set. Root's cred starts
 * with the full set, so the historical "euid == 0 means everything" behaviour
 * still holds — but a task can now drop capabilities and the kernel honours
 * the reduced set, which is what makes capset(2) meaningful. */
int cred_has_cap(const struct cred *cred, int cap)
{
    if (!cred) return 0;
    if (cap < 0 || cap > CAP_LAST) return 0;
    return (cred->cap_permitted & (1ULL << cap)) != 0;
}

int cred_has_cap_effective(const struct cred *cred, int cap)
{
    if (!cred) return 0;
    if (cap < 0 || cap > CAP_LAST) return 0;
    return (cred->cap_effective & (1ULL << cap)) != 0;
}
