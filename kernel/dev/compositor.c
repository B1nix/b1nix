#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/fb_console.h>
#include <b1nix/sched.h>
#include <b1nix/mm.h>
#include <string.h>

struct window {
    int x;
    int y;
    int width;
    int height;
    u32 *buffer;
    struct window *next;
};

static u32 *backbuffer;
static usize backbuffer_size;
static struct window *window_list;

static void compositor_put_pixel(int x, int y, u32 color)
{
    u32 width = fb_console_width();
    u32 height = fb_console_height();
    if (!backbuffer || x < 0 || y < 0 || (u32)x >= width || (u32)y >= height) {
        return;
    }
    backbuffer[(u32)y * width + (u32)x] = color;
}

static void compositor_fill_rect(int x, int y, int w, int h, u32 color)
{
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            compositor_put_pixel(px, py, color);
        }
    }
}

static void compositor_render_scene(void)
{
    u32 width = fb_console_width();
    u32 height = fb_console_height();
    if (!backbuffer || width == 0 || height == 0) {
        return;
    }

    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            u32 shade = ((x * 255U) / (width ? width : 1)) & 0xffU;
            backbuffer[y * width + x] = 0x00101020U | (shade << 16);
        }
    }

    compositor_fill_rect((int)width - 220, 24, 180, 96, 0x002040f0);
    compositor_fill_rect(36, (int)height - 120, 320, 64, 0x00404040);

    for (struct window *w = window_list; w; w = w->next) {
        if (!w->buffer) continue;
        for (int wy = 0; wy < w->height; wy++) {
            for (int wx = 0; wx < w->width; wx++) {
                int x = w->x + wx;
                int y = w->y + wy;
                if (x < 0 || y < 0 || (u32)x >= width || (u32)y >= height) continue;
                compositor_put_pixel(x, y, w->buffer[(usize)wy * (usize)w->width + (usize)wx]);
            }
        }
    }
}

static void compositor_flush(void)
{
    volatile u8 *front = (volatile u8 *)fb_console_frontbuffer();
    if (!front || !backbuffer) {
        return;
    }

    u32 width = fb_console_width();
    u32 height = fb_console_height();
    u32 pitch = fb_console_pitch();

    for (u32 y = 0; y < height; y++) {
        volatile u32 *dst = (volatile u32 *)(front + (u64)y * pitch);
        u32 *src = backbuffer + (u64)y * width;
        for (u32 x = 0; x < width; x++) {
            dst[x] = src[x];
        }
    }
}

static void compositor_thread(void *arg)
{
    (void)arg;
    if (!fb_console_ready()) {
        console_write("compositor: no framebuffer, thread exiting\n");
        return;
    }

    console_write("compositor: started loop with backbuffer\n");

    while (1) {
        compositor_render_scene();
        compositor_flush();
        scheduler_sleep_ticks(2);
    }
}

void compositor_init(void)
{
    const struct boot_info *info = bootinfo_get();
    if (!info->has_framebuffer || !fb_console_ready() || fb_console_bpp() != 32) {
        console_write("compositor: disabled (framebuffer unavailable or unsupported bpp)\n");
        return;
    }

    backbuffer_size = (usize)fb_console_width() * (usize)fb_console_height() * sizeof(u32);
    backbuffer = (u32 *)kmalloc(backbuffer_size);
    if (!backbuffer) {
        console_write("compositor: backbuffer alloc failed\n");
        return;
    }
    memset(backbuffer, 0, backbuffer_size);
    window_list = 0;

    if (kthread_create("compositor", compositor_thread, 0) < 0) {
        console_write("compositor: failed to create thread\n");
        return;
    }
    console_write("compositor: initialized\n");
}
