/*
 * SPDX-License-Identifier: MIT
 *
 * M101 linuxkpi: kobject and runtime PM.
 *
 * The only subtlety is the ordering on the last put: the child's release runs
 * before the parent's reference is dropped. A release that walks up to its
 * parent — which is what a driver's teardown does — would otherwise be reading
 * memory that had already been freed underneath it.
 */

#include <b1nix/errno.h>
#include <b1nix/netlink.h>
#include <b1nix/sysfs_attr.h>
#include <lkpi/device.h>
#include <stdio.h>
#include <string.h>

static void kobject_release_chain(struct kref *ref)
{
	struct kobject *kobj = kref_container_of(ref, struct kobject, ref);
	struct kobject *parent = kobj->parent;

	/* The object's own release first, while its parent is still alive. */
	if (kobj->release)
		kobj->release(kobj);

	/* Then let go of the parent, which may in turn be the last reference. */
	if (parent)
		kobject_put(parent);
}

void kobject_init_and_add(struct kobject *kobj, const char *name,
                          struct kobject *parent, kobject_release_t release)
{
	if (!kobj)
		return;
	kobj->name = name;
	kobj->release = release;
	kobj->parent = parent;
	kref_init(&kobj->ref);
	/* A child holds its parent: the parent's memory must outlive it. */
	if (parent)
		kobject_get(parent);
}

struct kobject *kobject_get(struct kobject *kobj)
{
	if (kobj)
		kref_get(&kobj->ref);
	return kobj;
}

int kobject_put(struct kobject *kobj)
{
	if (!kobj)
		return 0;
	return kref_put(&kobj->ref, kobject_release_chain);
}

u32 kobject_depth(const struct kobject *kobj)
{
	u32 depth = 0;
	for (const struct kobject *k = kobj; k; k = k->parent)
		depth++;
	return depth;
}

/* ── runtime PM ─────────────────────────────────────────────────── */

void lkpi_device_init(struct lkpi_device *dev, const char *name,
                      const struct lkpi_pm_ops *pm)
{
	if (!dev)
		return;
	kobject_init_and_add(&dev->kobj, name, 0, 0);
	lkpi_mutex_init(&dev->lock);
	dev->pm = pm;
	dev->usage = 0;
	/* Starts resumed and PM-disabled: a device is powered when its driver
	 * finds it, and only becomes suspendable once the driver says so. */
	dev->suspended = 0;
	dev->pm_enabled = 0;
	dev->suspend_count = 0;
	dev->resume_count = 0;
	dev->driver_data = 0;
}

void lkpi_pm_runtime_enable(struct lkpi_device *dev)
{
	if (!dev)
		return;
	lkpi_mutex_lock(&dev->lock);
	dev->pm_enabled = 1;
	lkpi_mutex_unlock(&dev->lock);
}

void lkpi_pm_runtime_disable(struct lkpi_device *dev)
{
	if (!dev)
		return;
	lkpi_mutex_lock(&dev->lock);
	dev->pm_enabled = 0;
	lkpi_mutex_unlock(&dev->lock);
}

int lkpi_pm_runtime_get_sync(struct lkpi_device *dev)
{
	if (!dev)
		return -EINVAL;

	lkpi_mutex_lock(&dev->lock);
	dev->usage++;
	if (!dev->suspended) {
		lkpi_mutex_unlock(&dev->lock);
		return 0;
	}

	int err = 0;
	if (dev->pm && dev->pm->runtime_resume)
		err = dev->pm->runtime_resume(dev);
	if (err) {
		/* No hardware: undo the reference so the caller's failed get does
		 * not pin a device that never came back. */
		dev->usage--;
		lkpi_mutex_unlock(&dev->lock);
		return err;
	}
	dev->suspended = 0;
	dev->resume_count++;
	lkpi_mutex_unlock(&dev->lock);
	return 0;
}

int lkpi_pm_runtime_put_sync(struct lkpi_device *dev)
{
	if (!dev)
		return -EINVAL;

	lkpi_mutex_lock(&dev->lock);
	if (dev->usage > 0)
		dev->usage--;
	if (dev->usage > 0 || !dev->pm_enabled || dev->suspended) {
		lkpi_mutex_unlock(&dev->lock);
		return 0;
	}

	int err = 0;
	if (dev->pm && dev->pm->runtime_suspend)
		err = dev->pm->runtime_suspend(dev);
	if (err) {
		/* A driver that refused to power down has not powered down; saying
		 * otherwise would let the next get skip the resume. */
		lkpi_mutex_unlock(&dev->lock);
		return err;
	}
	dev->suspended = 1;
	dev->suspend_count++;
	lkpi_mutex_unlock(&dev->lock);
	return 0;
}

i32 lkpi_pm_runtime_usage(struct lkpi_device *dev)
{
	if (!dev)
		return 0;
	lkpi_mutex_lock(&dev->lock);
	i32 usage = dev->usage;
	lkpi_mutex_unlock(&dev->lock);
	return usage;
}

int lkpi_pm_runtime_suspended(struct lkpi_device *dev)
{
	if (!dev)
		return 0;
	lkpi_mutex_lock(&dev->lock);
	int suspended = dev->suspended != 0;
	lkpi_mutex_unlock(&dev->lock);
	return suspended;
}

/* ── uevents ────────────────────────────────────────────────────── */

static volatile u64 g_uevents;

static const char *kobject_action_name(enum kobject_action action)
{
	switch (action) {
	case KOBJ_ADD:     return "add";
	case KOBJ_REMOVE:  return "remove";
	case KOBJ_CHANGE:  return "change";
	case KOBJ_ONLINE:  return "online";
	case KOBJ_OFFLINE: return "offline";
	}
	return "change";
}

/*
 * Format and broadcast one uevent.
 *
 * The layout is the kernel's own and userspace depends on every part of it: a
 * summary line "action@devpath", then NUL-separated key=value properties. mdev
 * matches on SUBSYSTEM before it looks at anything else, and udev refuses a
 * message with no SEQNUM, so neither is optional.
 *
 * DEVPATH is the /sys path with the mount point stripped — userspace prepends
 * its own /sys, so leaving it on produces /sys/sys/... and every lookup fails.
 */
int kobject_uevent_env(struct kobject *kobj, enum kobject_action action,
                       char *envp[])
{
	static volatile u64 g_seqnum;

	__atomic_fetch_add(&g_uevents, 1ull, __ATOMIC_RELAXED);
	if (!kobj || !kobj->sysfs)
		return 0; /* nothing published: there is no devpath to announce */

	char path[192];
	isize plen = sysfs_reg_path((struct sysfs_dir *)kobj->sysfs, path,
	                            sizeof(path));
	if (plen < 0)
		return (int)plen;

	const char *devpath = path;
	if (strncmp(devpath, "/sys", 4) == 0)
		devpath += 4;

	const char *act = kobject_action_name(action);
	const char *subsystem = kobj->subsystem ? kobj->subsystem : "unknown";
	u64 seq = __atomic_add_fetch(&g_seqnum, 1ull, __ATOMIC_ACQ_REL);

	char msg[512];
	usize pos = 0;
	int n = snprintf(msg, sizeof(msg), "%s@%s", act, devpath);
	if (n < 0)
		return -EINVAL;
	pos = (usize)n + 1; /* the summary line is NUL-terminated in the payload */

	struct {
		const char *fmt;
		const char *s;
		u64 v;
	} props[] = {
		{ "ACTION=%s",    act,       0 },
		{ "DEVPATH=%s",   devpath,   0 },
		{ "SUBSYSTEM=%s", subsystem, 0 },
		{ "SEQNUM=%lu",   0,         seq },
	};
	for (usize i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
		if (pos >= sizeof(msg))
			break;
		int w = props[i].s
		            ? snprintf(msg + pos, sizeof(msg) - pos, props[i].fmt,
		                       props[i].s)
		            : snprintf(msg + pos, sizeof(msg) - pos, props[i].fmt,
		                       (unsigned long)props[i].v);
		if (w < 0)
			break;
		pos += (usize)w + 1;
	}
	if (pos > sizeof(msg))
		pos = sizeof(msg);

	/* Properties the caller added itself, appended verbatim — a driver puts
	 * the ones its helper scripts match on here. */
	if (envp) {
		for (usize i = 0; envp[i] && pos < sizeof(msg); i++) {
			usize len = strlen(envp[i]);
			if (pos + len + 1 > sizeof(msg))
				break;
			memcpy(msg + pos, envp[i], len + 1);
			pos += len + 1;
		}
	}

	netlink_uevent_broadcast(msg, pos);
	return 0;
}

int kobject_uevent(struct kobject *kobj, enum kobject_action action)
{
	return kobject_uevent_env(kobj, action, 0);
}

u64 kobject_uevents_raised(void)
{
	return __atomic_load_n(&g_uevents, __ATOMIC_ACQUIRE);
}
