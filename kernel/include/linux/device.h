/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_DEVICE_H
#define LKPI_LINUX_DEVICE_H

#include <linux/kdev_t.h>
#include <linux/sysfs.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <lkpi/device.h>

/*
 * The device model, as much of it as imported source touches.
 *
 * `struct device` is what a driver is handed and what it hangs everything off:
 * a name, a parent, its private pointer, and — the part that carries real
 * weight — devres, the list of allocations that are released automatically when
 * the device goes away. A driver's error paths depend on that: it allocates a
 * dozen things during probe and returns an error from the middle, and the
 * unwinding is devres's job, not a goto ladder's. Faking devres by making
 * devm_kzalloc a plain kmalloc compiles and then leaks every failed probe.
 *
 * Runtime PM lives in the embedded lkpi_device; <linux/pm_runtime.h> is the
 * Linux-named front for it.
 */

struct device;
struct class;
struct device_type;

/* A driver bound to devices on a bus. b1nix has no bus matching for imported
 * drivers yet — the first vendor driver is instantiated directly — so this
 * carries the fields the core reads and nothing walks a list of them. */
struct device_driver {
	const char *name;
	struct module *owner;
	const void *of_match_table;
};

typedef void (*devres_release_t)(struct device *dev, void *res);

struct devres_node {
	struct devres_node *next;
	devres_release_t release;
	usize size;
	/* The caller's memory follows this header, so devm_kfree can find the node
	 * from the pointer it was given by stepping back one header. */
};

struct device {
	/* Imported code reaches for dev->kobj to hang sysfs entries off. It is the
	 * kobject inside the embedded lkpi_device, exposed under the name Linux
	 * uses so those references resolve to one object rather than two. */
	struct kobject kobj;
	const char *init_name;
	/* The device number userspace sees for this device's node. */
	dev_t devt;
	/* The class this device belongs to, which decides where it appears in
	 * /sys. Assigned at registration; nothing consumes it yet. */
	const struct class *class;
	const struct device_type *type;
	/* Attribute groups published for this device. Assigned at registration;
	 * nothing publishes them yet. */
	const struct attribute_group **groups;
	void (*release)(struct device *dev);
	struct device *parent;
	struct device_driver *driver;
	void *driver_data;
	struct lkpi_device lk;      /* kobject + runtime PM */
	struct devres_node *devres; /* most recent first: released in reverse */
	struct lkpi_mutex devres_lock;
};

/* Linux's form takes only the device — the name is assigned separately, into
 * init_name, before the device is added. */
void device_initialize(struct device *dev);
void device_set_name(struct device *dev, const char *name);

/* What the driver core calls a device in a message when no driver is bound:
 * the bus name on Linux, the device's own name here. */
static inline const char *dev_driver_string(const struct device *dev);

/* A class of devices sharing release behaviour. Nothing groups devices that
 * way here yet; the type exists for the core's declarations. */
struct device_type {
	const char *name;
	void (*release)(struct device *dev);
};

#define kobj_to_dev(k) container_of(k, struct device, kobj)

static inline const char *dev_name(const struct device *dev)
{
	return (dev && dev->init_name) ? dev->init_name : "(unnamed)";
}

static inline void *dev_get_drvdata(const struct device *dev)
{
	return dev ? dev->driver_data : 0;
}

/* Set the device's name. Linux's takes a format string; the DRM core passes a
 * plain one plus arguments, and the result is stored for dev_name to return. */
int dev_set_name(struct device *dev, const char *fmt, ...);

static inline void dev_set_drvdata(struct device *dev, void *data)
{
	if (dev)
		dev->driver_data = data;
}

/*
 * Managed allocation. Freed in reverse order of allocation when the device is
 * released, or explicitly with devm_kfree.
 */
static inline const char *dev_driver_string(const struct device *dev)
{
	return (dev && dev->driver && dev->driver->name) ? dev->driver->name
	                                                 : "device";
}

/* Device references. The count lives in the embedded kobject, so a device
 * outlives every holder rather than the last put winning a race. */
static inline struct device *get_device(struct device *dev)
{
	if (dev)
		kobject_get(&dev->lk.kobj);
	return dev;
}

static inline void put_device(struct device *dev)
{
	if (dev)
		kobject_put(&dev->lk.kobj);
}

/*
 * Registering a device. This is what makes it visible: its directory appears
 * under its class in /sys, the attribute groups the driver declared are
 * published, and a `dev` file carries the device number in the major:minor form
 * hotplug tooling parses. Implemented in kernel/lkpi/sysfs.c.
 */
int device_add(struct device *dev);
void device_del(struct device *dev);
int device_register(struct device *dev);
void device_unregister(struct device *dev);
static inline int device_is_registered(struct device *dev)
{ return dev && dev->lk.sysfs != 0; }

/* A class of devices, which is a directory under /sys/class that devices join
 * when they are added. */
struct class {
	const char *name;
	/* What a device's node should be called under /dev. Assigned by the core
	 * at registration; b1nix has no devtmpfs, so nothing consumes it. */
	char *(*devnode)(const struct device *dev, umode_t *mode);
	/* The class's directory, an opaque `struct sysfs_dir *`. */
	void *sysfs;
};

/* Returns an ERR_PTR on failure, as Linux's does — callers IS_ERR() it. */
struct class *class_create(const char *name);
void class_destroy(struct class *c);
int class_create_file(struct class *c, const struct attribute *attr);
void class_remove_file(struct class *c, const struct attribute *attr);

void *devm_kmalloc(struct device *dev, usize size, gfp_t flags);
void *devm_kzalloc(struct device *dev, usize size, gfp_t flags);
void *devm_kcalloc(struct device *dev, usize n, usize size, gfp_t flags);
void devm_kfree(struct device *dev, void *ptr);

/* Register an arbitrary cleanup to run with the rest. */
int devm_add_action(struct device *dev, void (*action)(void *), void *data);
int devm_add_action_or_reset(struct device *dev, void (*action)(void *),
                             void *data);

/* Run every registered release, newest first, and empty the list. */
void devres_release_all(struct device *dev);

/* Outstanding managed allocations. Diagnostics and self-test. */
usize devres_count(struct device *dev);

/* The level-prefixed form imported code calls directly. The level is passed
 * through into the message rather than acted on: b1nix's console has no
 * per-level filtering to hand it to. */
#define dev_printk(level, dev, fmt, ...) \
	lkpi_printk("drm %s: " fmt, dev_name(dev), ##__VA_ARGS__)

/*
 * Device attribute files. The show/read halves are compiled and the store/write
 * halves too; only the sysfs node they would appear under is absent, so the
 * definitions stay meaningful the day /sys grows one.
 */
struct device_attribute {
	struct attribute attr;
	ssize_t (*show)(struct device *dev, struct device_attribute *attr,
	                char *buf);
	ssize_t (*store)(struct device *dev, struct device_attribute *attr,
	                 const char *buf, usize count);
};

#define __ATTR(_name, _mode, _show, _store) \
	{ { #_name, _mode }, _show, _store }
#define __ATTR_RO(_name) __ATTR(_name, 0444, _name##_show, 0)
#define __ATTR_RW(_name) __ATTR(_name, 0644, _name##_show, _name##_store)

#define DEVICE_ATTR(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)
#define DEVICE_ATTR_RO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#define DEVICE_ATTR_RW(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RW(_name)

/* One attribute file on an already-registered device. */
int device_create_file(struct device *dev, const struct device_attribute *attr);
void device_remove_file(struct device *dev,
                        const struct device_attribute *attr);

#define dev_err(dev, fmt, ...)   lkpi_printk("drm %s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...)  lkpi_printk("drm %s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_info(dev, fmt, ...)  lkpi_printk("drm %s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...) lkpi_printk("drm %s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...)   ((void)0)
#define dev_err_once(dev, fmt, ...) dev_err(dev, fmt, ##__VA_ARGS__)
#define dev_warn_once(dev, fmt, ...) dev_warn(dev, fmt, ##__VA_ARGS__)

#endif
