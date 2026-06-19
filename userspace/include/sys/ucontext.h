#ifndef B1NIX_U_SYS_UCONTEXT_H
#define B1NIX_U_SYS_UCONTEXT_H

#ifdef __x86_64__
#define REG_RIP 16
#define REG_RBP 10
#define REG_RSP 15
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
