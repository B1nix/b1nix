/* B1NIX Userspace Linker Script
 *
 * Produces position-independent ELF executables for x86_64-b1nix.
 * Entry point: _start (from crt0.o)
 * Base address: 0x400000 (standard Linux-compatible)
 */

ENTRY(_start)

SECTIONS
{
	. = 0x2000000;

	.text ALIGN(4K) : {
		*(.text .text.*)
	}

	.rodata ALIGN(4K) : {
		*(.rodata .rodata.*)
	}

	.data ALIGN(4K) : {
		*(.data .data.*)
	}

	/* ELF constructor array. crt0 walks [__init_array_start, __init_array_end)
	 * before main(). Empty for plain-C b1nix binaries, but the boundary
	 * symbols must be defined so crt0 links. */
	.init_array ALIGN(8) : {
		__init_array_start = .;
		KEEP(*(.preinit_array))
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array .ctors))
		__init_array_end = .;
	}

	.bss ALIGN(4K) : {
		*(COMMON)
		*(.bss .bss.*)
	}

	/DISCARD/ : {
		*(.comment)
		*(.note.*)
		*(.eh_frame)
		*(.eh_frame_hdr)
	}
}
