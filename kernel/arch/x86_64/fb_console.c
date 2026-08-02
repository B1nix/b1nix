#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/fb_console.h>
#include <b1nix/mm.h>
#include <b1nix/types.h>
#include <string.h>
#include "font8x8.h"

/* M47: while a userspace display server owns /dev/fb0 the kernel text console
 * must not touch the framebuffer — otherwise its blinking cursor and any
 * kernel/login output scribble over the composited desktop. Defined in
 * kernel/dev/fb.c (always linked on x86_64/x86). */
int fb_dev_claimed(void);

static struct boot_framebuffer fb;
static u32 cursor_x;
static u32 cursor_y;
static u32 fg_color = 0x00FFFFFF;
static u32 bg_color = 0x00000000;
volatile u8 *fb_ptr = 0;
static u8 *fb_shadow = 0;
static usize fb_shadow_size = 0;
static usize fb_shadow_frames = 0;

static inline void put_pixel(u32 x, u32 y, u32 color)
{
	if (!fb_ptr || x >= fb.width || y >= fb.height) {
		return;
	}
	u64 off = ((u64)y * fb.pitch) + ((u64)x * (fb.bpp / 8));
	volatile u8 *pixel = fb_ptr + off;
	*(volatile u32 *)pixel = color;
	if (fb_shadow && off + sizeof(u32) <= fb_shadow_size) {
		*(u32 *)(void *)(fb_shadow + off) = color;
	}
}

static void fb_flush_rect(u32 x, u32 y, u32 w, u32 h)
{
	if (!fb_ptr || !fb_shadow || w == 0 || h == 0) return;
	if (x >= fb.width || y >= fb.height) return;
	if (x + w > fb.width) w = fb.width - x;
	if (y + h > fb.height) h = fb.height - y;

	u32 bytes_per_px = fb.bpp / 8;
	for (u32 py = y; py < y + h; py++) {
		u64 row_off = (u64)py * fb.pitch;
		u64 start = row_off + (u64)x * bytes_per_px;
		usize len = (usize)w * bytes_per_px;
		if (start + len > fb_shadow_size) break;
		memcpy((void *)(fb_ptr + start), fb_shadow + start, len);
	}
}

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

	u64 fb_size = (u64)fb.height * fb.pitch;
	fb_ptr = (volatile u8 *)vmm_map_mmio(fb.address, (usize)fb_size, VMM_WRITABLE | VMM_PCD);
	if (!fb_ptr) {
		console_write("fb: mmio map failed\n");
		return;
	}
	fb_shadow_size = (usize)fb_size;
	fb_shadow_frames = (fb_shadow_size + PAGE_SIZE - 1) / PAGE_SIZE;
	u64 fb_shadow_phys = pmm_alloc_frames(fb_shadow_frames);
	if (fb_shadow_phys) {
		fb_shadow = (u8 *)vmm_map_mmio(fb_shadow_phys, fb_shadow_size, VMM_WRITABLE);
	}
	if (!fb_shadow) {
		console_write("fb: shadow alloc failed (using direct mmio)\n");
		fb_shadow_size = 0;
		fb_shadow_frames = 0;
	} else {
		memset(fb_shadow, 0, fb_shadow_size);
	}

	cursor_x = 0;
	cursor_y = 0;

	// Clear screen
	for (u32 y = 0; y < fb.height; y++) {
		for (u32 x = 0; x < fb.width; x++) {
			put_pixel(x, y, bg_color);
		}
	}
}

void fb_console_clear(void)
{
    if (!fb_ptr) return;

	cursor_x = 0;
	cursor_y = 0;

	if (fb_dev_claimed()) return;

	for (u32 y = 0; y < fb.height; y++) {
		for (u32 x = 0; x < fb.width; x++) {
			put_pixel(x, y, bg_color);
		}
	}
}

static const u32 FONT_SCALE = 1;

/* M107: the console face is replaceable at runtime (setfont / PIO_FONT /
 * KDFONTOP). The builtin 8x8 bitmap is the default; a loaded face is 8 pixels
 * wide, up to FB_FONT_MAX_H rows tall, and carries up to 256 glyphs. `stride`
 * is the per-glyph byte pitch in the caller's buffer: 32 for the PIO_FONT
 * layout, equal to the height for a packed one. */
#define FB_FONT_MAX_H 16
static const u8 *g_font = &font8x8_basic[0][0];
static u32 g_font_h = 8;
static u32 g_font_stride = 8;
static u32 g_font_count = 128;

/* Cell height in pixels — the vertical advance and the scroll unit. */
#define FB_CELL_H (g_font_h * FONT_SCALE)

const u8 *fb_console_builtin_font(void) { return &font8x8_basic[0][0]; }

void fb_console_font_metrics(u32 *height, u32 *count)
{
	if (height) *height = g_font_h;
	if (count) *count = g_font_count;
}

int fb_console_set_font(const u8 *glyphs, u32 height, u32 stride, u32 count)
{
	if (!glyphs) {
		g_font = &font8x8_basic[0][0];
		g_font_h = 8;
		g_font_stride = 8;
		g_font_count = 128;
		return 0;
	}
	if (height == 0 || height > FB_FONT_MAX_H || stride < height ||
	    count == 0 || count > 256)
		return -1;
	g_font = glyphs;
	g_font_h = height;
	g_font_stride = stride;
	g_font_count = count;
	return 0;
}

static void fb_draw_char(char c, u32 x, u32 y)
{
    if ((unsigned char)c >= g_font_count) c = '?';
    const u8 *glyph = g_font + (usize)(unsigned char)c * g_font_stride;
    u32 bytes_per_px = fb.bpp / 8;

    for (u32 cy = 0; cy < g_font_h; cy++) {
        for (u32 dy = 0; dy < FONT_SCALE; dy++) {
            u32 py = y + cy * FONT_SCALE + dy;
            if (py >= fb.height) continue;
            for (u32 cx = 0; cx < 8; cx++) {
                u8 bit = (glyph[cy] >> (7 - cx)) & 1;
                u32 color = bit ? fg_color : bg_color;
                u32 px_base = x + cx * FONT_SCALE;
                for (u32 dx = 0; dx < FONT_SCALE; dx++) {
                    u32 px = px_base + dx;
                    if (px >= fb.width) continue;
                    u64 off = (u64)py * fb.pitch + (u64)px * bytes_per_px;
                    *(volatile u32 *)(void *)(fb_ptr + off) = color;
                    if (fb_shadow && off + sizeof(u32) <= fb_shadow_size) {
                        *(u32 *)(void *)(fb_shadow + off) = color;
                    }
                }
            }
        }
    }
}

static void fb_console_scroll(void)
{
    u32 line_height = FB_CELL_H;
    if (fb.height < line_height) return;

    u32 bytes_per_line = fb.pitch;
    u32 scroll_height = fb.height - line_height;

    if (fb_shadow) {
        u8 *dst = fb_shadow;
        u8 *src = fb_shadow + (line_height * bytes_per_line);
        memmove(dst, src, scroll_height * bytes_per_line);

        u8 *tail = dst + scroll_height * bytes_per_line;
        u64 bg64 = ((u64)bg_color << 32) | bg_color;
        u64 *tail64 = (u64 *)(void *)tail;
        u32 words = (line_height * bytes_per_line) / 8;
        for (u32 i = 0; i < words; i++) tail64[i] = bg64;
        fb_flush_rect(0, 0, fb.width, fb.height);
    } else {
        /* Safety fallback: avoid MMIO readback scrolling when no RAM shadow exists. */
        fb_console_clear();
        return;
    }

    cursor_y -= line_height;
}

static int cursor_visible = 0;

static void fb_console_erase_cursor(void)
{
    if (!fb_ptr || !cursor_visible) return;
    
    u32 y_base = cursor_y + (g_font_h - 1) * FONT_SCALE;
    for (u32 dy = 0; dy < FONT_SCALE; dy++) {
        for (u32 dx = 0; dx < 8 * FONT_SCALE; dx++) {
            u32 px = cursor_x + dx;
            u32 py = y_base + dy;
            if (px < fb.width && py < fb.height) {
                put_pixel(px, py, bg_color);
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
    if (!fb_ptr || ansi_cursor_hidden || fb_dev_claimed()) return;

    cursor_visible = !cursor_visible;
    u32 color = cursor_visible ? fg_color : bg_color;

    u32 y_base = cursor_y + (g_font_h - 1) * FONT_SCALE;
    for (u32 dy = 0; dy < FONT_SCALE; dy++) {
        for (u32 dx = 0; dx < 8 * FONT_SCALE; dx++) {
            u32 px = cursor_x + dx;
            u32 py = y_base + dy;
            if (px < fb.width && py < fb.height) {
                put_pixel(px, py, color);
            }
        }
    }
}

void fb_console_putchar(char c)
{
    if (!fb_ptr || fb_dev_claimed()) return;

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
            int v = ansi_params[ansi_param_idx] * 10 + (c - '0');
            ansi_params[ansi_param_idx] = (v > 9999) ? 9999 : v;
        } else if (c == ';') {
            if (ansi_param_idx < 7) ansi_param_idx++;
            else ansi_state = 0;
        } else if (c == '?') {
            /* ignore for now */
        } else {
            if (c == 'J') {
                if (ansi_params[0] == 2) fb_console_clear();
            } else if (c == 'H') {
                int row = ansi_params[0] > 0 ? ansi_params[0] - 1 : 0;
                int col = ansi_params[1] > 0 ? ansi_params[1] - 1 : 0;
                cursor_y = row * FB_CELL_H;
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
        cursor_y += FB_CELL_H;
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
            cursor_y += FB_CELL_H;
        }
    }

    while (cursor_y + FB_CELL_H > fb.height) {
        fb_console_scroll();
    }
}

void fb_console_write(const char *str)
{
    while (*str) {
        fb_console_putchar(*str++);
    }
}

int fb_console_ready(void)
{
	return fb_ptr != 0;
}

u32 fb_console_width(void) { return fb.width; }
u32 fb_console_height(void) { return fb.height; }
u32 fb_console_pitch(void) { return fb.pitch; }
u8 fb_console_bpp(void) { return fb.bpp; }
volatile void *fb_console_frontbuffer(void) { return (volatile void *)fb_ptr; }
