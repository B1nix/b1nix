ENTRY(_start)

/* Higher-half kernel: the kernel is LINKED at the top -2GB (the canonical
 * -mcmodel=kernel window) but LOADED at physical 1M. This keeps the kernel's
 * virtual addresses (its code, data, klog_buf, page tables, ...) entirely out
 * of the low address range that userspace uses (base 0x2000000). Before this,
 * the kernel was identity-mapped low and a large userspace binary (e.g. the
 * 12MB Mesa demo) would map over the kernel's own data in the per-process page
 * tables, corrupting it. See docs / project_m52_kernel_userspace_va_overlap. */
KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
	/* .boot runs at LOW physical (identity) — the loader hands off in 32-bit
	 * protected mode with paging off, so the entry code, the multiboot header,
	 * and the early page-table setup must be reachable below 4GB. */
	. = 1M;
	__kernel_phys_start = .;

	__boot_start = .;
	.boot ALIGN(8) : {
		KEEP(*(.multiboot2))
		*(.boot)
	}
	__boot_end = .;

	/* Everything else is linked HIGH (VMA) but still LOADED low (LMA via AT). */
	. += KERNEL_VMA;
	__kernel_start = .;

	__kernel_text_start = .;
	.text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
		*(.text .text.*)
	}
	__kernel_text_end = .;

	/* No section padding here. Each of .text/.rodata/.data used to be padded to
	 * a 512K boundary so that adding a few hundred bytes of kernel code could
	 * not shift the layout — TinyCC was a non-PIE ET_EXEC linked at a fixed
	 * address and hung whenever it did. That compiler is gone: the in-guest
	 * compiler is b1cc, which ships as a position-independent ET_DYN loaded
	 * through musl's ld.so, and the kernel has been higher-half since M52, so
	 * its addresses cannot collide with a userspace image at all. Padding a
	 * layout to hide an address-space bug was treating the symptom; nothing
	 * depends on frozen section addresses now. (kallsyms' two-pass link does
	 * need .text addresses to be identical between the passes — that is why the
	 * blob is placed after .text/.rodata/.data below, which holds with or
	 * without padding.) */

	.rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
		*(.rodata .rodata.*)
	}

	.data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
		*(.data .data.*)
	}

	/* M95: EXPORT_SYMBOL table — one { u64 value; const char *name; } record
	 * per exported kernel symbol, plus the name strings. The module loader
	 * walks __ksymtab_start..__ksymtab_end to resolve a .ko's undefined
	 * symbols. Placed after .text/.rodata/.data so growing it never shifts a
	 * function address. */
	.ksymtab ALIGN(8) : AT(ADDR(.ksymtab) - KERNEL_VMA) {
		__ksymtab_start = .;
		KEEP(*(.ksymtab))
		__ksymtab_end = .;
		KEEP(*(.ksymtab_strings))
	}
	. = ALIGN(4K);

	/* M35: kallsyms symbol blob, generated post-link and appended on a second
	 * link pass. Placed AFTER .text/.rodata/.data so appending it on the second
	 * pass never shifts a function address the first pass already recorded. */
	.kallsyms ALIGN(8) : AT(ADDR(.kallsyms) - KERNEL_VMA) {
		__kallsyms_start = .;
		KEEP(*(.kallsyms))
		__kallsyms_end = .;
	}
	. = ALIGN(4K);

	.bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
		*(COMMON)
		*(.bss .bss.*)
	}

	__kernel_end = .;
}
