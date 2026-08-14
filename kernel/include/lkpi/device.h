/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_DEVICE_H
#define LKPI_DEVICE_H

#include <lkpi/kref.h>
#include <lkpi/lock.h>
#include <lkpi/types.h>

/*
 * kobject and runtime power management.
 *
 * kobject is what a driver's object graph is made of: a name, a refcount and a
 * parent. Its value here is not the sysfs plumbing Linux hangs off it — that is
 * deliberately absent, see below — but the lifetime rule, which drivers rely on
 * constantly: an object stays alive while anything holds it, and its release
 * runs exactly once, on the last put, with the parent released after the child.
 *
 * The kobject carries the device's /sys directory once it is registered
 * (`sysfs`, opaque here: it is a `struct sysfs_dir *` from
 * <b1nix/sysfs_attr.h>, and this header must not reach across the boundary to
 * name it). The DRM core import is what settled the question this comment used
 * to leave open — it registers a class, adds devices to it and publishes
 * attribute groups off them, so those exist now, and no more than those.
 *
 * pm_runtime is the usage-counted suspend that a GPU spends most of its life
 * in. The whole contract is: the device is resumed while at least one caller
 * holds it, and suspends when the last one lets go. Getting the counting wrong
 * does not fail loudly — it leaves hardware powered down while a driver pokes
 * its registers, so the count and the callbacks are both asserted by the
 * self-test rather than assumed.
 */

struct kobject;

typedef void (*kobject_release_t)(struct kobject *kobj);

struct kobject {
	const char *name;
	struct kref ref;
	struct kobject *parent;
	kobject_release_t release;
	/* The /sys directory this object publishes into, once it has one. Opaque
	 * (`struct sysfs_dir *`): imported code hands a kobject to the attribute
	 * calls, and this is what turns it back into a place in the tree. */
	void *sysfs;
	/* The class this object belongs to, as userspace names it — SUBSYSTEM= in
	 * a uevent, and the only property a hotplug helper matches on before it
	 * has looked at anything else. Set when the device is added. */
	const char *subsystem;
};

/*
 * The type a kobject is created with. Defined here rather than in
 * <linux/kobject.h> because kobject_init_and_add() below takes one and this is
 * the header that declares it; the Linux-named header includes this one.
 */
struct sysfs_ops;
struct attribute_group;
struct kobj_type {
	void (*release)(struct kobject *kobj);
	const struct sysfs_ops *sysfs_ops;
	const struct attribute_group **default_groups;
};

/* Initialise with one reference and take one on the parent, so a child cannot
 * outlive its parent's memory. */
void lkpi_kobject_init_and_add(struct kobject *kobj, const char *name,
                          struct kobject *parent, kobject_release_t release);

/* Upstream's spelling: the release comes from the type, and the name from a
 * format string. The formatted name is allocated and owned by the kobject. */
int kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype,
                         struct kobject *parent, const char *fmt, ...);
void kobject_init(struct kobject *kobj, const struct kobj_type *ktype);
int kobject_add(struct kobject *kobj, struct kobject *parent, const char *fmt, ...);

struct kobject *kobject_get(struct kobject *kobj);
/* Returns 1 if this put released the object. */
int kobject_put(struct kobject *kobj);

/* Depth from the root, counting this object. 1 for a parentless kobject. */
u32 kobject_depth(const struct kobject *kobj);

/*
 * Announce a change in a kobject's state to userspace.
 *
 * Delivered over NETLINK_KOBJECT_UEVENT, in the form every hotplug helper
 * parses: a summary line "add@/devices/…", then NUL-separated properties —
 * ACTION, DEVPATH, SUBSYSTEM and a monotonic SEQNUM. A kobject with no /sys
 * directory has no DEVPATH to announce, so nothing is sent for it; the counter
 * still moves, which is what separates "no listener" from "never raised".
 *
 */
enum kobject_action {
	KOBJ_ADD,
	KOBJ_REMOVE,
	KOBJ_CHANGE,
	KOBJ_ONLINE,
	KOBJ_OFFLINE,
};

int kobject_uevent(struct kobject *kobj, enum kobject_action action);
int kobject_uevent_env(struct kobject *kobj, enum kobject_action action,
                       char *envp[]);
/* Uevents raised so far. Diagnostics: it is the only observable effect. */
u64 kobject_uevents_raised(void);

/* ── runtime PM ─────────────────────────────────────────────────── */

struct lkpi_device;

struct lkpi_pm_ops {
	/* Return 0 on success. A failing resume leaves the device suspended and
	 * the usage count unchanged, so the caller learns it has no hardware. */
	int (*runtime_suspend)(struct lkpi_device *dev);
	int (*runtime_resume)(struct lkpi_device *dev);
};

struct lkpi_device {
	struct kobject kobj;
	const struct lkpi_pm_ops *pm;
	struct lkpi_mutex lock;
	i32 usage;             /* callers currently holding it resumed */
	u32 suspended;         /* 1 while the device is powered down */
	u32 pm_enabled;
	u64 suspend_count;     /* transitions, for the self-test */
	u64 resume_count;
	void *driver_data;
	/* The device's directory under /sys, or NULL until it is registered.
	 * Opaque: only kernel/lkpi/sysfs.c and the registry behind it look in. */
	void *sysfs;
};

void lkpi_device_init(struct lkpi_device *dev, const char *name,
                      const struct lkpi_pm_ops *pm);

/* Runtime PM starts disabled, as in Linux: a driver enables it once the device
 * is in a state it is willing to have suspended underneath it. */
void lkpi_pm_runtime_enable(struct lkpi_device *dev);
void lkpi_pm_runtime_disable(struct lkpi_device *dev);

/*
 * Take a usage reference, resuming the device if this is the first one. Returns
 * 0 once the device is usable, or the resume callback's error. Sleeps (it takes
 * the device mutex and may call into the driver).
 */
int lkpi_pm_runtime_get_sync(struct lkpi_device *dev);

/*
 * Drop a usage reference, suspending the device if that was the last one.
 * Returns 0, or the suspend callback's error — in which case the device stays
 * resumed, because a driver that cannot power down has not powered down.
 */
int lkpi_pm_runtime_put_sync(struct lkpi_device *dev);

/* Current usage count and state, for diagnostics and the self-test. */
i32 lkpi_pm_runtime_usage(struct lkpi_device *dev);
int lkpi_pm_runtime_suspended(struct lkpi_device *dev);


#endif
