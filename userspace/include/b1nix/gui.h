#ifndef _B1NIX_GUI_H
#define _B1NIX_GUI_H

#include <stdint.h>

#define B1GUI_EV_CLOSE 1
#define B1GUI_EV_POINTER_ENTER 2
#define B1GUI_EV_POINTER_LEAVE 3
#define B1GUI_EV_POINTER_MOTION 4
#define B1GUI_EV_POINTER_BUTTON 5
#define B1GUI_EV_KEY 6
#define B1GUI_EV_FOCUS_ENTER 7
#define B1GUI_EV_FOCUS_LEAVE 8
#define B1GUI_EV_FRAME 9

struct b1gui_event {
	uint16_t type;
	uint32_t args[8];
	unsigned nargs;
};

struct b1gui_window {
	int fd;
	int buffer_fd;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	uint32_t registry_id;
	uint32_t compositor_id;
	uint32_t shm_id;
	uint32_t wm_base_id;
	uint32_t seat_id;
	uint32_t surface_id;
	uint32_t xdg_surface_id;
	uint32_t buffer_id;
	uint32_t pool_id;
	uint32_t toplevel_id;
	uint32_t pointer_id;
	uint32_t keyboard_id;
	uint32_t next_id;
	uint8_t inbuf[512];
	unsigned inlen;
};

int b1gui_connect(struct b1gui_window *win);
int b1gui_create_window(struct b1gui_window *win, uint32_t width,
                        uint32_t height, const char *title);
int b1gui_present(struct b1gui_window *win, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height);
int b1gui_next_event(struct b1gui_window *win, struct b1gui_event *event,
                     int timeout_ms);
uint32_t b1gui_checksum(struct b1gui_window *win);
void b1gui_destroy(struct b1gui_window *win);

#endif
