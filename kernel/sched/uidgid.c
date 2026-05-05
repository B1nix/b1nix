#include <string.h>
#include <b1nix/console.h>
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
    struct group *g = group_find_by_gid(gid);
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
    if (!cred) return -1;
    /* Only root can change real UID, or if we have CAP_SETUID */
    if (cred->euid != ROOT_UID && !cred_has_cap(cred, CAP_SETUID)) {
        /* Non-root can only set uid to one of: euid, suid, or uid */
        if (uid != cred->euid && uid != cred->suid && uid != cred->uid) {
            return -1;
        }
    }
    cred->uid = uid;
    return 0;
}

int cred_set_euid(struct cred *cred, u16 euid)
{
    if (!cred) return -1;
    if (cred->euid != ROOT_UID && !cred_has_cap(cred, CAP_SETUID)) {
        if (euid != cred->uid && euid != cred->suid && euid != cred->euid) {
            return -1;
        }
    }
    cred->euid = euid;
    return 0;
}

int cred_set_gid(struct cred *cred, u16 gid)
{
    if (!cred) return -1;
    if (cred->egid != ROOT_GID && !cred_has_cap(cred, CAP_SETGID)) {
        if (gid != cred->egid && gid != cred->sgid && gid != cred->gid) {
            return -1;
        }
    }
    cred->gid = gid;
    return 0;
}

int cred_set_egid(struct cred *cred, u16 egid)
{
    if (!cred) return -1;
    if (cred->egid != ROOT_GID && !cred_has_cap(cred, CAP_SETGID)) {
        if (egid != cred->gid && egid != cred->sgid && egid != cred->egid) {
            return -1;
        }
    }
    cred->egid = egid;
    return 0;
}

/* ── Permission checks ── */

int cred_can_access(const struct cred *cred, u16 file_uid, u16 file_gid, u16 file_mode, int write_access)
{
    if (!cred) return 0;

    /* Root can access everything */
    if (cred->euid == ROOT_UID) return 1;

    /* Check owner permissions */
    if (cred->euid == file_uid) {
        if (write_access) {
            return (file_mode & 0200) != 0; /* Owner write */
        } else {
            return (file_mode & 0400) != 0; /* Owner read */
        }
    }

    /* Check group permissions */
    if (cred->egid == file_gid) {
        if (write_access) {
            return (file_mode & 0020) != 0; /* Group write */
        } else {
            return (file_mode & 0040) != 0; /* Group read */
        }
    }

    /* Check supplementary groups */
    for (int i = 0; i < cred->ngroups; i++) {
        if (cred->groups[i] == file_gid) {
            if (write_access) {
                return (file_mode & 0020) != 0;
            } else {
                return (file_mode & 0040) != 0;
            }
        }
    }

    /* Check others permissions */
    if (write_access) {
        return (file_mode & 0002) != 0; /* Others write */
    } else {
        return (file_mode & 0004) != 0; /* Others read */
    }
}

/* ── Capabilities ── */

int cred_has_cap(const struct cred *cred, int cap)
{
    if (!cred) return 0;
    /* Root (euid == 0) implicitly has all capabilities */
    if (cred->euid == ROOT_UID) return 1;
    return 0;
}

int cred_has_cap_effective(const struct cred *cred, int cap)
{
    (void)cap;
    if (!cred) return 0;
    if (cred->euid == ROOT_UID) return 1;
    return 0;
}
