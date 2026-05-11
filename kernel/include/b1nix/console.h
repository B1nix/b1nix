#ifndef B1NIX_CONSOLE_H
#define B1NIX_CONSOLE_H

#include <b1nix/posix.h>
#include <b1nix/types.h>

struct console_state {
  usize fg_pgrp;
  struct b1nix_termios termios;
};

extern struct console_state console;

void console_init(void);
void console_clear(void);
void console_putc(char ch);
void console_write(const char *text);
void console_write_hex32(u32 value);
void console_write_hex64(u64 value);
void console_write_dec(u64 value);

#endif
