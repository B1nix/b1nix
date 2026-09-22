/* SPDX-License-Identifier: GPL-2.0-only */
.set MULTIBOOT2_MAGIC, 0xe85250d6
.set MULTIBOOT2_ARCH_I386, 0
.set KERNEL_VMA, 0xFFFFFFFF80000000

.section .multiboot2, "a"
.align 8
multiboot_header:
	.long MULTIBOOT2_MAGIC
	.long MULTIBOOT2_ARCH_I386
	.long multiboot_header_end - multiboot_header
	.long 0x100000000 - (MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH_I386 + (multiboot_header_end - multiboot_header))

	/* Framebuffer tag (type 5). 1280x800 matches the virtio-gpu native scanout
	 * so the multiboot framebuffer, displayd, and the GPU scanout all agree. */
	.align 8
framebuffer_tag_start:
	.short 5
	.short 0
	.long 20
	.long 1280
	.long 800
	.long 32
framebuffer_tag_end:

	/* Relocatable tag (type 10). The firmware decides where this image goes.
	 *
	 * Under UEFI the 2 MiB the kernel is linked at is not necessarily free --
	 * Limine answered "multiboot2: Could not find viable load address for
	 * executable" and the machine never started -- and an image that cannot be
	 * moved cannot be booted on such a machine at all. The alignment matches
	 * the 2 MiB pages the early tables use for the kernel's own window, so the
	 * distance between where this is linked and where it lands is always a
	 * whole number of those pages. `preference = 1` asks for the lowest
	 * address that fits, so a machine with room at the linked address loads
	 * there and the offset is zero. */
	.align 8
relocatable_tag_start:
	.short 10
	.short 0
	.long 24
	.long 0x200000    /* min_addr */
	.long 0x40000000  /* max_addr */
	.long 0x200000    /* align */
	.long 1           /* preference: lowest possible address */
relocatable_tag_end:

	/* End tag. */
	.align 8
	.short 0
	.short 0
	.long 8
multiboot_header_end:

/* Page tables and stack live in .bss, which is linked HIGH. Early 32-bit code
 * (paging off) reaches them by their PHYSICAL address = symbol - KERNEL_VMA. */
.section .bss
.align 4096
.global pml4
pml4:
	.skip 4096
pdpt:
	.skip 4096
pd0:
	.skip 4096
pd1:
	.skip 4096
pd2:
	.skip 4096
pd3:
	.skip 4096
/* The page directory for the kernel's OWN window (0xFFFFFFFF80000000).
 *
 * The low windows are an identity map and do not care where the kernel was
 * loaded; this one does -- it has to map the kernel's linked addresses onto the
 * physical pages the loader actually put it at -- so it cannot be shared with
 * pd0 any more. */
pdk:
	.skip 4096

/* The 5-level top table (M128). Used only when the CPU has LA57 and the
 * command line asks for it; see enable_long_mode. */
.global pml5
pml5:
	.skip 4096

/* The boot CPU's kernel stack, and the guard page that catches it running off
 * the bottom.
 *
 * This stack is an ordinary .bss array, and so is everything under it: the six
 * boot page tables above, and below those the plain scalars of the networking
 * files (proto_list, proto_lock, the DHCP/DNS statics). Overflowing it was
 * therefore completely silent. It wrote first through the boot page tables —
 * dead memory by then, because vmm_init has long since switched CR3 to tables
 * of its own — which gave the overflow 24 KiB of consequence-free runway, and
 * only then reached the networking scalars. Nothing faulted and nothing
 * complained; the first code to READ one of those words took the blame, which
 * for months was net_proto_reset() at the end of net_init(), reading a
 * proto_list that had a pushed register in it.
 *
 * Two changes, because the bug had two halves. The stack is large enough for
 * the boot path with room to spare — the measured peak is 103,232 bytes on the
 * deepest configuration, which is the whole of the old 64 KiB and half as much
 * again — and `boot_stack_guard` below it is unmapped from the kernel window
 * once paging is up (paging_install_guard_page), so an overflow faults on the
 * instruction that overflows instead of quietly scribbling on a neighbour.
 *
 * The guard is 32 KiB rather than one page because a guard only works if the
 * stack cannot step over it, and this kernel has a stack frame of 28,584 bytes
 * (syscall_dispatch_impl_inner). One page under a frame that big is a tripwire
 * a burglar walks past. 32 KiB is unmapped address space plus 32 KiB of .bss
 * nothing reads; the cost is nil and the guarantee is total.
 *
 * All four symbols are page-aligned and global: the guard has to be whole pages
 * to be unmappable, and C needs to see them to unmap it, to paint the stack,
 * and to report its high-water mark. */
.align 4096
.global boot_stack_guard
boot_stack_guard:
	.skip 32768
.global boot_stack_guard_end
boot_stack_guard_end:

.align 4096
.global stack_bottom
stack_bottom:
	.skip 262144
.global stack_top
stack_top:

/* ---------------------------------------------------------------------------
 * Early boot. Linked LOW (.boot is at physical 1M, identity), 32-bit, paging
 * off. The bootloader enters here (e_entry = _start, a low address reachable in 32-bit
 * protected mode). Sets up page tables that map BOTH the low identity window
 * (for this code + physical access) AND the kernel's high window
 * (0xFFFFFFFF80000000), enables long mode, then trampolines up to the high VA.
 * ------------------------------------------------------------------------- */
.section .boot, "ax", @progbits
.code32
.global _start
.type _start, @function
_start:
	cli
	/* Where we actually are, minus where we were linked.
	 *
	 * The image is relocatable (see the header tag), so every absolute address
	 * in this file -- the boot stack, the page tables, the GDT pointer, the
	 * far-jump target -- is a LINK-time address and the loader may have put the
	 * image somewhere else entirely. %ebp carries the difference for the whole
	 * of the 32-bit boot, and every absolute reference below is written
	 * relative to it. It is derived from the program counter rather than from a
	 * Multiboot tag: a call/pop tells us where this instruction really is, and
	 * we know where it was linked, which needs no cooperation from anybody. */
	call 1f
1:
	popl %ebp
	subl $1b, %ebp

	/* Physical stack: stack_top is a high symbol; reach it via -KERNEL_VMA. */
	leal (stack_top - KERNEL_VMA)(%ebp), %esp

	movl %eax, (multiboot_magic_saved - KERNEL_VMA)(%ebp)
	movl %ebx, (multiboot_info_saved - KERNEL_VMA)(%ebp)
	/* The C side needs the same offset: a kernel symbol's physical address is
	 * its virtual address minus KERNEL_VMA plus this. */
	movl %ebp, (kernel_load_offset - KERNEL_VMA)(%ebp)

	call setup_page_tables

	/* The lgdt operand holds a 32-bit physical base, which moved with us. */
	leal (gdt64 - KERNEL_VMA)(%ebp), %eax
	movl %eax, (gdt64_pointer_low + 2)(%ebp)
	lgdt (gdt64_pointer_low)(%ebp)

	call enable_long_mode

	/* A 32-bit far jump can't encode the 64-bit high target, so jump to a
	 * 64-bit trampoline still in the low identity window, then leap to high.
	 * The trampoline moved with us too, so the far pointer is built rather
	 * than encoded. */
	leal (long_mode_low)(%ebp), %eax
	movl %eax, (long_mode_far_ptr)(%ebp)
	ljmpl *(long_mode_far_ptr)(%ebp)

setup_page_tables:
	/* Clear pml4 + pdpt + pd0..pd3 + pdk + pml5 = 8 pages = 32768 bytes (8192
	 * dwords). They are contiguous in .bss, in that order. */
	leal (pml4 - KERNEL_VMA)(%ebp), %edi
	xorl %eax, %eax
	movl $8192, %ecx
	rep stosl

	/* pml4[0]   -> pdpt : low identity window (this code + physical access)
	 * pml4[256] -> pdpt : direct-map window (0xFFFF800000000000)
	 * pml4[511] -> pdpt : kernel high window (0xFFFFFFFF80000000) */
	leal (pdpt - KERNEL_VMA)(%ebp), %eax
	orl $0x003, %eax
	movl %eax, (pml4 - KERNEL_VMA)(%ebp)
	movl %eax, (pml4 - KERNEL_VMA + 256*8)(%ebp)
	movl %eax, (pml4 - KERNEL_VMA + 511*8)(%ebp)

	/* pdpt[0..3] -> pd0..pd3 : first 4 GiB (identity + direct map). */
	leal (pd0 - KERNEL_VMA)(%ebp), %eax
	orl $0x003, %eax
	movl %eax, (pdpt - KERNEL_VMA)(%ebp)
	leal (pd1 - KERNEL_VMA)(%ebp), %eax
	orl $0x003, %eax
	movl %eax, (pdpt - KERNEL_VMA + 8)(%ebp)
	leal (pd2 - KERNEL_VMA)(%ebp), %eax
	orl $0x003, %eax
	movl %eax, (pdpt - KERNEL_VMA + 16)(%ebp)
	leal (pd3 - KERNEL_VMA)(%ebp), %eax
	orl $0x003, %eax
	movl %eax, (pdpt - KERNEL_VMA + 24)(%ebp)

	/* pdpt[510] -> pdk : maps 0xFFFFFFFF80000000-0xFFFFFFFFC0000000 onto the
	 * gigabyte the kernel was LOADED at, which is where its linked addresses
	 * have to resolve. */
	leal (pdk - KERNEL_VMA)(%ebp), %eax
	orl $0x003, %eax
	movl %eax, (pdpt - KERNEL_VMA + 510*8)(%ebp)

	/* Fill pd0..pd3 with 2 MiB pages covering phys 0-4 GiB. */
	movl $0, %ecx
.map_pd:
	movl %ecx, %eax
	shll $21, %eax
	orl $0x083, %eax
	movl %eax, (pd0 - KERNEL_VMA)(%ebp,%ecx,8)
	movl $0, (pd0 - KERNEL_VMA + 4)(%ebp,%ecx,8)
	incl %ecx
	cmpl $2048, %ecx
	jne .map_pd

	/* Fill pdk with 2 MiB pages covering the first gigabyte FROM THE LOAD
	 * ADDRESS: entry i maps the kernel window's i-th 2 MiB onto physical
	 * (i << 21) + offset. The load offset is a whole number of 2 MiB pages
	 * (the header tag asks for that alignment), so this is exact. */
	movl $0, %ecx
.map_pdk:
	movl %ecx, %eax
	shll $21, %eax
	addl %ebp, %eax
	orl $0x083, %eax
	movl %eax, (pdk - KERNEL_VMA)(%ebp,%ecx,8)
	movl $0, (pdk - KERNEL_VMA + 4)(%ebp,%ecx,8)
	incl %ecx
	cmpl $512, %ecx
	jne .map_pdk
	ret

/* ── 5-level paging (M128) ──────────────────────────────────────────────
 *
 * Decide whether to run with LA57: the CPU must have it (CPUID leaf 7,
 * ECX bit 16) and the command line must ask for it (b1nix.la57). It is opt-in
 * rather than automatic because no machine here has the feature — it is
 * verified under QEMU's TCG with -cpu max,la57=on — and because this is the
 * one decision that cannot be revisited once paging is on.
 *
 * The table is shaped so that nothing above this file has to know: PML5[0]
 * and PML5[511] both point at the PML4 that four-level paging would have
 * used, so the low identity window (bits 56:48 = 0) and the kernel's high
 * windows (bits 56:48 = all ones) resolve through exactly the same entries
 * they always did. Every existing four-level walk stays correct; what changes
 * is only which table CR3 names.
 *
 * Returns with %edx = 1 when LA57 was enabled, 0 otherwise. Clobbers
 * eax/ebx/ecx/esi/edi. */
decide_la57:
	xorl %edx, %edx
	/* CPUID leaf 7 needs max-leaf >= 7. */
	pushl %ebx
	xorl %eax, %eax
	cpuid
	cmpl $7, %eax
	jb 9f
	movl $7, %eax
	xorl %ecx, %ecx
	cpuid
	testl $(1 << 16), %ecx     /* LA57 */
	jz 9f
	movl $1, %edx
9:
	popl %ebx
	testl %edx, %edx
	jz 8f
	/* The command line has to ask. Walk the Multiboot2 tag list for the
	 * boot-command-line tag (type 1) and look for "b1nix.la57" in it. */
	xorl %edx, %edx
	movl (multiboot_info_saved - KERNEL_VMA)(%ebp), %esi
	testl %esi, %esi
	jz 8f
	addl $8, %esi              /* first tag, past total_size + reserved */
1:
	movl (%esi), %eax          /* tag type */
	movl 4(%esi), %ecx         /* tag size */
	testl %eax, %eax
	jz 8f                      /* type 0: end of list */
	cmpl $8, %ecx
	jb 8f                      /* a malformed list: stop rather than loop */
	cmpl $1, %eax
	jne 3f
	/* Search the NUL-terminated string at %esi+8 for the flag. */
	leal 8(%esi), %edi
2:
	movb (%edi), %al
	testb %al, %al
	jz 3f
	pushl %esi
	pushl %edi
	leal (la57_flag_string - KERNEL_VMA)(%ebp), %esi
4:
	movb (%esi), %al
	testb %al, %al
	jz 5f                      /* matched the whole flag */
	movb (%edi), %bl
	cmpb %al, %bl
	jne 6f
	incl %esi
	incl %edi
	jmp 4b
5:
	popl %edi
	popl %esi
	movl $1, %edx
	jmp 8f
6:
	popl %edi
	popl %esi
	incl %edi
	jmp 2b
3:
	addl %ecx, %esi
	addl $7, %esi
	andl $-8, %esi             /* tags are 8-byte aligned */
	jmp 1b
8:
	ret

enable_long_mode:
	call decide_la57
	movl %edx, (la57_enabled - KERNEL_VMA)(%ebp)
	testl %edx, %edx
	jz 7f
	/* PML5[0] and PML5[511] -> the PML4 built above. */
	leal (pml4 - KERNEL_VMA)(%ebp), %eax
	orl $0x003, %eax
	movl %eax, (pml5 - KERNEL_VMA)(%ebp)
	movl %eax, (pml5 - KERNEL_VMA + 511*8)(%ebp)
	leal (pml5 - KERNEL_VMA)(%ebp), %eax
	movl %eax, %cr3
	movl %cr4, %eax
	orl $(1 << 5) | (1 << 12), %eax  /* PAE + LA57 */
	movl %eax, %cr4
	jmp 6f
7:
	leal (pml4 - KERNEL_VMA)(%ebp), %eax
	movl %eax, %cr3

	movl %cr4, %eax
	orl $(1 << 5), %eax        /* PAE */
	movl %eax, %cr4
6:

	/* M95: probe NX support (CPUID.80000001H:EDX[20]) so EFER.NXE can be set
	 * alongside LME. The module loader maps module data pages with PTE bit 63
	 * set; with NXE clear that bit is reserved and any access to such a page
	 * faults. %ebx still holds the Multiboot2 info pointer here, so save it
	 * across cpuid. */
	pushl %ebx
	pushl %esi
	movl $0x80000000, %eax
	cpuid
	xorl %esi, %esi
	cmpl $0x80000001, %eax
	jb 1f
	movl $0x80000001, %eax
	cpuid
	andl $(1 << 20), %edx
	movl %edx, %esi
1:
	movl $0xc0000080, %ecx
	rdmsr
	orl $(1 << 8) | 1, %eax    /* LME (bit 8) + SCE (bit 0) */
	testl %esi, %esi
	jz 2f
	orl $(1 << 11), %eax       /* NXE */
2:
	wrmsr
	popl %esi
	popl %ebx

	movl %cr0, %eax
	orl $0x80000001, %eax      /* PG + PE */
	movl %eax, %cr0
	ret

	/* Early 32-bit lgdt operand: limit + 32-bit PHYSICAL base of the GDT. The
	 * base is patched with the load offset before use. */
.align 16
gdt64_pointer_low:
	.word gdt64_end - gdt64 - 1
	.long gdt64 - KERNEL_VMA

	/* The far pointer the 32-bit code jumps through to reach the 64-bit
	 * trampoline. The offset is filled in at run time, because the trampoline
	 * is wherever the image was loaded. */
.align 8
long_mode_far_ptr:
	.long 0
	.short 0x08

	/* 64-bit trampoline, still at a low identity address. Leap to the high VA. */
.code64
long_mode_low:
	movabs $long_mode_start, %rax
	jmp *%rax

/* ---------------------------------------------------------------------------
 * High-half kernel entry. Runs at the kernel's linked (high) VA.
 * ------------------------------------------------------------------------- */
.section .text
.code64
long_mode_start:
	lgdt gdt64_pointer(%rip)
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %ss
	movw %ax, %fs
	movw %ax, %gs

	movq $stack_top, %rsp      /* high stack */
	xorq %rbp, %rbp

	movl multiboot_magic_saved(%rip), %edi
	movl multiboot_info_saved(%rip), %esi
	call kernel_main

.hang:
	cli
	hlt
	jmp .hang

/* GDT lives in .text (writable via the 2MB kernel mapping — arch.c fills the
 * per-CPU TSS descriptors at runtime). Both the early phys pointer and the
 * high pointer below reference it. */
.align 16
.global gdt64
gdt64:
	.quad 0
	.quad 0x00af9a000000ffff /* 0x08: Kernel Code */
	.quad 0x00af92000000ffff /* 0x10: Kernel Data */
	.quad 0x00aff2000000ffff /* 0x18: User Data (Ring 3) */
	.quad 0x00affa000000ffff /* 0x20: User Code (Ring 3) */
.global gdt64_tss
gdt64_tss:
	/* One 16-byte TSS descriptor per CPU (MAX_CPUS=64). Filled by arch.c. */
	.fill 128, 8, 0
gdt64_end:
.global gdt64_pointer
gdt64_pointer:
	.word gdt64_end - gdt64 - 1
	.quad gdt64

.section .data
multiboot_magic_saved:
	.long 0
multiboot_info_saved:
	.long 0
/* 1 when CR4.LA57 was set above; read by the C paging code, which has to
 * build a 5-level top table for every address space it creates. */
.global la57_enabled
la57_enabled:
	.long 0
/* Where the loader put this image, minus where it was linked. Zero when the
 * two agree. Every conversion of a kernel symbol's address to a physical one
 * has to add it: see kernel_phys_offset() in mm.h. */
.global kernel_load_offset
kernel_load_offset:
	.long 0
la57_flag_string:
	.asciz "b1nix.la57"
