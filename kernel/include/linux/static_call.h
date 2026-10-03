/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_STATIC_CALL_H
#define LKPI_LINUX_STATIC_CALL_H

/*
 * Static calls as what they are underneath: one function pointer per call
 * site name. Linux patches the call instruction in place; an indirect call
 * through a variable has the same meaning and costs a load. KVM routes every
 * vendor callback (kvm_x86_ops) through these.
 */
#define STATIC_CALL_KEY(name) __SCK__##name

#define DECLARE_STATIC_CALL(name, func) \
	extern typeof(func) *STATIC_CALL_KEY(name)
#define DEFINE_STATIC_CALL(name, func) \
	typeof(func) *STATIC_CALL_KEY(name) = (func)
#define DEFINE_STATIC_CALL_NULL(name, func) \
	typeof(func) *STATIC_CALL_KEY(name) = NULL
#define DEFINE_STATIC_CALL_RET0(name, func) \
	typeof(func) *STATIC_CALL_KEY(name) = (typeof(func) *)__static_call_return0
#define EXPORT_STATIC_CALL(name)
#define EXPORT_STATIC_CALL_GPL(name)
#define EXPORT_STATIC_CALL_TRAMP(name)
#define EXPORT_STATIC_CALL_TRAMP_GPL(name)

/* A static call with no target does nothing, as upstream's does: the call
 * site is patched to a no-op there, and callers rely on it -- KVM leaves its
 * optional vendor callbacks NULL and calls them unconditionally. Only void
 * callbacks are ever left unset (the value-returning ones default to
 * __static_call_return0), and the no-op ignores their arguments, which the
 * SysV calling convention makes well defined for this ABI. */
void __static_call_nop(void);
#define static_call(name) \
	(*(STATIC_CALL_KEY(name) ? STATIC_CALL_KEY(name) \
	                         : (typeof(STATIC_CALL_KEY(name)))__static_call_nop))
#define static_call_cond(name) static_call(name)
#define static_call_query(name) (STATIC_CALL_KEY(name))
#define static_call_update(name, func) \
	((void)(STATIC_CALL_KEY(name) = (typeof(STATIC_CALL_KEY(name)))(func)))

long __static_call_return0(void);

#endif
