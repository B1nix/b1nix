#ifndef TINYUNIX_IO_H
#define TINYUNIX_IO_H

#include <tinyunix/types.h>

void outb(u16 port, u8 value);
u8 inb(u16 port);
void io_wait(void);

#endif
