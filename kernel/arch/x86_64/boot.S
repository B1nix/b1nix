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

.align 16
stack_bottom:
	.skip 65536
stack_top:

/* ---------------------------------------------------------------------------
 * Early boot. Linked LOW (.boot is at physical 1M, identity), 32-bit, paging
 * off. GRUB enters here (e_entry = _start, a low address reachable in 32-bit
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
	/* Physical stack: stack_top is a high symbol; reach it via -KERNEL_VMA. */
	movl $(stack_top - KERNEL_VMA), %esp

	movl %eax, (multiboot_magic_saved - KERNEL_VMA)
	movl %ebx, (multiboot_info_saved - KERNEL_VMA)

	call setup_page_tables
	lgdt gdt64_pointer_low      /* .boot-local: low VMA = physical */
	call enable_long_mode

	/* A 32-bit far jump can't encode the 64-bit high target, so jump to a
	 * 64-bit trampoline still in the low identity window, then leap to high. */
	ljmp $0x08, $long_mode_low

setup_page_tables:
	/* Clear pml4 + pdpt + pd0..pd3 = 6 pages = 24576 bytes (6144 dwords). */
	movl $(pml4 - KERNEL_VMA), %edi
	xorl %eax, %eax
	movl $6144, %ecx
	rep stosl

	/* pml4[0]   -> pdpt : low identity window (this code + physical access)
	 * pml4[256] -> pdpt : direct-map window (0xFFFF800000000000)
	 * pml4[511] -> pdpt : kernel high window (0xFFFFFFFF80000000) */
	movl $(pdpt - KERNEL_VMA), %eax
	orl $0x003, %eax
	movl %eax, (pml4 - KERNEL_VMA)
	movl %eax, (pml4 - KERNEL_VMA + 256*8)
	movl %eax, (pml4 - KERNEL_VMA + 511*8)

	/* pdpt[0..3] -> pd0..pd3 : first 4 GiB (identity + direct map). */
	movl $(pd0 - KERNEL_VMA), %eax
	orl $0x003, %eax
	movl %eax, (pdpt - KERNEL_VMA)
	movl $(pd1 - KERNEL_VMA), %eax
	orl $0x003, %eax
	movl %eax, (pdpt - KERNEL_VMA + 8)
	movl $(pd2 - KERNEL_VMA), %eax
	orl $0x003, %eax
	movl %eax, (pdpt - KERNEL_VMA + 16)
	movl $(pd3 - KERNEL_VMA), %eax
	orl $0x003, %eax
	movl %eax, (pdpt - KERNEL_VMA + 24)

	/* pdpt[510] -> pd0 : maps 0xFFFFFFFF80000000-0xFFFFFFFFC0000000 onto phys
	 * 0-1GiB, i.e. the kernel's high VA window onto its physical load. */
	movl $(pd0 - KERNEL_VMA), %eax
	orl $0x003, %eax
	movl %eax, (pdpt - KERNEL_VMA + 510*8)

	/* Fill pd0..pd3 with 2 MiB pages covering phys 0-4 GiB. */
	movl $0, %ecx
.map_pd:
	movl %ecx, %eax
	shll $21, %eax
	orl $0x083, %eax
	movl %eax, (pd0 - KERNEL_VMA)(,%ecx,8)
	movl $0, (pd0 - KERNEL_VMA + 4)(,%ecx,8)
	incl %ecx
	cmpl $2048, %ecx
	jne .map_pd
	ret

enable_long_mode:
	movl $(pml4 - KERNEL_VMA), %eax
	movl %eax, %cr3

	movl %cr4, %eax
	orl $(1 << 5), %eax        /* PAE */
	movl %eax, %cr4

	movl $0xc0000080, %ecx
	rdmsr
	orl $(1 << 8) | 1, %eax    /* LME (bit 8) + SCE (bit 0) */
	wrmsr

	movl %cr0, %eax
	orl $0x80000001, %eax      /* PG + PE */
	movl %eax, %cr0
	ret

	/* Early 32-bit lgdt operand: limit + 32-bit PHYSICAL base of the GDT. */
.align 16
gdt64_pointer_low:
	.word gdt64_end - gdt64 - 1
	.long gdt64 - KERNEL_VMA

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
