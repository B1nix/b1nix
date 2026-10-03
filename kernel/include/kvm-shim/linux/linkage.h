/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_LINUX_LINKAGE_H
#define KVM_SHIM_LINUX_LINKAGE_H
/* Symbol annotations for KVM's assembly (vmx/vmenter.S), without objtool,
 * IBT or return thunks: a function is a global, aligned label with a size. */
#ifdef __ASSEMBLY__
#define SYM_L_GLOBAL .globl
#define SYM_L_LOCAL  .local
#define SYM_FUNC_START(name) \
	.globl name; .type name, @function; .p2align 4; name:
#define SYM_FUNC_START_LOCAL(name) \
	.type name, @function; .p2align 4; name:
#define SYM_FUNC_END(name) .size name, . - name
#define SYM_INNER_LABEL_ALIGN(name, linkage) \
	.p2align 4; linkage name; name:
#define SYM_INNER_LABEL(name, linkage) linkage name; name:
#define SYM_CODE_START(name) .globl name; .p2align 4; name:
#define SYM_CODE_END(name) .size name, . - name
#define RET ret
#define ENDBR
#define UNWIND_HINT_SAVE
#define UNWIND_HINT_RESTORE
#define UNWIND_HINT_EMPTY
#define UNWIND_HINT_FUNC
#define UNWIND_HINT_END_OF_STACK
#else
#define asmlinkage
#endif
#endif
