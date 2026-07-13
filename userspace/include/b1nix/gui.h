#ifndef _B1NIX_GUI_H
#define _B1NIX_GUI_H

#include <stdint.h>

/* ── standard events ── */
#define B1GUI_EV_CLOSE          1
#define B1GUI_EV_POINTER_ENTER  2
#define B1GUI_EV_POINTER_LEAVE  3
#define B1GUI_EV_POINTER_MOTION 4
#define B1GUI_EV_POINTER_BUTTON 5
#define B1GUI_EV_KEY            6
#define B1GUI_EV_FOCUS_ENTER    7
#define B1GUI_EV_FOCUS_LEAVE    8
#define B1GUI_EV_FRAME          9

/* ── extended events (displayd → app) ── */
#define B1GUI_EV_MENU_ITEM     20  /* args[0] = item_id from register_menu */
#define B1GUI_EV_DOCK_RESTORE  21  /* dock icon clicked while minimized */

/* ── menu item flags ── */
#define B1GUI_MENU_ENABLED   0x00
#define B1GUI_MENU_DISABLED  0x01
#define B1GUI_MENU_SEPARATOR 0x02
#define B1GUI_MENU_CHECKED   0x04

/* Maximum menu items per app */
#define B1GUI_MAX_MENU_ITEMS 16

struct b1gui_menu_item {
	uint16_t id;       /* returned in B1GUI_EV_MENU_ITEM.args[0] */
	uint16_t flags;    /* B1GUI_MENU_* */
	char     label[32]; /* visible label, e.g. "New File" */
	char     accel[12]; /* keyboard shortcut text, e.g. "Ctrl+N" (display only) */
};

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

/* ── core API ── */
int b1gui_connect(struct b1gui_window *win);
int b1gui_create_window(struct b1gui_window *win, uint32_t width,
                        uint32_t height, const char *title);
int b1gui_present(struct b1gui_window *win, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height);
int b1gui_next_event(struct b1gui_window *win, struct b1gui_event *event,
                     int timeout_ms);
uint32_t b1gui_checksum(struct b1gui_window *win);
void b1gui_destroy(struct b1gui_window *win);

/* ── app menu registration (macOS-style: items appear in top bar) ── */
int b1gui_register_menu(struct b1gui_window *win,
                        const struct b1gui_menu_item *items, int count);

#endif
