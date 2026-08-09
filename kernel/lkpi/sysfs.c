/* SPDX-License-Identifier: MIT
 *
 * M101 linuxkpi: sysfs and debugfs, the parts imported drivers reach for.
 *
 * Linux drivers publish through three shapes, and all three end up in the same
 * registry underneath (see <b1nix/sysfs_attr.h>):
 *
 *   - a class, which is a directory under /sys/class that devices join;
 *   - a device, whose registration creates /sys/class/<class>/<name> and
 *     publishes the attribute groups the driver hung off it;
 *   - individual attribute files, added and removed one at a time.
 *
 * The show/store halves are the driver's own functions, called with the same
 * one-page buffer contract Linux gives them, so a ported attribute behaves the
 * way it was written rather than the way this layer would prefer.
 *
 * debugfs is the same machinery under /sys/kernel/debug. b1nix has no separate
 * debugfs mount, and inventing one to hold a handful of diagnostic files would
 * be a filesystem's worth of code for no behaviour a driver can tell apart.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>
#include <lkpi/env.h>

/* ── attribute plumbing ─────────────────────────────────────────── */

/*
 * One registered file. The context a show() needs is the device plus the
 * attribute it was declared as, so both are carried here rather than
 * reconstructed: an attribute's show() is free to look at which attribute it
 * was called for, and several share one function precisely so they can.
 */
struct lkpi_attr_ctx {
	struct device *dev;
	struct device_attribute *dev_attr;
	const struct bin_attribute *bin_attr;
	struct kobject *kobj;
};

static isize lkpi_attr_show(void *ctx, char *buf, usize cap)
{
	struct lkpi_attr_ctx *c = ctx;

	if (!c)
		return -EINVAL;
	if (c->bin_attr) {
		if (!c->bin_attr->read)
			return -EACCES;
		return c->bin_attr->read(0, c->kobj,
		                         (struct bin_attribute *)c->bin_attr, buf, 0,
		                         cap);
	}
	if (!c->dev_attr || !c->dev_attr->show)
		return -EACCES;
	return c->dev_attr->show(c->dev, c->dev_attr, buf);
}

static isize lkpi_attr_store(void *ctx, const char *buf, usize len)
{
	struct lkpi_attr_ctx *c = ctx;

	if (!c)
		return -EINVAL;
	if (c->bin_attr) {
		if (!c->bin_attr->write)
			return -EACCES;
		return c->bin_attr->write(0, c->kobj,
		                          (struct bin_attribute *)c->bin_attr,
		                          (char *)buf, 0, len);
	}
	if (!c->dev_attr || !c->dev_attr->store)
		return -EACCES;
	return c->dev_attr->store(c->dev, c->dev_attr, buf, len);
}

static void attr_ctx_release(void *ctx)
{
	kfree(ctx);
}

static struct lkpi_attr_ctx *attr_ctx(struct device *dev,
                                      struct device_attribute *da,
                                      const struct bin_attribute *ba,
                                      struct kobject *kobj)
{
	struct lkpi_attr_ctx *c = kzalloc(sizeof(*c), GFP_KERNEL);

	if (!c)
		return 0;
	c->dev = dev;
	c->dev_attr = da;
	c->bin_attr = ba;
	c->kobj = kobj;
	return c;
}

int device_create_file(struct device *dev, const struct device_attribute *attr)
{
	if (!dev || !attr)
		return -EINVAL;
	if (!dev->lk.sysfs)
		return 0; /* not registered anywhere yet: nothing to hang it off */

	struct lkpi_attr_ctx *c =
		attr_ctx(dev, (struct device_attribute *)attr, 0, &dev->kobj);
	if (!c)
		return -ENOMEM;
	int rc = lkpi_sysfs_attr(dev->lk.sysfs, attr->attr.name, attr->attr.mode,
	                         lkpi_attr_show, attr->store ? lkpi_attr_store : 0,
	                         c, attr_ctx_release);
	if (rc != 0)
		kfree(c);
	return rc;
}

void device_remove_file(struct device *dev, const struct device_attribute *attr)
{
	if (!dev || !attr || !dev->lk.sysfs)
		return;
	lkpi_sysfs_attr_remove(dev->lk.sysfs, attr->attr.name);
}

/* Publish one attribute group under `dir`. A group with a name gets its own
 * subdirectory, which is how Linux nests them. */
static int publish_group(void *dir, struct device *dev, struct kobject *kobj,
                         const struct attribute_group *grp)
{
	if (!grp || !dir)
		return 0;

	void *target = dir;
	if (grp->name) {
		target = lkpi_sysfs_dir(dir, grp->name);
		if (!target)
			return -ENOMEM;
	}

	if (grp->attrs) {
		for (usize i = 0; grp->attrs[i]; i++) {
			struct attribute *a = grp->attrs[i];
			/* is_visible() is the driver saying this attribute does not apply
			 * to this device — a hidden attribute must not appear at all,
			 * because userspace treats presence as capability. */
			if (grp->is_visible && !grp->is_visible(kobj, a, (int)i))
				continue;
			struct device_attribute *da =
				container_of(a, struct device_attribute, attr);
			struct lkpi_attr_ctx *c = attr_ctx(dev, da, 0, kobj);
			if (!c)
				return -ENOMEM;
			int rc = lkpi_sysfs_attr(target, a->name, a->mode, lkpi_attr_show,
			                         da->store ? lkpi_attr_store : 0, c,
			                         attr_ctx_release);
			if (rc != 0)
				kfree(c);
		}
	}

	if (grp->bin_attrs) {
		for (usize i = 0; grp->bin_attrs[i]; i++) {
			struct bin_attribute *b = grp->bin_attrs[i];
			struct lkpi_attr_ctx *c = attr_ctx(dev, 0, b, kobj);
			if (!c)
				return -ENOMEM;
			int rc = lkpi_sysfs_attr(target, b->attr.name, b->attr.mode,
			                         lkpi_attr_show, b->write ? lkpi_attr_store : 0,
			                         c, attr_ctx_release);
			if (rc != 0)
				kfree(c);
		}
	}
	return 0;
}

/*
 * The directory a kobject publishes into. A registered device sets it on both
 * of its kobjects; anything else — a bare kobject standing in for a parent
 * directory, which is how the core names one — has none, and says so.
 */
static void *kobj_dir(struct kobject *kobj)
{
	return kobj ? kobj->sysfs : 0;
}

int sysfs_create_group(struct kobject *kobj, const struct attribute_group *grp)
{
	void *dir = kobj_dir(kobj);

	if (!kobj)
		return -EINVAL;
	if (!dir)
		return 0;
	return publish_group(dir, kobj_to_dev(kobj), kobj, grp);
}

int sysfs_create_groups(struct kobject *kobj,
                        const struct attribute_group **groups)
{
	if (!groups)
		return 0;
	for (usize i = 0; groups[i]; i++) {
		int rc = sysfs_create_group(kobj, groups[i]);
		if (rc != 0)
			return rc;
	}
	return 0;
}

/*
 * Remove a group's files. A named group is a directory of its own and goes
 * whole; an unnamed one has its attributes mixed in with the device's, so they
 * are removed by name, one at a time — removing the directory there would take
 * the device's other files with it.
 */
void sysfs_remove_group(struct kobject *kobj, const struct attribute_group *grp)
{
	void *dir = kobj_dir(kobj);

	if (!dir || !grp)
		return;

	if (grp->name) {
		void *sub = lkpi_sysfs_find(dir, grp->name);
		if (sub)
			lkpi_sysfs_remove(sub);
		return;
	}

	if (grp->attrs) {
		for (usize i = 0; grp->attrs[i]; i++)
			lkpi_sysfs_attr_remove(dir, grp->attrs[i]->name);
	}
	if (grp->bin_attrs) {
		for (usize i = 0; grp->bin_attrs[i]; i++)
			lkpi_sysfs_attr_remove(dir, grp->bin_attrs[i]->attr.name);
	}
}

void sysfs_remove_groups(struct kobject *kobj,
                         const struct attribute_group **groups)
{
	if (!groups)
		return;
	for (usize i = 0; groups[i]; i++)
		sysfs_remove_group(kobj, groups[i]);
}

int sysfs_create_link(struct kobject *kobj, struct kobject *target,
                      const char *name)
{
	if (!target || !name)
		return -EINVAL;

	void *target_dir = kobj_dir(target);
	if (!target_dir)
		return 0; /* the target is not published: there is nothing to point at */

	/* The core names the directory to link *in* by the parent kobject of the
	 * target — which b1nix does not model as an object. Resolving it as "the
	 * directory the target sits in" is what that means here, and it is what
	 * Linux produces for the same call. */
	void *dir = kobj_dir(kobj);
	if (!dir)
		dir = lkpi_sysfs_parent(target_dir);
	if (!dir)
		return 0;

	return lkpi_sysfs_link(dir, name, target_dir);
}

void sysfs_remove_link(struct kobject *kobj, const char *name)
{
	void *dir = kobj_dir(kobj);

	if (dir && name)
		lkpi_sysfs_link_remove(dir, name);
}

/* ── classes and devices ────────────────────────────────────────── */

struct class *class_create(const char *name)
{
	struct class *c;

	if (!name)
		return ERR_PTR(-EINVAL);
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return ERR_PTR(-ENOMEM);
	c->name = name;

	/* /sys/class/<name>, joining whatever is already there rather than
	 * creating a second /sys/class. */
	void *classes = lkpi_sysfs_dir(0, "class");
	c->sysfs = classes ? lkpi_sysfs_dir(classes, name) : 0;
	if (!c->sysfs) {
		kfree(c);
		return ERR_PTR(-ENOMEM);
	}
	return c;
}

void class_destroy(struct class *c)
{
	if (!c || IS_ERR(c))
		return;
	if (c->sysfs)
		lkpi_sysfs_remove(c->sysfs);
	kfree(c);
}

/* A class-wide attribute whose value is a fixed string — what the DRM core
 * publishes its interface version as. */
static isize const_string_show(void *ctx, char *buf, usize cap)
{
	const char *str = ctx;

	return (isize)lkpi_snprintf(buf, cap, "%s\n", str ? str : "");
}

int class_create_file(struct class *c, const struct attribute *attr)
{
	if (!c || IS_ERR(c) || !attr || !c->sysfs)
		return -EINVAL;

	/* The class-wide attributes the DRM core registers are constant strings
	 * (its interface version), declared as class_attribute_string. */
	const struct class_attribute_string *cs =
		container_of(attr, struct class_attribute_string, attr);
	return lkpi_sysfs_attr(c->sysfs, attr->name, attr->mode ? attr->mode : 0444,
	                       const_string_show, 0, (void *)cs->str, 0);
}

void class_remove_file(struct class *c, const struct attribute *attr)
{
	(void)c;
	(void)attr;
}

/*
 * Registering a device. This is where a device becomes visible: its directory
 * appears under its class, the attribute groups the driver declared are
 * published, and — because userspace looks for it — a `dev` file carrying the
 * device number, in the major:minor form every hotplug tool parses.
 */
static isize devt_show(void *ctx, char *buf, usize cap)
{
	struct device *dev = ctx;

	if (!dev)
		return -EINVAL;
	return (isize)lkpi_snprintf(buf, cap, "%u:%u\n", MAJOR(dev->devt),
	                            MINOR(dev->devt));
}

static isize name_show(void *ctx, char *buf, usize cap)
{
	struct device *dev = ctx;

	return (isize)lkpi_snprintf(buf, cap, "%s\n", dev_name(dev));
}

int device_add(struct device *dev)
{
	if (!dev)
		return -EINVAL;
	if (dev->lk.sysfs)
		return 0; /* already added */

	void *parent_dir = 0;
	if (dev->class && !IS_ERR(dev->class))
		parent_dir = dev->class->sysfs;
	if (!parent_dir) {
		/* No class: it still belongs somewhere, and /sys/devices is where
		 * Linux puts a device whose class has not claimed it. */
		parent_dir = lkpi_sysfs_dir(0, "devices");
	}
	if (!parent_dir)
		return -ENOMEM;

	dev->lk.sysfs = lkpi_sysfs_dir(parent_dir, dev_name(dev));
	if (!dev->lk.sysfs)
		return -ENOMEM;

	/* Both kobjects a device carries point at the same directory, so an
	 * attribute registered through either lands in one place. */
	dev->kobj.sysfs = dev->lk.sysfs;
	dev->lk.kobj.sysfs = dev->lk.sysfs;
	/* What a hotplug helper matches on first. */
	dev->kobj.subsystem = (dev->class && !IS_ERR(dev->class)) ? dev->class->name
	                                                          : "device";
	dev->lk.kobj.subsystem = dev->kobj.subsystem;

	if (dev->devt)
		lkpi_sysfs_attr(dev->lk.sysfs, "dev", 0444, devt_show, 0, dev, 0);
	lkpi_sysfs_attr(dev->lk.sysfs, "name", 0444, name_show, 0, dev, 0);

	if (dev->groups) {
		for (usize i = 0; dev->groups[i]; i++)
			publish_group(dev->lk.sysfs, dev, &dev->kobj, dev->groups[i]);
	}

	/* Announce it only once everything it publishes exists: a listener acts on
	 * the event by reading the device's attributes, and an announcement that
	 * arrives first is one it would answer with ENOENT. */
	kobject_uevent(&dev->kobj, KOBJ_ADD);
	return 0;
}

void device_del(struct device *dev)
{
	if (!dev || !dev->lk.sysfs)
		return;
	/* Announce the removal while the device still has a devpath to name. */
	kobject_uevent(&dev->kobj, KOBJ_REMOVE);
	lkpi_sysfs_remove(dev->lk.sysfs);
	dev->lk.sysfs = 0;
	dev->kobj.sysfs = 0;
	dev->lk.kobj.sysfs = 0;
}

int device_register(struct device *dev)
{
	device_initialize(dev);
	return device_add(dev);
}

void device_unregister(struct device *dev)
{
	device_del(dev);
	put_device(dev);
}

/* ── debugfs ────────────────────────────────────────────────────── */

/*
 * debugfs files are declared with a file_operations, not a show(). The ones
 * drivers actually use are seq_file-backed reads, so a read here calls the
 * registered read() with the caller's buffer and a zero offset — enough for a
 * diagnostic dump, and honest about what it is: there is no seq_file iteration
 * state here, so a dump larger than one buffer is truncated rather than
 * silently continued from the wrong place.
 */
struct lkpi_debugfs_file {
	const struct file_operations *fops;
	/* The inode the driver's open() is handed: it reads i_private for the
	 * pointer passed at creation, which is how single_open() finds its data. */
	struct inode inode;
	int opened;
	/* The file a driver's read() is handed. Drivers reach for
	 * file->private_data — the pointer they passed at creation — so this is a
	 * real file rather than a NULL that would fault on the first dereference.
	 * One per debugfs file, not per open: nothing here opens them twice, and a
	 * per-open file would need an open path b1nix's /sys does not have. */
	struct file file;
	char name[64];
};

/*
 * A debugfs read, given the offset the reader is at.
 *
 * This is what makes a dump larger than one buffer work: the driver's read()
 * takes a loff_t, advances it, and the next read continues from there. What is
 * still missing is seq_file — a driver whose fops are {seq_read, single_open}
 * needs that machinery, and it is not here yet, so such a file reads as empty
 * rather than as a wrong answer. Files with a plain read() work fully.
 */
static isize debugfs_read_at(void *ctx, char *buf, usize cap, u64 offset)
{
	struct lkpi_debugfs_file *f = ctx;
	loff_t pos = (loff_t)offset;

	if (!f || !f->fops || !f->fops->read)
		return 0;

	/*
	 * Open on first use. A seq_file-backed file does its real work in open() —
	 * single_open() is what attaches the seq_file that seq_read() then reads
	 * from — so a read that skipped open() would find no state and return
	 * nothing. b1nix's /sys has no open path of its own to hang this on, so
	 * the first read performs it, once.
	 */
	if (!f->opened && f->fops->open) {
		int rc = f->fops->open(&f->inode, &f->file);
		if (rc != 0)
			return rc;
	}
	f->opened = 1;

	return f->fops->read(&f->file, buf, cap, &pos);
}

static void debugfs_file_release(void *ctx)
{
	struct lkpi_debugfs_file *f = ctx;

	/* Whatever open() allocated — a seq_file and its buffer — is the driver's
	 * to free, through the release() it declared. */
	if (f && f->opened && f->fops && f->fops->release)
		f->fops->release(&f->inode, &f->file);
	kfree(f);
}

struct dentry *debugfs_create_dir(const char *name, struct dentry *parent)
{
	void *p = parent ? (void *)parent : lkpi_sysfs_debug_root();

	if (!name || !p)
		return 0;
	return (struct dentry *)lkpi_sysfs_dir(p, name);
}

struct dentry *debugfs_create_file(const char *name, umode_t mode,
                                   struct dentry *parent, void *data,
                                   const struct file_operations *fops)
{
	void *p = parent ? (void *)parent : lkpi_sysfs_debug_root();

	if (!name || !p || !fops)
		return 0;

	struct lkpi_debugfs_file *f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return 0;
	f->fops = fops;
	f->file.f_op = fops;
	f->file.private_data = data;
	f->inode.i_private = data;
	f->inode.i_mode = mode ? mode : 0444;
	if (lkpi_sysfs_attr_at(p, name, mode ? mode : 0444, debugfs_read_at, 0, f,
	                       debugfs_file_release) != 0) {
		kfree(f);
		return 0;
	}
	/* The directory is what a caller can later remove; an individual file is
	 * removed with the directory that holds it. */
	return (struct dentry *)p;
}

void debugfs_remove(struct dentry *d)
{
	(void)d;
}

void debugfs_remove_recursive(struct dentry *d)
{
	if (d)
		lkpi_sysfs_remove((void *)d);
}

/* ── self-test support ──────────────────────────────────────────── */

/*
 * A debugfs file written the way a driver writes one: an open() that calls
 * single_open(), reads through seq_read(). It prints more than one read's worth
 * on purpose — that is the case a buffered seq_file exists for, and the case a
 * read() that always starts at zero silently truncates.
 */
static int selftest_seq_show(struct seq_file *m, void *v)
{
	(void)v;
	for (int i = 0; i < 200; i++)
		seq_printf(m, "line %d\n", i);
	return 0;
}

static int selftest_seq_open(struct inode *inode, struct file *file)
{
	return single_open(file, selftest_seq_show, inode->i_private);
}

static const struct file_operations selftest_seq_fops = {
	.open = selftest_seq_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* Publish it under debugfs and report where it lives. */
int lkpi_selftest_seq_file_create(void)
{
	struct dentry *d = debugfs_create_dir("b1nix-seq", 0);

	if (!d)
		return -ENOMEM;
	if (!debugfs_create_file("lines", 0444, d, 0, &selftest_seq_fops))
		return -ENOMEM;
	return 0;
}

/*
 * Register a device with a class, so the uevent it raises can be observed from
 * the other side of the boundary. It is a real registration through the same
 * device_add() every driver uses — a test that formatted a message itself
 * would prove only that this file can print.
 */
static struct class *g_probe_class;
static struct device g_probe_dev;

int lkpi_selftest_uevent_device_add(void)
{
	if (!g_probe_class)
		g_probe_class = class_create("b1nix-selftest");
	if (!g_probe_class || IS_ERR(g_probe_class))
		return -ENOMEM;

	device_initialize(&g_probe_dev);
	dev_set_name(&g_probe_dev, "uevent0");
	g_probe_dev.class = g_probe_class;
	g_probe_dev.devt = MKDEV(240, 1);
	return device_add(&g_probe_dev);
}

void lkpi_selftest_uevent_device_del(void)
{
	device_del(&g_probe_dev);
	if (g_probe_class && !IS_ERR(g_probe_class)) {
		class_destroy(g_probe_class);
		g_probe_class = 0;
	}
}
