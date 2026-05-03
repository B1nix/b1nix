#ifndef TINYUNIX_CONSOLE_H
#define TINYUNIX_CONSOLE_H

#include <tinyunix/types.h>

void console_init(void);
void console_putc(char ch);
void console_write(const char *text);
void console_write_hex32(u32 value);
void console_write_hex64(u64 value);

#endif
