#include <tinyunix/console.h>
#include <tinyunix/serial.h>

void panic(const char *message)
{
	console_write("\nKERNEL PANIC: ");
	console_write(message);
	console_write("\n");

	serial_write("\nKERNEL PANIC: ");
	serial_write(message);
	serial_write("\n");

	for (;;) {
		__asm__ volatile("cli; hlt");
	}
}
