/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SYSFS_H
#define LKPI_LINUX_SYSFS_H
#include <linux/types.h>

/* Declared before anything names them: a struct first seen inside a member
 * list or a parameter list becomes a type in that scope, and its pointer then
 * refuses to match the file-scope one. */
struct file;
struct kobject;
struct device;
/*
 * sysfs attribute and link management. These publish for real: b1nix's /sys
 * grew a registry of runtime-registered attribute files (<b1nix/sysfs_attr.h>)
 * precisely because the DRM core import needed one, and a registration here
 * creates a file whose reads call the driver's own show().
 */

/* A read-only attribute whose value is a fixed string — what the class-wide
 * attributes are. Imported code passes &attr.attr to the registration calls,
 * so the embedded attribute has to exist. */
struct attribute { const char *name; umode_t mode; };
struct class_attribute_string {
	struct attribute attr;
	const char *str;
};
#define CLASS_ATTR_STRING(_name, _mode, _str) \
	struct class_attribute_string class_attr_##_name = { { #_name, _mode }, _str }
#define S_IRUGO 0444
#define S_IWUSR 0200
struct attribute;
struct attribute_group {
	const char *name;
	struct attribute **attrs;
	struct bin_attribute **bin_attrs;
	int (*is_visible)(struct kobject *, struct attribute *, int);
};
/* Format into a sysfs output buffer. The buffer is one page and the count is
 * what the read returns, which is why this exists rather than plain snprintf. */
int sysfs_emit(char *buf, const char *fmt, ...);
int sysfs_emit_at(char *buf, int at, const char *fmt, ...);

/* An attribute whose value is binary rather than text — an EDID blob, for
 * instance. Published like the text ones; its read() is called with the
 * caller's buffer and offset zero. */
struct bin_attribute {
	struct attribute attr;
	usize size;
	/* The owner's context, handed back to read/write. Upstream's callbacks get
	 * the kobject too, but a driver publishing one attribute per connector
	 * needs to know which connector — and this is where it puts that. */
	void *private;
	ssize_t (*read)(struct file *, struct kobject *, struct bin_attribute *,
	                char *, loff_t, size_t);
	ssize_t (*write)(struct file *, struct kobject *, struct bin_attribute *,
	                 char *, loff_t, size_t);
	/* Mapping the attribute's contents instead of reading them. Nothing here
	 * calls it — b1nix's sysfs serves attributes through read — so an
	 * attribute that offers only mmap is unreadable rather than misread. */
	int (*mmap)(struct file *, struct kobject *, struct bin_attribute *,
	            struct vm_area_struct *);
};

#define __BIN_ATTR(_name, _mode, _read, _write, _size) \
	{ .attr = { #_name, _mode }, .size = _size, .read = _read, .write = _write }
#define BIN_ATTR_RO(_name, _size) \
	struct bin_attribute bin_attr_##_name = \
		__BIN_ATTR(_name, 0444, _name##_read, 0, _size)

int sysfs_create_link(struct kobject *kobj, struct kobject *target,
                      const char *name);
void sysfs_remove_link(struct kobject *kobj, const char *name);
int sysfs_create_group(struct kobject *kobj, const struct attribute_group *grp);
int sysfs_create_groups(struct kobject *kobj,
                        const struct attribute_group **groups);
void sysfs_remove_group(struct kobject *kobj,
                        const struct attribute_group *grp);
void sysfs_remove_groups(struct kobject *kobj,
                         const struct attribute_group **groups);

/*
 * An attribute on a plain kobject, as opposed to one on a device.
 *
 * The two differ only in what the show/store callbacks are handed — a kobject
 * here, a device there — and imported code declares both. The registry behind
 * them is the same one; see <lkpi/env.h> for the crossing.
 */
struct kobj_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr, char *buf);
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr,
	                 const char *buf, size_t count);
};

#define __ATTR_RO(_name) { .attr = { .name = __stringify(_name), .mode = 0444 }, \
                           .show = _name##_show }
#define __ATTR_RW(_name) { .attr = { .name = __stringify(_name), .mode = 0644 }, \
                           .show = _name##_show, .store = _name##_store }


/* Create or remove a NULL-terminated array of attributes in one call. The
 * partial-failure behaviour is upstream's: on the first failure the ones
 * already created are removed, so the caller never has to unwind a half-built
 * set. */
int sysfs_create_files(struct kobject *kobj, const struct attribute * const *ptr);
void sysfs_remove_files(struct kobject *kobj, const struct attribute * const *ptr);


int sysfs_create_file(struct kobject *kobj, const struct attribute *attr);
void sysfs_remove_file(struct kobject *kobj, const struct attribute *attr);
struct kobject *kobject_create_and_add(const char *name, struct kobject *parent);


/* How a kobject's attributes are read and written. A kobj_type points at one,
 * so every object of that type dispatches the same way and the attribute
 * itself carries the behaviour. */
struct sysfs_ops {
	ssize_t (*show)(struct kobject *kobj, struct attribute *attr, char *buf);
	ssize_t (*store)(struct kobject *kobj, struct attribute *attr,
	                 const char *buf, size_t count);
};


/* Initialise an attribute's lockdep key. No lockdep here; the attribute needs
 * no other setup. */
#define sysfs_attr_init(attr) do { (void)(attr); } while (0)

/* Add a group's files into an already-created group. b1nix's sysfs creates the
 * files a group names at creation time and has no merge step, so this reports
 * success without adding anything: the files upstream would have merged are
 * absent, which costs those attributes, not the ones already there. */
struct attribute_group;
struct kobject;
static inline int sysfs_merge_group(struct kobject *kobj,
                                    const struct attribute_group *grp)
{ (void)kobj; (void)grp; return 0; }
static inline void sysfs_unmerge_group(struct kobject *kobj,
                                       const struct attribute_group *grp)
{ (void)kobj; (void)grp; }


/* Build the NULL-terminated group array a kobj_type wants from a bare
 * attribute array, under the name upstream generates it with. */
#define ATTRIBUTE_GROUPS(_name)                                          \
static const struct attribute_group _name##_group = {                    \
	.attrs = _name##_attrs,                                              \
};                                                                       \
static const struct attribute_group *_name##_groups[] = {                \
	&_name##_group,                                                      \
	NULL,                                                                \
}

#endif
