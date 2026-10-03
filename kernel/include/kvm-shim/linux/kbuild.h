/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_LINUX_KBUILD_H
#define KVM_SHIM_LINUX_KBUILD_H
/* Kbuild's asm-offsets markers: compiled with -S, each becomes a line the
 * Makefile turns into a #define (the same "->NAME value" format). */
#define DEFINE(sym, val) \
	__asm__ volatile("\n.ascii \"->" #sym " %0 " #val "\"" : : "i" (val))
#define BLANK() __asm__ volatile("\n.ascii \"->\"" : : )
#define OFFSET(sym, str, mem) DEFINE(sym, __builtin_offsetof(struct str, mem))
#define COMMENT(x) __asm__ volatile("\n.ascii \"->#" x "\"")
#endif
