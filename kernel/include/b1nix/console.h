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
/* Terminal output — /dev/console, /dev/tty and VT echo. Unlike console_putc,
 * these add no timestamp and no severity: they are not log records. */
void console_putc_raw(char ch);
void console_write_raw(const char *text);
void console_bust_lock(void);
/* Dump anything the console is still holding back because the kernel command
 * line has not been parsed yet. Crash paths only. */
void console_log_panic_flush(void);
/* Let other subsystems that bypass console_write() but still hit the shared
 * physical UART (e.g. tty write() -> serial_port_putc()) serialize a whole
 * buffer against it, so neither side's multi-byte output can land mid-way
 * through the other's. */
void console_lock_acquire_irqsave(u64 *flags);
void console_lock_release_irqrestore(u64 flags);
void console_write_hex32(u32 value);
void console_write_hex64(u64 value);
void console_write_dec(u64 value);


/* TIOCCONS: kernel console output copied to a terminal. push() is called from
 * console_write() under its lock and never sleeps; set() takes a reference on
 * the target and starts the draining thread on first use (0 clears it). */
struct vfs_node;
void console_redirect_push(const char *text);
int console_redirect_set(struct vfs_node *node);

#endif
