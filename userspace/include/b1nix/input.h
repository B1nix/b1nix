#ifndef _B1NIX_INPUT_H
#define _B1NIX_INPUT_H

#include <stdint.h>

/* M47 /dev/input/event* ABI — userspace mirror of
 * kernel/include/b1nix/input.h. Keep the two files matched.
 * The record is 16 bytes on both arches. */

struct b1nix_input_event {
  uint64_t time_ticks; /* scheduler uptime ticks at enqueue */
  uint16_t type;
  uint16_t code;
  int32_t value;
};

/* Event types (Linux evdev numbering). */
#define B1NIX_EV_SYN 0x00
#define B1NIX_EV_KEY 0x01
#define B1NIX_EV_REL 0x02
#define B1NIX_EV_ABS 0x03

/* Axis codes. */
#define B1NIX_REL_X 0x00
#define B1NIX_REL_Y 0x01
#define B1NIX_ABS_X 0x00
#define B1NIX_ABS_Y 0x01

/* Keyboard events carry raw PS/2 set-1 scancodes (0xE0-prefixed scancodes
 * arrive as 0xE000 | code); mouse buttons use the Linux BTN_* values. */
#define B1NIX_BTN_LEFT 0x110
#define B1NIX_BTN_RIGHT 0x111
#define B1NIX_BTN_MIDDLE 0x112

#endif
