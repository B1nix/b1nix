/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_THREAD_INFO_H
#define KVM_SHIM_ARM64_ASM_THREAD_INFO_H
/*
 * The thread flags KVM's FP/SIMD handling tests. b1nix keeps a task's FP
 * state in the task and reloads it itself; TIF_FOREIGN_FPSTATE is the one
 * flag with a meaning here -- this CPU's FP registers do not hold the current
 * task's state (KVM put a guest's there, or saved and flushed them) -- and
 * the arm64 KVM glue keeps it per CPU.
 */
#include <linux/types.h>
#define TIF_SIGPENDING		0
#define TIF_NEED_RESCHED	1
#define TIF_NOTIFY_RESUME	2
#define TIF_FOREIGN_FPSTATE	3
#define TIF_32BIT		22
#define TIF_SVE			23
#define TIF_SME			27

#ifndef __ASSEMBLY__
bool test_thread_flag(int flag);
void set_thread_flag(int flag);
void clear_thread_flag(int flag);
#endif
#endif
