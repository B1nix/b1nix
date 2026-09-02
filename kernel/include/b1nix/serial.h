#ifndef B1NIX_SERIAL_H
#define B1NIX_SERIAL_H

#include <b1nix/types.h>

/* Number of UART lines the driver knows about: 0 = COM1 (console mirror),
 * 1 = COM2 (optional second line, probed at boot). */
#define SERIAL_NPORTS 2

void serial_init(void);
void serial_putc(char ch);
char serial_getc(void);
int serial_has_data(void);
void serial_write(const char *text);

/* Indexed multi-port API (M39 serial ttys). idx is 0-based (0=COM1, 1=COM2);
 * all calls are no-ops / return 0 for absent ports. */
int serial_port_present(int idx);

/* ── M109 line configuration (termios c_cflag on a real UART) ──
 * Until these existed, tcsetattr on /dev/ttySn changed the line discipline and
 * silently ignored every hardware attribute, so a tcgetattr afterwards
 * reported a baud rate the wire was not running at. These program the divisor
 * latch and the line-control register and read them back, so what termios
 * reports is what the UART is actually doing.
 *
 * baud must divide 115200; data_bits 5..8; parity 0=none 1=odd 2=even;
 * stop_bits 1 or 2. Returns 0, -ENODEV for an absent port, -EINVAL for a rate
 * the divisor cannot express (or any other unusable parameter), and -ENOSYS on
 * a controller whose line this kernel cannot reprogram at all.
 *
 * The last two are distinct on purpose: a rate this UART cannot produce is the
 * caller's error and tcsetattr must report it, while a controller with no
 * reachable divisor (PL011) is not an error in what was asked -- the rest of
 * the termios still applies. Collapsing both onto -1 made tcsetattr accept a
 * baud rate the 16550 refused. */
int serial_port_set_line(int idx, u32 baud, u8 data_bits, u8 parity,
                         u8 stop_bits);
/* What the divisor latch and LCR currently say, read back from the chip. Any
 * out pointer may be NULL. Returns 0, or -1 for an absent port. */
int serial_port_get_line(int idx, u32 *baud, u8 *data_bits, u8 *parity,
                         u8 *stop_bits);
/* Modem control (MCR, TIOCMGET/TIOCMSET's output half) and modem status (MSR,
 * its input half). Bits are the UART's own, not Linux's TIOCM_*; the tty layer
 * translates. get returns 0 for an absent port. */
u8 serial_port_get_mcr(int idx);
void serial_port_set_mcr(int idx, u8 mcr);
u8 serial_port_get_msr(int idx);
/* The I/O port this line lives at, for TIOCGSERIAL. 0 = absent. */
u16 serial_port_base(int idx);
void serial_port_putc(int idx, char ch);
char serial_port_getc(int idx);
int serial_port_has_data(int idx);

#endif
