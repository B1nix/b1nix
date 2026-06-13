/* M49: prove the upstream libwayland-client API against displayd. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

struct globals {
	int compositor;
	int shm;
	struct wl_seat *seat;
	int keymap;
};

static void global(void *data, struct wl_registry *registry, uint32_t name,
		   const char *interface, uint32_t version)
{
	struct globals *globals = data;
	(void)registry;
	(void)name;
	(void)version;

	if (!strcmp(interface, "wl_compositor"))
		globals->compositor = 1;
	else if (!strcmp(interface, "wl_shm"))
		globals->shm = 1;
	else if (!strcmp(interface, "wl_seat"))
		globals->seat = wl_registry_bind(registry, name, &wl_seat_interface,
						version < 5 ? version : 5);
}

static void global_remove(void *data, struct wl_registry *registry,
			  uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	global,
	global_remove,
};

static void keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
		   int fd, uint32_t size)
{
	struct globals *globals = data;
	(void)keyboard;
	if (format == WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP && size == 0)
		globals->keymap = 1;
	close(fd);
}

static void repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate,
			int32_t delay)
{
	(void)data;
	(void)keyboard;
	(void)rate;
	(void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keymap,
	.repeat_info = repeat_info,
};

int main(void)
{
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_keyboard *keyboard = NULL;
	struct globals globals = {0};

	display = wl_display_connect("/run/wayland-0");
	if (!display) {
		puts("M49-LIBWL: fail connect");
		return 1;
	}
	registry = wl_display_get_registry(display);
	if (!registry ||
	    wl_registry_add_listener(registry, &registry_listener, &globals) < 0 ||
	    wl_display_roundtrip(display) < 0 ||
	    !globals.compositor || !globals.shm || !globals.seat) {
		puts("M49-LIBWL: fail registry");
		if (registry)
			wl_registry_destroy(registry);
		wl_display_disconnect(display);
		return 1;
	}

	keyboard = wl_seat_get_keyboard(globals.seat);
	if (!keyboard ||
	    wl_keyboard_add_listener(keyboard, &keyboard_listener, &globals) < 0 ||
	    wl_display_roundtrip(display) < 0 || !globals.keymap) {
		puts("M49-LIBWL: fail keymap");
		if (keyboard)
			wl_keyboard_destroy(keyboard);
		wl_seat_destroy(globals.seat);
		wl_registry_destroy(registry);
		wl_display_disconnect(display);
		return 1;
	}

	puts("M49-LIBWL: ok keymap");
	puts("M49-LIBWL: ok upstream-client");
	wl_keyboard_destroy(keyboard);
	wl_seat_destroy(globals.seat);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);
	return 0;
}
