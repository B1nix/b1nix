.section .text
.code32

/* void arch_fpu_save(void *area) */
.global arch_fpu_save
.type arch_fpu_save, @function
arch_fpu_save:
	movl 4(%esp), %eax
	fxsave (%eax)
	ret

/* void arch_fpu_restore(void *area) */
.global arch_fpu_restore
.type arch_fpu_restore, @function
arch_fpu_restore:
	movl 4(%esp), %eax
	fxrstor (%eax)
	ret

/* void arch_fpu_capture_clean(void *area) */
.global arch_fpu_capture_clean
.type arch_fpu_capture_clean, @function
arch_fpu_capture_clean:
	fninit
	ldmxcsr mxcsr_default
	movl 4(%esp), %eax
	fxsave (%eax)
	ret

/* void arch_fpu_init_current(void) */
.global arch_fpu_init_current
.type arch_fpu_init_current, @function
arch_fpu_init_current:
	fninit
	ldmxcsr mxcsr_default
	ret

.section .rodata
.align 4
mxcsr_default:
	.long 0x00001F80      /* default MXCSR: all FP exceptions masked */
