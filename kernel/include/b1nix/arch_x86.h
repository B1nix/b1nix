#ifndef B1NIX_ARCH_X86_H
#define B1NIX_ARCH_X86_H

#include <b1nix/types.h>

struct interrupt_frame32 {
  u32 eax;
  u32 ebx;
  u32 ecx;
  u32 edx;
  u32 ebp;
  u32 edi;
  u32 esi;
  u32 vector;
  u32 error_code;
  u32 eip;
  u32 cs;
  u32 eflags;
  u32 esp;
  u32 ss;
} __attribute__((packed));

#endif
