/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_CMPXCHG_H
#define KVM_SHIM_ASM_CMPXCHG_H
#include <linux/atomic.h>
#ifndef arch_xchg
#define arch_xchg(ptr, v) __atomic_exchange_n((ptr), (v), __ATOMIC_SEQ_CST)
#endif
#endif
