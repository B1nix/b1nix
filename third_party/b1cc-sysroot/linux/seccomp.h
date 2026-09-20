#ifndef _LINUX_SECCOMP_H
#define _LINUX_SECCOMP_H

/* seccomp-bpf constants + the seccomp_data filter input (M63). */
#include <stdint.h>

/* seccomp() operations + prctl modes. */
#define SECCOMP_MODE_DISABLED 0
#define SECCOMP_MODE_STRICT   1
#define SECCOMP_MODE_FILTER   2

#define SECCOMP_SET_MODE_STRICT 0
#define SECCOMP_SET_MODE_FILTER 1

/* Filter return actions (high 16 bits) + data (low 16). */
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#define SECCOMP_RET_KILL_THREAD  0x00000000U
#define SECCOMP_RET_KILL         SECCOMP_RET_KILL_THREAD
#define SECCOMP_RET_TRAP         0x00030000U
#define SECCOMP_RET_ERRNO        0x00050000U
#define SECCOMP_RET_TRACE        0x7ff00000U
#define SECCOMP_RET_LOG          0x7ffc0000U
#define SECCOMP_RET_ALLOW        0x7fff0000U
#define SECCOMP_RET_DATA         0x0000ffffU

/* AUDIT_ARCH values for seccomp_data.arch. */
#define AUDIT_ARCH_X86_64 0xC000003EU
#define AUDIT_ARCH_I386   0x40000003U

struct seccomp_data {
  int nr;
  uint32_t arch;
  uint64_t instruction_pointer;
  uint64_t args[6];
};

/* libc wrapper (declared in <unistd.h> too). */
int seccomp(unsigned int op, unsigned int flags, void *args);

#endif
