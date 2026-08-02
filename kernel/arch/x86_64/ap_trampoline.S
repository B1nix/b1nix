/*
 * AP Trampoline — compiled as a flat binary linked at 0x8000.
 * The BSP copies this to physical address 0x8000 and sends SIPI with
 * vector 0x08 (0x8000 >> 12) so the AP begins executing here.
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
    /* M95: probe NX support (CPUID.80000001H:EDX[20]) and enable EFER.NXE
     * together with LME. Without it, an AP that touches a module data page —
     * which carries PTE bit 63 — would take a reserved-bit page fault before
     * x86_ap_arch_init gets a chance to set NXE from C. */
    movl $0x80000000, %eax
    cpuid
    xorl %ebx, %ebx
    cmpl $0x80000001, %eax
    jb 1f
    movl $0x80000001, %eax
    cpuid
    andl $(1 << 20), %edx
    movl %edx, %ebx
1:
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    testl %ebx, %ebx
    jz 2f
    orl $(1 << 11), %eax
2:
    wrmsr
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0
    ljmpl $0x18, $ap_64

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

/* Data filled by the BSP before SIPI.
 *
 * The BSP patches these fields by byte offset, and those offsets are GENERATED
 * from this file's symbol table (ap_trampoline_offsets.h) rather than written
 * out by hand in C — a hand-written copy is what broke when the code above grew
 * an NX probe: the block slid down, the BSP wrote CR3 and RSP into the middle
 * of this trampoline's own code, and every AP triple-faulted on its first SIPI.
 * The magic word is the runtime check that the blob and the generated offsets
 * came from the same build; adding or reordering fields here is now safe. */
tramp_magic: .quad 0x54524D50303031   /* "TRMP001" */
pml4_phys:  .quad 0
stack_ptr:  .quad 0
percpu_ptr: .quad 0
cpu_id:     .quad 0
gdt_ptr:    .word gdt_end - gdt - 1  /* limit */
            .quad gdt                /* base */
ready_flag: .long 0
ap_main_ptr: .quad 0

/* GDT with a real 32-bit code/data pair for the protected-mode phase (ap_32)
 * AND a 64-bit code segment for long mode (ap_64). The old GDT had only the
 * 64-bit (L=1, D=0) code segment at 0x08; running 32-bit code through it makes
 * the CPU decode ap_32 as 16-bit (D=0) and fault — which is why the AP
 * trampoline never actually worked. */
.align 8
gdt:    .quad 0x0000000000000000  /* 0x00 null */
        .quad 0x00CF9A000000FFFF  /* 0x08 32-bit code (G=1,D=1,L=0) */
        .quad 0x00CF92000000FFFF  /* 0x10 32-bit data */
        .quad 0x00AF9A000000FFFF  /* 0x18 64-bit code (G=1,D=0,L=1) */
gdt_end:
