/*
 * M98 T2 on AArch64 — memory typing and cache maintenance.
 *
 * The x86_64 side of this (kernel/arch/x86_64/memtype.c) programs IA32_PAT and
 * evicts lines with CLFLUSH. Neither instruction exists here, but both *jobs*
 * do, spelled differently:
 *
 *   - memory type: an MAIR_EL1 slot selected by the descriptor's AttrIndx,
 *     programmed once in boot.S rather than per CPU (MAIR is not per-CPU state
 *     the way the PAT MSR is). Attr2 is Normal Non-cacheable, which is this
 *     architecture's write-combining: writes may be merged and reordered, which
 *     is the whole point for a framebuffer, while Device memory forbids exactly
 *     that.
 *   - line eviction: DC CIVAC by virtual address, and a set/way walk over every
 *     cache level for the "write back and invalidate everything" case.
 *
 * So the PAT-MSR readback has no counterpart here and is the one check the
 * harness skips on this arch; everything else in the group is real.
 */

#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/memtype.h>
#include <b1nix/mm.h>
#include <b1nix/types.h>

/* MAIR_EL1 is set up in boot.S before the MMU is enabled, and it is not
 * per-CPU state, so a secondary CPU has nothing to program. */
void pat_init_cpu(void) {}

int pat_available(void) { return 1; }

u32 cache_line_size(void)
{
	u64 ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	/* CTR_EL0.DminLine (bits 19:16): log2 of the smallest data cache line,
	 * counted in 4-byte words. */
	u32 words = 1u << ((u32)(ctr >> 16) & 0xf);
	return words * 4u;
}

/* DC CVAC/CIVAC are architectural on every AArch64 implementation — there is no
 * "this CPU cannot flush a line" case to fall back from. */
int cache_have_clflush(void) { return 1; }

/* Write back and invalidate every data cache level, the set/way way. This is
 * the direct counterpart of x86's WBINVD: architecturally defined, privileged,
 * and it names no address. CLIDR_EL1 says how many levels there are and which
 * of them have a data cache; CCSIDR_EL1 gives each level's geometry. */
static void flush_dcache_all(void)
{
	u64 clidr;
	__asm__ volatile("mrs %0, clidr_el1" : "=r"(clidr));
	u32 loc = (u32)((clidr >> 24) & 0x7); /* level of coherence */

	for (u32 level = 0; level < loc; level++) {
		u32 ctype = (u32)((clidr >> (level * 3)) & 0x7);
		if (ctype < 2)
			continue; /* no data cache at this level */

		/* Select this level, then read back its geometry. The ISB is
		 * required: CCSIDR_EL1 reads the level CSSELR_EL1 selected. */
		__asm__ volatile("msr csselr_el1, %0\n\tisb"
		                 :
		                 : "r"((u64)(level << 1)));
		u64 ccsidr;
		__asm__ volatile("mrs %0, ccsidr_el1" : "=r"(ccsidr));

		u32 line_bits = (u32)(ccsidr & 0x7) + 4;          /* log2(line bytes) */
		u32 ways = (u32)((ccsidr >> 3) & 0x3ff) + 1;
		u32 sets = (u32)((ccsidr >> 13) & 0x7fff) + 1;

		/* Way index sits at the top of the register, so it has to be
		 * shifted by (32 - log2(ways)) rounded up. */
		u32 way_shift = 0;
		while ((1u << way_shift) < ways)
			way_shift++;
		way_shift = 32u - way_shift;

		for (u32 way = 0; way < ways; way++) {
			for (u32 set = 0; set < sets; set++) {
				u64 sw = ((u64)way << way_shift) |
				         ((u64)set << line_bits) | ((u64)level << 1);
				__asm__ volatile("dc cisw, %0" : : "r"(sw) : "memory");
			}
		}
	}
	__asm__ volatile("dsb sy\n\tisb" ::: "memory");
}

void cache_flush_range_ex(const void *addr, usize size, int force_wbinvd)
{
	if (!addr || size == 0)
		return;

	if (force_wbinvd) {
		flush_dcache_all();
		return;
	}

	u64 line = cache_line_size();
	u64 start = (u64)(usize)addr & ~(line - 1);
	u64 end = (u64)(usize)addr + size;

	__asm__ volatile("dsb sy" ::: "memory");
	for (u64 va = start; va < end; va += line)
		__asm__ volatile("dc civac, %0" : : "r"(va) : "memory");
	__asm__ volatile("dsb sy\n\tisb" ::: "memory");
}

void cache_flush_range(const void *addr, usize size)
{
	cache_flush_range_ex(addr, size, 0);
}

/* A page of kernel VA to hold the write-combining alias built below. Inside
 * L0[0] like every other kernel range on this arch, and clear of the heap
 * (64 GiB), the large-allocation arena (128 GiB) and the two vmap windows
 * (320/352 GiB). */
#define WC_TEST_VA 0x6400000000ULL

void memtype_selftest(void)
{
	if (!bootinfo_has_flag("b1nix.test=1"))
		return;

	u64 frame = pmm_alloc_frame();
	if (!frame) {
		console_write("M98-DRV-SMOKE: FAIL pat-wc no-frame\n");
		return;
	}

	/* The direct map on this arch is the identity map, so the write-back view
	 * of the frame is the physical address itself. */
	volatile u32 *wb = (volatile u32 *)(usize)(frame + vmm_direct_map_base());
	for (int i = 0; i < 16; i++)
		wb[i] = 0;
	cache_flush_range((const void *)(usize)wb, 64);

	vmm_map_page(WC_TEST_VA, frame,
	             VMM_WRITABLE | VMM_NO_EXECUTE | VMM_WC);
	volatile u32 *wc = (volatile u32 *)(usize)WC_TEST_VA;

	u64 pte = paging_leaf_pte(WC_TEST_VA);
	int idx_ok = paging_pte_is_wc(pte);

	/* Write through the non-cacheable alias, then read through the write-back
	 * identity map. Two mappings of one frame with mismatched cacheability is
	 * exactly the case ARM requires cache maintenance for: the WB view may hold
	 * a stale line that the NC store never invalidated, so invalidate it before
	 * reading. That maintenance is part of what this checks — a GPU buffer
	 * depends on the same sequence. */
	for (int i = 0; i < 16; i++)
		wc[i] = 0xC0FFEE00u + (u32)i;
	mem_sfence();
	cache_flush_range((const void *)(usize)wb, 64);

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

	/* Flush the range by address and re-read: a cache flush must not lose a
	 * completed store. */
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

	/* The whole-hierarchy path. Same contract as x86's wbinvd branch: what is
	 * observable is that the privileged sequence executes on this CPU and that
	 * a completed store survives writing every level back — not the cache state
	 * itself, which software cannot see. */
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

	/* The WC alias keeps the frame mapped, so it stays owned by that mapping
	 * and must not go back to the pmm. */
}
