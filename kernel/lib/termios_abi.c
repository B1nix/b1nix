#include <string.h>

#include <b1nix/errno.h>
#include <b1nix/syscall.h>
#include <b1nix/termios_abi.h>

int tty_termios_copyout(void *user_arg, const struct b1nix_termios *t) {
  struct linux_termios lt;
  memset(&lt, 0, sizeof(lt));
  lt.c_iflag = t->c_iflag;
  lt.c_oflag = t->c_oflag;
  lt.c_cflag = t->c_cflag;
  lt.c_lflag = t->c_lflag;
  lt.c_line = 0; /* N_TTY; b1nix has no other line discipline */
  memcpy(lt.c_cc, t->c_cc, sizeof(lt.c_cc));
  if (!user_arg || syscall_copyout(user_arg, &lt, sizeof(lt)) < 0)
    return -EFAULT;
  return 0;
}

int tty_termios_copyin(struct b1nix_termios *t, const void *user_arg) {
  struct linux_termios lt;
  if (!user_arg || syscall_copyin(&lt, user_arg, sizeof(lt)) < 0)
    return -EFAULT;
  t->c_iflag = lt.c_iflag;
  t->c_oflag = lt.c_oflag;
  t->c_cflag = lt.c_cflag;
  t->c_lflag = lt.c_lflag;
  memcpy(t->c_cc, lt.c_cc, sizeof(lt.c_cc));
  return 0;
}

/* ── termios2 ───────────────────────────────────────────────────────────────
 *
 * The only thing termios2 adds is a pair of literal speed words. b1nix keeps a
 * line rate in exactly one place -- the CBAUD bits of c_cflag -- so that is
 * where the speeds are read from and written back to, rather than inventing a
 * second copy the drivers would never consult. */
#define TTY_CBAUD 0x100fu
#define TTY_BOTHER 0x1000u

/* Linux's CBAUD codes 0x00..0x0f, in the order the B<rate> constants run. */
static const u32 tty_baud_table[16] = {
    0,   50,   75,   110,  134,  150,  200,   300,
    600, 1200, 1800, 2400, 4800, 9600, 19200, 38400,
};

static u32 tty_cbaud_to_rate(u32 cflag) {
  u32 code = cflag & TTY_CBAUD;
  switch (code) {
  case 0x1001: return 57600;
  case 0x1002: return 115200;
  case 0x1003: return 230400;
  case 0x1004: return 460800;
  case 0x1005: return 500000;
  case 0x1006: return 576000;
  case 0x1007: return 921600;
  case 0x1008: return 1000000;
  default: break;
  }
  if (code < 16)
    return tty_baud_table[code];
  return 0; /* BOTHER, or a code this kernel has no rate for */
}

static u32 tty_rate_to_cbaud(u32 rate, int *exact) {
  *exact = 1;
  switch (rate) {
  case 57600:   return 0x1001;
  case 115200:  return 0x1002;
  case 230400:  return 0x1003;
  case 460800:  return 0x1004;
  case 500000:  return 0x1005;
  case 576000:  return 0x1006;
  case 921600:  return 0x1007;
  case 1000000: return 0x1008;
  default: break;
  }
  for (u32 i = 0; i < 16; i++)
    if (tty_baud_table[i] == rate)
      return i;
  *exact = 0;
  return TTY_BOTHER;
}

int tty_termios2_copyout(void *user_arg, const struct b1nix_termios *t) {
  struct linux_termios2 lt;
  memset(&lt, 0, sizeof(lt));
  lt.c_iflag = t->c_iflag;
  lt.c_oflag = t->c_oflag;
  lt.c_cflag = t->c_cflag;
  lt.c_lflag = t->c_lflag;
  lt.c_line = 0; /* N_TTY; b1nix has no other line discipline */
  memcpy(lt.c_cc, t->c_cc, sizeof(lt.c_cc));
  /* Both directions carry the one rate the line actually runs at. A terminal
   * with no rate (a pty, the virtual console) reports 0, which is what Linux
   * reports for B0 and what every caller treats as "not a serial line". */
  lt.c_ispeed = lt.c_ospeed = tty_cbaud_to_rate(t->c_cflag);
  if (!user_arg || syscall_copyout(user_arg, &lt, sizeof(lt)) < 0)
    return -EFAULT;
  return 0;
}

int tty_termios2_copyin(struct b1nix_termios *t, const void *user_arg) {
  struct linux_termios2 lt;
  if (!user_arg || syscall_copyin(&lt, user_arg, sizeof(lt)) < 0)
    return -EFAULT;
  t->c_iflag = lt.c_iflag;
  t->c_oflag = lt.c_oflag;
  t->c_lflag = lt.c_lflag;
  memcpy(t->c_cc, lt.c_cc, sizeof(lt.c_cc));

  u32 cflag = lt.c_cflag;
  /* BOTHER means "the rate is in c_ospeed, not in CBAUD". Fold it back into a
   * CBAUD code when one exists, because that is the only field the drivers
   * read; an arbitrary rate they cannot express stays BOTHER and the driver
   * refuses it, which is better than silently running at some other speed. */
  if ((cflag & TTY_CBAUD) == TTY_BOTHER && lt.c_ospeed) {
    int exact = 0;
    u32 code = tty_rate_to_cbaud(lt.c_ospeed, &exact);
    if (exact)
      cflag = (cflag & ~TTY_CBAUD) | code;
  }
  t->c_cflag = cflag;
  return 0;
}
