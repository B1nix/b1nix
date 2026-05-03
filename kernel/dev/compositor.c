#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/sched.h>
#include <b1nix/mm.h>

static void compositor_thread(void *arg)
{
    (void)arg;
    const struct boot_info *info = bootinfo_get();
    
    if (!info->has_framebuffer) {
        console_write("compositor: no framebuffer, thread exiting\n");
        return;
    }

    console_write("compositor: started loop\n");

    volatile u32 *fb = (volatile u32 *)(usize)(0xffffa00000000000ULL);
    u32 width = info->framebuffer.width;
    u32 height = info->framebuffer.height;
    u32 pitch = info->framebuffer.pitch;
    u8 bpp = info->framebuffer.bpp;
    
    // Draw a small blue box to prove the compositor can write directly
    u32 box_w = 100;
    u32 box_h = 100;
    u32 start_x = width - box_w - 20;
    u32 start_y = 20;

    while (1) {
        for (u32 y = 0; y < box_h; y++) {
            for (u32 x = 0; x < box_w; x++) {
                u8 *pixel = (u8 *)fb + ((start_y + y) * pitch) + ((start_x + x) * (bpp / 8));
                *(u32 *)pixel = 0x000000FF; // Blue
            }
        }
        
        scheduler_sleep_ticks(10);
    }
}

void compositor_init(void)
{
    kthread_create("compositor", compositor_thread, 0);
}
