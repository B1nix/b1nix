/*
 * SPDX-License-Identifier: MIT
 *
 * M101: proof that the imported DRM core actually runs.
 *
 * Linking is not running. These checks call into upstream's own code — the GPU
 * address allocator, the rectangle maths, the format table — and compare what
 * it returns against values worked out independently here. That is the
 * difference between "the symbols resolved" and "the shim underneath behaves
 * the way the code expects".
 *
 * Each check is chosen because it exercises a *different* part of the shim:
 *
 *   - drm_mm runs on the rbtree and its augmentation. A tree whose cached
 *     subtree maxima went stale under rotation would still allocate, and would
 *     hand out overlapping ranges — which is why the check is "no two
 *     allocations overlap", verified against every earlier one, rather than
 *     "the calls returned 0".
 *   - drm_rect is pure arithmetic over our types, so it catches a u64/s64 or
 *     signedness mismatch that compiles cleanly.
 *   - drm_format_info reads a large static table through our const handling
 *     and container_of.
 */

/* No b1nix headers here: this file includes the drm and linux shims, and a
 * translation unit that sees both worlds fails on names they share — see
 * <lkpi/env.h>. */
#include <drm/drm_fourcc.h>
#include <drm/drm_mm.h>
#include <drm/drm_rect.h>
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <lkpi/env.h>

static void import_report(const char *name, int ok, u64 detail)
{
	/* One call, one line: a marker assembled from several writes can be
	 * interleaved by another CPU, which has eaten test markers before. */
	lkpi_printk("%s%s detail=%lu\n",
	            ok ? "M101-IMPORT: ok " : "M101-IMPORT: FAIL ", name,
	            (unsigned long)detail);
}

/* ── drm_mm: upstream's GPU address allocator ───────────────────── */

#define MM_NODES 64
#define MM_BASE  0x100000ull
#define MM_SIZE  (16ull * 1024 * 1024)

static struct drm_mm g_mm;
static struct drm_mm_node g_nodes[MM_NODES];

static void test_drm_mm(void)
{
	int ok = 1;
	usize allocated = 0;

	drm_mm_init(&g_mm, MM_BASE, MM_SIZE);

	/* Sizes that are not all equal, so the allocator has to actually search
	 * rather than hand out a stride. */
	for (usize i = 0; i < MM_NODES; i++) {
		u64 size = 4096ull * (1 + (i % 7));
		if (drm_mm_insert_node(&g_mm, &g_nodes[i], size) != 0)
			continue;
		allocated++;
	}
	if (allocated < MM_NODES / 2)
		ok = 0; /* the range is large enough that most must fit */

	/*
	 * Every allocation must lie inside the range and overlap no other. This is
	 * the check that would catch a stale subtree maximum in our augmented
	 * rbtree: the allocator would still return success and still hand out
	 * addresses, just overlapping ones.
	 */
	usize overlaps = 0;
	for (usize i = 0; i < MM_NODES; i++) {
		if (!drm_mm_node_allocated(&g_nodes[i]))
			continue;
		u64 a0 = g_nodes[i].start;
		u64 a1 = a0 + g_nodes[i].size;
		if (a0 < MM_BASE || a1 > MM_BASE + MM_SIZE)
			ok = 0;

		for (usize j = i + 1; j < MM_NODES; j++) {
			if (!drm_mm_node_allocated(&g_nodes[j]))
				continue;
			u64 b0 = g_nodes[j].start;
			u64 b1 = b0 + g_nodes[j].size;
			if (a0 < b1 && b0 < a1)
				overlaps++;
		}
	}
	if (overlaps != 0)
		ok = 0;

	/* Freeing half and reallocating must reuse the holes — otherwise the
	 * allocator is leaking address space rather than managing it. */
	for (usize i = 0; i < MM_NODES; i += 2) {
		if (drm_mm_node_allocated(&g_nodes[i]))
			drm_mm_remove_node(&g_nodes[i]);
	}
	usize reused = 0;
	for (usize i = 0; i < MM_NODES; i += 2) {
		if (drm_mm_insert_node(&g_mm, &g_nodes[i], 4096) == 0)
			reused++;
	}
	if (reused == 0)
		ok = 0;

	for (usize i = 0; i < MM_NODES; i++) {
		if (drm_mm_node_allocated(&g_nodes[i]))
			drm_mm_remove_node(&g_nodes[i]);
	}
	/* An emptied allocator must report itself clean; a leftover node here
	 * means remove_node did not unlink from the tree. */
	if (!drm_mm_clean(&g_mm))
		ok = 0;
	drm_mm_takedown(&g_mm);

	import_report("drm-mm", ok, allocated);
}

/* ── drm_rect: upstream's rectangle maths ───────────────────────── */

static void test_drm_rect(void)
{
	int ok = 1;

	struct drm_rect a = { .x1 = 10, .y1 = 20, .x2 = 110, .y2 = 220 };
	if (drm_rect_width(&a) != 100 || drm_rect_height(&a) != 200)
		ok = 0;

	/* A partial overlap: the answer is worked out here rather than taken from
	 * the code being tested. */
	struct drm_rect b = { .x1 = 60, .y1 = 20, .x2 = 200, .y2 = 100 };
	struct drm_rect clip = a;
	if (!drm_rect_intersect(&clip, &b))
		ok = 0;
	if (clip.x1 != 60 || clip.y1 != 20 || clip.x2 != 110 || clip.y2 != 100)
		ok = 0;

	/* Disjoint rectangles must report no intersection, not an empty one that
	 * a caller would then treat as a valid region. */
	struct drm_rect far = { .x1 = 1000, .y1 = 1000, .x2 = 1100, .y2 = 1100 };
	struct drm_rect none = a;
	if (drm_rect_intersect(&none, &far))
		ok = 0;

	/* Scaling is fixed-point: the source is in 16.16, so a 100-wide
	 * destination from a 200-wide source is a factor of two, expressed as
	 * 2 << 16. */
	struct drm_rect src = { .x1 = 0, .y1 = 0, .x2 = 200 << 16, .y2 = 100 << 16 };
	struct drm_rect dst = { .x1 = 0, .y1 = 0, .x2 = 100, .y2 = 50 };
	int hscale = drm_rect_calc_hscale(&src, &dst, 0, 1 << 20);
	if (hscale != (2 << 16))
		ok = 0;

	import_report("drm-rect", ok, (u64)hscale);
}

/* ── drm_fourcc: upstream's format table ────────────────────────── */

static void test_drm_fourcc(void)
{
	int ok = 1;

	/* Values from the format definitions themselves, not from the lookup. */
	const struct drm_format_info *xrgb = drm_format_info(DRM_FORMAT_XRGB8888);
	if (!xrgb || xrgb->num_planes != 1 || xrgb->cpp[0] != 4)
		ok = 0;

	const struct drm_format_info *rgb565 = drm_format_info(DRM_FORMAT_RGB565);
	if (!rgb565 || rgb565->num_planes != 1 || rgb565->cpp[0] != 2)
		ok = 0;

	/* A planar format, where the subsampling is the part a wrong table entry
	 * would get wrong: NV12 is two planes, chroma halved in both directions. */
	const struct drm_format_info *nv12 = drm_format_info(DRM_FORMAT_NV12);
	if (!nv12 || nv12->num_planes != 2 || nv12->hsub != 2 || nv12->vsub != 2)
		ok = 0;

	/* An unknown code must report absence rather than a default entry, or a
	 * driver would accept a format it cannot produce. */
	if (drm_format_info(0xDEADBEEF))
		ok = 0;

	import_report("drm-fourcc", ok, xrgb ? xrgb->cpp[0] : 0);
}

void drm_import_selftest(void)
{
	if (!lkpi_test_mode())
		return;

	test_drm_mm();
	test_drm_rect();
	test_drm_fourcc();
	lkpi_printk("M101-IMPORT: done\n");
}
