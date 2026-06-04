#ifndef B1NIX_USB_H
#define B1NIX_USB_H

#include <b1nix/types.h>

/*
 * Minimal USB stack for real-hardware input (M37). The host has no PS/2
 * controller, so a USB HID boot keyboard on the xHCI controller is the only
 * way to get keystrokes on the metal.
 */

/* Probe + initialise the first xHCI controller and enumerate a HID boot
 * keyboard on it. Returns 1 if a controller was found and brought up, else 0.
 * Bounded throughout so an absent/wedged controller never hangs the boot. */
int xhci_probe(void);

/* Poll the keyboard's interrupt endpoint once and feed any new key events into
 * the shared keyboard input ring (via ps2_kbd_handle_byte). Cheap no-op if no
 * keyboard was enumerated. Called from the timer tick / input poll path. */
void usb_kbd_poll(void);

/* Translate a HID boot-protocol keyboard report (8 bytes) into PS/2 set-1
 * make/break scancodes against the previous report, driving ps2_kbd_handle_byte
 * so USB keys reuse the existing shift/ctrl/signal/line-discipline handling.
 * Exposed for the M37-USB self-test. */
void usb_hid_translate_report(const u8 report[8]);

/* Self-test (test mode only): emits M37-USB markers for controller bring-up and
 * enumeration, and verifies the HID->scancode translation with a synthetic
 * report. No-op if no controller was initialised. */
void usb_selftest(void);

#endif
