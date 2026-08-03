#include <stdio.h>
#include <wayland-server.h>

int main(void)
{
	struct wl_display *display = wl_display_create();
	struct wl_event_loop *loop;

	if (!display) {
		puts("M49-LIBWLS: fail display");
		return 1;
	}
	loop = wl_display_get_event_loop(display);
	if (!loop || wl_event_loop_dispatch(loop, 0) < 0) {
		puts("M49-LIBWLS: fail event-loop");
		wl_display_destroy(display);
		return 1;
	}
	wl_display_destroy(display);
	puts("M49-LIBWLS: ok server-core");
	return 0;
}
