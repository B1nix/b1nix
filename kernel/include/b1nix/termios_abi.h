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

/* Copy the kernel's termios out to a user buffer in the wire layout. */
int tty_termios_copyout(void *user_arg, const struct b1nix_termios *t);

/* Read a user termios in the wire layout into the kernel's form. Control
 * characters past NCCS keep the value the kernel already had. */
int tty_termios_copyin(struct b1nix_termios *t, const void *user_arg);

#endif
