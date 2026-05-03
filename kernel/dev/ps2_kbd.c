#include <tinyunix/console.h>
#include <tinyunix/io.h>
#include <tinyunix/types.h>

#define KBD_BUFFER_SIZE 256

static char kbd_buffer[KBD_BUFFER_SIZE];
static usize kbd_head = 0;
static usize kbd_tail = 0;
static int shift_pressed = 0;

static const char scancode_map[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '-', 0, 0, 0, '+', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const char scancode_map_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '-', 0, 0, 0, '+', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

void ps2_kbd_init(void)
{
	console_write("ps2_kbd: initialized on irq1\n");
}

void ps2_kbd_interrupt_handler(void)
{
	u8 scancode = inb(0x60);
	if (scancode & 0x80) {
		// Key release
		u8 key = scancode & 0x7F;
		if (key == 0x2A || key == 0x36) { // Left or Right Shift
			shift_pressed = 0;
		}
	} else {
		// Key press
		if (scancode == 0x2A || scancode == 0x36) {
			shift_pressed = 1;
		} else if (scancode < 128) {
			char c = shift_pressed ? scancode_map_shift[scancode] : scancode_map[scancode];
			if (c != 0) {
				usize next_head = (kbd_head + 1) % KBD_BUFFER_SIZE;
				if (next_head != kbd_tail) {
					kbd_buffer[kbd_head] = c;
					kbd_head = next_head;
				}
			}
		}
	}
}

char ps2_kbd_getc(void)
{
	if (kbd_head == kbd_tail) {
		return 0;
	}

	char c = kbd_buffer[kbd_tail];
	kbd_tail = (kbd_tail + 1) % KBD_BUFFER_SIZE;
	return c;
}
