.section .text
.code32

/* void x86_user_jump(u32 entry, u32 stack, u32 argc, u32 argv);
 * Sets up segments, pushes iret frame, clears registers, and executes iret.
 */
.global x86_user_jump
.type x86_user_jump, @function
x86_user_jump:
	/* Fetch parameters from stack */
	movl 4(%esp), %eax   /* entry (EIP) */
	movl 8(%esp), %edx   /* stack (ESP) */

	/* Set up segment registers for Ring 3.
	 * User Data selector is 0x23 (entry 4 | 3).
	 * TLS selector is 0x33 (entry 6 | 3).
	 * We load FS with User Data selector (0x23) to clear kernel segment.
	 */
	movw $0x23, %cx
	movw %cx, %ds
	movw %cx, %es
	movw %cx, %fs

	/* Load GS with TLS selector (0x33) */
	movw $0x33, %cx
	movw %cx, %gs

	/* Push stack frame for iret: SS, ESP, EFLAGS, CS, EIP */
	pushl $0x23          /* SS */
	pushl %edx           /* ESP */

	/* EFLAGS with interrupts enabled (0x200) */
	pushfd
	popl %edx
	orl $0x200, %edx
	pushl %edx           /* EFLAGS */

	pushl $0x1B          /* CS (User Code 0x1B) */
	pushl %eax           /* EIP */

	/* Clear general-purpose registers to avoid leaking info */
	xorl %eax, %eax
	xorl %ebx, %ebx
	xorl %ecx, %ecx
	xorl %edx, %edx
	xorl %esi, %esi
	xorl %edi, %edi
	xorl %ebp, %ebp

	iret
