/*
 * displayd_render.c — compositing, panel, dock, text drawing.
 */
#include "displayd.h"
#include "font8x8.h"

/* ── text drawing ── */
int text_len(const char *s, int limit) {
	int n = 0;
	while (s[n] && n < limit) n++;
	return n;
}

void draw_text_clipped(uint32_t *row, int screen_y, int rx, int rw,
                       int x0, int y0, const char *text, uint32_t color) {
	if (screen_y < y0 || screen_y >= y0 + 8) return;
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

/* ── active app title (for the macOS-style menu bar) ── */
const char *active_app_title(void) {
	struct dsurface *f = slot_surface(focus_slot);
	if (f) {
		struct dtoplevel *t = surface_toplevel(f);
		if (t && t->title[0]) return t->title;
	}
	return "Finder";
}

/* ── panel layout ── */
struct panel_layout get_panel_layout(void) {
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
	p.window_x = p.view_x + p.view_w;
	p.window_w = 8 * 8;
	p.clock_w = strlen(clock_hhmm) * 8 + 16;
	p.clock_x = (int)scr_w - p.clock_w - 4;
	return p;
}

/* ── blit one surface buffer (no decoration) ── */
void blit_surface_at(struct dsurface *s, int ax, int ay, int rx, int ry,
                     int rw, int rh) {
	if (!s || !s->buf) return;
	int sw = (int)s->buf->w, sh = (int)s->buf->h;
	for (int y = ry; y < ry + rh; y++) {
		if (y < ay || y >= ay + sh) continue;
		uint32_t *row = fb + (uint32_t)y * scr_w;
		int ly = y - ay;
		const uint8_t *src =
		    (const uint8_t *)s->buf->mem + (uint32_t)ly * s->buf->stride;
		for (int x = rx; x < rx + rw; x++) {
			if (x < ax || x >= ax + sw) continue;
			row[x] = ((const uint32_t *)src)[x - ax];
		}
	}
}

/* ── composite a rectangular region ── */
void composite_rect(int rx, int ry, int rw, int rh) {
	if (rx < 0) { rw += rx; rx = 0; }
	if (ry < 0) { rh += ry; ry = 0; }
	if (rw <= 0 || rh <= 0 || (uint32_t)rx >= scr_w || (uint32_t)ry >= scr_h)
		return;
	if ((uint32_t)(rx + rw) > scr_w) rw = (int)scr_w - rx;
	if ((uint32_t)(ry + rh) > scr_h) rh = (int)scr_h - ry;

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
			    {MENU_SYSTEM, panel.system_x, panel.system_w, "b1nix", 0x00DCE8F2u},
			    {MENU_APP, panel.app_x, panel.app_w, app, 0x00F4F7FAu},
			    {MENU_FILE, panel.file_x, panel.file_w, "File", 0x00DCE8F2u},
			    {MENU_EDIT, panel.edit_x, panel.edit_w, "Edit", 0x00DCE8F2u},
			    {MENU_VIEW, panel.view_x, panel.view_w, "View", 0x00DCE8F2u},
			    {MENU_WINDOW, panel.window_x, panel.window_w, "Window", 0x00DCE8F2u},
			    {MENU_CLOCK, panel.clock_x, panel.clock_w, clock_hhmm, 0x00DCE8F2u},
			};
			for (unsigned hi = 0; hi < sizeof(headers) / sizeof(headers[0]); hi++) {
				if (headers[hi].x >= panel.clock_x &&
				    headers[hi].menu != MENU_CLOCK)
					continue;
				if (headers[hi].menu == open_menu)
					for (int x = headers[hi].x; x < headers[hi].x + headers[hi].w; x++)
						if (x >= rx && x < rx + rw) row[x] = PANEL_ACTIVE_COLOR;
				draw_text_clipped(row, y, rx, rw, headers[hi].x + 8, 10,
				                  headers[hi].label, headers[hi].color);
			}
			/* Taskbar buttons */
			int tx0 = taskbar_x0();
			for (int n = 0;; n++) {
				int tslot = taskbar_slot(n);
				if (tslot < 0) break;
				int bx = tx0 + n * TASKBTN_W;
				if (bx + TASKBTN_W - 4 >= panel.clock_x - 4) break;
				struct dtoplevel *t = &toplevels[tslot];
				int active = t->surface &&
				             (int)(t->surface - surfaces) == focus_slot &&
				             !t->minimized;
				for (int x = bx; x < bx + TASKBTN_W - 6; x++)
					if (x >= rx && x < rx + rw)
						row[x] = active ? PANEL_ACTIVE_COLOR
						                : t->minimized ? 0x001A2430u : 0x00222E3Cu;
				uint32_t tc = t->minimized ? 0x008A97A2u : 0x00DCE8F2u;
				char label[12];
				const char *src = t->title[0] ? t->title : "Window";
				int li = 0;
				for (; src[li] && li < 11; li++) label[li] = src[li];
				label[li] = 0;
				draw_text_clipped(row, y, rx, rw, bx + 6, 10, label, tc);
			}
		}
	}

	/* surfaces bottom → top */
	for (int zi = 0; zi < zcount; zi++) {
		struct dsurface *s = slot_surface(zorder[zi]);
		if (!s || !s->mapped || !s->buf) continue;
		int sw = (int)s->buf->w, sh = (int)s->buf->h;
		struct dtoplevel *top_obj = 0;
		for (int ti = 0; ti < MAX_TOPLEVELS; ti++)
			if (toplevels[ti].used && toplevels[ti].surface == s)
				top_obj = &toplevels[ti];
		int decorated = top_obj != 0;
		int top = decorated ? TITLE_H : 1;
		for (int y = ry; y < ry + rh; y++) {
			if (y < s->y - top || y > s->y + sh) continue;
			uint32_t *row = fb + (uint32_t)y * scr_w;
			for (int x = rx; x < rx + rw; x++) {
				if (x < s->x - 1 || x > s->x + sw) continue;
				int lx = x - s->x, ly = y - s->y;
				if (lx >= 0 && lx < sw && ly >= 0 && ly < sh) {
					const uint8_t *src = (const uint8_t *)s->buf->mem +
					                     (uint32_t)ly * s->buf->stride;
					row[x] = ((const uint32_t *)src)[lx];
				} else {
					if (decorated && y < s->y) {
						uint32_t c = zorder[zi] == focus_slot
						                 ? TITLE_FOCUS_COLOR : TITLE_COLOR;
						if (x >= s->x + sw - 15) c = CLOSE_COLOR;
						row[x] = c;
					} else {
						row[x] = BORDER_COLOR;
					}
				}
			}
			if (decorated && top_obj)
				draw_text_clipped(row, y, rx, rw, s->x + 6,
				                  s->y - TITLE_H + 3, top_obj->title, 0x00F4F7FAu);
		}
		for (int si = 0; si < MAX_SURFACES; si++) {
			struct dsurface *sub = &surfaces[si];
			if (sub->used && sub->mapped && sub->buf && sub->parent == s)
				blit_surface_at(sub, s->x + sub->sub_x, s->y + sub->sub_y,
				                rx, ry, rw, rh);
		}
	}

	/* ── Bottom dock (macOS-style) ── */
	{
		int dock_y = (int)scr_h - DOCK_H;
		if (ry + rh > dock_y) {
			for (int y = (ry > dock_y ? ry : dock_y); y < ry + rh && y < (int)scr_h; y++) {
				uint32_t *row = fb + (uint32_t)y * scr_w;
				for (int x = rx; x < rx + rw; x++)
					row[x] = (y == dock_y) ? DOCK_BORDER_COLOR : DOCK_BG_COLOR;
			}
			int item_count = 0;
			for (int i = 0; i < MAX_TOPLEVELS; i++)
				if (toplevels[i].used && toplevels[i].surface) item_count++;
			if (item_count > 0) {
				int total_w = item_count * DOCK_ITEM_W;
				int dx0 = ((int)scr_w - total_w) / 2;
				if (dx0 < 4) dx0 = 4;
				int di = 0;
				for (int i = 0; i < MAX_TOPLEVELS; i++) {
					struct dtoplevel *t = &toplevels[i];
					if (!t->used || !t->surface) continue;
					int ix = dx0 + di * DOCK_ITEM_W + DOCK_PAD;
					int iy = dock_y + (DOCK_H - DOCK_ICON_H) / 2;
					int is_focused = t->surface &&
					    (int)(t->surface - surfaces) == focus_slot && !t->minimized;
					for (int y = iy; y < iy + DOCK_ICON_H; y++) {
						if (y < ry || y >= ry + rh) continue;
						uint32_t *row = fb + (uint32_t)y * scr_w;
						for (int x = ix; x < ix + DOCK_ICON_W; x++) {
							if (x < rx || x >= rx + rw) continue;
							uint32_t base = is_focused ? DOCK_ACTIVE_COLOR : 0x002A3A4Au;
							int shade = ((y - iy) * 8) / DOCK_ICON_H;
							row[x] = base + (uint32_t)(shade << 16) + (uint32_t)(shade << 8);
						}
					}
					char label[8];
					const char *src = t->title[0] ? t->title : "?";
					int li = 0;
					for (; src[li] && li < 7; li++) label[li] = src[li];
					label[li] = 0;
					uint32_t tc = t->minimized ? 0x006A7A8Au : 0x00DCE8F2u;
					if (iy + 12 >= ry && iy + 12 < ry + rh)
						draw_text_clipped(fb + (uint32_t)(iy + 12) * scr_w,
						                  iy + 12, rx, rw, ix + 4, iy + 12, label, tc);
					if (!t->minimized) {
						int dot_y = iy + DOCK_ICON_H + 2;
						if (dot_y >= ry && dot_y < ry + rh) {
							uint32_t *row = fb + (uint32_t)dot_y * scr_w;
							int cx = ix + DOCK_ICON_W / 2;
							if (cx >= rx && cx < rx + rw) row[cx] = DOCK_DOT_COLOR;
							if (cx - 1 >= rx && cx - 1 < rx + rw) row[cx - 1] = DOCK_DOT_COLOR;
							if (cx + 1 >= rx && cx + 1 < rx + rw) row[cx + 1] = DOCK_DOT_COLOR;
						}
					}
					di++;
				}
			}
		}
	}

	/* Menus above client windows */
	draw_panel_overlay(rx, ry, rw, rh);

	/* arrow cursor on top */
	for (int cyi = 0; cyi < CURSOR_H; cyi++) {
		int cy = py + cyi;
		if (cy < ry || cy >= ry + rh) continue;
		const char *crow = cursor_bitmap[cyi];
		for (int cxi = 0; crow[cxi]; cxi++) {
			char c = crow[cxi];
			if (c == ' ') continue;
			int cx = px + cxi;
			if (cx < rx || cx >= rx + rw) continue;
			fb[(uint32_t)cy * scr_w + cx] =
			    (c == 'B') ? 0x00000000u : 0x00FFFFFFu;
		}
	}

	struct b1nix_fb_rect rect = {(uint32_t)rx, (uint32_t)ry, (uint32_t)rw,
	                             (uint32_t)rh};
	ioctl(fb_fd, B1NIX_FBIOFLUSH, &rect);
}

void composite_surface_region(struct dsurface *s) {
	int sw = s->buf ? (int)s->buf->w : 0;
	int sh = s->buf ? (int)s->buf->h : 0;
	int top = 1;
	for (int i = 0; i < MAX_TOPLEVELS; i++)
		if (toplevels[i].used && toplevels[i].surface == s) top = TITLE_H;
	composite_rect(s->x - 1, s->y - top, sw + 2, sh + top + 1);
}
