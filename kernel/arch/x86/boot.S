.set MULTIBOOT2_MAGIC, 0xe85250d6
.set MULTIBOOT2_ARCH_I386, 0
.set KERNEL_VMA, 0x80000000

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

/* The boot page directory and stack live in .bss, which is linked HIGH (into
 * the direct map). Early 32-bit code (paging off) reaches them by their
 * PHYSICAL address = symbol - KERNEL_VMA. */
.section .bss
.align 4096
.global boot_page_directory
boot_page_directory:
	.skip 4096

.align 16
stack_bottom:
	.skip 65536
stack_top:

/* ---------------------------------------------------------------------------
 * Early boot. Linked LOW (.boot at physical 1M, identity), 32-bit, paging off.
 * Maps the first 64M at BOTH identity (for this code) and the direct map
 * (0x80000000 = KERNEL_VMA, where the kernel is linked), enables paging, then
 * jumps to the high entry. A 32-bit far jump can encode the high target
 * directly, so no trampoline is needed.
 * ------------------------------------------------------------------------- */
.section .boot, "ax", @progbits
.code32
.global _start
.type _start, @function
_start:
	cli
	/* Physical stack (stack_top is a high symbol). */
	movl $(stack_top - KERNEL_VMA), %esp

	movl %eax, (multiboot_magic_saved - KERNEL_VMA)
	movl %ebx, (multiboot_info_saved - KERNEL_VMA)

	call setup_boot_page_tables

	/* Load page directory (physical). */
	movl $(boot_page_directory - KERNEL_VMA), %eax
	movl %eax, %cr3

	/* Enable PSE (4MB pages). */
	movl %cr4, %eax
	orl $(1 << 4), %eax
	movl %eax, %cr4

	/* Enable paging. */
	movl %cr0, %eax
	orl $0x80000001, %eax
	movl %eax, %cr0

	/* Paging is on; the GDT (high VA) is now reachable via the direct map. */
	lgdt gdt32_pointer

	/* Jump to the high (direct-map) kernel. */
	ljmp $0x08, $higher_half_start

setup_boot_page_tables:
	/* Clear boot_page_directory (physical). */
	movl $(boot_page_directory - KERNEL_VMA), %edi
	xorl %eax, %eax
	movl $1024, %ecx
	rep stosl

	/* Map the first 64MB (16 entries of 4MB) at identity AND at
	 * KERNEL_VMA (0x80000000, PD index 512 -> byte offset 2048). */
	movl $0, %ecx
.map_boot:
	movl %ecx, %eax
	shll $22, %eax
	orl $0x83, %eax
	movl %eax, (boot_page_directory - KERNEL_VMA)(,%ecx,4)
	movl %eax, (boot_page_directory - KERNEL_VMA + 2048)(,%ecx,4)
	incl %ecx
	cmpl $16, %ecx
	jne .map_boot
	ret

/* ---------------------------------------------------------------------------
 * High-half kernel entry (runs at the kernel's linked, direct-map VA).
 * ------------------------------------------------------------------------- */
.section .text
.code32
higher_half_start:
	/* Set up segment registers. */
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %ss
	movw %ax, %fs
	movw %ax, %gs

	movl $stack_top, %esp      /* high stack */

	/* Call kernel_main(magic, info). */
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
