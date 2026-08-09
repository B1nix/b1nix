/*
 * SPDX-License-Identifier: MIT
 *
 * M101 linuxkpi: the boundary between imported code and b1nix.
 *
 * Every function here is a one-line forward. The value is not what they do but
 * where they sit: this is the only file on the shim side that includes both
 * b1nix's headers and lkpi's, so it is the only place the two naming worlds
 * meet. See <lkpi/env.h> for why that matters.
 */

#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/klog.h>
#include <b1nix/lapic.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/sysfs_attr.h>
#include <b1nix/virtio_gpu.h>
#include <b1nix/tlb.h>
#include <b1nix/vfs.h>
#include <lkpi/env.h>
#include <stdarg.h>
#include <stdio.h>

u64 lkpi_ticks(void) { return scheduler_get_ticks(); }

void lkpi_sleep_ticks(u64 ticks) { scheduler_sleep_ticks(ticks); }

void lkpi_yield(void) { scheduler_yield(); }

int lkpi_can_block(void) { return scheduler_can_block(); }

void lkpi_cpu_relax(void)
{
	__asm__ volatile("pause");
	/* Servicing shootdowns here is not optional: a caller spinning with
	 * interrupts disabled would otherwise leave the CPU that sent one waiting
	 * forever. */
	tlb_shootdown_poll();
}

u32 lkpi_cpu_id(void)
{
	struct percpu *pc = get_percpu();
	return pc ? pc->cpu_id : 0;
}

u32 lkpi_cpu_count(void) { return (u32)g_max_cpus; }

u64 lkpi_irq_save(void) { return interrupts_save(); }

void lkpi_irq_restore(u64 flags) { interrupts_restore(flags); }

int lkpi_irqs_enabled(void) { return interrupts_enabled(); }

void lkpi_wait_prepare(void *chan) { scheduler_wait_prepare(chan); }

void lkpi_wait_prepare_timeout(void *chan, u64 timeout_ticks)
{
	scheduler_wait_prepare_timeout(chan, timeout_ticks);
}

void lkpi_wait_commit(void) { scheduler_wait_commit(); }

void lkpi_wait_cancel(void) { scheduler_wait_cancel(); }

void lkpi_wake_all(void *chan) { scheduler_wake_all(chan); }

/*
 * A per-CPU snapshot rather than a pointer into b1nix's task: the caller may
 * hold it across a sleep, and a task can exit in that window. Copying costs a
 * few stores and removes the lifetime question entirely.
 */
static struct lkpi_task g_lkpi_task[MAX_CPUS];

struct lkpi_task *lkpi_current(void)
{
	struct percpu *pc = get_percpu();
	u32 cpu = pc && pc->cpu_id < MAX_CPUS ? pc->cpu_id : 0;
	struct lkpi_task *t = &g_lkpi_task[cpu];

	struct task *cur = current_task;
	if (cur) {
		t->pid = (int)cur->id;
		t->tgid = (int)cur->id;
		const char *name = cur->name;
		usize i = 0;
		for (; name && name[i] && i < sizeof(t->comm) - 1; i++)
			t->comm[i] = name[i];
		t->comm[i] = 0;
	} else {
		t->pid = 0;
		t->tgid = 0;
		t->comm[0] = 0;
	}
	return t;
}

int lkpi_copy_from_user(void *dst, const void *user_src, usize n)
{
	return syscall_copyin(dst, user_src, n);
}

int lkpi_copy_to_user(void *user_dst, const void *src, usize n)
{
	return syscall_copyout(user_dst, src, n);
}

int lkpi_test_mode(void) { return bootinfo_has_flag("b1nix.test=1"); }

void lkpi_panic(const char *message) { panic(message); }

/* ── descriptors ────────────────────────────────────────────────── */

void *lkpi_handle_alloc(void)
{
	/* VFS_HANDLE_NODE rather than a new kind: the handle carries the caller's
	 * object in private_data and never reaches the node paths, so inventing a
	 * kind would add a case nothing switches on. */
	struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_NODE);
	if (h)
		h->private_data = 0;
	return h;
}

void lkpi_handle_release(void *handle)
{
	if (handle)
		vfs_handle_release((struct vfs_handle *)handle);
}

void *lkpi_handle_private(void *handle)
{
	return handle ? ((struct vfs_handle *)handle)->private_data : 0;
}

void lkpi_handle_set_private(void *handle, void *priv)
{
	if (handle)
		((struct vfs_handle *)handle)->private_data = priv;
}

int lkpi_fd_install(void *handle)
{
	return handle ? scheduler_fd_alloc((struct vfs_handle *)handle) : -1;
}

void *lkpi_fd_lookup(int fd)
{
	return scheduler_fd_get(fd);
}

void lkpi_fd_close(int fd)
{
	scheduler_fd_close(fd);
}

/* ── /sys ───────────────────────────────────────────────────────── */

void *lkpi_sysfs_dir(void *parent, const char *name)
{
  return sysfs_reg_dir((struct sysfs_dir *)parent, name);
}

int lkpi_sysfs_attr(void *dir, const char *name, u32 mode,
                    isize (*show)(void *ctx, char *buf, usize cap),
                    isize (*store)(void *ctx, const char *buf, usize len),
                    void *ctx, void (*release)(void *ctx))
{
  return sysfs_reg_attr((struct sysfs_dir *)dir, name, (u16)mode, show, store,
                        ctx, release);
}

int lkpi_sysfs_attr_at(void *dir, const char *name, u32 mode,
                       isize (*read_at)(void *ctx, char *buf, usize cap,
                                        u64 offset),
                       isize (*store)(void *ctx, const char *buf, usize len),
                       void *ctx, void (*release)(void *ctx))
{
  return sysfs_reg_attr_at((struct sysfs_dir *)dir, name, (u16)mode, read_at,
                           store, ctx, release);
}

int lkpi_sysfs_attr_remove(void *dir, const char *name)
{
  return sysfs_reg_attr_remove((struct sysfs_dir *)dir, name);
}

int lkpi_sysfs_link_remove(void *dir, const char *name)
{
  return sysfs_reg_link_remove((struct sysfs_dir *)dir, name);
}

int lkpi_sysfs_link(void *dir, const char *name, void *target_dir)
{
  char path[256];
  isize len = sysfs_reg_path((struct sysfs_dir *)target_dir, path, sizeof(path));
  if (len < 0)
    return (int)len;
  return sysfs_reg_link((struct sysfs_dir *)dir, name, path);
}

void *lkpi_sysfs_find(void *parent, const char *name)
{
  return sysfs_reg_find((struct sysfs_dir *)parent, name);
}

void lkpi_sysfs_remove(void *dir) { sysfs_reg_remove((struct sysfs_dir *)dir); }

void *lkpi_sysfs_parent(void *dir)
{
  return sysfs_reg_parent((struct sysfs_dir *)dir);
}

void *lkpi_sysfs_debug_root(void) { return sysfs_reg_debug_root(); }

int lkpi_snprintf(char *buf, usize cap, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, cap, fmt, ap);
  va_end(ap);
  return n;
}

int lkpi_vsnprintf(char *buf, usize cap, const char *fmt, __builtin_va_list ap)
{
  return vsnprintf(buf, cap, fmt, ap);
}

/* ── the scanout ────────────────────────────────────────────────── */

int lkpi_scanout_ready(void) { return virtio_gpu_ready(); }

void lkpi_scanout_mode(u32 *width, u32 *height)
{
  virtio_gpu_get_mode(width, height);
}

int lkpi_scanout_present(const u32 *pixels, u32 width, u32 height)
{
  /* The whole frame is dirty: a plane update carries no damage rectangle here,
   * and claiming a smaller one would leave stale pixels on screen. The cursor
   * arguments say "leave it as it is" rather than moving or hiding it. */
  return virtio_gpu_present(pixels, width, height, 0, 0, width, height, -1, -1,
                            0);
}
