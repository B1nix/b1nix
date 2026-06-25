#ifndef B1NIX_U_SYS_USER_H
#define B1NIX_U_SYS_USER_H

/* ptrace register layouts (Linux ABI). Used by crashpad's Linux capture code
 * (PTRACE_GETREGS / GETFPREGS). b1nix doesn't implement ptrace, so these only
 * provide the struct layouts so that code compiles. */

#ifdef __x86_64__

struct user_regs_struct {
  unsigned long long r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
  unsigned long long rax, rcx, rdx, rsi, rdi, orig_rax, rip, cs, eflags;
  unsigned long long rsp, ss, fs_base, gs_base, ds, es, fs, gs;
};

struct user_fpregs_struct {
  unsigned short cwd, swd, ftw, fop;
  unsigned long long rip, rdp;
  unsigned int mxcsr, mxcr_mask;
  unsigned int st_space[32];   /* 8 * 16 bytes for the x87 stack */
  unsigned int xmm_space[64];  /* 16 * 16 bytes for the SSE regs */
  unsigned int padding[24];
};

struct user {
  struct user_regs_struct regs;
  int u_fpvalid;
  struct user_fpregs_struct i387;
  unsigned long int u_tsize, u_dsize, u_ssize;
  unsigned long long start_code, start_stack;
  long long signal;
  int reserved;
  struct user_regs_struct* u_ar0;
  struct user_fpregs_struct* u_fpstate;
  unsigned long long magic;
  char u_comm[32];
  unsigned long long u_debugreg[8];
};

#else  /* i386 (frozen port) */

struct user_regs_struct {
  long ebx, ecx, edx, esi, edi, ebp, eax, xds, xes, xfs, xgs;
  long orig_eax, eip, xcs, eflags, esp, xss;
};
struct user_fpregs_struct {
  long cwd, swd, twd, fip, fcs, foo, fos;
  long st_space[20];
};

#endif

#endif /* B1NIX_U_SYS_USER_H */
