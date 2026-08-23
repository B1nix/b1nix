#include <b1nix/bootinfo.h>
#include <b1nix/fb_panel.h>
#include <b1nix/mm.h>
#include <b1nix/virtio_gpu.h>

/*
 * A QEMU virt machine has no bootloader-set framebuffer: the display is a
 * virtio-gpu device that is handed whole frames rather than scanned out of a
 * region the CPU writes. So the console gets a plain RAM buffer to draw into
 * and every rectangle it touches is sent to the device.
 */

static u32 *g_frame;
static u32 g_width, g_height;

int fb_panel_probe(struct boot_framebuffer *out, int *in_ram)
{
	u32 w = 0, h = 0;
	usize bytes;

	if (!virtio_gpu_ready())
		return -1;
	virtio_gpu_get_mode(&w, &h);
	if (!w || !h)
		return -1;

	bytes = (usize)w * h * 4;
	if (!g_frame) {
		u64 phys = pmm_alloc_frames((bytes + PAGE_SIZE - 1) / PAGE_SIZE);

		if (!phys)
			return -1;
		/* Ordinary RAM, reached through the direct map. Mapping it as MMIO
		 * would give it Device attributes, and this buffer is memset and
		 * memmoved like the normal memory it is. */
		g_frame = (u32 *)(usize)(DIRECT_MAP_BASE + phys);
	}
	g_width = w;
	g_height = h;

	out->address = (u64)(usize)g_frame;
	out->width = w;
	out->height = h;
	out->pitch = w * 4;
	out->bpp = 32;
	*in_ram = 1;
	return 0;
}

void fb_panel_present(const void *pixels, u32 pitch, u32 width, u32 height,
                      u32 x, u32 y, u32 w, u32 h)
{
	(void)pitch;
	if (!g_frame || pixels != (const void *)g_frame)
		return;
	virtio_gpu_present((const u32 *)pixels, width, height, x, y, w, h, 0, 0, 0);
}
