/* SPDX-License-Identifier: GPL-2.0-only */
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/fb_console.h>
#include <b1nix/panic_screen.h>
#include <b1nix/splash.h>
#include <b1nix/types.h>
#include <b1nix/version.h>
#include <string.h>
#include "font8x8.h"

#if defined(__aarch64__)
#define SPLASH_ARCH "aarch64"
#else
#define SPLASH_ARCH "x86_64"
#endif

/*
 * B1nix Unified Design Tokens (32-bit ARGB)
 */
#define B1NIX_COLOR_BG            0xFF181C24u
#define B1NIX_COLOR_SURFACE       0xFF2A3040u
#define B1NIX_COLOR_BORDER        0xFF3E4858u
#define B1NIX_COLOR_TEXT_MUTED    0xFF7890A8u
#define B1NIX_COLOR_TEXT_MAIN     0xFFD0D8E0u
#define B1NIX_COLOR_ACCENT_CYAN   0xFF00E5FFu
#define B1NIX_COLOR_ACCENT_AMBER  0xFFFFAA00u
#define B1NIX_COLOR_ALERT_RED     0xFFFF5A4Du
#define B1NIX_COLOR_SUCCESS_GREEN 0xFF55FF88u

static int splash_shown;
static int splash_dismissed;

static void draw_fill_rect(u32 x0, u32 y0, u32 rw, u32 rh, u32 color, u32 w, u32 h, u32 pitch, volatile u8 *fb)
{
	for (u32 py = y0; py < y0 + rh && py < h; py++) {
		u64 row_off = (u64)py * pitch;
		for (u32 px = x0; px < x0 + rw && px < w; px++) {
			*(volatile u32 *)(fb + row_off + (u64)px * 4) = color;
		}
	}
}

static void draw_title_centered(const char *str, u32 cy, u32 scale, u32 color, u32 w, u32 h, u32 pitch, volatile u8 *fb)
{
	usize len = strlen(str);
	u32 str_w = (u32)len * 8 * scale;
	u32 cx = (w > str_w) ? (w - str_w) / 2 : 0;

	for (usize i = 0; i < len; i++) {
		char c = str[i];
		if ((unsigned char)c <= ' ') {
			cx += 8 * scale;
			continue;
		}
		const unsigned char *glyph = font8x8_basic[(unsigned char)c];
		for (u32 gy = 0; gy < 8; gy++) {
			for (u32 gx = 0; gx < 8; gx++) {
				if (!((glyph[gy] >> (7 - gx)) & 1))
					continue;
				for (u32 dy = 0; dy < scale; dy++) {
					u32 py = cy + gy * scale + dy;
					if (py >= h) continue;
					u64 row_off = (u64)py * pitch;
					for (u32 dx = 0; dx < scale; dx++) {
						u32 px = cx + gx * scale + dx;
						if (px < w) {
							*(volatile u32 *)(fb + row_off + (u64)px * 4) = color;
						}
					}
				}
			}
		}
		cx += 8 * scale;
	}
}

/*
 * Draw the B1nix Otter using the identical RLE sprite and palette from panic_screen.
 */
static void splash_draw_otter(u32 x0, u32 y0, u32 scale, u32 w, u32 h, u32 pitch, volatile u8 *fb)
{
	u32 total = (u32)panic_otter_width * panic_otter_height;
	u32 pos = 0;

	for (usize i = 0; i < panic_otter_rle_len && pos < total; i++) {
		u32 idx = panic_otter_rle[i] >> 5;
		u32 run = (panic_otter_rle[i] & 0x1f) + 1u;

		for (u32 r = 0; r < run && pos < total; r++, pos++) {
			u32 px = pos % panic_otter_width;
			u32 py = pos / panic_otter_width;

			if (!idx)
				continue;
			u32 color = panic_otter_palette[idx];
			for (u32 dy = 0; dy < scale; dy++) {
				u32 sy = y0 + py * scale + dy;
				if (sy >= h) continue;
				u64 row_off = (u64)sy * pitch;
				for (u32 dx = 0; dx < scale; dx++) {
					u32 sx = x0 + px * scale + dx;
					if (sx < w) {
						*(volatile u32 *)(fb + row_off + (u64)sx * 4) = color;
					}
				}
			}
		}
	}
}

/*
 * Show the B1nix boot splash.
 *
 * In graphical mode (framebuffer available):
 * Paints the B1nix Otter artwork and titles into the upper region of the screen,
 * sets the console top boundary to split_y so kernel boot logs scroll underneath,
 * and presents the buffer.
 *
 * In headless/serial mode:
 * Avoids dumping massive ASCII artwork into the serial log ring, maintaining
 * parity between architectures. A full ASCII dump is only emitted if explicitly
 * requested via b1nix.asciisplash.
 */
void b1nix_splash_show(void)
{
	static int serial_splash_done;

	if (splash_shown || splash_dismissed)
		return;

	if (!fb_console_ready()) {
		if (serial_splash_done)
			return;
		serial_splash_done = 1;

		/* Only dump full ASCII art to serial if explicitly requested on cmdline */
		if (bootinfo_has_flag("b1nix.asciisplash")) {
			for (const char *const *l = panic_otter_ascii; *l; l++) {
				console_write(*l);
				console_write("\n");
			}
			console_write("=== b1nix " B1NIX_VERSION_STR " " SPLASH_ARCH " ===\n\n");
		}
		return;
	}

	u32 w = fb_console_width();
	u32 h = fb_console_height();
	u32 pitch = fb_console_pitch();
	volatile u8 *fb = (volatile u8 *)fb_console_frontbuffer();

	if (!fb || w == 0 || h == 0)
		return;

	int portrait = (h > w + w / 4);

	/* Determine optimal otter magnification scale */
	u32 os = portrait ? (w * 3 / 5) / panic_otter_width
	                  : (w * 2 / 5) / panic_otter_width;
	if (h / 3 / panic_otter_height < os)
		os = (h / 3) / panic_otter_height;
	if (os < 1) os = 1;
	if (os > 4) os = 4;

	u32 art_w = (u32)panic_otter_width * os;
	u32 art_h = (u32)panic_otter_height * os;
	u32 pad = 12;

	u32 card_w = art_w + 2 * pad;
	u32 card_h = art_h + 2 * pad;
	u32 card_x = (w > card_w) ? (w - card_w) / 2 : 0;
	u32 card_y = 68;

	u32 top_title_y = 16;
	u32 split_y = card_y + card_h + 24;
	if (split_y >= h - 200)
		split_y = h / 2;

	/* Clear top artwork region to B1NIX_COLOR_BG */
	draw_fill_rect(0, 0, w, split_y, B1NIX_COLOR_BG, w, h, pitch, fb);

	/* Render top brand titles */
	draw_title_centered("B 1 N I X", top_title_y, 3, B1NIX_COLOR_ACCENT_CYAN, w, h, pitch, fb);
	draw_title_centered("UNIX-LIKE MONOLITHIC KERNEL  *  VERSION " B1NIX_VERSION_STR "  *  " SPLASH_ARCH,
	                    top_title_y + 30, 1, B1NIX_COLOR_TEXT_MUTED, w, h, pitch, fb);

	/* Draw Card Container */
	draw_fill_rect(card_x, card_y, card_w, card_h, B1NIX_COLOR_SURFACE, w, h, pitch, fb);
	/* Card borders */
	draw_fill_rect(card_x, card_y, card_w, 2, B1NIX_COLOR_BORDER, w, h, pitch, fb);
	draw_fill_rect(card_x, card_y + card_h - 2, card_w, 2, B1NIX_COLOR_BORDER, w, h, pitch, fb);
	draw_fill_rect(card_x, card_y, 2, card_h, B1NIX_COLOR_BORDER, w, h, pitch, fb);
	draw_fill_rect(card_x + card_w - 2, card_y, 2, card_h, B1NIX_COLOR_BORDER, w, h, pitch, fb);

	/* Draw Otter (pixel-perfect RLE matching panic_screen) */
	splash_draw_otter(card_x + pad, card_y + pad, os, w, h, pitch, fb);

	/* Neon divider bar separating artwork from terminal */
	u32 split_bar_y = split_y - 4;
	draw_fill_rect(40, split_bar_y, w > 80 ? w - 80 : w, 2, B1NIX_COLOR_ACCENT_AMBER, w, h, pitch, fb);

	/* Boot log scrolls below the splash art */
	fb_console_set_top(split_y);
	fb_console_replay();
	fb_console_present_all();
	splash_shown = 1;
}

/*
 * Dismiss the boot splash and return the full screen to the console / userspace.
 */
void b1nix_splash_dismiss(void)
{
	if (bootinfo_has_flag("b1nix.keep-splash"))
		return;
	splash_dismissed = 1;
	if (!splash_shown)
		return;
	splash_shown = 0;
	fb_console_set_top(0);
	fb_console_clear();
}
