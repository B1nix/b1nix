/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * debugfs.c — Dynamic Kernel Introspection & Tracing Filesystem for b1nix.
 *
 * Provides a synthetic filesystem exposing live kernel internal state, heap
 * telemetry, scheduler task tables, and VFS metrics for real-time debugging
 * and profiling without requiring printk re-compilation.
 */

#include <b1nix/debugfs.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/kprintf.h>
#include <b1nix/ktime.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/version.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <string.h>

#define DEBUGFS_BUF_SIZE 4096

static isize debugfs_serve_buf(const char *src, usize src_len, u64 offset,
                               char *buf, usize count)
{
	if (offset >= src_len)
		return 0;

	if (offset + count > src_len)
		count = src_len - offset;

	memcpy(buf, src + offset, count);
	return (isize)count;
}

/* ── Built-in diagnostic generators ────────────────────────────────────────── */

static isize debugfs_kheap_stats_read(struct vfs_node *node, u64 offset,
                                      char *buf, usize size, int flags)
{
	(void)node; (void)flags;
	char tmp[512];
	u64 total_ram = pmm_total_usable_memory();
	u64 free_frames = (u64)pmm_free_frame_count();
	u64 free_ram = free_frames * PAGE_SIZE;
	u64 mapped_kheap = kheap_mapped_bytes();

	int len = snprintf(tmp, sizeof(tmp),
	                   "=== b1nix Kernel Memory & Heap Diagnostics ===\n"
	                   "Total Usable RAM:   %lu KB (%lu MB)\n"
	                   "Free Physical RAM:  %lu KB (%lu MB)\n"
	                   "Free Page Frames:   %lu (4KB frames)\n"
	                   "Kernel Heap Mapped: %lu KB (%lu MB)\n",
	                   (unsigned long)(total_ram / 1024ULL),
	                   (unsigned long)(total_ram / (1024ULL * 1024ULL)),
	                   (unsigned long)(free_ram / 1024ULL),
	                   (unsigned long)(free_ram / (1024ULL * 1024ULL)),
	                   (unsigned long)free_frames,
	                   (unsigned long)(mapped_kheap / 1024ULL),
	                   (unsigned long)(mapped_kheap / (1024ULL * 1024ULL)));

	if (len < 0)
		return -EIO;

	return debugfs_serve_buf(tmp, (usize)len, offset, buf, size);
}

static const char *task_state_name(enum task_state s)
{
	switch (s) {
	case TASK_RUNNING:  return "RUNNING";
	case TASK_READY:    return "READY";
	case TASK_BLOCKED:  return "BLOCKED";
	case TASK_SLEEPING: return "SLEEP";
	case TASK_STOPPED:  return "STOPPED";
	case TASK_DEAD:     return "DEAD";
	case TASK_REAPING:  return "REAPING";
	default:            return "UNUSED";
	}
}

static isize debugfs_sched_tasks_read(struct vfs_node *node, u64 offset,
                                      char *buf, usize size, int flags)
{
	(void)node; (void)flags;
	char *page = kmalloc(DEBUGFS_BUF_SIZE);
	if (!page)
		return -ENOMEM;

	int pos = snprintf(page, DEBUGFS_BUF_SIZE,
	                   "%-5s %-5s %-8s %-4s %-8s %-8s %s\n",
	                   "PID", "PPID", "STATE", "PRIO", "NVCSW", "NIVCSW", "NAME");

	usize slots = scheduler_task_slots();
	for (usize i = 0; i < slots && pos < (int)(DEBUGFS_BUF_SIZE - 128); i++) {
		struct task *t = scheduler_task_slot(i);
		if (!t || t->state == TASK_UNUSED)
			continue;

		const char *tname = "[unnamed]";
		if (t->name && (uintptr_t)t->name >= 4096 && t->name[0]) {
			tname = t->name;
		}

		pos += snprintf(page + pos, DEBUGFS_BUF_SIZE - pos,
		                "%-5lu %-5lu %-8s %-4d %-8lu %-8lu %s\n",
		                (unsigned long)t->id,
		                (unsigned long)t->parent_id,
		                task_state_name(t->state),
		                (int)t->priority,
		                (unsigned long)task_nvcsw(t),
		                (unsigned long)task_nivcsw(t),
		                tname);
	}

	isize ret = debugfs_serve_buf(page, (usize)pos, offset, buf, size);
	kfree(page);
	return ret;
}

static isize debugfs_sched_load_read(struct vfs_node *node, u64 offset,
                                     char *buf, usize size, int flags)
{
	(void)node; (void)flags;
	char tmp[256];
	usize active = scheduler_task_count();
	usize max_t = scheduler_max_tasks();
	u64 uptime_ms = ktime_monotonic_ns() / 1000000ULL;

	int len = snprintf(tmp, sizeof(tmp),
	                   "Active Tasks: %lu\n"
	                   "Max Tasks:    %lu\n"
	                   "Uptime MS:    %lu\n",
	                   (unsigned long)active,
	                   (unsigned long)max_t,
	                   (unsigned long)uptime_ms);

	if (len < 0)
		return -EIO;

	return debugfs_serve_buf(tmp, (usize)len, offset, buf, size);
}

static isize debugfs_vfs_mounts_read(struct vfs_node *node, u64 offset,
                                     char *buf, usize size, int flags)
{
	(void)node; (void)flags;
	char *page = kmalloc(DEBUGFS_BUF_SIZE);
	if (!page)
		return -ENOMEM;

	int pos = snprintf(page, DEBUGFS_BUF_SIZE,
	                   "%-16s %-24s %-12s %s\n",
	                   "SOURCE", "TARGET", "FSTYPE", "FLAGS");

	struct vfs_mount_info *info = kmalloc(sizeof(struct vfs_mount_info) * 64);
	if (info) {
		isize count = vfs_mounts_info(info, 64);
		for (isize i = 0; i < count && pos < (int)(DEBUGFS_BUF_SIZE - 128); i++) {
			pos += snprintf(page + pos, DEBUGFS_BUF_SIZE - pos,
			                "%-16s %-24s %-12s 0x%lx\n",
			                info[i].source[0] ? info[i].source : "none",
			                info[i].target,
			                info[i].fstype,
			                (unsigned long)info[i].flags);
		}
		kfree(info);
	}

	isize ret = debugfs_serve_buf(page, (usize)pos, offset, buf, size);
	kfree(page);
	return ret;
}

static isize debugfs_system_version_read(struct vfs_node *node, u64 offset,
                                         char *buf, usize size, int flags)
{
	(void)node; (void)flags;
	char tmp[256];
	int len = snprintf(tmp, sizeof(tmp),
	                   "b1nix Version: %s\n"
	                   "Kernel Release: %s\n",
	                   B1NIX_VERSION_STR,
	                   B1NIX_RELEASE_STR);
	if (len < 0)
		return -EIO;

	return debugfs_serve_buf(tmp, (usize)len, offset, buf, size);
}

/* ── Node construction helpers ────────────────────────────────────────────── */

struct vfs_node *b1nix_debugfs_create_dir(const char *name, struct vfs_node *parent)
{
	struct vfs_node *dir = vfs_create_node(VFS_DIRECTORY);
	if (!dir)
		return NULL;

	strncpy(dir->name, name, sizeof(dir->name) - 1);
	dir->name[sizeof(dir->name) - 1] = '\0';
	dir->inode->mode = 0755;
	dir->inode->nlink = 2;

	if (parent) {
		dir->parent = parent;
		dir->refcount++;
		vfs_attach_child(parent, dir);
	}
	return dir;
}

struct vfs_node *b1nix_debugfs_create_file(const char *name, u16 mode,
                                           struct vfs_node *parent, void *data,
                                           isize (*read_cb)(struct vfs_node *, u64, char *, usize, int))
{
	struct vfs_node *f = vfs_create_node(VFS_DEVICE);
	if (!f)
		return NULL;

	strncpy(f->name, name, sizeof(f->name) - 1);
	f->name[sizeof(f->name) - 1] = '\0';
	f->inode->mode = mode;
	f->inode->nlink = 1;
	f->inode->flags |= VFS_NODE_PSEUDO_REG;
	f->inode->data = data;
	f->inode->read_cb = read_cb;

	if (parent) {
		f->parent = parent;
		f->refcount++;
		vfs_attach_child(parent, f);
	}
	return f;
}

static isize debugfs_u32_read(struct vfs_node *node, u64 offset, char *buf,
                              usize size, int flags)
{
	(void)flags;
	if (!node || !node->inode || !node->inode->data)
		return -EINVAL;

	u32 *val = (u32 *)node->inode->data;
	char tmp[32];
	int len = snprintf(tmp, sizeof(tmp), "%u\n", *val);
	if (len < 0)
		return -EIO;

	return debugfs_serve_buf(tmp, (usize)len, offset, buf, size);
}

struct vfs_node *b1nix_debugfs_create_u32(const char *name, u16 mode,
                                         struct vfs_node *parent, u32 *value)
{
	return b1nix_debugfs_create_file(name, mode, parent, value, debugfs_u32_read);
}

static isize debugfs_bool_read(struct vfs_node *node, u64 offset, char *buf,
                               usize size, int flags)
{
	(void)flags;
	if (!node || !node->inode || !node->inode->data)
		return -EINVAL;

	int *val = (int *)node->inode->data;
	char tmp[8];
	int len = snprintf(tmp, sizeof(tmp), "%c\n", *val ? 'Y' : 'N');
	if (len < 0)
		return -EIO;

	return debugfs_serve_buf(tmp, (usize)len, offset, buf, size);
}

struct vfs_node *b1nix_debugfs_create_bool(const char *name, u16 mode,
                                          struct vfs_node *parent, int *value)
{
	return b1nix_debugfs_create_file(name, mode, parent, value, debugfs_bool_read);
}

static struct vfs_node *debugfs_mount_cb(const char *source, u64 flags, void *data)
{
	(void)source; (void)flags; (void)data;

	struct vfs_node *root = b1nix_debugfs_create_dir("debugfs", NULL);
	if (!root)
		return ERR_PTR(-ENOMEM);

	/* /kheap */
	struct vfs_node *kheap_dir = b1nix_debugfs_create_dir("kheap", root);
	if (kheap_dir) {
		b1nix_debugfs_create_file("stats", 0444, kheap_dir, NULL, debugfs_kheap_stats_read);
	}

	/* /sched */
	struct vfs_node *sched_dir = b1nix_debugfs_create_dir("sched", root);
	if (sched_dir) {
		b1nix_debugfs_create_file("tasks", 0444, sched_dir, NULL, debugfs_sched_tasks_read);
		b1nix_debugfs_create_file("load", 0444, sched_dir, NULL, debugfs_sched_load_read);
	}

	/* /vfs */
	struct vfs_node *vfs_dir = b1nix_debugfs_create_dir("vfs", root);
	if (vfs_dir) {
		b1nix_debugfs_create_file("mounts", 0444, vfs_dir, NULL, debugfs_vfs_mounts_read);
	}

	/* /system */
	struct vfs_node *sys_dir = b1nix_debugfs_create_dir("system", root);
	if (sys_dir) {
		b1nix_debugfs_create_file("version", 0444, sys_dir, NULL, debugfs_system_version_read);
	}

	return root;
}

static struct vfs_fs debugfs_fs = {
	.name = "debugfs",
	.mount = debugfs_mount_cb,
	.flags = VFS_FS_NODEV,
};

static struct vfs_fs tracefs_fs = {
	.name = "tracefs",
	.mount = debugfs_mount_cb,
	.flags = VFS_FS_NODEV,
};

void b1nix_debugfs_init(void)
{
	vfs_register_fs(&debugfs_fs);
	vfs_register_fs(&tracefs_fs);
	klog_info("DebugFS: kernel diagnostic and tracing filesystem registered\n");
}
