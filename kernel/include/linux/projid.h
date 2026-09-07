/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_PROJID_H
#define LKPI_LINUX_PROJID_H

#include <linux/types.h>
#include <linux/uidgid.h>

/*
 * Project ids, the third quota class beside user and group.
 *
 * b1nix has no user namespaces, so a project id is the number itself and the
 * translation functions are the identity — the same bargain <linux/uidgid.h>
 * already makes for uids and gids. The type lives there, next to them; this
 * header is where the imported quota code looks for it.
 */

struct user_namespace;
extern struct user_namespace init_user_ns;

#define INVALID_PROJID ((kprojid_t){ (unsigned int)-1 })
#define OVERFLOW_PROJID 65534

static inline projid_t __kprojid_val(kprojid_t projid) { return projid.val; }

static inline bool projid_eq(kprojid_t left, kprojid_t right)
{ return __kprojid_val(left) == __kprojid_val(right); }

static inline bool projid_lt(kprojid_t left, kprojid_t right)
{ return __kprojid_val(left) < __kprojid_val(right); }

static inline bool projid_valid(kprojid_t projid)
{ return !projid_eq(projid, INVALID_PROJID); }

static inline kprojid_t make_kprojid(struct user_namespace *ns, projid_t projid)
{ (void)ns; return (kprojid_t){ projid }; }

static inline projid_t from_kprojid(struct user_namespace *ns, kprojid_t projid)
{ (void)ns; return __kprojid_val(projid); }

static inline projid_t from_kprojid_munged(struct user_namespace *ns,
                                           kprojid_t projid)
{
	projid_t v = from_kprojid(ns, projid);

	return v == (projid_t)-1 ? OVERFLOW_PROJID : v;
}

#endif /* LKPI_LINUX_PROJID_H */
