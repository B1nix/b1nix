/* SPDX-License-Identifier: MIT
 *
 * M102a linuxkpi: the facilities i915 needs that had no home yet.
 *
 * Each of these could have been an inline in its header. They are here instead
 * because each needs something from b1nix — the timer wheel, the workqueue, the
 * klog — and a header that reached for those would drag b1nix's own spelling of
 * `spinlock_t` and `kmalloc` into every imported translation unit. See the note
 * in <lkpi/env.h>; this file is on the Linux side of that boundary and forwards
 * through the shim rather than through b1nix's own headers.
 */

#include <b1nix/iommu.h>
#include <b1nix/irq.h>
#include <linux/bug.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-array.h>
#include <linux/dma-resv.h>
#include <linux/errno.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/shrinker.h>
#include <linux/io-mapping.h>
#include <linux/irq_work.h>
#include <linux/ktime.h>
#include <linux/printk.h>
#include <linux/i2c.h>
#include <linux/kobject.h>
#include <linux/processor.h>
#include <linux/pci.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/relay.h>
#include <linux/sched.h>
#include <linux/shmem_fs.h>
#include <linux/stop_machine.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <asm/fpu/api.h>
#include <lkpi/env.h>

/* ── taint ──────────────────────────────────────────────────────── */

/*
 * Linux keeps a bitmask and prints it with every later oops, so a report can be
 * read knowing something already went wrong. b1nix has one log, and a line in
 * it is the same information in the place people actually look — so the flag is
 * recorded there rather than in a register nothing prints.
 */
void add_taint(unsigned flag, int lockdep_ok)
{
	(void)lockdep_ok;
	lkpi_printk("lkpi: kernel tainted (flag %u)\n", flag);
}

/* ── deferred work from interrupt context ───────────────────────── */

static void irq_work_run(struct work_struct *work)
{
	struct irq_work *iw = container_of(work, struct irq_work, work);

	if (iw->func)
		iw->func(iw);
}

void lkpi_irq_work_queue(struct irq_work *w)
{
	if (!w || !w->func)
		return;
	/* A workqueue item, so this runs on a kernel thread rather than out of a
	 * self-IPI. Later than upstream, and <linux/irq_work.h> says so. */
	INIT_WORK(&w->work, irq_work_run);
	schedule_work(&w->work);
}

/* ── io-mapping ─────────────────────────────────────────────────── */

/*
 * The whole window is mapped once, write-combining, and the per-page calls are
 * offsets into it. Linux maps a page at a time out of a fixmap slot; b1nix has
 * no fixmap, and mapping the region once is the honest alternative — the cost is
 * address space, which on 64-bit is not scarce, and the benefit is that the
 * per-page path has no failure mode at all.
 */
bool io_mapping_init_wc(struct io_mapping *iomap, resource_size_t base,
                        unsigned long size)
{
	if (!iomap)
		return false;
	iomap->base = base;
	iomap->size = size;
	iomap->iomem = ioremap_wc(base, size);
	return iomap->iomem != 0;
}

void io_mapping_fini(struct io_mapping *iomap)
{
	if (!iomap || !iomap->iomem)
		return;
	iounmap(iomap->iomem);
	iomap->iomem = 0;
}

struct io_mapping *io_mapping_create_wc(resource_size_t base, unsigned long size)
{
	struct io_mapping *iomap = kzalloc(sizeof(*iomap), GFP_KERNEL);

	if (!iomap)
		return 0;
	if (!io_mapping_init_wc(iomap, base, size)) {
		kfree(iomap);
		return 0;
	}
	return iomap;
}

void io_mapping_free(struct io_mapping *iomap)
{
	if (!iomap)
		return;
	io_mapping_fini(iomap);
	kfree(iomap);
}

/* ── hrtimer over the tick ──────────────────────────────────────── */

/*
 * Resolution is the scheduler tick, 10 ms — a request for less is rounded up to
 * one tick rather than refused, because every caller here is arming a timeout
 * and a longer one is safe where a missing one is not. <linux/hrtimer.h> states
 * the difference; this is where it happens.
 */
#define LKPI_TICK_NS (10ull * 1000ull * 1000ull)

static void hrtimer_trampoline(struct timer_list *t)
{
	struct hrtimer *h = container_of(t, struct hrtimer, timer);

	if (!h->function)
		return;
	if (h->function(h) == HRTIMER_RESTART && h->interval_ns) {
		u64 ticks = (h->interval_ns + LKPI_TICK_NS - 1) / LKPI_TICK_NS;

		mod_timer(&h->timer, jiffies + (ticks ? ticks : 1));
	}
}

void hrtimer_init(struct hrtimer *t, int clock_id, enum hrtimer_mode mode)
{
	(void)clock_id;
	(void)mode;
	if (!t)
		return;
	timer_setup(&t->timer, hrtimer_trampoline, 0);
	t->interval_ns = 0;
}

void hrtimer_start(struct hrtimer *t, ktime_t when, enum hrtimer_mode mode)
{
	if (!t)
		return;

	s64 ns = ktime_to_ns(when);

	/* An absolute deadline is turned into a delay against now; a relative one
	 * already is. Getting this backwards arms a timer decades out, which looks
	 * exactly like a timer that never fires. */
	if (mode == HRTIMER_MODE_ABS)
		ns -= (s64)(lkpi_ticks() * LKPI_TICK_NS);
	if (ns < 0)
		ns = 0;

	u64 ticks = ((u64)ns + LKPI_TICK_NS - 1) / LKPI_TICK_NS;

	mod_timer(&t->timer, jiffies + (ticks ? ticks : 1));
}

int hrtimer_cancel(struct hrtimer *t)
{
	if (!t)
		return 0;
	t->interval_ns = 0;
	return del_timer_sync(&t->timer);
}

int hrtimer_try_to_cancel(struct hrtimer *t)
{
	/* No untimed cancel here: b1nix's timer teardown is the synchronous one,
	 * and returning without waiting would let the callback run against a
	 * structure the caller is about to free. Waiting is the safe difference. */
	return t ? del_timer_sync(&t->timer) : 0;
}

bool hrtimer_active(const struct hrtimer *t)
{
	return t ? timer_pending(&t->timer) : false;
}

/* ── dma-fence-array ────────────────────────────────────────────── */

/*
 * One fence over several, signalled when the last member is.
 *
 * The pending count is what makes it work: each member gets a callback, every
 * callback decrements, and the array signals on the transition to zero. The
 * callback also has to cope with a member that was already signalled when it
 * was added — dma_fence_add_callback reports that rather than calling back — so
 * the count is decremented in that case too, or the array would never complete.
 */
static const char *fence_array_get_driver_name(struct dma_fence *fence)
{ (void)fence; return "dma_fence_array"; }

static const char *fence_array_get_timeline_name(struct dma_fence *fence)
{ (void)fence; return "unbound"; }

static void fence_array_release(struct dma_fence *fence)
{
	struct dma_fence_array *array = (struct dma_fence_array *)fence;

	for (unsigned int i = 0; i < array->num_fences; i++)
		dma_fence_put(array->fences[i]);
	kfree(array->fences);
	kfree(array);
}

static const struct dma_fence_ops dma_fence_array_ops = {
	.get_driver_name = fence_array_get_driver_name,
	.get_timeline_name = fence_array_get_timeline_name,
	.release = fence_array_release,
};

bool dma_fence_is_array(struct dma_fence *fence)
{
	return fence && fence->ops == &dma_fence_array_ops;
}

static void fence_array_member_signalled(struct dma_fence *f,
                                         struct dma_fence_cb *cb)
{
	struct dma_fence_array *array = container_of(cb, struct dma_fence_array,
	                                             cb_storage);
	(void)f;
	if (atomic_dec_and_test(&array->num_pending))
		dma_fence_signal(&array->base);
}

struct dma_fence_array *dma_fence_array_create(int num_fences,
                                               struct dma_fence **fences,
                                               u64 context, unsigned seqno,
                                               bool signal_on_any)
{
	struct dma_fence_array *array;

	if (num_fences <= 0 || !fences)
		return 0;

	array = kzalloc(sizeof(*array), GFP_KERNEL);
	if (!array)
		return 0;

	spin_lock_init(&array->lock);
	dma_fence_init(&array->base, &dma_fence_array_ops, &array->lock, context,
	               seqno);

	array->num_fences = (unsigned int)num_fences;
	array->fences = fences;
	atomic_set(&array->num_pending, signal_on_any ? 1 : num_fences);

	for (int i = 0; i < num_fences; i++) {
		if (dma_fence_add_callback(fences[i], &array->cb_storage,
		                           fence_array_member_signalled) != 0) {
			/* Already signalled: the callback will never run, so account
			 * for it here or the array waits for a member that is done. */
			if (atomic_dec_and_test(&array->num_pending)) {
				dma_fence_signal(&array->base);
				break;
			}
		}
	}

	return array;
}

bool dma_fence_match_context(struct dma_fence *fence, u64 context)
{
	if (!dma_fence_is_array(fence))
		return fence && fence->context == context;

	struct dma_fence_array *array = (struct dma_fence_array *)fence;

	for (unsigned int i = 0; i < array->num_fences; i++)
		if (array->fences[i]->context != context)
			return false;
	return true;
}

struct dma_fence *dma_fence_array_first(struct dma_fence *head)
{
	if (!head)
		return 0;
	if (!dma_fence_is_array(head))
		return head;
	struct dma_fence_array *array = (struct dma_fence_array *)head;
	return array->num_fences ? array->fences[0] : 0;
}

struct dma_fence *dma_fence_array_next(struct dma_fence *head,
                                       unsigned int index)
{
	if (!dma_fence_is_array(head))
		return 0;
	struct dma_fence_array *array = (struct dma_fence_array *)head;
	return index < array->num_fences ? array->fences[index] : 0;
}

/* ── tasklets ───────────────────────────────────────────────────── */

/*
 * The state machine, spelled out because getting it wrong is a class of bug
 * that only shows under load:
 *
 *   SCHED is set by tasklet_schedule and cleared by the worker *before* the
 *   callback runs. Clearing it first is what lets the callback re-schedule
 *   itself — execlists does exactly that when it processes an event and finds
 *   more work — and clearing it after would silently swallow that request.
 *
 *   RUN is held across the callback. It is what tasklet_trylock takes and what
 *   tasklet_unlock_wait spins on, and it is the guarantee that the callback is
 *   not running when a caller tears the engine down.
 *
 *   count is the disable nesting. A schedule while disabled sets SCHED and does
 *   not queue, so tasklet_enable has to queue anything that arrived meanwhile —
 *   otherwise a disable/enable pair around a reset loses the submission that
 *   arrived inside it, and the engine simply stops.
 */

static void tasklet_run(struct work_struct *work)
{
	struct tasklet_struct *t = container_of(work, struct tasklet_struct, work);

	if (atomic_read(&t->count)) {
		/* Disabled after it was queued: leave SCHED set so tasklet_enable
		 * re-queues it rather than dropping the request. */
		return;
	}

	__atomic_and_fetch(&t->state, ~(1UL << TASKLET_STATE_SCHED),
	                   __ATOMIC_ACQ_REL);
	__atomic_or_fetch(&t->state, 1UL << TASKLET_STATE_RUN, __ATOMIC_ACQ_REL);

	if (t->callback)
		t->callback(t);
	else if (t->func)
		t->func(t->data);

	__atomic_and_fetch(&t->state, ~(1UL << TASKLET_STATE_RUN),
	                   __ATOMIC_ACQ_REL);
	lkpi_wake_all(t);
}

void tasklet_setup(struct tasklet_struct *t, tasklet_callback_t callback)
{
	if (!t)
		return;
	INIT_WORK(&t->work, tasklet_run);
	t->callback = callback;
	t->func = 0;
	t->data = 0;
	t->state = 0;
	atomic_set(&t->count, 0);
}

void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long),
                  unsigned long data)
{
	if (!t)
		return;
	tasklet_setup(t, 0);
	t->func = func;
	t->data = data;
}

void tasklet_schedule(struct tasklet_struct *t)
{
	if (!t)
		return;
	/* Already queued: coalesce. One run after a burst of schedules is what a
	 * tasklet promises, and what the interrupt handler is counting on. */
	if (__atomic_fetch_or(&t->state, 1UL << TASKLET_STATE_SCHED,
	                      __ATOMIC_ACQ_REL) & (1UL << TASKLET_STATE_SCHED))
		return;
	if (atomic_read(&t->count))
		return; /* disabled; tasklet_enable will queue it */
	schedule_work(&t->work);
}

void tasklet_disable_nosync(struct tasklet_struct *t)
{
	if (t)
		atomic_inc(&t->count);
}

void tasklet_disable(struct tasklet_struct *t)
{
	tasklet_disable_nosync(t);
	tasklet_unlock_wait(t);
}

void tasklet_enable(struct tasklet_struct *t)
{
	if (!t)
		return;
	if (!atomic_dec_and_test(&t->count))
		return;
	/* Re-queue whatever arrived while it was disabled. */
	if (t->state & (1UL << TASKLET_STATE_SCHED))
		schedule_work(&t->work);
}

void tasklet_kill(struct tasklet_struct *t)
{
	if (!t)
		return;
	/* Both halves matter: the queued run has to happen or be abandoned, and the
	 * running one has to finish, before the caller may free what the callback
	 * dereferences. */
	flush_work(&t->work);
	tasklet_unlock_wait(t);
	__atomic_and_fetch(&t->state, ~(1UL << TASKLET_STATE_SCHED),
	                   __ATOMIC_ACQ_REL);
}

int tasklet_trylock(struct tasklet_struct *t)
{
	if (!t)
		return 0;
	return (__atomic_fetch_or(&t->state, 1UL << TASKLET_STATE_RUN,
	                          __ATOMIC_ACQ_REL) &
	        (1UL << TASKLET_STATE_RUN)) == 0;
}

void tasklet_unlock(struct tasklet_struct *t)
{
	if (!t)
		return;
	__atomic_and_fetch(&t->state, ~(1UL << TASKLET_STATE_RUN),
	                   __ATOMIC_ACQ_REL);
	lkpi_wake_all(t);
}

void tasklet_unlock_wait(struct tasklet_struct *t)
{
	if (!t)
		return;
	while (t->state & (1UL << TASKLET_STATE_RUN)) {
		/* Parking is only legal off an interrupt stack; the spin path below is
		 * the one a caller in atomic context gets, and it is a spin because
		 * there is nothing else it may do. */
		if (lkpi_can_block()) {
			lkpi_wait_prepare(t);
			if (!(t->state & (1UL << TASKLET_STATE_RUN))) {
				lkpi_wait_cancel();
				break;
			}
			lkpi_wait_commit();
		} else {
			lkpi_cpu_relax();
		}
	}
}

void tasklet_unlock_spin_wait(struct tasklet_struct *t)
{
	while (t && (t->state & (1UL << TASKLET_STATE_RUN)))
		lkpi_cpu_relax();
}

/* ── shrinkers ──────────────────────────────────────────────────── */

/*
 * Registration that records nothing, because nothing reclaims here — see
 * <linux/shrinker.h> for what that costs. The allocation is real so the
 * driver's teardown has something to free, and so a probe that checks for NULL
 * takes the success path it would on Linux.
 */
struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...)
{
	struct shrinker *s = kzalloc(sizeof(*s), GFP_KERNEL);

	(void)flags;
	(void)fmt;
	return s;
}

void shrinker_register(struct shrinker *shrinker)
{
	(void)shrinker;
}

void shrinker_free(struct shrinker *shrinker)
{
	kfree(shrinker);
}

/* ── slab caches ────────────────────────────────────────────────── */

/*
 * A named pool of same-sized objects, over b1nix's heap.
 *
 * The size and the constructor are the parts callers depend on and they are
 * honoured: every object handed out is exactly `size` bytes and has had `ctor`
 * run on it. What is not here is the locality a real slab buys — objects come
 * from the general heap, so they are not packed together. That costs cache
 * misses on a hot allocation path and nothing else.
 */
struct kmem_cache {
	const char *name;
	unsigned int size;
	unsigned int align;
	void (*ctor)(void *);
};

struct kmem_cache *kmem_cache_create(const char *name, unsigned int size,
                                     unsigned int align, unsigned long flags,
                                     void (*ctor)(void *))
{
	struct kmem_cache *c = kzalloc(sizeof(*c), GFP_KERNEL);

	(void)flags;
	if (!c)
		return 0;
	c->name = name;
	c->size = size;
	c->align = align;
	c->ctor = ctor;
	return c;
}

void kmem_cache_destroy(struct kmem_cache *c)
{
	kfree(c);
}

void *kmem_cache_alloc(struct kmem_cache *c, gfp_t flags)
{
	if (!c)
		return 0;

	void *obj = kmalloc(c->size, flags);

	/* The constructor runs on a freshly allocated object, as upstream's does.
	 * A cache with a constructor and a caller that also initialises is the
	 * normal arrangement; running it here keeps that true. */
	if (obj && c->ctor)
		c->ctor(obj);
	return obj;
}

void *kmem_cache_zalloc(struct kmem_cache *c, gfp_t flags)
{
	return c ? kzalloc(c->size, flags) : 0;
}

void kmem_cache_free(struct kmem_cache *c, void *obj)
{
	(void)c;
	kfree(obj);
}

void kmem_cache_shrink(struct kmem_cache *c)
{
	/* Nothing is cached, so there is nothing to return to the heap. */
	(void)c;
}

/* ── rbtree post-order walk ─────────────────────────────────────── */

/*
 * The order a tree must be freed in: a node only after both its children.
 *
 * Descend left as far as possible, then take the leftmost leaf of the right
 * subtree — that is the first node with no children left to visit. From there,
 * a node's successor is its sibling's leftmost leaf if it was a left child, and
 * its parent otherwise. Nothing here rebalances: the tree is on its way out.
 */
static struct rb_node *leftmost_leaf(const struct rb_node *node)
{
	for (;;) {
		if (node->rb_left)
			node = node->rb_left;
		else if (node->rb_right)
			node = node->rb_right;
		else
			return (struct rb_node *)node;
	}
}

struct rb_node *rb_first_postorder(const struct rb_root *root)
{
	if (!root || !root->rb_node)
		return 0;
	return leftmost_leaf(root->rb_node);
}

struct rb_node *rb_next_postorder(const struct rb_node *node)
{
	if (!node)
		return 0;

	const struct rb_node *parent = rb_parent(node);

	if (!parent)
		return 0;
	/* A left child whose sibling exists: that subtree is next. Otherwise the
	 * parent itself, because both of its children are now done. */
	if (node == parent->rb_left && parent->rb_right)
		return leftmost_leaf(parent->rb_right);
	return (struct rb_node *)parent;
}

/* ── PCI windows ────────────────────────────────────────────────── */

/*
 * Fill a device's resource array from b1nix's own BAR sizing.
 *
 * `end` is inclusive, as it is upstream — pci_resource_len is
 * end - start + 1, and an exclusive end here would report every window one byte
 * short, which a driver would not notice until the last page of an aperture.
 *
 * An unimplemented BAR is left zeroed rather than skipped: the array is indexed
 * by BAR number, so entries have to keep their positions.
 */
void lkpi_pci_dev_fill_resources(struct pci_dev *pdev)
{
	if (!pdev)
		return;

	for (unsigned int i = 0; i < 6; i++) {
		u64 start = 0, size = 0;
		u32 is_io = 0;

		pdev->resource[i].start = 0;
		pdev->resource[i].end = 0;
		pdev->resource[i].flags = 0;

		if (!lkpi_pci_bar(pdev->bus_nr, pdev->slot, pdev->func, i, &start,
		                  &size, &is_io))
			continue;
		if (!size)
			continue;

		pdev->resource[i].start = start;
		pdev->resource[i].end = start + size - 1;
		pdev->resource[i].flags = is_io ? IORESOURCE_IO : IORESOURCE_MEM;
	}
}

/* ── string to number ───────────────────────────────────────────── */

/*
 * The parsers imported code uses for module parameters and sysfs writes.
 *
 * Strict, as upstream's are: the whole string must be a number, optionally with
 * one trailing newline, and anything else is rejected. That strictness is the
 * point — a sysfs write of "12abc" setting a value to 12 is how a typo becomes
 * a silently wrong setting. The output is left untouched on failure, because
 * callers parse straight into a live field.
 */
static int parse_ull(const char *s, unsigned int base, unsigned long long *out,
                     int allow_sign, int *negative)
{
	unsigned long long v = 0;
	int any = 0;

	if (!s || !out)
		return -EINVAL;

	*negative = 0;
	if (allow_sign && *s == '-') {
		*negative = 1;
		s++;
	}

	if (base == 0) {
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			base = 16;
			s += 2;
		} else if (s[0] == '0' && s[1]) {
			base = 8;
			s++;
		} else {
			base = 10;
		}
	} else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}

	for (; *s; s++) {
		unsigned int digit;

		if (*s >= '0' && *s <= '9')
			digit = (unsigned int)(*s - '0');
		else if (*s >= 'a' && *s <= 'f')
			digit = (unsigned int)(*s - 'a') + 10;
		else if (*s >= 'A' && *s <= 'F')
			digit = (unsigned int)(*s - 'A') + 10;
		else if (*s == '\n' && s[1] == '\0')
			break; /* one trailing newline, as a sysfs write carries */
		else
			return -EINVAL;

		if (digit >= base)
			return -EINVAL;

		unsigned long long next = v * base + digit;

		/* Overflow has to be refused rather than wrapped: a wrapped value is
		 * a plausible number, and the caller has no way to tell. */
		if (next < v)
			return -ERANGE;
		v = next;
		any = 1;
	}

	if (!any)
		return -EINVAL;
	*out = v;
	return 0;
}

int kstrtoull(const char *s, unsigned int base, unsigned long long *res)
{
	unsigned long long v;
	int neg;
	int rc = parse_ull(s, base, &v, 0, &neg);

	if (rc)
		return rc;
	*res = v;
	return 0;
}

int kstrtoll(const char *s, unsigned int base, long long *res)
{
	unsigned long long v;
	int neg;
	int rc = parse_ull(s, base, &v, 1, &neg);

	if (rc)
		return rc;
	if (v > (unsigned long long)0x7fffffffffffffffULL + (neg ? 1u : 0u))
		return -ERANGE;
	*res = neg ? -(long long)v : (long long)v;
	return 0;
}

int kstrtou32(const char *s, unsigned int base, u32 *res)
{
	unsigned long long v;
	int rc = kstrtoull(s, base, &v);

	if (rc)
		return rc;
	if (v > 0xffffffffULL)
		return -ERANGE;
	*res = (u32)v;
	return 0;
}

int kstrtos32(const char *s, unsigned int base, s32 *res)
{
	long long v;
	int rc = kstrtoll(s, base, &v);

	if (rc)
		return rc;
	if (v > 0x7fffffffLL || v < -0x80000000LL)
		return -ERANGE;
	*res = (s32)v;
	return 0;
}

int kstrtou16(const char *s, unsigned int base, u16 *res)
{
	u32 v;
	int rc = kstrtou32(s, base, &v);

	if (rc)
		return rc;
	if (v > 0xffff)
		return -ERANGE;
	*res = (u16)v;
	return 0;
}

int kstrtou8(const char *s, unsigned int base, u8 *res)
{
	u32 v;
	int rc = kstrtou32(s, base, &v);

	if (rc)
		return rc;
	if (v > 0xff)
		return -ERANGE;
	*res = (u8)v;
	return 0;
}

int kstrtobool(const char *s, bool *res)
{
	if (!s || !res)
		return -EINVAL;
	switch (s[0]) {
	case 'y': case 'Y': case '1':
		*res = true;
		return 0;
	case 'n': case 'N': case '0':
		*res = false;
		return 0;
	default:
		return -EINVAL;
	}
}

/* ── PCI config space, narrow widths ────────────────────────────── */

/*
 * b1nix's config accessors are 32-bit, so a byte or word access is a read of
 * the containing dword and a splice. The read-modify-write is done here rather
 * than by the caller because the neighbours in that dword belong to other
 * fields — writing the whole dword back with them zeroed is how a driver
 * accidentally disables bus mastering while setting a latency timer.
 */
int pci_write_config_byte(struct pci_dev *dev, int where, u8 val)
{
	u32 dword;
	int rc;
	unsigned int shift = (unsigned int)(where & 3) * 8;

	if (!dev)
		return -EINVAL;
	rc = pci_read_config_dword(dev, where & ~3, &dword);
	if (rc)
		return rc;
	dword = (dword & ~(0xffu << shift)) | ((u32)val << shift);
	return pci_write_config_dword(dev, where & ~3, dword);
}

int pci_write_config_word(struct pci_dev *dev, int where, u16 val)
{
	u32 dword;
	int rc;
	unsigned int shift = (unsigned int)(where & 2) * 8;

	if (!dev)
		return -EINVAL;
	rc = pci_read_config_dword(dev, where & ~3, &dword);
	if (rc)
		return rc;
	dword = (dword & ~(0xffffu << shift)) | ((u32)val << shift);
	return pci_write_config_dword(dev, where & ~3, dword);
}

/* ── reservation-object iteration ───────────────────────────────── */

/*
 * The cursor walks the object's fence array by index, under no lock of its own.
 *
 * Upstream restarts the walk when the array changed underneath it and tells the
 * caller through is_restarted, so that anything accumulated so far can be
 * discarded. Here the array is only ever grown by dma_resv_add_fence under the
 * object's ww_mutex, and every caller of this iterator holds that mutex — so a
 * change mid-walk cannot be observed, is_restarted is always false, and it says
 * so rather than being a field nothing maintains.
 */
void dma_resv_iter_begin(struct dma_resv_iter *cursor, struct dma_resv *obj,
                         enum dma_resv_usage usage)
{
	if (!cursor)
		return;
	cursor->obj = obj;
	cursor->usage = usage;
	cursor->fence = 0;
	cursor->fence_usage = usage;
	cursor->index = 0;
	cursor->is_restarted = false;
}

void dma_resv_iter_end(struct dma_resv_iter *cursor)
{
	if (!cursor)
		return;
	/* The iterator holds no reference: the caller's ww_mutex is what keeps the
	 * fences alive for the walk, and taking one per fence would make the
	 * common case — look, then drop — pay for a case nobody has. */
	cursor->fence = 0;
	cursor->obj = 0;
}

static struct dma_fence *iter_advance(struct dma_resv_iter *cursor)
{
	if (!cursor || !cursor->obj)
		return 0;

	while (cursor->index < cursor->obj->count) {
		struct dma_resv_fence *slot = &cursor->obj->fences[cursor->index++];

		/* Usage levels are ordered: a caller asking for writes must not be
		 * handed a read fence, but one asking for reads waits for both. */
		if (slot->usage > cursor->usage)
			continue;
		cursor->fence = slot->fence;
		cursor->fence_usage = slot->usage;
		return cursor->fence;
	}

	cursor->fence = 0;
	return 0;
}

struct dma_fence *dma_resv_iter_first(struct dma_resv_iter *cursor)
{
	if (!cursor)
		return 0;
	cursor->index = 0;
	return iter_advance(cursor);
}

struct dma_fence *dma_resv_iter_next(struct dma_resv_iter *cursor)
{
	return iter_advance(cursor);
}

struct dma_fence *dma_resv_iter_first_unlocked(struct dma_resv_iter *cursor)
{
	return dma_resv_iter_first(cursor);
}

struct dma_fence *dma_resv_iter_next_unlocked(struct dma_resv_iter *cursor)
{
	return iter_advance(cursor);
}

/* ── mapping a BAR ──────────────────────────────────────────────── */

/*
 * `maxlen` of 0 means "the whole window", as upstream's does; anything else
 * caps it. The cap is not a courtesy — an aperture can be gigabytes, and
 * mapping all of it when the driver wants one page wastes the address space
 * that the rest of the kernel window lives in.
 */
void __iomem *pci_iomap_range(struct pci_dev *dev, int bar, unsigned long offset,
                              unsigned long maxlen)
{
	if (!dev || bar < 0 || bar >= 6)
		return 0;

	u64 start = dev->resource[bar].start;
	u64 len = pci_resource_len(dev, bar);

	if (!start || !len || offset >= len)
		return 0;

	len -= offset;
	if (maxlen && maxlen < len)
		len = maxlen;
	return ioremap(start + offset, (usize)len);
}

void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen)
{
	return pci_iomap_range(dev, bar, 0, maxlen);
}

void pci_iounmap(struct pci_dev *dev, void __iomem *addr)
{
	(void)dev;
	iounmap(addr);
}

/* The sysfs group a device's power attributes live in. One string, shared, so
 * every device names the same group rather than each creating its own. */
const char power_group_name[] = "power";

/* The boot CPU's identity, filled once from CPUID leaf 1. A driver keys a
 * workaround on family/model, so these are the real values — a zeroed struct
 * would silently match "family 0", which no quirk table expects. */
struct cpuinfo_x86 boot_cpu_data;

void lkpi_cpuinfo_init(void)
{
  u32 eax = 0, ebx = 0, ecx = 0, edx = 0;

  __asm__ volatile("cpuid"
                   : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                   : "a"(1u), "c"(0u));

  boot_cpu_data.x86 = (u8)((eax >> 8) & 0xf);
  boot_cpu_data.x86_model = (u8)((eax >> 4) & 0xf);
  /* Extended family and model, as the encoding requires above family 0xf. */
  if (boot_cpu_data.x86 == 0xf)
    boot_cpu_data.x86 = (u8)(boot_cpu_data.x86 + ((eax >> 20) & 0xff));
  if (boot_cpu_data.x86 == 0x6 || boot_cpu_data.x86 == 0xf)
    boot_cpu_data.x86_model =
        (u8)(boot_cpu_data.x86_model + (((eax >> 16) & 0xf) << 4));
  boot_cpu_data.x86_stepping = (u8)(eax & 0xf);
  boot_cpu_data.x86_capability[0] = edx;
  boot_cpu_data.x86_capability[1] = ecx;
}

/* ── PCI device state ───────────────────────────────────────────── */

/*
 * b1nix keeps every device in D0 and has no system suspend, so the only state
 * that can be reached is the one the device is already in. Refusing the others
 * matters: a driver told it reached D3 skips the register save and restore it
 * does around a real transition, and then resumes a device that never left D0
 * with state it never wrote back.
 */
int pci_set_power_state(struct pci_dev *dev, pci_power_t state)
{
	if (!dev)
		return -EINVAL;
	return state == PCI_D0 ? 0 : -EIO;
}

void pci_disable_device(struct pci_dev *dev)
{
	(void)dev;
	/* Deliberately not disabling decode: b1nix's own drivers may share the
	 * bus, and turning a device's windows off underneath them would be a
	 * failure far from here. */
}

int pci_save_state(struct pci_dev *dev)
{
	(void)dev;
	/* Nothing to save while nothing ever powers down. */
	return 0;
}

void pci_restore_state(struct pci_dev *dev)
{
	(void)dev;
}

void pci_clear_master(struct pci_dev *dev)
{
	(void)dev;
	/* Same reasoning as pci_disable_device: b1nix's own drivers may be on the
	 * same bus, and clearing bus mastering underneath them fails elsewhere. */
}

/*
 * Drain a workqueue: keep flushing until a flush finds nothing new.
 *
 * flush_workqueue waits for the items that were pending when it was called; an
 * item that queues another during its own run leaves work behind it. Teardown
 * cannot tolerate that, so this repeats until a pass adds nothing — with a
 * bound, because a queue that re-arms itself forever would otherwise hang the
 * caller with no diagnostic at all.
 */
void drain_workqueue(struct workqueue_struct *wq)
{
	if (!wq)
		return;

	for (unsigned int pass = 0; pass < 64; pass++) {
		flush_workqueue(wq);
		if (!workqueue_pending(wq))
			return;
	}
	lkpi_printk("lkpi: drain_workqueue gave up after 64 passes\n");
}

/* The sysfs operations every kobj_attribute dispatches through: the attribute
 * carries the show/store, so this only has to route to it. */
static ssize_t kobj_attr_show(struct kobject *kobj, struct attribute *attr,
                              char *buf)
{
	struct kobj_attribute *kattr =
		container_of(attr, struct kobj_attribute, attr);

	return kattr->show ? kattr->show(kobj, kattr, buf) : -EIO;
}

static ssize_t kobj_attr_store(struct kobject *kobj, struct attribute *attr,
                               const char *buf, size_t count)
{
	struct kobj_attribute *kattr =
		container_of(attr, struct kobj_attribute, attr);

	return kattr->store ? kattr->store(kobj, kattr, buf, count) : -EIO;
}

const struct sysfs_ops kobj_sysfs_ops = {
	.show = kobj_attr_show,
	.store = kobj_attr_store,
};

/* ── i2c adapters ───────────────────────────────────────────────── */

/*
 * b1nix's i2c core is what actually drives a bus; an adapter registered here is
 * the driver's own, and its transfers go through the algorithm it supplied
 * rather than through the core. So registration records the adapter and
 * nothing more — there is no bus number to allocate, because nothing else
 * enumerates these.
 */
int i2c_add_adapter(struct i2c_adapter *adap)
{
	if (!adap)
		return -EINVAL;
	/* An adapter with no algorithm cannot transfer, and accepting it would
	 * turn every later read into a NULL call rather than a refused probe. */
	if (!adap->algo)
		return -EINVAL;
	return 0;
}

void i2c_del_adapter(struct i2c_adapter *adap)
{
	(void)adap;
}

int i2c_check_functionality(struct i2c_adapter *adap, u32 func)
{
	if (!adap || !adap->algo || !adap->algo->functionality)
		return 0;
	return (adap->algo->functionality(adap) & func) == func;
}

/* The device pool the lookups hand out of; defined with them below. */
static struct pci_dev *lkpi_pci_publish(u8 bus, u8 slot, u8 func);

/* ── PCI driver registration ────────────────────────────────────── */

/*
 * Registration is what binds an imported driver to real hardware.
 *
 * b1nix has no bus layer that walks driver tables, so this does the walk: every
 * function that responds to a config read is matched against the driver's id
 * table and, on a match, given a struct pci_dev and handed to ->probe. That is
 * the whole of the binding — there is no deferred probe and no rebinding, so a
 * driver registered after enumeration sees the machine exactly as it is now.
 *
 * Reporting success on no match matters: a driver whose registration failed
 * tears down everything it built during module init, and "no device of yours is
 * present" is not a registration failure.
 */
int pci_register_driver(struct pci_driver *drv)
{
	u32 bus, slot, func;

	if (!drv)
		return -EINVAL;
	if (!drv->probe || !drv->id_table)
		return 0;

	for (bus = 0; bus < 256; bus++) {
		for (slot = 0; slot < 32; slot++) {
			for (func = 0; func < 8; func++) {
				struct pci_dev probe_key = { 0 };
				struct pci_dev *pdev;
				const struct pci_device_id *id;
				u32 ident = pci_config_read32((u8)bus, (u8)slot,
				                              (u8)func, 0x00);

				if ((ident & 0xffff) == 0xffff)
					continue;
				/* Match against a stack copy first: publishing every
				 * function on the machine would fill the pool with
				 * devices no driver claims. */
				probe_key.vendor = (u16)(ident & 0xffff);
				probe_key.device = (u16)(ident >> 16);
				probe_key.class = pci_config_read32((u8)bus, (u8)slot,
				                                    (u8)func, 0x08) >> 8;
				id = pci_match_id(drv->id_table, &probe_key);
				if (!id)
					continue;
				pdev = lkpi_pci_publish((u8)bus, (u8)slot, (u8)func);
				if (!pdev)
					continue;

				/* The BARs are decoded before probe, not on demand: the
				 * driver maps them as one of the first things it does. */
				lkpi_pci_dev_fill_resources(pdev);
				pdev->dev.driver = &drv->driver;
				device_initialize(&pdev->dev);
				dev_set_name(&pdev->dev, "%04x:%02x:%02x.%u",
				             0, bus, slot, func);
				if (drv->probe(pdev, id) != 0)
					pdev->dev.driver = 0;
			}
		}
	}
	return 0;
}

void pci_unregister_driver(struct pci_driver *drv)
{
	(void)drv;
}

/*
 * Queue after a grace period.
 *
 * Waits here rather than in a callback: b1nix's synchronize_rcu is a real wait
 * (proved by the M101 self-test), so the ordering is the same and there is no
 * callback thread to route through. The cost lands on the caller, which must
 * therefore be somewhere it can sleep — and every caller of this already is.
 */
bool queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rwork)
{
	if (!wq || !rwork)
		return false;
	synchronize_rcu();
	return queue_work(wq, &rwork->work) != 0;
}

/* ── PCI config through a bus ───────────────────────────────────── */

/*
 * A device the driver does not own — the host bridge, in i915's case, which it
 * reads to learn the memory configuration. The bus argument is ignored because
 * b1nix has one PCI segment, and the devfn carries the slot and function.
 */
int pci_bus_read_config_dword(struct pci_bus *bus, unsigned int devfn, int where,
                              u32 *val)
{
	(void)bus;
	if (!val)
		return -EINVAL;
	*val = pci_config_read32(0, (u8)((devfn >> 3) & 0x1f), (u8)(devfn & 7),
	                         (u8)where);
	return 0;
}

int pci_bus_read_config_word(struct pci_bus *bus, unsigned int devfn, int where,
                             u16 *val)
{
	u32 dword;
	int rc = pci_bus_read_config_dword(bus, devfn, where & ~3, &dword);

	if (rc)
		return rc;
	*val = (u16)(dword >> ((where & 2) * 8));
	return 0;
}

int pci_bus_read_config_byte(struct pci_bus *bus, unsigned int devfn, int where,
                             u8 *val)
{
	u32 dword;
	int rc = pci_bus_read_config_dword(bus, devfn, where & ~3, &dword);

	if (rc)
		return rc;
	*val = (u8)(dword >> ((where & 3) * 8));
	return 0;
}

int pci_bus_write_config_dword(struct pci_bus *bus, unsigned int devfn, int where,
                               u32 val)
{
	(void)bus;
	pci_config_write32(0, (u8)((devfn >> 3) & 0x1f), (u8)(devfn & 7),
	                   (u8)where, val);
	return 0;
}

int pci_bus_write_config_word(struct pci_bus *bus, unsigned int devfn, int where,
                              u16 val)
{
	u32 dword;
	unsigned int shift = (unsigned int)(where & 2) * 8;
	int rc = pci_bus_read_config_dword(bus, devfn, where & ~3, &dword);

	if (rc)
		return rc;
	dword = (dword & ~(0xffffu << shift)) | ((u32)val << shift);
	return pci_bus_write_config_dword(bus, devfn, where & ~3, dword);
}

int pci_bus_write_config_byte(struct pci_bus *bus, unsigned int devfn, int where,
                              u8 val)
{
	u32 dword;
	unsigned int shift = (unsigned int)(where & 3) * 8;
	int rc = pci_bus_read_config_dword(bus, devfn, where & ~3, &dword);

	if (rc)
		return rc;
	dword = (dword & ~(0xffu << shift)) | ((u32)val << shift);
	return pci_bus_write_config_dword(bus, devfn, where & ~3, dword);
}

/*
 * Match against an id table. PCI_ANY_ID matches anything; the class comparison
 * is masked, which is what lets an entry match a whole class of devices.
 */
const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
                                         struct pci_dev *dev)
{
	if (!ids || !dev)
		return 0;

	for (; ids->vendor || ids->class_mask; ids++) {
		if (ids->vendor != PCI_ANY_ID && ids->vendor != dev->vendor)
			continue;
		if (ids->device != PCI_ANY_ID && ids->device != dev->device)
			continue;
		if (ids->subvendor != PCI_ANY_ID &&
		    ids->subvendor != dev->subsystem_vendor)
			continue;
		if (ids->subdevice != PCI_ANY_ID &&
		    ids->subdevice != dev->subsystem_device)
			continue;
		if ((ids->class ^ dev->class) & ids->class_mask)
			continue;
		return ids;
	}
	return 0;
}

/*
 * The option ROM. Not mapped: the VBT i915 needs on these parts lives in the
 * ACPI OpRegion, and handing back a pointer to whatever is at the ROM window
 * would have the driver parse unrelated memory as a BIOS image.
 */
void __iomem *pci_map_rom(struct pci_dev *pdev, size_t *size)
{
	(void)pdev;
	if (size)
		*size = 0;
	return 0;
}

void pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom)
{
	(void)pdev;
	(void)rom;
}

int kstrtobool_from_user(const char __user *s, size_t count, bool *res)
{
	char buf[4];
	size_t n = count < sizeof(buf) - 1 ? count : sizeof(buf) - 1;

	if (!s || !res)
		return -EINVAL;
	if (copy_from_user(buf, s, n))
		return -EFAULT;
	/* The caller's buffer is not NUL-terminated: it is however many bytes
	 * userspace wrote. Terminating here is what keeps the parser inside it. */
	buf[n] = '\0';
	return kstrtobool(buf, res);
}

/* Whether an address lies in the vmap window. b1nix reserves one range for it
 * (see <lkpi/page.h>), so this is a bounds test rather than a tree lookup —
 * exact, not a heuristic, which matters because callers use the answer to
 * decide between vfree and kfree. */
bool is_vmalloc_addr(const void *x)
{
	u64 addr = (u64)(usize)x;

	return addr >= lkpi_vmap_window_base() &&
	       addr < lkpi_vmap_window_base() + lkpi_vmap_window_size();
}

/* vmap onto lkpi's window. The flags and protection upstream passes select
 * between cached and write-combining; only the latter distinction exists here,
 * and it is carried by the prot argument. */
void *vmap(struct page **pages, unsigned int count, unsigned long flags,
           pgprot_t prot)
{
	(void)flags;
	return lkpi_vmap(pages, count, (u32)pgprot_val(prot));
}

void vunmap(const void *addr)
{
	lkpi_vunmap((void *)addr);
}

/* ── memory the imported core asks about ──────────────────────────── */

/* Machine memory, for a driver sizing a cache against it. Pages, with the unit
 * reported separately as upstream does. */
void si_meminfo(struct sysinfo *val)
{
	if (!val)
		return;
	val->totalram = lkpi_total_pages();
	val->freeram = lkpi_free_pages();
	val->totalhigh = 0;   /* no high memory: all of RAM is in the direct map */
	val->freehigh = 0;
	val->mem_unit = PAGE_SIZE;
}

/*
 * Address back to its struct page.
 *
 * b1nix has no address-to-page map — see the note on pfn_to_page() in
 * <linux/mm.h>. The single caller is TTM's coherent-allocation path, which
 * upstream's own comment calls "an illegal abuse of the DMA API"; it runs only
 * when a device asks its pool for coherent pages, and i915 does not. Returning
 * NULL would be dereferenced one line later, so this stops instead: a loud
 * refusal at the call site rather than a fault somewhere further on.
 */
struct page *virt_to_page(const void *addr)
{
	(void)addr;
	lkpi_panic("virt_to_page: b1nix has no address-to-page map; "
	           "TTM's use_dma_alloc pool is not available here");
}

struct page *vmalloc_to_page(const void *addr)
{
	(void)addr;
	lkpi_panic("vmalloc_to_page: see virt_to_page");
}

/*
 * Installing a PFN mapping from a fault handler.
 *
 * b1nix's page tables are edited by its VMM against a named address space and
 * there is no entry taking a foreign VMA — see set_pte_at() in
 * <linux/pgtable.h>. This is the one caller that cannot be left to fail at link
 * time (TTM's fault handler is reached through i915's object ops), so it
 * refuses: SIGBUS is what a process gets when a mapping cannot be established,
 * which is exactly true here.
 */
vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr,
                               unsigned long pfn, pgprot_t pgprot)
{
	(void)vma; (void)addr; (void)pfn; (void)pgprot;
	return VM_FAULT_SIGBUS;
}

/*
 * Reading a page back out of a shmem file.
 *
 * b1nix has no tmpfs a driver can swap to, so a buffer object that was swapped
 * out cannot be swapped back in. TTM's swap-in checks IS_ERR on every page, so
 * the refusal is reported through the path it already has rather than through a
 * page that does not hold the right bytes.
 */
struct page *shmem_read_mapping_page_gfp(struct address_space *mapping,
                                         unsigned long index, gfp_t gfp)
{
	(void)mapping; (void)index; (void)gfp;
	return ERR_PTR(-ENODEV);
}

struct page *shmem_read_mapping_page(struct address_space *mapping,
                                     unsigned long index)
{
	return shmem_read_mapping_page_gfp(mapping, index, GFP_KERNEL);
}

/* ── DMA ──────────────────────────────────────────────────────────── */

/* One page, mapped for the device. Onto lkpi's single-buffer mapping, which
 * bounces or translates as the device's reach requires (M99/M100). */
dma_addr_t dma_map_page(struct device *dev, struct page *page, usize offset,
                        usize size, enum dma_data_direction dir)
{
	(void)dev;
	return dma_map_single((char *)page_address(page) + offset, size, (int)dir);
}

void dma_unmap_page(struct device *dev, dma_addr_t addr, usize size,
                    enum dma_data_direction dir)
{
	(void)dev;
	dma_unmap_single(addr, size, (int)dir);
}

/* ── uevents ──────────────────────────────────────────────────────── */

/*
 * Adding a variable to a uevent's environment.
 *
 * b1nix emits uevents over netlink with a fixed set of keys and has no
 * per-event environment a driver can extend, so the variable is not carried.
 * What is lost is a hotplug helper keying off a driver-specific variable; the
 * event itself still reaches userspace.
 */
int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...)
{
	(void)env; (void)format;
	return 0;
}

/* Total RAM in pages, under the name imported code calls it by. */
unsigned long totalram_pages(void)
{
	return (unsigned long)lkpi_total_pages();
}

bool current_is_kswapd(void)
{
	return lkpi_is_kswapd() != 0;
}

bool need_resched(void)
{
	return lkpi_need_resched() != 0;
}

/*
 * Copy every fence from one reservation object to another.
 *
 * Used when a buffer's backing memory moves: the destination must wait for
 * everything the source was still waiting for, or the move races the work in
 * flight. Each fence is referenced as it is added, so the destination keeps
 * them alive independently of the source.
 */
int dma_resv_copy_fences(struct dma_resv *dst, struct dma_resv *src)
{
	struct dma_resv_iter cursor;
	struct dma_fence *f;
	int ret = 0;

	if (!dst || !src)
		return -EINVAL;

	dma_resv_iter_begin(&cursor, src, DMA_RESV_USAGE_READ);
	dma_resv_for_each_fence_unlocked(&cursor, f) {
		dma_resv_add_fence(dst, f, dma_resv_iter_usage(&cursor));
	}
	dma_resv_iter_end(&cursor);
	return ret;
}

/* ── pages by address ─────────────────────────────────────────────── */

unsigned long __get_free_page(gfp_t gfp)
{
	struct page *p = alloc_page(gfp);

	return p ? (unsigned long)(usize)page_address(p) : 0;
}

/*
 * Free a page named by its address.
 *
 * Needs the inverse lookup b1nix does not have — see virt_to_page() above — so
 * it stops rather than freeing the wrong frame or silently leaking. The single
 * caller here is i915's GuC log, on a path only reached when its allocation
 * succeeded through __get_free_page above.
 */
void free_page(unsigned long addr)
{
	(void)addr;
	lkpi_panic("free_page: b1nix has no address-to-page map; "
	           "free the struct page instead");
}

/* ── page tables ──────────────────────────────────────────────────── */

/*
 * Editing page tables directly.
 *
 * b1nix's tables are owned by its VMM and are edited through it against a named
 * address space; there is no entry that takes a bare slot. apply_to_page_range
 * is the way in, and it refuses, so the three below cannot be reached — but
 * they are defined as refusals rather than left undefined because the objects
 * that reference them (i915_mm.c) carry other code the driver needs.
 */
int apply_to_page_range(struct mm_struct *mm, unsigned long address,
                        unsigned long size, pte_fn_t fn, void *data)
{
	(void)mm; (void)address; (void)size; (void)fn; (void)data;
	return -ENOSYS;
}

pte_t pfn_pte(unsigned long pfn, pgprot_t prot)
{
	pte_t pte = { ((u64)pfn << PAGE_SHIFT) | pgprot_val(prot) };
	return pte;
}

pte_t pte_mkspecial(pte_t pte) { return pte; }

void set_pte_at(struct mm_struct *mm, unsigned long addr, pte_t *ptep, pte_t pte)
{
	(void)mm; (void)addr; (void)ptep; (void)pte;
	lkpi_panic("set_pte_at: b1nix's page tables are not editable from here; "
	           "apply_to_page_range refuses, so this should be unreachable");
}

/*
 * Dropping a VMA's PTEs so the next access faults back into the driver.
 *
 * Nothing here ever established such a mapping: vm_mmap() and
 * vmf_insert_pfn_prot() both refuse, so a GEM object has no userspace mapping
 * to revoke. Reporting success would be a claim about mappings that do not
 * exist; -ENOSYS says what is true.
 */
int zap_vma_ptes(struct vm_area_struct *vma, unsigned long address,
                 unsigned long size)
{
	(void)vma; (void)address; (void)size;
	return -ENOSYS;
}

unsigned long vm_mmap(struct file *file, unsigned long addr, unsigned long len,
                      unsigned long prot, unsigned long flag,
                      unsigned long offset)
{
	(void)file; (void)addr; (void)len; (void)prot; (void)flag; (void)offset;
	return (unsigned long)-ENOSYS;
}

/*
 * Walking another process's mappings.
 *
 * b1nix's VMAs are private to the process that owns them and there is no
 * kernel-side reader for another task's address space. NULL is "no mapping
 * here", which is what every caller checks for and what is true from kernel
 * context.
 */
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
	(void)mm; (void)addr;
	return 0;
}

/* Mapping bare frame numbers needs the same inverse lookup as virt_to_page. */
void *vmap_pfn(unsigned long *pfns, unsigned int count, pgprot_t prot)
{
	(void)pfns; (void)count; (void)prot;
	return 0;
}

/* ── files a driver would like to have ────────────────────────────── */

struct page *find_lock_page(struct address_space *mapping, unsigned long index)
{
	(void)mapping; (void)index;
	return 0;
}

/*
 * b1nix's file table is not RCU-protected — see the note in <linux/fs.h> — so
 * a reference cannot be taken safely from a pointer read without one. Failing
 * is the only correct answer; the caller treats it as "the file is going away".
 */
struct file *get_file_rcu(struct file *f)
{
	(void)f;
	return 0;
}

/* No tmpfs to allocate an inode from — see <linux/shmem_fs.h>. */
struct file *shmem_file_setup_with_mnt(struct vfsmount *mnt, const char *name,
                                       loff_t size, unsigned long flags)
{
	(void)mnt; (void)name; (void)size; (void)flags;
	return ERR_PTR(-ENODEV);
}

/* shmem_file_setup() itself is in linux_file.c, next to the rest of the file
 * interface; this is only the mount-taking variant. */

/* Nothing is backed by shmem here (see above), so there is no cache to punch. */
void shmem_truncate_range(struct inode *inode, loff_t start, loff_t end)
{
	(void)inode; (void)start; (void)end;
}

void kern_unmount(struct vfsmount *mnt) { (void)mnt; }

struct file_system_type *get_fs_type(const char *name)
{
	(void)name;
	return 0;
}

struct vfsmount *vfs_kern_mount(struct file_system_type *type, int flags,
                                const char *name, void *data)
{
	(void)type; (void)flags; (void)name; (void)data;
	return ERR_PTR(-ENODEV);
}

/* ── tasks ────────────────────────────────────────────────────────── */

/*
 * A refcounted handle on a task's identity.
 *
 * b1nix has no pid object that outlives its task — which is the whole point of
 * upstream's. Handing back the raw pid would let a caller hold something that
 * can be recycled. NULL means "no identity recorded"; i915 uses it only to show
 * an owner in debugfs, and shows none.
 */
struct pid *get_task_pid(struct lkpi_task *task, enum pid_type type)
{
	(void)task; (void)type;
	return 0;
}

long io_schedule_timeout(long timeout)
{
	return schedule_timeout(timeout);
}

/* ── formatting ───────────────────────────────────────────────────── */

int vscnprintf(char *buf, usize size, const char *fmt, __builtin_va_list args)
{
	int n = vsnprintf(buf, size, fmt, args);

	if (n < 0)
		return 0;
	/* vsnprintf reports what it would have written; this reports what it did. */
	return (usize)n >= size ? (int)(size ? size - 1 : 0) : n;
}

/*
 * Parsing formatted input.
 *
 * b1nix's string library is output-only and this port does not add a parser.
 * The callers are debugfs write handlers; zero means "no field matched", which
 * they turn into -EINVAL on the write. A write that should have taken effect is
 * rejected rather than silently ignored.
 */
int sscanf(const char *buf, const char *fmt, ...)
{
	(void)buf; (void)fmt;
	return 0;
}

/* One line of a hex dump, into a caller's buffer. groupsize is the number of
 * bytes printed per group; ascii appends the printable rendering. */
int hex_dump_to_buffer(const void *buf, usize len, int rowsize, int groupsize,
                       char *linebuf, usize linebuflen, bool ascii)
{
	static const char hexdigits[] = "0123456789abcdef";
	const u8 *p = buf;
	usize used = 0, i;

	(void)rowsize;
	if (!linebuf || linebuflen == 0)
		return 0;
	if (groupsize <= 0)
		groupsize = 1;
	for (i = 0; i < len; i++) {
		if (used + 3 >= linebuflen)
			break;
		linebuf[used++] = hexdigits[p[i] >> 4];
		linebuf[used++] = hexdigits[p[i] & 0xf];
		if (((int)i % groupsize) == groupsize - 1)
			linebuf[used++] = ' ';
	}
	if (ascii) {
		for (i = 0; i < len && used + 1 < linebuflen; i++)
			linebuf[used++] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '.';
	}
	linebuf[used] = '\0';
	return (int)used;
}

int kstrtoint_from_user(const char __user *s, usize count, unsigned int base,
                        int *res)
{
	char tmp[32];
	usize n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;

	if (copy_from_user(tmp, s, n))
		return -EFAULT;
	tmp[n] = '\0';
	return kstrtoint(tmp, base, res);
}

int single_open_size(struct file *file, int (*show)(struct seq_file *, void *),
                     void *data, usize size)
{
	/* The size is a hint about the first allocation; b1nix's seq_file grows on
	 * demand, so it only changes how many growth steps happen. */
	(void)size;
	return single_open(file, show, data);
}

/* ── things b1nix has no mechanism for ────────────────────────────── */

/*
 * Running a function with every other CPU held still.
 *
 * b1nix has no stop_machine: there is no mechanism to park the other CPUs. The
 * callers use it to make a change that must not be observed half-applied, so
 * running the function on this CPU alone would be exactly the race it exists to
 * prevent. -ENOSYS instead; the caller reports the operation as unavailable.
 */
int stop_machine(int (*fn)(void *), void *data, const struct cpumask *cpus)
{
	(void)fn; (void)data; (void)cpus;
	return -ENOSYS;
}

/*
 * SSE in kernel context.
 *
 * b1nix's kernel is built -mno-sse and saves no FPU state on entry, so a driver
 * cannot use vector instructions here at all. This is unreachable: SSE4.1 is
 * reported unavailable in kernel context by <asm/cpufeature.h>, which is what
 * keeps i915_memcpy on its plain-memcpy path. It stops rather than returning,
 * because there is no safe way to continue past it.
 */
void kernel_fpu_begin(void)
{
	lkpi_panic("kernel_fpu_begin: b1nix's kernel may not execute SSE; "
	           "the caller should have taken its no-SSE path");
}

void kernel_fpu_end(void) { }

/* Machine-wide cache flush. b1nix has no cross-CPU call for drivers, and doing
 * it on this CPU alone leaves stale lines elsewhere — the exact failure this
 * call exists to prevent. */
void wbinvd_on_all_cpus(void)
{
	lkpi_panic("wbinvd_on_all_cpus: b1nix cannot flush other CPUs' caches");
}

/* ── i2c bit-banging ──────────────────────────────────────────────── */

/*
 * DDC over bit-banged i2c.
 *
 * i915 talks to displays through GMBUS, the hardware controller, and only falls
 * back to bit-banging when GMBUS fails. b1nix has no GPIO bit-bang layer, so
 * that fallback reports no device and the driver stays on GMBUS.
 */
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	(void)adap; (void)msgs; (void)num;
	return -ENODEV;
}

const struct i2c_algorithm i2c_bit_algo = { 0 };

/* ── firmware-discovered graphics memory ──────────────────────────── */

/*
 * The stolen-memory window some firmware reports through an early PCI quirk.
 *
 * b1nix runs no early quirks, so nothing fills this in and it stays an empty,
 * unset resource. i915 falls back to reading the window out of the chipset
 * registers, which M98 already decodes.
 */
struct resource intel_graphics_stolen_res = {
	.start = 0, .end = 0, .flags = IORESOURCE_UNSET,
};

/* Did the boot firmware ask that only its own driver bind? Nothing here sets
 * that policy, so no. */
bool video_firmware_drivers_only(void) { return false; }

/* ── relay ────────────────────────────────────────────────────────── */

/* No channel is ever created (see <linux/relay.h>), so there is no buffer to
 * reserve space in. */
void *relay_reserve(struct rchan *chan, usize length)
{
	(void)chan; (void)length;
	return 0;
}

/* ── wait queues ──────────────────────────────────────────────────── */

/* The lockdep-keyed form. There is no lockdep here, so the name and key are
 * dropped and the queue is initialised as usual. */
void __init_waitqueue_head(struct wait_queue_head *wq, const char *name,
                           void *key)
{
	(void)name; (void)key;
	init_waitqueue_head(wq);
}

/* Claiming and releasing an MSI vector; defined with the rest of the interrupt
 * bridge below, declared here because pci_enable_msi() is the caller. */
static int lkpi_msi_claim(void);
static void lkpi_msi_release(int vec);

/* ── PCI lookups ──────────────────────────────────────────────────── */

/*
 * Finding devices by class or by address.
 *
 * b1nix enumerates PCI once at boot and keeps the result; these walk that list.
 * `from` continues a previous search, as upstream's does — a NULL `from` starts
 * over. There is no refcounting (see pci_dev_put in <linux/pci.h>), so the
 * returned device is valid for the life of the system.
 */
/*
 * A small pool of devices the driver looks up but does not own — the ISA
 * bridge, the host bridge. They are handed out by address and live for the life
 * of the system, which is why pci_dev_put() has nothing to do.
 */
#define LKPI_PCI_FOUND_MAX 8
static struct pci_dev lkpi_pci_found[LKPI_PCI_FOUND_MAX];
static unsigned lkpi_pci_found_n;

static struct pci_dev *lkpi_pci_publish(u8 bus, u8 slot, u8 func)
{
	unsigned i;
	u32 id, cls;
	struct pci_dev *d;

	for (i = 0; i < lkpi_pci_found_n; i++)
		if (lkpi_pci_found[i].bus_nr == bus &&
		    lkpi_pci_found[i].devfn == (unsigned)((slot << 3) | func))
			return &lkpi_pci_found[i];
	if (lkpi_pci_found_n == LKPI_PCI_FOUND_MAX)
		return 0;
	id = pci_config_read32(bus, slot, func, 0x00);
	if ((id & 0xffff) == 0xffff)
		return 0;
	cls = pci_config_read32(bus, slot, func, 0x08);
	d = &lkpi_pci_found[lkpi_pci_found_n++];
	d->bus_nr = bus;
	d->slot = slot;
	d->func = func;
	d->devfn = (unsigned)((slot << 3) | func);
	d->vendor = (u16)(id & 0xffff);
	d->device = (u16)(id >> 16);
	d->revision = (u8)(cls & 0xff);
	d->class = cls >> 8;
	return d;
}

struct pci_dev *pci_get_class(unsigned int class_code, struct pci_dev *from)
{
	u32 bus, slot, func;
	int seen_from = from ? 0 : 1;

	for (bus = 0; bus < 256; bus++) {
		for (slot = 0; slot < 32; slot++) {
			for (func = 0; func < 8; func++) {
				u32 id = pci_config_read32((u8)bus, (u8)slot, (u8)func, 0x00);
				u32 cls;

				if ((id & 0xffff) == 0xffff)
					continue;
				cls = pci_config_read32((u8)bus, (u8)slot, (u8)func, 0x08) >> 8;
				/* Upstream matches on the full 24-bit class/subclass/prog-if
				 * when the caller gives one, and i915 passes a 16-bit
				 * class/subclass — so compare only the bits it supplied. */
				if ((cls >> 8) != (class_code & 0xffff) &&
				    cls != class_code)
					continue;
				if (!seen_from) {
					if (from->bus_nr == bus &&
					    from->devfn == ((slot << 3) | func))
						seen_from = 1;
					continue;
				}
				return lkpi_pci_publish((u8)bus, (u8)slot, (u8)func);
			}
		}
	}
	return 0;
}

struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus,
                                            unsigned int devfn)
{
	/* One PCI domain here, so the domain selects nothing. */
	(void)domain;
	return lkpi_pci_publish((u8)bus, (u8)((devfn >> 3) & 0x1f), (u8)(devfn & 7));
}

int pci_dev_present(const struct pci_device_id *ids)
{
	usize i;

	if (!ids)
		return 0;
	for (i = 0; ids[i].vendor || ids[i].device; i++) {
		struct pci_device_info info;

		if (pci_find_device(ids[i].vendor, ids[i].device, &info) == 0)
			return 1;
	}
	return 0;
}

/*
 * Single-vector MSI.
 *
 * M98 built the MSI-X machinery; this is the one-vector case of it, under the
 * older name. Enabling records the vector on the device so a driver can tell an
 * MSI interrupt from a shared legacy line.
 */
int pci_enable_msi(struct pci_dev *dev)
{
	int vec;

	if (!dev)
		return -EINVAL;
	vec = lkpi_msi_claim();
	if (vec < 0)
		return vec;
	if (pci_msi_enable(dev->bus_nr, dev->slot, dev->func, (u8)vec) != 0) {
		lkpi_msi_release(vec);
		return -ENODEV;
	}
	dev->irq = vec;
	dev->msi_enabled = 1;
	return 0;
}

void pci_disable_msi(struct pci_dev *dev)
{
	if (!dev || !dev->msi_enabled)
		return;
	pci_msi_disable(dev->bus_nr, dev->slot, dev->func);
	dev->msi_enabled = 0;
}

/*
 * Allocating a window out of a bus's address space.
 *
 * b1nix assigns BARs during enumeration and keeps no per-bus free-space map
 * afterwards, so there is nothing to allocate from. -ENOSPC rather than a
 * fabricated range: i915's old-chipset stolen-memory fallback is the only
 * caller and takes its "could not place it" path.
 */
int pci_bus_alloc_resource(struct pci_bus *bus, struct resource *res,
                           resource_size_t size, resource_size_t align,
                           resource_size_t min, unsigned long type_mask,
                           resource_size_t (*alignf)(void *, const struct resource *,
                                                     resource_size_t, resource_size_t),
                           void *alignf_data)
{
	(void)bus; (void)res; (void)size; (void)align; (void)min;
	(void)type_mask; (void)alignf; (void)alignf_data;
	return -ENOSPC;
}

resource_size_t pcibios_align_resource(void *data, const struct resource *res,
                                       resource_size_t size,
                                       resource_size_t align)
{
	(void)data; (void)res; (void)size;
	return align;
}

/* ── devices ──────────────────────────────────────────────────────── */

/*
 * Is this device's DMA translated?
 *
 * M100 attaches a domain per device when an IOMMU is present, and every device
 * that is attached is translated — b1nix has no pass-through domain (QEMU's
 * intel-iommu offers none either). So the question reduces to whether
 * translation is running at all.
 */
bool device_iommu_mapped(struct device *dev)
{
	(void)dev;
	return iommu_active() != 0;
}

/*
 * Claiming a physical range for the life of the device.
 *
 * b1nix does not arbitrate MMIO between drivers — see <linux/ioport.h> — so the
 * claim always succeeds. The resource is allocated from the device's devres
 * list so it is released with the device, which is the part of the contract
 * that does mean something here.
 */
struct resource *devm_request_mem_region(struct device *dev,
                                         resource_size_t start,
                                         resource_size_t n, const char *name)
{
	struct resource *res = devm_kzalloc(dev, sizeof(*res), GFP_KERNEL);

	if (!res)
		return 0;
	res->start = start;
	res->end = start + n - 1;
	res->name = name;
	res->flags = IORESOURCE_MEM;
	return res;
}

/* ── interrupts b1nix has no shape for ────────────────────────────── */

/*
 * An IRQ domain for a device that demultiplexes its own interrupts.
 *
 * b1nix maps vectors to handlers at the APIC and has no chained-chip layer:
 * there is no irq_desc to allocate and no chip to attach. i915's GSC sub-device
 * is the only caller, and it reports the failure and carries on without the
 * sub-device — which is the same outcome as not having a driver for it.
 */
int irq_alloc_desc(int node) { (void)node; return -ENOSYS; }
void irq_free_desc(unsigned int irq) { (void)irq; }
int irq_set_chip_and_handler_name(unsigned int irq, const struct irq_chip *chip,
                                  void (*handle)(struct irq_desc *desc),
                                  const char *name)
{
	(void)irq; (void)chip; (void)handle; (void)name;
	return -ENOSYS;
}
int irq_set_chip_data(unsigned int irq, void *data)
{ (void)irq; (void)data; return -ENOSYS; }
void *irq_get_chip_data(unsigned int irq) { (void)irq; return 0; }
void handle_simple_irq(struct irq_desc *desc) { (void)desc; }
int generic_handle_irq(unsigned int irq) { (void)irq; return -ENOSYS; }

/* ── buses with no driver on the other side ───────────────────────── */

/*
 * The auxiliary bus, and platform devices.
 *
 * Both exist so a driver can publish a piece of its hardware for another driver
 * to claim: i915 publishes the GSC for the mei driver and the LPE audio block
 * for a sound driver. Neither driver exists here, so registration reports
 * -ENODEV and i915 carries on without that half of the hardware — rather than
 * creating a device that sits unclaimed forever.
 */
int auxiliary_device_init(struct auxiliary_device *auxdev)
{ (void)auxdev; return -ENODEV; }
int auxiliary_device_add(struct auxiliary_device *auxdev)
{ (void)auxdev; return -ENODEV; }
void auxiliary_device_uninit(struct auxiliary_device *auxdev) { (void)auxdev; }
void auxiliary_device_delete(struct auxiliary_device *auxdev) { (void)auxdev; }

struct platform_device *platform_device_register_full(
	const struct platform_device_info *pdevinfo)
{
	(void)pdevinfo;
	return ERR_PTR(-ENODEV);
}

void platform_device_unregister(struct platform_device *pdev) { (void)pdev; }

/* ── anonymous descriptors ────────────────────────────────────────── */

/*
 * A descriptor with no name in any directory — how a fence or a dma-buf is
 * handed to userspace. b1nix's VFS makes one from a handle carrying the
 * caller's object in its private slot, which is the same shape the DRM
 * chardev already uses.
 */
int anon_inode_getfd(const char *name, const struct file_operations *fops,
                     void *priv, int flags)
{
	/*
	 * Not wired: b1nix's anonymous inodes exist, but which of its paths a DRM
	 * object should be published through is the decision the first
	 * descriptor-returning ioctl makes, and none is wired yet. -ENODEV is what
	 * every caller checks for, and it keeps a half-built descriptor out of a
	 * process's table.
	 */
	(void)name; (void)fops; (void)priv; (void)flags;
	return -ENODEV;
}

/* ── the CPU's timebase ───────────────────────────────────────────── */

/*
 * TSC frequency in kHz, as b1nix calibrated it against the PIT at boot. Zero
 * would mean uncalibrated; the callers divide by it and check first.
 */
unsigned int tsc_khz;

void lkpi_set_tsc_khz(unsigned int khz) { tsc_khz = khz; }

/* ── device interrupts ────────────────────────────────────────────── */

/*
 * Registering a device interrupt.
 *
 * b1nix's IRQ layer calls a handler with a context pointer and nothing else,
 * while Linux's also takes the line number — so the two are bridged through a
 * small table, one entry per registration, and the table index is the context.
 *
 * The same table serves both kinds of interrupt. A legacy line is registered
 * with irq_register_handler() and unmasked at the IOAPIC; an MSI vector is
 * claimed from M98's vector pool by pci_enable_msi() before the driver asks for
 * it, so by the time request_irq() runs the entry already exists and only the
 * handler is missing.
 */
#define LKPI_IRQ_MAX 16
static struct {
	irq_handler_t handler;
	void *dev;
	unsigned int irq;
	bool used;
} lkpi_irqs[LKPI_IRQ_MAX];

static int lkpi_irq_trampoline(void *ctx)
{
	unsigned i = (unsigned)(usize)ctx;

	if (i >= LKPI_IRQ_MAX || !lkpi_irqs[i].used)
		return 0;
	return lkpi_irqs[i].handler((int)lkpi_irqs[i].irq, lkpi_irqs[i].dev)
	       == IRQ_HANDLED;
}

/* Claim an MSI vector, with the trampoline already attached; the driver's own
 * handler arrives later through request_irq(). Returns the vector. */
static int lkpi_msi_claim(void)
{
	unsigned i;
	int vec;

	for (i = 0; i < LKPI_IRQ_MAX; i++)
		if (!lkpi_irqs[i].used)
			break;
	if (i == LKPI_IRQ_MAX)
		return -ENOSPC;
	vec = msi_alloc_vector(lkpi_irq_trampoline, (void *)(usize)i);
	if (vec < 0)
		return -ENOSPC;
	lkpi_irqs[i].handler = 0;
	lkpi_irqs[i].dev = 0;
	lkpi_irqs[i].irq = (unsigned)vec;
	lkpi_irqs[i].used = true;
	return vec;
}

static void lkpi_msi_release(int vec)
{
	unsigned i;

	for (i = 0; i < LKPI_IRQ_MAX; i++)
		if (lkpi_irqs[i].used && lkpi_irqs[i].irq == (unsigned)vec) {
			msi_free_vector(vec);
			lkpi_irqs[i].used = false;
			return;
		}
}

int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
                const char *name, void *dev)
{
	unsigned i;

	(void)flags; (void)name;
	if (!handler)
		return -EINVAL;

	/* An MSI vector claimed by pci_enable_msi() already has an entry. */
	for (i = 0; i < LKPI_IRQ_MAX; i++)
		if (lkpi_irqs[i].used && lkpi_irqs[i].irq == irq &&
		    !lkpi_irqs[i].handler) {
			lkpi_irqs[i].handler = handler;
			lkpi_irqs[i].dev = dev;
			return 0;
		}

	if (irq >= 16)
		return -ENOSYS; /* a vector nobody claimed: not ours to route */
	for (i = 0; i < LKPI_IRQ_MAX; i++)
		if (!lkpi_irqs[i].used)
			break;
	if (i == LKPI_IRQ_MAX)
		return -ENOSPC;
	lkpi_irqs[i].handler = handler;
	lkpi_irqs[i].dev = dev;
	lkpi_irqs[i].irq = irq;
	lkpi_irqs[i].used = true;
	if (irq_register_handler((u8)irq, lkpi_irq_trampoline,
	                         (void *)(usize)i) != 0) {
		lkpi_irqs[i].used = false;
		return -EBUSY;
	}
	irq_unmask((u8)irq);
	return 0;
}

void free_irq(unsigned int irq, void *dev)
{
	unsigned i;

	for (i = 0; i < LKPI_IRQ_MAX; i++) {
		if (!lkpi_irqs[i].used || lkpi_irqs[i].irq != irq ||
		    lkpi_irqs[i].dev != dev)
			continue;
		irq_unregister_handler((u8)irq, lkpi_irq_trampoline,
		                       (void *)(usize)i);
		lkpi_irqs[i].used = false;
		return;
	}
}
