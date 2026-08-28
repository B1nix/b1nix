/*
 * SPDX-License-Identifier: GPL-2.0-only
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
#include <b1nix/memtype.h>
#include <b1nix/mm.h>
#include <string.h>

#include <b1nix/pci.h>
#include <b1nix/arch_x86_64.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/sysfs_attr.h>
#include <b1nix/uidgid.h>
#include <b1nix/virtio_gpu.h>
#include <b1nix/tlb.h>
#include <b1nix/drm.h>
#include <b1nix/vfs.h>
#include <lkpi/env.h>
#include <stdarg.h>
#include <stdio.h>

/* A jiffy, in the units the imported code was written for.
 *
 * linux/jiffies.h fixes HZ at 100 — imported code converts through
 * msecs_to_jiffies and is right whatever the real rate is, as long as the
 * counter it reads advances at the rate HZ claims. This used to be the raw
 * scheduler tick because the scheduler ran at 100 Hz too, and when the timer
 * was reprogrammed to 1 kHz that quiet coincidence broke every timeout in the
 * driver: a two-second wait expired in two hundred milliseconds, and an i915
 * request that the hardware had already completed came back as a timeout.
 *
 * So scale, rather than assume. A tick rate that is not a multiple of HZ
 * simply rounds; a rate below it cannot happen (the timer is never programmed
 * slower than the PIT's 100 Hz), and the max() keeps the divisor sane if it
 * ever were. */
u64 lkpi_ticks(void)
{
	u32 hz = sched_tick_hz();
	u32 per_jiffy = hz / 100u;

	if (per_jiffy < 1u)
		per_jiffy = 1u;
	return scheduler_get_ticks() / per_jiffy;
}

/* ── stuck-call watchdog ────────────────────────────────────────
 *
 * The watched call, and the last place it went to sleep. Both are written by
 * the watched task itself and read by a watchdog thread on another CPU, so
 * everything here is volatile and nothing is a pointer the reader dereferences:
 * the reader only compares the task pointer and prints numbers.
 */
static void *volatile g_watch_task;
static const char *volatile g_watch_what;
static volatile u64 g_watch_detail;
static volatile u64 g_watch_start_ns;
static volatile u64 g_watch_site;
static volatile u64 g_watch_frame;
static volatile u64 g_watch_parks;

static void watch_note_park(u64 site, u64 frame)
{
  if (!g_watch_task || g_watch_task != (void *)current_task)
    return;
  g_watch_site = site;
  g_watch_frame = frame;
  g_watch_parks++;
}

void lkpi_diag_watch_begin(const char *what, u64 detail)
{
  g_watch_what = what;
  g_watch_detail = detail;
  g_watch_start_ns = lkpi_monotonic_ns();
  g_watch_site = 0;
  g_watch_frame = 0;
  g_watch_parks = 0;
  /* Last, so a reader that sees a task also sees the rest of the record. */
  g_watch_task = (void *)current_task;
}

void lkpi_diag_watch_end(void) { g_watch_task = 0; }

int lkpi_diag_watch_report(u64 min_ms)
{
  void *task = g_watch_task;
  u64 ms;

  if (!task)
    return 0;
  ms = (lkpi_monotonic_ns() - g_watch_start_ns) / 1000000ull;
  if (ms < min_ms)
    return 0;

  console_write("lkpi: ");
  console_write(g_watch_what ? g_watch_what : "call");
  console_write(" 0x");
  console_write_hex64(g_watch_detail);
  console_write(" still running after ");
  console_write_dec(ms);
  console_write(" ms, ");
  console_write_dec(g_watch_parks);
  console_write(" park(s)");
  if (!g_watch_parks) {
    /* Never parked: it is spinning, not waiting, and the frame recorded below
     * would be from an older call. Say so rather than print a stale trace. */
    console_write(" — spinning, not parked\n");
    return 1;
  }
  console_write(", last parked at 0x");
  console_write_hex64(g_watch_site);
  ksym_print(g_watch_site);
  console_write("\n");
  arch_backtrace(g_watch_frame, g_watch_site);
  return 1;
}

/* Sleep for a number of JIFFIES, not scheduler ticks.
 *
 * Every caller counts in jiffies — msleep converts milliseconds at HZ, and
 * schedule_timeout is handed a jiffy count straight out of imported code — and
 * this used to pass that number to the scheduler as if a jiffy were a tick.
 * That was true while both were 10 ms and became a tenfold error the moment the
 * timer moved to 1 kHz: every driver timeout expired ten times too early, and
 * an i915 request the hardware had already completed came back as a timeout
 * because the wait for it lasted 200 ms instead of two seconds. */
/* Wake a task that parked in schedule_timeout, named by the snapshot above.
 *
 * The pid in that snapshot is b1nix's task id, and waking by id is safe against
 * the task having exited in the meantime: the scheduler finds no live task and
 * does nothing. */
void lkpi_prepare_to_sleep(void)
{
	lkpi_current()->wake_pending = 0;
}

int lkpi_wake_task(struct lkpi_task *t)
{
	if (!t || t->pid <= 0)
		return 0;
	/* Record it first. If the target has not parked yet the state CAS below
	 * finds it RUNNING and does nothing, and this flag is the only thing that
	 * keeps the wake from being lost. */
	t->wake_pending = 1;
	/* Without the runqueue: this runs from fence callbacks, and those run from
	 * interrupt handlers. See scheduler_wake_task_norq. */
	scheduler_wake_task_norq((usize)t->pid);
	return 1;
}

u64 lkpi_sleep_jiffies(u64 jiffies_count)
{
	u32 hz = sched_tick_hz();
	u32 per_jiffy = hz / 100u;
	u64 ticks, deadline, now;

	if (per_jiffy < 1u)
		per_jiffy = 1u;
	ticks = jiffies_count * per_jiffy;
	deadline = scheduler_get_ticks() + ticks;
	{
  watch_note_park((u64)(usize)__builtin_return_address(0),
                  (u64)(usize)__builtin_frame_address(0));
  /*
   * A sleep abandons a park that was armed and never committed.
   *
   * Imported code publishes itself on a wait queue with prepare_to_wait() and
   * then, on some paths, sleeps for a fixed time instead of calling schedule()
   * — a delay in the middle of a modeset, for instance. b1nix's sleep refuses
   * to run on a task that is marked blocked, and rightly: the two states are
   * different things. Dropping the arm here is what the caller meant, and it
   * also puts the interrupt state back the way prepare_to_wait found it.
   */
  if (scheduler_wait_armed())
    scheduler_wait_cancel();
	}

	/*
	 * Sleep in slices, checking for a wake between them.
	 *
	 * A single long sleep can only be cut short by the state CAS a waker
	 * performs, and that CAS misses a task that has not parked yet — the
	 * window between "decided to sleep" and "published SLEEPING". Slicing
	 * bounds that miss to one slice instead of the whole timeout, and the flag
	 * catches it on the very next check. One jiffy is the resolution the
	 * imported code asked for anyway.
	 */
	{
		struct lkpi_task *self = lkpi_current();
		u64 left, slice;

		/*
		 * One slice, then back to the caller with the remainder.
		 *
		 * schedule_timeout() is allowed to return before its deadline — every
		 * caller in the imported tree is a loop that re-tests its condition and
		 * sleeps again, precisely because a wake can be spurious. Sleeping the
		 * whole timeout in one go turns that loop into a single shot, and a
		 * condition that becomes true while the task sleeps is then not noticed
		 * until the deadline: i915_request_wait slept a full second on a
		 * request the GPU had finished in microseconds, because the only thing
		 * that would have re-checked it was the loop it never returned to.
		 *
		 * A jiffy is the resolution the caller asked for, so a slice is a
		 * jiffy. A wake posted before the sleep is honoured immediately.
		 */
		if (self->wake_pending) {
			self->wake_pending = 0;
		} else {
			now = scheduler_get_ticks();
			if (now < deadline) {
				left = deadline - now;
				slice = left < per_jiffy ? left : per_jiffy;
				scheduler_sleep_ticks(slice);
			}
		}
	}

	/*
	 * How much of the sleep was left.
	 *
	 * schedule_timeout() returns the remainder, and zero means — to every
	 * caller in the imported tree — that the wait expired. Returning zero
	 * unconditionally, as this used to, turned every early wake into a
	 * reported timeout: i915_request_wait was woken the moment its fence
	 * signalled, saw a remainder of zero, and answered -ETIME for a request
	 * the hardware had already retired.
	 */
	now = scheduler_get_ticks();
	if (now >= deadline)
		return 0;
	return (deadline - now) / per_jiffy;
}

void lkpi_yield(void)
{
  watch_note_park((u64)(usize)__builtin_return_address(0),
                  (u64)(usize)__builtin_frame_address(0));
  scheduler_yield();
}

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

/* Where interrupts were last turned off on this CPU.
 *
 * "Cannot block because interrupts are off" is useless without the site that
 * turned them off — the code that trips over it is usually several frames away
 * and imported, so a backtrace from the complaint names the wrong function. */
static u64 g_irq_off_site[MAX_CPUS];

u64 lkpi_irq_off_site(void)
{
	struct percpu *pc = get_percpu();
	u32 cpu = pc && pc->cpu_id < MAX_CPUS ? pc->cpu_id : 0;

	return g_irq_off_site[cpu];
}

/* Cleared when they come back on, so a non-zero site means "still off, from
 * here" rather than "was off at some point". */
void lkpi_note_irq_on(void)
{
	struct percpu *pc = get_percpu();
	u32 cpu = pc && pc->cpu_id < MAX_CPUS ? pc->cpu_id : 0;

	g_irq_off_site[cpu] = 0;
}

void lkpi_note_irq_off(u64 site)
{
	struct percpu *pc = get_percpu();
	u32 cpu = pc && pc->cpu_id < MAX_CPUS ? pc->cpu_id : 0;

	g_irq_off_site[cpu] = site;
}

u64 lkpi_irq_save(void)
{
	int was_on = interrupts_enabled();
	u64 flags = interrupts_save();

	if (was_on)
		lkpi_note_irq_off((u64)(usize)__builtin_return_address(0));
	return flags;
}

void lkpi_irq_restore(u64 flags)
{
	interrupts_restore(flags);
	if (interrupts_enabled())
		lkpi_note_irq_on();
}

void lkpi_irq_enable(void)
{
	interrupts_enable();
	lkpi_note_irq_on();
}

int lkpi_irqs_enabled(void) { return interrupts_enabled(); }

void lkpi_wait_prepare(void *chan) { scheduler_wait_prepare(chan); }

void lkpi_wait_prepare_timeout(void *chan, u64 timeout_ticks)
{
	scheduler_wait_prepare_timeout(chan, timeout_ticks);
}

void lkpi_wait_commit(void)
{
  watch_note_park((u64)(usize)__builtin_return_address(0),
                  (u64)(usize)__builtin_frame_address(0));
  scheduler_wait_commit();
}

void lkpi_wait_cancel(void) { scheduler_wait_cancel(); }

void lkpi_schedule(void)
{
  watch_note_park((u64)(usize)__builtin_return_address(0),
                  (u64)(usize)__builtin_frame_address(0));
  /* The second phase of a park, when one was armed; otherwise what a bare
   * schedule() asks for. See the note on schedule() in <linux/sched.h>. */
  if (scheduler_wait_armed())
    scheduler_wait_commit();
  else
    scheduler_yield();
}

void lkpi_wake_all(void *chan)
{
	scheduler_wake_all(chan);
	/*
	 * And the pollers.
	 *
	 * Imported code wakes its own wait queue; b1nix's poll() sleeps on one
	 * shared channel and re-tests readiness when woken, so a queue it has never
	 * heard of leaves it asleep with the event already waiting. That is what
	 * kept a compositor blocked in poll() on the card: the page-flip completion
	 * was queued, readable, and nobody told the sleeper. Waking the poll channel
	 * costs a re-test that finds nothing when the wake was for something else.
	 */
	scheduler_wake_all(vfs_poll_chan);
}

/*
 * A per-CPU snapshot rather than a pointer into b1nix's task: the caller may
 * hold it across a sleep, and a task can exit in that window. Copying costs a
 * few stores and removes the lifetime question entirely.
 */
/* One per task slot, not one per CPU.
 *
 * A per-CPU snapshot is stable only until the next task on that CPU asks for
 * `current`, and imported code holds the pointer across a sleep on purpose:
 * i915_request_wait saves it and has the fence callback wake it. With the
 * snapshot shared per CPU that callback woke whichever task had most recently
 * run there, so the waiter slept out its full timeout with its request long
 * since complete — two seconds per engine, and the GT read as dead.
 *
 * Indexed by the scheduler's own task slot, so it is stable for the life of the
 * task and reused only when the slot is. */
static struct lkpi_task g_lkpi_task[4096];

struct lkpi_task *lkpi_current(void)
{
	usize slot = scheduler_current_slot();
	struct lkpi_task *t;

	if (slot >= sizeof(g_lkpi_task) / sizeof(g_lkpi_task[0]))
		slot = 0;
	t = &g_lkpi_task[slot];

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

void lkpi_handle_inherit_node(void *handle, void *source)
{
	struct vfs_handle *h = (struct vfs_handle *)handle;
	struct vfs_handle *src = (struct vfs_handle *)source;

	if (!h || !src || !src->node || h->node)
		return;
	h->node = vfs_node_get(src->node);
	h->ops = src->ops;
	h->flags = src->flags;
}

void lkpi_handle_attach_drm_minor(void *handle, u32 minor)
{
	struct vfs_handle *h = (struct vfs_handle *)handle;
	struct vfs_node *node;

	(void)node;
	if (!h)
		return;
	drm_card_attach_handle(h, minor);
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

/* ── capabilities ────────────────────────────────────────────────── */

/* Linux's capability numbers are not b1nix's -- CAP_SYS_ADMIN is 21 there and
 * 20 here, and the two lists diverge from CAP_SYS_RAWIO onwards. Imported code
 * passes Linux's, so the translation belongs here, at the one file that is
 * allowed to see both headers. Only the capabilities imported drivers actually
 * ask about are mapped; anything else is refused rather than guessed at, which
 * is the answer a driver can act on safely. */
int lkpi_capable(int cap)
{
  struct cred *c = scheduler_get_current_cred();
  int b1nix_cap;

  if (!c)
    return 0;
  switch (cap) {
  case 17: b1nix_cap = CAP_SYS_RAWIO; break; /* Linux CAP_SYS_RAWIO */
  case 21: b1nix_cap = CAP_SYS_ADMIN; break; /* Linux CAP_SYS_ADMIN */
  case 23: b1nix_cap = CAP_SYS_NICE; break;  /* Linux CAP_SYS_NICE  */
  default: return 0;
  }
  return cred_has_cap(c, b1nix_cap) ? 1 : 0;
}

int lkpi_scanout_pci_id(u16 *vendor, u16 *device, u8 *bus, u8 *slot, u8 *func)
{
  /* Both device ids virtio-gpu is enumerated under, in the order the driver
   * itself looks for them, so the answer is the function the driver bound to
   * and not merely a virtio device that happens to be present. */
  static const u16 ids[] = { 0x1010, 0x1050 };
  struct pci_device_info info;

  for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
    if (!pci_find_device(0x1af4, ids[i], &info))
      continue;
    if (vendor)
      *vendor = 0x1af4;
    if (device)
      *device = ids[i];
    if (bus)
      *bus = info.bus;
    if (slot)
      *slot = info.slot;
    if (func)
      *func = info.func;
    return 0;
  }
  return -ENODEV;
}

/* ── virgl bridge ───────────────────────────────────────────────────────
 *
 * Straight pass-through: the shim adds nothing but the boundary itself, so the
 * driver ioctls above can reach b1nix's virtio-gpu transport without either
 * side including the other's headers.
 */
int lkpi_virgl_available(void) { return virtio_gpu_virgl_available(); }

u32 lkpi_virgl_res_alloc(void) { return virtio_gpu_virgl_res_alloc(); }

int lkpi_virgl_ctx_create(u32 ctx_id)
{
  return virtio_gpu_virgl_ctx_create(ctx_id);
}

int lkpi_virgl_res_create(u32 ctx_id, const struct lkpi_virgl_res_desc *d,
                          const u64 *phys, u32 npages, u32 res_id)
{
  struct virtio_gpu_res_params p;

  if (!d)
    return -1;
  p.target = d->target;
  p.format = d->format;
  p.bind = d->bind;
  p.width = d->width;
  p.height = d->height;
  p.depth = d->depth;
  p.array_size = d->array_size;
  p.last_level = d->last_level;
  p.nr_samples = d->nr_samples;
  p.flags = d->flags;
  return virtio_gpu_virgl_res_create(ctx_id, &p, phys, npages, res_id);
}

int lkpi_virgl_submit(u32 ctx_id, const u32 *cmd, u32 bytes)
{
  return virtio_gpu_virgl_submit(ctx_id, cmd, bytes);
}

int lkpi_virgl_transfer(int to_host, u32 ctx_id, u32 res_id, u32 level,
                        const u32 *box6, u64 offset)
{
  return virtio_gpu_virgl_transfer(to_host, ctx_id, res_id, level, box6, offset);
}

int lkpi_virgl_unref(u32 res_id) { return virtio_gpu_virgl_unref(res_id); }

int lkpi_virgl_capset(u32 index, u32 want_id, u32 want_ver, void *out, u32 *len)
{
  return virtio_gpu_virgl_capset(index, want_id, want_ver, out, len);
}

int lkpi_virgl_capset_ids(u64 *mask) { return virtio_gpu_virgl_capset_ids(mask); }

int lkpi_scanout_present(const u32 *pixels, u32 width, u32 height, u32 dirty_x,
                         u32 dirty_y, u32 dirty_w, u32 dirty_h)
{
  /* The damage rectangle comes from the atomic commit that produced this
   * frame — a caller with nothing better to say passes the whole frame. The
   * cursor arguments say "leave it as it is" rather than moving or hiding it. */
  return virtio_gpu_present(pixels, width, height, dirty_x, dirty_y, dirty_w,
                            dirty_h, -1, -1, 0);
}

/* ── randomness ─────────────────────────────────────────────────── */

/* Imported drivers ask for randomness through <linux/random.h>, which declares
 * this rather than including b1nix's own header: the two sides spell the same
 * names differently, and a translation unit that saw both would not compile.
 * The source is the kernel's, not a second generator. */
u32 lkpi_random_u32(void) { return (u32)kernel_random_u64(); }

/* ── context assertions ─────────────────────────────────────────── */

/*
 * "This function may sleep — is the caller somewhere it can?"
 *
 * Reported rather than fatal: reaching a sleeping call from an atomic context
 * is a bug, but panicking on it would turn a diagnosable problem into a dead
 * machine, and the callers that trip it are usually imported code taking a path
 * b1nix's context rules did not anticipate. The message names the function so
 * the path is identifiable without a debugger.
 */
void lkpi_might_sleep(const char *where)
{
  if (lkpi_can_block())
    return;

  /* The first few get a backtrace and the reason.
   *
   * "Cannot block" has three quite different causes — no scheduler yet, no
   * current task, or interrupts already off — and only the last one points at a
   * caller that disabled them and did not put them back. The trace names that
   * caller; without it the warning says only that something, somewhere, is
   * wrong. */
  {
    static int traced;

    if (traced < 3) {
      traced++;
      console_write("lkpi: cannot block (");
      console_write(current_task ? "" : "no task, ");
      console_write(interrupts_enabled() ? "irqs on" : "irqs off");
      console_write(") in ");
      console_write(where ? where : "?");
      console_write(", task=");
      console_write(current_task && current_task->name ? current_task->name
                                                       : "?");
      console_write(", last irq-off at 0x");
      console_write_hex64(lkpi_irq_off_site());
      ksym_print(lkpi_irq_off_site());
      console_write("\n");
      arch_backtrace((u64)(usize)__builtin_frame_address(0),
                     (u64)(usize)__builtin_return_address(0));
    }
  }

  char line[96];
  lkpi_snprintf(line, sizeof(line),
                "lkpi: might_sleep() in a context that cannot block: %s",
                where ? where : "<unknown>");
  klog_warn(line);
}

/* ── PCI windows ────────────────────────────────────────────────── */

int lkpi_pci_bar(u32 bus, u32 slot, u32 func, u32 index, u64 *start, u64 *size,
                 u32 *is_io)
{
  struct pci_bar bar;

  if (!start || !size || index >= PCI_MAX_BARS)
    return 0;
  if (pci_bar_read((u8)bus, (u8)slot, (u8)func, (u8)index, &bar) != 0)
    return 0;
  if (!bar.valid)
    return 0;

  *start = bar.base;
  *size = bar.size;
  if (is_io)
    *is_io = bar.is_io;
  return 1;
}

/* ── memory types ───────────────────────────────────────────────── */

/* Whether write-combining is really available through the PAT (M98). Answered
 * from the CPU's own capability rather than assumed: a driver that maps an
 * aperture WC when the PAT is not programmed gets uncached memory and blames
 * the GPU for the frame rate. */
int lkpi_pat_enabled(void) { return pat_available(); }

/* One CPUID query per call, the encoding described in <asm/cpufeature.h>.
 * Upstream patches the branch away at boot; this does not, and the cost is a
 * cpuid on a path that upstream made free — measurable, never wrong. */
int lkpi_cpu_has(u32 feature)
{
  u32 leaf = feature >> 8;
  u32 reg = (feature >> 5) & 7;
  u32 bit = feature & 31;
  u32 eax = 0, ebx = 0, ecx = 0, edx = 0;

  __asm__ volatile("cpuid"
                   : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                   : "a"(leaf), "c"(0));

  u32 value = reg == 0 ? eax : reg == 1 ? ebx : reg == 2 ? ecx : edx;
  return (value >> bit) & 1u;
}

/* ── machine memory ─────────────────────────────────────────────────── */

u64 lkpi_total_pages(void)
{
	return pmm_total_usable_memory() / PAGE_SIZE;
}

u64 lkpi_free_pages(void)
{
	return (u64)pmm_free_frame_count();
}

/*
 * Is a reschedule pending?
 *
 * b1nix exposes no such flag, and the two possible constants are not
 * equivalent: answering "no" means a driver's long loop never yields
 * voluntarily, which on a kernel that does not preempt ring 0 cooperatively is
 * a hang. So this answers "yes", and every caller yields once per iteration —
 * slower than upstream, and it cannot wedge.
 */
int lkpi_need_resched(void) { return 1; }

/* The reclaim thread identifies itself by name: kswapd is created with it in
 * pmm.c, and a driver's shrinker only needs to know whether it is being called
 * from inside reclaim. */
int lkpi_is_kswapd(void)
{
	struct task *t = current_task;
	return t && strcmp(t->name, "kswapd") == 0;
}

/* ── preemption ─────────────────────────────────────────────────────
 *
 * b1nix has no preempt count: the timer ISR yields whenever the current task is
 * RUNNING, so the only way to make a region non-preemptible is to disable
 * interrupts. That is stronger than Linux's preempt_disable and never weaker,
 * which is safe in that direction only.
 *
 * What it must NOT do is enable interrupts on the way out when the caller had
 * them off. The earlier version restored a hardcoded IF, and i915 calls this
 * pair inside _wait_for_atomic() while holding uncore->lock with interrupts
 * disabled — so the GPU's own MSI arrived on that CPU, its handler took
 * uncore->lock, and the boot CPU deadlocked against itself on real hardware.
 *
 * So the state is saved and nested: only the outermost enable restores, and it
 * restores what the outermost disable actually saw. The scheduler keeps that
 * count now, so there is nothing to keep here.
 */

/*
 * Preemption off, interrupts ON — which is what Linux means by this.
 *
 * Disabling interrupts as well looks like the safer reading and is not: inside
 * such a region no timer tick arrives, so jiffies stand still and any wait with
 * a timeout runs forever. i915 enters one for a two-microsecond poll of the
 * GMBUS status and then falls back to a fifty-millisecond wait — with the clock
 * stopped, that second wait never ends, and connector probing hung there with a
 * compositor waiting on it.
 *
 * The scheduler honours the count instead: the timer still fires, still
 * accounts, and simply does not take the CPU away until the region ends.
 */
void lkpi_preempt_disable(void)
{
	scheduler_preempt_disable();
}

void lkpi_preempt_enable(void)
{
	scheduler_preempt_enable();
}

/* ── the host-visible display ───────────────────────────────────────
 *
 * b1nix's virtio-gpu scanout, which in a VM is the window the user actually
 * sees. Imported code cannot reach b1nix's own drivers directly — see the note
 * at the top of this file — so these are the bridge.
 */

int lkpi_display_get_mode(u32 *width, u32 *height)
{
	if (!virtio_gpu_ready())
		return 0;
	virtio_gpu_get_mode(width, height);
	return 1;
}

int lkpi_display_present(const u32 *pixels, u32 width, u32 height)
{
	if (!pixels || !virtio_gpu_ready())
		return 0;
	/* Whole-frame update: this is a mirror, so there is no dirty tracking to
	 * inherit from the source. No cursor either — the source framebuffer has
	 * whatever cursor was composited into it. */
	return virtio_gpu_present(pixels, width, height, 0, 0, width, height,
	                          0, 0, 0) == 0;
}

/* A kernel command-line flag, for imported code that cannot reach b1nix's
 * bootinfo directly. */
int lkpi_bootflag(const char *flag)
{
	return bootinfo_has_flag(flag);
}

/*
 * Monotonic nanoseconds, from the TSC.
 *
 * The obvious source is the scheduler tick, and that is what this used to be:
 * ticks × 10 ms. It reads as a detail about resolution, but imported drivers do
 * not use this clock to stamp events — they use it to time out hardware. i915
 * polls a GMBUS transfer with a 2 µs deadline first and a 50 ms one after, and
 * on a clock that only moves in 10 ms steps the short deadline is either zero
 * or a whole step depending on where the tick boundary happened to fall. The
 * result was a bus that worked being declared timed out, and a monitor that was
 * plugged in being reported as absent — in some boots and not others, which is
 * what a phase relationship looks like from the outside.
 *
 * The TSC is invariant on every CPU this kernel targets and its frequency is
 * already calibrated, so this is a real nanosecond clock rather than a finer
 * label on a coarse one.
 *
 * The reading itself is now taken from the system clock (see
 * lkpi_monotonic_ns); what stays here is the raw counter, which udelay uses to
 * measure a busy-wait.
 */
static u64 lkpi_tsc(void)
{
	u32 lo, hi;

	__asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
}

/*
 * Busy-wait for a number of microseconds, measured rather than counted.
 *
 * This was a loop of usecs * 50 pause instructions — a guess at how many fit in
 * a microsecond, with no calibration behind it and no relation to the machine
 * it runs on. Drivers do not use udelay to be polite; they use it to satisfy
 * hardware timing that is specified in microseconds, and i915 spends it on i2c
 * rise/fall time, PLL settling and AUX bit periods. A count that is wrong by a
 * factor of anything is a delay that does not happen.
 *
 * The TSC is the same clock the rest of this file measures with, so the delay
 * is as accurate as the calibration is. Interrupts are left alone: callers rely
 * on udelay being usable with them off, and shootdowns are still serviced
 * through cpu_relax so a spinning CPU does not wedge one that is waiting on it.
 */
void lkpi_udelay(u64 usecs)
{
	u32 khz = arch_cpu_khz();
	u64 start, cycles;

	if (!khz) {
		/* Uncalibrated: fall back to the old spin so a delay is at least
		 * non-zero, and say nothing more precise than that. */
		for (u64 i = 0; i < usecs * 50; i++)
			lkpi_cpu_relax();
		return;
	}

	start = lkpi_tsc();
	cycles = (usecs * khz) / 1000;
	while (lkpi_tsc() - start < cycles)
		lkpi_cpu_relax();
}

u64 lkpi_monotonic_ns(void)
{
	u64 ns;

	/* Whatever CLOCK_MONOTONIC is, this must be it.
	 *
	 * ktime_get() is not only a duration source inside the driver: the DRM
	 * core stamps it into every page-flip and vblank event it delivers, and
	 * the client that reads one compares it against the CLOCK_MONOTONIC it
	 * gets from clock_gettime(2). Two clocks that count the same
	 * nanoseconds from DIFFERENT ORIGINS are not the same clock, and the
	 * difference is not a rounding error -- it is however far into the boot
	 * the driver first asked for the time.
	 *
	 * This counted from its own first call, so weston read every flip event
	 * as having happened about ten seconds before the frame it belonged to
	 * ("computed repaint delay is insane: -10736 msec"), and scheduled the
	 * next repaint immediately, every frame, for ever. Take the same base
	 * the system clock uses. */
	ns = arch_tsc_monotonic_ns();
	if (ns)
		return ns;

	/* The TSC is not yet declared fit to be a clock. Fall back to the tick,
	 * which is coarse and is measured from boot -- the same origin, so the
	 * changeover moves the resolution and not the epoch.
	 *
	 * A finer fallback that counted from its own first call was tried here and
	 * is wrong for the same reason the bug above was: this function is called
	 * before arch_tsc_clock_init has finished and again after, so the two
	 * epochs both appear within one boot, and any code holding a timestamp
	 * across the changeover sees time jump forward by the whole of the boot so
	 * far. The guest watchdog did, and killed the machine at eleven seconds
	 * for sixty seconds of silence. */
	return lkpi_ticks() * 10000000ull;
}
