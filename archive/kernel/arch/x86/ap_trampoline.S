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

    /* Load temporary GDT using physical address */
    lgdtl (gdt_ptr_phys - _start + 0x8000)

    /* Enable PM */
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0

    /* Jump to 32-bit protected mode */
    ljmpl $0x08, $(ap_32 - _start + 0x8000)

ap_32:
    .code32
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    /* Load page directory */
    movl (pd_phys - _start + 0x8000), %eax
    movl %eax, %cr3

    /* Enable PSE */
    movl %cr4, %eax
    orl $(1 << 4), %eax
    movl %eax, %cr4

    /* Enable paging */
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0

    /* Now paging is active. Switch stack to virtual stack top */
    movl (stack_ptr - _start + 0x8000), %esp
    xorl %ebp, %ebp

    /* Load per-CPU virtual GDT */
    movl (gdt_virtual - _start + 0x8000), %eax
    lgdt (%eax)

    /* Set up segments.
     * FS is loaded with 0x38 (Kernel per-CPU segment, entry 7).
     */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x38, %ax
    movw %ax, %fs
    movw $0x23, %ax /* default User Data for GS */
    movw %ax, %gs

    /* Signal readiness to BSP */
    movl $1, (ready_flag - _start + 0x8000)

    /* Call ap_main(cpu_id) */
    movl (cpu_id - _start + 0x8000), %eax
    pushl %eax
    call *(ap_main_ptr - _start + 0x8000)

hang:
    cli
    hlt
    jmp hang

/* Data populated by BSP before SIPI */
.org 0x100
pd_phys:     .long 0
stack_ptr:   .long 0
percpu_ptr:  .long 0
cpu_id:      .long 0
gdt_virtual: .long 0   /* Pointer to virtual GDTR descriptor */
ready_flag:  .long 0
ap_main_ptr: .long 0

gdt_ptr_phys:
    .word gdt_end - gdt - 1
    .long gdt - _start + 0x8000

.align 8
gdt:
    .quad 0
    .quad 0x00cf9a000000ffff /* 0x08: Kernel Code */
    .quad 0x00cf92000000ffff /* 0x10: Kernel Data */
gdt_end:
