/* B1NIX Userspace Linker Script
 *
 * Produces statically-linked ET_EXEC ELF executables for both b1nix targets
 * (x86_64-b1nix and i686-b1nix; the -m flag in userspace/Makefile selects the
 * ELF class). Entry point: _start (from crt0.o).
 * Load base: 0x02000000 (32 MiB) — above the kernel image; 0x400000 collides
 * with the 32-bit kernel. See docs/x86-32bit-port.md and CLAUDE.md (ABI).
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

		/* ELF constructor array. crt0 walks [__init_array_start, __init_array_end)
		 * before main(). Empty for plain-C b1nix binaries, but the boundary
		 * symbols must be defined so crt0 links. */
		__init_array_start = .;
		KEEP(*(.preinit_array))
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array .ctors))
		__init_array_end = .;
	}

	/* Thread-local storage. Collect every per-variable COMDAT .tdata.<sym>/
	 * .tbss.<sym> section into one contiguous PT_TLS. Without this, ld places
	 * each as an orphan and overlaps them all at the same address, so distinct
	 * thread_locals alias the same storage (the bug that crashed V8's
	 * Isolate::Enter — see userspace/linker-cxx.ld). Plain-C b1nix binaries
	 * rarely have >1 thread_local, but keep the layout correct regardless.
	 * .tbss is NOBITS and must follow .tdata so the segment memsz spans both. */
	.tdata ALIGN(8) : {
		*(.tdata .tdata.* .gnu.linkonce.td.*)
	}
	.tbss ALIGN(8) : {
		*(.tbss .tbss.* .gnu.linkonce.tb.*)
		*(.tcommon)
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
