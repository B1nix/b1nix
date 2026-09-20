#ifndef B1NIX_U_LINUX_INPUT_H
#define B1NIX_U_LINUX_INPUT_H
/* Linux evdev interface (UAPI). b1nix uses its own input stack; this provides
 * the struct layouts + event/key codes for ports that speak evdev (Chromium's
 * DOM keycode converter, ozone evdev). */
#include <linux/input-event-codes.h>
#include <sys/time.h>
#include <stdint.h>

struct input_event {
  struct timeval time;
  uint16_t type;
  uint16_t code;
  int32_t value;
};

struct input_id {
  uint16_t bustype;
  uint16_t vendor;
  uint16_t product;
  uint16_t version;
};

struct input_absinfo {
  int32_t value;
  int32_t minimum;
  int32_t maximum;
  int32_t fuzz;
  int32_t flat;
  int32_t resolution;
};

struct input_keymap_entry {
  uint8_t flags;
  uint8_t len;
  uint16_t index;
  uint32_t keycode;
  uint8_t scancode[32];
};

#define EVIOCGVERSION   0x80044501
#define EVIOCGID        0x80084502
#define EVIOCGNAME(len) (0x80004506 | ((len) << 16))

#endif /* B1NIX_U_LINUX_INPUT_H */
