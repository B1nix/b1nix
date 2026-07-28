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

	.boot ALIGN(8) : {
		KEEP(*(.multiboot2))
		*(.boot)
	}

	/* Everything else is linked HIGH (VMA) but still LOADED low (LMA via AT). */
	. += KERNEL_VMA;
	__kernel_start = .;

	__kernel_text_start = .;
	.text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
		*(.text .text.*)
	}
	__kernel_text_end = .;
	. = ALIGN(512K);  /* Pad .text to 512K boundary — stabilises layout for TCC */

	.rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
		*(.rodata .rodata.*)
	}
	. = ALIGN(512K);  /* Pad .rodata to 512K boundary — stabilises layout */

	.data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
		*(.data .data.*)
	}
	. = ALIGN(512K);  /* Pad .data to 512K boundary — stabilises layout */

	/* M35: kallsyms symbol blob, generated post-link and appended on a second
	 * link pass. Placed AFTER .text/.rodata/.data (whose addresses are frozen
	 * by the 512K padding above) so adding it never shifts a function address. */
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
