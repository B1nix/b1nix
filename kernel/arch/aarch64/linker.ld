ENTRY(_start)

SECTIONS
{
	/* Set from the Makefile's KERNEL_BASE (--defsym), which defaults to QEMU
	 * virt's RAM base plus the arm64 Image header's text_offset. A board whose
	 * RAM starts elsewhere needs its own value — see the Makefile. */
	. = DEFINED(KERNEL_LOAD_BASE) ? KERNEL_LOAD_BASE : 0x40080000;
	__kernel_start = .;

	/* .text.boot first: the arm64 Image header must be the very first bytes
	 * of the raw image QEMU/u-boot loads. */
	.text ALIGN(4K) : {
		/* Bounds of kernel text. The linuxkpi lock validator checks a return
		 * address against these; x86_64's script has exported them all along. */
		__kernel_text_start = .;
		KEEP(*(.text.boot))
		*(.text .text.*)
		__kernel_text_end = .;
	}

	.rodata ALIGN(4K) : {
		*(.rodata .rodata.*)
	}

	/* M95 module loader: resolve_ksym walks __ksymtab_start..__ksymtab_end
	 * to bind a .ko's undefined symbols (mirrors the x86_64 script). */
	.ksymtab ALIGN(8) : {
		__ksymtab_start = .;
		KEEP(*(.ksymtab))
		__ksymtab_end = .;
		KEEP(*(.ksymtab_strings))
	}

	.data ALIGN(4K) : {
		*(.data .data.*)
	}

	/* M35: kallsyms symbol blob, generated post-link and appended on a second
	 * link pass. Placed after .text/.rodata/.data — only .bss shifts, so the
	 * pass-1 addresses it records stay valid (mirrors the x86_64 script). */
	.kallsyms ALIGN(8) : {
		__kallsyms_start = .;
		KEEP(*(.kallsyms))
		__kallsyms_end = .;
	}
	. = ALIGN(4K);

	.bss ALIGN(4K) : {
		__bss_start = .;
		*(COMMON)
		*(.bss .bss.*)
		__bss_end = .;
	}

	__kernel_end = .;
	_kernel_size = __kernel_end - __kernel_start;
}
