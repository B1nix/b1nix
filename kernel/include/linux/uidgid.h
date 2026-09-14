/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_UIDGID_H
#define LKPI_LINUX_UIDGID_H

#include <linux/types.h>

/*
 * User and group ids as the kernel holds them.
 *
 * They are structs rather than integers for one reason, and it is worth keeping
 * even though b1nix has a single user namespace: a `kuid_t` cannot be assigned
 * to or compared with a raw uid by accident. Every conversion goes through
 * `from_kuid`/`make_kuid`, which is where a namespace mapping would live — and
 * where its absence is visible, rather than being spread across every
 * filesystem that stores a uid on disk.
 */

typedef struct { uid_t val; } kuid_t;
typedef struct { gid_t val; } kgid_t;
/* A project id, for quota accounting by project rather than by user. Same
 * struct-wrapper reasoning as above. */
typedef struct { unsigned int val; } kprojid_t;

struct user_namespace;
extern struct user_namespace init_user_ns;

#define KUIDT_INIT(value) (kuid_t){ (uid_t)(value) }
#define KGIDT_INIT(value) (kgid_t){ (gid_t)(value) }

#define GLOBAL_ROOT_UID KUIDT_INIT(0)
#define GLOBAL_ROOT_GID KGIDT_INIT(0)
#define INVALID_UID     KUIDT_INIT(-1)
#define INVALID_GID     KGIDT_INIT(-1)

static inline uid_t __kuid_val(kuid_t uid) { return uid.val; }
static inline gid_t __kgid_val(kgid_t gid) { return gid.val; }

static inline bool uid_eq(kuid_t a, kuid_t b) { return a.val == b.val; }
static inline bool gid_eq(kgid_t a, kgid_t b) { return a.val == b.val; }
static inline bool uid_lt(kuid_t a, kuid_t b) { return a.val < b.val; }
static inline bool gid_lt(kgid_t a, kgid_t b) { return a.val < b.val; }
static inline bool uid_valid(kuid_t uid) { return uid.val != (uid_t)-1; }
static inline bool gid_valid(kgid_t gid) { return gid.val != (gid_t)-1; }

/* One namespace, so every mapping is the identity. The functions exist so the
 * conversion points are named; if namespaces ever arrive, this is the one place
 * that changes. */
static inline kuid_t make_kuid(struct user_namespace *from, uid_t uid)
{ (void)from; return KUIDT_INIT(uid); }
static inline kgid_t make_kgid(struct user_namespace *from, gid_t gid)
{ (void)from; return KGIDT_INIT(gid); }
static inline uid_t from_kuid(struct user_namespace *to, kuid_t kuid)
{ (void)to; return kuid.val; }
static inline gid_t from_kgid(struct user_namespace *to, kgid_t kgid)
{ (void)to; return kgid.val; }
/* The `_munged` forms return the overflow id rather than -1 for an unmappable
 * value, because their callers are reporting an id to userspace and (uid_t)-1
 * is a legal id there. */
static inline uid_t from_kuid_munged(struct user_namespace *to, kuid_t kuid)
{ (void)to; return kuid.val; }
static inline gid_t from_kgid_munged(struct user_namespace *to, kgid_t kgid)
{ (void)to; return kgid.val; }
static inline bool kuid_has_mapping(struct user_namespace *ns, kuid_t uid)
{ (void)ns; (void)uid; return true; }
static inline bool kgid_has_mapping(struct user_namespace *ns, kgid_t gid)
{ (void)ns; (void)gid; return true; }

#endif
