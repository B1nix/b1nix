/* SPDX-License-Identifier: MIT */
#ifndef B1NIX_SYSFS_ATTR_H
#define B1NIX_SYSFS_ATTR_H

#include <b1nix/types.h>

/*
 * Attribute files registered at runtime, under /sys and /sys/kernel/debug.
 *
 * b1nix's /sys is otherwise a tree built once at mount from lookup callbacks
 * (kernel/fs/sysfs.c). That is the right shape for facts the kernel always
 * knows — the CPU list, the block topology — and the wrong shape for a driver
 * that appears later and wants to publish its own files. A DRM device does
 * exactly that: registering it creates /sys/class/drm/card0 with attributes
 * whose values are read out of the driver when userspace opens them.
 *
 * So this is a registry rather than a tree. Directories and attributes may be
 * registered before /sys is mounted, in which case they are materialised when
 * it is; afterwards they appear immediately. Callers never need to know which
 * side of the mount they are on, because a driver's probe order and the mount
 * order have no reason to agree.
 *
 * Reads and writes go straight to the caller's callback — nothing is cached, so
 * a value that changes between two reads is two different answers, which is what
 * a status file is for.
 */

struct sysfs_dir;
struct vfs_node;

/*
 * Show renders into `buf` (at most `cap` bytes) and returns the length, or a
 * negative errno. Store consumes `len` bytes and returns the number accepted,
 * or a negative errno; a NULL store makes the file read-only regardless of the
 * mode bits, because a writable file that silently discards writes is worse
 * than one that refuses them.
 */
typedef isize (*sysfs_attr_show)(void *ctx, char *buf, usize cap);
typedef isize (*sysfs_attr_store)(void *ctx, const char *buf, usize len);
/* Called once when the attribute goes away — individually or with its
 * directory. The registry owns nothing behind `ctx`, so whoever allocated it
 * says here how to release it; without this an attribute that is removed leaks
 * its context, which is the common case for a driver that unbinds. */
typedef void (*sysfs_attr_release)(void *ctx);

/*
 * A read that is given the offset instead of being served a window of a fully
 * rendered value. Most attributes want the simple form — one page, rendered
 * whole — but a debugfs dump is not one page, and re-rendering it per read only
 * to discard the first N bytes is both slower and wrong when the value moves
 * between reads. A file registers one form or the other, never both.
 */
typedef isize (*sysfs_attr_read_at)(void *ctx, char *buf, usize cap,
                                    u64 offset);

/* A directory under /sys. `parent` NULL means directly under the root. Naming
 * an existing directory returns it rather than creating a second one, so two
 * subsystems can both publish under /sys/class without racing to create it. */
struct sysfs_dir *sysfs_reg_dir(struct sysfs_dir *parent, const char *name);

/* Look a directory up without creating it. NULL if absent. */
struct sysfs_dir *sysfs_reg_find(struct sysfs_dir *parent, const char *name);

/* An attribute file inside `dir`. Returns 0, or a negative errno. */
int sysfs_reg_attr(struct sysfs_dir *dir, const char *name, u16 mode,
                   sysfs_attr_show show, sysfs_attr_store store, void *ctx,
                   sysfs_attr_release release);

/* The offset-aware form. Same file, different read contract. */
int sysfs_reg_attr_at(struct sysfs_dir *dir, const char *name, u16 mode,
                      sysfs_attr_read_at read_at, sysfs_attr_store store,
                      void *ctx, sysfs_attr_release release);

/* Remove one attribute by name. Returns 0, or -ENOENT. */
int sysfs_reg_attr_remove(struct sysfs_dir *dir, const char *name);
/* Remove one symbolic link by name. Returns 0, or -ENOENT. */
int sysfs_reg_link_remove(struct sysfs_dir *dir, const char *name);

/* A symbolic link inside `dir` pointing at `target` (an absolute /sys path).
 * /sys is full of these — a class entry links to the device that backs it. */
int sysfs_reg_link(struct sysfs_dir *dir, const char *name, const char *target);

/* Remove a directory, its attributes and everything below it. */
void sysfs_reg_remove(struct sysfs_dir *dir);

/* The absolute path of `dir`, e.g. "/sys/class/drm/card0". Returns the length
 * written, or a negative errno. Symlink targets need it, and a caller that
 * built the directory does not otherwise know where it ended up. */
isize sysfs_reg_path(struct sysfs_dir *dir, char *buf, usize cap);

/* The directory `dir` sits in, or NULL for a top-level one. */
struct sysfs_dir *sysfs_reg_parent(struct sysfs_dir *dir);

/* The debugfs root, /sys/kernel/debug. Created on first use. */
struct sysfs_dir *sysfs_reg_debug_root(void);

/* M101: read the registered files back through the VFS, by path. */
void sysfs_attr_selftest(void);

/* Called by the sysfs mount callback with the freshly built root, to
 * materialise whatever was registered before the mount. */
void sysfs_reg_attach_root(struct vfs_node *sys_root);

#endif
