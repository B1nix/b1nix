#include <b1nix/bootinfo.h>
#include <b1nix/io.h>

/*
 * Port I/O on a machine that has none.
 *
 * An AArch64 CPU has no IN/OUT instructions: a PCI card's I/O BAR is reached
 * through a window the host bridge maps into ordinary memory, and the device
 * tree says where. So `port` here is an offset into that window rather than
 * an address on an ISA bus, and every access below is a plain MMIO one.
 *
 * A board whose device tree describes no such window (a Raspberry Pi) gets
 * reads of all-ones and writes that go nowhere, which is what a bus with no
 * card on it looks like — the same answer the caller would get on a PC with
 * an empty I/O address.
 */

static volatile u8 *io_port(u16 port)
{
	u64 base = fdt_pci_io_base();
	u64 size = fdt_pci_io_size();

	if (!base || port >= size)
		return 0;
	return (volatile u8 *)(usize)(base + port);
}

void outb(u16 port, u8 value)
{
	volatile u8 *p = io_port(port);

	if (p)
		*p = value;
}

u8 inb(u16 port)
{
	volatile u8 *p = io_port(port);

	return p ? *p : 0xffu;
}

void outw(u16 port, u16 value)
{
	volatile u8 *p = io_port(port);

	if (p)
		*(volatile u16 *)p = value;
}

u16 inw(u16 port)
{
	volatile u8 *p = io_port(port);

	return p ? *(volatile u16 *)p : 0xffffu;
}

void outl(u16 port, u32 value)
{
	volatile u8 *p = io_port(port);

	if (p)
		*(volatile u32 *)p = value;
}

u32 inl(u16 port)
{
	volatile u8 *p = io_port(port);

	return p ? *(volatile u32 *)p : 0xffffffffu;
}

void io_wait(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}
