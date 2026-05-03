#include <tinyunix/bootinfo.h>
#include <tinyunix/console.h>
#include <tinyunix/mm.h>
#include <tinyunix/types.h>
#include "font8x8.h"

static struct boot_framebuffer fb;
static u32 cursor_x;
static u32 cursor_y;
static u32 fg_color = 0x00FFFFFF;
static u32 bg_color = 0x00000000;
static volatile u32 *fb_ptr;

void fb_console_init(void)
{
	const struct boot_info *info = bootinfo_get();
	if (!info->has_framebuffer) {
		return;
	}

	fb = info->framebuffer;
	
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

static const u32 FONT_SCALE = 2;

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

void fb_console_putchar(char c)
{
    if (!fb_ptr) return;

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

    if (cursor_y >= fb.height) {
        // Simple scroll (not implemented yet, just wrap around for now)
        cursor_y = 0;
    }
}

void fb_console_write(const char *str)
{
    while (*str) {
        fb_console_putchar(*str++);
    }
}
