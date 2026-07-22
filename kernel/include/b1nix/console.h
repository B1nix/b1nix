#ifndef B1NIX_CONSOLE_H
#define B1NIX_CONSOLE_H

#include <b1nix/posix.h>
#include <b1nix/types.h>

struct console_state {
  usize fg_pgrp;
  usize session_id;
  struct b1nix_termios termios;
};

extern struct console_state console;

void console_init(void);
void console_clear(void);
void console_putc(char ch);
void console_write(const char *text);
void console_bust_lock(void);
/* Let other subsystems that bypass console_write() but still hit the shared
 * physical UART (e.g. tty write() -> serial_port_putc()) serialize a whole
 * buffer against it, so neither side's multi-byte output can land mid-way
 * through the other's. */
void console_lock_acquire_irqsave(u64 *flags);
void console_lock_release_irqrestore(u64 flags);
void console_write_hex32(u32 value);
void console_write_hex64(u64 value);
void console_write_dec(u64 value);

#endif
