.set MULTIBOOT2_MAGIC, 0xe85250d6
.set MULTIBOOT2_ARCH_I386, 0

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

.section .bss
.align 4096
.global boot_page_directory
boot_page_directory:
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

	call setup_boot_page_tables

	/* Load page directory */
	movl $boot_page_directory, %eax
	movl %eax, %cr3

	/* Enable PSE (4MB pages) */
	movl %cr4, %eax
	orl $(1 << 4), %eax
	movl %eax, %cr4

	/* Enable paging */
	movl %cr0, %eax
	orl $0x80000001, %eax
	movl %eax, %cr0

	/* Load boot GDT */
	lgdt gdt32_pointer

	/* Jump to higher half */
	ljmp $0x08, $higher_half_start

setup_boot_page_tables:
	/* Clear boot_page_directory */
	movl $boot_page_directory, %edi
	xorl %eax, %eax
	movl $1024, %ecx
	rep stosl

	/* Map first 64MB (16 entries of 4MB) identity and to DIRECT_MAP_BASE */
	movl $0, %ecx
.map_boot:
	movl %ecx, %eax
	shll $22, %eax
	orl $0x83, %eax
	movl %eax, boot_page_directory(,%ecx,4)
	movl %eax, boot_page_directory+2048(,%ecx,4)
	incl %ecx
	cmpl $16, %ecx
	jne .map_boot

	ret

higher_half_start:
	/* Set up segment registers */
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %ss
	movw %ax, %fs
	movw %ax, %gs

	/* Call kernel_main(magic, info) */
	movl multiboot_info_saved, %eax
	pushl %eax
	movl multiboot_magic_saved, %eax
	pushl %eax

	call kernel_main

.hang:
	cli
	hlt
	jmp .hang

.section .data
.align 16
.global gdt32
gdt32:
	.quad 0
	.quad 0x00cf9a000000ffff /* 0x08: Kernel Code */
	.quad 0x00cf92000000ffff /* 0x10: Kernel Data */
	.quad 0x00cffa000000ffff /* 0x18: User Code */
	.quad 0x00cff2000000ffff /* 0x20: User Data */
	.quad 0                  /* 0x28: TSS (set up at runtime) */
	.quad 0                  /* 0x30: User TLS (set up at runtime) */
	.quad 0                  /* 0x38: Kernel percpu (set up at runtime) */

.global gdt32_pointer
gdt32_pointer:
	.word 63                 /* 8 entries * 8 bytes - 1 */
	.long gdt32

multiboot_magic_saved:
	.long 0
multiboot_info_saved:
	.long 0
