ENTRY(_start)

SECTIONS
{
	. = 0x40080000;
	__kernel_start = .;

	.text ALIGN(4K) : {
		*(.text .text.*)
	}

	.rodata ALIGN(4K) : {
		*(.rodata .rodata.*)
	}

	.data ALIGN(4K) : {
		*(.data .data.*)
	}

	.bss ALIGN(4K) : {
		*(COMMON)
		*(.bss .bss.*)
	}

	__kernel_end = .;
}
