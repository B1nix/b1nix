#ifndef B1NIX_SERIAL_H
#define B1NIX_SERIAL_H

void serial_init(void);
void serial_putc(char ch);
char serial_getc(void);
int serial_has_data(void);
void serial_write(const char *text);

#endif
