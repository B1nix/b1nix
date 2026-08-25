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
/* reboot(magic1, magic2, cmd, arg): Linux passes its command in arg2 as a magic
 * constant, b1nix's SYS_REBOOT takes its own command in arg0. */
#define LINUX_NR_REBOOT      169
/* The xattr calls: b1nix's handlers carry a trailing `nofollow` argument that
 * selects the follow-symlink (setxattr) or don't-follow (lsetxattr) variant,
 * which Linux encodes in the syscall number instead. The caller never supplies
 * that argument, so it must be set from the number rather than read off a
 * register that holds nothing in particular. */
#define LINUX_NR_SETXATTR     188
#define LINUX_NR_LSETXATTR    189
#define LINUX_NR_GETXATTR     191
#define LINUX_NR_LGETXATTR    192
#define LINUX_NR_LISTXATTR    194
#define LINUX_NR_LLISTXATTR   195
#define LINUX_NR_REMOVEXATTR  197
#define LINUX_NR_LREMOVEXATTR    198
#define LINUX_NR_SIGNALFD4       289

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

struct b1nix_stat; /* forward decl; defined in <b1nix/posix.h> */

/* Convert a b1nix stat result into the Linux x86_64 layout. */
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
