/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_PKEYS_H
#define B1NIX_PKEYS_H
/* Memory protection keys: pkey_alloc(2), pkey_free(2), pkey_mprotect(2).
 * Implemented by the architecture (x86 PKU, kernel/arch/x86_64/pkeys.c); an
 * architecture without the hardware answers as a CPU without it. */
#include <b1nix/types.h>

struct task;

#define PKEY_DISABLE_ACCESS 0x1
#define PKEY_DISABLE_WRITE  0x2

int arch_pkeys_enabled(void);
int arch_pkey_alloc(u32 rights);
int arch_pkey_free(int pkey);
int arch_pkey_is_allocated(int pkey);
int arch_execute_only_pkey(void);
int arch_pkey_is_exec_only(int pkey);
/* May the current thread read (write) a user page that carries `pkey`? */
int arch_pkey_allows(int pkey, int write);

/* The thread's PKRU as its user code sees it. */
u32 arch_pkru_user_get(void);
void arch_pkru_user_set(u32 value);
/* A kernel-mode access to a user page tripped on a key (a copy racing another
 * thread's pkey_mprotect): clear the keys for the rest of the kernel's work.
 * Returns 1 when that makes the access retryable. */
int arch_pkru_kernel_fault(void);
/* Put back a value arch_pkru_kernel_fault cleared; on every return to user. */
void arch_pkru_return_to_user(void);
void arch_pkru_switch(struct task *prev, struct task *next);
void arch_pkru_fork(struct task *parent, struct task *child);
void arch_pkeys_exec(void);
/* Load the rights a signal handler starts with. */
void arch_pkeys_signal_rights(void);
void arch_pkeys_mm_clone(u64 parent_root, u64 child_root);
void arch_pkeys_mm_release(u64 root);
#endif
