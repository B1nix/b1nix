/*
 * displayd_window.c — surface/buffer/toplevel management, z-order,
 *                     minimize/restore, focus, drag/resize.
 */
#include "displayd.h"

/* ── surface/buffer/toplevel lookups ── */
struct dsurface *slot_surface(int slot) {
	return (slot >= 0 && slot < MAX_SURFACES && surfaces[slot].used)
	           ? &surfaces[slot] : 0;
}

struct dsurface *find_surface(int client, uint32_t id) {
	for (int i = 0; i < MAX_SURFACES; i++)
		if (surfaces[i].used && surfaces[i].client == client &&
		    surfaces[i].id == id)
			return &surfaces[i];
	return 0;
}

struct dbuffer *find_buffer(int client, uint32_t id) {
	for (int i = 0; i < MAX_BUFFERS; i++)
		if (buffers[i].used && buffers[i].client == client &&
		    buffers[i].id == id)
			return &buffers[i];
	return 0;
}

struct dtoplevel *find_toplevel(int client, uint32_t id) {
	for (int i = 0; i < MAX_TOPLEVELS; i++)
		if (toplevels[i].used && toplevels[i].client == client &&
		    toplevels[i].id == id)
			return &toplevels[i];
	return 0;
}

struct dtoplevel *surface_toplevel(struct dsurface *s) {
	for (int i = 0; i < MAX_TOPLEVELS; i++)
		if (toplevels[i].used && toplevels[i].surface == s)
			return &toplevels[i];
	return 0;
}

/* ── z-order ── */
void zorder_remove(int slot) {
	int j = 0;
	for (int i = 0; i < zcount; i++)
		if (zorder[i] != slot) zorder[j++] = zorder[i];
	zcount = j;
}

void zorder_raise(int slot) {
	zorder_remove(slot);
	zorder[zcount++] = slot;
}

/* ── create / destroy ── */
void create_surface(int ci, uint32_t id) {
	int slot = -1;
	for (int i = 0; i < MAX_SURFACES; i++)
		if (!surfaces[i].used) { slot = i; break; }
	if (slot < 0) return;
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

void create_toplevel(int ci, uint32_t id, uint32_t surface_id) {
	struct dsurface *s = find_surface(ci, surface_id);
	if (!s || surface_toplevel(s)) return;
	int slot = -1;
	for (int i = 0; i < MAX_TOPLEVELS; i++)
		if (!toplevels[i].used) { slot = i; break; }
	if (slot < 0) return;
	struct dtoplevel *t = &toplevels[slot];
	memset(t, 0, sizeof(*t));
	t->used = 1;
	t->id = id;
	t->client = ci;
	t->surface = s;
	strcpy(t->title, "Wayland");
	memset(&app_menus[slot], 0, sizeof(app_menus[slot]));
}

void buffer_destroy(struct dbuffer *b) {
	if (b->map_base) munmap(b->map_base, b->map_size);
	memset(b, 0, sizeof(*b));
}

void surface_destroy(struct dsurface *s) {
	int slot = (int)(s - surfaces);
	if (s->buf) send_msg(s->client, s->buf->id, 0, 0, 0);
	zorder_remove(slot);
	if (enter_slot == slot) enter_slot = -1;
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

/* ── hit test ── */
int surface_at(int x, int y) {
	if (y >= (int)scr_h - DOCK_H) return -1;
	for (int zi = zcount - 1; zi >= 0; zi--) {
		struct dsurface *s = slot_surface(zorder[zi]);
		if (!s || !s->mapped || !s->buf) continue;
		int top = surface_toplevel(s) ? TITLE_H : 0;
		if (x >= s->x && x < s->x + (int)s->buf->w && y >= s->y - top &&
		    y < s->y + (int)s->buf->h)
			return zorder[zi];
	}
	return -1;
}

/* ── work area ── */
uint32_t work_w(void) { return scr_w; }
uint32_t work_h(void) { return scr_h - PANEL_H - DOCK_H; }

/* ── focus ── */
void focus_cycle(void) {
	if (zcount == 0) return;
	int pos = -1;
	for (int i = 0; i < zcount; i++)
		if (zorder[i] == focus_slot) pos = i;
	int slot = zorder[(pos + 1) % zcount];
	struct dsurface *next = slot_surface(slot);
	if (!next || !next->mapped) return;
	struct dsurface *old = slot_surface(focus_slot);
	if (old) {
		uint32_t w[1] = {old->id};
		send_seat_event(old->client, SEAT_FOCUS_LEAVE, w, 1);
	}
	focus_slot = slot;
	zorder_raise(slot);
	uint32_t w[1] = {next->id};
	send_seat_event(next->client, SEAT_FOCUS_ENTER, w, 1);
	if (old) composite_surface_region(old);
	composite_rect(0, 0, (int)scr_w, PANEL_H);
	composite_surface_region(next);
}

void close_focused_window(void) {
	struct dsurface *f = slot_surface(focus_slot);
	struct dtoplevel *t = f ? surface_toplevel(f) : 0;
	if (t) send_msg(t->client, t->id, 1, 0, 0);
}

/* ── minimize / restore ── */
void minimize_toplevel(struct dtoplevel *t) {
	struct dsurface *s = t->surface;
	if (!s || t->minimized) return;
	int slot = (int)(s - surfaces);
	int x = s->x, y = s->y;
	int w = s->buf ? (int)s->buf->w : 0, h = s->buf ? (int)s->buf->h : 0;
	t->minimized = 1;
	s->mapped = 0;
	zorder_remove(slot);
	if (enter_slot == slot) enter_slot = -1;
	if (focus_slot == slot) {
		uint32_t leave[1] = {s->id};
		send_seat_event(s->client, SEAT_FOCUS_LEAVE, leave, 1);
		focus_slot = zcount > 0 ? zorder[zcount - 1] : -1;
		struct dsurface *nf = slot_surface(focus_slot);
		if (nf) {
			uint32_t en[1] = {nf->id};
			send_seat_event(nf->client, SEAT_FOCUS_ENTER, en, 1);
		}
	}
	composite_rect(x - 1, y - TITLE_H, w + 2, h + TITLE_H + 1);
	composite_rect(0, 0, (int)scr_w, PANEL_H);
	/* repaint dock */
	composite_rect(0, (int)scr_h - DOCK_H, (int)scr_w, DOCK_H);
}

void restore_toplevel(struct dtoplevel *t) {
	struct dsurface *s = t->surface;
	if (!s || !t->minimized || !s->buf) return;
	int slot = (int)(s - surfaces);
	t->minimized = 0;
	s->mapped = 1;
	struct dsurface *old = slot_surface(focus_slot);
	if (old && old != s) {
		uint32_t leave[1] = {old->id};
		send_seat_event(old->client, SEAT_FOCUS_LEAVE, leave, 1);
	}
	zorder_raise(slot);
	focus_slot = slot;
	uint32_t en[1] = {s->id};
	send_seat_event(s->client, SEAT_FOCUS_ENTER, en, 1);
	composite_rect(0, 0, (int)scr_w, (int)scr_h);
}

/* ── toplevel state (maximize/fullscreen) ── */
void send_state_configure(struct dtoplevel *t, uint32_t w, uint32_t h,
                                 const uint32_t *states, unsigned nstates) {
	uint32_t msg[8];
	if (nstates > 4) nstates = 4;
	msg[0] = w; msg[1] = h;
	msg[2] = nstates * 4;
	for (unsigned i = 0; i < nstates; i++) msg[3 + i] = states[i];
	send_msg(t->client, t->id, 0, msg, 3 + nstates);
	for (int i = 0; i < MAX_WOBJECTS; i++)
		if (wobjects[i].used && wobjects[i].client == t->client &&
		    wobjects[i].type == WOBJ_XDG_SURFACE && t->surface &&
		    wobjects[i].link == t->surface->id) {
			uint32_t serial = ++frame_serial;
			send_msg(t->client, wobjects[i].id, 0, &serial, 1);
			wobjects[i].configured = 1;
			break;
		}
	t->geom_dirty = 1;
}

void toplevel_set_state(struct dtoplevel *t, int maximized, int fullscreen) {
	struct dsurface *s = t->surface;
	if (!s) return;
	if (!t->maximized && !t->fullscreen && (maximized || fullscreen)) {
		t->saved_x = s->x;
		t->saved_y = s->y;
	}
	t->restoring = (!maximized && !fullscreen);
	t->maximized = maximized;
	t->fullscreen = fullscreen;
	uint32_t states[2];
	unsigned n = 0;
	uint32_t w = 0, h = 0;
	if (fullscreen) {
		states[n++] = 2; w = scr_w; h = scr_h;
	} else if (maximized) {
		states[n++] = 1; w = work_w(); h = work_h();
	}
	states[n++] = 4;
	send_state_configure(t, w, h, states, n);
}

/* ── send_focused_shortcut ── */
void send_focused_shortcut(uint32_t key) {
	struct dsurface *f = slot_surface(focus_slot);
	if (!f) return;
	uint32_t ctrl_down[2] = {0x1d, 1};
	uint32_t key_down[2] = {key, 1};
	uint32_t key_up[2] = {key, 0};
	uint32_t ctrl_up[2] = {0x1d, 0};
	send_seat_event(f->client, SEAT_KEY, ctrl_down, 2);
	send_seat_event(f->client, SEAT_KEY, key_down, 2);
	send_seat_event(f->client, SEAT_KEY, key_up, 2);
	send_seat_event(f->client, SEAT_KEY, ctrl_up, 2);
}

void send_focus_modifiers(void) {
	struct dsurface *f = slot_surface(focus_slot);
	if (!f) return;
	struct wobject *kbd = wobject_type_find(f->client, WOBJ_KEYBOARD);
	if (kbd) send_kbd_modifiers(f->client, kbd->id);
}
