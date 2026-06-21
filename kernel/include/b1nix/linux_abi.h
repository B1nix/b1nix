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

/* Linux<->b1nix signal-number remap (b1nix uses different signo values, e.g.
 * Linux SIGUSR1=10 vs b1nix 19). Returns 0 if there is no equivalent. */
int linux_signo_to_b1nix(int lx);
int b1nix_signo_to_linux(int b);

/* Same remap applied to a sigset_t (translates set bit positions). */
u64 linux_sigset_to_b1nix(u64 lx);
u64 b1nix_sigset_to_linux(u64 b);

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
