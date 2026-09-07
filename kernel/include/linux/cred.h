/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CRED_H
#define LKPI_LINUX_CRED_H

#include <linux/types.h>
#include <linux/uidgid.h>

/*
 * The credentials a filesystem checks against.
 *
 * `current_fsuid`/`current_fsgid` are the ones that matter: a filesystem stamps
 * a new inode with them, and it must be the FS uid rather than the real or
 * effective one — they differ exactly when a process has called setfsuid, which
 * exists so that a server can act as a client for filesystem access without
 * becoming it for signals.
 *
 * `override_creds`/`revert_creds` are used by btrfs around its own worker
 * threads. b1nix has no credential override, so they are structural: the
 * override returns the current credentials and the revert takes them back,
 * which is a correct no-op rather than a dropped privilege change — nothing
 * here ever hands them a DIFFERENT set to install.
 */

struct cred {
	kuid_t uid, euid, suid, fsuid;
	kgid_t gid, egid, sgid, fsgid;
	struct user_namespace *user_ns;
};

const struct cred *current_cred(void);
kuid_t current_fsuid_val(void);
kgid_t current_fsgid_val(void);

#define current_fsuid() current_fsuid_val()
#define current_fsgid() current_fsgid_val()

static inline const struct cred *get_current_cred(void) { return current_cred(); }
static inline const struct cred *get_cred(const struct cred *cred)
{ return cred; }
static inline void put_cred(const struct cred *cred) { (void)cred; }
static inline const struct cred *override_creds(const struct cred *new)
{ (void)new; return current_cred(); }
static inline void revert_creds(const struct cred *old) { (void)old; }
static inline struct cred *prepare_creds(void) { return NULL; }

#endif
