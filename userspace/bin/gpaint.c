#include <b1nix/display.h>
#include <b1nix/gui.h>
#include <b1nix/input.h>
#include <stdint.h>

int main(void) {
	struct b1gui_window win;
	if (b1gui_connect(&win) || b1gui_create_window(&win, 280, 190, "Paint"))
		return 1;
	for (unsigned y = 0; y < win.height; y++)
		for (unsigned x = 0; x < win.width; x++)
			win.pixels[y * win.width + x] =
			    y < 24 ? 0x00233145u : 0x00F4F0E8u;
	for (unsigned x = 8; x < 72; x++)
		for (unsigned y = 6; y < 18; y++)
			win.pixels[y * win.width + x] =
			    x < 28 ? 0x00E44C65u : x < 50 ? 0x0044A8E0u : 0x0049C878u;
	b1gui_present(&win, 0, 0, win.width, win.height);

	int drawing = 0;
	unsigned px = 0, py = 0;
	for (;;) {
		struct b1gui_event event;
		if (b1gui_next_event(&win, &event, 100) != 1)
			continue;
		if (event.object_id == win.toplevel_id &&
		    event.opcode == B1D_EV_TOPLEVEL_CLOSE)
			break;
		if (event.object_id != B1D_OBJ_SEAT)
			continue;
		if (event.opcode == B1D_EV_SEAT_POINTER_BUTTON &&
		    event.nargs >= 2 && event.args[0] == B1NIX_BTN_LEFT)
			drawing = event.args[1] != 0;
		if ((event.opcode == B1D_EV_SEAT_POINTER_ENTER ||
		     event.opcode == B1D_EV_SEAT_POINTER_MOTION) &&
		    event.nargs >= 2) {
			px = event.args[event.opcode == B1D_EV_SEAT_POINTER_ENTER ? 1 : 0];
			py = event.args[event.opcode == B1D_EV_SEAT_POINTER_ENTER ? 2 : 1];
		}
		if (drawing && px < win.width && py >= 24 && py < win.height) {
			for (int dy = -2; dy <= 2; dy++)
				for (int dx = -2; dx <= 2; dx++) {
					int x = (int)px + dx, y = (int)py + dy;
					if (x >= 0 && y >= 24 && x < (int)win.width &&
					    y < (int)win.height)
						win.pixels[(unsigned)y * win.width + (unsigned)x] =
						    0x00D83B55u;
				}
			b1gui_present(&win, px > 2 ? px - 2 : 0, py - 2, 5, 5);
		}
	}
	b1gui_destroy(&win);
	return 0;
}
