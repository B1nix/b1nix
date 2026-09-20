/* SPDX-License-Identifier: GPL-2.0-only */
#include <b1nix/io.h>

void outb(u16 port, u8 value)
{
	__asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* A run of bytes to one port as a single string instruction: under a
 * hypervisor that is one exit for the run, where a loop of outb is one per
 * byte. */
void outsb(u16 port, const u8 *buf, u32 count)
{
	u64 left = count;
	const u8 *src = buf;

	__asm__ volatile("rep outsb" : "=S"(src), "=c"(left) : "S"(src), "c"(left), "d"(port));
}

u8 inb(u16 port)
{
	u8 value;

	__asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

void outw(u16 port, u16 value)
{
	__asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

u16 inw(u16 port)
{
	u16 value;
	__asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

void outl(u16 port, u32 value)
{
	__asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

u32 inl(u16 port)
{
	u32 value;
	__asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

void io_wait(void)
{
	outb(0x80, 0);
}
