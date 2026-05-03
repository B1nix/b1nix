#include <tinyunix/console.h>
#include <tinyunix/io.h>
#include <tinyunix/types.h>

void ps2_kbd_init(void)
{
	console_write("ps2_kbd: initialized on irq1\n");
}

void ps2_kbd_interrupt_handler(void)
{
	u8 scancode = inb(0x60);
	if (scancode & 0x80) {
		// Key release
	} else {
		// Key press
		console_write("kbd: scancode 0x");
		console_write_hex32(scancode);
		console_write("\n");
	}
}
