/*
 * SPDX-License-Identifier: MIT
 *
 * M100 in-kernel self-tests: dma-fence, the GPU scheduler, and scatter-gather
 * buffer-object backing.
 *
 * The properties under test are the ones the rest of the stack relies on and
 * that cannot be observed from userspace:
 *
 *   - a fence signals exactly once, wakes a *parked* waiter (not a spinner),
 *     runs its callbacks, and carries an error through to the waiter;
 *   - the scheduler runs each entity's jobs in submission order and
 *     round-robins between entities rather than draining one first;
 *   - a job's dependency is honoured before its run_job is entered;
 *   - a deliberately discontiguous page list maps into one linear kernel view
 *     with every page landing where the scatterlist says it does.
 */

#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/dma_fence.h>
#include <b1nix/errno.h>
#include <b1nix/gpu_scheduler.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <lkpi/scatterlist.h>
#include <string.h>

static void drm_report(const char *name, int ok, u64 detail)
{
	console_write(ok ? "M100-SMOKE: ok " : "M100-SMOKE: FAIL ");
	console_write(name);
	if (!ok) {
		console_write(" detail=");
		console_write_dec(detail);
	}
	console_write("\n");
}

/* ── dma-fence ──────────────────────────────────────────────────── */

static struct dma_fence g_fence;
static volatile u32 g_cb_hits;
static volatile u64 g_cb_seqno;
static volatile int g_signaller_done;

static void fence_cb(struct dma_fence *f, void *data)
{
	g_cb_hits++;
	g_cb_seqno = f->seqno;
	*(u32 *)data = 0xB0B0B0B0u;
}

static void fence_signal_thread(void *arg)
{
	(void)arg;
	/* Give the main thread time to park on the fence, so the wakeup path — not
	 * a fast-path "already signalled" check — is what is being tested. */
	scheduler_sleep_ticks(3);
	dma_fence_signal(&g_fence);
	g_signaller_done = 1;
	scheduler_exit_current(0);
}

static void test_dma_fence(void)
{
	int ok = 1;
	u32 cb_payload = 0;
	struct dma_fence_cb cb;

	u64 ctx_a = dma_fence_context_alloc(1);
	u64 ctx_b = dma_fence_context_alloc(1);
	if (ctx_a == ctx_b || ctx_a == 0)
		ok = 0;

	dma_fence_init(&g_fence, ctx_a, 42, "m100-test");
	g_cb_hits = 0;
	g_cb_seqno = 0;
	g_signaller_done = 0;

	if (dma_fence_is_signaled(&g_fence))
		ok = 0;
	if (dma_fence_add_callback(&g_fence, &cb, fence_cb, &cb_payload) != 0)
		ok = 0;
	/* Registering must not fire the callback early. */
	if (g_cb_hits != 0)
		ok = 0;
	/* A timeout on a fence nobody signals must expire. */
	if (dma_fence_wait_timeout(&g_fence, 2) != 0)
		ok = 0;

	if (kthread_create("m100-fence", fence_signal_thread, 0) < 0) {
		drm_report("fence-signal", 0, 1);
		return;
	}

	if (dma_fence_wait(&g_fence) != 0)
		ok = 0;
	if (!dma_fence_is_signaled(&g_fence))
		ok = 0;
	/* The callback ran exactly once, on the right fence, with our data. */
	if (g_cb_hits != 1 || g_cb_seqno != 42 || cb_payload != 0xB0B0B0B0u)
		ok = 0;
	/* Signalling twice is a driver bug and must be reported, not absorbed. */
	if (dma_fence_signal(&g_fence) != -EINVAL)
		ok = 0;
	/* A wait on an already-signalled fence returns immediately. */
	if (dma_fence_wait(&g_fence) != 0)
		ok = 0;
	/* A callback added after the fact runs immediately and says so. */
	struct dma_fence_cb late;
	u32 late_payload = 0;
	if (dma_fence_add_callback(&g_fence, &late, fence_cb, &late_payload) !=
	        -ENOENT ||
	    late_payload != 0xB0B0B0B0u)
		ok = 0;

	drm_report("fence-signal", ok, g_cb_hits);

	/* Error propagation. */
	int eok = 1;
	struct dma_fence errf;
	dma_fence_init(&errf, ctx_b, 1, "m100-err");
	if (dma_fence_signal_error(&errf, -EIO) != 0)
		eok = 0;
	if (dma_fence_wait(&errf) != -EIO)
		eok = 0;
	if (dma_fence_error(&errf) != -EIO)
		eok = 0;
	drm_report("fence-error", eok, 0);

	/* Refcounting: the release callback fires exactly when the last reference
	 * goes, not before. */
	int rok = 1;
	struct dma_fence rf;
	dma_fence_init(&rf, dma_fence_context_alloc(1), 1, "m100-ref");
	rf.release = 0;
	dma_fence_get(&rf);
	dma_fence_get(&rf);
	if (rf.refs != 3)
		rok = 0;
	dma_fence_put(&rf);
	dma_fence_put(&rf);
	if (rf.refs != 1)
		rok = 0;
	dma_fence_put(&rf);
	if (rf.refs != 0)
		rok = 0;
	drm_report("fence-refcount", rok, rf.refs);
}

/* ── GPU scheduler ──────────────────────────────────────────────── */

#define SCHED_JOBS_PER_ENTITY 4
#define SCHED_TOTAL (SCHED_JOBS_PER_ENTITY * 2)

struct test_job {
	struct drm_sched_job base;
	u32 tag; /* high nibble = entity, low = index */
};

static volatile u32 g_run_order[SCHED_TOTAL + 4];
static volatile u32 g_run_count;
static volatile u32 g_dep_observed;
static struct dma_fence g_gate;

static int test_run_job(struct drm_sched_job *job)
{
	struct test_job *tj = (struct test_job *)job;
	u32 slot = g_run_count;
	if (slot < SCHED_TOTAL + 4)
		g_run_order[slot] = tj->tag;
	g_run_count = slot + 1;
	/* The gate fence is the dependency of every job in the fairness test; by
	 * the time run_job is entered it must already be signalled. */
	if (!dma_fence_is_signaled(&g_gate))
		g_dep_observed = 0;
	return DRM_SCHED_RUN_DONE;
}

static const struct drm_sched_backend_ops test_ops = {
	.run_job = test_run_job,
	.free_job = 0,
};

static void test_scheduler(void)
{
	struct drm_gpu_scheduler sched;
	struct drm_sched_entity ent_a, ent_b;
	static struct test_job jobs_a[SCHED_JOBS_PER_ENTITY];
	static struct test_job jobs_b[SCHED_JOBS_PER_ENTITY];
	struct dma_fence *fences[SCHED_TOTAL];

	if (drm_sched_init(&sched, &test_ops, "m100-sched") < 0) {
		drm_report("sched-submit", 0, 1);
		return;
	}
	drm_sched_entity_init(&ent_a, &sched, "ent-a");
	drm_sched_entity_init(&ent_b, &sched, "ent-b");

	g_run_count = 0;
	g_dep_observed = 1;
	memset((void *)g_run_order, 0, sizeof(g_run_order));

	/* Gate every job on one unsignalled fence, so all sixteen are queued
	 * before any of them can run. Without the gate the first job could be
	 * picked before the rest were submitted, and the interleaving below would
	 * be a race rather than a property. */
	dma_fence_init(&g_gate, dma_fence_context_alloc(1), 1, "m100-gate");

	u32 n = 0;
	for (u32 i = 0; i < SCHED_JOBS_PER_ENTITY; i++) {
		jobs_a[i].tag = 0xA0u | i;
		drm_sched_job_init(&jobs_a[i].base, &ent_a);
		drm_sched_job_add_dependency(&jobs_a[i].base, &g_gate);
		fences[n++] = drm_sched_job_submit(&jobs_a[i].base);

		jobs_b[i].tag = 0xB0u | i;
		drm_sched_job_init(&jobs_b[i].base, &ent_b);
		drm_sched_job_add_dependency(&jobs_b[i].base, &g_gate);
		fences[n++] = drm_sched_job_submit(&jobs_b[i].base);
	}

	int ok = 1;
	for (u32 i = 0; i < n; i++)
		if (!fences[i])
			ok = 0;
	/* Nothing may have run yet: the gate is still closed. */
	scheduler_sleep_ticks(2);
	if (g_run_count != 0)
		ok = 0;

	dma_fence_signal(&g_gate);

	for (u32 i = 0; i < n && ok; i++) {
		if (dma_fence_wait_timeout(fences[i], 1000) <= 0)
			ok = 0;
	}
	if (g_run_count != SCHED_TOTAL)
		ok = 0;
	if (!g_dep_observed)
		ok = 0;

	drm_report("sched-submit", ok, g_run_count);

	/* Ordering: within an entity, submission order; between entities, strict
	 * alternation (round-robin). A single global FIFO would drain A then B and
	 * fail the alternation check. */
	int order_ok = ok;
	u32 seen_a = 0, seen_b = 0;
	for (u32 i = 0; i < SCHED_TOTAL && order_ok; i++) {
		u32 tag = g_run_order[i];
		if ((tag & 0xF0u) == 0xA0u) {
			if ((tag & 0x0Fu) != seen_a++)
				order_ok = 0;
		} else if ((tag & 0xF0u) == 0xB0u) {
			if ((tag & 0x0Fu) != seen_b++)
				order_ok = 0;
		} else {
			order_ok = 0;
		}
		if (i > 0 && (g_run_order[i] & 0xF0u) == (g_run_order[i - 1] & 0xF0u))
			order_ok = 0; /* two in a row from the same entity: not fair */
	}
	if (seen_a != SCHED_JOBS_PER_ENTITY || seen_b != SCHED_JOBS_PER_ENTITY)
		order_ok = 0;
	drm_report("sched-fairness", order_ok, g_run_order[0]);

	for (u32 i = 0; i < n; i++) {
		if (fences[i])
			dma_fence_put(fences[i]);
	}
	for (u32 i = 0; i < SCHED_JOBS_PER_ENTITY; i++) {
		dma_fence_put(&jobs_a[i].base.finished);
		dma_fence_put(&jobs_b[i].base.finished);
	}

	drm_sched_entity_fini(&ent_a);
	drm_sched_entity_fini(&ent_b);
	drm_sched_fini(&sched);
}

/* ── scatter-gather buffer-object backing ───────────────────────── */

/* Scratch kernel window used to give a deliberately discontiguous page list a
 * linear view, exactly as drm_bo_alloc does for a real buffer object. Sits a
 * terabyte above the DRM object windows. */
#define SG_TEST_VA 0xffffa11000000000ULL
#define SG_TEST_PAGES 8

static void test_gem_sg(void)
{
	int ok = 1;
	u64 pool[SG_TEST_PAGES * 2];
	u64 frames[SG_TEST_PAGES];

	/* Force discontiguity: take 2N frames, hand back every other one, then
	 * take N. The allocator refills from the holes it was just given, so the
	 * resulting list cannot be one run. */
	for (u32 i = 0; i < SG_TEST_PAGES * 2; i++) {
		pool[i] = pmm_alloc_frame();
		if (!pool[i])
			ok = 0;
	}
	if (!ok) {
		drm_report("gem-sg", 0, 1);
		return;
	}
	for (u32 i = 0; i < SG_TEST_PAGES * 2; i += 2)
		pmm_free_frame(pool[i]);
	for (u32 i = 0; i < SG_TEST_PAGES; i++) {
		frames[i] = pmm_alloc_frame();
		if (!frames[i])
			ok = 0;
	}

	u32 adjacent_pairs = 0;
	for (u32 i = 1; i < SG_TEST_PAGES; i++)
		if (frames[i] == frames[i - 1] + PAGE_SIZE)
			adjacent_pairs++;

	struct sg_table sgt;
	if (sg_alloc_table_from_pages(&sgt, frames, SG_TEST_PAGES) < 0) {
		drm_report("gem-sg", 0, 2);
		return;
	}

	/* Map the (probably discontiguous) list into one linear window. */
	for (u32 i = 0; i < SG_TEST_PAGES; i++)
		vmm_map_page(SG_TEST_VA + (u64)i * PAGE_SIZE, frames[i],
		             VMM_WRITABLE | VMM_NO_EXECUTE | VMM_PRESENT);

	/* Write a linear pattern through the linear view. */
	volatile u32 *linear = (volatile u32 *)(usize)SG_TEST_VA;
	usize words = (SG_TEST_PAGES * PAGE_SIZE) / sizeof(u32);
	for (usize i = 0; i < words; i++)
		linear[i] = 0x5A000000u ^ (u32)i;

	/* Read it back page by page through the direct map, using the physical
	 * address the scatterlist reports — the two must agree for every page, or
	 * the linear view is lying about where the data went. */
	for (u32 p = 0; p < SG_TEST_PAGES && ok; p++) {
		u64 phys = sg_phys_at(&sgt, (u64)p * PAGE_SIZE);
		if (phys != frames[p]) {
			ok = 0;
			break;
		}
		volatile u32 *page =
		    (volatile u32 *)(usize)(phys + vmm_direct_map_base());
		usize base_word = (usize)p * (PAGE_SIZE / sizeof(u32));
		for (usize w = 0; w < PAGE_SIZE / sizeof(u32); w += 64) {
			if (page[w] != (0x5A000000u ^ (u32)(base_word + w))) {
				ok = 0;
				break;
			}
		}
	}

	if (sgt.total_bytes != (u64)SG_TEST_PAGES * PAGE_SIZE)
		ok = 0;
	/* The entry count must match the runs actually present. */
	if (sgt.nents != SG_TEST_PAGES - adjacent_pairs)
		ok = 0;

	u32 reported_nents = sgt.nents;
	for (u32 i = 0; i < SG_TEST_PAGES; i++)
		vmm_unmap_page(SG_TEST_VA + (u64)i * PAGE_SIZE);
	sg_free_table(&sgt);
	for (u32 i = 0; i < SG_TEST_PAGES; i++)
		pmm_free_frame(frames[i]);
	for (u32 i = 1; i < SG_TEST_PAGES * 2; i += 2)
		pmm_free_frame(pool[i]);

	drm_report("gem-sg", ok, reported_nents);
	/* Report whether the forced fragmentation actually produced more than one
	 * run. It normally does; when the allocator hands back a single run anyway
	 * the mapping test above still ran, so this is information, not a verdict. */
	console_write(adjacent_pairs + 1 < SG_TEST_PAGES
	                  ? "M100-SMOKE: ok gem-sg-discontig\n"
	                  : "M100-SMOKE: skip gem-sg-discontig (allocator returned "
	                    "one run)\n");
}

void dma_fence_selftest(void)
{
	if (!bootinfo_has_flag("b1nix.test=1"))
		return;
	test_dma_fence();
}

void drm_sched_selftest(void)
{
	if (!bootinfo_has_flag("b1nix.test=1"))
		return;
	test_scheduler();
	test_gem_sg();
	console_write("M100-SMOKE: done\n");
}
