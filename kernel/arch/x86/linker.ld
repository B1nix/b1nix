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

	.bss ALIGN(4K) : {
		*(COMMON)
		*(.bss .bss.*)
	}

	__kernel_end = .;
}
