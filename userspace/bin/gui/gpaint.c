#include <b1nix/gui.h>
#include <b1nix/input.h>
#include <stdint.h>

enum paint_menu {
	PMENU_CLEAR = 1,
	PMENU_RED   = 2,
	PMENU_GREEN = 3,
	PMENU_BLUE  = 4,
};

static uint32_t draw_color = 0x00D83B55u;

int main(void) {
	struct b1gui_window win;
	if (b1gui_connect(&win) || b1gui_create_window(&win, 280, 190, "Paint"))
		return 1;
	struct b1gui_menu_item items[] = {
	    {PMENU_CLEAR, 0, "Clear Canvas", ""},
	    {0,           B1GUI_MENU_SEPARATOR, "", ""},
	    {PMENU_RED,   0, "Red",   ""},
	    {PMENU_GREEN, 0, "Green", ""},
	    {PMENU_BLUE,  0, "Blue",  ""},
	};
	b1gui_register_menu(&win, items, 5);
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
		if (event.type == B1GUI_EV_CLOSE)
			break;
		if (event.type == B1GUI_EV_MENU_ITEM) {
			uint32_t id = event.args[0];
			if (id == PMENU_CLEAR) {
				for (unsigned y = 24; y < win.height; y++)
					for (unsigned x = 0; x < win.width; x++)
						win.pixels[y * win.width + x] = 0x00F4F0E8u;
				b1gui_present(&win, 0, 24, win.width, win.height - 24);
			} else if (id == PMENU_RED)   draw_color = 0x00D83B55u;
			else if (id == PMENU_GREEN) draw_color = 0x0049C878u;
			else if (id == PMENU_BLUE)  draw_color = 0x0044A8E0u;
			continue;
		}
		if (event.type == B1GUI_EV_POINTER_BUTTON &&
		    event.nargs >= 2 && event.args[0] == B1NIX_BTN_LEFT)
			drawing = event.args[1] != 0;
		if ((event.type == B1GUI_EV_POINTER_ENTER ||
		     event.type == B1GUI_EV_POINTER_MOTION) &&
		    event.nargs >= 2) {
			px = event.args[0];
			py = event.args[1];
		}
		if (drawing && px < win.width && py >= 24 && py < win.height) {
			for (int dy = -2; dy <= 2; dy++)
				for (int dx = -2; dx <= 2; dx++) {
					int x = (int)px + dx, y = (int)py + dy;
					if (x >= 0 && y >= 24 && x < (int)win.width &&
					    y < (int)win.height)
						win.pixels[(unsigned)y * win.width + (unsigned)x] =
						    draw_color;
				}
			b1gui_present(&win, px > 2 ? px - 2 : 0, py - 2, 5, 5);
		}
	}
	b1gui_destroy(&win);
	return 0;
}
