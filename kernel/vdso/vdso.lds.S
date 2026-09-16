/*
 * Layout of the vDSO shared object. Preprocessed with -DVDSO_X86_64 or
 * -DVDSO_AARCH64 before the link.
 *
 * One PT_LOAD at address 0 that starts with the ELF and program headers, so the
 * kernel maps the file verbatim and AT_SYSINFO_EHDR is the start of the
 * mapping. The [vvar] page is mapped one page below it; vdso_vvar_page is
 * defined relative to the image start, so the code reaches it PC-relatively.
 */

#define PAGE_SIZE 4096

SECTIONS
{
	vdso_vvar_page = . - PAGE_SIZE;

	. = SIZEOF_HEADERS;

	.hash		: { *(.hash) }			:text
	.gnu.hash	: { *(.gnu.hash) }
	.dynsym		: { *(.dynsym) }
	.dynstr		: { *(.dynstr) }
	.gnu.version	: { *(.gnu.version) }
	.gnu.version_d	: { *(.gnu.version_d) }
	.gnu.version_r	: { *(.gnu.version_r) }

	.dynamic	: { *(.dynamic) }		:text	:dynamic

	.rodata		: { *(.rodata .rodata.*) }	:text

	.note		: { *(.note.*) }		:text	:note

	.eh_frame_hdr	: { *(.eh_frame_hdr) }		:text	:eh_frame_hdr
	.eh_frame	: { KEEP (*(.eh_frame)) }	:text

	. = ALIGN(16);
	.text		: { *(.text .text.*) }		:text

	/* Writable or relocated data has nowhere to live: the mapping is shared
	 * and read-only. The build checks that nothing needed any of these. */
	/DISCARD/ : {
		*(.data .data.* .bss .bss.* .got .got.plt .plt .plt.*)
		*(.comment .note.GNU-stack)
	}
}

PHDRS
{
	text		PT_LOAD		FLAGS(5) FILEHDR PHDRS;	/* PF_R|PF_X */
	dynamic		PT_DYNAMIC	FLAGS(4);		/* PF_R */
	note		PT_NOTE		FLAGS(4);
	eh_frame_hdr	PT_GNU_EH_FRAME;
}

/* The versions Linux uses on each architecture, which is what libc asks for. */
VERSION
{
#if defined(VDSO_X86_64)
	LINUX_2.6 {
	global:
		clock_gettime;
		__vdso_clock_gettime;
		gettimeofday;
		__vdso_gettimeofday;
		time;
		__vdso_time;
		clock_getres;
		__vdso_clock_getres;
	local: *;
	};
#elif defined(VDSO_AARCH64)
	LINUX_2.6.39 {
	global:
		__kernel_clock_gettime;
		__kernel_gettimeofday;
		__kernel_clock_getres;
	local: *;
	};
#endif
}
