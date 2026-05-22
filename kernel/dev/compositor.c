#include <b1nix/bootinfo.h>
#include <b1nix/compositor.h>
#include <b1nix/console.h>
#include <b1nix/fb_console.h>
#include <b1nix/mm.h>
#include <b1nix/ps2_mouse.h>
#include <b1nix/sched.h>
#include <b1nix/virtio_gpu.h>
#include <string.h>

struct compositor_window {
    int x;
    int y;
    int width;
    int height;
    u32 *buffer;
    int dirty;
    struct compositor_window *next;
};

static u32 *backbuffer;
static usize backbuffer_size;
static struct compositor_window *window_list;

static int scene_initialized;
static int moving_box_x;
static int moving_box_y;
static int moving_box_dx = 4;
static int moving_box_dy = 3;
static int moving_box_w = 96;
static int moving_box_h = 72;

static u32 dirty_x0;
static u32 dirty_y0;
static u32 dirty_x1;
static u32 dirty_y1;
static int dirty_valid;
static int force_full_redraw;

static u64 frames_total;
static u64 frames_partial;
static u64 frames_full;
static u64 virtio_fallbacks;
static u64 virtio_fallbacks_prev;

static u32 dirty_full_threshold_percent = 40U;
static int cursor_x;
static int cursor_y;
static int cursor_prev_x = -1;
static int cursor_prev_y = -1;
static int demo_animation_enabled;
static int compositor_started;
static int compositor_dirty_event;

#define CURSOR_SIZE 10
#define DIRTY_THRESHOLD_MIN_PERCENT 20U
#define DIRTY_THRESHOLD_MAX_PERCENT 70U
#define DIRTY_THRESHOLD_STEP_UP 5U
#define DIRTY_THRESHOLD_STEP_DOWN 2U
#define COMPOSITOR_STATS_PERIOD 300U
#define COMPOSITOR_WAKE_CHAN ((void *)&compositor_dirty_event)

static void compositor_mark_dirty_rect(int x, int y, int w, int h)
{
    u32 fbw = fb_console_width();
    u32 fbh = fb_console_height();
    if (w <= 0 || h <= 0 || fbw == 0 || fbh == 0) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if ((u32)x >= fbw || (u32)y >= fbh) return;
    if ((u32)(x + w) > fbw) w = (int)fbw - x;
    if ((u32)(y + h) > fbh) h = (int)fbh - y;
    if (w <= 0 || h <= 0) return;

    u32 rx0 = (u32)x;
    u32 ry0 = (u32)y;
    u32 rx1 = (u32)(x + w);
    u32 ry1 = (u32)(y + h);
    if (!dirty_valid) {
        dirty_x0 = rx0;
        dirty_y0 = ry0;
        dirty_x1 = rx1;
        dirty_y1 = ry1;
        dirty_valid = 1;
    } else {
        if (rx0 < dirty_x0) dirty_x0 = rx0;
        if (ry0 < dirty_y0) dirty_y0 = ry0;
        if (rx1 > dirty_x1) dirty_x1 = rx1;
        if (ry1 > dirty_y1) dirty_y1 = ry1;
    }

    u64 total_area = (u64)fbw * (u64)fbh;
    u64 dirty_area = (u64)(dirty_x1 - dirty_x0) * (u64)(dirty_y1 - dirty_y0);
    if (total_area > 0 && dirty_area * 100ULL >= total_area * dirty_full_threshold_percent) {
        force_full_redraw = 1;
    }
    compositor_dirty_event = 1;
}

static void compositor_update_threshold(void)
{
    u64 new_fallbacks = virtio_fallbacks - virtio_fallbacks_prev;
    virtio_fallbacks_prev = virtio_fallbacks;

    if (new_fallbacks >= 10) {
        if (dirty_full_threshold_percent > DIRTY_THRESHOLD_MIN_PERCENT + DIRTY_THRESHOLD_STEP_UP)
            dirty_full_threshold_percent -= DIRTY_THRESHOLD_STEP_UP;
        else
            dirty_full_threshold_percent = DIRTY_THRESHOLD_MIN_PERCENT;
        return;
    }

    if (new_fallbacks == 0) {
        if (dirty_full_threshold_percent + DIRTY_THRESHOLD_STEP_DOWN < DIRTY_THRESHOLD_MAX_PERCENT)
            dirty_full_threshold_percent += DIRTY_THRESHOLD_STEP_DOWN;
        else
            dirty_full_threshold_percent = DIRTY_THRESHOLD_MAX_PERCENT;
    }
}

static void compositor_put_pixel(int x, int y, u32 color)
{
    u32 width = fb_console_width();
    u32 height = fb_console_height();
    if (!backbuffer || x < 0 || y < 0 || (u32)x >= width || (u32)y >= height) return;
    backbuffer[(u32)y * width + (u32)x] = color;
}

static void compositor_fill_rect(int x, int y, int w, int h, u32 color)
{
    compositor_mark_dirty_rect(x, y, w, h);
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            compositor_put_pixel(px, py, color);
        }
    }
}

static u32 gradient_color_at(int x)
{
    u32 width = fb_console_width();
    u32 shade = ((u32)x * 255U) / (width ? width : 1);
    return 0x00101020U | ((shade & 0xffU) << 16);
}

static void compositor_fill_gradient_rect(int x, int y, int w, int h)
{
    compositor_mark_dirty_rect(x, y, w, h);
    for (int py = y; py < y + h; py++) {
        if (py < 0 || (u32)py >= fb_console_height()) continue;
        for (int px = x; px < x + w; px++) {
            if (px < 0 || (u32)px >= fb_console_width()) continue;
            compositor_put_pixel(px, py, gradient_color_at(px));
        }
    }
}

static void compositor_draw_mouse_cursor_front(volatile u8 *front, u32 pitch, int x, int y)
{
    if (!front) return;
    u32 width = fb_console_width();
    u32 height = fb_console_height();
    for (int i = 0; i < CURSOR_SIZE; i++) {
        int px1 = x + i;
        int py1 = y;
        if (px1 >= 0 && py1 >= 0 && (u32)px1 < width && (u32)py1 < height) {
            volatile u32 *p = (volatile u32 *)(front + (u64)py1 * pitch) + px1;
            *p = 0x00ffffff;
        }
        int px2 = x;
        int py2 = y + i;
        if (px2 >= 0 && py2 >= 0 && (u32)px2 < width && (u32)py2 < height) {
            volatile u32 *p = (volatile u32 *)(front + (u64)py2 * pitch) + px2;
            *p = 0x00ffffff;
        }
    }
    for (int i = 0; i < CURSOR_SIZE / 2; i++) {
        int px = x + i;
        int py = y + i;
        if (px >= 0 && py >= 0 && (u32)px < width && (u32)py < height) {
            volatile u32 *p = (volatile u32 *)(front + (u64)py * pitch) + px;
            *p = 0x00ffffff;
        }
    }
}

static void compositor_render_windows(void)
{
    u32 width = fb_console_width();
    u32 height = fb_console_height();
    for (struct compositor_window *w = window_list; w; w = w->next) {
        if (!w->buffer) continue;
        for (int wy = 0; wy < w->height; wy++) {
            for (int wx = 0; wx < w->width; wx++) {
                int x = w->x + wx;
                int y = w->y + wy;
                if (x < 0 || y < 0 || (u32)x >= width || (u32)y >= height) continue;
                compositor_put_pixel(x, y, w->buffer[(usize)wy * (usize)w->width + (usize)wx]);
            }
        }
        if (w->dirty) {
            compositor_mark_dirty_rect(w->x, w->y, w->width, w->height);
            w->dirty = 0;
        }
    }
}

static void compositor_render_scene(void)
{
    u32 width = fb_console_width();
    u32 height = fb_console_height();
    if (!backbuffer || width == 0 || height == 0) return;

    struct ps2_mouse_state ms;
    ps2_mouse_get_state(&ms);
    cursor_x = ms.x;
    cursor_y = ms.y;

    if (!scene_initialized) {
        for (u32 y = 0; y < height; y++) {
            for (u32 x = 0; x < width; x++) {
                backbuffer[y * width + x] = gradient_color_at((int)x);
            }
        }
        compositor_fill_rect((int)width - 220, 24, 180, 96, 0x002040f0);
        compositor_fill_rect(36, (int)height - 120, 320, 64, 0x00404040);
        moving_box_x = (int)width / 4;
        moving_box_y = (int)height / 4;
        compositor_fill_rect(moving_box_x, moving_box_y, moving_box_w, moving_box_h, 0x00f0c020);
        compositor_mark_dirty_rect(0, 0, (int)width, (int)height);
        cursor_prev_x = cursor_x;
        cursor_prev_y = cursor_y;
        scene_initialized = 1;
        return;
    }

    if (demo_animation_enabled) {
        int old_x = moving_box_x;
        int old_y = moving_box_y;
        compositor_fill_gradient_rect(old_x, old_y, moving_box_w, moving_box_h);

        moving_box_x += moving_box_dx;
        moving_box_y += moving_box_dy;
        if (moving_box_x < 0) { moving_box_x = 0; moving_box_dx = -moving_box_dx; }
        if (moving_box_y < 0) { moving_box_y = 0; moving_box_dy = -moving_box_dy; }
        if ((u32)(moving_box_x + moving_box_w) >= width) {
            moving_box_x = (int)width - moving_box_w - 1;
            moving_box_dx = -moving_box_dx;
        }
        if ((u32)(moving_box_y + moving_box_h) >= height) {
            moving_box_y = (int)height - moving_box_h - 1;
            moving_box_dy = -moving_box_dy;
        }
        compositor_fill_rect(moving_box_x, moving_box_y, moving_box_w, moving_box_h, 0x00f0c020);
    }

    compositor_render_windows();

    if (cursor_prev_x >= 0 && cursor_prev_y >= 0)
        compositor_mark_dirty_rect(cursor_prev_x, cursor_prev_y, CURSOR_SIZE, CURSOR_SIZE);
    compositor_mark_dirty_rect(cursor_x, cursor_y, CURSOR_SIZE, CURSOR_SIZE);
    cursor_prev_x = cursor_x;
    cursor_prev_y = cursor_y;
}

static void compositor_log_stats(void)
{
    if (frames_total % COMPOSITOR_STATS_PERIOD != 0) return;
    compositor_update_threshold();
    console_write("compositor: frames total=");
    console_write_dec(frames_total);
    console_write(" partial=");
    console_write_dec(frames_partial);
    console_write(" full=");
    console_write_dec(frames_full);
    console_write(" fallbacks=");
    console_write_dec(virtio_fallbacks);
    console_write(" thr=");
    console_write_dec(dirty_full_threshold_percent);
    console_write("%\n");
}

static void compositor_flush(void)
{
    if (!backbuffer || !dirty_valid) return;

    u32 width = fb_console_width();
    u32 height = fb_console_height();
    u32 dirty_x = force_full_redraw ? 0 : dirty_x0;
    u32 dirty_y = force_full_redraw ? 0 : dirty_y0;
    u32 dirty_w = force_full_redraw ? width : (dirty_x1 - dirty_x0);
    u32 dirty_h = force_full_redraw ? height : (dirty_y1 - dirty_y0);
    if (dirty_x >= width || dirty_y >= height || dirty_w == 0 || dirty_h == 0) {
        dirty_valid = 0;
        force_full_redraw = 0;
        return;
    }
    if (dirty_x + dirty_w > width) dirty_w = width - dirty_x;
    if (dirty_y + dirty_h > height) dirty_h = height - dirty_y;

    frames_total++;
    if (force_full_redraw) frames_full++; else frames_partial++;

    if (virtio_gpu_ready()) {
        if (virtio_gpu_present(backbuffer, width, height, dirty_x, dirty_y, dirty_w, dirty_h,
                               cursor_x, cursor_y, 1) == 0) {
            dirty_valid = 0;
            force_full_redraw = 0;
            compositor_log_stats();
            return;
        }
        virtio_fallbacks++;
    }

    volatile u8 *front = (volatile u8 *)fb_console_frontbuffer();
    if (!front) return;

    u32 pitch = fb_console_pitch();
    for (u32 y = dirty_y; y < dirty_y + dirty_h; y++) {
        volatile u32 *dst = (volatile u32 *)(front + (u64)y * pitch) + dirty_x;
        u32 *src = backbuffer + (u64)y * width + dirty_x;
        for (u32 x = 0; x < dirty_w; x++) dst[x] = src[x];
    }
    compositor_draw_mouse_cursor_front(front, pitch, cursor_x, cursor_y);

    dirty_valid = 0;
    force_full_redraw = 0;
    compositor_log_stats();
}

static void compositor_thread(void *arg)
{
    (void)arg;
    if (!fb_console_ready()) {
        console_write("compositor: no framebuffer, thread exiting\n");
        return;
    }

    if (!bootinfo_has_flag("b1nix.ui=1") && !bootinfo_has_flag("ui=1") && !bootinfo_has_flag("gfx_demo")) {
        console_write("compositor: UI or demo flags not set, compositor thread exiting\n");
        return;
    }

    console_write("compositor: started loop with backbuffer\n");
    compositor_started = 1;
    while (1) {
        if (!dirty_valid && !demo_animation_enabled) {
            scheduler_block_on(COMPOSITOR_WAKE_CHAN);
            continue;
        }
        compositor_dirty_event = 0;
        compositor_render_scene();
        compositor_flush();
        if (demo_animation_enabled) scheduler_sleep_ticks(2);
    }
}

void compositor_init(void)
{
    const struct boot_info *info = bootinfo_get();
    if (!info->has_framebuffer || !fb_console_ready() || fb_console_bpp() != 32) {
        console_write("compositor: disabled (framebuffer unavailable or unsupported bpp)\n");
        return;
    }
    if (fb_console_pitch() != fb_console_width() * 4U) {
        console_write("compositor: note pitch != width*4 (using explicit pitch on flush path)\n");
    }

    backbuffer_size = (usize)fb_console_width() * (usize)fb_console_height() * sizeof(u32);
    usize backbuffer_frames = (backbuffer_size + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 backbuffer_phys = pmm_alloc_frames(backbuffer_frames);
    backbuffer = backbuffer_phys ? (u32 *)(usize)(backbuffer_phys + vmm_direct_map_base()) : 0;
    if (!backbuffer) {
        console_write("compositor: backbuffer alloc failed\n");
        return;
    }
    memset(backbuffer, 0, backbuffer_size);
    window_list = 0;
    demo_animation_enabled = bootinfo_has_flag("gfx_demo");
    compositor_dirty_event = 1;
    dirty_valid = 1;
    dirty_x0 = 0;
    dirty_y0 = 0;
    dirty_x1 = fb_console_width();
    dirty_y1 = fb_console_height();

    if (kthread_create("compositor", compositor_thread, 0) < 0) {
        console_write("compositor: failed to create thread\n");
        return;
    }
    console_write("compositor: initialized\n");
}

void compositor_wake(void)
{
    compositor_dirty_event = 1;
    if (compositor_started) {
        scheduler_wake_all(COMPOSITOR_WAKE_CHAN);
    }
}

struct compositor_window *compositor_window_create(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) return 0;
    struct compositor_window *w = (struct compositor_window *)kzalloc(sizeof(*w));
    if (!w) return 0;
    w->buffer = (u32 *)kzalloc((usize)width * (usize)height * sizeof(u32));
    if (!w->buffer) {
        kfree(w);
        return 0;
    }
    w->x = x;
    w->y = y;
    w->width = width;
    w->height = height;
    w->dirty = 1;
    w->next = window_list;
    window_list = w;
    compositor_mark_dirty_rect(x, y, width, height);
    compositor_wake();
    return w;
}

void compositor_window_destroy(struct compositor_window *win)
{
    if (!win) return;
    struct compositor_window **pp = &window_list;
    while (*pp && *pp != win) pp = &(*pp)->next;
    if (*pp == win) *pp = win->next;
    compositor_mark_dirty_rect(win->x, win->y, win->width, win->height);
    compositor_wake();
    if (win->buffer) kfree(win->buffer);
    kfree(win);
}

void compositor_window_move(struct compositor_window *win, int x, int y)
{
    if (!win) return;
    compositor_mark_dirty_rect(win->x, win->y, win->width, win->height);
    win->x = x;
    win->y = y;
    win->dirty = 1;
    compositor_mark_dirty_rect(win->x, win->y, win->width, win->height);
    compositor_wake();
}

void compositor_window_resize(struct compositor_window *win, int width, int height)
{
    if (!win || width <= 0 || height <= 0) return;
    u32 *new_buf = (u32 *)kzalloc((usize)width * (usize)height * sizeof(u32));
    if (!new_buf) return;

    int min_w = width < win->width ? width : win->width;
    int min_h = height < win->height ? height : win->height;
    for (int y = 0; y < min_h; y++) {
        memcpy(new_buf + (usize)y * (usize)width,
               win->buffer + (usize)y * (usize)win->width,
               (usize)min_w * sizeof(u32));
    }

    compositor_mark_dirty_rect(win->x, win->y, win->width, win->height);
    kfree(win->buffer);
    win->buffer = new_buf;
    win->width = width;
    win->height = height;
    win->dirty = 1;
    compositor_mark_dirty_rect(win->x, win->y, win->width, win->height);
    compositor_wake();
}

void compositor_window_raise(struct compositor_window *win)
{
    if (!win || window_list == win) return;
    struct compositor_window **pp = &window_list;
    while (*pp && *pp != win) pp = &(*pp)->next;
    if (*pp != win) return;
    *pp = win->next;
    win->next = window_list;
    window_list = win;
    win->dirty = 1;
    compositor_mark_dirty_rect(win->x, win->y, win->width, win->height);
    compositor_wake();
}

void compositor_window_invalidate(struct compositor_window *win)
{
    if (!win) return;
    win->dirty = 1;
    compositor_mark_dirty_rect(win->x, win->y, win->width, win->height);
    compositor_wake();
}

u32 *compositor_window_buffer(struct compositor_window *win)
{
    return win ? win->buffer : 0;
}
