#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/input.h>
#include <b1nix/io.h>
#include <b1nix/sched.h>
#include <b1nix/types.h>
#include <b1nix/bootinfo.h>
#include <b1nix/vt.h>

#define KBD_BUFFER_SIZE 256

static char kbd_buffer[KBD_BUFFER_SIZE];
static usize kbd_head = 0;
static usize kbd_tail = 0;
static u8 shift_mask = 0;
static int ctrl_pressed = 0;
static int extended_scancode = 0;
/* M107: Alt is tracked so the VT layer can claim Alt+Fn as a console switch. */
static int alt_pressed = 0;
static int num_lock_enabled = 1;
static int kbd_debug_enabled;

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
/* Bounded in time rather than in reads: the count that used to bound these was
 * a duration only on the machine it was measured on. */
static int kbd_wait_input_clear(void)
{
	u64 deadline = arch_tsc_monotonic_ns() + 20000000ull; /* 20 ms */

	while (arch_tsc_monotonic_ns() < deadline)
		if (!(inb(0x64) & 0x02))
			return 0;
	return -1;
}

static int kbd_wait_output_full(void)
{
	u64 deadline = arch_tsc_monotonic_ns() + 20000000ull; /* 20 ms */

	while (arch_tsc_monotonic_ns() < deadline)
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
	kbd_debug_enabled = bootinfo_has_flag("b1nix.kbd-debug");

	/* M107: hand the builtin layout to the VT keymap. From here on every
	 * translation goes through that table, so KDSKBENT (loadkmap) can replace
	 * any entry and KDGKBENT (dumpkmap) can read the live layout back. */
	vt_keymap_seed(scancode_map, scancode_map_shift);

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

	/* Keep the physical PS/2 LED in sync with our initial numeric mode. */
	kbd_dev_write(0xED);        /* set LEDs */
	if (kbd_dev_read() == 0xFA) {
		kbd_dev_write(0x02);    /* Num Lock */
		(void)kbd_dev_read();
	}
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

static void kbd_push_escape_tilde(char number)
{
	kbd_push(27);
	kbd_push('[');
	kbd_push(number);
	kbd_push('~');
}

static void kbd_push_fkey(int f)
{
	kbd_push(27);
	kbd_push('[');
	kbd_push('M');
	kbd_push((char)f);
}

static void kbd_handle_keypad(u8 scancode)
{
	/* Console-friendly policy: keypad digits work immediately even when the
	 * firmware LED state cannot be read. Num Lock on means numbers; when it is
	 * off, holding Shift still requests numbers, matching the usual inversion
	 * rule. Holding Shift with Num Lock on requests navigation. */
	int shift_pressed = shift_mask != 0;
	int numeric = num_lock_enabled ? !shift_pressed : shift_pressed;

	if (numeric) {
		switch (scancode) {
		case 0x47: kbd_push('7'); break;
		case 0x48: kbd_push('8'); break;
		case 0x49: kbd_push('9'); break;
		case 0x4B: kbd_push('4'); break;
		case 0x4C: kbd_push('5'); break;
		case 0x4D: kbd_push('6'); break;
		case 0x4F: kbd_push('1'); break;
		case 0x50: kbd_push('2'); break;
		case 0x51: kbd_push('3'); break;
		case 0x52: kbd_push('0'); break;
		case 0x53: kbd_push('.'); break;
		default: break;
		}
		return;
	}

	switch (scancode) {
	case 0x47: kbd_push_escape('H'); break;       /* Home */
	case 0x48: kbd_push_escape('A'); break;       /* Up */
	case 0x49: kbd_push_escape_tilde('5'); break; /* Page Up */
	case 0x4B: kbd_push_escape('D'); break;       /* Left */
	case 0x4C: break;                              /* Keypad 5 */
	case 0x4D: kbd_push_escape('C'); break;       /* Right */
	case 0x4F: kbd_push_escape('F'); break;       /* End */
	case 0x50: kbd_push_escape('B'); break;       /* Down */
	case 0x51: kbd_push_escape_tilde('6'); break; /* Page Down */
	case 0x52: kbd_push_escape_tilde('2'); break; /* Insert */
	case 0x53: kbd_push_escape_tilde('3'); break; /* Delete */
	default: break;
	}
}

extern void ps2_mouse_handle_byte(u8 data);

void ps2_kbd_handle_byte(u8 scancode)
{
	if (kbd_debug_enabled) {
		console_write("kbd: raw=0x");
		console_write_hex32(scancode);
		console_write("\n");
	}

	if (scancode == 0xE0) {
		extended_scancode = 1;
		return;
	}

	/* M47: mirror the raw make/break stream to /dev/input/event0 before the
	 * console line discipline consumes it (keymaps live in userspace). */
	input_kbd_scancode(scancode, extended_scancode);

	if (scancode & 0x80) {
		// Key release
		u8 key = scancode & 0x7F;
		if (extended_scancode && (key == 0x2A || key == 0x36)) {
			/* Fake Shift used by some set-1 keypad/PrintScreen sequences. */
		} else if (key == 0x2A) {
			shift_mask &= (u8)~1u;
		} else if (key == 0x36) {
			shift_mask &= (u8)~2u;
		} else if (key == 0x1D) {
			ctrl_pressed = 0;
		} else if (key == 0x38) {
			alt_pressed = 0;
		}
		if (extended_scancode && (key == 0x5B || key == 0x5C)) {
			ctrl_pressed = 0;
		}
		extended_scancode = 0;
	} else {
		if (extended_scancode) {
			switch (scancode) {
			case 0x2A: /* Fake Shift in keypad/PrintScreen sequence. */
			case 0x36:
				break;
			case 0x48: kbd_push_escape('A'); break; /* up */
			case 0x50: kbd_push_escape('B'); break; /* down */
			case 0x4D: kbd_push_escape('C'); break; /* right */
			case 0x4B: kbd_push_escape('D'); break; /* left */
			case 0x47: kbd_push_escape('H'); break; /* home */
			case 0x4F: kbd_push_escape('F'); break; /* end */
			case 0x49: kbd_push_escape_tilde('5'); break; /* page up */
			case 0x51: kbd_push_escape_tilde('6'); break; /* page down */
			case 0x52: kbd_push_escape_tilde('2'); break; /* insert */
			case 0x53: kbd_push_escape_tilde('3'); break; /* delete */
			case 0x1C: kbd_push('\n'); break;             /* keypad enter */
			case 0x35: kbd_push('/'); break;              /* keypad slash */
			case 0x1D: /* Right Ctrl */
			case 0x5B: /* Left GUI (Mac Cmd) */
			case 0x5C: /* Right GUI (Mac Cmd) */
				ctrl_pressed = 1;
				break;
			case 0x38: /* Right Alt (AltGr) */
				alt_pressed = 1;
				break;
			default: break;
			}
			extended_scancode = 0;
			return;
		}

		// Key press
		if (scancode == 0x2A) {
			shift_mask |= 1u;
		} else if (scancode == 0x36) {
			shift_mask |= 2u;
		} else if (scancode == 0x1D) {
			ctrl_pressed = 1;
		} else if (scancode == 0x38) {
			alt_pressed = 1;
		} else if (vt_kbd_hotkey(scancode, alt_pressed)) {
			/* Alt+Fn switched (or tried to switch) the virtual terminal; the
			 * key must not also reach the foreground application. */
		} else if (scancode == 0x45) {
			num_lock_enabled = !num_lock_enabled;
		} else if (scancode >= 0x3B && scancode <= 0x44) {
			// F1 to F10
			kbd_push_fkey((int)(scancode - 0x3B + 1));
		} else if (scancode == 0x57) {
			kbd_push_fkey(11);
		} else if (scancode == 0x58) {
			kbd_push_fkey(12);
		} else if (scancode >= 0x47 && scancode <= 0x53 &&
		           scancode != 0x4A && scancode != 0x4E) {
			kbd_handle_keypad(scancode);
		} else if (scancode < 128) {
			/* M107: translate through the loadable keymap (KDSKBENT) instead of
			 * the builtin tables directly, so `loadkmap` really changes what a
			 * key produces. The control fold lives in the ctrl tables. */
			if (!vt_kbd_mode_is_xlate()) {
				/* K_RAW / K_MEDIUMRAW: the VT's owner wants scancodes, which
				 * it reads from /dev/input/event0 (mirrored above). */
				extended_scancode = 0;
				return;
			}
			char c = 0;
			if (!vt_keymap_translate(scancode, shift_mask != 0, ctrl_pressed,
			                         alt_pressed, &c))
				c = 0;
			if (c != 0) {
				if (vt_kbd_char(c)) {
					/* A non-console VT owns the keyboard right now. */
					extended_scancode = 0;
					return;
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

/* Non-consuming readiness probe used by the tty poll path. */
int ps2_kbd_has_data(void)
{
	usize tail = __atomic_load_n(&kbd_tail, __ATOMIC_RELAXED);
	usize head = __atomic_load_n(&kbd_head, __ATOMIC_ACQUIRE);
	return head != tail;
}
