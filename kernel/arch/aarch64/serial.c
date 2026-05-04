#include <b1nix/serial.h>
#include <b1nix/types.h>

#define UART0_BASE 0x09000000
#define UART0_DR   (*(volatile u32 *)(UART0_BASE + 0x00))
#define UART0_FR   (*(volatile u32 *)(UART0_BASE + 0x18))

static int serial_ready(void)
{
	return (UART0_FR & (1 << 5)) == 0; // TXFF (Transmit FIFO full)
}

void serial_init(void)
{
	// QEMU virt UART is already initialized by QEMU enough to print.
}

void serial_putc(char ch)
{
	while (!serial_ready()) {
	}
	UART0_DR = ch;
}

void serial_write(const char *text)
{
	for (usize i = 0; text[i] != '\0'; i++) {
		serial_putc(text[i]);
	}
}
