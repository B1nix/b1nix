PHDRS
{
	text PT_LOAD FLAGS(5);
	ro PT_LOAD FLAGS(4);
	data PT_LOAD FLAGS(6);
	dynamic PT_DYNAMIC FLAGS(6);
	tls PT_TLS FLAGS(4);
}

SECTIONS
{
	. = 0;
	PROVIDE_HIDDEN(__b1nix_image_base = .);
	.text : ALIGN(0x1000) { *(.text .text.*) } :text
	.rodata : ALIGN(0x1000) { *(.rodata .rodata.*) } :ro
	.rela.dyn : ALIGN(0x1000) { *(.rela.dyn) } :ro
	.rela.plt : { *(.rela.plt) } :ro
	.dynsym : { *(.dynsym) } :ro
	.dynstr : { *(.dynstr) } :ro
	.hash : { *(.hash) } :ro
	.gnu.hash : { *(.gnu.hash) } :ro

	/* Thread-local storage template. A dlopen'd object's __thread variables are
	 * reached via general-dynamic TLS (__tls_get_addr); the loader copies this
	 * PT_TLS template into a per-module block on first use. .tbss is NOBITS and
	 * must follow .tdata so PT_TLS memsz spans both. */
	.tdata ALIGN(8) : { *(.tdata .tdata.* .gnu.linkonce.td.*) } :data :tls
	.tbss ALIGN(8) : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) } :data :tls

	.data : ALIGN(0x1000) { *(.data .data.*) } :data
	.init_array : ALIGN(8) {
		__init_array_start = .;
		KEEP(*(.preinit_array))
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array .ctors))
		__init_array_end = .;
	} :data
	.fini_array : ALIGN(8) {
		__fini_array_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
		KEEP(*(.fini_array .dtors))
		__fini_array_end = .;
	} :data
	.bss : ALIGN(0x1000) { *(COMMON) *(.bss .bss.*) } :data
	.dynamic : ALIGN(0x1000) { *(.dynamic) } :data :dynamic
	.got : { *(.got .got.*) } :data
	.got.plt : { *(.got.plt) } :data
	/DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.eh_frame_hdr) }
}
