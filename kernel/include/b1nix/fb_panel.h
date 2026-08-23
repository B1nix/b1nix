#ifndef B1NIX_FB_PANEL_H
#define B1NIX_FB_PANEL_H

#include <b1nix/bootinfo.h>
#include <b1nix/types.h>

/*
 * The one piece of the framebuffer console that differs between machines:
 * where the pixels live, and what has to happen for them to reach a screen.
 * Everything above this — glyphs, the cursor, scrolling, the ANSI escapes — is
 * in kernel/dev/fb_console.c and is the same code on every architecture.
 */

/* Describe the panel. Returns 0 and fills `out` when there is one. `address`
 * is a physical address the console maps as MMIO, unless `in_ram` is set, in
 * which case it is already a kernel virtual address written to with ordinary
 * stores. */
int fb_panel_probe(struct boot_framebuffer *out, int *in_ram);

/* Push a rectangle of the console's own buffer to the display. A panel that
 * IS the scanout does nothing here; one that has to be handed to a device
 * (virtio-gpu) sends it. */
void fb_panel_present(const void *pixels, u32 pitch, u32 width, u32 height,
                      u32 x, u32 y, u32 w, u32 h);

#endif
