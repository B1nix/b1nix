#ifndef B1NIX_INPUT_H
#define B1NIX_INPUT_H

#include <b1nix/types.h>

/* M47: evdev-style input event devices (/dev/input/event0..N).
 *
 * Event record layout is identical on both arches (16 bytes, no padding):
 * userspace mirrors this struct in userspace/include/b1nix/input.h — keep
 * them matched. */
struct b1nix_input_event {
  u64 time_ticks; /* scheduler uptime ticks at enqueue */
  u16 type;
  u16 code;
  i32 value;
};

/* Event types (Linux evdev numbering). */
#define B1NIX_EV_SYN 0x00
#define B1NIX_EV_KEY 0x01
#define B1NIX_EV_REL 0x02
#define B1NIX_EV_ABS 0x03

/* Relative/absolute axis codes. */
#define B1NIX_REL_X 0x00
#define B1NIX_REL_Y 0x01
#define B1NIX_ABS_X 0x00
#define B1NIX_ABS_Y 0x01

/* Button/key codes. Keyboard events carry the raw PS/2 set-1 scancode
 * (0xE0-prefixed scancodes are reported as 0xE000 | code); keymap handling
 * is a userspace concern. Mouse buttons use the Linux BTN_* values. */
#define B1NIX_BTN_LEFT 0x110
#define B1NIX_BTN_RIGHT 0x111
#define B1NIX_BTN_MIDDLE 0x112
/* Touchscreen contact key (Linux BTN_TOUCH): value 1 = finger down, 0 = up. */
#define B1NIX_BTN_TOUCH 0x14a

/* Device indices. */
#define INPUT_DEV_KBD 0
#define INPUT_DEV_MOUSE 1
#define INPUT_DEV_TOUCH 2 /* /dev/input/event2 — virtio touchscreen (wl_touch) */
#define INPUT_NDEVS 3

void input_init(void);

/* Producer API (IRQ-safe): queue one event on every open client of `dev`.
 * A B1NIX_EV_SYN event marks the end of one hardware report. */
void input_event_push(int dev, u16 type, u16 code, i32 value);
void input_event_sync(int dev);

/* Keyboard helper: translates one PS/2 byte stream step into an EV_KEY
 * event (called from the kbd driver with the pre-0xE0 state). */
void input_kbd_scancode(u8 scancode, int extended);

/* True when at least one client has the device open (used by the smoke-test
 * injector to know a reader is listening). */
int input_dev_has_clients(int dev);

/* vfs_open() intercept support, mirroring serial_tty: returns an fd or
 * -errno; `resolved_path` form is /dev/input/event<N>. */
int input_path_index(const char *resolved_path);
int input_dev_open(int idx, int flags);

#endif
