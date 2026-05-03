#ifndef TINYUNIX_SERIAL_H
#define TINYUNIX_SERIAL_H

void serial_init(void);
void serial_putc(char ch);
void serial_write(const char *text);

#endif
