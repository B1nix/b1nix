#ifndef B1NIX_IO_H
#define B1NIX_IO_H

#include <b1nix/types.h>

void outb(u16 port, u8 value);
u8 inb(u16 port);
void io_wait(void);

#endif
