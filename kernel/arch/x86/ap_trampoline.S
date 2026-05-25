/*
 * AP Trampoline — compiled as a flat binary linked at 0x8000.
 * The BSP copies this to physical address 0x8000 and sends SIPI.
 */

.global _start
_start:
    .code16
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ss
    movw $0x1000, %sp
    lgdtl (gdt_ptr)
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $0x08, $ap_32

ap_32:
    .code32
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl (pml4_phys), %eax
    movl %eax, %cr3
    movl %cr4, %eax
    orl $(1 << 5) | (1 << 7), %eax
    movl %eax, %cr4
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0
    ljmpl $0x08, $ap_64

ap_64:
    .code64
    xorq %rax, %rax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movq (stack_ptr), %rsp
    movq (percpu_ptr), %rax
    testq %rax, %rax
    jz 1f
    movq %rax, %rdx
    shrq $32, %rdx
    movl $0xC0000101, %ecx
    movl %eax, %eax
    wrmsr
1:
    movl $1, (ready_flag)
    movq (cpu_id), %rdi
    xorq %rbp, %rbp
    call *ap_main_ptr(%rip)
2:  cli
    hlt
    jmp 2b

/* Data filled by BSP before SIPI */
.align 8
pml4_phys:  .quad 0
stack_ptr:  .quad 0
percpu_ptr: .quad 0
cpu_id:     .quad 0
gdt_ptr:    .word 23  /* limit */
            .quad gdt /* base */
ready_flag: .long 0
ap_main_ptr: .quad 0

.align 8
gdt:    .quad 0x0000000000000000
        .quad 0x00AF9A000000FFFF
        .quad 0x00AF92000000FFFF
