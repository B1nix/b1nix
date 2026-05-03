.section .text
.global _start
.type _start, %function

.equ UART0, 0x09000000
.equ UARTFR, 0x18
.equ UARTDR, 0x00

_start:
	ldr x0, =stack_top
	mov sp, x0
	ldr x0, =message
	bl puts

.hang:
	wfe
	b .hang

puts:
	ldrb w1, [x0], #1
	cbz w1, .puts_done
	bl putc
	b puts
.puts_done:
	ret

putc:
	ldr x2, =UART0
.wait:
	ldr w3, [x2, #UARTFR]
	tbnz w3, #5, .wait
	str w1, [x2, #UARTDR]
	ret

.section .rodata
message:
	.asciz "b1nix aarch64 qemu virt boot\n"

.section .bss
.align 12
stack_bottom:
	.skip 16384
stack_top:
