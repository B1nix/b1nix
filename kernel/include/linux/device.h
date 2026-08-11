/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_DEVICE_H
#define LKPI_LINUX_DEVICE_H
#include <linux/ratelimit.h> /* upstream reaches it through dev_printk */

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
struct dev_pm_ops;
struct bus_type;
struct device_driver {
	const char *name;
	struct module *owner;
	const void *of_match_table;
	const struct dev_pm_ops *pm;
	/* The bus this driver binds on, and the callbacks that bus invokes.
	 * b1nix binds drivers directly rather than through a bus — see struct
	 * bus_type below — so a driver registered here is never matched and
	 * these are never called. */
	const struct bus_type *bus;
	int (*probe)(struct device *dev);
	int (*remove)(struct device *dev);
	void (*shutdown)(struct device *dev);
};
/* Register a driver on its bus. Nothing matches — see above — so this reports
 * success and no probe follows. */
static inline int driver_register(struct device_driver *drv)
{ (void)drv; return 0; }
static inline void driver_unregister(struct device_driver *drv) { (void)drv; }

typedef void (*devres_release_t)(struct device *dev, void *res);

struct devres_node {
	struct devres_node *next;
	devres_release_t release;
	usize size;
	/* The caller's memory follows this header, so devm_kfree can find the node
	 * from the pointer it was given by stepping back one header. */
};

/*
 * The runtime-PM state a device carries, under the name imported code reads it
 * by. The counters themselves live in the embedded lkpi_device — this is the
 * view of them Linux's callers expect, so the two never disagree.
 */
struct dev_pm_info {
	unsigned int disable_depth;
	unsigned int runtime_auto;
	int runtime_error;
	unsigned int usage_count;
	/* Whether the device is currently powered down. Kept in step with the
	 * embedded lkpi_device's own flag by the runtime-PM shim, so a reader of
	 * either sees the same state. */
	unsigned int is_suspended;
};

struct device {
	/* Imported code reaches for dev->kobj to hang sysfs entries off. It is the
	 * kobject inside the embedded lkpi_device, exposed under the name Linux
	 * uses so those references resolve to one object rather than two. */
	struct kobject kobj;
	struct dev_pm_info power;
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
	/* The bus this device sits on. b1nix registers no buses of its own —
	 * see bus_register_notifier() below — so this stays NULL and a caller
	 * that walks it finds nothing rather than the wrong bus. */
	const struct bus_type *bus;
	/* This device's device-tree node. Always NULL — see <linux/of.h>. */
	struct device_node *of_node;
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


/* The rate-limited spellings. Nothing here suppresses anything — see
 * <linux/ratelimit.h> — so these are the plain forms, and a driver storming a
 * message will storm the log. */
#define dev_err_ratelimited(dev, fmt, ...)  dev_err(dev, fmt, ##__VA_ARGS__)
#define dev_warn_ratelimited(dev, fmt, ...) dev_warn(dev, fmt, ##__VA_ARGS__)
#define dev_info_ratelimited(dev, fmt, ...) dev_info(dev, fmt, ##__VA_ARGS__)
#define dev_dbg_ratelimited(dev, fmt, ...)  dev_dbg(dev, fmt, ##__VA_ARGS__)
#define printk_ratelimited(fmt, ...)        printk(fmt, ##__VA_ARGS__)
#define pr_warn_ratelimited(fmt, ...)       pr_warn(fmt, ##__VA_ARGS__)
#define pr_err_ratelimited(fmt, ...)        pr_err(fmt, ##__VA_ARGS__)
#define pr_info_ratelimited(fmt, ...)       pr_info(fmt, ##__VA_ARGS__)


/* The once-only spellings, per call site — see <linux/printk.h>. */
#define dev_info_once(dev, fmt, ...)                             \
	do {                                                         \
		static bool __printed;                                   \
		if (!__printed) { __printed = true; dev_info(dev, fmt, ##__VA_ARGS__); } \
	} while (0)
#define dev_warn_once(dev, fmt, ...)                             \
	do {                                                         \
		static bool __printed;                                   \
		if (!__printed) { __printed = true; dev_warn(dev, fmt, ##__VA_ARGS__); } \
	} while (0)
#define dev_err_once(dev, fmt, ...)                              \
	do {                                                         \
		static bool __printed;                                   \
		if (!__printed) { __printed = true; dev_err(dev, fmt, ##__VA_ARGS__); } \
	} while (0)
#define dev_notice_once(dev, fmt, ...) dev_info_once(dev, fmt, ##__VA_ARGS__)


/* Power-management declarations, including power_group_name, reach drivers
 * through the device interface. */
#include <linux/pm.h>


/*
 * A dependency between two devices, so one is suspended after and resumed
 * before the other.
 *
 * b1nix has no system suspend and no dependency-ordered resume, so a link
 * records nothing and orders nothing. Returning a non-NULL handle anyway would
 * be worse than failing: the caller would believe an ordering it does not have.
 */
struct device_link;
static inline struct device_link *device_link_add(struct device *consumer,
                                                  struct device *supplier,
                                                  u32 flags)
{ (void)consumer; (void)supplier; (void)flags; return 0; }
static inline void device_link_del(struct device_link *link) { (void)link; }
static inline void device_link_remove(void *consumer, struct device *supplier)
{ (void)consumer; (void)supplier; }

#define DL_FLAG_STATELESS      (1 << 0)
#define DL_FLAG_PM_RUNTIME     (1 << 1)
#define DL_FLAG_RPM_ACTIVE     (1 << 2)


/* Watching a bus for devices appearing and disappearing. b1nix enumerates once
 * at boot and nothing hotplugs, so a registered notifier is never called — and
 * a driver waiting for a device that is already there will find it by the
 * lookup it does first. */
/*
 * A bus type: how devices on it are matched to drivers and probed.
 *
 * b1nix has no bus registry — devices are enumerated by the PCI code and bound
 * directly — so registering one records nothing and no device is ever matched
 * against it. A driver that publishes a child on its own bus (i915 does, for
 * DSI hosts) finds no one to bind it, which is the same outcome as the child's
 * driver not being built.
 */
struct kobj_uevent_env;
struct bus_type {
	const char *name;
	const char *dev_name;
	const struct attribute_group **bus_groups;
	const struct attribute_group **dev_groups;
	const struct attribute_group **drv_groups;
	int (*match)(struct device *dev, struct device_driver *drv);
	int (*uevent)(const struct device *dev, struct kobj_uevent_env *env);
	int (*probe)(struct device *dev);
	void (*remove)(struct device *dev);
	void (*shutdown)(struct device *dev);
	const struct dev_pm_ops *pm;
};
static inline int bus_register(const struct bus_type *bus) { (void)bus; return 0; }
static inline void bus_unregister(const struct bus_type *bus) { (void)bus; }
struct notifier_block;
static inline int bus_register_notifier(const struct bus_type *bus,
                                        struct notifier_block *nb)
{ (void)bus; (void)nb; return 0; }
static inline int bus_unregister_notifier(const struct bus_type *bus,
                                          struct notifier_block *nb)
{ (void)bus; (void)nb; return 0; }


/* Bus notifier events. Nothing delivers them here — see bus_register_notifier
 * — but a driver's switch on them has to compile. */
#define BUS_NOTIFY_ADD_DEVICE       0x00000001
#define BUS_NOTIFY_DEL_DEVICE       0x00000002
#define BUS_NOTIFY_BOUND_DRIVER     0x00000004
#define BUS_NOTIFY_UNBIND_DRIVER    0x00000005
#define BUS_NOTIFY_UNBOUND_DRIVER   0x00000006


/* An ACPI handle hangs off a device upstream, and driver code names the type
 * having included only this header. */
#include <linux/acpi.h>


#define BUS_NOTIFY_DRIVER_NOT_BOUND 0x00000007
#define BUS_NOTIFY_BIND_DRIVER      0x00000003


/* Platform data attached at device creation. Nothing here creates platform
 * devices with data, so there is none to hand back. */
static inline void *dev_get_platdata(const struct device *dev)
{ (void)dev; return NULL; }

#define dev_notice_ratelimited(dev, fmt, ...) dev_notice(dev, fmt, ##__VA_ARGS__)

/* Is this device behind an IOMMU domain? M100 attaches one per device when the
 * IOMMU is enabled; this reports what that layer recorded. */
bool device_iommu_mapped(struct device *dev);

/*
 * Binary sysfs attributes.
 *
 * b1nix's sysfs serves text attributes only — the read path builds a string
 * into a page buffer. A binary attribute (i915 exports its error state this
 * way) has no representation, so creating one reports success and serves
 * nothing rather than half-serving it as text.
 */
struct bin_attribute;
static inline int device_create_bin_file(struct device *dev,
                                         const struct bin_attribute *attr)
{ (void)dev; (void)attr; return 0; }
static inline void device_remove_bin_file(struct device *dev,
                                          const struct bin_attribute *attr)
{ (void)dev; (void)attr; }

/* Claim a physical range for the life of the device. b1nix does not arbitrate
 * MMIO ranges between drivers — see <linux/ioport.h> — so the claim always
 * succeeds and the range is whatever the caller asked for. */
struct resource;
struct resource *devm_request_mem_region(struct device *dev,
                                         resource_size_t start,
                                         resource_size_t n, const char *name);


/* Attach a firmware node to a device. There is no firmware node tree here — see
 * <linux/of.h> — so this records nothing and dev->of_node stays NULL. */
struct fwnode_handle;
static inline void device_set_node(struct device *dev, struct fwnode_handle *fw)
{ (void)dev; (void)fw; }

/* Find a device on a bus by its device-tree node. Same absence: no node, no
 * match. */
static inline struct device *bus_find_device_by_of_node(const struct bus_type *bus,
                                                        const void *np)
{ (void)bus; (void)np; return NULL; }

/* Add a variable to a uevent's environment. b1nix emits uevents over netlink
 * with a fixed set of keys; a driver's extra variables are not carried, so
 * hotplug helpers that key off them do not see them. */
struct kobj_uevent_env;
int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...);


/* Walk a device's children. b1nix records no child list on a device — a driver
 * that publishes children keeps its own — so the walk visits nothing and the
 * callback is not called. */
static inline int device_for_each_child(struct device *parent, void *data,
                                        int (*fn)(struct device *dev, void *data))
{ (void)parent; (void)data; (void)fn; return 0; }

#endif
