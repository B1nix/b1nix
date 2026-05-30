/* B1NIX Userspace PIE Linker Script (M30)
 *
 * Produces a position-independent ELF (ET_DYN) for x86_64-b1nix.
 * Base address: 0 (the in-kernel ELF loader picks a load base —
 * PIE_LOAD_BASE in kernel/user/process.c — and applies RELATIVE
 * relocations to fix absolute pointers). Entry point: _start.
 */

ENTRY(_start)

SECTIONS
{
	. = 0;

	.text : ALIGN(0x1000) {
		*(.text .text.*)
	}

	.rodata : ALIGN(0x1000) {
		*(.rodata .rodata.*)
	}

	.data : ALIGN(0x1000) {
		*(.data .data.*)
	}

	.bss : ALIGN(0x1000) {
		*(COMMON)
		*(.bss .bss.*)
	}

	.rela.dyn : { *(.rela.dyn) }
	.rela.plt : { *(.rela.plt) }
	.dynsym   : { *(.dynsym) }
	.dynstr   : { *(.dynstr) }
	.dynamic  : { *(.dynamic) }
	.hash     : { *(.hash) }
	.gnu.hash : { *(.gnu.hash) }

	/DISCARD/ : {
		*(.comment)
		*(.note.*)
		*(.eh_frame)
		*(.eh_frame_hdr)
		*(.interp)
	}
}
