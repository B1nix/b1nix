/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Page audit: every shared file mapping against the page cache.
 *
 * A MAP_SHARED mapping of a file page is only shared because every mapper's
 * PTE points at the frame the page cache holds for (inode, offset). Two faults
 * on the same page that each installed their own frame leave one process
 * writing into a frame nobody else sees: a compositor then reads a client's
 * buffer page that was never drawn into. That was the "torn" desktop of
 * 2026-09-15 (page_cache_fault_install now makes the loser adopt the winner).
 *
 * This walks every task's shared file mappings, page by page, and compares
 * the present PTE's frame with the cache's. Each mismatch is reported once per
 * run with enough to find it: task, mapping, file, offset, both frames. A page
 * that is mapped but not cached at all is counted too; it is the same defect
 * seen from the other side.
 *
 *   b1nix.pageaudit=<seconds>   run every <seconds> from a kernel thread
 *   /proc/b1nix-pageaudit       run once now (the summary is the file's text)
 *
 * The walk takes no task lock: a mapping list is only ever appended to or
 * retired (vma_retire keeps unlinked entries alive for walkers), and a task
 * that exits mid-walk leaves its slot's list intact until reaping. A frame
 * that changes under the walk is a real event, not a race in the audit.
 */
#include <b1nix/types.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/page_cache.h>
#include <b1nix/sched.h>
#include <b1nix/vfs.h>

#define AUDIT_MAX_REPORTS 32

struct audit_totals {
	u64 tasks, mappings, pages, mismatches, uncached;
};

static void report_mismatch(struct task *t, struct vm_area *v, u64 va,
                            u64 file_page, u64 pte_frame, u64 cache_frame)
{
	console_write("pageaudit: task 0x");
	console_write_hex64(t->id);
	console_write(" va 0x");
	console_write_hex64(va);
	console_write(" file ");
	console_write((v->node && v->node->name[0]) ? v->node->name : "?");
	console_write(" ino ");
	console_write_dec(v->node->inode->ino);
	console_write(" off 0x");
	console_write_hex64(file_page);
	console_write(": pte frame 0x");
	console_write_hex64(pte_frame);
	console_write(cache_frame ? " cache frame 0x" : " not in cache");
	if (cache_frame)
		console_write_hex64(cache_frame);
	console_write("\n");
}

static void audit_task(struct task *t, struct audit_totals *tot, u32 *reports)
{
	if (!t->pml4_phys)
		return;
	tot->tasks++;
	for (struct vm_area *v = t->vma_list; v; v = v->next) {
		if (!(v->flags & MAP_SHARED) || !v->node || !v->node->inode)
			continue;
		/* A device's pages (a DRM card's GEM objects) are the driver's,
		 * mapped by the DRM bridge and never in the page cache. */
		if (v->special || v->node->inode->type == VFS_DEVICE)
			continue;
		tot->mappings++;
		for (u64 va = v->start; va < v->end; va += PAGE_SIZE) {
			u64 frame = paging_user_frame(t->pml4_phys, va);

			if (!frame)
				continue;
			tot->pages++;
			u64 file_page = ((u64)v->offset + (va - v->start)) & ~(u64)(PAGE_SIZE - 1);
			struct page_cache_entry *e = page_cache_get_page(v->node->inode, file_page);
			u64 cache_frame = e ? e->frame : 0;

			if (e)
				page_cache_put_page(e);
			if (!e)
				tot->uncached++;
			else if (cache_frame != frame)
				tot->mismatches++;
			else
				continue;
			if (*reports < AUDIT_MAX_REPORTS) {
				(*reports)++;
				report_mismatch(t, v, va, file_page, frame, cache_frame);
			}
		}
	}
}

void page_audit_run(struct audit_totals *out)
{
	struct audit_totals tot = {0, 0, 0, 0, 0};
	u32 reports = 0;
	usize slots = scheduler_task_slots();

	for (usize i = 0; i < slots; i++) {
		struct task *t = scheduler_task_slot(i);

		if (!t || t->state == TASK_UNUSED || t->state == TASK_DEAD ||
		    t->state == TASK_REAPING)
			continue;
		audit_task(t, &tot, &reports);
	}
	console_write("pageaudit: ");
	console_write_dec(tot.tasks);
	console_write(" tasks, ");
	console_write_dec(tot.mappings);
	console_write(" shared file mappings, ");
	console_write_dec(tot.pages);
	console_write(" present pages: ");
	console_write_dec(tot.mismatches);
	console_write(" frame mismatches, ");
	console_write_dec(tot.uncached);
	console_write(" mapped but not cached\n");
	if (out)
		*out = tot;
}

/* The summary as text, for /proc/b1nix-pageaudit. Returns the length. */
usize page_audit_summary(char *buf, usize size)
{
	struct audit_totals tot;
	usize n = 0;

	page_audit_run(&tot);
	const struct { const char *name; u64 v; } f[] = {
		{ "tasks", tot.tasks }, { "mappings", tot.mappings },
		{ "pages", tot.pages }, { "mismatches", tot.mismatches },
		{ "uncached", tot.uncached },
	};
	for (u32 i = 0; i < sizeof(f) / sizeof(f[0]) && n + 40 < size; i++) {
		const char *s = f[i].name;

		while (*s && n < size - 1)
			buf[n++] = *s++;
		buf[n++] = ' ';
		char tmp[24];
		u32 k = 0;
		u64 v = f[i].v;

		do {
			tmp[k++] = (char)('0' + v % 10);
			v /= 10;
		} while (v);
		while (k && n < size - 1)
			buf[n++] = tmp[--k];
		buf[n++] = '\n';
	}
	buf[n] = 0;
	return n;
}

static void page_audit_thread(void *arg)
{
	u32 every = (u32)(usize)arg;

	for (;;) {
		scheduler_sleep_ticks((u64)every * sched_tick_hz());
		page_audit_run(0);
	}
}

void page_audit_init(void)
{
	u32 every = bootinfo_get_u32("b1nix.pageaudit", 0);

	if (!every)
		return;
	kthread_create("pageaudit", page_audit_thread, (void *)(usize)every);
	console_write("pageaudit: every ");
	console_write_dec(every);
	console_write(" s\n");
}
