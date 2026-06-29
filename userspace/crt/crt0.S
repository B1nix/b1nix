/* B1NIX Userspace C Runtime (crt0.S)
 *
 * Entry point for userspace ELF executables on x86_64-b1nix.
 * The kernel sets up:
 *   - RSP pointing to the initial stack (argc, argv, envp, auxv)
 *   - All registers cleared except RSP
 *
 * crt0 responsibilities:
 *   1. Align stack to 16 bytes
 *   2. Extract argc, argv, envp from stack
 *   3. Call _init() if present
 *   4. Call main(argc, argv, envp)
 *   5. Call exit() with main's return value
 */

#ifdef __x86_64__
#ifndef B1NIX_DYNAMIC
/* Weak references to libgcc's DWARF frame-registration entry points. They are
 * defined only when a program links libgcc (e.g. the GCC-built native
 * toolchain: cc1/gcc/as/ld). For clang-linked b1nix programs they resolve to 0
 * and the registration below is skipped. */
.weak __register_frame_info
.weak __EH_FRAME_BEGIN__

.section .bss
.align 16
.local __b1nix_eh_object
__b1nix_eh_object:
	.skip 48                        /* sizeof(struct object) in libgcc */
#endif

.section .text
.globl _start
.type _start, @function

_start:
	/* Stack layout at entry (top to bottom):
	 *   [auxv entries]  — auxiliary vector
	 *   [0]             — envp terminator
	 *   [envp[n-1]]     — environment pointers
	 *   ...
	 *   [envp[0]]
	 *   [0]             — argv terminator
	 *   [argv[argc-1]]  — argument pointers
	 *   ...
	 *   [argv[0]]
	 *   [argc]          — argument count
	 */

	/* Pop argc into %rdi (first argument to main) */
	popq %rdi

	/* argv = RSP after popping argc */
	movq %rsp, %rsi

	/* envp = argv + (argc + 1) * 8  (skip argc+1 pointers past argv terminator) */
	leaq 8(%rsp, %rdi, 8), %rdx

	/* Store envp in the global environ variable. In a statically linked
	 * binary `environ` is a local definition, reachable with a PC-relative
	 * store. In a dynamic binary it is imported from libc.so.1, so it must be
	 * reached through the GOT: environ@GOTPCREL emits an R_X86_64_GLOB_DAT the
	 * loader resolves to libc's own `environ`, and we store envp through that
	 * pointer. A plain environ(%rip) would reference a nonexistent local copy,
	 * leaving getenv()/environ broken for every dynamically linked program. */
#ifndef B1NIX_DYNAMIC
	movq %rdx, environ(%rip)
#else
	movq environ@GOTPCREL(%rip), %rax
	movq %rdx, (%rax)
#endif

	/* Align for C call ABI while preserving decoded argc/argv/envp. */
	andq $-16, %rsp

	/* Stash argc/argv/envp in callee-saved registers across the init calls
	 * below (constructors preserve these per the SysV ABI). */
	movq %rdi, %rbx                 /* argc */
	movq %rsi, %r12                 /* argv */
	movq %rdx, %r13                 /* envp */

#ifndef B1NIX_DYNAMIC
	/* Register the program's .eh_frame with libgcc so its DWARF unwinder can
	 * find FDEs. This is normally done by frame_dummy() called from _init,
	 * but the x86_64-b1nix target has no crti.o/crtn.o (so no callable _init)
	 * and crt0 runs only .init_array — leaving frame_dummy's call orphaned.
	 * Without this, any unwind in the GCC-built cc1 (a C++ throw, or the
	 * ICE crash-handler's backtrace) hits gcc_assert(code==_URC_NO_REASON)
	 * in uw_init_context_1 and executes ud2 (SIGILL). Replicate frame_dummy:
	 *   if (&__register_frame_info)
	 *     __register_frame_info(__EH_FRAME_BEGIN__, &__b1nix_eh_object);
	 * The weak NULL check makes this a no-op for clang-linked programs. */
	leaq __register_frame_info(%rip), %rax
	testq %rax, %rax
	jz 3f
	leaq __EH_FRAME_BEGIN__(%rip), %rdi
	leaq __b1nix_eh_object(%rip), %rsi
	call *%rax
3:
#endif

	/* M75: run shared-library constructors (their DT_INIT_ARRAY) before the
	 * executable's own __init_array. The kernel links dynamic binaries eagerly
	 * in-kernel and crt0 only walks the executable's __init_array, so a library's
	 * constructors (e.g. libLLVM.so's 458: X86 target + regalloc cl::opt seeding)
	 * would otherwise never run, aborting clang's backend. __b1nix_run_dso_init
	 * reads the kernel's AT_B1NIX_DSO_INIT descriptor table and is a no-op when
	 * absent (static binaries / no shared-lib ctors). argc/argv/envp stay live in
	 * rbx/r12/r13 (callee-saved) across the call. */
	movq %rbx, %rdi
	movq %r12, %rsi
	movq %r13, %rdx
	call __b1nix_run_dso_init

	/* Run ELF .init_array constructors before main(). C++ static
	 * constructors and __attribute__((constructor)) functions live here;
	 * skipping them leaves global state uninitialized (this is why the
	 * natively-built GCC's cc1 crashed in code generation). */
	leaq __init_array_start(%rip), %r14
	leaq __init_array_end(%rip), %r15
1:
	cmpq %r15, %r14
	jae 2f
	call *(%r14)
	addq $8, %r14
	jmp 1b
2:
	movq %rbx, %rdi
	movq %r12, %rsi
	movq %r13, %rdx

	/* Call main(argc, argv, envp) */
	call main

	/* Exit with main's return value */
	movl %eax, %edi
	call exit

	/* Should never reach here */
	hlt
	jmp .
#else
#ifndef B1NIX_DYNAMIC
/* Weak refs to libgcc's DWARF frame registration (see the x86_64 block). 0 for
 * clang-linked programs; real for libgcc-linked C++ binaries. */
.weak __register_frame_info
.weak __EH_FRAME_BEGIN__

.section .bss
.align 16
.local __b1nix_eh_object
__b1nix_eh_object:
	.skip 48                        /* sizeof(struct object) in libgcc */
#endif

.section .text
.globl _start
.type _start, @function

_start:
	/* Pop argc into %eax */
	popl %eax

	/* argv = ESP */
	movl %esp, %edx

	/* envp = argv + (argc + 1) * 4 */
	leal 4(%esp,%eax,4), %ecx

	/* Store envp in global environ variable */
	movl %ecx, environ

	/* Stash argc, argv, envp in callee-saved registers */
	movl %eax, %ebx                 /* argc */
	movl %edx, %esi                 /* argv */
	movl %ecx, %edi                 /* envp */

#ifndef B1NIX_DYNAMIC
	/* Register the program's .eh_frame with libgcc's DWARF unwinder so C++
	 * exceptions work (mirrors the x86_64 path; cdecl, args on the stack).
	 * No-op for clang binaries where __register_frame_info is weak-undefined. */
	movl $__register_frame_info, %eax
	testl %eax, %eax
	jz 3f
	pushl $__b1nix_eh_object
	pushl $__EH_FRAME_BEGIN__
	call __register_frame_info
	addl $8, %esp
3:
#endif

	/* Run ELF .init_array constructors before main(). */
	movl $__init_array_start, %ecx
	movl $__init_array_end, %edx
1:
	cmpl %edx, %ecx
	jae 2f
	subl $12, %esp
	pushl %edx
	pushl %ecx
	call *(%ecx)
	popl %ecx
	popl %edx
	addl $12, %esp
	addl $4, %ecx
	jmp 1b
2:
	/* Align stack to 16 bytes, then push arguments to main: main(argc, argv, envp) */
	andl $-16, %esp
	subl $4, %esp
	pushl %edi
	pushl %esi
	pushl %ebx
	call main

	/* Exit with main's return value */
	pushl %eax
	call exit

	/* Should never reach here */
	hlt
	jmp .
#endif

.size _start, . - _start
