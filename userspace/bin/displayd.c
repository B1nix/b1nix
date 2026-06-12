/*
 * displayd — the b1nix userspace display server (M47 Phase 2).
 *
 * Single-threaded poll loop over: one listening UNIX socket
 * (B1D_SOCKET_PATH), connected clients, and /dev/input/event0+1.
 * Composites client SHM surfaces damage-driven into the mmap'd /dev/fb0
 * shadow buffer and pushes dirty rectangles with B1NIX_FBIOFLUSH.
 *
 * Protocol: b1nix/display.h (b1display v1, Wayland-shaped — see
 * docs/display-server.md). Server-side policy kept deliberately small:
 * first surface is centered, later ones cascade; 1-px border around each
 * surface; focus follows click (click raises); software crosshair cursor.
 *
 * Buffer release semantics (v1): the server reads a committed buffer on
 * every recomposite (cursor crossing, raise), so B1D_EV_BUFFER_RELEASE is
 * only sent when the buffer stops being the committed one (replaced by a
 * new attach+commit, or the surface is destroyed).
 */
#include <b1nix/display.h>
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
#define CLOSE_COLOR 0x00E05263u

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
	void *mem; /* shmat'd base + offset applied */
	void *shm_base;
	uint32_t w, h, stride;
};

struct dsurface {
	int used;
	uint32_t id;
	int client;
	int x, y;
	int mapped;            /* committed at least once */
	int has_pos;           /* client requested an explicit position */
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
};

static struct dclient clients[MAX_CLIENTS];
static struct dsurface surfaces[MAX_SURFACES];
static struct dbuffer buffers[MAX_BUFFERS];
static struct dtoplevel toplevels[MAX_TOPLEVELS];
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
static char clock_hhmm[6] = "--:--";
static int clock_last_min = -1;

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
	uint8_t msg[B1D_MAX_MSG];
	struct b1d_hdr h;
	h.object_id = obj;
	h.opcode = opcode;
	h.size = (uint16_t)(sizeof(h) + nwords * 4);
	memcpy(msg, &h, sizeof(h));
	memcpy(msg + sizeof(h), words, nwords * 4);
	send(clients[client].fd, msg, h.size, 0);
}

static void send_err(int client, uint32_t obj, uint32_t code) {
	uint32_t w[2] = {obj, code};
	send_msg(client, B1D_OBJ_DISPLAY, B1D_EV_DISPLAY_ERROR, w, 2);
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

static void composite_rect(int rx, int ry, int rw, int rh) {
	if (rx < 0) { rw += rx; rx = 0; }
	if (ry < 0) { rh += ry; ry = 0; }
	if (rw <= 0 || rh <= 0 || (uint32_t)rx >= scr_w || (uint32_t)ry >= scr_h)
		return;
	if ((uint32_t)(rx + rw) > scr_w) rw = (int)scr_w - rx;
	if ((uint32_t)(ry + rh) > scr_h) rh = (int)scr_h - ry;

	/* macOS-style top bar layout, computed once for this rect's panel rows:
	 *   [● b1nix]  [ActiveApp]  File  Edit  View ............ [HH:MM]
	 * The bullet + b1nix is the system menu, ActiveApp is the focused
	 * window's title, the File/Edit/View labels are display-only for now,
	 * and the clock sits flush right. */
	const char *app = active_app_title();
	int app_x = 12 + (5 + 1) * 8;          /* after "b1nix " */
	int applen = 0;
	while (app[applen] && applen < 18) applen++;
	int menu_x = app_x + (applen + 1) * 8;
	int clock_x = (int)scr_w - (5 * 8) - 12; /* "HH:MM" flush right */

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
			draw_text_clipped(row, y, rx, rw, 12, 10, "b1nix", 0x00DCE8F2u);
			draw_text_clipped(row, y, rx, rw, app_x, 10, app, 0x00F4F7FAu);
			draw_text_clipped(row, y, rx, rw, menu_x, 10,
			                  "File  Edit  View", 0x008DA5B8u);
			draw_text_clipped(row, y, rx, rw, clock_x, 10, clock_hhmm,
			                  0x00DCE8F2u);
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
	/* Detach the whole SHM segment; surfaces still pointing here are
	 * unmapped by the callers. */
	if (b->shm_base)
		syscall(SYS_SHMDT, (long)(uintptr_t)b->shm_base, 0, 0, 0, 0, 0);
	memset(b, 0, sizeof(*b));
}

static void surface_destroy(struct dsurface *s) {
	int slot = (int)(s - surfaces);
	if (s->buf)
		send_msg(s->client, s->buf->id, B1D_EV_BUFFER_RELEASE, 0, 0);
	zorder_remove(slot);
	if (enter_slot == slot)
		enter_slot = -1;
	if (focus_slot == slot)
		focus_slot = -1;
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
	close(clients[ci].fd);
	memset(&clients[ci], 0, sizeof(clients[ci]));
}

/* ── request handlers ── */

static void handle_display_req(int ci, uint16_t op, const uint32_t *a,
                               unsigned n) {
	switch (op) {
	case B1D_REQ_DISPLAY_HELLO: {
		uint32_t w[3] = {scr_w, scr_h, B1D_FORMAT_XRGB8888};
		send_msg(ci, B1D_OBJ_DISPLAY, B1D_EV_DISPLAY_INFO, w, 3);
		break;
	}
	case B1D_REQ_DISPLAY_CREATE_SURFACE: {
		if (n < 1)
			return;
		int slot = -1;
		for (int i = 0; i < MAX_SURFACES; i++)
			if (!surfaces[i].used) { slot = i; break; }
		if (slot < 0) {
			send_err(ci, a[0], B1D_ERR_NO_RESOURCE);
			return;
		}
		struct dsurface *s = &surfaces[slot];
		memset(s, 0, sizeof(*s));
		s->used = 1;
		s->id = a[0];
		s->client = ci;
		s->placement = (unsigned)surfaces_created;
		s->x = 48 + 40 * (surfaces_created % 8);
		s->y = 48 + 32 * (surfaces_created % 8);
		surfaces_created++;
		zorder[zcount++] = slot;
		break;
	}
	case B1D_REQ_DISPLAY_CREATE_BUFFER: {
		if (n < 6)
			return;
		uint32_t id = a[0], key = a[1], off = a[2], w = a[3], h = a[4],
		         stride = a[5];
		if (w == 0 || h == 0 || w > 2048 || h > 2048 || stride < w * 4 ||
		    (uint64_t)off + (uint64_t)stride * h > 0x100000) {
			send_err(ci, id, B1D_ERR_BAD_BUFFER);
			return;
		}
		int slot = -1;
		for (int i = 0; i < MAX_BUFFERS; i++)
			if (!buffers[i].used) { slot = i; break; }
		if (slot < 0) {
			send_err(ci, id, B1D_ERR_NO_RESOURCE);
			return;
		}
		int shmid = (int)syscall(SYS_SHMGET, key, off + stride * h, 0666, 0, 0, 0);
		if (shmid < 0) {
			send_err(ci, id, B1D_ERR_BAD_BUFFER);
			return;
		}
		void *mem = (void *)syscall(SYS_SHMAT, shmid, 0, 0, 0, 0, 0);
		if (mem == (void *)-1) {
			send_err(ci, id, B1D_ERR_BAD_BUFFER);
			return;
		}
		struct dbuffer *b = &buffers[slot];
		b->used = 1;
		b->id = id;
		b->client = ci;
		b->shm_base = mem;
		b->mem = (uint8_t *)mem + off;
		b->w = w;
		b->h = h;
		b->stride = stride;
		break;
	}
	case B1D_REQ_DISPLAY_SYNC: {
		if (n < 1)
			return;
		uint32_t w[1] = {frame_serial};
		send_msg(ci, a[0], B1D_EV_CALLBACK_DONE, w, 1);
		break;
	}
	case B1D_REQ_DISPLAY_SHUTDOWN:
		running = 0;
		break;
	case B1D_REQ_DISPLAY_CREATE_TOPLEVEL: {
		if (n < 2)
			return;
		struct dsurface *s = find_surface(ci, a[1]);
		if (!s || surface_toplevel(s)) {
			send_err(ci, a[0], B1D_ERR_BAD_OBJECT);
			return;
		}
		int slot = -1;
		for (int i = 0; i < MAX_TOPLEVELS; i++)
			if (!toplevels[i].used) { slot = i; break; }
		if (slot < 0) {
			send_err(ci, a[0], B1D_ERR_NO_RESOURCE);
			return;
		}
		struct dtoplevel *t = &toplevels[slot];
		memset(t, 0, sizeof(*t));
		t->used = 1;
		t->id = a[0];
		t->client = ci;
		t->surface = s;
		strcpy(t->title, "b1nix");
		break;
	}
	case B1D_REQ_DISPLAY_CHECKSUM: {
		if (n < 1)
			return;
		uint32_t sum = 2166136261u;
		for (uint32_t y = 0; y < scr_h; y += 7)
			for (uint32_t x = 0; x < scr_w; x += 7)
				sum = (sum ^ fb[y * scr_w + x]) * 16777619u;
		uint32_t w[1] = {sum};
		send_msg(ci, a[0], B1D_EV_CALLBACK_VALUE, w, 1);
		break;
	}
	default:
		send_err(ci, B1D_OBJ_DISPLAY, B1D_ERR_BAD_REQUEST);
	}
}

static void handle_toplevel_req(int ci, struct dtoplevel *t, uint16_t op,
                                const uint32_t *a, unsigned n) {
	struct dsurface *s = t->surface;
	switch (op) {
	case B1D_REQ_TOPLEVEL_SET_TITLE: {
		if (n < 1)
			return;
		unsigned len = a[0];
		if (len > sizeof(t->title) - 1)
			len = sizeof(t->title) - 1;
		if (len > (n - 1) * 4)
			len = (n - 1) * 4;
		memcpy(t->title, &a[1], len);
		t->title[len] = 0;
		if (s->mapped)
			composite_surface_region(s);
		break;
	}
	case B1D_REQ_TOPLEVEL_MOVE:
		if (n < 3 || a[0] != input_serial)
			return;
		composite_surface_region(s);
		s->x += (int32_t)a[1];
		s->y += (int32_t)a[2];
		composite_surface_region(s);
		break;
	case B1D_REQ_TOPLEVEL_RESIZE:
		if (n < 3 || a[0] != input_serial || a[1] == 0 || a[2] == 0)
			return;
		{
			uint32_t w[4] = {(uint32_t)s->x, (uint32_t)s->y, a[1], a[2]};
			send_msg(ci, t->id, B1D_EV_TOPLEVEL_CONFIGURE, w, 4);
		}
		break;
	case B1D_REQ_TOPLEVEL_DESTROY:
		memset(t, 0, sizeof(*t));
		if (s->mapped)
			composite_surface_region(s);
		break;
	default:
		send_err(ci, t->id, B1D_ERR_BAD_REQUEST);
	}
}

static void handle_surface_req(int ci, struct dsurface *s, uint16_t op,
                               const uint32_t *a, unsigned n) {
	switch (op) {
	case B1D_REQ_SURFACE_ATTACH:
		if (n < 1)
			return;
		s->pend_buffer_id = a[0];
		s->pend_attach = 1;
		break;
	case B1D_REQ_SURFACE_DAMAGE: {
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
	case B1D_REQ_SURFACE_FRAME:
		if (n < 1)
			return;
		s->frame_cb = a[0];
		s->has_frame_cb = 1;
		break;
	case B1D_REQ_SURFACE_COMMIT: {
		if (s->pend_attach) {
			struct dbuffer *nb = find_buffer(ci, s->pend_buffer_id);
			if (!nb) {
				send_err(ci, s->id, B1D_ERR_BAD_BUFFER);
				return;
			}
			if (s->buf && s->buf != nb)
				send_msg(ci, s->buf->id, B1D_EV_BUFFER_RELEASE, 0, 0);
			s->buf = nb;
			s->pend_attach = 0;
			if (!s->mapped && !s->has_pos) {
				/* Give the default desktop apps distinct, useful positions.
				 * A client that called SET_POSITION keeps its own spot. */
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
			composite_surface_region(s);
			struct dsurface *old = slot_surface(focus_slot);
			if (old) {
				uint32_t leave[1] = {old->id};
				send_msg(old->client, B1D_OBJ_SEAT,
				         B1D_EV_SEAT_FOCUS_LEAVE, leave, 1);
			}
			focus_slot = (int)(s - surfaces);
			zorder_raise(focus_slot);
			uint32_t enter[1] = {s->id};
			send_msg(s->client, B1D_OBJ_SEAT, B1D_EV_SEAT_FOCUS_ENTER,
			         enter, 1);
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
			send_msg(ci, s->frame_cb, B1D_EV_CALLBACK_DONE, w, 1);
			s->has_frame_cb = 0;
		}
		break;
	}
	case B1D_REQ_SURFACE_SET_POSITION: {
		if (n < 2)
			return;
		int nx = (int32_t)a[0], ny = (int32_t)a[1];
		if (s->mapped) {
			composite_surface_region(s);
			s->x = nx;
			s->y = ny;
			composite_surface_region(s);
		} else {
			s->x = nx;
			s->y = ny;
		}
		s->has_pos = 1;
		break;
	}
	case B1D_REQ_SURFACE_DESTROY:
		surface_destroy(s);
		break;
	default:
		send_err(ci, s->id, B1D_ERR_BAD_REQUEST);
	}
}

static void handle_msg(int ci, const struct b1d_hdr *h, const uint32_t *args,
                       unsigned nargs) {
	if (h->object_id == B1D_OBJ_DISPLAY) {
		handle_display_req(ci, h->opcode, args, nargs);
		return;
	}
	struct dsurface *s = find_surface(ci, h->object_id);
	if (s) {
		handle_surface_req(ci, s, h->opcode, args, nargs);
		return;
	}
	struct dbuffer *b = find_buffer(ci, h->object_id);
	if (b) {
		if (h->opcode == B1D_REQ_BUFFER_DESTROY) {
			/* Unmap from any surface still showing it. */
			for (int i = 0; i < MAX_SURFACES; i++)
				if (surfaces[i].used && surfaces[i].buf == b) {
					surfaces[i].buf = 0;
					surfaces[i].mapped = 0;
				}
			buffer_destroy(b);
		} else {
			send_err(ci, h->object_id, B1D_ERR_BAD_REQUEST);
		}
		return;
	}
	struct dtoplevel *t = find_toplevel(ci, h->object_id);
	if (t) {
		handle_toplevel_req(ci, t, h->opcode, args, nargs);
		return;
	}
	send_err(ci, h->object_id, B1D_ERR_BAD_OBJECT);
}

static void client_data(int ci) {
	struct dclient *c = &clients[ci];
	ssize_t n = recv(c->fd, c->inbuf + c->inlen, sizeof(c->inbuf) - c->inlen, 0);
	if (n <= 0) {
		client_disconnect(ci);
		return;
	}
	c->inlen += (unsigned)n;
	for (;;) {
		if (c->inlen < sizeof(struct b1d_hdr))
			break;
		struct b1d_hdr h;
		memcpy(&h, c->inbuf, sizeof(h));
		if (h.size < sizeof(h) || h.size > B1D_MAX_MSG || (h.size & 3)) {
			client_disconnect(ci);
			return;
		}
		if (c->inlen < h.size)
			break;
		uint32_t args[(B1D_MAX_MSG - sizeof(struct b1d_hdr)) / 4];
		unsigned nargs = (h.size - sizeof(h)) / 4;
		memcpy(args, c->inbuf + sizeof(h), nargs * 4);
		memmove(c->inbuf, c->inbuf + h.size, c->inlen - h.size);
		c->inlen -= h.size;
		handle_msg(ci, &h, args, nargs);
		if (!clients[ci].used)
			return; /* disconnected during handling */
	}
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

static void pointer_moved(void) {
	int slot = surface_at(px, py);
	struct dsurface *s = slot_surface(slot);
	if (slot != enter_slot) {
		struct dsurface *old = slot_surface(enter_slot);
		if (old) {
			uint32_t w[1] = {old->id};
			send_msg(old->client, B1D_OBJ_SEAT, B1D_EV_SEAT_POINTER_LEAVE, w, 1);
		}
		if (s) {
			uint32_t w[3] = {s->id, (uint32_t)(px - s->x), (uint32_t)(py - s->y)};
			send_msg(s->client, B1D_OBJ_SEAT, B1D_EV_SEAT_POINTER_ENTER, w, 3);
		}
		enter_slot = slot;
	} else if (s) {
		uint32_t w[2] = {(uint32_t)(px - s->x), (uint32_t)(py - s->y)};
		send_msg(s->client, B1D_OBJ_SEAT, B1D_EV_SEAT_POINTER_MOTION, w, 2);
	}
}

static void pointer_button(uint16_t code, int state) {
	input_serial++;
	if (state) {
		int slot = surface_at(px, py);
		struct dsurface *s = slot_surface(slot);
		if (s && slot != focus_slot) {
			struct dsurface *old = slot_surface(focus_slot);
			if (old) {
				uint32_t w[1] = {old->id};
				send_msg(old->client, B1D_OBJ_SEAT, B1D_EV_SEAT_FOCUS_LEAVE, w, 1);
			}
			focus_slot = slot;
			uint32_t w[1] = {s->id};
			send_msg(s->client, B1D_OBJ_SEAT, B1D_EV_SEAT_FOCUS_ENTER, w, 1);
			zorder_raise(slot);
			composite_surface_region(s);
		}
		if (s && code == B1NIX_BTN_LEFT && surface_toplevel(s) &&
		    py < s->y) {
			if (px >= s->x + (int)s->buf->w - 16) {
				struct dtoplevel *t = surface_toplevel(s);
				send_msg(t->client, t->id, B1D_EV_TOPLEVEL_CLOSE, 0, 0);
			} else {
				drag_slot = slot;
			}
		}
	} else if (code == B1NIX_BTN_LEFT) {
		drag_slot = -1;
	}
	struct dsurface *f = slot_surface(focus_slot);
	if (f) {
		uint32_t w[2] = {code, (uint32_t)state};
		send_msg(f->client, B1D_OBJ_SEAT, B1D_EV_SEAT_POINTER_BUTTON, w, 2);
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
		send_msg(old->client, B1D_OBJ_SEAT, B1D_EV_SEAT_FOCUS_LEAVE, w, 1);
		composite_surface_region(old);
	}
	focus_slot = slot;
	zorder_raise(slot);
	uint32_t w[1] = {next->id};
	send_msg(next->client, B1D_OBJ_SEAT, B1D_EV_SEAT_FOCUS_ENTER, w, 1);
	composite_surface_region(next);
}

static void input_drain(int which) {
	struct b1nix_input_event evs[16];
	static int acc_dx, acc_dy, moved;
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
								         B1D_EV_TOPLEVEL_CLOSE, 0, 0);
							continue;
						}
						uint32_t w[2] = {e->code, (uint32_t)e->value};
						send_msg(f->client, B1D_OBJ_SEAT, B1D_EV_SEAT_KEY, w, 2);
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
			case B1NIX_EV_KEY:
				pointer_button(e->code, e->value);
				break;
			case B1NIX_EV_SYN:
				if (moved) {
					int ox = px, oy = py;
					px += acc_dx;
					py += acc_dy;
					acc_dx = acc_dy = 0;
					moved = 0;
					if (px < 0) px = 0;
					if (py < 0) py = 0;
					if (px >= (int)scr_w) px = (int)scr_w - 1;
					if (py >= (int)scr_h) py = (int)scr_h - 1;
					if (px != ox || py != oy) {
						struct dsurface *drag = slot_surface(drag_slot);
						if (drag) {
							composite_surface_region(drag);
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
	if (t->tm_min == clock_last_min)
		return 0;
	clock_last_min = t->tm_min;
	int h = t->tm_hour, m = t->tm_min;
	clock_hhmm[0] = (char)('0' + (h / 10) % 10);
	clock_hhmm[1] = (char)('0' + h % 10);
	clock_hhmm[2] = ':';
	clock_hhmm[3] = (char)('0' + (m / 10) % 10);
	clock_hhmm[4] = (char)('0' + m % 10);
	clock_hhmm[5] = 0;
	return 1;
}

/* ── main ── */

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

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

	unlink(B1D_SOCKET_PATH);
	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		out("displayd: socket failed\n");
		return 1;
	}
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, B1D_SOCKET_PATH);
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

		if (pfds[0].revents & POLLIN) {
			int cfd = accept(listen_fd, 0, 0);
			if (cfd >= 0) {
				int slot = -1;
				for (int i = 0; i < MAX_CLIENTS; i++)
					if (!clients[i].used) { slot = i; break; }
				if (slot < 0) {
					close(cfd);
				} else {
					clients[slot].used = 1;
					clients[slot].fd = cfd;
					clients[slot].inlen = 0;
				}
			}
		}
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
	unlink(B1D_SOCKET_PATH);
	out("displayd: bye\n");
	return 0;
}
