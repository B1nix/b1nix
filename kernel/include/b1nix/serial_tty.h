#ifndef B1NIX_SERIAL_TTY_H
#define B1NIX_SERIAL_TTY_H

#include <b1nix/types.h>

/* M39 serial tty devices (/dev/ttyS0 = COM1, /dev/ttyS1 = COM2). See
 * kernel/dev/serial_tty.c. */

void serial_tty_init(void);           /* after serial_init(); probes ports */
void serial_tty_register_nodes(void); /* create /dev/ttySn VFS nodes */
int serial_tty_open(int idx, int flags); /* returns fd or -errno */
int serial_tty_path_index(const char *resolved_path); /* -1 if not a ttySn */
int serial_tty_present(int idx);
int serial_tty_claimed(int idx); /* open handles exist: tty owns its UART RX */
void serial_tty_tick(void);      /* BSP timer tick: drain UART RX */

/* M39 self-test hooks. */
void serial_tty_test_inject(int idx, const char *buf, usize n);
usize serial_tty_fg_pgrp(int idx);

#endif
