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
	/* Size the page from the number of mounts, not from a fixed 4 KiB.
	 *
	 * A mount line is source + target + fstype + flags, and source and target
	 * are VFS_MAX_PATH each, so a fixed buffer silently stopped listing once
	 * enough filesystems were mounted -- and the entries it dropped were the
	 * NEWEST ones, which is precisely what M119 looks for: the test mounts
	 * debugfs and then expects to find it here. */
	isize total = vfs_mounts_info(NULL, 0);
	usize want = total > 0 ? (usize)total : 0;
	if (want > 256)
		want = 256;
	/* Budget by a typical line, not by the worst case. Sources and targets are
	 * VFS_MAX_PATH-sized fields but hold short strings; reserving 2*256 per
	 * mount asked the heap for well over a hundred kilobytes, and a read that
	 * fails with -ENOMEM tells the caller nothing at all. */
	usize line_max = 96;
	usize page_size = 128 + want * line_max;
	if (page_size < DEBUGFS_BUF_SIZE)
		page_size = DEBUGFS_BUF_SIZE;
	if (page_size > 32768)
		page_size = 32768;

	char *page = kmalloc(page_size);
	if (!page) {
		static volatile int oom_reported;
		if (!__atomic_exchange_n(&oom_reported, 1, __ATOMIC_ACQ_REL)) {
			console_write("debugfs/mounts: no memory for ");
			console_write_dec((u64)page_size);
			console_write(" bytes (total=");
			console_write_dec((u64)total);
			console_write(")\n");
		}
		return -ENOMEM;
	}

	int pos = snprintf(page, page_size,
	                   "%-16s %-24s %-12s %s\n",
	                   "SOURCE", "TARGET", "FSTYPE", "FLAGS");

	/* Size the array from the answer, and never iterate past what was
	 * actually filled in.
	 *
	 * vfs_mounts_info() returns the TOTAL number of visible mounts, not how
	 * many it wrote -- snprintf's convention. This read asked for 64 and then
	 * looped over the returned count, so on a system with more than 64 mounts
	 * it walked straight off the end of the array and formatted whatever
	 * followed it in the heap. It also meant the newest mounts were simply
	 * absent from the listing, which is what made M119 fail: the test mounts
	 * debugfs and then looks for it here, and its own entry is the last one. */
	struct vfs_mount_info *info =
	    want ? kmalloc(sizeof(struct vfs_mount_info) * want) : NULL;
	if (!info) {
		static volatile int info_reported;
		if (!__atomic_exchange_n(&info_reported, 1, __ATOMIC_ACQ_REL)) {
			console_write("debugfs/mounts: no memory for ");
			console_write_dec((u64)(sizeof(struct vfs_mount_info) * want));
			console_write(" bytes of mount info (total=");
			console_write_dec((u64)total);
			console_write(")\n");
		}
	}
	if (info) {
		isize count = vfs_mounts_info(info, want);
		if (count > (isize)want)
			count = (isize)want;
		for (isize i = 0; i < count && pos < (int)(page_size - 640); i++) {
			pos += snprintf(page + pos, page_size - pos,
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
