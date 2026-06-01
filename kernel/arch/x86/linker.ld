ENTRY(_start)

SECTIONS
{
	. = 1M;
	__kernel_start = .;

	.multiboot2 ALIGN(8) : {
		KEEP(*(.multiboot2))
	}

	.text ALIGN(4K) : {
		*(.text .text.*)
	}
	. = ALIGN(512K);  /* Pad .text to 512K boundary — stabilises layout for TCC */

	.rodata ALIGN(4K) : {
		*(.rodata .rodata.*)
	}
	. = ALIGN(512K);  /* Pad .rodata to 512K boundary — stabilises layout */

	.data ALIGN(4K) : {
		*(.data .data.*)
	}
	. = ALIGN(512K);  /* Pad .data to 512K boundary — stabilises layout */

	/* M35: kallsyms symbol blob, generated post-link and appended on a second
	 * link pass. Placed AFTER .text/.rodata/.data (whose addresses are frozen
	 * by the 512K padding above) so adding it never shifts a function address —
	 * the pass-1 addresses baked into the blob stay valid in the final image.
	 * The start/end labels are defined here so the FIRST pass (no blob yet)
	 * still links: the section is simply empty (start == end) until pass two. */
	.kallsyms ALIGN(8) : {
		__kallsyms_start = .;
		KEEP(*(.kallsyms))
		__kallsyms_end = .;
	}
	. = ALIGN(4K);

	.bss ALIGN(4K) : {
		*(COMMON)
		*(.bss .bss.*)
	}

	__kernel_end = .;
}
