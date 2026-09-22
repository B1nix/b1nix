/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The code the firmware jumps to when the machine comes back from S3 (M129).
 *
 * A sleep that the firmware can enter and the operating system cannot leave is
 * worse than no sleep at all, and this file is the whole difference. ACPI's
 * contract is blunt: the OS writes the physical address of REAL-MODE code into
 * the FACS waking vector, writes SLP_TYP|SLP_EN into PM1_CNT, and the machine
 * stops. On wake the firmware puts the processor back in the state it had at
 * power-on — real mode, paging off, no long mode, caches disabled, one CPU
 * running — and jumps here. Everything the kernel was is gone from the
 * processor and still present in memory; this brings the processor back to
 * where the memory expects it.
 *
 * Compiled as a flat binary like ap_trampoline.S and copied below 1 MiB, with
 * its data block patched by offsets generated from this file's own symbols
 * (s3_wakeup_offsets.h), for the reason written out there: hand-kept offsets
 * silently rot the moment the code above them grows.
 *
 * What it does, in order: 16-bit entry with a known stack, its own GDT,
 * protected mode, the kernel's CR3 and CR4 (PAE, and LA57 when the kernel runs
 * five-level), EFER (LME and NXE), CR0 with paging on and caches enabled, then
 * a far jump to 64-bit code that loads the kernel's own GDT and jumps to the C
 * resume path on a stack the suspend saved. It restores no general-purpose
 * register: the C side does that, from the frame s3_suspend_and_sleep pushed.
 */

/* One character per stage, straight to COM1.
 *
 * The resume path has no kernel, no console driver and no way to report a
 * failure except the byte it has already written: a machine that stops between
 * real mode and long mode leaves exactly as many characters as it managed, and
 * that is the difference between debugging this and guessing at it. Three bytes
 * on every resume is a price worth paying. */
.macro S3TRACE ch
    movw $0x3f8, %dx
    movb $\ch, %al
    outb %al, %dx
.endm

.global _start
_start:
    .code16
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ss
    movw $0x1000, %sp
    /* The firmware enters with CS set from the waking vector and DS undefined;
     * every reference below is absolute and this blob is identity-mapped, so
     * a flat DS is all that is needed. */
    movw %ax, %ds
    movw %ax, %es
    S3TRACE 'W'
    lgdtl (gdt_ptr)
    movl %cr0, %eax
    orl $1, %eax                 /* PE */
    movl %eax, %cr0
    ljmpl $0x08, $s3_32

s3_32:
    .code32
    S3TRACE '3'
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl (saved_cr3), %eax
    movl %eax, %cr3
    /* PAE, and LA57 where the kernel is running five-level paging: the CR3
     * above names a PML5 then, and a four-level walk of it would fault on the
     * first instruction fetched from the higher half. PGE is deliberately left
     * off, for the reason ap_trampoline.S spells out: this kernel uses bit 8
     * of a leaf entry as a software flag. */
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl (saved_la57), %ebx
    testl %ebx, %ebx
    jz 1f
    orl $(1 << 12), %eax
1:
    movl %eax, %cr4
    /* EFER: LME always, NXE when the processor has it — the kernel's page
     * tables carry bit 63 on every non-executable page, and a walk that meets
     * it with NXE clear is a reserved-bit fault rather than a mapping. */
    movl $0x80000000, %eax
    cpuid
    xorl %ebx, %ebx
    cmpl $0x80000001, %eax
    jb 2f
    movl $0x80000001, %eax
    cpuid
    andl $(1 << 20), %edx
    movl %edx, %ebx
2:
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax          /* LME */
    testl %ebx, %ebx
    jz 3f
    orl $(1 << 11), %eax         /* NXE */
3:
    wrmsr
    movl %cr0, %eax
    orl $0x80000000, %eax        /* PG */
    /* And caches on: the firmware hands the processor back with CD and NW set,
     * exactly as at reset, and a kernel running uncached is slow enough to look
     * like a hang. */
    andl $~((1 << 30) | (1 << 29)), %eax
    movl %eax, %cr0
    ljmpl $0x18, $s3_64

s3_64:
    .code64
    S3TRACE '6'
    xorq %rax, %rax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    /* The kernel's own descriptor tables, and the stack the suspend was on.
     * From here the processor is the processor the kernel remembers, and the C
     * side can restore the rest (the TSS, the segment MSRs, the devices). */
    lgdt (saved_gdtr)
    movq (saved_stack), %rsp
    xorq %rbp, %rbp
    movq (resume_entry), %rax
    jmpq *%rax
4:  cli
    hlt
    jmp 4b

/* Data patched by the suspend path before the sleep. The magic word is the
 * runtime check that the blob and the generated offsets came from one build. */
.align 8
wake_magic:   .quad 0x53335741414B45   /* "S3WAAKE" */
saved_cr3:    .quad 0
saved_stack:  .quad 0
resume_entry: .quad 0
saved_la57:   .quad 0
gdt_ptr:      .word gdt_end - gdt - 1
              .quad gdt
/* The kernel's GDTR, stored by sgdt before the sleep: a 2-byte limit and an
 * 8-byte base, which is what lgdt wants back. */
saved_gdtr:   .word 0
              .quad 0

.align 8
gdt:    .quad 0x0000000000000000  /* 0x00 null */
        .quad 0x00CF9A000000FFFF  /* 0x08 32-bit code */
        .quad 0x00CF92000000FFFF  /* 0x10 32-bit data */
        .quad 0x00AF9A000000FFFF  /* 0x18 64-bit code */
gdt_end:
