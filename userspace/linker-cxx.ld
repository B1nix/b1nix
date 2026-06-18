/* B1NIX Userspace Linker Script — C++ variant (tools/b1nix-c++).
 *
 * Identical to linker.ld except it KEEPs the DWARF unwind info and exception
 * landing-pad tables that C++ exceptions need. The plain linker.ld discards
 * .eh_frame (clang C binaries never throw), so this separate script keeps the
 * change isolated to C++ binaries — every existing clang binary stays byte for
 * byte the same.
 *
 * crt0 hands __EH_FRAME_BEGIN__ to libgcc's __register_frame_info; the trailing
 * zero word terminates the CIE/FDE chain it walks. KEEP guards the sections
 * against --gc-sections.
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

	/* Global constructors. GCC/libstdc++ on this target emit a mix of the modern
	 * .init_array form AND the legacy priority-suffixed .ctors.NNNNN form — the
	 * iostream initializer (_GLOBAL__sub_I...__ZSt3cin, which constructs
	 * std::cin/cout/cerr and std::ios_base::Init) lands in .ctors.65445. crt0
	 * runs this block forward, so the legacy .ctors.* (which the toolchain emits
	 * with priority encoded so a plain ascending sort matches default-priority
	 * ctors last) are gathered after the explicit .init_array entries. Missing
	 * the .ctors.* glob leaves std::cout's streambuf unconstructed → the first
	 * "std::cout <<" faults in ostream::sentry. */
	.init_array ALIGN(8) : {
		__init_array_start = .;
		KEEP(*(.preinit_array))
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(SORT_BY_NAME(.ctors.*)))
		KEEP(*(.init_array .ctors))
		__init_array_end = .;
	}

	.eh_frame ALIGN(8) : {
		__EH_FRAME_BEGIN__ = .;
		KEEP(*(.eh_frame .eh_frame.*))
		LONG(0)
	}
	.gcc_except_table ALIGN(8) : {
		KEEP(*(.gcc_except_table .gcc_except_table.*))
	}

	.bss ALIGN(4K) : {
		*(COMMON)
		*(.bss .bss.*)
	}

	/DISCARD/ : {
		*(.comment)
		*(.note.*)
		*(.eh_frame_hdr)
	}
}
