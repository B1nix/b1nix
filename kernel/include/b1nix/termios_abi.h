#ifndef B1NIX_TERMIOS_ABI_H
#define B1NIX_TERMIOS_ABI_H

#include <b1nix/posix.h>
#include <b1nix/types.h>

/* The termios layout a tty ioctl uses ON THE WIRE.
 *
 * This is Linux's `struct termios` as the kernel defines it — four flag words,
 * the line discipline byte, and NCCS=19 control characters, 36 bytes in all.
 * It is deliberately NOT struct b1nix_termios (48 bytes, no c_line): that is
 * the kernel's internal form, and handing it to userspace wrote 12 bytes past
 * the end of the buffer glibc passes to TCGETS, which is a 36-byte
 * `struct __kernel_termios`. The result was "*** stack smashing detected ***"
 * from every Debian coreutil that so much as checked whether stdout was a
 * terminal.
 *
 * Every tty driver converts through these two helpers, so there is one answer
 * to "what does a termios look like to userspace" instead of one per driver. */
struct linux_termios {
  u32 c_iflag;
  u32 c_oflag;
  u32 c_cflag;
  u32 c_lflag;
  u8 c_line;
  u8 c_cc[19];
};

/* Linux's `struct termios2`: the same 36 bytes with an input and an output
 * speed appended, 44 in all. It is what TCGETS2/TCSETS2 carry, and since glibc
 * 2.42 it is what tcgetattr(3) and tcsetattr(3) use on every call -- so a
 * kernel that answers only TCGETS has no terminals as far as a current glibc
 * is concerned. */
struct linux_termios2 {
  u32 c_iflag;
  u32 c_oflag;
  u32 c_cflag;
  u32 c_lflag;
  u8 c_line;
  u8 c_cc[19];
  u32 c_ispeed;
  u32 c_ospeed;
};

/* Copy the kernel's termios out to a user buffer in the wire layout. */
int tty_termios_copyout(void *user_arg, const struct b1nix_termios *t);

/* Read a user termios in the wire layout into the kernel's form. Control
 * characters past NCCS keep the value the kernel already had. */
int tty_termios_copyin(struct b1nix_termios *t, const void *user_arg);

/* The same pair for termios2. The speeds are derived from, and folded back
 * into, the CBAUD bits of c_cflag, because that is the only place b1nix's
 * drivers keep a line rate. */
int tty_termios2_copyout(void *user_arg, const struct b1nix_termios *t);
int tty_termios2_copyin(struct b1nix_termios *t, const void *user_arg);

/* Does this ioctl request belong to the termios2 family? Every tty driver asks
 * the same question, so it is answered in one place. */
static inline int tty_is_termios2_get(u64 request) {
  return request == B1NIX_TCGETS2;
}
static inline int tty_is_termios2_set(u64 request) {
  return request == B1NIX_TCSETS2 || request == B1NIX_TCSETSW2 ||
         request == B1NIX_TCSETSF2;
}

#endif
