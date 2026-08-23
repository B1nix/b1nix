// AArch64 FP/SIMD context save & restore.
//
// The kernel itself is built -mgeneral-regs-only and never touches V0-V31, but
// USERSPACE does constantly: musl copies a struct stat out of the syscall
// buffer with `ldp q0,q1 / stp q0,q1`, memcpy/strlen use SIMD, and any float
// code obviously does. Until this existed, arch_fpu_save/arch_fpu_restore were
// empty stubs on this arch, so a context switch landing between a task's
// `ldp q0,q1` and its `stp q0,q1` handed it back another task's vector
// registers — struct stat came out as garbage, and OpenRC then walked a
// corrupted dentry/mode and branched through a wild pointer.
//
// Written in assembly on purpose: the C files cannot name V registers under
// -mgeneral-regs-only.
//
// The save area is struct task's fpu_state[528] (16-byte aligned), laid out as
// Linux's struct user_fpsimd_state: 32 * 16 bytes of Q registers, then FPSR,
// then FPCR, then 8 reserved bytes. That is deliberate — it is exactly what
// PTRACE_GETREGSET(NT_PRFPREG) hands a debugger, so ptrace copies this area out
// verbatim. FPSR/FPCR used to be skipped entirely, which both left a task's
// rounding mode and sticky exception flags at the mercy of whoever ran last and
// made the regset 512 bytes where every arm64 debugger expects 528.

// The kernel builds with -mgeneral-regs-only, which also disables FP/SIMD in
// the integrated assembler — re-enable it for this file only.
.arch armv8-a+fp+simd

.section .text

.global arch_fpu_save
.type arch_fpu_save, %function
arch_fpu_save:                  // x0 = 512-byte, 16-aligned save area
	cbz	x0, 1f
	stp	q0,  q1,  [x0, #16 * 0]
	stp	q2,  q3,  [x0, #16 * 2]
	stp	q4,  q5,  [x0, #16 * 4]
	stp	q6,  q7,  [x0, #16 * 6]
	stp	q8,  q9,  [x0, #16 * 8]
	stp	q10, q11, [x0, #16 * 10]
	stp	q12, q13, [x0, #16 * 12]
	stp	q14, q15, [x0, #16 * 14]
	stp	q16, q17, [x0, #16 * 16]
	stp	q18, q19, [x0, #16 * 18]
	stp	q20, q21, [x0, #16 * 20]
	stp	q22, q23, [x0, #16 * 22]
	stp	q24, q25, [x0, #16 * 24]
	stp	q26, q27, [x0, #16 * 26]
	stp	q28, q29, [x0, #16 * 28]
	stp	q30, q31, [x0, #16 * 30]
	mrs	x1, fpsr
	mrs	x2, fpcr
	str	w1, [x0, #512]
	str	w2, [x0, #516]
1:
	ret

.global arch_fpu_restore
.type arch_fpu_restore, %function
arch_fpu_restore:               // x0 = 512-byte, 16-aligned save area
	cbz	x0, 1f
	ldp	q0,  q1,  [x0, #16 * 0]
	ldp	q2,  q3,  [x0, #16 * 2]
	ldp	q4,  q5,  [x0, #16 * 4]
	ldp	q6,  q7,  [x0, #16 * 6]
	ldp	q8,  q9,  [x0, #16 * 8]
	ldp	q10, q11, [x0, #16 * 10]
	ldp	q12, q13, [x0, #16 * 12]
	ldp	q14, q15, [x0, #16 * 14]
	ldp	q16, q17, [x0, #16 * 16]
	ldp	q18, q19, [x0, #16 * 18]
	ldp	q20, q21, [x0, #16 * 20]
	ldp	q22, q23, [x0, #16 * 22]
	ldp	q24, q25, [x0, #16 * 24]
	ldp	q26, q27, [x0, #16 * 26]
	ldp	q28, q29, [x0, #16 * 28]
	ldp	q30, q31, [x0, #16 * 30]
	ldr	w1, [x0, #512]
	ldr	w2, [x0, #516]
	msr	fpsr, x1
	msr	fpcr, x2
1:
	ret

// Reset the LIVE FP/SIMD state to the ABI default a freshly exec'd image
// expects: all vectors zero, no sticky exception flags, default rounding.
.global arch_fpu_init_current
.type arch_fpu_init_current, %function
arch_fpu_init_current:
	movi	v0.2d,  #0
	movi	v1.2d,  #0
	movi	v2.2d,  #0
	movi	v3.2d,  #0
	movi	v4.2d,  #0
	movi	v5.2d,  #0
	movi	v6.2d,  #0
	movi	v7.2d,  #0
	movi	v8.2d,  #0
	movi	v9.2d,  #0
	movi	v10.2d, #0
	movi	v11.2d, #0
	movi	v12.2d, #0
	movi	v13.2d, #0
	movi	v14.2d, #0
	movi	v15.2d, #0
	movi	v16.2d, #0
	movi	v17.2d, #0
	movi	v18.2d, #0
	movi	v19.2d, #0
	movi	v20.2d, #0
	movi	v21.2d, #0
	movi	v22.2d, #0
	movi	v23.2d, #0
	movi	v24.2d, #0
	movi	v25.2d, #0
	movi	v26.2d, #0
	movi	v27.2d, #0
	movi	v28.2d, #0
	movi	v29.2d, #0
	movi	v30.2d, #0
	movi	v31.2d, #0
	msr	fpcr, xzr
	msr	fpsr, xzr
	ret

// Snapshot a clean (all-zero) FP image into `area` without disturbing the live
// registers — fork copies this into the child so it does not inherit a stale
// save area. Zeroed vectors are exactly the state arch_fpu_init_current sets.
.global arch_fpu_capture_clean
.type arch_fpu_capture_clean, %function
arch_fpu_capture_clean:         // x0 = 512-byte, 16-aligned save area
	cbz	x0, 2f
	mov	x1, #512
1:
	stp	xzr, xzr, [x0], #16
	subs	x1, x1, #16
	b.ne	1b
2:
	ret
