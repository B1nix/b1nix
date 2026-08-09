/* SPDX-License-Identifier: MIT */
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
	ssize_t (*read)(struct file *, struct kobject *, struct bin_attribute *,
	                char *, loff_t, size_t);
	ssize_t (*write)(struct file *, struct kobject *, struct bin_attribute *,
	                 char *, loff_t, size_t);
};

#define __BIN_ATTR(_name, _mode, _read, _write, _size) \
	{ { #_name, _mode }, _size, _read, _write }
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
#endif
