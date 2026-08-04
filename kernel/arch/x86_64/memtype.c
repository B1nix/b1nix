/*
 * M98 T2 — IA32_PAT programming, write-combining mappings and cache
 * maintenance. See kernel/include/b1nix/memtype.h for the slot layout and why
 * rewriting only PA5 is safe against live mappings.
 */

#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/klog.h>
#include <b1nix/memtype.h>
#include <b1nix/mm.h>
#include <string.h>

static int g_pat_ready;
static u32 g_clflush_size;
static int g_have_clflush;

static void cpuid_leaf(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
	u32 ra, rb, rc, rd;
	__asm__ volatile("cpuid"
	                 : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
	                 : "a"(leaf), "c"(0));
	if (a) *a = ra;
	if (b) *b = rb;
	if (c) *c = rc;
	if (d) *d = rd;
}

void pat_init_cpu(void)
{
	u32 eax = 0, ebx = 0, ecx = 0, edx = 0;
	cpuid_leaf(1, &eax, &ebx, &ecx, &edx);

	/* CPUID.01H:EDX[19] CLFSH, [16] PAT. */
	g_have_clflush = (edx & (1u << 19)) != 0;
	g_clflush_size = g_have_clflush ? (((ebx >> 8) & 0xff) * 8) : 0;
	if (g_clflush_size == 0)
		g_clflush_size = 64;

	if ((edx & (1u << 16)) == 0) {
		/* No PAT: slot 5 keeps its architectural meaning (write-through), so
		 * VMM_WC degrades to WT. Correct, just not combining. */
		return;
	}

	wrmsr(IA32_MSR_PAT, B1NIX_PAT_VALUE);
	g_pat_ready = 1;
}

int pat_available(void)
{
	return g_pat_ready;
}

u32 cache_line_size(void)
{
	return g_clflush_size ? g_clflush_size : 64;
}

void cache_flush_range_ex(const void *addr, usize size, int force_wbinvd)
{
	if (!addr || size == 0)
		return;
	if (force_wbinvd || !g_have_clflush) {
		/* No CLFLUSH (or a caller that wants the whole hierarchy back in
		 * memory regardless): the only architectural alternative is the big
		 * hammer. */
		mem_wbinvd();
		return;
	}
	u32 line = cache_line_size();
	u64 start = (u64)(usize)addr & ~(u64)(line - 1);
	u64 end = (u64)(usize)addr + size;
	mem_mfence();
	for (u64 p = start; p < end; p += line)
		mem_clflush((const void *)(usize)p);
	mem_mfence();
}

void cache_flush_range(const void *addr, usize size)
{
	cache_flush_range_ex(addr, size, 0);
}

int cache_have_clflush(void)
{
	return g_have_clflush;
}

/* ── M98 self-test ──────────────────────────────────────────────────
 *
 * Every check is verified against state read back from the hardware or from
 * the page tables, never against the value this code just passed in:
 *
 *   pat-msr  — read IA32_PAT back after the write and decode slot 5.
 *   pat-wc   — map a freshly allocated frame WC into the MMIO window, read the
 *              *installed* PTE out of the page tables and confirm the memory
 *              type bits select slot 5, then write a pattern through the WC
 *              alias and read it back through the unrelated write-back direct
 *              map. The two views must agree, which is the property every GPU
 *              buffer depends on.
 *   clflush  — flush that range and re-read; the data must survive (a cache
 *              flush must not lose a completed store).
 */
void memtype_selftest(void)
{
	if (!bootinfo_has_flag("b1nix.test=1"))
		return;

	u64 pat = rdmsr(IA32_MSR_PAT);
	u8 slot5 = (u8)((pat >> 40) & 0xff);
	if (pat_available() && slot5 == 0x01 && pat == B1NIX_PAT_VALUE) {
		console_write("M98-DRV-SMOKE: ok pat-msr\n");
	} else {
		console_write("M98-DRV-SMOKE: FAIL pat-msr pat=0x");
		console_write_hex64(pat);
		console_write("\n");
		return;
	}

	u64 frame = pmm_alloc_frame();
	if (!frame) {
		console_write("M98-DRV-SMOKE: FAIL pat-wc no-frame\n");
		return;
	}

	volatile u32 *wb = (volatile u32 *)(usize)(frame + vmm_direct_map_base());
	for (int i = 0; i < 16; i++)
		wb[i] = 0;

	volatile u32 *wc =
	    (volatile u32 *)vmm_map_mmio(frame, PAGE_SIZE,
	                                 VMM_WRITABLE | VMM_NO_EXECUTE | VMM_WC);
	if (!wc) {
		pmm_free_frame(frame);
		console_write("M98-DRV-SMOKE: FAIL pat-wc no-mapping\n");
		return;
	}

	u64 pte = paging_leaf_pte((u64)(usize)wc);
	u64 type_bits = pte & (VMM_PAT | VMM_PCD | VMM_PWT);
	int idx_ok = (pte & VMM_PRESENT) && type_bits == (VMM_PAT | VMM_PWT);

	/* Write through the WC alias, then read through the write-back direct map.
	 * WC stores sit in the fill buffers until fenced, so the fence is part of
	 * what is under test. */
	for (int i = 0; i < 16; i++)
		wc[i] = 0xC0FFEE00u + (u32)i;
	mem_sfence();

	int data_ok = 1;
	for (int i = 0; i < 16; i++)
		if (wb[i] != 0xC0FFEE00u + (u32)i)
			data_ok = 0;

	if (idx_ok && data_ok) {
		console_write("M98-DRV-SMOKE: ok pat-wc\n");
	} else {
		console_write("M98-DRV-SMOKE: FAIL pat-wc pte=0x");
		console_write_hex64(pte);
		console_write(" data=");
		console_write_dec((u64)data_ok);
		console_write("\n");
	}

	/* clflush the range through the write-back alias and re-read it. */
	cache_flush_range((const void *)(usize)wb, 64);
	int survived = 1;
	for (int i = 0; i < 16; i++)
		if (wb[i] != 0xC0FFEE00u + (u32)i)
			survived = 0;
	if (survived && cache_line_size() >= 8) {
		console_write("M98-DRV-SMOKE: ok clflush\n");
	} else {
		console_write("M98-DRV-SMOKE: FAIL clflush line=");
		console_write_dec((u64)cache_line_size());
		console_write("\n");
	}

	/* The wbinvd fallback. It is the path a CPU without CLFLUSH would take,
	 * and every CPU QEMU emulates reports CLFLUSH — so instead of leaving the
	 * branch unreachable, take it deliberately. What is observable is that the
	 * instruction executes on this CPU (it faults or #UDs if it does not, and
	 * it is privileged, so a bug in the calling context shows up here) and
	 * that a completed store survives writing the whole hierarchy back. What
	 * is NOT observable from software is the cache state itself; that is why
	 * this marker says the path runs, not that lines were evicted. */
	cache_flush_range_ex((const void *)(usize)wb, 64, 1);
	int wb_survived = 1;
	for (int i = 0; i < 16; i++)
		if (wb[i] != 0xC0FFEE00u + (u32)i)
			wb_survived = 0;
	if (wb_survived) {
		console_write("M98-DRV-SMOKE: ok wbinvd-fallback clflush=");
		console_write_dec((u64)cache_have_clflush());
		console_write("\n");
	} else {
		console_write("M98-DRV-SMOKE: FAIL wbinvd-fallback (data lost)\n");
	}

	/* The MMIO window is never reclaimed, so the frame stays owned by the
	 * alias — do not return it to the pmm. */
}
