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
	. = ALIGN(512K);

	.rodata ALIGN(4K) : {
		*(.rodata .rodata.*)
	}
	. = ALIGN(512K);

	.data ALIGN(4K) : {
		*(.data .data.*)
	}
	. = ALIGN(512K);

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
