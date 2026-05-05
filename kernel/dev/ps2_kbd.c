#include <b1nix/console.h>
#include <b1nix/io.h>
#include <b1nix/types.h>

#define KBD_BUFFER_SIZE 256

static char kbd_buffer[KBD_BUFFER_SIZE];
static usize kbd_head = 0;
static usize kbd_tail = 0;
static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int extended_scancode = 0;

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

static void kbd_push(char c)
{
	usize next_head = (kbd_head + 1) % KBD_BUFFER_SIZE;
	if (next_head != kbd_tail) {
		kbd_buffer[kbd_head] = c;
		kbd_head = next_head;
	}
}

static void kbd_push_escape(char final)
{
	kbd_push(27);
	kbd_push('[');
	kbd_push(final);
}

static void kbd_push_fkey(int f)
{
	kbd_push(27);
	kbd_push('[');
	kbd_push('M');
	kbd_push((char)f);
}

void ps2_kbd_interrupt_handler(void)
{
	u8 scancode = inb(0x60);
	if (scancode == 0xE0) {
		extended_scancode = 1;
		return;
	}

	if (scancode & 0x80) {
		// Key release
		u8 key = scancode & 0x7F;
		if (key == 0x2A || key == 0x36) { // Left or Right Shift
			shift_pressed = 0;
		} else if (key == 0x1D) {
			ctrl_pressed = 0;
		}
		if (extended_scancode && (key == 0x5B || key == 0x5C)) {
			ctrl_pressed = 0;
		}
		extended_scancode = 0;
	} else {
		if (extended_scancode) {
			switch (scancode) {
			case 0x48: kbd_push_escape('A'); break; /* up */
			case 0x50: kbd_push_escape('B'); break; /* down */
			case 0x4D: kbd_push_escape('C'); break; /* right */
			case 0x4B: kbd_push_escape('D'); break; /* left */
			case 0x47: kbd_push_escape('H'); break; /* home */
			case 0x4F: kbd_push_escape('F'); break; /* end */
			case 0x1D: /* Right Ctrl */
			case 0x5B: /* Left GUI (Mac Cmd) */
			case 0x5C: /* Right GUI (Mac Cmd) */
				ctrl_pressed = 1;
				break;
			default: break;
			}
			extended_scancode = 0;
			return;
		}

		// Key press
		if (scancode == 0x2A || scancode == 0x36) {
			shift_pressed = 1;
		} else if (scancode == 0x1D) {
			ctrl_pressed = 1;
		} else if (scancode >= 0x3B && scancode <= 0x44) {
			// F1 to F10
			kbd_push_fkey((int)(scancode - 0x3B + 1));
		} else if (scancode == 0x57) {
			kbd_push_fkey(11);
		} else if (scancode == 0x58) {
			kbd_push_fkey(12);
		} else if (scancode < 128) {
			char c = shift_pressed ? scancode_map_shift[scancode] : scancode_map[scancode];
			if (c != 0) {
				if (ctrl_pressed && c >= 'a' && c <= 'z') {
					c = (char)(c - 'a' + 1);
				} else if (ctrl_pressed && c >= 'A' && c <= 'Z') {
					c = (char)(c - 'A' + 1);
				}
				kbd_push(c);
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
