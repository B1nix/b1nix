/* B1NIX shared-object linker script — C++ variant.
 *
 * = linker_shared.ld (PIC/-shared layout: PHDRS with PT_DYNAMIC, the
 *   dynsym/dynstr/hash/rela.dyn machinery) PLUS the C++ sections from
 *   linker-cxx.ld that a .so built from libc++/SwiftShader needs:
 *     - PT_TLS + .tdata/.tbss   (per-thread ELF TLS; libc++ & V8 use __thread)
 *     - .ctors.* in .init_array (legacy priority-suffixed global ctors)
 *     - .eh_frame + .gcc_except_table (C++ exception unwind/landing-pad tables)
 *
 * Used by tools/v8/chromium-link.sh to relink Chromium's b1nix-target graphics
 * .so's (libEGL/libGLESv2/libvulkan.so.1/libvk_swiftshader.so) as real b1nix
 * shared objects (the gn toolchain's clang-driver link can't produce these).
 */

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
	/* C++ exception tables — read-only, walked by libgcc's unwinder. */
	.eh_frame : ALIGN(8) {
		__EH_FRAME_BEGIN__ = .;
		KEEP(*(.eh_frame .eh_frame.*))
		LONG(0)
	} :ro
	.gcc_except_table : ALIGN(8) {
		KEEP(*(.gcc_except_table .gcc_except_table.*))
	} :ro
	.rela.dyn : ALIGN(0x1000) { *(.rela.dyn) } :ro
	.rela.plt : { *(.rela.plt) } :ro
	.dynsym : { *(.dynsym) } :ro
	.dynstr : { *(.dynstr) } :ro
	.hash : { *(.hash) } :ro
	.gnu.hash : { *(.gnu.hash) } :ro

	/* Thread-local storage: one contiguous PT_TLS spanning .tdata then .tbss
	 * (NOBITS, must follow) so each __thread var gets a distinct offset. */
	.tdata : ALIGN(8) { *(.tdata .tdata.* .gnu.linkonce.td.*) } :data :tls
	.tbss : ALIGN(8) { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) } :data :tls

	.data : ALIGN(0x1000) { *(.data .data.*) } :data
	.init_array : ALIGN(8) {
		__init_array_start = .;
		KEEP(*(.preinit_array))
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(SORT_BY_NAME(.ctors.*)))
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
	/DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame_hdr) }
}
