/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SYSCALLS_H
#define LKPI_LINUX_SYSCALLS_H

/*
 * The system-call definition macros.
 *
 * b1nix has a system-call layer of its own, so an imported file's
 * SYSCALL_DEFINEn bodies are nothing any user can reach: they compile to static
 * functions no caller names, and the compiler drops them. What such a file is
 * imported for is the handlers those bodies call, which b1nix's own entry
 * points call instead (fs/quota/quota.c is the case: see
 * kernel/lkpi/fs_quotactl.c).
 */
#include <linux/compiler.h>

#define __LKPI_MAP0(m, ...)
#define __LKPI_MAP1(m, t, a, ...) m(t, a)
#define __LKPI_MAP2(m, t, a, ...) m(t, a), __LKPI_MAP1(m, __VA_ARGS__)
#define __LKPI_MAP3(m, t, a, ...) m(t, a), __LKPI_MAP2(m, __VA_ARGS__)
#define __LKPI_MAP4(m, t, a, ...) m(t, a), __LKPI_MAP3(m, __VA_ARGS__)
#define __LKPI_MAP5(m, t, a, ...) m(t, a), __LKPI_MAP4(m, __VA_ARGS__)
#define __LKPI_MAP6(m, t, a, ...) m(t, a), __LKPI_MAP5(m, __VA_ARGS__)
#define __LKPI_MAP(n, ...) __LKPI_MAP##n(__VA_ARGS__)
#define __LKPI_SC_DECL(t, a) t a

#define SYSCALL_DEFINE0(name) \
	static __maybe_unused long lkpi_unreachable_sys_##name(void)
#define __LKPI_SYSCALL_DEFINEx(n, name, ...) \
	static __maybe_unused long lkpi_unreachable_sys_##name( \
		__LKPI_MAP(n, __LKPI_SC_DECL, __VA_ARGS__))
#define SYSCALL_DEFINE1(name, ...) __LKPI_SYSCALL_DEFINEx(1, name, __VA_ARGS__)
#define SYSCALL_DEFINE2(name, ...) __LKPI_SYSCALL_DEFINEx(2, name, __VA_ARGS__)
#define SYSCALL_DEFINE3(name, ...) __LKPI_SYSCALL_DEFINEx(3, name, __VA_ARGS__)
#define SYSCALL_DEFINE4(name, ...) __LKPI_SYSCALL_DEFINEx(4, name, __VA_ARGS__)
#define SYSCALL_DEFINE5(name, ...) __LKPI_SYSCALL_DEFINEx(5, name, __VA_ARGS__)
#define SYSCALL_DEFINE6(name, ...) __LKPI_SYSCALL_DEFINEx(6, name, __VA_ARGS__)

#endif
