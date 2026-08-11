/* SPDX-License-Identifier: MIT */
#ifndef LKPI_ENV_H
#define LKPI_ENV_H

#include <b1nix/types.h>

/*
 * The kernel services the linux shims need, declared without reaching into
 * b1nix's headers.
 *
 * This is the boundary. A translation unit compiling imported DRM source must
 * not see <b1nix/sched.h>, <b1nix/spinlock.h> or <b1nix/mm.h>, because b1nix
 * and Linux share names — spinlock_t, kmalloc, spin_lock, ERR_PTR — with
 * different meanings, and any translation unit that sees both has to be rescued
 * by include order. That worked until a struct member happened to be declared
 * before the rescuing header was reached, and the failure surfaced as a pointer
 * type mismatch three files away.
 *
 * So: headers on this side declare, the .c files behind them include b1nix and
 * forward. <b1nix/types.h> is the one exception, and only because it is
 * nothing but typedefs.
 */

/* ── time ───────────────────────────────────────────────────────── */
u64 lkpi_ticks(void);            /* scheduler ticks, 10 ms each */
void lkpi_sleep_ticks(u64 ticks); /* parks; must not be called atomically */

/* ── scheduling ─────────────────────────────────────────────────── */
void lkpi_yield(void);
/* 1 when the caller may park: not early boot, not an interrupt handler. */
int lkpi_can_block(void);
/* One spin iteration for a caller that cannot park: pause plus TLB service. */
void lkpi_cpu_relax(void);

/* ── CPUs ───────────────────────────────────────────────────────── */
u32 lkpi_cpu_id(void);
u32 lkpi_cpu_count(void);

/* ── interrupts ─────────────────────────────────────────────────── */
u64 lkpi_irq_save(void);

/* Non-preemptible region. Nested; the outermost enable restores the interrupt
 * state the outermost disable saw, rather than assuming it was enabled. */
void lkpi_preempt_disable(void);
void lkpi_preempt_enable(void);
void lkpi_irq_restore(u64 flags);

/*
 * Enable interrupts outright, rather than restoring a saved state.
 *
 * Imported code spells this local_irq_enable(), and it means exactly this: turn
 * them on, no matter what was saved. It cannot be expressed as a restore — the
 * flags word here is the whole of RFLAGS, so restoring an invented value both
 * fails to set IF and overwrites every other flag with it.
 */
void lkpi_irq_enable(void);
int lkpi_irqs_enabled(void);

/* Monotonic nanoseconds from the TSC — real resolution, not tick-rounded.
 * Imported drivers time hardware out with it; see the note in env.c. */
u64 lkpi_monotonic_ns(void);

/* Busy-wait for microseconds, timed against the TSC. See the note in env.c. */
void lkpi_udelay(u64 usecs);

/* Device interrupts taken so far. */
u64 lkpi_device_irq_count(void);


/* ── two-phase wait ─────────────────────────────────────────────
 *
 * Publish on the channel, re-test the condition, then park. Splitting it this
 * way is what closes the lost-wakeup race: a wake landing between the test and
 * the park is already visible to the re-test.
 */
void lkpi_wait_prepare(void *chan);
void lkpi_wait_prepare_timeout(void *chan, u64 timeout_ticks);
void lkpi_wait_commit(void);
void lkpi_wait_cancel(void);
void lkpi_wake_all(void *chan);

/* ── the calling task ───────────────────────────────────────────
 *
 * What imported code reads off `current`: its pid, its thread-group id and its
 * name. Filled from b1nix's own current task, so these are the real values
 * rather than placeholders — and the struct is deliberately small, because
 * anything more would be inventing a task model the DRM core does not need.
 */
struct mm_struct;
struct lkpi_task {
	int pid;
	int tgid;
	char comm[16];
	/* The calling process's address space. b1nix's is not a struct mm_struct
	 * and nothing here can walk it from another task — see find_vma() in
	 * <linux/mm.h> — so this is always NULL and exists because imported code
	 * passes current->mm along to a function that must fail to link. */
	struct mm_struct *mm;
};

struct lkpi_task *lkpi_current(void);

/* ── userspace access ───────────────────────────────────────────── */
/* Return 0 on success, non-zero on fault — validated against the calling
 * process's mappings, which is why imported code must never dereference a
 * __user pointer itself. */
int lkpi_copy_from_user(void *dst, const void *user_src, usize n);
int lkpi_copy_to_user(void *user_dst, const void *src, usize n);

/* ── descriptors ────────────────────────────────────────────────
 *
 * A descriptor is a b1nix vfs_handle in the calling process's table, carrying
 * the caller's object in its private slot. The handle is opaque here on
 * purpose: <b1nix/vfs.h> and <b1nix/sched.h> cannot be included alongside the
 * linux headers — b1nix has its own `current` and `task_tgid`, and this side
 * defines both — so the bridge crosses through these calls instead.
 */
void *lkpi_handle_alloc(void);
void lkpi_handle_release(void *handle);
void *lkpi_handle_private(void *handle);
void lkpi_handle_set_private(void *handle, void *priv);

/* Install a handle in the calling process's table. Returns the descriptor, or
 * negative on failure — in which case the handle is still the caller's. */
int lkpi_fd_install(void *handle);
void *lkpi_fd_lookup(int fd);
void lkpi_fd_close(int fd);

/* 1 when the kernel was booted with b1nix.test=1. */
int lkpi_test_mode(void);

/* ── /sys ───────────────────────────────────────────────────────
 *
 * Attribute files, registered at runtime. The handles are opaque `struct
 * sysfs_dir *` from <b1nix/sysfs_attr.h>: naming that type here would drag
 * b1nix's headers into every translation unit that publishes an attribute,
 * which is the thing this boundary exists to prevent.
 *
 * A NULL parent means directly under /sys. Naming an existing directory returns
 * it rather than creating a second one.
 */
void *lkpi_sysfs_dir(void *parent, const char *name);
/* `release` is called once when the attribute goes away, individually or with
 * its directory: the registry owns nothing behind `ctx`, so the caller says
 * how to free it. Without it, a driver that unbinds leaks one context per
 * attribute it published. */
int lkpi_sysfs_attr(void *dir, const char *name, u32 mode,
                    isize (*show)(void *ctx, char *buf, usize cap),
                    isize (*store)(void *ctx, const char *buf, usize len),
                    void *ctx, void (*release)(void *ctx));
/* The offset-aware read: given where the reader is, rather than served a window
 * of a value rendered whole. A dump larger than one buffer needs this. */
int lkpi_sysfs_attr_at(void *dir, const char *name, u32 mode,
                       isize (*read_at)(void *ctx, char *buf, usize cap,
                                        u64 offset),
                       isize (*store)(void *ctx, const char *buf, usize len),
                       void *ctx, void (*release)(void *ctx));
int lkpi_sysfs_attr_remove(void *dir, const char *name);
int lkpi_sysfs_link_remove(void *dir, const char *name);
/* A symbolic link named `name` in `dir`, pointing at another registered
 * directory. /sys is full of these — a class entry points at its device. */
int lkpi_sysfs_link(void *dir, const char *name, void *target_dir);
/* Look a directory up without creating it. NULL if absent. */
void *lkpi_sysfs_find(void *parent, const char *name);
void lkpi_sysfs_remove(void *dir);
/* The directory `dir` sits in, or NULL when it is directly under /sys. */
void *lkpi_sysfs_parent(void *dir);
/* /sys/kernel/debug, created on first use. */
void *lkpi_sysfs_debug_root(void);

/* Formatting, for attribute values. Same contract as snprintf: always
 * NUL-terminates, returns the length that would have been written. */
int lkpi_snprintf(char *buf, usize cap, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
/* The va_list form. Returns what was written, not what would have been: b1nix's
 * vsnprintf stops counting at truncation, and callers here are written against
 * that rather than against C99 — see the comment on kvasprintf. */
int lkpi_vsnprintf(char *buf, usize cap, const char *fmt, __builtin_va_list ap);

/* ── the scanout ────────────────────────────────────────────────
 *
 * What a display driver on the imported core ultimately writes to. b1nix's own
 * virtio-gpu driver owns the device; this is the one call that crosses from a
 * DRM plane update to it.
 */
int lkpi_scanout_ready(void);
void lkpi_scanout_mode(u32 *width, u32 *height);
/* A full frame of 32-bit XRGB pixels, `width` * `height`, presented as the
 * whole visible area. Returns 0 on success. */
int lkpi_scanout_present(const u32 *pixels, u32 width, u32 height);

/* ── diagnostics ────────────────────────────────────────────────── */
void lkpi_panic(const char *message) __attribute__((noreturn));


/* ── PCI windows ────────────────────────────────────────────────
 *
 * One decoded BAR, so imported code can be told where a device's registers and
 * aperture actually are. b1nix sizes a BAR by writing all-ones and reading the
 * mask back, restoring the original — the real geometry, not a guess, which
 * matters because a driver maps exactly this range and a wrong length either
 * truncates the aperture or maps memory that belongs to something else.
 *
 * Returns 1 when the BAR is implemented (and fills the outputs), 0 when it is
 * not — including the upper half of a 64-bit BAR, which is not a window of its
 * own and must not be counted as one.
 */
int lkpi_pci_bar(u32 bus, u32 slot, u32 func, u32 index, u64 *start, u64 *size,
                 u32 *is_io);

/* Whether the PAT is programmed and write-combining is available through it —
 * M98's work. A driver asks before choosing WC for an aperture; if the answer
 * were wrong in the optimistic direction it would map uncached memory as WC and
 * see stale pixels. */
int lkpi_pat_enabled(void);

/* ── machine memory ─────────────────────────────────────────────────
 *
 * Totals from b1nix's physical allocator, in pages. Imported code sizes caches
 * and shrinker targets against these, so they are real numbers rather than a
 * constant: a driver told the machine has no memory shrinks to nothing.
 */
u64 lkpi_total_pages(void);
u64 lkpi_free_pages(void);

/* Is a reschedule pending on this CPU? Set by the timer tick; imported code
 * checks it inside long loops to decide when to yield. */
int lkpi_need_resched(void);

/* Is the calling thread b1nix's page-reclaim thread? A driver's shrinker asks
 * so it can avoid recursing into reclaim from inside reclaim. */
int lkpi_is_kswapd(void);


/* ── the host-visible display ───────────────────────────────────────
 *
 * b1nix's own scanout — the virtio-gpu framebuffer, which in a VM is the window
 * on screen. A driver that renders into its own framebuffer can be mirrored
 * there, which is the only way to see what a passed-through GPU produced when
 * nothing is plugged into it.
 *
 * get_mode returns 0 when there is no such display.
 */
int lkpi_display_get_mode(u32 *width, u32 *height);
int lkpi_display_present(const u32 *pixels, u32 width, u32 height);

/* A kernel command-line flag, by name. 1 when present. */
int lkpi_bootflag(const char *flag);

#endif
