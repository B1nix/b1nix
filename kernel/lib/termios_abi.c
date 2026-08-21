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
