#ifndef B1NIX_U_SYS_UCONTEXT_H
#define B1NIX_U_SYS_UCONTEXT_H

#ifdef __x86_64__
/* gregset_t indices, Linux x86_64 layout. Chromium's thread_delegate_posix and
 * the crash/profiling code read gregs[REG_*]; full set so they compile. */
#define REG_R8  0
#define REG_R9  1
#define REG_R10 2
#define REG_R11 3
#define REG_R12 4
#define REG_R13 5
#define REG_R14 6
#define REG_R15 7
#define REG_RDI 8
#define REG_RSI 9
#define REG_RBP 10
#define REG_RBX 11
#define REG_RDX 12
#define REG_RAX 13
#define REG_RCX 14
#define REG_RSP 15
#define REG_RIP 16
#define REG_EFL 17
#define REG_CSGSFS 18
#define REG_ERR 19
#define REG_TRAPNO 20
#define REG_OLDMASK 21
#define REG_CR2 22
#else
#define REG_EIP 14
#define REG_EBP 6
#define REG_ESP 7
#endif

typedef struct mcontext {
    long gregs[23];
} mcontext_t;

typedef struct ucontext {
    unsigned long uc_flags;
    struct ucontext *uc_link;
    char *uc_stack;
    mcontext_t uc_mcontext;
} ucontext_t;

#endif
