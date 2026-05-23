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

	/* Align for C call ABI while preserving decoded argc/argv/envp. */
	andq $-16, %rsp

	/* Call main(argc, argv, envp) */
	call main

	/* Exit with main's return value */
	movl %eax, %edi
	call exit

	/* Should never reach here */
	hlt
	jmp .

.size _start, . - _start
