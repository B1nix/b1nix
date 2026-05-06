#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/types.h>
#include <string.h>
#include "font8x8.h"

static struct boot_framebuffer fb;
static u32 cursor_x;
static u32 cursor_y;
static u32 fg_color = 0x00FFFFFF;
static u32 bg_color = 0x00000000;
volatile u32 *fb_ptr = 0;

void fb_console_init(void)
{
	const struct boot_info *info = bootinfo_get();
	if (!info->has_framebuffer) {
		return;
	}

	fb = info->framebuffer;
	if (fb.bpp != 32 && fb.bpp != 24) {
		return; /* Unsupported color depth */
	}
	
	console_write("fb: addr 0x");
	console_write_hex64(fb.address);
	console_write(" w ");
	console_write_hex64(fb.width);
	console_write(" h ");
	console_write_hex64(fb.height);
	console_write(" pitch ");
	console_write_hex64(fb.pitch);
	console_write(" bpp ");
	console_write_hex64(fb.bpp);
	console_write("\n");

	u64 aligned_address = fb.address & ~(PAGE_SIZE - 1);
    u64 fb_size = fb.height * fb.pitch;
    u64 total_map_size = ((fb.address - aligned_address) + fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (u64 offset = 0; offset < total_map_size; offset += PAGE_SIZE) {
        vmm_map_page(0xffffa00000000000ULL + offset, aligned_address + offset, VMM_WRITABLE);
    }
    fb_ptr = (volatile u32 *)(usize)(0xffffa00000000000ULL + (fb.address - aligned_address));

	cursor_x = 0;
	cursor_y = 0;

	// Clear screen
	for (u32 y = 0; y < fb.height; y++) {
		for (u32 x = 0; x < fb.width; x++) {
            u8 *pixel = (u8 *)fb_ptr + (y * fb.pitch) + (x * (fb.bpp / 8));
            *(u32 *)pixel = bg_color;
		}
	}
}

void fb_console_clear(void)
{
    if (!fb_ptr) return;

	cursor_x = 0;
	cursor_y = 0;

	for (u32 y = 0; y < fb.height; y++) {
		for (u32 x = 0; x < fb.width; x++) {
            u8 *pixel = (u8 *)fb_ptr + (y * fb.pitch) + (x * (fb.bpp / 8));
            *(u32 *)pixel = bg_color;
		}
	}
}

static const u32 FONT_SCALE = 1;

static void fb_draw_char(char c, u32 x, u32 y)
{
    if (c < 0 || c > 127) c = '?';
    const u8 *glyph = font8x8_basic[(int)c];

    for (u32 cy = 0; cy < 8; cy++) {
        for (u32 cx = 0; cx < 8; cx++) {
            // MSB is on the left (x=0), LSB is on the right (x=7)
            u8 bit = (glyph[cy] >> (7 - cx)) & 1;
            u32 color = bit ? fg_color : bg_color;

            // Draw a 2x2 block for each font pixel (scaling by FONT_SCALE)
            for (u32 dy = 0; dy < FONT_SCALE; dy++) {
                for (u32 dx = 0; dx < FONT_SCALE; dx++) {
                    u32 px = x + cx * FONT_SCALE + dx;
                    u32 py = y + cy * FONT_SCALE + dy;

                    if (px < fb.width && py < fb.height) {
                        u8 *pixel = (u8 *)fb_ptr + (py * fb.pitch) + (px * (fb.bpp / 8));
                        *(u32 *)pixel = color;
                    }
                }
            }
        }
    }
}

static void fb_console_scroll(void)
{
    u32 line_height = 8 * FONT_SCALE;
    if (fb.height < line_height) return;

    u32 bytes_per_line = fb.pitch;
    u32 scroll_height = fb.height - line_height;

    u8 *dst = (u8 *)fb_ptr;
    u8 *src = (u8 *)fb_ptr + (line_height * bytes_per_line);
    
    memmove(dst, src, scroll_height * bytes_per_line);

    // Clear bottom line
    u64 bg64 = ((u64)bg_color << 32) | bg_color;
    u64 *dst64 = (u64 *)(dst + scroll_height * bytes_per_line);
    u32 words = (line_height * bytes_per_line) / 8;
    for (u32 i = 0; i < words; i++) {
        dst64[i] = bg64;
    }

    cursor_y -= line_height;
}

static int cursor_visible = 0;

static void fb_console_erase_cursor(void)
{
    if (!fb_ptr || !cursor_visible) return;
    
    u32 y_base = cursor_y + 7 * FONT_SCALE;
    for (u32 dy = 0; dy < FONT_SCALE; dy++) {
        for (u32 dx = 0; dx < 8 * FONT_SCALE; dx++) {
            u32 px = cursor_x + dx;
            u32 py = y_base + dy;
            if (px < fb.width && py < fb.height) {
                u8 *pixel = (u8 *)fb_ptr + (py * fb.pitch) + (px * (fb.bpp / 8));
                *(u32 *)pixel = bg_color;
            }
        }
    }
    cursor_visible = 0;
}

static int ansi_state = 0;
static int ansi_params[8];
static int ansi_param_idx = 0;
static int ansi_cursor_hidden = 0;



void fb_console_blink_cursor(void)
{
    if (!fb_ptr || ansi_cursor_hidden) return;

    cursor_visible = !cursor_visible;
    u32 color = cursor_visible ? fg_color : bg_color;

    u32 y_base = cursor_y + 7 * FONT_SCALE;
    for (u32 dy = 0; dy < FONT_SCALE; dy++) {
        for (u32 dx = 0; dx < 8 * FONT_SCALE; dx++) {
            u32 px = cursor_x + dx;
            u32 py = y_base + dy;
            if (px < fb.width && py < fb.height) {
                u8 *pixel = (u8 *)fb_ptr + (py * fb.pitch) + (px * (fb.bpp / 8));
                *(u32 *)pixel = color;
            }
        }
    }
}

void fb_console_putchar(char c)
{
    if (!fb_ptr) return;

    if (ansi_state == 1) {
        if (c == '[') {
            ansi_state = 2;
            ansi_param_idx = 0;
            for (int i = 0; i < 8; i++) ansi_params[i] = 0;
        } else {
            ansi_state = 0;
        }
        return;
    }
    if (ansi_state == 2) {
        if (c >= '0' && c <= '9') {
            ansi_params[ansi_param_idx] = ansi_params[ansi_param_idx] * 10 + (c - '0');
        } else if (c == ';') {
            if (ansi_param_idx < 7) ansi_param_idx++;
        } else if (c == '?') {
            /* ignore for now */
        } else {
            if (c == 'J') {
                if (ansi_params[0] == 2) fb_console_clear();
            } else if (c == 'H') {
                int row = ansi_params[0] > 0 ? ansi_params[0] - 1 : 0;
                int col = ansi_params[1] > 0 ? ansi_params[1] - 1 : 0;
                cursor_y = row * 8 * FONT_SCALE;
                cursor_x = col * 8 * FONT_SCALE;
            } else if (c == 'm') {
                for (int i = 0; i <= ansi_param_idx; i++) {
                    int code = ansi_params[i];
                    if (code == 0) {
                        fg_color = 0xFFFFFFFF; bg_color = 0xFF000000;
                    } else if (code >= 30 && code <= 37) {
                        u32 colors[8] = { 0xFF000000, 0xFFAA0000, 0xFF00AA00, 0xFFAAAA00, 0xFF0000AA, 0xFFAA00AA, 0xFF00AAAA, 0xFFAAAAAA };
                        fg_color = colors[code - 30];
                    } else if (code >= 40 && code <= 47) {
                        u32 colors[8] = { 0xFF000000, 0xFFAA0000, 0xFF00AA00, 0xFFAAAA00, 0xFF0000AA, 0xFFAA00AA, 0xFF00AAAA, 0xFFAAAAAA };
                        bg_color = colors[code - 40];
                    } else if (code == 90) {
                        fg_color = 0xFF555555; /* bright black/grey */
                    }
                }
            } else if (c == 'l') {
                if (ansi_params[0] == 25) {
                    fb_console_erase_cursor();
                    ansi_cursor_hidden = 1;
                }
            } else if (c == 'h') {
                if (ansi_params[0] == 25) ansi_cursor_hidden = 0;
            }
            ansi_state = 0;
        }
        return;
    }

    if (c == 27) {
        ansi_state = 1;
        return;
    }

    fb_console_erase_cursor();

    if (c == '\n') {
        cursor_x = 0;
        cursor_y += 8 * FONT_SCALE;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\b') {
        if (cursor_x >= 8 * FONT_SCALE) {
            cursor_x -= 8 * FONT_SCALE;
        }
    } else {
        fb_draw_char(c, cursor_x, cursor_y);
        cursor_x += 8 * FONT_SCALE;
        if (cursor_x >= fb.width) {
            cursor_x = 0;
            cursor_y += 8 * FONT_SCALE;
        }
    }

    while (cursor_y + 8 * FONT_SCALE > fb.height) {
        fb_console_scroll();
    }
}

void fb_console_write(const char *str)
{
    while (*str) {
        fb_console_putchar(*str++);
    }
}
