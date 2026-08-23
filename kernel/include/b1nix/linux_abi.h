#ifndef B1NIX_LINUX_ABI_H
#define B1NIX_LINUX_ABI_H

#include <b1nix/types.h>

/* M40 — Linux x86_64 ABI compatibility.
 *
 * The Linux x86_64 syscall convention is identical to b1nix's at the CPU level
 * (the `syscall` instruction; number in RAX; args RDI, RSI, RDX, R10, R8, R9;
 * return value in RAX as a value or -errno). Only the syscall NUMBERS (and some
 * struct/flag semantics) differ. This module maps a Linux x86_64 syscall number
 * to the matching b1nix native number so that a process tagged with the Linux
 * personality can reuse the existing b1nix sys_* handlers unchanged.
 *
 * It is intentionally a NUMBER translation, not a re-implementation: every
 * mapped call dispatches into the same handler a native b1nix process would hit.
 */

/* Sentinel for "no native equivalent" — the dispatcher returns -ENOSYS. */
#define LINUX_SYS_UNMAPPED ((u32)0xffffffffu)

/* Linux x86_64 syscall numbers whose result struct layout differs from b1nix's
 * and therefore needs a semantic translation, not just a number remap. */
#if defined(__aarch64__)
#define LINUX_NR_STAT       0xffff
#define LINUX_NR_FSTAT      80
#define LINUX_NR_LSTAT      0xffff
#define LINUX_NR_UNAME      160
#define LINUX_NR_GETDENTS64 61
#define LINUX_NR_ARCH_PRCTL 0xffff
#define LINUX_NR_RT_SIGACTION 134
#define LINUX_NR_RT_SIGPROCMASK 135
#define LINUX_NR_RT_SIGRETURN 139
#define LINUX_NR_KILL        129
#define LINUX_NR_TKILL       130
#define LINUX_NR_TGKILL      131
#define LINUX_NR_REBOOT      142
#define LINUX_NR_SETXATTR     5
#define LINUX_NR_LSETXATTR    6
#define LINUX_NR_GETXATTR     8
#define LINUX_NR_LGETXATTR    9
#define LINUX_NR_LISTXATTR    11
#define LINUX_NR_LLISTXATTR   12
#define LINUX_NR_REMOVEXATTR  14
#define LINUX_NR_LREMOVEXATTR 15
#define LINUX_NR_SIGNALFD4    74
#define LINUX_NR_OPEN         0xffff
#define LINUX_NR_FCNTL        25
#define LINUX_NR_SELECT       0xffff
#define LINUX_NR_PSELECT6     72
#define LINUX_NR_SETITIMER    103
#define LINUX_NR_GETITIMER    102
#define LINUX_NR_UTIMES       0xffff
#define LINUX_NR_UTIMENSAT    88
/* asm-generic 95 — the x86_64 number (247) was left here, so waitid(2) from an
 * aarch64 binary was routed as whatever 247 means on this ABI. */
#define LINUX_NR_WAITID       95
#define LINUX_NR_PRLIMIT64    261
#define LINUX_NR_SHMGET       194
#define LINUX_NR_RT_SIGTIMEDWAIT 137
#define LINUX_NR_RT_SIGSUSPEND 133
#define LINUX_NR_MQ_OPEN      180
#define LINUX_NR_MQ_UNLINK    181
#define LINUX_NR_MQ_TIMEDSEND 182
#define LINUX_NR_MQ_TIMEDRECEIVE 183
#define LINUX_NR_SETPRIORITY  140
#define LINUX_NR_GETPRIORITY  141
#define LINUX_NR_RT_SIGQUEUEINFO 138
#define LINUX_NR_TIMER_CREATE 107
#define LINUX_NR_SYSINFO      179
#else
#define LINUX_NR_STAT       4
#define LINUX_NR_FSTAT      5
#define LINUX_NR_LSTAT      6
#define LINUX_NR_UNAME      63
#define LINUX_NR_GETDENTS64 217
#define LINUX_NR_ARCH_PRCTL 158
#define LINUX_NR_RT_SIGACTION 13
#define LINUX_NR_RT_SIGPROCMASK 14
#define LINUX_NR_RT_SIGRETURN 15
#define LINUX_NR_KILL        62
#define LINUX_NR_TKILL       200
#define LINUX_NR_TGKILL      234
#define LINUX_NR_REBOOT      169
#define LINUX_NR_SETXATTR     188
#define LINUX_NR_LSETXATTR    189
#define LINUX_NR_GETXATTR     191
#define LINUX_NR_LGETXATTR    192
#define LINUX_NR_LISTXATTR    194
#define LINUX_NR_LLISTXATTR   195
#define LINUX_NR_REMOVEXATTR  197
#define LINUX_NR_LREMOVEXATTR 198
#define LINUX_NR_SIGNALFD4    289
#define LINUX_NR_OPEN         2
#define LINUX_NR_FCNTL        72
#define LINUX_NR_SELECT       23
#define LINUX_NR_PSELECT6     270
#define LINUX_NR_SETITIMER    38
#define LINUX_NR_GETITIMER    36
#define LINUX_NR_UTIMES       235
#define LINUX_NR_UTIMENSAT    280
#define LINUX_NR_WAITID       247
#define LINUX_NR_PRLIMIT64    302
#define LINUX_NR_SHMGET       29
#define LINUX_NR_RT_SIGTIMEDWAIT 128
#define LINUX_NR_RT_SIGSUSPEND 130
#define LINUX_NR_MQ_OPEN      240
#define LINUX_NR_MQ_UNLINK    241
#define LINUX_NR_MQ_TIMEDSEND 242
#define LINUX_NR_MQ_TIMEDRECEIVE 243
#define LINUX_NR_SETPRIORITY  141
#define LINUX_NR_GETPRIORITY  140
#define LINUX_NR_RT_SIGQUEUEINFO 129
#define LINUX_NR_TIMER_CREATE 222
#define LINUX_NR_SYSINFO      99
#endif

/* reboot(2) command constants (linux/reboot.h). */
#define LINUX_REBOOT_CMD_RESTART   0x01234567u
#define LINUX_REBOOT_CMD_HALT      0xcdef0123u
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321fedcu
/* Ctrl-Alt-Del handling. On Linux these only flip a flag — the machine keeps
 * running — and PID 1 calls DISABLE_CAD at startup (runit, openrc-init,
 * sysvinit all do). b1nix has no CAD path at all, so both are accepted no-ops;
 * rejecting them made every init see EINVAL from its first syscall. */
#define LINUX_REBOOT_CMD_CAD_OFF   0x00000000u
#define LINUX_REBOOT_CMD_CAD_ON    0x89abcdefu

/* Linux<->b1nix signal-number remap (b1nix uses different signo values, e.g.
 * Linux SIGUSR1=10 vs b1nix 19). Returns 0 if there is no equivalent. */
int linux_signo_to_b1nix(int lx);
int b1nix_signo_to_linux(int b);

/* Same remap applied to a sigset_t (translates set bit positions). */
u64 linux_sigset_to_b1nix(u64 lx);
u64 b1nix_sigset_to_linux(u64 b);

/* Linux x86_64 siginfo_t (128 bytes). Only the fields a SI_USER (kill) handler
 * commonly reads are named; si_pid/si_uid sit in the union at offset 16. */
struct linux_siginfo {
  int si_signo; /* 0 */
  int si_errno; /* 4 */
  int si_code;  /* 8 */
  int _pad0;    /* 12 */
  int si_pid;   /* 16 (SI_USER) */
  int si_uid;   /* 20 */
  /* For SI_QUEUE/SI_TIMER the _sigfields._rt union places si_value here, so an
   * SA_SIGINFO handler reads info->si_value.sival_int/ptr at this offset. */
  long si_value; /* 24 (union sigval, 8 bytes) */
  unsigned char _pad[96]; /* -> 128 */
};

/* Linux x86_64 ucontext_t. uc_mcontext.gregs starts at offset 40 and holds the
 * 23 registers in REG_* order (R8..CR2). fpregs is left NULL. */
struct linux_ucontext {
  u64 uc_flags;            /* 0 */
  u64 uc_link;             /* 8 */
  u64 uc_stack_ss_sp;      /* 16 */
  u32 uc_stack_ss_flags;   /* 24 */
  u32 _ucpad;              /* 28 */
  u64 uc_stack_ss_size;    /* 32 */
  u64 gregs[23];           /* 40 (uc_mcontext) */
  u64 fpregs;              /* 224 (NULL) */
  u64 __reserved[8];       /* 232 */
  u64 uc_sigmask;          /* 296 (kernel sigset) */
  unsigned char _sigpad[120];
  /* 424: __fpregs_mem, the floating-point state a ucontext_t carries INSIDE
   * itself.
   *
   * uc_mcontext.fpregs is a pointer, so it is tempting to leave the space out
   * when the pointer is null -- and that is what was done. But the C library's
   * ucontext_t is 936 bytes on x86_64 and this tail is part of the object, not
   * something hanging off it: a handler that copies its ucontext, or that calls
   * into getcontext/swapcontext with it, reads and writes the whole 936. The
   * kernel reserved 424 and then placed the siginfo immediately above, so the
   * rest of the object overlapped the siginfo, the red zone and the interrupted
   * frame -- including its stack canary. Reserving what the ABI says the object
   * is costs half a kilobyte of stack and removes the overlap. */
  unsigned char __fpregs_mem[512];
};

/* Linux aarch64's ucontext_t, which has nothing in common with the x86_64 one
 * above. uc_mcontext sits at offset 176 (the kernel pads uc_sigmask out to the
 * 1024-bit set glibc declares, and the 16-byte alignment of struct sigcontext
 * adds the final 8), and the 4 KiB reserved tail carries a chain of
 * _aarch64_ctx records — the FPSIMD one first, then a zero terminator. A crash
 * reporter reads exactly these fields out of the crashing process's memory. */
struct linux_aarch64_ctx_head {
  u32 magic;
  u32 size;
};

#define LX_FPSIMD_MAGIC 0x46508001u

struct linux_fpsimd_context {
  struct linux_aarch64_ctx_head head; /* 0 */
  u32 fpsr;                           /* 8 */
  u32 fpcr;                           /* 12 */
  u8 vregs[512];                      /* 16 — V0-V31, 16 bytes each */
};

struct linux_sigcontext_aarch64 {
  u64 fault_address; /* 0 */
  u64 regs[31];      /* 8 */
  u64 sp;            /* 256 */
  u64 pc;            /* 264 */
  u64 pstate;        /* 272 */
  u8 __reserved[4096] __attribute__((aligned(16))); /* 280 */
};

struct linux_ucontext_aarch64 {
  u64 uc_flags;          /* 0 */
  u64 uc_link;           /* 8 */
  u64 uc_stack_ss_sp;    /* 16 */
  u32 uc_stack_ss_flags; /* 24 */
  u32 _ucpad;            /* 28 */
  u64 uc_stack_ss_size;  /* 32 */
  u64 uc_sigmask;        /* 40 — kernel sigset_t (64 bits) */
  u8 _sigpad[120];       /* 48 — padded out to glibc's 1024-bit set */
  u64 _align_pad;        /* 168 — struct sigcontext is 16-byte aligned */
  struct linux_sigcontext_aarch64 uc_mcontext; /* 176 */
};

/* gregs[] indices (Linux REG_* enum). */
enum {
  LX_REG_R8 = 0, LX_REG_R9, LX_REG_R10, LX_REG_R11, LX_REG_R12, LX_REG_R13,
  LX_REG_R14, LX_REG_R15, LX_REG_RDI, LX_REG_RSI, LX_REG_RBP, LX_REG_RBX,
  LX_REG_RDX, LX_REG_RAX, LX_REG_RCX, LX_REG_RSP, LX_REG_RIP, LX_REG_EFL,
  LX_REG_CSGSFS, LX_REG_ERR, LX_REG_TRAPNO, LX_REG_OLDMASK, LX_REG_CR2
};

/* arch_prctl options (asm/prctl.h). */
#define LINUX_ARCH_SET_FS 0x1002
#define LINUX_ARCH_GET_FS 0x1003

/* Linux `struct linux_dirent64` — variable-length, packed (d_name starts at
 * byte 19). getdents64 writes a run of these into the user buffer and returns
 * the number of bytes written. b1nix's `struct dirent` is fixed-size with the
 * name first, so the buffer must be repacked. */
struct linux_dirent64 {
  u64 d_ino;
  i64 d_off;
  u16 d_reclen;
  u8 d_type;
  char d_name[]; /* NUL-terminated */
} __attribute__((packed));

/* Linux x86_64 `struct stat` (arch/x86/include/uapi/asm/stat.h). Field order and
 * widths differ from b1nix's `struct b1nix_stat` (e.g. Linux has a 64-bit
 * st_nlink before a 32-bit st_mode, then a __pad0), so a Linux binary that reads
 * st_mode/st_size off a b1nix-filled buffer gets the wrong bytes. 144 bytes. */
/* Linux `struct stat`. The layout is per-arch: x86_64 has its own 144-byte
 * ABI struct, while every asm-generic port (aarch64 among them) uses the
 * 128-byte generic one with a different field ORDER as well — st_mode/st_nlink
 * are 32-bit and swapped, and the trailing padding is 2 ints, not 3 longs.
 * Copying the x86_64 shape out to an aarch64 process overran its 128-byte
 * buffer by 16 bytes, which on musl's own stack frame is exactly the saved
 * x30/x19 — every fstatat returned to address 0. */
#if defined(__aarch64__)
struct linux_stat {
  u64 st_dev;
  u64 st_ino;
  u32 st_mode;
  u32 st_nlink;
  u32 st_uid;
  u32 st_gid;
  u64 st_rdev;
  u64 __pad1;
  i64 st_size;
  i32 st_blksize;
  i32 __pad2;
  i64 st_blocks;
  i64 st_atime;
  u64 st_atime_nsec;
  i64 st_mtime;
  u64 st_mtime_nsec;
  i64 st_ctime;
  u64 st_ctime_nsec;
  u32 __unused[2];
};
#else
struct linux_stat {
  u64 st_dev;
  u64 st_ino;
  u64 st_nlink;
  u32 st_mode;
  u32 st_uid;
  u32 st_gid;
  u32 __pad0;
  u64 st_rdev;
  i64 st_size;
  i64 st_blksize;
  i64 st_blocks;
  u64 st_atime;
  u64 st_atime_nsec;
  u64 st_mtime;
  u64 st_mtime_nsec;
  u64 st_ctime;
  u64 st_ctime_nsec;
  i64 __unused[3];
};
#endif

struct b1nix_stat; /* forward decl; defined in <b1nix/posix.h> */

/* Convert a b1nix stat result into this arch's Linux stat layout. */
void linux_stat_from_b1nix(struct linux_stat *out, const struct b1nix_stat *in);

/* Linux `struct utsname` (new_utsname): six NUL-terminated 65-byte fields,
 * 390 bytes. b1nix's `struct b1nix_utsname` uses 32-byte fields and has no
 * domainname, so a Linux binary needs the wider layout. */
struct linux_utsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
};

struct b1nix_utsname; /* forward decl; defined in <b1nix/posix.h> */

/* Convert a b1nix uname result into the Linux layout (domainname = "(none)"). */
void linux_utsname_from_b1nix(struct linux_utsname *out,
                              const struct b1nix_utsname *in);

/* Translate a Linux x86_64 syscall number to the b1nix native number.
 * Returns LINUX_SYS_UNMAPPED if there is no supported native equivalent. */
u32 linux_syscall_to_b1nix(u64 linux_nr);

/* Human-readable name for a Linux syscall number (for diagnostics / unmapped
 * logging). Returns "?" for unknown numbers. */
const char *linux_syscall_name(u64 linux_nr);

#endif
