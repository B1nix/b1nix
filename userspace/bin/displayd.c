/*
 * displayd — the b1nix userspace display server (M47 Phase 2).
 *
 * Single-threaded poll loop over /run/wayland-0, connected clients, and
 * /dev/input/event0+1.
 * Composites client memfd surfaces damage-driven into the mmap'd /dev/fb0
 * shadow buffer and pushes dirty rectangles with B1NIX_FBIOFLUSH.
 *
 * Protocol: Wayland core + xdg-shell subset. Server-side policy stays small:
 * first surface is centered, later ones cascade; 1-px border around each
 * surface; focus follows click (click raises); software crosshair cursor.
 *
 * Buffer release semantics (v1): the server reads a committed buffer on
 * every recomposite (cursor crossing, raise), so 0 is
 * only sent when the buffer stops being the committed one (replaced by a
 * new attach+commit, or the surface is destroyed).
 */
#include <b1nix/fb.h>
#include <b1nix/input.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syscall.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#include "font8x8.h"

#define MAX_CLIENTS 8
#define MAX_SURFACES 8
#define MAX_BUFFERS 8
#define MAX_TOPLEVELS 8
#define CURSOR_SIZE 10
#define TITLE_H 14
#define BG_COLOR 0x00202830u
#define BORDER_COLOR 0x00E0E0E0u
#define TITLE_COLOR 0x00355085u
#define TITLE_FOCUS_COLOR 0x005078B0u
#define PANEL_H 28
#define PANEL_COLOR 0x00111924u
#define PANEL_ACTIVE_COLOR 0x00354A5Du
#define MENU_BG_COLOR 0x00E8EDF1u
#define MENU_TEXT_COLOR 0x00131A20u
#define MENU_DISABLED_COLOR 0x00828B92u
#define MENU_HOVER_COLOR 0x003D78B5u
#define MENU_ITEM_H 18
#define MENU_W 176
#define CLOSE_COLOR 0x00E05263u
#define WAYLAND_SOCKET_PATH "/run/wayland-0"
#define MAX_MSG 256
#define MAX_WOBJECTS 64
#define MAX_WPOOLS 8

enum surface_action {
	SURFACE_ATTACH,
	SURFACE_DAMAGE,
	SURFACE_FRAME,
	SURFACE_COMMIT,
	SURFACE_DESTROY,
};

enum seat_event {
	SEAT_POINTER_ENTER,
	SEAT_POINTER_LEAVE,
	SEAT_POINTER_MOTION,
	SEAT_POINTER_BUTTON,
	SEAT_KEY,
	SEAT_FOCUS_ENTER,
	SEAT_FOCUS_LEAVE,
};

struct wl_hdr {
	uint32_t object_id;
	uint16_t opcode;
	uint16_t size;
};

enum wobject_type {
	WOBJ_REGISTRY,
	WOBJ_COMPOSITOR,
	WOBJ_SHM,
	WOBJ_SEAT,
	WOBJ_POINTER,
	WOBJ_KEYBOARD,
	WOBJ_REGION,
	WOBJ_XDG_WM_BASE,
	WOBJ_XDG_SURFACE,
	WOBJ_OUTPUT,
	WOBJ_DDM,
	WOBJ_DATA_SOURCE,
	WOBJ_DATA_DEVICE,
	WOBJ_DATA_OFFER,
};

struct wobject {
	int used;
	uint32_t id;
	int client;
	enum wobject_type type;
	uint32_t link;
	int configured;
	char mime[64]; /* data_source: the single offered MIME type */
};

struct wpool {
	int used;
	uint32_t id;
	int client;
	int fd;
	uint32_t size;
};

static void out(const char *s) { write(1, s, strlen(s)); }
static void out_dec(unsigned v) {
	char b[12];
	int i = 12;
	b[--i] = 0;
	do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v && i);
	out(&b[i]);
}

struct dbuffer {
	int used;
	uint32_t id;
	int client;
	void *mem;
	void *map_base;
	size_t map_size;
	uint32_t w, h, stride;
};

struct dsurface {
	int used;
	uint32_t id;
	int client;
	int x, y;
	int mapped;            /* committed at least once */
	unsigned placement;    /* desktop launch order */
	struct dbuffer *buf;   /* committed buffer */
	/* pending (until commit) */
	uint32_t pend_buffer_id;
	int pend_attach;
	int pend_dmg_valid;
	uint32_t dx0, dy0, dx1, dy1; /* pending damage, surface-local */
	uint32_t frame_cb;
	int has_frame_cb;
};

struct dtoplevel {
	int used;
	uint32_t id;
	int client;
	struct dsurface *surface;
	char title[32];
};

struct dclient {
	int used;
	int fd;
	uint8_t inbuf[512];
	unsigned inlen;
	int pending_fd;
};

static struct dclient clients[MAX_CLIENTS];
static struct dsurface surfaces[MAX_SURFACES];
static struct dbuffer buffers[MAX_BUFFERS];
static struct dtoplevel toplevels[MAX_TOPLEVELS];
static struct wobject wobjects[MAX_WOBJECTS];
static struct wpool wpools[MAX_WPOOLS];
static int zorder[MAX_SURFACES]; /* surface slot indices, bottom → top */
static int zcount;

static int fb_fd = -1;
static uint32_t *fb;
static uint32_t scr_w, scr_h;
static int ev_fds[2] = {-1, -1};
static int listen_fd = -1;
static int running = 1;
static unsigned frame_serial;
static int surfaces_created;

/* Top-bar clock, refreshed from the loop when the minute changes. */
static char clock_hhmm[10] = "--:--";
static int clock_last_min = -1;
static int clock_24h = 1;

enum panel_menu {
	MENU_NONE,
	MENU_SYSTEM,
	MENU_APP,
	MENU_FILE,
	MENU_EDIT,
	MENU_VIEW,
	MENU_CLOCK,
};

static enum panel_menu open_menu;
static int menu_hover = -1;

/* pointer state */
static int px, py;
static int enter_slot = -1; /* surface slot pointer is inside, -1 none */
static int focus_slot = -1;
static int drag_slot = -1;
static int left_alt;
static uint32_t input_serial;

/* ── wire helpers ── */

static void send_msg(int client, uint32_t obj, uint16_t opcode,
                     const uint32_t *words, unsigned nwords) {
	if (client < 0 || client >= MAX_CLIENTS || !clients[client].used)
		return;
	uint8_t msg[MAX_MSG];
	struct wl_hdr h;
	h.object_id = obj;
	h.opcode = opcode;
	h.size = (uint16_t)(sizeof(h) + nwords * 4);
	memcpy(msg, &h, sizeof(h));
	memcpy(msg + sizeof(h), words, nwords * 4);
	send(clients[client].fd, msg, h.size, 0);
}

static void send_msg_fd(int client, uint32_t obj, uint16_t opcode,
                        const uint32_t *words, unsigned nwords, int fd) {
	uint8_t msg[MAX_MSG];
	char control[CMSG_SPACE(sizeof(int))];
	struct wl_hdr h = {obj, opcode, (uint16_t)(sizeof(h) + nwords * 4)};
	struct iovec iov = {msg, h.size};
	struct msghdr mh;
	struct cmsghdr *cm;

	memcpy(msg, &h, sizeof(h));
	memcpy(msg + sizeof(h), words, nwords * 4);
	memset(&mh, 0, sizeof(mh));
	memset(control, 0, sizeof(control));
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = control;
	mh.msg_controllen = sizeof(control);
	cm = CMSG_FIRSTHDR(&mh);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &fd, sizeof(fd));
	sendmsg(clients[client].fd, &mh, 0);
}

static void keyboard_init(int client, uint32_t id) {
	int fd = memfd_create("wayland-keymap", MFD_CLOEXEC);
	uint32_t keymap[2] = {0, 0}; /* no_keymap: clients consume raw evdev codes */
	uint32_t repeat[2] = {25, 400};

	if (fd >= 0) {
		send_msg_fd(client, id, 0, keymap, 2, fd);
		close(fd);
	}
	send_msg(client, id, 5, repeat, 2);
}

static struct wobject *wobject_type_find(int client, enum wobject_type type) {
	for (int i = 0; i < MAX_WOBJECTS; i++)
		if (wobjects[i].used && wobjects[i].client == client &&
		    wobjects[i].type == type)
			return &wobjects[i];
	return 0;
}

static void send_seat_event(int client, uint16_t opcode, const uint32_t *words,
                            unsigned nwords) {
	struct wobject *obj = 0;
	uint32_t event[5];
	unsigned count = 0;
	uint16_t wl_opcode = 0;
	if (opcode <= SEAT_POINTER_BUTTON) {
		obj = wobject_type_find(client, WOBJ_POINTER);
		if (!obj)
			return;
		if (opcode == SEAT_POINTER_ENTER && nwords >= 3) {
			event[0] = input_serial;
			event[1] = words[0];
			event[2] = words[1] << 8;
			event[3] = words[2] << 8;
			count = 4;
		} else if (opcode == SEAT_POINTER_LEAVE && nwords >= 1) {
			wl_opcode = 1;
			event[0] = input_serial;
			event[1] = words[0];
			count = 2;
		} else if (opcode == SEAT_POINTER_MOTION && nwords >= 2) {
			wl_opcode = 2;
			event[0] = frame_serial;
			event[1] = words[0] << 8;
			event[2] = words[1] << 8;
			count = 3;
		} else if (opcode == SEAT_POINTER_BUTTON && nwords >= 2) {
			wl_opcode = 3;
			event[0] = input_serial;
			event[1] = frame_serial;
			event[2] = words[0];
			event[3] = words[1];
			count = 4;
		}
	} else {
		obj = wobject_type_find(client, WOBJ_KEYBOARD);
		if (!obj)
			return;
		if (opcode == SEAT_KEY && nwords >= 2) {
			wl_opcode = 3;
			event[0] = input_serial;
			event[1] = frame_serial;
			event[2] = words[0];
			event[3] = words[1];
			count = 4;
		} else if (opcode == SEAT_FOCUS_ENTER && nwords >= 1) {
			wl_opcode = 1;
			event[0] = input_serial;
			event[1] = words[0];
			event[2] = 0;
			count = 3;
		} else if (opcode == SEAT_FOCUS_LEAVE && nwords >= 1) {
			wl_opcode = 2;
			event[0] = input_serial;
			event[1] = words[0];
			count = 2;
		}
	}
	if (obj && count)
		send_msg(client, obj->id, wl_opcode, event, count);
}

static void wl_send_string(int client, uint32_t obj, uint16_t opcode,
                           const uint32_t *prefix, unsigned nprefix,
                           const char *text, const uint32_t *suffix,
                           unsigned nsuffix) {
	uint32_t words[32];
	unsigned len = (unsigned)strlen(text) + 1;
	unsigned ntext = (len + 3) / 4;
	if (nprefix + 1 + ntext + nsuffix > 32)
		return;
	memcpy(words, prefix, nprefix * 4);
	words[nprefix] = len;
	memset(&words[nprefix + 1], 0, ntext * 4);
	memcpy(&words[nprefix + 1], text, len);
	memcpy(&words[nprefix + 1 + ntext], suffix, nsuffix * 4);
	send_msg(client, obj, opcode, words, nprefix + 1 + ntext + nsuffix);
}

static struct wobject *wobject_find(int client, uint32_t id) {
	for (int i = 0; i < MAX_WOBJECTS; i++)
		if (wobjects[i].used && wobjects[i].client == client &&
		    wobjects[i].id == id)
			return &wobjects[i];
	return 0;
}

static struct wobject *wobject_add(int client, uint32_t id,
                                   enum wobject_type type, uint32_t link) {
	if (id < 2 || wobject_find(client, id))
		return 0;
	for (int i = 0; i < MAX_WOBJECTS; i++)
		if (!wobjects[i].used) {
			wobjects[i].used = 1;
			wobjects[i].id = id;
			wobjects[i].client = client;
			wobjects[i].type = type;
			wobjects[i].link = link;
			wobjects[i].configured = 0;
			return &wobjects[i];
		}
	return 0;
}

static void wl_delete_id(int client, uint32_t id) {
	send_msg(client, 1, 1, &id, 1);
}

static void wobject_remove(struct wobject *obj) {
	uint32_t id = obj->id;
	int client = obj->client;
	memset(obj, 0, sizeof(*obj));
	wl_delete_id(client, id);
}

static struct wpool *wpool_find(int client, uint32_t id) {
	for (int i = 0; i < MAX_WPOOLS; i++)
		if (wpools[i].used && wpools[i].client == client &&
		    wpools[i].id == id)
			return &wpools[i];
	return 0;
}

/* ── compositor ── */

static struct dsurface *slot_surface(int slot) {
	return (slot >= 0 && slot < MAX_SURFACES && surfaces[slot].used)
	           ? &surfaces[slot]
	           : 0;
}

static void draw_text_clipped(uint32_t *row, int screen_y, int rx, int rw,
                              int x0, int y0, const char *text,
                              uint32_t color) {
	if (screen_y < y0 || screen_y >= y0 + 8)
		return;
	int gy = screen_y - y0;
	for (int i = 0; text[i] && i < 28; i++) {
		unsigned char c = (unsigned char)text[i];
		const unsigned char *glyph = font8x8_basic[c < 128 ? c : '?'];
		for (int gx = 0; gx < 8; gx++) {
			int x = x0 + i * 8 + gx;
			if (x >= rx && x < rx + rw && (glyph[gy] & (1u << (7 - gx))))
				row[x] = color;
		}
	}
}

static struct dtoplevel *surface_toplevel(struct dsurface *s);

/* Title of the currently focused toplevel, for the macOS-style menu bar
 * ("active application" name). Falls back to a generic label. */
static const char *active_app_title(void) {
	struct dsurface *f = slot_surface(focus_slot);
	if (f) {
		struct dtoplevel *t = surface_toplevel(f);
		if (t && t->title[0])
			return t->title;
	}
	return "Finder";
}

struct panel_layout {
	int system_x, system_w;
	int app_x, app_w;
	int file_x, file_w;
	int edit_x, edit_w;
	int view_x, view_w;
	int clock_x, clock_w;
};

static int text_len(const char *s, int limit) {
	int n = 0;
	while (s[n] && n < limit)
		n++;
	return n;
}

static struct panel_layout get_panel_layout(void) {
	struct panel_layout p;
	int app_chars = text_len(active_app_title(), 14);
	p.system_x = 6;
	p.system_w = 6 * 8 + 8;
	p.app_x = p.system_x + p.system_w;
	p.app_w = app_chars * 8 + 16;
	p.file_x = p.app_x + p.app_w;
	p.file_w = 6 * 8;
	p.edit_x = p.file_x + p.file_w;
	p.edit_w = 6 * 8;
	p.view_x = p.edit_x + p.edit_w;
	p.view_w = 6 * 8;
	p.clock_w = strlen(clock_hhmm) * 8 + 16;
	p.clock_x = (int)scr_w - p.clock_w - 4;
	return p;
}

static int menu_item_count(enum panel_menu menu) {
	switch (menu) {
	case MENU_SYSTEM: return 6;
	case MENU_APP: return 3;
	case MENU_FILE: return 2;
	case MENU_EDIT: return 3;
	case MENU_VIEW: return 2;
	case MENU_CLOCK: return 3;
	default: return 0;
	}
}

static const char *menu_item_label(enum panel_menu menu, int item) {
	switch (menu) {
	case MENU_SYSTEM: {
		static const char *labels[] = {
		    "About b1nix", "Terminal", "Paint", "Clock App", "Next Window", "Close Window"};
		return labels[item];
	}
	case MENU_APP: {
		static const char *labels[] = {
		    "About This App", "Next Window", "Quit"};
		return labels[item];
	}
	case MENU_FILE: {
		static const char *labels[] = {"Close Window", "Quit"};
		return labels[item];
	}
	case MENU_EDIT: {
		static const char *labels[] = {"Cut", "Copy", "Paste"};
		return labels[item];
	}
	case MENU_VIEW: {
		static const char *labels[] = {"Next Window", "Bring to Front"};
		return labels[item];
	}
	case MENU_CLOCK: {
		if (item == 0) return "Open Clock App";
		if (item == 1) return clock_24h ? "[X] 24-hour clock" : "[ ] 24-hour clock";
		if (item == 2) return "b1nix local time";
		return "";
	}
	default:
		return "";
	}
}

static int menu_item_enabled(enum panel_menu menu, int item) {
	if (menu == MENU_CLOCK)
		return 1;
	if ((menu == MENU_APP || menu == MENU_FILE || menu == MENU_EDIT) &&
	    !slot_surface(focus_slot))
		return 0;
	if (menu == MENU_SYSTEM) {
		if (item == 4 && zcount == 0) return 0;
		if (item == 5 && !slot_surface(focus_slot)) return 0;
	}
	return 1;
}

static int menu_x(enum panel_menu menu) {
	struct panel_layout p = get_panel_layout();
	int x;
	switch (menu) {
	case MENU_SYSTEM: x = p.system_x; break;
	case MENU_APP: x = p.app_x; break;
	case MENU_FILE: x = p.file_x; break;
	case MENU_EDIT: x = p.edit_x; break;
	case MENU_VIEW: x = p.view_x; break;
	case MENU_CLOCK: x = p.clock_x + p.clock_w - MENU_W; break;
	default: x = 0; break;
	}
	if (x + MENU_W > (int)scr_w)
		x = (int)scr_w - MENU_W;
	if (x < 0)
		x = 0;
	return x;
}

static int menu_h(enum panel_menu menu) {
	return menu_item_count(menu) * MENU_ITEM_H + 8;
}

static enum panel_menu panel_menu_at(int x) {
	struct panel_layout p = get_panel_layout();
	if (x >= p.system_x && x < p.system_x + p.system_w)
		return MENU_SYSTEM;
	if (x >= p.app_x && x < p.app_x + p.app_w)
		return MENU_APP;
	if (x >= p.file_x && x < p.file_x + p.file_w && x < p.clock_x)
		return MENU_FILE;
	if (x >= p.edit_x && x < p.edit_x + p.edit_w && x < p.clock_x)
		return MENU_EDIT;
	if (x >= p.view_x && x < p.view_x + p.view_w && x < p.clock_x)
		return MENU_VIEW;
	if (x >= p.clock_x && x < p.clock_x + p.clock_w)
		return MENU_CLOCK;
	return MENU_NONE;
}

static int menu_item_at(int x, int y) {
	if (open_menu == MENU_NONE || x < menu_x(open_menu) ||
	    x >= menu_x(open_menu) + MENU_W || y < PANEL_H + 4 ||
	    y >= PANEL_H + menu_h(open_menu) - 4)
		return -1;
	int item = (y - PANEL_H - 4) / MENU_ITEM_H;
	return item < menu_item_count(open_menu) ? item : -1;
}

static void draw_panel_overlay(int rx, int ry, int rw, int rh) {
	if (open_menu == MENU_NONE)
		return;
	int mx = menu_x(open_menu);
	int mh = menu_h(open_menu);
	int x0 = mx - 2, x1 = mx + MENU_W + 3;
	int y0 = PANEL_H, y1 = PANEL_H + mh + 3;
	for (int y = ry; y < ry + rh; y++) {
		if (y < y0 || y >= y1)
			continue;
		uint32_t *row = fb + (uint32_t)y * scr_w;
		for (int x = rx; x < rx + rw; x++) {
			if (x < x0 || x >= x1)
				continue;
			if (x >= mx + 3 && x < mx + MENU_W + 3 &&
			    y >= PANEL_H + 3)
				row[x] = 0x00060A0Du;
			if (x >= mx && x < mx + MENU_W &&
			    y < PANEL_H + mh) {
				int border = x == mx || x == mx + MENU_W - 1 ||
				             y == PANEL_H || y == PANEL_H + mh - 1;
				row[x] = border ? 0x006B7780u : MENU_BG_COLOR;
			}
		}
		for (int item = 0; item < menu_item_count(open_menu); item++) {
			int iy = PANEL_H + 4 + item * MENU_ITEM_H;
			if (y < iy || y >= iy + MENU_ITEM_H)
				continue;
			int enabled = menu_item_enabled(open_menu, item);
			if (enabled && item == menu_hover)
				for (int x = mx + 4; x < mx + MENU_W - 4; x++)
					if (x >= rx && x < rx + rw)
						row[x] = MENU_HOVER_COLOR;
			uint32_t color = !enabled ? MENU_DISABLED_COLOR
			                         : item == menu_hover
			                               ? 0x00FFFFFFu
			                               : MENU_TEXT_COLOR;
			draw_text_clipped(row, y, rx, rw, mx + 12, iy + 5,
			                  menu_item_label(open_menu, item), color);
		}
	}
}

static void composite_rect(int rx, int ry, int rw, int rh) {
	if (rx < 0) { rw += rx; rx = 0; }
	if (ry < 0) { rh += ry; ry = 0; }
	if (rw <= 0 || rh <= 0 || (uint32_t)rx >= scr_w || (uint32_t)ry >= scr_h)
		return;
	if ((uint32_t)(rx + rw) > scr_w) rw = (int)scr_w - rx;
	if ((uint32_t)(ry + rh) > scr_h) rh = (int)scr_h - ry;

	/* macOS-style top bar. The focused application's title becomes its menu,
	 * with common server-owned menus alongside it and the clock flush right. */
	const char *active_app = active_app_title();
	char app[15];
	int app_len = text_len(active_app, 14);
	memcpy(app, active_app, (size_t)app_len);
	app[app_len] = 0;
	struct panel_layout panel = get_panel_layout();

	/* background */
	for (int y = ry; y < ry + rh; y++) {
		uint32_t *row = fb + (uint32_t)y * scr_w;
		uint32_t shade = (uint32_t)((y * 24) / (scr_h ? scr_h : 1));
		for (int x = rx; x < rx + rw; x++) {
			uint32_t glow = (uint32_t)(((x ^ y) & 63) < 2 ? 0x00040406 : 0);
			row[x] = y < PANEL_H ? PANEL_COLOR
			                      : (BG_COLOR + (shade << 8) + shade + glow);
		}
		if (y < PANEL_H) {
			struct {
				enum panel_menu menu;
				int x, w;
				const char *label;
				uint32_t color;
			} headers[] = {
			    {MENU_SYSTEM, panel.system_x, panel.system_w, "b1nix",
			     0x00DCE8F2u},
			    {MENU_APP, panel.app_x, panel.app_w, app, 0x00F4F7FAu},
			    {MENU_FILE, panel.file_x, panel.file_w, "File",
			     0x00DCE8F2u},
			    {MENU_EDIT, panel.edit_x, panel.edit_w, "Edit",
			     0x00DCE8F2u},
			    {MENU_VIEW, panel.view_x, panel.view_w, "View",
			     0x00DCE8F2u},
			    {MENU_CLOCK, panel.clock_x, panel.clock_w, clock_hhmm,
			     0x00DCE8F2u},
			};
			for (unsigned hi = 0; hi < sizeof(headers) / sizeof(headers[0]);
			     hi++) {
				if (headers[hi].x >= panel.clock_x &&
				    headers[hi].menu != MENU_CLOCK)
					continue;
				if (headers[hi].menu == open_menu)
					for (int x = headers[hi].x;
					     x < headers[hi].x + headers[hi].w; x++)
						if (x >= rx && x < rx + rw)
							row[x] = PANEL_ACTIVE_COLOR;
				draw_text_clipped(row, y, rx, rw, headers[hi].x + 8, 10,
				                  headers[hi].label, headers[hi].color);
			}
		}
	}

	/* surfaces bottom → top, with a server-side toplevel frame */
	for (int zi = 0; zi < zcount; zi++) {
		struct dsurface *s = slot_surface(zorder[zi]);
		if (!s || !s->mapped || !s->buf)
			continue;
		int sw = (int)s->buf->w, sh = (int)s->buf->h;
		struct dtoplevel *top_obj = 0;
		for (int ti = 0; ti < MAX_TOPLEVELS; ti++)
			if (toplevels[ti].used && toplevels[ti].surface == s)
				top_obj = &toplevels[ti];
		int decorated = top_obj != 0;
		int top = decorated ? TITLE_H : 1;
		for (int y = ry; y < ry + rh; y++) {
			if (y < s->y - top || y > s->y + sh)
				continue;
			uint32_t *row = fb + (uint32_t)y * scr_w;
			for (int x = rx; x < rx + rw; x++) {
				if (x < s->x - 1 || x > s->x + sw)
					continue;
				int lx = x - s->x, ly = y - s->y;
				if (lx >= 0 && lx < sw && ly >= 0 && ly < sh) {
					const uint8_t *src = (const uint8_t *)s->buf->mem +
					                     (uint32_t)ly * s->buf->stride;
					row[x] = ((const uint32_t *)src)[lx];
				} else {
					if (decorated && y < s->y) {
						uint32_t c = zorder[zi] == focus_slot
						                 ? TITLE_FOCUS_COLOR : TITLE_COLOR;
						if (x >= s->x + sw - 15)
							c = CLOSE_COLOR;
						row[x] = c;
					} else {
						row[x] = BORDER_COLOR;
					}
				}
			}
			if (decorated && top_obj)
				draw_text_clipped(row, y, rx, rw, s->x + 6,
				                  s->y - TITLE_H + 3, top_obj->title,
				                  0x00F4F7FAu);
		}
	}

	/* Menus belong to the desktop shell and always sit above client windows. */
	draw_panel_overlay(rx, ry, rw, rh);

	/* crosshair cursor on top */
	for (int i = 0; i < CURSOR_SIZE; i++) {
		int cx = px + i, cy = py;
		if (cx >= rx && cx < rx + rw && cy >= ry && cy < ry + rh)
			fb[(uint32_t)cy * scr_w + cx] = 0x00FFFFFFu;
		cx = px; cy = py + i;
		if (cx >= rx && cx < rx + rw && cy >= ry && cy < ry + rh)
			fb[(uint32_t)cy * scr_w + cx] = 0x00FFFFFFu;
	}

	struct b1nix_fb_rect rect = {(uint32_t)rx, (uint32_t)ry, (uint32_t)rw,
	                             (uint32_t)rh};
	ioctl(fb_fd, B1NIX_FBIOFLUSH, &rect);
}

static void composite_surface_region(struct dsurface *s) {
	int sw = s->buf ? (int)s->buf->w : 0;
	int sh = s->buf ? (int)s->buf->h : 0;
	int top = 1;
	for (int i = 0; i < MAX_TOPLEVELS; i++)
		if (toplevels[i].used && toplevels[i].surface == s)
			top = TITLE_H;
	composite_rect(s->x - 1, s->y - top, sw + 2, sh + top + 1);
}

/* ── object helpers ── */

static struct dsurface *find_surface(int client, uint32_t id) {
	for (int i = 0; i < MAX_SURFACES; i++)
		if (surfaces[i].used && surfaces[i].client == client &&
		    surfaces[i].id == id)
			return &surfaces[i];
	return 0;
}

static struct dbuffer *find_buffer(int client, uint32_t id) {
	for (int i = 0; i < MAX_BUFFERS; i++)
		if (buffers[i].used && buffers[i].client == client &&
		    buffers[i].id == id)
			return &buffers[i];
	return 0;
}

static struct dtoplevel *find_toplevel(int client, uint32_t id) {
	for (int i = 0; i < MAX_TOPLEVELS; i++)
		if (toplevels[i].used && toplevels[i].client == client &&
		    toplevels[i].id == id)
			return &toplevels[i];
	return 0;
}

static struct dtoplevel *surface_toplevel(struct dsurface *s) {
	for (int i = 0; i < MAX_TOPLEVELS; i++)
		if (toplevels[i].used && toplevels[i].surface == s)
			return &toplevels[i];
	return 0;
}

static void zorder_remove(int slot) {
	int j = 0;
	for (int i = 0; i < zcount; i++)
		if (zorder[i] != slot)
			zorder[j++] = zorder[i];
	zcount = j;
}

static void zorder_raise(int slot) {
	zorder_remove(slot);
	zorder[zcount++] = slot;
}

static void buffer_destroy(struct dbuffer *b) {
	if (b->map_base)
		munmap(b->map_base, b->map_size);
	memset(b, 0, sizeof(*b));
}

static void surface_destroy(struct dsurface *s) {
	int slot = (int)(s - surfaces);
	if (s->buf)
		send_msg(s->client, s->buf->id, 0, 0, 0);
	zorder_remove(slot);
	if (enter_slot == slot)
		enter_slot = -1;
	if (focus_slot == slot) {
		focus_slot = -1;
		composite_rect(0, 0, (int)scr_w, PANEL_H);
	}
	int x = s->x, y = s->y;
	int w = s->buf ? (int)s->buf->w : 0, h = s->buf ? (int)s->buf->h : 0;
	int was_mapped = s->mapped;
	for (int i = 0; i < MAX_TOPLEVELS; i++)
		if (toplevels[i].used && toplevels[i].surface == s)
			memset(&toplevels[i], 0, sizeof(toplevels[i]));
	memset(s, 0, sizeof(*s));
	if (was_mapped)
		composite_rect(x - 1, y - TITLE_H, w + 2, h + TITLE_H + 1);
}

static void client_disconnect(int ci) {
	if (!clients[ci].used)
		return;
	for (int i = 0; i < MAX_SURFACES; i++)
		if (surfaces[i].used && surfaces[i].client == ci)
			surface_destroy(&surfaces[i]);
	for (int i = 0; i < MAX_BUFFERS; i++)
		if (buffers[i].used && buffers[i].client == ci)
			buffer_destroy(&buffers[i]);
	for (int i = 0; i < MAX_TOPLEVELS; i++)
		if (toplevels[i].used && toplevels[i].client == ci)
			memset(&toplevels[i], 0, sizeof(toplevels[i]));
	for (int i = 0; i < MAX_WOBJECTS; i++)
		if (wobjects[i].used && wobjects[i].client == ci)
			memset(&wobjects[i], 0, sizeof(wobjects[i]));
	for (int i = 0; i < MAX_WPOOLS; i++)
		if (wpools[i].used && wpools[i].client == ci) {
			close(wpools[i].fd);
			memset(&wpools[i], 0, sizeof(wpools[i]));
		}
	if (clients[ci].pending_fd >= 0)
		close(clients[ci].pending_fd);
	close(clients[ci].fd);
	memset(&clients[ci], 0, sizeof(clients[ci]));
}

/* ── request handlers ── */

static void create_surface(int ci, uint32_t id) {
		int slot = -1;
		for (int i = 0; i < MAX_SURFACES; i++)
			if (!surfaces[i].used) { slot = i; break; }
		if (slot < 0)
			return;
		struct dsurface *s = &surfaces[slot];
		memset(s, 0, sizeof(*s));
		s->used = 1;
		s->id = id;
		s->client = ci;
		s->placement = (unsigned)surfaces_created;
		s->x = 48 + 40 * (surfaces_created % 8);
		s->y = 48 + 32 * (surfaces_created % 8);
		surfaces_created++;
		zorder[zcount++] = slot;
}

static void create_toplevel(int ci, uint32_t id, uint32_t surface_id) {
		struct dsurface *s = find_surface(ci, surface_id);
		if (!s || surface_toplevel(s))
			return;
		int slot = -1;
		for (int i = 0; i < MAX_TOPLEVELS; i++)
			if (!toplevels[i].used) { slot = i; break; }
		if (slot < 0)
			return;
		struct dtoplevel *t = &toplevels[slot];
		memset(t, 0, sizeof(*t));
		t->used = 1;
		t->id = id;
		t->client = ci;
		t->surface = s;
		strcpy(t->title, "Wayland");
}

static void surface_action(int ci, struct dsurface *s, uint16_t op,
                           const uint32_t *a, unsigned n) {
	switch (op) {
	case SURFACE_ATTACH:
		if (n < 1)
			return;
		s->pend_buffer_id = a[0];
		s->pend_attach = 1;
		break;
	case SURFACE_DAMAGE: {
		if (n < 4)
			return;
		uint32_t x0 = a[0], y0 = a[1], x1 = a[0] + a[2], y1 = a[1] + a[3];
		if (!s->pend_dmg_valid) {
			s->dx0 = x0; s->dy0 = y0; s->dx1 = x1; s->dy1 = y1;
			s->pend_dmg_valid = 1;
		} else {
			if (x0 < s->dx0) s->dx0 = x0;
			if (y0 < s->dy0) s->dy0 = y0;
			if (x1 > s->dx1) s->dx1 = x1;
			if (y1 > s->dy1) s->dy1 = y1;
		}
		break;
	}
	case SURFACE_FRAME:
		if (n < 1)
			return;
		s->frame_cb = a[0];
		s->has_frame_cb = 1;
		break;
	case SURFACE_COMMIT: {
		if (s->pend_attach) {
			struct dbuffer *nb = find_buffer(ci, s->pend_buffer_id);
			if (!nb)
				return;
			if (s->buf && s->buf != nb)
				send_msg(ci, s->buf->id, 0, 0, 0);
			s->buf = nb;
			s->pend_attach = 0;
			if (!s->mapped) {
				/* Give the default desktop apps distinct, useful positions. */
				if (s->placement == 0) {
					s->x = 48;
					s->y = 92;
				} else if (s->placement == 1) {
					s->x = (int)scr_w - (int)nb->w - 48;
					s->y = 52;
				} else if (s->placement == 2) {
					s->x = ((int)scr_w - (int)nb->w) / 2;
					s->y = (int)scr_h - (int)nb->h - TITLE_H - 44;
				}
			}
		}
		if (!s->buf)
			return; /* commit with nothing attached: no-op */
		int first_map = !s->mapped;
		s->mapped = 1;
		if (first_map) {
			struct dsurface *old = slot_surface(focus_slot);
			if (old) {
				uint32_t leave[1] = {old->id};
				send_seat_event(old->client, SEAT_FOCUS_LEAVE, leave, 1);
			}
			focus_slot = (int)(s - surfaces);
			zorder_raise(focus_slot);
			uint32_t enter[1] = {s->id};
			send_seat_event(s->client, SEAT_FOCUS_ENTER, enter, 1);
			if (old) {
				composite_surface_region(old);
			}
			composite_rect(0, 0, (int)scr_w, PANEL_H);
			composite_surface_region(s);
		} else if (s->pend_dmg_valid) {
			int dx = (int)s->dx0, dy = (int)s->dy0;
			int dw = (int)(s->dx1 - s->dx0), dh = (int)(s->dy1 - s->dy0);
			if (dw > 0 && dh > 0)
				composite_rect(s->x + dx, s->y + dy, dw, dh);
		}
		s->pend_dmg_valid = 0;
		frame_serial++;
		if (s->has_frame_cb) {
			uint32_t w[1] = {frame_serial};
			send_msg(ci, s->frame_cb, 0, w, 1);
			s->has_frame_cb = 0;
		}
		break;
	}
	case SURFACE_DESTROY:
		surface_destroy(s);
		break;
	default:
		break;
	}
}

/* Pack a Wayland wire string (length-prefixed, NUL-terminated, 4-byte padded)
 * into a uint32 word buffer; returns the new word index. */
static unsigned wl_pack_string(uint32_t *w, unsigned i, const char *s) {
	unsigned len = (unsigned)strlen(s) + 1;
	w[i++] = len;
	unsigned words = (len + 3) / 4;
	memset(&w[i], 0, words * 4);
	memcpy(&w[i], s, len - 1);
	return i + words;
}

/* wl_output advertises one fixed 1024x768 output so toolkits can lay out. */
static void wl_send_output_events(int ci, uint32_t id) {
	uint32_t geo[32];
	unsigned k = 0;
	geo[k++] = 0;     /* x */
	geo[k++] = 0;     /* y */
	geo[k++] = 270;   /* physical width mm */
	geo[k++] = 203;   /* physical height mm */
	geo[k++] = 0;     /* subpixel unknown */
	k = wl_pack_string(geo, k, "b1nix");
	k = wl_pack_string(geo, k, "b1nix-display");
	geo[k++] = 0;     /* transform normal */
	send_msg(ci, id, 0, geo, k); /* geometry */
	uint32_t mode[4] = {0x3 /* current|preferred */, 1024, 768, 60000};
	send_msg(ci, id, 1, mode, 4); /* mode */
	uint32_t scale = 1;
	send_msg(ci, id, 3, &scale, 1); /* scale (v2) */
	send_msg(ci, id, 2, 0, 0);      /* done (v2) */
}

/* --- Clipboard (wl_data_device selection) ---
 * One active selection at a time: the owning client + its data_source id and
 * MIME. Server-allocated data_offer ids live in the >= 0xff000000 range so they
 * never collide with client ids. */
static int sel_client = -1;
static uint32_t sel_source = 0;
static char sel_mime[64];
static uint32_t server_id_next = 0xff000000u;

/* Push the current selection to one data_device: server-create a data_offer,
 * announce its MIME, then make it the selection. */
static void clipboard_offer_to(int ci, uint32_t device_id) {
	if (sel_client < 0 || ci == sel_client)
		return;
	uint32_t offer_id = server_id_next++;
	wobject_add(ci, offer_id, WOBJ_DATA_OFFER, 0); /* so receive() resolves it */
	send_msg(ci, device_id, 0, &offer_id, 1); /* data_device.data_offer */
	uint32_t buf[20];
	unsigned k = wl_pack_string(buf, 0, sel_mime);
	send_msg(ci, offer_id, 0, buf, k);        /* data_offer.offer(mime) */
	send_msg(ci, device_id, 5, &offer_id, 1); /* data_device.selection(offer) */
}

static void wl_registry_globals(int ci, uint32_t registry) {
	static const struct {
		uint32_t name;
		const char *interface;
		uint32_t version;
	} globals[] = {
	    {1, "wl_compositor", 4},
	    {2, "wl_shm", 1},
	    {3, "wl_seat", 5},
	    {4, "xdg_wm_base", 1},
	    {5, "wl_output", 2},
	    {6, "wl_data_device_manager", 3},
	};
	for (unsigned i = 0; i < sizeof(globals) / sizeof(globals[0]); i++) {
		uint32_t prefix = globals[i].name;
		uint32_t suffix = globals[i].version;
		wl_send_string(ci, registry, 0, &prefix, 1, globals[i].interface,
		               &suffix, 1);
	}
}

static void wl_surface_configure(int ci, struct wobject *xdg) {
	struct dsurface *surface = find_surface(ci, xdg->link);
	struct dtoplevel *t = surface ? surface_toplevel(surface) : 0;
	if (!t)
		return;
	uint32_t top[3] = {0, 0, 0}; /* width, height, empty states array */
	send_msg(ci, t->id, 0, top, 3);
	uint32_t serial = ++frame_serial;
	send_msg(ci, xdg->id, 0, &serial, 1);
	xdg->configured = 1;
}

static void wl_create_buffer(int ci, struct wpool *pool, const uint32_t *a,
                             unsigned n) {
	if (n < 6 || (a[5] != 0 && a[5] != 1) || a[2] == 0 || a[3] == 0 ||
	    a[4] < a[2] * 4 ||
	    (uint64_t)a[1] + (uint64_t)a[4] * a[3] > pool->size)
		return;
	int slot = -1;
	for (int i = 0; i < MAX_BUFFERS; i++)
		if (!buffers[i].used) { slot = i; break; }
	if (slot < 0)
		return;
	size_t map_size = (size_t)a[1] + (size_t)a[4] * a[3];
	void *mem = mmap(0, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	                 pool->fd, 0);
	if (mem == MAP_FAILED)
		return;
	struct dbuffer *b = &buffers[slot];
	b->used = 1;
	b->id = a[0];
	b->client = ci;
	b->map_base = mem;
	b->map_size = map_size;
	b->mem = (uint8_t *)mem + a[1];
	b->w = a[2];
	b->h = a[3];
	b->stride = a[4];
}

static void handle_wayland_msg(int ci, const struct wl_hdr *h,
                               const uint32_t *a, unsigned n) {
	if (h->object_id == 1) {
		if (h->opcode == 0 && n >= 1) {
			uint32_t serial = frame_serial;
			send_msg(ci, a[0], 0, &serial, 1);
			wl_delete_id(ci, a[0]);
		} else if (h->opcode == 1 && n >= 1 &&
		           wobject_add(ci, a[0], WOBJ_REGISTRY, 0)) {
			wl_registry_globals(ci, a[0]);
		}
		return;
	}

	struct dsurface *surface = find_surface(ci, h->object_id);
	if (surface) {
		if (h->opcode == 0)
			surface_action(ci, surface, SURFACE_DESTROY, a, n);
		else if (h->opcode == 1 && n >= 1)
			surface_action(ci, surface, SURFACE_ATTACH, a, 1);
		else if (h->opcode == 2 || h->opcode == 9)
			surface_action(ci, surface, SURFACE_DAMAGE, a, n);
		else if (h->opcode == 3)
			surface_action(ci, surface, SURFACE_FRAME, a, n);
		else if (h->opcode == 6) {
			struct wobject *xdg = 0;
			for (int i = 0; i < MAX_WOBJECTS; i++)
				if (wobjects[i].used && wobjects[i].client == ci &&
				    wobjects[i].type == WOBJ_XDG_SURFACE &&
				    wobjects[i].link == surface->id)
					xdg = &wobjects[i];
			if (!surface->buf && xdg && !xdg->configured)
				wl_surface_configure(ci, xdg);
			else
				surface_action(ci, surface, SURFACE_COMMIT, a, n);
		}
		return;
	}

	struct dbuffer *buffer = find_buffer(ci, h->object_id);
	if (buffer) {
		if (h->opcode == 0)
			buffer_destroy(buffer);
		return;
	}

	struct dtoplevel *top = find_toplevel(ci, h->object_id);
	if (top) {
		if (h->opcode == 0)
			memset(top, 0, sizeof(*top));
		else if (h->opcode == 2 && n >= 1) {
			unsigned len = a[0];
			if (len > sizeof(top->title))
				len = sizeof(top->title);
			if (len)
				memcpy(top->title, &a[1], len - 1);
			top->title[len ? len - 1 : 0] = 0;
		}
		return;
	}

	struct wpool *pool = wpool_find(ci, h->object_id);
	if (pool) {
		if (h->opcode == 0)
			wl_create_buffer(ci, pool, a, n);
		else if (h->opcode == 1) {
			close(pool->fd);
			memset(pool, 0, sizeof(*pool));
			wl_delete_id(ci, h->object_id);
		} else if (h->opcode == 2 && n >= 1 && a[0] > pool->size) {
			pool->size = a[0];
		}
		return;
	}

	struct wobject *obj = wobject_find(ci, h->object_id);
	if (!obj)
		return;
	switch (obj->type) {
	case WOBJ_REGISTRY:
		if (h->opcode == 0 && n >= 4) {
			uint32_t new_id = a[n - 1];
			enum wobject_type type;
			if (a[0] == 1) type = WOBJ_COMPOSITOR;
			else if (a[0] == 2) type = WOBJ_SHM;
			else if (a[0] == 3) type = WOBJ_SEAT;
			else if (a[0] == 4) type = WOBJ_XDG_WM_BASE;
			else if (a[0] == 5) type = WOBJ_OUTPUT;
			else if (a[0] == 6) type = WOBJ_DDM;
			else break;
			if (wobject_add(ci, new_id, type, 0)) {
				if (type == WOBJ_SHM) {
					uint32_t format = 0;
					send_msg(ci, new_id, 0, &format, 1);
					format = 1;
					send_msg(ci, new_id, 0, &format, 1);
				} else if (type == WOBJ_SEAT) {
					uint32_t capabilities = 3;
					send_msg(ci, new_id, 0, &capabilities, 1);
					wl_send_string(ci, new_id, 1, 0, 0, "b1nix", 0, 0);
				} else if (type == WOBJ_OUTPUT) {
					wl_send_output_events(ci, new_id);
				}
			}
		}
		break;
	case WOBJ_COMPOSITOR:
		if (h->opcode == 0 && n >= 1)
			create_surface(ci, a[0]);
		else if (h->opcode == 1 && n >= 1)
			wobject_add(ci, a[0], WOBJ_REGION, 0);
		break;
	case WOBJ_SHM:
		if (h->opcode == 0 && n >= 2 && clients[ci].pending_fd >= 0)
			for (int i = 0; i < MAX_WPOOLS; i++)
				if (!wpools[i].used) {
					wpools[i].used = 1;
					wpools[i].id = a[0];
					wpools[i].client = ci;
					wpools[i].fd = clients[ci].pending_fd;
					wpools[i].size = a[1];
					clients[ci].pending_fd = -1;
					break;
				}
		break;
	case WOBJ_XDG_WM_BASE:
		if (h->opcode == 0)
			wobject_remove(obj);
		else if (h->opcode == 2 && n >= 2)
			wobject_add(ci, a[0], WOBJ_XDG_SURFACE, a[1]);
		break;
	case WOBJ_XDG_SURFACE:
		if (h->opcode == 0)
			wobject_remove(obj);
		else if (h->opcode == 1 && n >= 1) {
			create_toplevel(ci, a[0], obj->link);
			struct dtoplevel *created = find_toplevel(ci, a[0]);
			if (created)
				strcpy(created->title, "Wayland");
		}
		break;
	case WOBJ_REGION:
		if (h->opcode == 0)
			wobject_remove(obj);
		break;
	case WOBJ_SEAT:
		if (h->opcode == 0 && n >= 1)
			wobject_add(ci, a[0], WOBJ_POINTER, 0);
		else if (h->opcode == 1 && n >= 1) {
			if (wobject_add(ci, a[0], WOBJ_KEYBOARD, 0))
				keyboard_init(ci, a[0]);
		}
		else if (h->opcode == 3)
			wobject_remove(obj);
		break;
	case WOBJ_POINTER:
	case WOBJ_KEYBOARD:
		if (h->opcode == 0)
			wobject_remove(obj);
		break;
	case WOBJ_DDM:
		if (h->opcode == 0 && n >= 1) /* create_data_source */
			wobject_add(ci, a[0], WOBJ_DATA_SOURCE, 0);
		else if (h->opcode == 1 && n >= 1) { /* get_data_device(new_id, seat) */
			if (wobject_add(ci, a[0], WOBJ_DATA_DEVICE, 0))
				clipboard_offer_to(ci, a[0]); /* push any existing selection */
		}
		break;
	case WOBJ_DATA_SOURCE:
		if (h->opcode == 0 && n >= 1) { /* offer(mime) */
			strncpy(obj->mime, (const char *)&a[1], sizeof(obj->mime) - 1);
			obj->mime[sizeof(obj->mime) - 1] = 0;
		} else if (h->opcode == 1) /* destroy */
			wobject_remove(obj);
		break;
	case WOBJ_DATA_DEVICE:
		if (h->opcode == 1 && n >= 1) { /* set_selection(source, serial) */
			struct wobject *src = a[0] ? wobject_find(ci, a[0]) : 0;
			if (src && src->type == WOBJ_DATA_SOURCE) {
				sel_client = ci;
				sel_source = a[0];
				strncpy(sel_mime, src->mime, sizeof(sel_mime) - 1);
				sel_mime[sizeof(sel_mime) - 1] = 0;
				for (int i = 0; i < MAX_WOBJECTS; i++)
					if (wobjects[i].used && wobjects[i].type == WOBJ_DATA_DEVICE)
						clipboard_offer_to(wobjects[i].client, wobjects[i].id);
			}
		} else if (h->opcode == 2) /* release */
			wobject_remove(obj);
		break;
	case WOBJ_DATA_OFFER:
		if (h->opcode == 1) { /* receive(mime, fd): forward fd to the source */
			if (sel_client >= 0 && clients[ci].pending_fd >= 0) {
				uint32_t buf[20];
				unsigned k = wl_pack_string(buf, 0, sel_mime);
				send_msg_fd(sel_client, sel_source, 1 /* data_source.send */,
				            buf, k, clients[ci].pending_fd);
			}
			if (clients[ci].pending_fd >= 0) {
				close(clients[ci].pending_fd);
				clients[ci].pending_fd = -1;
			}
		} else if (h->opcode == 2) /* destroy */
			wobject_remove(obj);
		break;
	default:
		break;
	}
}

static void client_data(int ci) {
	struct dclient *c = &clients[ci];
	char control[CMSG_SPACE(sizeof(int))];
	struct iovec iov = {c->inbuf + c->inlen, sizeof(c->inbuf) - c->inlen};
	struct msghdr mh;
	memset(&mh, 0, sizeof(mh));
	memset(control, 0, sizeof(control));
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = control;
	mh.msg_controllen = sizeof(control);
	ssize_t n = recvmsg(c->fd, &mh, 0);
	if (n <= 0) {
		client_disconnect(ci);
		return;
	}
	for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm;
	     cm = CMSG_NXTHDR(&mh, cm))
		if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS &&
		    cm->cmsg_len >= CMSG_LEN(sizeof(int))) {
			if (c->pending_fd >= 0)
				close(c->pending_fd);
			memcpy(&c->pending_fd, CMSG_DATA(cm), sizeof(int));
		}
	c->inlen += (unsigned)n;
	for (;;) {
		if (c->inlen < sizeof(struct wl_hdr))
			break;
		struct wl_hdr h;
		memcpy(&h, c->inbuf, sizeof(h));
		if (h.size < sizeof(h) || h.size > MAX_MSG || (h.size & 3)) {
			client_disconnect(ci);
			return;
		}
		if (c->inlen < h.size)
			break;
		uint32_t args[(MAX_MSG - sizeof(struct wl_hdr)) / 4];
		unsigned nargs = (h.size - sizeof(h)) / 4;
		memcpy(args, c->inbuf + sizeof(h), nargs * 4);
		memmove(c->inbuf, c->inbuf + h.size, c->inlen - h.size);
		c->inlen -= h.size;
		handle_wayland_msg(ci, &h, args, nargs);
		if (!clients[ci].used)
			return; /* disconnected during handling */
	}
}

static void accept_client(int fd) {
	int cfd = accept(fd, 0, 0);
	if (cfd < 0)
		return;
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (!clients[i].used) {
			clients[i].used = 1;
			clients[i].fd = cfd;
			clients[i].inlen = 0;
			clients[i].pending_fd = -1;
			return;
		}
	close(cfd);
}

/* ── input ── */

static int surface_at(int x, int y) {
	for (int zi = zcount - 1; zi >= 0; zi--) {
		struct dsurface *s = slot_surface(zorder[zi]);
		if (!s || !s->mapped || !s->buf)
			continue;
		int top = surface_toplevel(s) ? TITLE_H : 0;
		if (x >= s->x && x < s->x + (int)s->buf->w && y >= s->y - top &&
		    y < s->y + (int)s->buf->h)
			return zorder[zi];
	}
	return -1;
}

static void focus_cycle(void);
static int update_clock(void);

static void close_focused_window(void) {
	struct dsurface *f = slot_surface(focus_slot);
	struct dtoplevel *t = f ? surface_toplevel(f) : 0;
	if (t)
		send_msg(t->client, t->id, 1, 0, 0);
}

static void send_focused_shortcut(uint32_t key) {
	struct dsurface *f = slot_surface(focus_slot);
	if (!f)
		return;
	uint32_t ctrl_down[2] = {0x1d, 1};
	uint32_t key_down[2] = {key, 1};
	uint32_t key_up[2] = {key, 0};
	uint32_t ctrl_up[2] = {0x1d, 0};
	send_seat_event(f->client, SEAT_KEY, ctrl_down, 2);
	send_seat_event(f->client, SEAT_KEY, key_down, 2);
	send_seat_event(f->client, SEAT_KEY, key_up, 2);
	send_seat_event(f->client, SEAT_KEY, ctrl_up, 2);
}

static void close_panel_menu(void) {
	if (open_menu == MENU_NONE)
		return;
	open_menu = MENU_NONE;
	menu_hover = -1;
	composite_rect(0, 0, (int)scr_w, (int)scr_h);
}

static void open_panel_menu(enum panel_menu menu) {
	open_menu = menu;
	menu_hover = menu_item_at(px, py);
	composite_rect(0, 0, (int)scr_w, (int)scr_h);
}

static void spawn_app(const char *path, const char *arg) {
	pid_t pid = fork();
	if (pid == 0) {
		if (arg) {
			execlp(path, path, arg, (char *)0);
		} else {
			execlp(path, path, (char *)0);
		}
		_exit(127);
	}
}

static void activate_menu_item(enum panel_menu menu, int item) {
	if (!menu_item_enabled(menu, item))
		return;
	close_panel_menu();
	if (menu == MENU_SYSTEM) {
		if (item == 0) spawn_app("/bin/gabout", NULL);
		else if (item == 1) spawn_app("/bin/gterm", NULL);
		else if (item == 2) spawn_app("/bin/gpaint", NULL);
		else if (item == 3) spawn_app("/bin/gclock", NULL);
		else if (item == 4) focus_cycle();
		else if (item == 5) close_focused_window();
	}
	else if (menu == MENU_APP) {
		if (item == 0) spawn_app("/bin/gabout", active_app_title());
		else if (item == 1) focus_cycle();
		else if (item == 2) close_focused_window();
	}
	else if (menu == MENU_FILE) {
		close_focused_window();
	}
	else if (menu == MENU_EDIT) {
		static const uint32_t edit_keys[] = {0x2d, 0x2e, 0x2f};
		send_focused_shortcut(edit_keys[item]);
	}
	else if (menu == MENU_VIEW) {
		if (item == 0) focus_cycle();
		else if (item == 1 && focus_slot >= 0) {
			zorder_raise(focus_slot);
			composite_rect(0, PANEL_H, (int)scr_w, (int)scr_h - PANEL_H);
		}
	}
	else if (menu == MENU_CLOCK) {
		if (item == 0) {
			spawn_app("/bin/gclock", NULL);
		} else if (item == 1) {
			clock_24h = !clock_24h;
			clock_last_min = -1;
			update_clock();
			composite_rect(0, 0, (int)scr_w, PANEL_H);
		} else if (item == 2) {
			spawn_app("/bin/gabout", "date");
		}
	}
}

static void pointer_moved(void) {
	if (open_menu != MENU_NONE) {
		enum panel_menu header = py < PANEL_H ? panel_menu_at(px) : MENU_NONE;
		if (header != MENU_NONE && header != open_menu) {
			open_panel_menu(header);
			return;
		}
		int hover = menu_item_at(px, py);
		if (hover != menu_hover) {
			menu_hover = hover;
			composite_rect(menu_x(open_menu) - 2, PANEL_H,
			               MENU_W + 5, menu_h(open_menu) + 3);
		}
		return;
	}
	int slot = surface_at(px, py);
	struct dsurface *s = slot_surface(slot);
	if (slot != enter_slot) {
		struct dsurface *old = slot_surface(enter_slot);
		if (old) {
			uint32_t w[1] = {old->id};
			send_seat_event(old->client, SEAT_POINTER_LEAVE, w, 1);
		}
		if (s) {
			uint32_t w[3] = {s->id, (uint32_t)(px - s->x), (uint32_t)(py - s->y)};
			send_seat_event(s->client, SEAT_POINTER_ENTER, w, 3);
		}
		enter_slot = slot;
	} else if (s) {
		uint32_t w[2] = {(uint32_t)(px - s->x), (uint32_t)(py - s->y)};
		send_seat_event(s->client, SEAT_POINTER_MOTION, w, 2);
	}
}

/* A left-press that landed on a window's server-side decoration (title bar /
 * close box) is consumed by the server (focus, drag, close) and must NOT be
 * forwarded to the client — otherwise e.g. gpaint sees it as a canvas click
 * and starts drawing while you drag the title. Tracked so the matching
 * release is suppressed too. */
static int btn_on_decoration;
static int btn_on_panel;

static void pointer_button(uint16_t code, int state) {
	input_serial++;
	int on_decoration = 0;
	if (code == B1NIX_BTN_LEFT) {
		if (state) {
			enum panel_menu header =
			    py < PANEL_H ? panel_menu_at(px) : MENU_NONE;
			if (header != MENU_NONE) {
				btn_on_panel = 1;
				if (open_menu == header)
					close_panel_menu();
				else
					open_panel_menu(header);
				return;
			}
			if (open_menu != MENU_NONE) {
				enum panel_menu menu = open_menu;
				int item = menu_item_at(px, py);
				btn_on_panel = 1;
				if (item >= 0)
					activate_menu_item(menu, item);
				else
					close_panel_menu();
				return;
			}
		} else if (btn_on_panel) {
			btn_on_panel = 0;
			return;
		}
	}
	if (state) {
		int slot = surface_at(px, py);
		struct dsurface *s = slot_surface(slot);
		if (s && slot != focus_slot) {
			struct dsurface *old = slot_surface(focus_slot);
			if (old) {
				uint32_t w[1] = {old->id};
				send_seat_event(old->client, SEAT_FOCUS_LEAVE, w, 1);
			}
			focus_slot = slot;
			uint32_t w[1] = {s->id};
			send_seat_event(s->client, SEAT_FOCUS_ENTER, w, 1);
			zorder_raise(slot);
			if (old) {
				composite_surface_region(old);
			}
			composite_rect(0, 0, (int)scr_w, PANEL_H);
			composite_surface_region(s);
		}
		if (s && code == B1NIX_BTN_LEFT && surface_toplevel(s) && py < s->y) {
			on_decoration = 1;
			if (px >= s->x + (int)s->buf->w - 16) {
				struct dtoplevel *t = surface_toplevel(s);
				send_msg(t->client, t->id, 1, 0, 0);
			} else {
				drag_slot = slot;
			}
		}
		if (code == B1NIX_BTN_LEFT)
			btn_on_decoration = on_decoration;
	} else {
		if (code == B1NIX_BTN_LEFT) {
			drag_slot = -1;
			on_decoration = btn_on_decoration;
			btn_on_decoration = 0;
		}
	}
	if (on_decoration)
		return; /* decoration interaction — don't reach the client */
	struct dsurface *f = slot_surface(focus_slot);
	if (f) {
		uint32_t w[2] = {code, (uint32_t)state};
		send_seat_event(f->client, SEAT_POINTER_BUTTON, w, 2);
	}
}

static void focus_cycle(void) {
	if (zcount == 0)
		return;
	int pos = -1;
	for (int i = 0; i < zcount; i++)
		if (zorder[i] == focus_slot)
			pos = i;
	int slot = zorder[(pos + 1) % zcount];
	struct dsurface *next = slot_surface(slot);
	if (!next || !next->mapped)
		return;
	struct dsurface *old = slot_surface(focus_slot);
	if (old) {
		uint32_t w[1] = {old->id};
		send_seat_event(old->client, SEAT_FOCUS_LEAVE, w, 1);
	}
	focus_slot = slot;
	zorder_raise(slot);
	uint32_t w[1] = {next->id};
	send_seat_event(next->client, SEAT_FOCUS_ENTER, w, 1);
	if (old) {
		composite_surface_region(old);
	}
	composite_rect(0, 0, (int)scr_w, PANEL_H);
	composite_surface_region(next);
}

static void input_drain(int which) {
	struct b1nix_input_event evs[16];
	static int acc_dx, acc_dy, moved;
	/* Absolute pointer (virtio-tablet): the kernel scales 0..32767 to screen
	 * pixels, so an EV_ABS sets the cursor position directly rather than
	 * accumulating a delta. Lets the mouse track grab-free in QEMU. */
	static int abs_x, abs_y, have_abs;
	for (;;) {
		ssize_t n = read(ev_fds[which], evs, sizeof(evs));
		if (n <= 0)
			break;
		int count = (int)(n / (ssize_t)sizeof(evs[0]));
		for (int i = 0; i < count; i++) {
			struct b1nix_input_event *e = &evs[i];
			if (which == 0) { /* keyboard */
				if (e->type == B1NIX_EV_KEY) {
					input_serial++;
					if (e->value && e->code == 0x01 &&
					    open_menu != MENU_NONE) {
						close_panel_menu();
						continue;
					}
					if (e->code == 0x38) {
						left_alt = e->value != 0;
						continue;
					}
					if (left_alt && e->value && e->code == 0x0f) {
						focus_cycle();
						continue;
					}
					struct dsurface *f = slot_surface(focus_slot);
					if (f) {
						if (left_alt && e->value && e->code == 0x3e) {
							struct dtoplevel *t = surface_toplevel(f);
							if (t)
								send_msg(t->client, t->id,
								         1, 0, 0);
							continue;
						}
						uint32_t w[2] = {e->code, (uint32_t)e->value};
						send_seat_event(f->client, SEAT_KEY, w, 2);
					}
				}
				continue;
			}
			/* mouse */
			switch (e->type) {
			case B1NIX_EV_REL:
				if (e->code == B1NIX_REL_X) acc_dx += e->value;
				if (e->code == B1NIX_REL_Y) acc_dy += e->value;
				moved = 1;
				break;
			case B1NIX_EV_ABS:
				if (e->code == B1NIX_ABS_X) abs_x = e->value;
				if (e->code == B1NIX_ABS_Y) abs_y = e->value;
				have_abs = 1;
				moved = 1;
				break;
			case B1NIX_EV_KEY:
				pointer_button(e->code, e->value);
				break;
			case B1NIX_EV_SYN:
				if (moved) {
					int ox = px, oy = py;
					if (have_abs) {
						px = abs_x;
						py = abs_y;
						have_abs = 0;
					} else {
						px += acc_dx;
						py += acc_dy;
					}
					acc_dx = acc_dy = 0;
					moved = 0;
					if (px < 0) px = 0;
					if (py < 0) py = 0;
					if (px >= (int)scr_w) px = (int)scr_w - 1;
					if (py >= (int)scr_h) py = (int)scr_h - 1;
					if (px != ox || py != oy) {
						struct dsurface *drag = slot_surface(drag_slot);
						if (drag) {
							int ox_pos = drag->x;
							int oy_pos = drag->y;
							drag->x += px - ox;
							drag->y += py - oy;
							/* Keep the title bar reachable: never under the top
							 * panel, and always leave a sliver on every edge so
							 * a window can't be lost off-screen. */
							int dw = drag->buf ? (int)drag->buf->w : 0;
							int dh = drag->buf ? (int)drag->buf->h : 0;
							int min_y = PANEL_H + TITLE_H;
							if (drag->y < min_y) drag->y = min_y;
							if (drag->y > (int)scr_h - 24)
								drag->y = (int)scr_h - 24;
							if (drag->x < -(dw - 48))
								drag->x = -(dw - 48);
							if (drag->x > (int)scr_w - 48)
								drag->x = (int)scr_w - 48;
							(void)dh;

							int top = 1;
							for (int i = 0; i < MAX_TOPLEVELS; i++)
								if (toplevels[i].used && toplevels[i].surface == drag)
									top = TITLE_H;
							composite_rect(ox_pos - 1, oy_pos - top, dw + 2, dh + top + 1);
							composite_surface_region(drag);
						}
						composite_rect(ox, oy, CURSOR_SIZE + 1, CURSOR_SIZE + 1);
						composite_rect(px, py, CURSOR_SIZE + 1, CURSOR_SIZE + 1);
						pointer_moved();
					}
				}
				break;
			default:
				break;
			}
		}
	}
}

/* Refresh the top-bar clock from the RTC. Returns 1 when the displayed
 * minute changed (so the caller repaints the panel), 0 otherwise. */
static int update_clock(void) {
	time_t now = time(0);
	struct tm tmv;
	struct tm *t = localtime_r(&now, &tmv);
	if (!t)
		return 0;
	if (t->tm_min == clock_last_min && clock_last_min >= 0)
		return 0;
	clock_last_min = t->tm_min;
	int h = t->tm_hour, m = t->tm_min;
	if (clock_24h) {
		clock_hhmm[0] = (char)('0' + (h / 10) % 10);
		clock_hhmm[1] = (char)('0' + h % 10);
		clock_hhmm[2] = ':';
		clock_hhmm[3] = (char)('0' + (m / 10) % 10);
		clock_hhmm[4] = (char)('0' + m % 10);
		clock_hhmm[5] = 0;
	} else {
		int is_pm = h >= 12;
		int h12 = h % 12;
		if (h12 == 0) h12 = 12;
		clock_hhmm[0] = (char)('0' + (h12 / 10) % 10);
		clock_hhmm[1] = (char)('0' + h12 % 10);
		clock_hhmm[2] = ':';
		clock_hhmm[3] = (char)('0' + (m / 10) % 10);
		clock_hhmm[4] = (char)('0' + m % 10);
		clock_hhmm[5] = ' ';
		clock_hhmm[6] = is_pm ? 'P' : 'A';
		clock_hhmm[7] = 'M';
		clock_hhmm[8] = 0;
	}
	return 1;
}

/* ── main ── */

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

	signal(SIGCHLD, SIG_IGN);

	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0) {
		out("displayd: no /dev/fb0\n");
		return 1;
	}
	struct b1nix_fb_info info;
	if (ioctl(fb_fd, B1NIX_FBIOGET_INFO, &info) != 0) {
		out("displayd: FBIOGET_INFO failed\n");
		return 1;
	}
	scr_w = info.width;
	scr_h = info.height;
	fb = mmap(0, (size_t)info.pitch * scr_h, PROT_READ | PROT_WRITE,
	          MAP_SHARED, fb_fd, 0);
	if (fb == MAP_FAILED) {
		out("displayd: fb mmap failed\n");
		return 1;
	}

	ev_fds[0] = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	ev_fds[1] = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);

	unlink(WAYLAND_SOCKET_PATH);
	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		out("displayd: socket failed\n");
		return 1;
	}
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, WAYLAND_SOCKET_PATH);
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(listen_fd, MAX_CLIENTS) < 0) {
		out("displayd: bind/listen failed\n");
		return 1;
	}

	px = (int)scr_w / 2;
	py = (int)scr_h / 2;
	update_clock();
	composite_rect(0, 0, (int)scr_w, (int)scr_h); /* clear to background */

	out("displayd: ready ");
	out_dec(scr_w);
	out("x");
	out_dec(scr_h);
	out("\n");

	while (running) {
		struct pollfd pfds[3 + MAX_CLIENTS];
		pfds[0].fd = listen_fd;
		pfds[0].events = POLLIN;
		pfds[1].fd = ev_fds[0];
		pfds[1].events = POLLIN;
		pfds[2].fd = ev_fds[1];
		pfds[2].events = POLLIN;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			pfds[3 + i].fd = clients[i].used ? clients[i].fd : -1;
			pfds[3 + i].events = POLLIN;
		}
		for (int i = 0; i < 3 + MAX_CLIENTS; i++)
			pfds[i].revents = 0;

		int pr = poll(pfds, 3 + MAX_CLIENTS, 500);

		/* Tick the clock on every wakeup (poll timeout or activity); repaint
		 * just the top bar when the minute rolls over. */
		if (update_clock())
			composite_rect(0, 0, (int)scr_w, PANEL_H);

		if (pr < 0)
			continue;

		if (pfds[0].revents & POLLIN)
			accept_client(listen_fd);
		if (pfds[1].revents & POLLIN)
			input_drain(0);
		if (pfds[2].revents & POLLIN)
			input_drain(1);
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (pfds[3 + i].fd >= 0 &&
			    (pfds[3 + i].revents & (POLLIN | POLLHUP)))
				client_data(i);
	}

	for (int i = 0; i < MAX_CLIENTS; i++)
		client_disconnect(i);
	unlink(WAYLAND_SOCKET_PATH);
	out("displayd: bye\n");
	return 0;
}
