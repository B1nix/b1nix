#ifndef B1NIX_ARCH_X86_H
#define B1NIX_ARCH_X86_H

#include <b1nix/types.h>

struct interrupt_frame {
  union { u32 eax; u32 rax; };
  union { u32 ebx; u32 rbx; };
  union { u32 ecx; u32 rcx; };
  union { u32 edx; u32 rdx; };
  union { u32 ebp; u32 rbp; };
  union { u32 edi; u32 rdi; };
  union { u32 esi; u32 rsi; };
  u32 vector;
  u32 error_code;
  union { u32 eip; u32 rip; };
  u32 cs;
  union { u32 eflags; u32 rflags; };
  union { u32 esp; u32 rsp; };
  u32 ss;
} __attribute__((packed));

void arch_check_and_deliver_signals(struct interrupt_frame *frame);
u64 sys_sigreturn(struct interrupt_frame *frame);
void x86_pic_unmask(u8 irq);
void arch_backtrace(u64 rbp, u64 rip);

#endif
