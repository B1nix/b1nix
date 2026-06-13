#include <b1nix/gui.h>
#include <stdint.h>
#include <time.h>

static void draw_digit(struct b1gui_window *w, int digit, int ox, int oy) {
	static const unsigned char segs[10] = {
	    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
	unsigned s = segs[digit % 10];
	for (int y = 0; y < 38; y++)
		for (int x = 0; x < 22; x++) {
			int on = ((s & 1) && y < 4) || ((s & 2) && x > 17 && y < 20) ||
			         ((s & 4) && x > 17 && y > 17) || ((s & 8) && y > 33) ||
			         ((s & 16) && x < 4 && y > 17) ||
			         ((s & 32) && x < 4 && y < 20) ||
			         ((s & 64) && y > 16 && y < 21);
			if (on)
				w->pixels[(oy + y) * w->width + ox + x] = 0x006FE7D2u;
		}
}

int main(void) {
	struct b1gui_window win;
	if (b1gui_connect(&win) || b1gui_create_window(&win, 170, 72, "Clock"))
		return 1;
	for (;;) {
		for (unsigned i = 0; i < win.width * win.height; i++)
			win.pixels[i] = 0x00121B2Au;
		time_t now = time(0);
		struct tm *tm = localtime(&now);
		int values[4] = {tm->tm_hour / 10, tm->tm_hour % 10,
		                 tm->tm_min / 10, tm->tm_min % 10};
		for (int i = 0; i < 4; i++)
			draw_digit(&win, values[i], 12 + i * 38 + (i >= 2 ? 8 : 0), 17);
		for (int y = 28; y < 45; y += 12)
			for (int x = 82; x < 86; x++)
				win.pixels[y * win.width + x] = 0x00F6C177u;
		b1gui_present(&win, 0, 0, win.width, win.height);

		struct b1gui_event event;
		for (int i = 0; i < 10; i++)
			if (b1gui_next_event(&win, &event, 100) == 1 &&
			    event.type == B1GUI_EV_CLOSE) {
				b1gui_destroy(&win);
				return 0;
			}
	}
}
