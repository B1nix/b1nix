PHDRS
{
	text PT_LOAD FLAGS(5);
	ro PT_LOAD FLAGS(4);
	data PT_LOAD FLAGS(6);
	dynamic PT_DYNAMIC FLAGS(6);
}

SECTIONS
{
	. = 0;
	.text : ALIGN(0x1000) { *(.text .text.*) } :text
	.rodata : ALIGN(0x1000) { *(.rodata .rodata.*) } :ro
	.rela.dyn : ALIGN(0x1000) { *(.rela.dyn) } :ro
	.rela.plt : { *(.rela.plt) } :ro
	.dynsym : { *(.dynsym) } :ro
	.dynstr : { *(.dynstr) } :ro
	.hash : { *(.hash) } :ro
	.gnu.hash : { *(.gnu.hash) } :ro

	.data : ALIGN(0x1000) { *(.data .data.*) } :data
	.init_array : ALIGN(8) {
		__init_array_start = .;
		KEEP(*(.preinit_array))
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array .ctors))
		__init_array_end = .;
	} :data
	.bss : ALIGN(0x1000) { *(COMMON) *(.bss .bss.*) } :data
	.dynamic : ALIGN(0x1000) { *(.dynamic) } :data :dynamic
	.got : { *(.got .got.*) } :data
	.got.plt : { *(.got.plt) } :data
	/DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.eh_frame_hdr) }
}
