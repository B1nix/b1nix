#ifndef B1NIX_VT_H
#define B1NIX_VT_H

#include <b1nix/types.h>

/*
 * Virtual terminals (M107) — kernel/dev/vt.c.
 *
 * VT 1 is the boot console: whatever the kernel prints lands there. VTs 2..N
 * are independent text screens with their own cell buffer, cursor, termios and
 * input queue, reachable as /dev/tty2../dev/ttyN. Switching a VT repaints the
 * target's cell buffer onto the physical screen and re-points the keyboard at
 * its input queue, which is what chvt/openvt/deallocvt drive through the VT_*
 * ioctls on /dev/tty0 (the "currently active VT" alias).
 */

#define VT_COUNT 6
#define VT_CONSOLE 1

void vt_init(void);
/* Re-register the /dev/ttyN nodes after the root filesystem is switched. */
void vt_register_nodes(void);

/* console_putc() hook. Records `ch` in the console VT's cell buffer so the
 * screen can be repainted after a switch away and back. Returns 1 when the
 * caller must NOT draw (another VT owns the screen), 0 to draw as usual. */
int vt_console_putc(char ch);

/* Keyboard hooks, called from the PS/2 driver.
 *
 * vt_kbd_hotkey() consumes Alt+Fn / Ctrl+Alt+Fn as a VT switch and returns 1
 * when it did. vt_kbd_char() delivers a translated character: it returns 1
 * when a non-console VT swallowed it, 0 when the caller should push it into
 * the console's own queue as before. */
int vt_kbd_hotkey(u8 scancode, int alt);
int vt_kbd_char(char c);

/* Keymap. The PS/2 driver seeds the plain and shifted tables from its builtin
 * scancode maps once at init, then translates through vt_keymap_translate()
 * so that a keymap loaded with KDSKBENT (BusyBox loadkmap) really changes what
 * the keys produce. Returns 1 and stores a byte in *out when the key maps to
 * a character, 0 when it does not. */
void vt_keymap_seed(const char *plain, const char *shifted);
int vt_keymap_translate(u8 scancode, int shift, int ctrl, int alt, char *out);
/* Raw / medium-raw keyboard mode (KDSKBMODE): when the active VT is not in
 * K_XLATE the driver must not translate at all. */
int vt_kbd_mode_is_xlate(void);

/* The VT that currently owns the screen (1-based). */
int vt_active(void);

#endif
