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
	tls PT_TLS FLAGS(4);
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

	/* Thread-local storage. Collect every per-variable COMDAT .tdata.<sym>/
	 * .tbss.<sym> into one contiguous PT_TLS so distinct thread_locals get
	 * distinct offsets (mirrors linker.ld / linker-cxx.ld). .tbss is NOBITS
	 * and must follow .tdata so the segment memsz spans both. The kernel's
	 * PIE loader copies p_filesz bytes of the template and zero-fills the rest
	 * for each thread, resolving the binary's TPOFF64 relocations. */
	.tdata ALIGN(8) : {
		*(.tdata .tdata.* .gnu.linkonce.td.*)
	} :data :tls
	.tbss ALIGN(8) : {
		*(.tbss .tbss.* .gnu.linkonce.tb.*)
		*(.tcommon)
	} :data :tls

	/* ELF constructor/destructor arrays. crt0 walks
	 * [__init_array_start, __init_array_end) before main(); the boundary
	 * symbols must be defined so crt0 links even for binaries with no ctors. */
	.init_array ALIGN(8) : {
		__init_array_start = .;
		KEEP(*(.preinit_array))
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array .ctors))
		__init_array_end = .;
	} :data
	.fini_array ALIGN(8) : {
		__fini_array_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
		KEEP(*(.fini_array .dtors))
		__fini_array_end = .;
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
