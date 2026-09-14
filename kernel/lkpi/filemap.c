/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: the page-cache operations, out of line.
 *
 * This is the first piece of the page cache the imported filesystems stand on.
 * What is here is the part that operates on a page or folio ALREADY in hand:
 * its lock, its dirty and uptodate state, and the small copy helpers. The
 * index — finding a folio by file offset, creating one, evicting it — is the
 * larger half and is not written yet; it belongs with the address_space.
 *
 * Two rules this file keeps, because the filesystems depend on them:
 *
 *   - The page lock serialises the CONTENTS; a reference keeps the page alive.
 *     They are separate, and both are needed by anyone reading the bytes.
 *   - `Uptodate` is set exactly once, after the last byte is in place. Setting
 *     it earlier hands a reader uninitialised memory, and nothing later
 *     un-sets it.
 *
 * The lock is the PG_locked bit, waited on through b1nix's wait channels keyed
 * on the page's own address — the same trick <linux/wait_bit.h> uses, and the
 * reason a page needs no wait queue of its own.
 */

/*
 * b1nix headers and lkpi ones only — never a linux/ header.
 *
 * This file includes <b1nix/sched.h> to park a waiter, and b1nix's `spinlock_t`
 * is a different type from the one <linux/spinlock.h> declares. A translation
 * unit that saw both would have to be rescued by include order. That is the
 * boundary <lkpi/env.h> describes, and it is why the page-state bits live in
 * <lkpi/page.h> rather than in <linux/mm.h>.
 */
#include <lkpi/env.h>
#include <lkpi/page.h>
#include <b1nix/arch.h>
#include <b1nix/sched.h>

struct folio;

/*
 * The folio batch, declared here rather than reached through <linux/mm.h> for
 * the boundary reason above. The layout must match the one there — it is the
 * same object, seen from the other side — so the two are checked against each
 * other by the assertion at the bottom of this file.
 */
#define LKPI_PAGEVEC_SIZE 15
struct folio_batch {
	unsigned char nr;
	unsigned char i;
	_Bool percpu_pvec_drained;
	struct folio *folios[LKPI_PAGEVEC_SIZE];
};

/* ── the page lock ──────────────────────────────────────────────── */

void lock_page(struct page *page)
{
	if (!page)
		return;
	for (;;) {
		if (!lkpi_page_test_set(page, PG_locked))
			return;
		if (!scheduler_can_block()) {
			/* Early boot or interrupt context. A caller that takes a page
			 * lock there is a bug, but spinning makes progress where
			 * parking cannot. */
			cpu_relax();
			tlb_shootdown_poll();
			continue;
		}
		/* Two-phase wait: publish on the channel, re-test, park only if the
		 * bit is still set — an unlock between the two is then already
		 * visible to the re-test rather than lost. */
		scheduler_wait_prepare(page);
		if (lkpi_page_test(page, PG_locked))
			scheduler_wait_commit();
		else
			scheduler_wait_cancel();
	}
}

int trylock_page(struct page *page)
{
	return page && !lkpi_page_test_set(page, PG_locked);
}

void unlock_page(struct page *page)
{
	if (!page)
		return;
	lkpi_page_clear(page, PG_locked);
	scheduler_wake_all(page);
}

void wait_on_page_locked(struct page *page)
{
	if (!page)
		return;
	while (lkpi_page_test(page, PG_locked)) {
		if (!scheduler_can_block()) {
			cpu_relax();
			tlb_shootdown_poll();
			continue;
		}
		scheduler_wait_prepare(page);
		if (lkpi_page_test(page, PG_locked))
			scheduler_wait_commit();
		else
			scheduler_wait_cancel();
	}
}

/*
 * The folio forms.
 *
 * `struct folio` is declared in <linux/mm.h>, which this file may not include —
 * so the folio is taken as an opaque pointer and turned into its page by the
 * cast the two types are built to allow (their layouts are asserted identical
 * there). That assert is what makes this cast safe rather than hopeful.
 */
static inline struct page *lkpi_folio_page(struct folio *folio)
{ return (struct page *)folio; }

void folio_lock(struct folio *folio) { lock_page(lkpi_folio_page(folio)); }
int folio_trylock(struct folio *folio) { return trylock_page(lkpi_folio_page(folio)); }
void folio_unlock(struct folio *folio) { unlock_page(lkpi_folio_page(folio)); }

/* The end of a read: up to date if it succeeded, and unlocked either way —
 * the uptodate bit goes on before the lock comes off, so a waiter woken by
 * the unlock sees the result. */
void folio_end_read(struct folio *folio, _Bool success)
{
	if (success)
		lkpi_page_test_set(lkpi_folio_page(folio), PG_uptodate);
	unlock_page(lkpi_folio_page(folio));
}
void folio_wait_locked(struct folio *folio)
{ wait_on_page_locked(lkpi_folio_page(folio)); }

/* ── writeback ──────────────────────────────────────────────────── */

void set_page_writeback(struct page *page)
{
	if (page)
		lkpi_page_set(page, PG_writeback);
}

void end_page_writeback(struct page *page)
{
	if (!page)
		return;
	lkpi_page_clear(page, PG_writeback);
	scheduler_wake_all(page);
}

void wait_on_page_writeback(struct page *page)
{
	if (!page)
		return;
	while (lkpi_page_test(page, PG_writeback)) {
		if (!scheduler_can_block()) {
			cpu_relax();
			tlb_shootdown_poll();
			continue;
		}
		scheduler_wait_prepare(page);
		if (lkpi_page_test(page, PG_writeback))
			scheduler_wait_commit();
		else
			scheduler_wait_cancel();
	}
}

void folio_start_writeback(struct folio *folio)
{ set_page_writeback(lkpi_folio_page(folio)); }
void folio_end_writeback(struct folio *folio)
{ end_page_writeback(lkpi_folio_page(folio)); }
void folio_wait_writeback(struct folio *folio)
{ wait_on_page_writeback(lkpi_folio_page(folio)); }

/* ── dirty and uptodate ─────────────────────────────────────────── */

/*
 * The mapping's own dirty_folio, when there is one.
 *
 * Implemented on the Linux side (kernel/lkpi/fs_filemap.c), because reaching
 * an address_space's operations needs <linux/fs.h>, which this file must not
 * see. Weak: a build without the filesystem import has no a_ops to dispatch
 * to, and then the flag alone is the whole state.
 */
__attribute__((weak)) int lkpi_dirty_folio_ops(struct folio *folio);

/*
 * Dirtying is not just a flag. A filesystem records the change when its
 * dirty_folio runs — btrfs's btree_dirty_folio is what puts a metadata block
 * on the list its writeback later walks — so a version that only set the bit
 * left every write invisible to the filesystem: the transaction wrote a new
 * superblock pointing at tree blocks that had never been written, and the
 * image failed `btrfs check` with "bad tree block ... have=0".
 */
/* The page lock, by the names <linux/mm.h> can reach without pulling in the
 * page-cache header. */
void lkpi_page_lock(struct page *page) { lock_page(page); }
void lkpi_page_unlock(struct page *page) { unlock_page(page); }

void folio_mark_dirty(struct folio *folio)
{
	if (!folio)
		return;
	if (lkpi_dirty_folio_ops && lkpi_dirty_folio_ops(folio))
		return;
	lkpi_page_set(lkpi_folio_page(folio), PG_dirty);
}

void folio_mark_accessed(struct folio *folio)
{
	/*
	 * Upstream this promotes the folio on the LRU, which is how the second
	 * access to a page keeps it out of reclaim's way. b1nix has no page LRU,
	 * so the bit is recorded and nothing consults it yet — which is where a
	 * reclaim policy would start reading.
	 */
	if (folio)
		lkpi_page_set(lkpi_folio_page(folio), PG_referenced);
}

void folio_mark_uptodate(struct folio *folio)
{
	if (folio)
		lkpi_page_set(lkpi_folio_page(folio), PG_uptodate);
}

/*
 * Claim a dirty page for writeback: clear the dirty bit and report whether it
 * was set.
 *
 * The clear happens BEFORE the I/O is issued, and that order is the point — a
 * write that lands while the I/O is in flight re-dirties the page, so it is
 * written again. Clearing afterwards would lose that write.
 */
int clear_page_dirty_for_io(struct page *page)
{
	return page && lkpi_page_test_clear(page, PG_dirty);
}

int folio_clear_dirty_for_io(struct folio *folio)
{
	return folio && lkpi_page_test_clear(lkpi_folio_page(folio), PG_dirty);
}

/* ── batches ────────────────────────────────────────────────────── */

void __folio_batch_release(struct folio_batch *fbatch)
{
	unsigned char i;

	if (!fbatch)
		return;
	/* The batch holds a reference per entry; dropping them is what the
	 * release is FOR, and a version that only reset the count would leak one
	 * page per entry. */
	for (i = 0; i < fbatch->nr; i++)
		if (fbatch->folios[i])
			put_page(lkpi_folio_page(fbatch->folios[i]));
	fbatch->nr = 0;
	fbatch->i = 0;
}

/* ── small copies ───────────────────────────────────────────────── */

void memzero_page(struct page *page, size_t offset, size_t len)
{
	if (page)
		__builtin_memset((char *)page_address(page) + offset, 0, len);
}

void memcpy_to_page(struct page *page, size_t offset, const char *from,
                    size_t len)
{
	if (page)
		__builtin_memcpy((char *)page_address(page) + offset, from, len);
}

void memcpy_from_page(char *to, struct page *page, size_t offset, size_t len)
{
	if (page)
		__builtin_memcpy(to, (const char *)page_address(page) + offset, len);
}

void memcpy_page(struct page *dst_page, size_t dst_off, struct page *src_page,
                 size_t src_off, size_t len)
{
	if (dst_page && src_page)
		__builtin_memcpy((char *)page_address(dst_page) + dst_off,
		                 (const char *)page_address(src_page) + src_off, len);
}

void memcpy_from_folio(char *to, struct folio *folio, size_t offset, size_t len)
{ memcpy_from_page(to, lkpi_folio_page(folio), offset, len); }

void memcpy_to_folio(struct folio *folio, size_t offset, const char *from,
                     size_t len)
{ memcpy_to_page(lkpi_folio_page(folio), offset, from, len); }

void zero_user_segment(struct page *page, unsigned start, unsigned end)
{
	if (page && end > start)
		memzero_page(page, start, end - start);
}

void zero_user_segments(struct page *page, unsigned s1, unsigned e1,
                        unsigned s2, unsigned e2)
{
	zero_user_segment(page, s1, e1);
	zero_user_segment(page, s2, e2);
}

void folio_zero_range(struct folio *folio, size_t start, size_t length)
{ memzero_page(lkpi_folio_page(folio), start, length); }

void folio_zero_segment(struct folio *folio, size_t start, size_t end)
{ zero_user_segment(lkpi_folio_page(folio), (unsigned)start, (unsigned)end); }

void folio_zero_segments(struct folio *folio, size_t s1, size_t e1, size_t s2,
                         size_t e2)
{
	folio_zero_segment(folio, s1, e1);
	folio_zero_segment(folio, s2, e2);
}

/* ── the shared zero page ───────────────────────────────────────── */

/*
 * One page of zeros, allocated on first use and never freed.
 *
 * It is handed to the block layer for the write of a hole, so nothing may ever
 * write to it — which is why it is allocated here rather than pointing at some
 * page the caller happened to have.
 */
static struct page *zero_page;

struct page *lkpi_zero_page(void)
{
	if (!zero_page) {
		zero_page = lkpi_alloc_page();
		if (zero_page)
			__builtin_memset(page_address(zero_page), 0, PAGE_SIZE);
	}
	return zero_page;
}

/* ── the overflow ids ───────────────────────────────────────────── */

/*
 * The uid and gid reported when a real one does not fit a 16-bit field.
 *
 * They are variables rather than constants because they are tunable upstream
 * (/proc/sys/kernel/overflowuid), and because <linux/highuid.h> takes their
 * address in the conversion macros. b1nix never truncates a uid of its own —
 * nothing here creates an id above 65535 — so nothing produces them; ext4 needs
 * them for an inode written by a kernel that did.
 */
int overflowuid = 65534;
int overflowgid = 65534;
int fs_overflowuid = 65534;
int fs_overflowgid = 65534;
