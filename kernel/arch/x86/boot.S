.set MULTIBOOT2_MAGIC, 0xe85250d6
.set MULTIBOOT2_ARCH_I386, 0

.section .multiboot2, "a"
.align 8
multiboot_header:
	.long MULTIBOOT2_MAGIC
	.long MULTIBOOT2_ARCH_I386
	.long multiboot_header_end - multiboot_header
	.long 0x100000000 - (MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH_I386 + (multiboot_header_end - multiboot_header))

	/* Framebuffer tag (type 5) */
	.align 8
framebuffer_tag_start:
	.short 5
	.short 0
	.long 20
	.long 1024
	.long 768
	.long 32
framebuffer_tag_end:

	/* End tag. */
	.align 8
	.short 0
	.short 0
	.long 8
multiboot_header_end:

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

.section .text
.code32
.global _start
.type _start, @function
_start:
	cli
	movl $stack_top, %esp

	movl %eax, multiboot_magic_saved
	movl %ebx, multiboot_info_saved

	call setup_page_tables
	lgdt gdt64_pointer
	call enable_long_mode

	ljmp $0x08, $long_mode_start

setup_page_tables:
	movl $pml4, %edi
	xorl %eax, %eax
	/* Clear 6 pages (pml4, pdpt, pd0, pd1, pd2, pd3) = 6 * 4096 = 24576 bytes */
	/* 24576 / 4 = 6144 */
	movl $6144, %ecx
	rep stosl

	movl $pdpt, %eax
	orl $0x003, %eax
	movl %eax, pml4
	movl %eax, pml4+(256*8) /* Map higher half to same physical pages */

	movl $pd0, %eax
	orl $0x003, %eax
	movl %eax, pdpt
	movl $pd1, %eax
	orl $0x003, %eax
	movl %eax, pdpt+8
	movl $pd2, %eax
	orl $0x003, %eax
	movl %eax, pdpt+16
	movl $pd3, %eax
	orl $0x003, %eax
	movl %eax, pdpt+24

	movl $0, %ecx
.map_pd:
	movl %ecx, %eax
	shll $21, %eax
	orl $0x083, %eax
	
	/* Map 2048 entries (4 * 512) across 4 page directories */
	movl %eax, pd0(,%ecx,8)
	movl $0, pd0+4(,%ecx,8)
	
	incl %ecx
	cmpl $2048, %ecx
	jne .map_pd

	ret

enable_long_mode:
	movl $pml4, %eax
	movl %eax, %cr3

	movl %cr4, %eax
	orl $(1 << 5), %eax
	movl %eax, %cr4

	movl $0xc0000080, %ecx
	rdmsr
	orl $(1 << 8) | 1, %eax /* Set LME (bit 8) and SCE (bit 0) */
	wrmsr

	movl %cr0, %eax
	orl $0x80000001, %eax
	movl %eax, %cr0
	ret

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
	/* One 16-byte (2-quad) TSS descriptor per CPU, MAX_CPUS=16 slots. The BSP
	 * uses selector 0x28; CPU k uses 0x28 + k*16. All filled in at runtime by
	 * arch.c (x86_tss_init_cpu). Reserving them up front keeps every CPU's TSS
	 * descriptor in the one shared GDT so APs can run ring 3. */
	.fill 32, 8, 0
gdt64_end:
.global gdt64_pointer
gdt64_pointer:
	.word gdt64_end - gdt64 - 1
	.quad gdt64

.code64
long_mode_start:
	lgdt gdt64_pointer(%rip)
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %ss
	movw %ax, %fs
	movw %ax, %gs

	movq $stack_top, %rsp
	xorq %rbp, %rbp

	movl multiboot_magic_saved(%rip), %edi
	movl multiboot_info_saved(%rip), %esi
	call kernel_main

.hang:
	cli
	hlt
	jmp .hang

.section .data
multiboot_magic_saved:
	.long 0
multiboot_info_saved:
	.long 0
