#ifndef B1NIX_SERIAL_H
#define B1NIX_SERIAL_H

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
void serial_port_putc(int idx, char ch);
char serial_port_getc(int idx);
int serial_port_has_data(int idx);

#endif
