/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_KOBJECT_H
#define LKPI_LINUX_KOBJECT_H
/* kobject is declared alongside the device it is embedded in — the lifetime
 * rule only makes sense with both in view, so they share a header. */
#include <lkpi/device.h>
#include <linux/sysfs.h>

/* The sysfs operations a plain kobject's attributes go through. One shared
 * instance, because every kobj_attribute dispatches the same way — the
 * attribute carries the behaviour, not the kobject. */
extern const struct sysfs_ops kobj_sysfs_ops;


/* struct kobj_type, kobject_init_and_add() and the two-step kobject_init() /
 * kobject_add() split are declared in <lkpi/device.h>, included above: the type
 * and the lifetime rule belong with the object they are about. */

/*
 * The rest of the kobject interface both filesystems use for their /sys trees.
 *
 * `ktype` is a member rather than an argument because the release function
 * lives on it: a kobject's last put has to know how to free the object it is
 * embedded in, and only its type knows that.
 *
 * b1nix's sysfs is generated rather than registered into, so `kobject_add` and
 * friends record the object without publishing a directory. The reference
 * counting is real, though — btrfs's per-device and per-space-info objects have
 * their lifetimes managed through it, and a put that did not free would leak
 * one per unmount.
 */
/* The lifetime calls themselves — kobject_init, kobject_add, kobject_get,
 * kobject_put, kobject_uevent — are declared in <lkpi/device.h> above. Only
 * what is missing from there is added here. */
void kobject_del(struct kobject *kobj);
int kobject_rename(struct kobject *kobj, const char *new_name);
const char *kobject_name(const struct kobject *kobj);

/*
 * A set of kobjects that share a directory and a uevent behaviour.
 *
 * btrfs creates one for /sys/fs/btrfs and hangs a kobject per mounted
 * filesystem off it. The embedded kobject is the set's own directory — which is
 * why `kset_create_and_add` returns a kset and `kobject_add` takes
 * `&kset->kobj` as the parent.
 */
struct kset {
	struct list_head list;
	struct kobject kobj;
	const struct kset_uevent_ops *uevent_ops;
};

struct kset_uevent_ops {
	int (*filter)(const struct kobject *kobj);
	const char *(*name)(const struct kobject *kobj);
	int (*uevent)(const struct kobject *kobj, struct kobj_uevent_env *env);
};

struct kobj_uevent_env {
	char *envp[64];
	int envp_idx;
	char buf[2048];
	int buflen;
};

struct kset *kset_create_and_add(const char *name,
                                 const struct kset_uevent_ops *uevent_ops,
                                 struct kobject *parent_kobj);
void kset_unregister(struct kset *kset);
int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...);

#endif
