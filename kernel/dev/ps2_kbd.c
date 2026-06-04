#include <b1nix/console.h>
#include <b1nix/io.h>
#include <b1nix/sched.h>
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
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '7', '8',
    '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', 0, 0
};

static const char scancode_map_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '7', '8',
    '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', 0, 0
};

/* i8042 controller access helpers. Status port 0x64: bit0 = output buffer full
 * (data ready to read from 0x60), bit1 = input buffer full (do not write yet).
 * All waits are bounded so a wedged/absent controller can never hang boot. */
static int kbd_wait_input_clear(void)
{
	for (int i = 0; i < 100000; i++)
		if (!(inb(0x64) & 0x02))
			return 0;
	return -1;
}

static int kbd_wait_output_full(void)
{
	for (int i = 0; i < 100000; i++)
		if (inb(0x64) & 0x01)
			return 0;
	return -1;
}

static void kbd_flush(void)
{
	for (int i = 0; i < 32 && (inb(0x64) & 0x01); i++)
		(void)inb(0x60);
}

static void kbd_ctrl_cmd(u8 cmd) /* command to the controller (port 0x64) */
{
	kbd_wait_input_clear();
	outb(0x64, cmd);
}

static void kbd_dev_write(u8 data) /* byte to the device / data port 0x60 */
{
	kbd_wait_input_clear();
	outb(0x60, data);
}

static u8 kbd_dev_read(void)
{
	if (kbd_wait_output_full() == 0)
		return inb(0x60);
	return 0;
}

/* Real-hardware i8042/keyboard bring-up. QEMU comes up with scanning enabled
 * and IRQ1 already on, so the old no-op "worked" there; bare metal (e.g. Acer
 * Aspire One ZG5) needs the full sequence. We also cannot rely on the PS/2
 * mouse driver to enable IRQ1 as a side effect of its config-byte write — it
 * bails out early when there is no framebuffer, which would leave the keyboard
 * dead. The controller config byte is read-modify-written so kbd and mouse
 * init compose regardless of order. */
void ps2_kbd_init(void)
{
	/* Disable the first port while we reconfigure, then drain stale bytes. */
	kbd_ctrl_cmd(0xAD);
	kbd_flush();

	/* IRQ off during init (so device ACKs don't get eaten by the handler or
	 * mis-read as scancodes), translation on (our map is scancode set 1),
	 * first-port clock enabled. */
	kbd_ctrl_cmd(0x20);
	u8 cfg = kbd_dev_read();
	cfg &= ~0x01u;  /* first-port interrupt off (for now) */
	cfg |=  0x40u;  /* scancode translation set2 -> set1 */
	cfg &= ~0x10u;  /* first-port clock enabled */
	kbd_ctrl_cmd(0x60);
	kbd_dev_write(cfg);

	/* Enable the first port and bring the keyboard device up. */
	kbd_ctrl_cmd(0xAE);
	kbd_flush();

	kbd_dev_write(0xFF);        /* reset; device replies 0xFA then 0xAA */
	(void)kbd_dev_read();
	(void)kbd_dev_read();
	kbd_flush();

	kbd_dev_write(0xF4);        /* enable scanning; device replies 0xFA */
	(void)kbd_dev_read();
	kbd_flush();

	/* Now arm IRQ1 (re-read config in case the device handshake touched it). */
	kbd_ctrl_cmd(0x20);
	cfg = kbd_dev_read();
	cfg |=  0x01u;  /* first-port interrupt on */
	cfg |=  0x40u;  /* keep translation */
	cfg &= ~0x10u;  /* keep first-port clock enabled */
	kbd_ctrl_cmd(0x60);
	kbd_dev_write(cfg);
	kbd_flush();

	console_write("ps2_kbd: initialized on irq1\n");
}

/* Single-producer (IRQ handler) / single-consumer (ps2_kbd_getc, called from a
 * syscall on possibly another CPU) ring. With the BKL gone from the device-IRQ
 * path, producer and consumer run unserialised, so the index hand-off needs
 * explicit ordering: the producer publishes kbd_head with RELEASE *after*
 * writing the slot, and the consumer ACQUIRE-loads kbd_head so it never reads a
 * slot before that write is visible; symmetrically for kbd_tail. A device IRQ
 * is delivered to one CPU at a time and not re-entrant, so there is only ever
 * one producer and one consumer — no CAS needed. */
static void kbd_push(char c)
{
	usize head = __atomic_load_n(&kbd_head, __ATOMIC_RELAXED); /* sole producer */
	usize tail = __atomic_load_n(&kbd_tail, __ATOMIC_ACQUIRE);
	usize next_head = (head + 1) % KBD_BUFFER_SIZE;
	if (next_head != tail) {
		kbd_buffer[head] = c;
		__atomic_store_n(&kbd_head, next_head, __ATOMIC_RELEASE);
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

extern void ps2_mouse_handle_byte(u8 data);

void ps2_kbd_handle_byte(u8 scancode)
{
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
				} else if (ctrl_pressed && c == '\\') {
					c = 28;
				}
				if ((console.termios.c_lflag & B1NIX_ISIG) && c == 3) {
					if (console.fg_pgrp > 0) {
						scheduler_kill_process_group(console.fg_pgrp, SIGINT);
					}
					console_write("^C\n");
					return;
				}
				if ((console.termios.c_lflag & B1NIX_ISIG) && c == 26) {
					if (console.fg_pgrp > 0) {
						scheduler_kill_process_group(console.fg_pgrp, SIGTSTP);
					}
					console_write("^Z\n");
					return;
				}
				if ((console.termios.c_lflag & B1NIX_ISIG) && c == 28) {
					if (console.fg_pgrp > 0) {
						scheduler_kill_process_group(console.fg_pgrp, SIGQUIT);
					}
					console_write("^\\\n");
					return;
				}
				kbd_push(c);
			}
		}
	}
}

void ps2_kbd_interrupt_handler(void)
{
	while (1) {
		u8 status = inb(0x64);
		if (!(status & 0x01)) {
			break;
		}
		u8 data = inb(0x60);
		if (status & 0x20) {
			ps2_mouse_handle_byte(data);
		} else {
			ps2_kbd_handle_byte(data);
		}
	}
}

char ps2_kbd_getc(void)
{
	usize tail = __atomic_load_n(&kbd_tail, __ATOMIC_RELAXED); /* sole consumer */
	usize head = __atomic_load_n(&kbd_head, __ATOMIC_ACQUIRE);
	if (head == tail) {
		return 0;
	}

	char c = kbd_buffer[tail];
	__atomic_store_n(&kbd_tail, (tail + 1) % KBD_BUFFER_SIZE, __ATOMIC_RELEASE);
	return c;
}
