/* B1NIX Userspace Linker Script — LLVM libc++ variant (tools/toolchain/bin/b1nix-c++).
 *
 * Distinct from linker-cxx.ld (the GCC libstdc++ / libgcc variant) because LLVM
 * libc++ unwinds C++ exceptions with libunwind, which locates every object's FDEs
 * by walking dl_iterate_phdr and reading each module's PT_GNU_EH_FRAME segment
 * (.eh_frame_hdr) — the standard ELF mechanism, NOT libgcc's __register_frame
 * registry. That imposes two requirements this script meets and linker-cxx.ld
 * deliberately does not:
 *
 *   1. The program header table must be MAPPED at run time, so the dl_iterate_phdr
 *      callback (libunwind::findUnwindSectionsByPhdr) can read dlpi_phdr without
 *      faulting. linker-cxx.ld places the ELF header/phdrs outside every PT_LOAD,
 *      so dlpi_phdr points at unmapped memory and the first throw segfaults.
 *      Starting `.` at base + SIZEOF_HEADERS leaves the ELF header + phdrs at the
 *      load base inside the first PT_LOAD, so they are mapped and lld emits PT_PHDR.
 *
 *   2. .eh_frame_hdr must NOT be explicitly defined in the linker script:
 *      the driver's --eh-frame-hdr tells ld.lld to auto-generate it from the
 *      .eh_frame contents. An explicit section definition with *(.eh_frame_hdr)
 *      that has no input content gets emptied and --gc-sections drops it,
 *      leaving the binary without PT_GNU_EH_FRAME.
 *
 * Crucially this script does NOT use an explicit PHDRS command: lld then
 * auto-generates the full set of program headers (PT_PHDR, PT_INTERP, PT_LOAD,
 * PT_DYNAMIC, PT_TLS, PT_GNU_EH_FRAME, PT_GNU_STACK). An explicit PHDRS that omits
 * PT_DYNAMIC/PT_INTERP would leave the M69 loader unable to link the binary, and
 * it crashes in _start before main.
 *
 * The fixed userspace load base 0x2000000 is unchanged (the loader/relocator
 * assume it). KEEP guards .init_array/.eh_frame/.gcc_except_table against
 * --gc-sections. .tdata must precede .tbss so the PT_TLS memsz spans both (one
 * COMDAT .tdata/.tbss per thread_local — see linker-cxx.ld for the rationale).
 */

ENTRY(_start)

SECTIONS
{
	/* Leave room for the ELF header + program headers at the load base so they
	 * land inside the first PT_LOAD and are mapped (requirement 1 above). */
	. = 0x2000000 + SIZEOF_HEADERS;

	.text ALIGN(16) : {
		*(.text .text.*)
	}

	.rodata ALIGN(4K) : {
		*(.rodata .rodata.*)
	}

	/* DO NOT explicitly define .eh_frame_hdr here — the b1nix-c++ driver's
	 * --eh-frame-hdr flag tells ld.lld to auto-generate it from .eh_frame.
	 * An explicit *(.eh_frame_hdr) with no input content gets emptied and
	 * --gc-sections drops it, leaving the binary without PT_GNU_EH_FRAME. */

	.eh_frame ALIGN(8) : {
		__EH_FRAME_BEGIN__ = .;
		KEEP(*(.eh_frame .eh_frame.*))
		LONG(0)
	}
	.gcc_except_table ALIGN(8) : {
		KEEP(*(.gcc_except_table .gcc_except_table.*))
	}

	/* Thread-local storage (one COMDAT section per thread_local). .tbss is NOBITS
	 * and must follow .tdata so the segment memsz spans both. */
	.tdata ALIGN(8) : {
		*(.tdata .tdata.* .gnu.linkonce.td.*)
	}
	.tbss ALIGN(8) : {
		*(.tbss .tbss.* .gnu.linkonce.tb.*)
		*(.tcommon)
	}

	.init_array ALIGN(8) : {
		__init_array_start = .;
		KEEP(*(.preinit_array))
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(SORT_BY_NAME(.ctors.*)))
		KEEP(*(.init_array .ctors))
		__init_array_end = .;
	}

	.data ALIGN(4K) : {
		*(.data .data.*)
	}

	.bss ALIGN(4K) : {
		*(COMMON)
		*(.bss .bss.*)
	}

	/DISCARD/ : {
		*(.comment)
		*(.note.*)
	}
}
