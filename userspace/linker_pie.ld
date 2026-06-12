/* B1NIX Userspace PIE Linker Script (M30)
 *
 * Produces a position-independent ELF (ET_DYN) for x86_64-b1nix.
 * Base address: 0 (the in-kernel ELF loader picks a load base —
 * PIE_LOAD_BASE in kernel/user/process.c — and applies RELATIVE
 * relocations to fix absolute pointers). Entry point: _start.
 */

ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);
	ro PT_LOAD FLAGS(4);
	data PT_LOAD FLAGS(6);
	dynamic PT_DYNAMIC FLAGS(6);
	interp PT_INTERP FLAGS(4);
}

SECTIONS
{
	. = 0;

	.text : ALIGN(0x1000) {
		*(.text .text.*)
	} :text

	.rodata : ALIGN(0x1000) {
		*(.rodata .rodata.*)
	} :ro

	.rela.dyn : ALIGN(0x1000) { *(.rela.dyn) } :ro
	.rela.plt : { *(.rela.plt) } :ro
	.dynsym   : { *(.dynsym) } :ro
	.dynstr   : { *(.dynstr) } :ro
	.hash     : { *(.hash) } :ro
	.gnu.hash : { *(.gnu.hash) } :ro
	.interp   : { *(.interp) } :ro :interp

	.data : ALIGN(0x1000) {
		*(.data .data.*)
	} :data

	.bss : ALIGN(0x1000) {
		*(COMMON)
		*(.bss .bss.*)
	} :data

	.dynamic  : ALIGN(0x1000) { *(.dynamic) } :data :dynamic
	.got      : { *(.got .got.*) } :data
	.got.plt  : { *(.got.plt) } :data

	/DISCARD/ : {
		*(.comment)
		*(.note.*)
		*(.eh_frame)
		*(.eh_frame_hdr)
	}
}
