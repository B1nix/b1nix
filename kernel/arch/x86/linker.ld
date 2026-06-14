ENTRY(_start)

/* Higher-half kernel. The 32-bit kernel is LINKED into the direct map
 * (KERNEL_VMA = DIRECT_MAP_BASE = 0x80000000) but LOADED at physical 1M, so its
 * symbols are simply the direct-map view of its own physical image and never
 * overlap the low range userspace uses (base 0x2000000). Before this, the
 * kernel was identity-mapped low and once it grew past 32M (e.g. with the Mesa
 * initramfs) it collided with userspace. See project_m52_kernel_userspace_va_overlap. */
KERNEL_VMA = 0x80000000;

SECTIONS
{
	/* .boot runs at LOW physical (identity) — GRUB hands off in 32-bit
	 * protected mode with paging off, so the entry + early page-table setup
	 * must be reachable at its physical load address. */
	. = 1M;
	__kernel_phys_start = .;

	.boot ALIGN(8) : {
		KEEP(*(.multiboot2))
		*(.boot)
	}

	/* Everything else is linked HIGH (into the direct map) but LOADED low. */
	. += KERNEL_VMA;
	__kernel_start = .;

	.text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
		*(.text .text.*)
	}
	. = ALIGN(512K);

	.rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
		*(.rodata .rodata.*)
	}
	. = ALIGN(512K);

	.data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
		*(.data .data.*)
	}
	. = ALIGN(512K);

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
