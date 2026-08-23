#include <b1nix/bootinfo.h>
#include <b1nix/fb_panel.h>

/* On a PC the bootloader has already set a mode and told us where its linear
 * framebuffer is; writing to it is what puts pixels on the screen. */
int fb_panel_probe(struct boot_framebuffer *out, int *in_ram)
{
	const struct boot_info *info = bootinfo_get();

	if (!info->has_framebuffer)
		return -1;
	*out = info->framebuffer;
	*in_ram = 0;
	return 0;
}

void fb_panel_present(const void *pixels, u32 pitch, u32 width, u32 height,
                      u32 x, u32 y, u32 w, u32 h)
{
	(void)pixels; (void)pitch; (void)width; (void)height;
	(void)x; (void)y; (void)w; (void)h;
}
