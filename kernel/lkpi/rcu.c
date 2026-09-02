/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M101 linuxkpi: RCU.
 *
 * See <lkpi/rcu.h> for the contract. This file is the two-bucket reader count
 * and the thread that runs deferred callbacks.
 *
 * Why the reader state is a per-CPU array rather than something hung off the
 * task: rcu_read_lock disables interrupts, so a reader cannot be preempted and
 * cannot migrate, which makes "this CPU's slot" a stable place to keep the
 * nesting depth and the bucket that was taken. It also means the scheduler is
 * never involved in the read path at all.
 */

#include <b1nix/arch.h>
#include <b1nix/lapic.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <lkpi/kthread_worker.h>
#include <lkpi/lock.h>
#include <lkpi/rcu.h>

/* Which bucket new readers join. Flipped by a writer starting a grace period. */
static volatile u32 g_rcu_idx;

/* Readers currently inside a section, per bucket. A grace period is "the old
 * bucket reached zero". */
static volatile i32 g_rcu_readers[2];

/* Per-CPU read-side state. Safe without a lock because the owning CPU has
 * interrupts disabled for the whole time these are non-zero. */
static u32 g_rcu_nesting[MAX_CPUS];
static u32 g_rcu_bucket[MAX_CPUS];
static u64 g_rcu_flags[MAX_CPUS];

/* Only one grace period runs at a time; concurrent ones would each flip the
 * index and could then wait on a bucket the other already drained. */
static struct lkpi_mutex g_rcu_gp_lock;
static u32 g_rcu_gp_lock_ready;

static volatile u64 g_rcu_gps;
static volatile u64 g_rcu_cb_invoked;

/* Callback queue, drained by one worker after a grace period. */
static spinlock_t g_rcu_cb_lock = SPINLOCK_INIT;
static struct rcu_head *g_rcu_cb_head;
static struct rcu_head *g_rcu_cb_tail;
static struct kthread_worker *g_rcu_worker;
static struct kthread_work g_rcu_work;
static volatile u64 g_rcu_batches_done;

static u32 rcu_cpu(void)
{
	struct percpu *pc = get_percpu();
	u32 id = pc ? pc->cpu_id : 0;
	return (id < MAX_CPUS) ? id : 0;
}

void rcu_read_lock(void)
{
	u64 flags = interrupts_save();
	u32 cpu = rcu_cpu();

	if (g_rcu_nesting[cpu]++ == 0) {
		/* Join whichever bucket is current. A flip racing this either happens
		 * before the load — in which case we join the new bucket and cannot
		 * hold that writer up — or after, in which case we are in the bucket it
		 * is about to wait on, which is exactly what makes it wait for us. */
		u32 idx = __atomic_load_n(&g_rcu_idx, __ATOMIC_ACQUIRE) & 1u;
		g_rcu_bucket[cpu] = idx;
		__atomic_fetch_add(&g_rcu_readers[idx], 1, __ATOMIC_ACQ_REL);
		g_rcu_flags[cpu] = flags;
		return;
	}
	/* Nested: interrupts were already off, so the saved state that matters is
	 * the outermost one, already stored. */
	(void)flags;
}

void rcu_read_unlock(void)
{
	u32 cpu = rcu_cpu();
	if (g_rcu_nesting[cpu] == 0)
		return; /* unbalanced unlock; ignore rather than corrupt the count */

	if (--g_rcu_nesting[cpu] == 0) {
		u32 idx = g_rcu_bucket[cpu] & 1u;
		u64 flags = g_rcu_flags[cpu];
		__atomic_fetch_sub(&g_rcu_readers[idx], 1, __ATOMIC_ACQ_REL);
		interrupts_restore(flags);
	}
}

int rcu_read_lock_held(void)
{
	u64 flags = interrupts_save();
	int held = g_rcu_nesting[rcu_cpu()] != 0;
	interrupts_restore(flags);
	return held;
}

void synchronize_rcu(void)
{
	if (!g_rcu_gp_lock_ready) {
		lkpi_mutex_init(&g_rcu_gp_lock);
		g_rcu_gp_lock_ready = 1;
	}
	/* Serialise: two writers flipping the index concurrently could each wait on
	 * a bucket the other had already drained and both return early. */
	lkpi_mutex_lock(&g_rcu_gp_lock);

	u32 old = __atomic_load_n(&g_rcu_idx, __ATOMIC_ACQUIRE) & 1u;
	__atomic_store_n(&g_rcu_idx, old ^ 1u, __ATOMIC_RELEASE);

	/* Everything that could still be looking at the pre-flip value is counted
	 * in `old`. Waiting for it to reach zero is the grace period. */
	while (__atomic_load_n(&g_rcu_readers[old], __ATOMIC_ACQUIRE) > 0) {
		if (scheduler_can_block())
			scheduler_sleep_ticks(1);
		else
			cpu_relax();
	}

	__atomic_fetch_add(&g_rcu_gps, 1ull, __ATOMIC_ACQ_REL);
	lkpi_mutex_unlock(&g_rcu_gp_lock);
}

/* Drain everything queued so far, after one grace period. Requeued by call_rcu
 * whenever the list is non-empty, so a callback added while this runs gets its
 * own grace period rather than riding on one that started before it. */
static void rcu_cb_worker(struct kthread_work *work)
{
	(void)work;

	for (;;) {
		u64 flags;
		spin_lock_irqsave(&g_rcu_cb_lock, &flags);
		struct rcu_head *batch = g_rcu_cb_head;
		g_rcu_cb_head = 0;
		g_rcu_cb_tail = 0;
		spin_unlock_irqrestore(&g_rcu_cb_lock, flags);

		if (!batch)
			break;

		/* The grace period must cover the whole batch, so it is taken after the
		 * list is detached: every callback on it was queued before this. */
		synchronize_rcu();

		while (batch) {
			struct rcu_head *next = batch->next;
			if (batch->func)
				batch->func(batch);
			__atomic_fetch_add(&g_rcu_cb_invoked, 1ull, __ATOMIC_ACQ_REL);
			batch = next;
		}
	}
	__atomic_fetch_add(&g_rcu_batches_done, 1ull, __ATOMIC_ACQ_REL);
}

void rcu_init(void)
{
	if (g_rcu_worker)
		return;
	lkpi_mutex_init(&g_rcu_gp_lock);
	g_rcu_gp_lock_ready = 1;
	kthread_init_work(&g_rcu_work, rcu_cb_worker);
	g_rcu_worker = kthread_create_worker("lkpi-rcu");
	if (g_rcu_worker) {
		/* Anything queued before the thread existed is still on the list. */
		u64 flags;
		spin_lock_irqsave(&g_rcu_cb_lock, &flags);
		int pending = g_rcu_cb_head != 0;
		spin_unlock_irqrestore(&g_rcu_cb_lock, flags);
		if (pending)
			kthread_queue_work(g_rcu_worker, &g_rcu_work);
	}
}

void call_rcu(struct rcu_head *head, rcu_callback_t func)
{
	if (!head)
		return;
	head->func = func;
	head->next = 0;

	u64 flags;
	spin_lock_irqsave(&g_rcu_cb_lock, &flags);
	if (g_rcu_cb_tail)
		g_rcu_cb_tail->next = head;
	else
		g_rcu_cb_head = head;
	g_rcu_cb_tail = head;
	spin_unlock_irqrestore(&g_rcu_cb_lock, flags);

	if (g_rcu_worker)
		kthread_queue_work(g_rcu_worker, &g_rcu_work);
}

void rcu_barrier(void)
{
	if (!g_rcu_worker) {
		/* No thread yet: run the queue here. Only reachable during early init,
		 * where there is nothing to race with. */
		rcu_cb_worker(0);
		return;
	}
	/* Make sure a drain is scheduled even if the queue was emptied and the work
	 * item has already finished, then wait for it to complete. */
	kthread_queue_work(g_rcu_worker, &g_rcu_work);
	kthread_flush_work(&g_rcu_work);

	/* A callback queued while that batch was running left the list non-empty
	 * and requeued the item; keep flushing until it is genuinely idle. */
	for (;;) {
		u64 flags;
		spin_lock_irqsave(&g_rcu_cb_lock, &flags);
		int pending = g_rcu_cb_head != 0;
		spin_unlock_irqrestore(&g_rcu_cb_lock, flags);
		if (!pending)
			return;
		kthread_queue_work(g_rcu_worker, &g_rcu_work);
		kthread_flush_work(&g_rcu_work);
	}
}

u64 rcu_grace_periods(void)
{
	return __atomic_load_n(&g_rcu_gps, __ATOMIC_ACQUIRE);
}

u64 rcu_callbacks_invoked(void)
{
	return __atomic_load_n(&g_rcu_cb_invoked, __ATOMIC_ACQUIRE);
}
