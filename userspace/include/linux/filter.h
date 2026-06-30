#ifndef _LINUX_FILTER_H
#define _LINUX_FILTER_H

/* Classic-BPF instruction + program (the subset seccomp uses). M63. */
#include <stdint.h>

struct sock_filter {
  uint16_t code;
  uint8_t jt;
  uint8_t jf;
  uint32_t k;
};

struct sock_fprog {
  uint16_t len;
  struct sock_filter *filter;
};

/* BPF opcode classes / fields (Linux ABI values). */
#define BPF_LD   0x00
#define BPF_LDX  0x01
#define BPF_ST   0x02
#define BPF_STX  0x03
#define BPF_ALU  0x04
#define BPF_JMP  0x05
#define BPF_RET  0x06
#define BPF_MISC 0x07

#define BPF_W   0x00
#define BPF_ABS 0x20
#define BPF_IMM 0x00
#define BPF_MEM 0x60
#define BPF_LEN 0x80

#define BPF_ADD 0x00
#define BPF_SUB 0x10
#define BPF_AND 0x50
#define BPF_OR  0x40

#define BPF_JA   0x00
#define BPF_JEQ  0x10
#define BPF_JGT  0x20
#define BPF_JGE  0x30
#define BPF_JSET 0x40

#define BPF_K 0x00
#define BPF_X 0x08
#define BPF_A 0x10

#define BPF_TAX 0x00
#define BPF_TXA 0x80

/* Statement / jump construction helpers (Linux <linux/filter.h>). */
#define BPF_STMT(code, k) { (uint16_t)(code), 0, 0, (uint32_t)(k) }
#define BPF_JUMP(code, k, jt, jf) \
  { (uint16_t)(code), (uint8_t)(jt), (uint8_t)(jf), (uint32_t)(k) }

#endif
