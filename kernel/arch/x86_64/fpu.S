/* kernel/arch/x86_64/fpu.S — x86_64 FPU/SSE state helpers.
 *
 * The kernel itself is built with -mno-sse and never touches the FPU, but
 * userspace (e.g. the natively-ported GCC's cc1) uses SSE/x87 heavily. The
 * kernel must therefore save/restore the FPU+SSE register file (x87, MXCSR,
 * XMM0-15) across context switches and start each new program with a clean
 * FPU state. fxsave/fxrstor cannot be emitted from -mno-sse C, so the actual
 * instructions live here.
 *
 * The 512-byte areas passed in %rdi MUST be 16-byte aligned (fxsave/fxrstor
 * #GP otherwise). Callers guarantee this (struct task FXSAVE area is aligned). */

	.section .text

	/* void arch_fpu_save(void *area) — save current FPU/SSE state to area. */
	.global arch_fpu_save
	.type arch_fpu_save, @function
arch_fpu_save:
	fxsave64 (%rdi)
	ret

	/* void arch_fpu_restore(void *area) — load FPU/SSE state from area. */
	.global arch_fpu_restore
	.type arch_fpu_restore, @function
arch_fpu_restore:
	fxrstor64 (%rdi)
	ret

	/* void arch_fpu_capture_clean(void *area) — reset the live FPU to a
	 * clean ABI default (FCW=0x037F via fninit, MXCSR=0x1F80) and snapshot it
	 * into area, producing a canonical image to load into never-run tasks.
	 * NOTE: this modifies the live FPU; callers must have already saved any
	 * state they care about. */
	.global arch_fpu_capture_clean
	.type arch_fpu_capture_clean, @function
arch_fpu_capture_clean:
	fninit
	ldmxcsr mxcsr_default(%rip)
	fxsave64 (%rdi)
	ret

	/* void arch_fpu_init_current(void) — reset the live FPU to the clean ABI
	 * default. Used on execve so a fresh program starts with a defined FPU. */
	.global arch_fpu_init_current
	.type arch_fpu_init_current, @function
arch_fpu_init_current:
	fninit
	ldmxcsr mxcsr_default(%rip)
	ret

	.section .rodata
	.align 4
mxcsr_default:
	.long 0x00001F80      /* default MXCSR: all FP exceptions masked */
