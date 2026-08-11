/*
 * SPDX-License-Identifier: MIT
 *
 * M101 linuxkpi self-test: kref, wait queues, wound/wait mutexes, rbtree.
 *
 * Each check is built so that the obvious wrong implementation fails it:
 *
 *   - kref's release must run exactly once even though the count is dropped
 *     from two places, so the release counter is asserted, not just the flag.
 *   - the wait-queue timeout is checked against the scheduler's own tick count,
 *     so a wait that returns early or never parks is caught.
 *   - the ww_mutex wound path is driven deterministically — an older context is
 *     made to collide with a younger holder on purpose — so "no deadlock
 *     happened" cannot be explained by the race never occurring.
 *   - the rbtree is fed strictly ascending keys, the input that turns an
 *     unbalanced tree into a linked list, and the balance invariants are
 *     verified rather than inferred from lookups succeeding.
 */

#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/lapic.h>
#include <b1nix/memtype.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/tlb.h>
#include <lkpi/lkpi.h>

static void m101_report(const char *name, int ok, u64 detail)
{
	console_write(ok ? "M101-SMOKE: ok " : "M101-SMOKE: FAIL ");
	console_write(name);
	console_write(" detail=");
	console_write_dec(detail);
	console_write("\n");
}

/* ── kref ───────────────────────────────────────────────────────── */

struct kref_object {
	struct kref ref;
	u32 payload;
};

static volatile u32 g_kref_releases;
static struct kref_object *g_kref_released;

static void kref_object_release(struct kref *kref)
{
	struct kref_object *obj = kref_container_of(kref, struct kref_object, ref);
	g_kref_released = obj;
	g_kref_releases++;
}

static void test_kref(void)
{
	int ok = 1;
	static struct kref_object obj;

	obj.payload = 0xC0FFEEu;
	g_kref_releases = 0;
	g_kref_released = 0;

	kref_init(&obj.ref);
	if (kref_read(&obj.ref) != 1)
		ok = 0;

	/* Two extra references, dropped from "elsewhere". Only the last drop may
	 * release, and the count must track exactly. */
	kref_get(&obj.ref);
	kref_get(&obj.ref);
	if (kref_read(&obj.ref) != 3)
		ok = 0;

	if (kref_put(&obj.ref, kref_object_release))
		ok = 0; /* released too early */
	if (kref_put(&obj.ref, kref_object_release))
		ok = 0;
	if (g_kref_releases != 0)
		ok = 0;
	if (kref_read(&obj.ref) != 1)
		ok = 0;

	/* A weak reference taken while the object is still alive must succeed and
	 * must keep it alive. */
	if (!kref_get_unless_zero(&obj.ref))
		ok = 0;
	if (kref_put(&obj.ref, kref_object_release))
		ok = 0;

	/* Final drop: release runs, once, on the right object. */
	if (!kref_put(&obj.ref, kref_object_release))
		ok = 0;
	if (g_kref_releases != 1)
		ok = 0;
	if (g_kref_released != &obj || g_kref_released->payload != 0xC0FFEEu)
		ok = 0;

	/* The object is dead now: a weak reference must report that rather than
	 * resurrect it. */
	if (kref_get_unless_zero(&obj.ref))
		ok = 0;
	if (kref_read(&obj.ref) != 0)
		ok = 0;

	m101_report("kref", ok, g_kref_releases);
}

/* ── wait queue ─────────────────────────────────────────────────── */

static struct wait_queue_head g_wq;
static volatile u32 g_wq_condition;
static volatile int g_wq_thread_done;

static void wq_thread(void *arg)
{
	(void)arg;
	/* Sleep first, so the waiter is genuinely parked before the wake. A
	 * wake that arrives before the wait is a different (also valid) path,
	 * tested separately below by setting the condition up front. */
	scheduler_sleep_ticks(3);
	g_wq_condition = 0xABCDu;
	wake_up(&g_wq);
	g_wq_thread_done = 1;
	scheduler_exit_current(0);
}

static void test_waitqueue(void)
{
	int ok = 1;
	init_waitqueue_head(&g_wq);
	g_wq_condition = 0;
	g_wq_thread_done = 0;

	/* Condition already true: must not park at all. */
	g_wq_condition = 1;
	wait_event(g_wq, g_wq_condition != 0);
	if (waitqueue_waiters(&g_wq) != 0)
		ok = 0;

	/* Real sleep/wake across two contexts. */
	g_wq_condition = 0;
	if (kthread_create("lkpi-wq", wq_thread, 0) < 0) {
		ok = 0;
	} else {
		wait_event(g_wq, g_wq_condition == 0xABCDu);
		if (g_wq_condition != 0xABCDu)
			ok = 0;
		if (waitqueue_wakeups(&g_wq) < 1)
			ok = 0;
		/* Every waiter must have left the queue's books. */
		if (waitqueue_waiters(&g_wq) != 0)
			ok = 0;
		for (int i = 0; i < 500 && !g_wq_thread_done; i++)
			scheduler_sleep_ticks(1);
		if (!g_wq_thread_done)
			ok = 0;
	}

	m101_report("waitqueue", ok, waitqueue_wakeups(&g_wq));
}

static void test_waitqueue_timeout(void)
{
	int ok = 1;
	static struct wait_queue_head wq;
	init_waitqueue_head(&wq);
	volatile u32 never = 0;

	/* A condition that never becomes true must return 0 and must actually have
	 * waited: the scheduler's own tick count is the independent witness. */
	u64 before = scheduler_get_ticks();
	u64 ret = 1;
	wait_event_timeout_r(wq, never != 0, 10ull, ret);
	u64 elapsed = scheduler_get_ticks() - before;
	if (ret != 0)
		ok = 0;
	if (elapsed < 10)
		ok = 0; /* returned early: it never really parked */

	/* A condition true on entry must return non-zero without sleeping. */
	volatile u32 ready = 1;
	u64 ret2 = 0;
	before = scheduler_get_ticks();
	wait_event_timeout_r(wq, ready != 0, 100ull, ret2);
	if (ret2 == 0)
		ok = 0;
	if (scheduler_get_ticks() - before > 2)
		ok = 0; /* slept when it should not have */

	m101_report("waitqueue-timeout", ok, elapsed);
}

/* ── ww_mutex ───────────────────────────────────────────────────── */

static struct ww_mutex g_ww_a;
static struct ww_mutex g_ww_b;

static void test_ww_basic(void)
{
	int ok = 1;
	struct ww_acquire_ctx ctx;

	ww_mutex_init(&g_ww_a);
	ww_mutex_init(&g_ww_b);
	ww_acquire_init(&ctx);

	if (ww_mutex_lock(&g_ww_a, &ctx) != 0)
		ok = 0;
	if (!ww_mutex_is_locked_by_current(&g_ww_a))
		ok = 0;

	/* The same lock twice under one context is the caller listing a buffer
	 * twice. It must be named, not deadlocked on. */
	if (ww_mutex_lock(&g_ww_a, &ctx) != -EALREADY)
		ok = 0;

	if (ww_mutex_lock(&g_ww_b, &ctx) != 0)
		ok = 0;
	if (ctx.acquired != 2)
		ok = 0;

	ww_mutex_unlock(&g_ww_b);
	ww_mutex_unlock(&g_ww_a);
	if (ctx.acquired != 0)
		ok = 0;
	/* Nothing wounded it, so it must not be carrying a back-off. */
	if (ctx.wounded != 0)
		ok = 0;
	ww_acquire_fini(&ctx);

	/* A lock taken with no context at all still excludes. */
	if (!ww_mutex_trylock(&g_ww_a, 0))
		ok = 0;
	if (ww_mutex_trylock(&g_ww_a, 0))
		ok = 0;
	ww_mutex_unlock(&g_ww_a);

	m101_report("ww-mutex-basic", ok, 0);
}

/*
 * Deterministic wound. The older context is handed to the thread, which takes B
 * and then blocks on A; the younger context is held by this task, which owns A.
 * That is a real cycle, and wound/wait must break it by refusing the younger
 * context's next acquire — not by letting both sleep.
 */
static struct ww_acquire_ctx g_ww_old_ctx;
static volatile int g_ww_thread_holds_b;
static volatile int g_ww_thread_done;
static volatile int g_ww_thread_ok;

static void ww_old_thread(void *arg)
{
	(void)arg;
	g_ww_thread_ok = 1;
	/* Older context takes B first ... */
	if (ww_mutex_lock(&g_ww_b, &g_ww_old_ctx) != 0)
		g_ww_thread_ok = 0;
	g_ww_thread_holds_b = 1;
	/* ... then asks for A, which the younger context holds. This wounds the
	 * younger one and parks here until it lets go. An older context must never
	 * be told to back off: that is the property being tested. */
	if (ww_mutex_lock(&g_ww_a, &g_ww_old_ctx) != 0)
		g_ww_thread_ok = 0;
	if (g_ww_old_ctx.wounded != 0)
		g_ww_thread_ok = 0;
	ww_mutex_unlock(&g_ww_a);
	ww_mutex_unlock(&g_ww_b);
	g_ww_thread_done = 1;
	scheduler_exit_current(0);
}

static void test_ww_wound(void)
{
	int ok = 1;
	struct ww_acquire_ctx young;

	ww_mutex_init(&g_ww_a);
	ww_mutex_init(&g_ww_b);
	g_ww_thread_holds_b = 0;
	g_ww_thread_done = 0;
	g_ww_thread_ok = 0;

	/* Order matters: the thread's context must be the older one, so it is
	 * stamped first. */
	ww_acquire_init(&g_ww_old_ctx);
	ww_acquire_init(&young);
	if (g_ww_old_ctx.stamp >= young.stamp)
		ok = 0;

	if (ww_mutex_lock(&g_ww_a, &young) != 0)
		ok = 0;

	u64 wounds_before = ww_mutex_wound_count();
	u64 backoffs_before = ww_mutex_backoff_count();

	if (kthread_create("lkpi-ww-old", ww_old_thread, 0) < 0) {
		ok = 0;
		ww_mutex_unlock(&g_ww_a);
		ww_acquire_fini(&young);
		m101_report("ww-mutex-wound", 0, 0);
		return;
	}

	/* Wait for the wound to actually be delivered rather than guessing with a
	 * sleep: the counter is the layer's own record that the path ran. */
	int waited = 0;
	while (ww_mutex_wound_count() == wounds_before && waited < 1000) {
		scheduler_sleep_ticks(1);
		waited++;
	}
	if (ww_mutex_wound_count() == wounds_before)
		ok = 0; /* the older context never wounded anyone */
	if (!g_ww_thread_holds_b)
		ok = 0;
	if (young.wounded == 0)
		ok = 0; /* the wound did not land on the younger context */

	/* Now the younger context asks for B. It is wounded, so this must be
	 * refused — sleeping here is the deadlock the whole class exists to
	 * prevent. */
	int r = ww_mutex_lock(&g_ww_b, &young);
	if (r != -EDEADLK)
		ok = 0;
	if (ww_mutex_backoff_count() <= backoffs_before)
		ok = 0;

	/* Back off properly: release everything, then re-acquire starting with the
	 * lock that refused us. */
	ww_mutex_unlock(&g_ww_a);
	ww_mutex_lock_slow(&g_ww_b, &young);
	if (young.wounded != 0)
		ok = 0; /* the slow path must clear the wound */
	if (ww_mutex_lock(&g_ww_a, &young) != 0)
		ok = 0;

	ww_mutex_unlock(&g_ww_a);
	ww_mutex_unlock(&g_ww_b);
	ww_acquire_fini(&young);

	for (int i = 0; i < 1000 && !g_ww_thread_done; i++)
		scheduler_sleep_ticks(1);
	if (!g_ww_thread_done || !g_ww_thread_ok)
		ok = 0;
	ww_acquire_fini(&g_ww_old_ctx);

	m101_report("ww-mutex-wound", ok, ww_mutex_backoff_count());
}

/*
 * Contended stress: two tasks take the same two locks in opposite orders, over
 * and over. Under wound/wait this terminates and the shared counter is exact;
 * under a plain mutex it deadlocks, and under a shim that ignores -EDEADLK it
 * loses updates.
 */
#define WW_STRESS_ROUNDS 60

static volatile u32 g_ww_counter;
static volatile int g_ww_stress_done;
static volatile int g_ww_stress_ok;

static int ww_hold_both(struct ww_mutex *first, struct ww_mutex *second,
                        struct ww_acquire_ctx *ctx)
{
	/* One stamp for the whole attempt, kept across back-offs: that is what
	 * makes a repeatedly-wounded context eventually the oldest, and therefore
	 * guarantees it finishes instead of livelocking. */
	ww_acquire_init(ctx);
	for (;;) {
		int r = ww_mutex_lock(first, ctx);
		if (r == -EDEADLK) {
			ww_mutex_lock_slow(first, ctx);
		} else if (r != 0) {
			return 0;
		}

		/* Hold the first lock across a yield before reaching for the second.
		 * Without this the two tasks pass through the window too quickly to
		 * overlap and the back-off path — the entire point of the class — never
		 * runs, leaving a test that only proves exclusion. */
		scheduler_yield();

		r = ww_mutex_lock(second, ctx);
		if (r == 0)
			return 1;
		if (r != -EDEADLK) {
			ww_mutex_unlock(first);
			return 0;
		}

		/* Back off: drop everything, then take the contended lock first. */
		ww_mutex_unlock(first);
		ww_mutex_lock_slow(second, ctx);
		r = ww_mutex_lock(first, ctx);
		if (r == 0) {
			/* Held in the other order; the caller only needs both. */
			return 1;
		}
		ww_mutex_unlock(second);
		if (r != -EDEADLK)
			return 0;
	}
}

static void ww_stress_body(struct ww_mutex *first, struct ww_mutex *second)
{
	for (int i = 0; i < WW_STRESS_ROUNDS; i++) {
		struct ww_acquire_ctx ctx;
		if (!ww_hold_both(first, second, &ctx)) {
			g_ww_stress_ok = 0;
			ww_acquire_fini(&ctx);
			return;
		}
		/* Non-atomic read-modify-write with a yield inside: any hole in the
		 * exclusion shows up as a lost update. */
		u32 v = g_ww_counter;
		scheduler_yield();
		g_ww_counter = v + 1;

		ww_mutex_unlock(second);
		ww_mutex_unlock(first);
		ww_acquire_fini(&ctx);
	}
}

static void ww_stress_thread(void *arg)
{
	(void)arg;
	/* Opposite order to the main task: the deadlock this is all about. */
	ww_stress_body(&g_ww_b, &g_ww_a);
	g_ww_stress_done = 1;
	scheduler_exit_current(0);
}

static void test_ww_stress(void)
{
	int ok = 1;
	ww_mutex_init(&g_ww_a);
	ww_mutex_init(&g_ww_b);
	g_ww_counter = 0;
	g_ww_stress_done = 0;
	g_ww_stress_ok = 1;

	u64 backoffs_before = ww_mutex_backoff_count();

	if (kthread_create("lkpi-ww-stress", ww_stress_thread, 0) < 0) {
		ok = 0;
	} else {
		ww_stress_body(&g_ww_a, &g_ww_b);
		for (int i = 0; i < 4000 && !g_ww_stress_done; i++)
			scheduler_sleep_ticks(1);
		if (!g_ww_stress_done)
			ok = 0; /* did not terminate: a deadlock survived */
		if (!g_ww_stress_ok)
			ok = 0;
		if (g_ww_counter != 2 * WW_STRESS_ROUNDS)
			ok = 0; /* lost updates: the exclusion has a hole */
	}

	/* How many back-offs the contention actually produced. Not asserted to be
	 * non-zero — the two tasks may happen never to overlap — but reported, so a
	 * run where the interesting path never ran is visible rather than silent. */
	m101_report("ww-mutex-stress", ok,
	            ww_mutex_backoff_count() - backoffs_before);
}

/* ── rbtree ─────────────────────────────────────────────────────── */

#define RB_TEST_NODES 1024

struct rb_test_node {
	struct rb_node node;
	u32 key;
};

static struct rb_test_node g_rb_nodes[RB_TEST_NODES];

static int rb_test_insert(struct rb_root *root, struct rb_test_node *item)
{
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = 0;
	while (*link) {
		parent = *link;
		struct rb_test_node *this = rb_entry(parent, struct rb_test_node, node);
		if (item->key < this->key)
			link = &parent->rb_left;
		else if (item->key > this->key)
			link = &parent->rb_right;
		else
			return 0; /* duplicate */
	}
	rb_link_node(&item->node, parent, link);
	rb_insert_color(&item->node, root);
	return 1;
}

static struct rb_test_node *rb_test_find(struct rb_root *root, u32 key)
{
	struct rb_node *n = root->rb_node;
	while (n) {
		struct rb_test_node *this = rb_entry(n, struct rb_test_node, node);
		if (key < this->key)
			n = n->rb_left;
		else if (key > this->key)
			n = n->rb_right;
		else
			return this;
	}
	return 0;
}

static int rb_test_depth(const struct rb_node *n)
{
	if (!n)
		return 0;
	int l = rb_test_depth(n->rb_left);
	int r = rb_test_depth(n->rb_right);
	return 1 + (l > r ? l : r);
}

static void test_rbtree(void)
{
	int ok = 1;
	struct rb_root root;
	rb_root_init(&root);

	if (!RB_EMPTY_ROOT(&root))
		ok = 0;
	if (rb_first(&root) || rb_last(&root))
		ok = 0;

	/* Strictly ascending keys: the input that degenerates an unbalanced tree
	 * into a list. Keys are spaced so erase can later look for absent ones. */
	for (int i = 0; i < RB_TEST_NODES; i++) {
		g_rb_nodes[i].key = (u32)(i * 2);
		if (!rb_test_insert(&root, &g_rb_nodes[i]))
			ok = 0;
	}
	if (rb_count(&root) != RB_TEST_NODES)
		ok = 0;

	int bh = rb_check(&root);
	if (bh < 0)
		ok = 0; /* invariants broken */

	/* Balance, checked as a bound rather than trusted: a red-black tree's
	 * height never exceeds 2*log2(n+1). For 1024 nodes that is 20; an
	 * unbalanced tree fed ascending keys would be 1024 deep. */
	int depth = rb_test_depth(root.rb_node);
	if (depth > 20)
		ok = 0;

	/* In-order traversal must produce the keys in order, all of them. */
	u32 expect = 0;
	usize seen = 0;
	for (struct rb_node *it = rb_first(&root); it; it = rb_next(it)) {
		struct rb_test_node *item = rb_entry(it, struct rb_test_node, node);
		if (item->key != expect)
			ok = 0;
		expect += 2;
		seen++;
	}
	if (seen != RB_TEST_NODES)
		ok = 0;

	/* And backwards, which exercises the other half of the threading. */
	expect = (RB_TEST_NODES - 1) * 2;
	seen = 0;
	for (struct rb_node *it = rb_last(&root); it; it = rb_prev(it)) {
		struct rb_test_node *item = rb_entry(it, struct rb_test_node, node);
		if (item->key != expect)
			ok = 0;
		expect -= 2;
		seen++;
	}
	if (seen != RB_TEST_NODES)
		ok = 0;

	/* Lookup hits every key that is there and no key that is not. */
	for (int i = 0; i < RB_TEST_NODES; i++) {
		if (rb_test_find(&root, (u32)(i * 2)) != &g_rb_nodes[i])
			ok = 0;
		if (rb_test_find(&root, (u32)(i * 2 + 1)))
			ok = 0;
	}

	/* Erase every other node — the case that exercises both the one-child and
	 * two-children paths — then re-verify the invariants. */
	for (int i = 0; i < RB_TEST_NODES; i += 2)
		rb_erase(&g_rb_nodes[i].node, &root);
	if (rb_count(&root) != RB_TEST_NODES / 2)
		ok = 0;
	if (rb_check(&root) < 0)
		ok = 0;
	for (int i = 0; i < RB_TEST_NODES; i++) {
		struct rb_test_node *found = rb_test_find(&root, (u32)(i * 2));
		if ((i % 2) == 0 && found)
			ok = 0; /* erased but still reachable */
		if ((i % 2) == 1 && found != &g_rb_nodes[i])
			ok = 0; /* survivor lost */
	}

	/* Drain it: the tree must end genuinely empty. */
	for (int i = 1; i < RB_TEST_NODES; i += 2)
		rb_erase(&g_rb_nodes[i].node, &root);
	if (!RB_EMPTY_ROOT(&root) || rb_count(&root) != 0)
		ok = 0;
	if (rb_check(&root) < 0)
		ok = 0;

	m101_report("rbtree", ok, (u64)depth);
}

/* ── rbtree churn: duplicates, interleaved erase and re-insert ──────
 *
 * The tree above is fed distinct ascending keys and then drained. That misses
 * the shape the DRM core actually builds: upstream's allocator keeps its free
 * ranges in a tree ordered by *size*, where dozens of holes share a size, and
 * it erases and re-inserts the same node repeatedly as ranges are split and
 * merged. A bug that only shows up on a duplicate key, or on re-inserting a
 * node that was erased earlier, survives the test above and then corrupts an
 * allocator two layers up — where it reads as a cycle between two unrelated
 * nodes, hours from its cause.
 *
 * So: heavy duplicates, insert and erase interleaved, and the invariants
 * checked after *every* operation rather than at the end. The cached leftmost
 * is verified against a fresh rb_first each time, because a stale one is what
 * makes an empty tree look non-empty to a caller's "does anything fit" guard.
 */

#define RB_CHURN_NODES 192
#define RB_CHURN_OPS   4096

static struct rb_test_node g_rb_churn[RB_CHURN_NODES];
static u8 g_rb_churn_in[RB_CHURN_NODES];

/* Ordered by key, descending, going right on equality — the same descent the
 * hole-size tree uses, so duplicates pile up on the right spine. */
static void rb_churn_insert(struct rb_root_cached *root,
                            struct rb_test_node *item)
{
	struct rb_node **link = &root->rb_root.rb_node;
	struct rb_node *parent = 0;
	int leftmost = 1;

	while (*link) {
		parent = *link;
		struct rb_test_node *this = rb_entry(parent, struct rb_test_node, node);
		if (item->key > this->key) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = 0;
		}
	}
	rb_link_node(&item->node, parent, link);
	rb_insert_color_cached(&item->node, root, leftmost);
}

static void test_rbtree_churn(void)
{
	int ok = 1;
	usize inserts = 0, erases = 0;
	struct rb_root_cached root = RB_ROOT_CACHED_INIT;
	usize live = 0;

	for (int i = 0; i < RB_CHURN_NODES; i++) {
		/* Seven distinct sizes across 192 nodes: every key has ~27 twins. */
		g_rb_churn[i].key = (u32)(4096 * (1 + (i % 7)));
		g_rb_churn_in[i] = 0;
	}

	/* A fixed sequence, not a random one: a failure has to be reproducible on
	 * the next boot to be worth reporting. */
	u32 lcg = 0x1234567u;
	for (usize op = 0; op < RB_CHURN_OPS && ok; op++) {
		lcg = lcg * 1103515245u + 12345u;
		usize idx = (lcg >> 8) % RB_CHURN_NODES;
		struct rb_test_node *item = &g_rb_churn[idx];

		if (g_rb_churn_in[idx]) {
			rb_erase_cached(&item->node, &root);
			g_rb_churn_in[idx] = 0;
			live--;
			erases++;
		} else {
			rb_churn_insert(&root, item);
			g_rb_churn_in[idx] = 1;
			live++;
			inserts++;
		}

		if (rb_check(&root.rb_root) < 0)
			ok = 0; /* colours, black height or parent pointers broken */
		if (rb_count(&root.rb_root) != live)
			ok = 0; /* the tree lost or duplicated a node */
		if (root.rb_leftmost != rb_first(&root.rb_root))
			ok = 0; /* a stale leftmost outlives the node it points at */
	}

	/* Drain what is left; the tree must end genuinely empty, with no cached
	 * pointer to a node that is no longer in it. */
	for (int i = 0; i < RB_CHURN_NODES; i++) {
		if (g_rb_churn_in[i]) {
			rb_erase_cached(&g_rb_churn[i].node, &root);
			g_rb_churn_in[i] = 0;
		}
	}
	if (!RB_EMPTY_ROOT(&root.rb_root) || root.rb_leftmost)
		ok = 0;

	m101_report("rbtree-churn", ok, (u64)(inserts + erases));
}

/* ── interval tree ──────────────────────────────────────────────── */

#define IT_NODES 256

static struct interval_tree_node g_it_nodes[IT_NODES];

/* Independent answer: count overlaps by looking at every range, which is what
 * the tree is supposed to agree with. */
static usize it_brute_count(u64 start, u64 last)
{
	usize n = 0;
	for (int i = 0; i < IT_NODES; i++) {
		if (g_it_nodes[i].start <= last && g_it_nodes[i].last >= start)
			n++;
	}
	return n;
}

static usize it_tree_count(struct interval_tree_root *root, u64 start, u64 last)
{
	usize n = 0;
	for (struct interval_tree_node *it =
	         interval_tree_iter_first(root, start, last);
	     it; it = interval_tree_iter_next(it, start, last)) {
		if (it->start > last || it->last < start)
			return (usize)-1; /* returned a range that does not overlap */
		n++;
	}
	return n;
}

static void test_interval_tree(void)
{
	int ok = 1;
	struct interval_tree_root root;
	interval_tree_init(&root);

	if (!interval_tree_empty(&root))
		ok = 0;
	if (interval_tree_iter_first(&root, 0, ~0ull))
		ok = 0;

	/* Overlapping ranges of several widths, deliberately not sorted by end:
	 * a long range early is what makes the subtree maximum matter. */
	for (int i = 0; i < IT_NODES; i++) {
		g_it_nodes[i].start = (u64)i * 10;
		u64 width = (i % 7 == 0) ? 500 : (u64)(i % 13) + 1;
		g_it_nodes[i].last = g_it_nodes[i].start + width;
		interval_tree_insert(&g_it_nodes[i], &root);
	}
	if (interval_tree_empty(&root))
		ok = 0;
	if (rb_check(&root.rb_root) < 0)
		ok = 0;
	/* Every node's cached maximum must equal one recomputed from its subtree.
	 * A rebalance that forgot to re-derive it fails here and nowhere else. */
	if (interval_tree_check(&root) != 0)
		ok = 0;

	/* Queries answered against a brute-force count over the same ranges. */
	usize mismatches = 0;
	for (u64 q = 0; q < IT_NODES * 10 + 100; q += 37) {
		if (it_tree_count(&root, q, q) != it_brute_count(q, q))
			mismatches++;
		if (it_tree_count(&root, q, q + 55) != it_brute_count(q, q + 55))
			mismatches++;
	}
	/* And the degenerate ends: everything, and nothing. */
	if (it_tree_count(&root, 0, ~0ull) != IT_NODES)
		mismatches++;
	if (it_tree_count(&root, ~0ull - 1, ~0ull) != 0)
		mismatches++;
	if (mismatches != 0)
		ok = 0;

	/* Remove half — the case where rebalancing moves subtrees around — then
	 * check the aggregates again and re-query. */
	for (int i = 0; i < IT_NODES; i += 2)
		interval_tree_remove(&g_it_nodes[i], &root);
	if (rb_check(&root.rb_root) < 0)
		ok = 0;
	if (interval_tree_check(&root) != 0)
		ok = 0;
	usize after = it_tree_count(&root, 0, ~0ull);
	if (after != IT_NODES / 2)
		ok = 0;

	for (int i = 1; i < IT_NODES; i += 2)
		interval_tree_remove(&g_it_nodes[i], &root);
	if (!interval_tree_empty(&root))
		ok = 0;

	m101_report("interval-tree", ok, mismatches);
}

/* ── xarray ─────────────────────────────────────────────────────── */

#define XA_TEST_ENTRIES 200

static u32 g_xa_values[XA_TEST_ENTRIES];

struct xa_walk_state {
	u64 prev;
	int first;
	usize seen;
	int ordered;
};

static int xa_walk_cb(u64 index, void *entry, void *data)
{
	struct xa_walk_state *st = data;
	if (!st->first && index <= st->prev)
		st->ordered = 0;
	st->first = 0;
	st->prev = index;
	st->seen++;
	(void)entry;
	return 0;
}

static void test_xarray(void)
{
	int ok = 1;
	struct xarray xa;
	xa_init(&xa);

	if (!xa_empty(&xa) || xa_count(&xa) != 0)
		ok = 0;
	if (xa_load(&xa, 0) || xa_load(&xa, 123456789ull))
		ok = 0;

	/* Indices spread far apart, which is the case a flat array cannot serve:
	 * a few small ones, then values needing every level of the tree. */
	for (int i = 0; i < XA_TEST_ENTRIES; i++) {
		g_xa_values[i] = 0x5A000000u + (u32)i;
		u64 index = (u64)i * 1000003ull; /* prime stride: no shared prefixes */
		if (xa_store(&xa, index, &g_xa_values[i]) != 0)
			ok = 0;
	}
	if (xa_count(&xa) != XA_TEST_ENTRIES || xa_empty(&xa))
		ok = 0;

	/* Every stored pointer comes back, verified through the value behind it. */
	for (int i = 0; i < XA_TEST_ENTRIES; i++) {
		u32 *p = xa_load(&xa, (u64)i * 1000003ull);
		if (p != &g_xa_values[i] || *p != 0x5A000000u + (u32)i)
			ok = 0;
		/* A neighbouring index was never stored and must miss. */
		if (xa_load(&xa, (u64)i * 1000003ull + 1))
			ok = 0;
	}

	/* Overwrite must replace, not add. */
	if (xa_store(&xa, 0, &g_xa_values[1]) != 0)
		ok = 0;
	if (xa_load(&xa, 0) != &g_xa_values[1])
		ok = 0;
	if (xa_count(&xa) != XA_TEST_ENTRIES)
		ok = 0;
	if (xa_store(&xa, 0, &g_xa_values[0]) != 0)
		ok = 0;

	/* The whole 64-bit range has to work, not just small indices. */
	static u32 top;
	if (xa_store(&xa, ~0ull, &top) != 0)
		ok = 0;
	if (xa_load(&xa, ~0ull) != &top)
		ok = 0;
	if (xa_erase(&xa, ~0ull) != &top)
		ok = 0;

	/* Iteration must be in ascending index order and hit everything once. */
	struct xa_walk_state st = { .prev = 0, .first = 1, .seen = 0, .ordered = 1 };
	xa_for_each(&xa, xa_walk_cb, &st);
	if (st.seen != XA_TEST_ENTRIES || !st.ordered)
		ok = 0;

	/* Erase returns what was there; erasing twice returns NULL. */
	for (int i = 0; i < XA_TEST_ENTRIES; i += 2) {
		if (xa_erase(&xa, (u64)i * 1000003ull) != &g_xa_values[i])
			ok = 0;
		if (xa_erase(&xa, (u64)i * 1000003ull))
			ok = 0;
	}
	if (xa_count(&xa) != XA_TEST_ENTRIES / 2)
		ok = 0;
	for (int i = 1; i < XA_TEST_ENTRIES; i += 2) {
		if (xa_load(&xa, (u64)i * 1000003ull) != &g_xa_values[i])
			ok = 0;
	}

	/* Storing NULL is an erase, as the header promises. */
	if (xa_store(&xa, 1000003ull, 0) != 0)
		ok = 0;
	if (xa_load(&xa, 1000003ull))
		ok = 0;

	/* Emptying it must make it genuinely empty again — the check that the
	 * tree's nodes were folded away rather than left as a skeleton. */
	for (int i = 1; i < XA_TEST_ENTRIES; i += 2)
		xa_erase(&xa, (u64)i * 1000003ull);
	if (!xa_empty(&xa) || xa_count(&xa) != 0)
		ok = 0;

	/* Reusable after being emptied. */
	if (xa_store(&xa, 42, &g_xa_values[0]) != 0)
		ok = 0;
	if (xa_load(&xa, 42) != &g_xa_values[0])
		ok = 0;
	xa_destroy(&xa);

	m101_report("xarray", ok, st.seen);
}

/* ── kthread_worker ─────────────────────────────────────────────── */

#define KW_ITEMS 16

struct kw_item {
	struct kthread_work work;
	u32 index;
};

static struct kw_item g_kw_items[KW_ITEMS];
static volatile u32 g_kw_order[KW_ITEMS];
static volatile u32 g_kw_ran;
static volatile u32 g_kw_slow_done;

static void kw_handler(struct kthread_work *work)
{
	struct kw_item *item = (struct kw_item *)work;
	u32 slot = g_kw_ran++;
	if (slot < KW_ITEMS)
		g_kw_order[slot] = item->index;
}

static struct kthread_work g_kw_slow;

static void kw_slow_handler(struct kthread_work *work)
{
	(void)work;
	/* Sleeps, which a handler is allowed to do — and which makes the flush
	 * below have something real to wait for. */
	scheduler_sleep_ticks(5);
	g_kw_slow_done = 1;
}

static void test_kthread_worker(void)
{
	int ok = 1;
	g_kw_ran = 0;
	g_kw_slow_done = 0;

	struct kthread_worker *worker = kthread_create_worker("lkpi-kw");
	if (!worker) {
		m101_report("kthread-worker", 0, 0);
		return;
	}

	for (int i = 0; i < KW_ITEMS; i++) {
		g_kw_items[i].index = (u32)i;
		kthread_init_work(&g_kw_items[i].work, kw_handler);
		if (!kthread_queue_work(worker, &g_kw_items[i].work))
			ok = 0;
	}
	/* Queuing something already pending must coalesce, not duplicate. */
	if (kthread_queue_work(worker, &g_kw_items[KW_ITEMS - 1].work))
		ok = 0;

	kthread_flush_worker(worker);
	if (g_kw_ran != KW_ITEMS)
		ok = 0;
	if (kthread_worker_executed(worker) != KW_ITEMS)
		ok = 0;
	/* Submission order, which is the guarantee a submission thread is chosen
	 * for in the first place. */
	for (u32 i = 0; i < KW_ITEMS; i++) {
		if (g_kw_order[i] != i)
			ok = 0;
	}

	/* A flush must actually wait for a sleeping handler, not just for it to
	 * have been dequeued. */
	kthread_init_work(&g_kw_slow, kw_slow_handler);
	kthread_queue_work(worker, &g_kw_slow);
	kthread_flush_work(&g_kw_slow);
	if (!g_kw_slow_done)
		ok = 0;
	if (g_kw_slow.seq != 1)
		ok = 0;

	/* An item may be requeued once it has run. */
	if (!kthread_queue_work(worker, &g_kw_slow))
		ok = 0;
	kthread_flush_work(&g_kw_slow);
	if (g_kw_slow.seq != 2)
		ok = 0;

	u64 executed = kthread_worker_executed(worker);
	kthread_destroy_worker(worker);
	if (executed != KW_ITEMS + 2)
		ok = 0;

	m101_report("kthread-worker", ok, executed);
}

/* ── RCU ────────────────────────────────────────────────────────── */

struct rcu_test_obj {
	struct rcu_head head;
	volatile u32 magic;
};

#define RCU_MAGIC  0x600DDA7Au
#define RCU_POISON 0xDEADBEEFu

static volatile u32 g_rcu_cb_ran;

static void rcu_test_cb(struct rcu_head *head)
{
	(void)head;
	g_rcu_cb_ran++;
}

static void test_rcu_basic(void)
{
	int ok = 1;

	/* Nesting: the section ends when the outermost unlock runs, not the
	 * innermost. Getting this wrong makes a reader unprotected halfway
	 * through, which is unobservable until it frees under someone. */
	if (rcu_read_lock_held())
		ok = 0;
	rcu_read_lock();
	if (!rcu_read_lock_held())
		ok = 0;
	rcu_read_lock();
	rcu_read_unlock();
	if (!rcu_read_lock_held())
		ok = 0; /* inner unlock ended the section: wrong */
	rcu_read_unlock();
	if (rcu_read_lock_held())
		ok = 0;

	/* With no readers a grace period must still be a real one, and must
	 * complete rather than being skipped. */
	u64 gps = rcu_grace_periods();
	synchronize_rcu();
	if (rcu_grace_periods() <= gps)
		ok = 0;

	/* Deferred callbacks run, exactly once each, and rcu_barrier means they
	 * have all run by the time it returns. */
	static struct rcu_test_obj objs[4];
	g_rcu_cb_ran = 0;
	u64 invoked = rcu_callbacks_invoked();
	for (int i = 0; i < 4; i++) {
		objs[i].magic = RCU_MAGIC;
		call_rcu(&objs[i].head, rcu_test_cb);
	}
	rcu_barrier();
	if (g_rcu_cb_ran != 4)
		ok = 0;
	if (rcu_callbacks_invoked() - invoked != 4)
		ok = 0;

	m101_report("rcu", ok, rcu_grace_periods());
}

/*
 * The property that matters: synchronize_rcu() must not return while a reader
 * that started before it is still inside its section.
 *
 * Proved the way the bug would show up. The reader keeps re-reading the object
 * for as long as it holds the section; the writer poisons that object the
 * instant synchronize_rcu() returns. A grace period that ends early therefore
 * poisons memory the reader is still walking, and the reader reports it — which
 * is exactly the use-after-free the primitive exists to prevent, made
 * observable instead of silent.
 *
 * Needs a second CPU to be meaningful: a reader holding a section does not
 * yield, so on one CPU the writer cannot even be running concurrently. When the
 * reader is never observed inside its section, that is reported as such rather
 * than counted as a pass.
 */
static struct rcu_test_obj g_rcu_shared;
static volatile u32 g_rcu_reader_entered;
static volatile u32 g_rcu_reader_left;
static volatile u32 g_rcu_reader_saw_poison;
static volatile u32 g_rcu_reader_done;
static volatile u32 g_rcu_seq;
static volatile u32 g_rcu_seq_left;
static volatile u32 g_rcu_seq_sync_returned;

static void rcu_reader_thread(void *arg)
{
	(void)arg;

	rcu_read_lock();
	g_rcu_reader_entered = 1;

	/* Bounded by iterations rather than ticks: the section runs with
	 * interrupts disabled on this CPU, so the tick counter is not advancing
	 * here and a tick deadline would never expire. The bound only has to
	 * outlast the writer noticing us and entering synchronize_rcu — a few
	 * scheduler ticks — after which synchronize_rcu is what waits for us. */
	for (u32 i = 0; i < 5000000u; i++) {
		if (g_rcu_shared.magic == RCU_POISON) {
			g_rcu_reader_saw_poison = 1;
			break;
		}
		__asm__ volatile("pause");
		tlb_shootdown_poll();
	}

	g_rcu_seq_left = ++g_rcu_seq;
	g_rcu_reader_left = 1;
	rcu_read_unlock();

	g_rcu_reader_done = 1;
	/* Return rather than scheduler_exit_current: a stealable worker is run by
	 * an AP's work-stealing trampoline, which unwinds it itself. Exiting from
	 * inside panics that AP with "dead task has nowhere to yield" — its parked
	 * loop is not a context a task can die in. */
}

void lkpi_rcu_smp_selftest(void)
{
	if (!bootinfo_has_flag("b1nix.test=1"))
		return;

	int ok = 1;
	g_rcu_shared.magic = RCU_MAGIC;
	g_rcu_reader_entered = 0;
	g_rcu_reader_left = 0;
	g_rcu_reader_saw_poison = 0;
	g_rcu_reader_done = 0;
	g_rcu_seq = 0;
	g_rcu_seq_left = 0;
	g_rcu_seq_sync_returned = 0;

	/*
	 * This needs two CPUs, and not merely as an optimisation. A reader holds
	 * its section with interrupts disabled, so if it lands on the CPU running
	 * this task it owns that CPU until it leaves — and this task then only
	 * resumes after the section is already over. On one CPU the writer can
	 * never be running while a reader is inside, so there is nothing to
	 * observe and nothing to prove.
	 */
	if (g_max_cpus < 2) {
		console_write("M101-SMOKE: rcu-grace-period needs-smp cpus=1"
		              " (a reader and a writer cannot overlap on one CPU)\n");
		return;
	}

	/*
	 * A stealable worker, run by an AP parked in the work-stealing loop. That
	 * is the one placement this kernel guarantees lands on another CPU, which
	 * is why this test runs from the early SMP self-test block rather than with
	 * the rest of M101.
	 *
	 * The alternatives were both tried and both failed to produce any overlap:
	 * a stealable worker created after the APs leave that loop is never picked
	 * up, and an ap_runnable kthread ran back on the BSP — where, holding a
	 * section with interrupts off, it finished before the writer resumed.
	 */
	if (sched_create_stealable_worker("lkpi-rcu-rd", rcu_reader_thread, 0) < 0) {
		m101_report("rcu-grace-period", 0, 0);
		return;
	}

	/*
	 * Spin rather than sleep while waiting for it to be picked up. Yielding
	 * here would let this CPU's own cooperative scheduler try to run the
	 * stealable worker, and only the AP path knows how to execute one — the
	 * same constraint the M24B work-stealing test spells out.
	 */
	int observed = 0;
	for (u64 i = 0; i < 200000000ull; i++) {
		if (g_rcu_reader_entered && !g_rcu_reader_left) {
			observed = 1;
			break;
		}
		if (g_rcu_reader_done)
			break;
		__asm__ volatile("pause");
		tlb_shootdown_poll();
	}

	if (!observed) {
		/* The reader was never caught inside its section, so nothing about
		 * ordering is checkable here — and poisoning now would only prove that
		 * a reader starting afterwards reads what it was given. Let it finish
		 * untouched and say plainly that the property was not exercised,
		 * rather than reporting a pass that was not earned.
		 *
		 * A previous version poisoned unconditionally and then blamed RCU for
		 * the reader seeing it: the reader had simply started later. The
		 * poison is only evidence when it lands while a reader is inside. */
		for (u64 i = 0; i < 200000000ull && !g_rcu_reader_done; i++) {
			__asm__ volatile("pause");
			tlb_shootdown_poll();
		}
		synchronize_rcu();
		console_write("M101-SMOKE: rcu-grace-period not-observed cpus=");
		console_write_dec((u64)g_max_cpus);
		console_write(" (reader never seen inside its section)\n");
		if (!g_rcu_reader_done)
			m101_report("rcu-grace-period", 0, (u64)g_max_cpus);
		return;
	}

	synchronize_rcu();
	g_rcu_seq_sync_returned = ++g_rcu_seq;

	/* The grace period claims every reader that started before it is gone.
	 * Poisoning right here is that claim being tested: if it was wrong, the
	 * reader is still walking this object and will see it change. */
	g_rcu_shared.magic = RCU_POISON;

	for (u64 i = 0; i < 200000000ull && !g_rcu_reader_done; i++) {
		__asm__ volatile("pause");
		tlb_shootdown_poll();
	}
	if (!g_rcu_reader_done)
		ok = 0;

	if (g_rcu_reader_saw_poison)
		ok = 0; /* the grace period ended while a reader was still inside */

	/* And the ordering directly: the reader left before synchronize returned. */
	if (g_rcu_seq_left == 0 || g_rcu_seq_left > g_rcu_seq_sync_returned)
		ok = 0;

	m101_report("rcu-grace-period", ok, (u64)g_max_cpus);
}

/* ── struct page / shmem / vmap ─────────────────────────────────── */

#define PAGE_TEST_COUNT 64

static void test_pages(void)
{
	int ok = 1;

	/* A single page, written through the direct map and read back through it. */
	struct page *p = lkpi_alloc_page();
	if (!p || !page_to_phys(p)) {
		m101_report("pages", 0, 0);
		return;
	}
	u32 *va = page_address(p);
	if (!va)
		ok = 0;
	else {
		va[0] = 0x5EEDBEEFu;
		va[1] = 0x0BADCAFEu;
		if (va[0] != 0x5EEDBEEFu || va[1] != 0x0BADCAFEu)
			ok = 0;
	}

	/* References: the frame stays until the last put. */
	get_page(p);
	if (put_page(p))
		ok = 0; /* freed while a reference was still held */
	if (!put_page(p))
		ok = 0; /* last put did not free */

	/* A contiguous run really is contiguous — the property alloc_pages sells. */
	struct page *run = alloc_pages(0, 2);
	if (!run) {
		ok = 0;
	} else {
		for (usize i = 0; i < 4; i++) {
			if (run[i].phys != run[0].phys + (u64)i * PAGE_SIZE)
				ok = 0;
		}
		__free_pages(run, 2);
	}

	/* A shmem backing must NOT be contiguous: a driver assuming page[i+1]
	 * follows page[i] has to break here, not on hardware with the IOMMU off. */
	struct page **pages = shmem_alloc_pages(PAGE_TEST_COUNT);
	if (!pages) {
		m101_report("pages", 0, 1);
		return;
	}
	usize adjacent = shmem_contiguous_runs(pages, PAGE_TEST_COUNT);
	if (adjacent >= PAGE_TEST_COUNT - 1)
		ok = 0; /* the whole array came out as one physical run */

	/* Distinct frames, every one of them. */
	for (usize i = 0; i < PAGE_TEST_COUNT; i++) {
		for (usize j = i + 1; j < PAGE_TEST_COUNT; j++) {
			if (pages[i]->phys == pages[j]->phys)
				ok = 0;
		}
	}

	/*
	 * vmap the scattered pages into one linear range, write a pattern through
	 * it, then read every page back through its own direct-map address — a
	 * different mapping of the same memory. A vmap that pointed somewhere else,
	 * or that mapped one page twice, fails here; checking through the vmap
	 * itself would not notice either.
	 */
	usize mapped_before = lkpi_vmap_pages_mapped();
	u32 *lin = lkpi_vmap(pages, PAGE_TEST_COUNT, LKPI_PROT_RW);
	if (!lin) {
		shmem_free_pages(pages, PAGE_TEST_COUNT);
		m101_report("pages", 0, 2);
		return;
	}
	if (lkpi_vmap_pages_mapped() != mapped_before + PAGE_TEST_COUNT)
		ok = 0;

	for (usize i = 0; i < PAGE_TEST_COUNT; i++)
		lin[i * (PAGE_SIZE / sizeof(u32))] = 0xA5000000u + (u32)i;

	for (usize i = 0; i < PAGE_TEST_COUNT; i++) {
		u32 *direct = page_address(pages[i]);
		if (!direct || direct[0] != 0xA5000000u + (u32)i)
			ok = 0;
	}

	/* And the other direction: a write through the direct map is visible
	 * through the vmap. */
	u32 *direct_last = page_address(pages[PAGE_TEST_COUNT - 1]);
	direct_last[1] = 0x1234ABCDu;
	if (lin[(PAGE_TEST_COUNT - 1) * (PAGE_SIZE / sizeof(u32)) + 1] != 0x1234ABCDu)
		ok = 0;

	lkpi_vunmap(lin);
	if (lkpi_vmap_pages_mapped() != mapped_before)
		ok = 0; /* the window did not come back */

	/* The window is reusable: the same slots must be handed out again. */
	u32 *again = lkpi_vmap(pages, PAGE_TEST_COUNT, LKPI_PROT_RW);
	if (!again)
		ok = 0;
	else {
		if (again[0] != 0xA5000000u)
			ok = 0; /* remapped somewhere else */
		lkpi_vunmap(again);
	}

	/* Write-combining, through the M98 PAT paths. The data still has to be
	 * coherent with the direct map after a flush. */
	u32 *wc = lkpi_vmap(pages, PAGE_TEST_COUNT, LKPI_PROT_WC);
	if (!wc) {
		ok = 0;
	} else {
		wc[0] = 0xC0FFEE11u;
		cache_flush_range(wc, PAGE_SIZE);
		u32 *direct0 = page_address(pages[0]);
		if (direct0[0] != 0xC0FFEE11u)
			ok = 0;
		lkpi_vunmap(wc);
	}

	shmem_free_pages(pages, PAGE_TEST_COUNT);
	m101_report("pages", ok, adjacent);
}


/* ── kobject / runtime PM ───────────────────────────────────────── */

static volatile u32 g_kobj_released_mask;

static void kobj_child_release(struct kobject *kobj)
{
	(void)kobj;
	/* Bit 0 records that the child released, and it must be set before the
	 * parent's bit: a release that walks up to its parent would otherwise be
	 * reading freed memory. */
	g_kobj_released_mask |= 1u;
}

static void kobj_parent_release(struct kobject *kobj)
{
	(void)kobj;
	g_kobj_released_mask |= 2u;
}

static volatile u32 g_pm_suspends;
static volatile u32 g_pm_resumes;
static volatile int g_pm_fail_resume;
static volatile int g_pm_fail_suspend;

static int pm_test_suspend(struct lkpi_device *dev)
{
	(void)dev;
	if (g_pm_fail_suspend)
		return -EIO;
	g_pm_suspends++;
	return 0;
}

static int pm_test_resume(struct lkpi_device *dev)
{
	(void)dev;
	if (g_pm_fail_resume)
		return -EIO;
	g_pm_resumes++;
	return 0;
}

static const struct lkpi_pm_ops g_pm_test_ops = {
	.runtime_suspend = pm_test_suspend,
	.runtime_resume = pm_test_resume,
};

static void test_device_pm(void)
{
	int ok = 1;
	static struct kobject parent;
	static struct kobject child;

	g_kobj_released_mask = 0;
	lkpi_kobject_init_and_add(&parent, "parent", 0, kobj_parent_release);
	lkpi_kobject_init_and_add(&child, "child", &parent, kobj_child_release);

	if (kobject_depth(&child) != 2 || kobject_depth(&parent) != 1)
		ok = 0;

	/* The creator's reference on the parent plus the child's makes two, so
	 * dropping the creator's must not release it. */
	if (kobject_put(&parent))
		ok = 0;
	if (g_kobj_released_mask != 0)
		ok = 0;

	/* Dropping the child releases the child and then, as its last act, the
	 * parent — in that order. */
	if (!kobject_put(&child))
		ok = 0;
	if (g_kobj_released_mask != 3u)
		ok = 0;

	/* Runtime PM. */
	static struct lkpi_device dev;
	g_pm_suspends = 0;
	g_pm_resumes = 0;
	g_pm_fail_resume = 0;
	g_pm_fail_suspend = 0;
	lkpi_device_init(&dev, "pmdev", &g_pm_test_ops);

	/* Starts resumed and PM-disabled: a put must not power anything down
	 * before the driver says it may. */
	if (lkpi_pm_runtime_suspended(&dev))
		ok = 0;
	lkpi_pm_runtime_get_sync(&dev);
	lkpi_pm_runtime_put_sync(&dev);
	if (g_pm_suspends != 0)
		ok = 0;

	lkpi_pm_runtime_enable(&dev);

	/* Nested holders: only the last put suspends. */
	if (lkpi_pm_runtime_get_sync(&dev) != 0)
		ok = 0;
	if (lkpi_pm_runtime_get_sync(&dev) != 0)
		ok = 0;
	if (lkpi_pm_runtime_usage(&dev) != 2)
		ok = 0;
	lkpi_pm_runtime_put_sync(&dev);
	if (g_pm_suspends != 0)
		ok = 0; /* suspended while a caller still held it */
	if (lkpi_pm_runtime_suspended(&dev))
		ok = 0;
	lkpi_pm_runtime_put_sync(&dev);
	if (g_pm_suspends != 1 || !lkpi_pm_runtime_suspended(&dev))
		ok = 0;

	/* And the next get resumes it again. */
	if (lkpi_pm_runtime_get_sync(&dev) != 0)
		ok = 0;
	if (g_pm_resumes != 1 || lkpi_pm_runtime_suspended(&dev))
		ok = 0;

	/* A driver that refuses to suspend has not suspended: the state must not
	 * claim otherwise, or the next get would skip the resume and hand back a
	 * device that was never powered up. */
	g_pm_fail_suspend = 1;
	lkpi_pm_runtime_put_sync(&dev);
	if (lkpi_pm_runtime_suspended(&dev))
		ok = 0;
	g_pm_fail_suspend = 0;

	/* A failed resume must not leave a usage reference behind. */
	lkpi_pm_runtime_get_sync(&dev);
	lkpi_pm_runtime_put_sync(&dev);        /* now suspended */
	if (!lkpi_pm_runtime_suspended(&dev))
		ok = 0;
	g_pm_fail_resume = 1;
	i32 usage_before = lkpi_pm_runtime_usage(&dev);
	if (lkpi_pm_runtime_get_sync(&dev) == 0)
		ok = 0; /* reported success with no hardware */
	if (lkpi_pm_runtime_usage(&dev) != usage_before)
		ok = 0; /* the failed get pinned the device anyway */
	g_pm_fail_resume = 0;

	m101_report("device-pm", ok, g_pm_suspends);
}

void lkpi_selftest_m101(void)
{
	if (!bootinfo_has_flag("b1nix.test=1"))
		return;

	test_kref();
	test_waitqueue();
	test_waitqueue_timeout();
	test_ww_basic();
	test_ww_wound();
	test_ww_stress();
	test_rbtree();
	test_rbtree_churn();
	test_interval_tree();
	test_xarray();
	test_kthread_worker();
	test_rcu_basic();
	test_pages();
	test_device_pm();
	console_write("M101-SMOKE: done\n");
}
