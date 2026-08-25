#ifndef B1NIX_ARCH_X86_64_H
#define B1NIX_ARCH_X86_64_H

#include <b1nix/types.h>

struct interrupt_frame {
  u64 rax;
  u64 rbx;
  u64 rcx;
  u64 rdx;
  u64 rbp;
  u64 rdi;
  u64 rsi;
  u64 r8;
  u64 r9;
  u64 r10;
  u64 r11;
  u64 r12;
  u64 r13;
  u64 r14;
  u64 r15;
  u64 vector;
  u64 error_code;
  u64 rip;
  u64 cs;
  u64 rflags;
  u64 rsp;
  u64 ss;
} __attribute__((packed));

void arch_check_and_deliver_signals(struct interrupt_frame *frame);
u64 sys_sigreturn(struct interrupt_frame *frame);
void x86_pic_unmask(u8 irq);
void arch_backtrace(u64 rbp, u64 rip);
/* Ring-3 backtrace for a faulting task: the faulting instruction plus the
 * return addresses still on the user stack, each named by the mapping it falls
 * in. Printed on an unhandled fatal fault, and on a handled one under
 * b1nix.user-bt. */
struct interrupt_frame;
void arch_user_backtrace(struct interrupt_frame *frame);

#endif
