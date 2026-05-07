.section .text
.global _start
.type _start, %function

.extern kernel_main
.extern __bss_start
.extern __bss_end

_start:
	// Save DTB address from x0 to x19
	mov x19, x0

	// Set up stack
	ldr x0, =stack_top
	mov sp, x0

	// Clear BSS
	ldr x0, =__bss_start
	ldr x1, =__bss_end
.clear_bss:
	cmp x0, x1
	b.eq .bss_cleared
	str xzr, [x0], #8
	b .clear_bss
.bss_cleared:

	// Restore DTB address to x0, arg1 to x1
	mov x0, x19
	mov x1, #0
	
	// Jump to kernel_main
	bl kernel_main

.hang:
	wfe
	b .hang

.section .bss
.align 12
stack_bottom:
	.skip 16384
stack_top:
