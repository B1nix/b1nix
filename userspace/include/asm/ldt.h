#ifndef B1NIX_U_ASM_LDT_H
#define B1NIX_U_ASM_LDT_H
/* x86 LDT / thread-area descriptor (Linux <asm/ldt.h>). Used by crashpad's
 * ptracer to read a thread's TLS segment (PTRACE_GET_THREAD_AREA). b1nix has no
 * ptrace; this only provides the struct layout so the code compiles. */
struct user_desc {
  unsigned int entry_number;
  unsigned int base_addr;
  unsigned int limit;
  unsigned int seg_32bit : 1;
  unsigned int contents : 2;
  unsigned int read_exec_only : 1;
  unsigned int limit_in_pages : 1;
  unsigned int seg_not_present : 1;
  unsigned int useable : 1;
#ifdef __x86_64__
  unsigned int lm : 1;
#endif
};

#define MODIFY_LDT_CONTENTS_DATA  0
#define MODIFY_LDT_CONTENTS_STACK 1
#define MODIFY_LDT_CONTENTS_CODE  2

#endif /* B1NIX_U_ASM_LDT_H */
