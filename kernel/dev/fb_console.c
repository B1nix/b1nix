#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/fb_console.h>
#include <b1nix/klog.h>
#include <b1nix/fb_panel.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
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
/* First row the console may use. Everything above it is frozen — the boot
 * splash is drawn once and then stays put while the log scrolls underneath.
 * Zero, and the console owns the whole panel as it always did. */
static u32 fb_top;
#ifndef FB_CONSOLE_FONT_SCALE
#define FB_CONSOLE_FONT_SCALE 1
#endif
static u32 FONT_SCALE = FB_CONSOLE_FONT_SCALE;

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

/* Hand a touched rectangle to the panel. Nothing happens on a machine whose
 * framebuffer is the scanout; on one whose display is a device, this is what
 * makes the characters appear. */
static u32 dirty_x0, dirty_y0, dirty_x1, dirty_y1;
/* Set by fb_console_attach(): a display that has to be told what changed
 * (a DRM framebuffer) instead of one that is memory on a screen. */
static void (*fb_present_hook)(unsigned x, unsigned y, unsigned w, unsigned h);

static void fb_present_rect(u32 x, u32 y, u32 w, u32 h)
{
	if (!fb_ptr || !w || !h)
		return;
	if (dirty_x1 == 0 && dirty_y1 == 0) {
		dirty_x0 = x; dirty_y0 = y;
		dirty_x1 = x + w; dirty_y1 = y + h;
		return;
	}
	if (x < dirty_x0) dirty_x0 = x;
	if (y < dirty_y0) dirty_y0 = y;
	if (x + w > dirty_x1) dirty_x1 = x + w;
	if (y + h > dirty_y1) dirty_y1 = y + h;
}

/* Send whatever has been drawn since the last flush. A panel that has to hand
 * frames to a device pays one command per flush rather than one per glyph,
 * which is the difference between a console and a slideshow. */
static void fb_console_present(u32 x0, u32 y0, u32 x1, u32 y1)
{
	if (fb_present_hook)
		fb_present_hook(x0, y0, x1 - x0, y1 - y0);
	else
		fb_panel_present((const void *)fb_ptr, fb.pitch, fb.width, fb.height,
		                 x0, y0, x1 - x0, y1 - y0);
}

void fb_console_flush(void)
{
	if (!fb_ptr || dirty_x1 == 0)
		return;
	fb_console_present(dirty_x0, dirty_y0, dirty_x1, dirty_y1);
	dirty_x0 = dirty_y0 = dirty_x1 = dirty_y1 = 0;
}

/* Presenting is not for the console lock.
 *
 * console_write holds the console lock with interrupts off, and a display
 * that has to be told what changed (a DRM framebuffer) is told through the
 * driver, which takes mutexes and may sleep: presenting from there tripped
 * "lkpi: cannot block" and wedged the machine the moment the console had
 * moved onto i915. So a write only notes that there is something to show,
 * and a thread with the right to sleep shows it, sixty times a second at
 * most -- the same split fbcon makes between drawing and the deferred-io
 * flush. Before the scheduler runs nothing presents, and nothing needs to:
 * the bootloader's framebuffer is memory on a screen. */
static volatile int fb_flush_wanted;
static int fb_flusher_up;
static int fb_panic; /* set once a panic owns the screen */

void fb_console_request_flush(void)
{
	/* Straight to the screen only until the flusher runs, and while a panic
	 * owns it: then nothing else will ever present. Afterwards a bootloader
	 * framebuffer is deferred like a DRM one. Presented per write, a scroll
	 * copied the whole screen for every line of text -- eight megabytes a
	 * line at 1080p, under the console lock with interrupts off -- and the
	 * console crawled as fast as that copy did. */
	if ((!fb_present_hook && !fb_flusher_up) || fb_panic) {
		fb_console_flush();
		return;
	}
	__atomic_store_n(&fb_flush_wanted, 1, __ATOMIC_RELEASE);
}

static void fb_console_flusher(void *arg)
{
	(void)arg;
	for (;;) {
		scheduler_sleep_ticks(sched_tick_hz() / 60 ? sched_tick_hz() / 60 : 1);
		if (!__atomic_exchange_n(&fb_flush_wanted, 0, __ATOMIC_ACQ_REL))
			continue;
		/* The damage is taken and cleared under the console lock, so a line
		 * drawn meanwhile is not lost from it; the copy runs outside, where a
		 * DRM present may sleep. A glyph drawn during the copy is shown by the
		 * next pass, which its own write has already asked for. */
		u64 flags;
		u32 x0, y0, x1, y1;

		console_lock_acquire_irqsave(&flags);
		x0 = dirty_x0; y0 = dirty_y0; x1 = dirty_x1; y1 = dirty_y1;
		dirty_x0 = dirty_y0 = dirty_x1 = dirty_y1 = 0;
		console_lock_release_irqrestore(flags);
		if (fb_ptr && x1)
			fb_console_present(x0, y0, x1, y1);
	}
}

void fb_console_start_flusher(void)
{
	if (fb_flusher_up)
		return;
	fb_flusher_up = 1;
	kthread_create("fbflush", fb_console_flusher, 0);
}

/* The log so far, redrawn onto a console that was hidden while a compositor
 * had the display. Same replay fb_console_attach does for a display that
 * arrived after boot. */
void fb_console_replay(void)
{
	char *log;

	if (!fb_ptr)
		return;
	cursor_x = 0;
	cursor_y = fb_top;
	fb_console_clear();
	log = kmalloc(65536);
	if (log) {
		usize n = klog_read(log, 65536);
		for (usize i = 0; i < n; i++)
			fb_console_putchar(log[i]);
		kfree(log);
	}
	fb_console_request_flush();
}

/* Everything, for a display that just came back (a compositor exited). */
void fb_console_present_all(void)
{
	if (!fb_ptr)
		return;
	fb_present_rect(0, 0, fb.width, fb.height);
	fb_console_flush();
}

/* A display that arrived after boot: a DRM device's framebuffer, vmapped.
 * One buffer, drawn in place; `present` tells the device which rectangle
 * changed. The log so far is replayed onto it, so a screen that was black
 * through the boot shows the boot. */
void fb_console_attach(void *pixels, unsigned pitch, unsigned width,
                       unsigned height, unsigned bpp,
                       void (*present)(unsigned x, unsigned y, unsigned w, unsigned h))
{
	if (!pixels || !width || !height)
		return;
	fb.address = (u64)(usize)pixels;
	fb.pitch = pitch;
	fb.width = width;
	fb.height = height;
	fb.bpp = bpp;
	fb_ptr = (volatile u8 *)pixels;
	/* Draw in RAM, copy to the device. The pixels handed over here are an
	 * aperture mapping on a real GPU -- write-combined, and reading it back
	 * is uncached: a scroll that reads the screen through it moved eight
	 * megabytes per line and took the boot from 2.6 s to 18 s. The shadow
	 * is ordinary memory; fb_flush_rect copies what changed. */
	fb_shadow_size = (usize)pitch * height;
	fb_shadow_frames = (fb_shadow_size + PAGE_SIZE - 1) / PAGE_SIZE;
	{
		u64 phys = pmm_alloc_frames(fb_shadow_frames);
		fb_shadow = phys ? (u8 *)(usize)(vmm_direct_map_base() + phys) : (u8 *)pixels;
		if (!phys)
			fb_shadow_frames = 0;
	}
	fb_present_hook = present;
	fb_top = 0;
	cursor_x = 0;
	cursor_y = 0;
	memset(fb_shadow, 0, fb_shadow_size);
	if ((const volatile u8 *)fb_shadow != fb_ptr)
		memset(pixels, 0, fb_shadow_size);
	fb_present_rect(0, 0, width, height);
	/* The transcript so far. */
	char *log = kmalloc(65536);
	if (log) {
		usize n = klog_read(log, 65536);
		for (usize i = 0; i < n; i++)
			fb_console_putchar(log[i]);
		kfree(log);
	}
	fb_console_request_flush();
}

static void fb_flush_rect(u32 x, u32 y, u32 w, u32 h)
{
	if (!fb_ptr || !fb_shadow || w == 0 || h == 0) return;
	if ((const volatile u8 *)fb_shadow == fb_ptr) return; /* one buffer, nothing to copy */
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
	int in_ram = 0;

	if (fb_ptr)
		return; /* already up */
	if (fb_panel_probe(&fb, &in_ram) != 0)
		return;

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
	/* Write-combining, not Device.
	 *
	 * A scanout buffer is the textbook write-combining surface: nothing in it
	 * is a register, no write has a side effect, and the order two pixels land
	 * in does not matter. Mapped Device (which is what vmm_map_mmio gives a
	 * caller that does not ask for WC) every single pixel becomes its own
	 * strongly-ordered, uncombinable bus transaction — on a 1080x2520 panel a
	 * full-screen clear is 2.7 million of them, and the console does not
	 * scroll so much as crawl. VMM_WC is Normal-NC on arm64 and a PAT WC slot
	 * on x86: writes merge into bursts, and the panel sees the same bytes. */
	fb_ptr = in_ram ? (volatile u8 *)(usize)fb.address
	                : (volatile u8 *)vmm_map_mmio(fb.address, (usize)fb_size,
	                                              VMM_WRITABLE | VMM_WC);
	if (!fb_ptr) {
		console_write("fb: mmio map failed\n");
		return;
	}
	fb_shadow_size = (usize)fb_size;
	if (in_ram) {
		/* The panel's buffer already IS memory: scrolling can move rows
		 * around in place, and a second copy of it would buy nothing. */
		fb_shadow = (u8 *)(usize)fb.address;
	} else {
		fb_shadow_frames = (fb_shadow_size + PAGE_SIZE - 1) / PAGE_SIZE;
		u64 fb_shadow_phys = pmm_alloc_frames(fb_shadow_frames);
		if (fb_shadow_phys) {
			/* Reached through the direct map, not the MMIO window. These are
			 * ordinary frames from the page allocator — the console memsets
			 * and memmoves them like the normal memory they are — but
			 * vmm_map_mmio hands out Device mappings to any caller that does
			 * not ask for write-combining, so the shadow was uncacheable and
			 * every glyph was drawn at bus speed before it was ever blitted. */
			fb_shadow = (u8 *)(usize)(vmm_direct_map_base() + fb_shadow_phys);
		}
	}
	if (!fb_shadow) {
		console_write("fb: shadow alloc failed (using direct mmio)\n");
		fb_shadow_size = 0;
		fb_shadow_frames = 0;
	} else {
		memset(fb_shadow, 0, fb_shadow_size);
	}

	/* Automatically adapt font scale for vertical / mobile panels */
	if (fb.height > fb.width + fb.width / 4) {
		if (fb.width >= 1000)
			FONT_SCALE = 3;
		else if (fb.width >= 500)
			FONT_SCALE = 2;
	}

	cursor_x = 0;
	cursor_y = fb_top;

	// Clear screen (below anything frozen)
	for (u32 y = fb_top; y < fb.height; y++) {
		for (u32 x = 0; x < fb.width; x++) {
			put_pixel(x, y, bg_color);
		}
	}
	/* Push it to the panel. put_pixel writes the shadow, and only regions
	 * something later draws over get presented — so every part of the screen
	 * the splash and the log never touch kept whatever the bootloader (and
	 * boot.S's FBMARK bands) left there, for the life of the boot. */
	fb_present_rect(0, 0, fb.width, fb.height);
}

void fb_console_clear(void)
{
    if (!fb_ptr) return;

	cursor_x = 0;
	cursor_y = fb_top;

	if (fb_dev_claimed()) return;

	for (u32 y = fb_top; y < fb.height; y++) {
		for (u32 x = 0; x < fb.width; x++) {
			put_pixel(x, y, bg_color);
		}
	}
	fb_present_rect(0, 0, fb.width, fb.height);
	fb_console_request_flush();
}



/* Change the magnification at runtime. The boot splash is ~100 columns of
 * ASCII art: at the scale that makes the log readable on a phone the panel
 * only holds 45, so it wraps into noise. Draw it at 1, then go back. Resets
 * the cursor, since a cell that changed size makes the old position
 * meaningless. */
void fb_console_set_font_scale(u32 scale)
{
	if (scale < 1)
		scale = 1;
	if (scale > 8)
		scale = 8;
	FONT_SCALE = scale;
	cursor_x = 0;
	cursor_y = fb_top;
}

/* Keep everything drawn so far. The console starts on the next line and never
 * touches anything above it again — not when it scrolls, not when it clears.
 * This is what leaves the boot splash on screen for the life of the boot. */
void fb_console_freeze_top(void)
{
	fb_top = cursor_y;
	cursor_x = 0;
}

void fb_console_set_top(u32 top_y)
{
	fb_top = top_y;
	cursor_x = 0;
	cursor_y = top_y;
}

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

/*
 * No scrolling: at the bottom the screen starts again from the top.
 *
 * Scrolling moved the whole screen down in the shadow and then copied the
 * whole screen out to the device -- about eight megabytes on a 1280x800
 * panel -- and it did that from console_write, with interrupts off and the
 * console lock held. One scroll per printed line made it the single largest
 * interrupts-off cost in the kernel: 28 G cycles over 3832 lines of a
 * desktop start-up, 87% of all the time this kernel spent with interrupts
 * disabled. Batching the scroll into groups of lines cut that by two thirds
 * and it was still the top entry.
 *
 * A kernel console is not a terminal with scrollback -- the whole log is in
 * the ring and on the serial line -- so the cheap answer is to stop moving
 * pixels at all: clear and continue from the top, one screen-sized fill per
 * screenful of text instead of a copy per line.
 */
/* fb_panic (declared above): set once a panic owns the screen; see
 * fb_console_panic_begin(). */
static int fb_panic_full;
static int fb_panic_off; /* painting faulted: hands off */

static void fb_console_wrap(void)
{
    u32 bytes_per_line = fb.pitch;
    u32 rows;

    /* A panic keeps its first screenful -- the reason, registers and the
     * backtrace -- instead of wiping it for the task table that follows. The
     * rest goes to the serial port and the log ring as always. */
    if (fb_panic) {
        fb_panic_full = 1;
        return;
    }

    if (fb.height < fb_top + FB_CELL_H)
        return;
    rows = fb.height - fb_top;

    if (fb_shadow) {
        u8 *dst = fb_shadow + ((u64)fb_top * bytes_per_line);
        u64 bg64 = ((u64)bg_color << 32) | bg_color;
        u64 *w = (u64 *)(void *)dst;
        u64 words = ((u64)rows * bytes_per_line) / 8;

        for (u64 i = 0; i < words; i++)
            w[i] = bg64;
        fb_flush_rect(0, fb_top, fb.width, rows);
        fb_present_rect(0, fb_top, fb.width, rows);
    } else {
        fb_console_clear();
    }
    cursor_x = 0;
    cursor_y = fb_top;
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
    fb_present_rect(cursor_x, y_base, 8 * FONT_SCALE, FONT_SCALE);
    fb_console_request_flush(); /* from the tick: never present here */
}

/*
 * Nobody is looking: a compositor owns the display.
 *
 * The text console kept drawing every character and scrolling the whole
 * screen while a desktop was on the panel -- work that costs about a
 * megabyte of copying per line, under the console lock with interrupts off,
 * for pixels no one can see. The log still reaches the serial line and the
 * ring; only the drawing stops, and the ring is replayed when the display
 * comes back.
 */
static int fb_console_hidden;

void fb_console_set_hidden(int hidden)
{
    if (fb_console_hidden == !!hidden)
        return;
    fb_console_hidden = !!hidden;
    if (!fb_console_hidden)
        fb_console_replay();
}

void fb_console_putchar(char c)
{
    if (!fb_ptr) return;
    if (fb_panic) {
        if (fb_panic_full) return;
    } else if (fb_dev_claimed() || fb_console_hidden) {
        return;
    }

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
                cursor_y = fb_top + row * FB_CELL_H;
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
        fb_present_rect(cursor_x, cursor_y, 8 * FONT_SCALE, FB_CELL_H);
        cursor_x += 8 * FONT_SCALE;
        if (cursor_x >= fb.width) {
            cursor_x = 0;
            cursor_y += FB_CELL_H;
        }
    }

    if (cursor_y + FB_CELL_H > fb.height)
        fb_console_wrap();
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

/*
 * The panic screen's drawing surface (kernel/dev/panic_screen.c).
 *
 * A panic draws whatever else owns the display: a compositor that hid the
 * console or mapped /dev/fb0 is not going to run again. Nothing here
 * allocates, locks or presents -- a device that needs to be told about new
 * pixels is told by nobody, because telling it may sleep; a scanned-out buffer
 * shows them as they are written.
 */
static void panic_px(u32 x, u32 y, u32 color)
{
	u64 off = (u64)y * fb.pitch + (u64)x * (fb.bpp / 8);

	if (fb.bpp == 24) {
		volatile u8 *p = fb_ptr + off;

		p[0] = (u8)color;
		p[1] = (u8)(color >> 8);
		p[2] = (u8)(color >> 16);
	} else {
		*(volatile u32 *)(void *)(fb_ptr + off) = color;
	}
	if (fb_shadow && (const volatile u8 *)fb_shadow != fb_ptr &&
	    off + 4 <= fb_shadow_size)
		*(u32 *)(void *)(fb_shadow + off) = color;
}

int fb_console_panic_begin(void)
{
	if (fb_panic_off || !fb_ptr || !fb.width || !fb.height ||
	    (fb.bpp != 32 && fb.bpp != 24))
		return -1;
	fb_panic = 1;
	fb_panic_full = 0;
	fb_console_hidden = 0;
	ansi_state = 0;
	cursor_visible = 0;
	ansi_cursor_hidden = 1;
	g_font = &font8x8_basic[0][0];
	g_font_h = 8;
	g_font_stride = 8;
	g_font_count = 128;
	return 0;
}

void fb_console_panic_fill(u32 x, u32 y, u32 w, u32 h, u32 color)
{
	if (!fb_panic || fb_panic_off || x >= fb.width || y >= fb.height)
		return;
	if (w > fb.width - x)
		w = fb.width - x;
	if (h > fb.height - y)
		h = fb.height - y;
	for (u32 py = y; py < y + h; py++)
		for (u32 px = x; px < x + w; px++)
			panic_px(px, py, color);
}

u32 fb_console_panic_text(u32 x, u32 y, const char *s, u32 scale, u32 color)
{
	if (!fb_panic || fb_panic_off || !s || !scale)
		return x;
	for (; *s; s++, x += 8 * scale) {
		const u8 *glyph;

		if (x + 8 * scale > fb.width)
			break;
		glyph = font8x8_basic[(unsigned char)*s & 0x7f];
		for (u32 gy = 0; gy < 8; gy++) {
			for (u32 gx = 0; gx < 8; gx++) {
				if (!((glyph[gy] >> (7 - gx)) & 1))
					continue;
				fb_console_panic_fill(x + gx * scale, y + gy * scale,
				                      scale, scale, color);
			}
		}
	}
	return x;
}

void fb_console_panic_region(u32 top, u32 scale, u32 fg, u32 bg)
{
	if (!fb_panic || fb_panic_off)
		return;
	if (scale < 1)
		scale = 1;
	FONT_SCALE = scale;
	fg_color = fg;
	bg_color = bg;
	fb_top = top < fb.height ? top : fb.height;
	cursor_x = 0;
	cursor_y = fb_top;
	fb_panic_full = fb_top + FB_CELL_H > fb.height;
}

/* Painting faulted: leave the display alone for the rest of the panic. */
void fb_console_panic_abort(void)
{
	fb_panic_off = 1;
	fb_panic = 1;
	fb_panic_full = 1;
}

/*
 * Test mode: prove that a character written to the console really lands in the
 * framebuffer. Draws one glyph and counts the pixels in its cell that are not
 * the background — a console that is wired up but drawing nothing (which is
 * what a stubbed-out fb_console_putchar looks like from the outside) leaves
 * the cell empty and fails here.
 */
void fb_console_selftest(void)
{
	u32 cx, cy, lit = 0;
	u32 bytes_per_px;

	if (!fb_ptr || fb_dev_claimed()) {
		console_write("M107-FB: skip render (no framebuffer console)\n");
		return;
	}

	cx = cursor_x;
	cy = cursor_y;
	bytes_per_px = fb.bpp / 8;
	fb_console_putchar('X');
	fb_console_flush();

	for (u32 py = cy; py < cy + FB_CELL_H && py < fb.height; py++) {
		for (u32 px = cx; px < cx + 8 * FONT_SCALE && px < fb.width; px++) {
			u64 off = (u64)py * fb.pitch + (u64)px * bytes_per_px;

			if ((*(volatile u32 *)(void *)(fb_ptr + off) & 0x00ffffffu) !=
			    (bg_color & 0x00ffffffu))
				lit++;
		}
	}
	fb_console_putchar('\n');
	console_write(lit ? "M107-FB: ok render\n" : "M107-FB: FAIL render\n");
}

/*
 * Note on the aarch64 panel: the drawing above is cheap (stores into a RAM
 * frame) but fb_panel_present hands that frame to virtio-gpu, and the device's
 * queue has other users - the DRM layer and a userspace display server. Mirror
 * every line of kernel log through it and those two fight over the same queue,
 * which is why console_putc on that architecture writes to serial and the VT
 * buffer only. The console renders and presents on demand (fb_console_selftest
 * proves the whole path); it is not a second, unsynchronised writer to the GPU.
 */
