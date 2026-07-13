/*
 * displayd_menu.c — panel menus, desktop context menu, app menus.
 */
#include "displayd.h"

/* ── menu geometry ── */
int menu_x(enum panel_menu menu) {
	struct panel_layout p = get_panel_layout();
	int x;
	switch (menu) {
	case MENU_SYSTEM: x = p.system_x; break;
	case MENU_APP: x = p.app_x; break;
	case MENU_FILE: x = p.file_x; break;
	case MENU_EDIT: x = p.edit_x; break;
	case MENU_VIEW: x = p.view_x; break;
	case MENU_WINDOW: x = p.window_x; break;
	case MENU_CLOCK: x = p.clock_x + p.clock_w - MENU_W; break;
	case MENU_DESKTOP: x = desktop_menu_x; break;
	default: x = 0; break;
	}
	if (x + MENU_W > (int)scr_w) x = (int)scr_w - MENU_W;
	if (x < 0) x = 0;
	return x;
}

int menu_h(enum panel_menu menu) {
	return menu_item_count(menu) * MENU_ITEM_H + 8;
}

/* ── panel header hit test ── */
enum panel_menu panel_menu_at(int x) {
	struct panel_layout p = get_panel_layout();
	if (x >= p.system_x && x < p.system_x + p.system_w) return MENU_SYSTEM;
	if (x >= p.app_x && x < p.app_x + p.app_w) return MENU_APP;
	if (x >= p.file_x && x < p.file_x + p.file_w && x < p.clock_x) return MENU_FILE;
	if (x >= p.edit_x && x < p.edit_x + p.edit_w && x < p.clock_x) return MENU_EDIT;
	if (x >= p.view_x && x < p.view_x + p.view_w && x < p.clock_x) return MENU_VIEW;
	if (x >= p.window_x && x < p.window_x + p.window_w && x < p.clock_x) return MENU_WINDOW;
	if (x >= p.clock_x && x < p.clock_x + p.clock_w) return MENU_CLOCK;
	return MENU_NONE;
}

/* ── taskbar ── */
int taskbar_x0(void) {
	struct panel_layout p = get_panel_layout();
	return p.window_x + p.window_w + 8;
}

int taskbar_slot(int n) {
	int seen = 0;
	for (int i = 0; i < MAX_TOPLEVELS; i++)
		if (toplevels[i].used && toplevels[i].surface)
			if (seen++ == n) return i;
	return -1;
}

int taskbar_button_at(int x, int y) {
	if (y < 0 || y >= PANEL_H) return -1;
	struct panel_layout p = get_panel_layout();
	int x0 = taskbar_x0();
	if (x < x0 || x >= p.clock_x - 4) return -1;
	int n = (x - x0) / TASKBTN_W;
	return taskbar_slot(n) >= 0 ? n : -1;
}

/* ── menu item counts ── */
int menu_item_count(enum panel_menu menu) {
	switch (menu) {
	case MENU_SYSTEM: return 7;
	case MENU_APP: {
		if (focus_slot < 0) return 0;
		for (int i = 0; i < MAX_TOPLEVELS; i++)
			if (toplevels[i].used && toplevels[i].surface &&
			    (int)(toplevels[i].surface - surfaces) == focus_slot)
				return app_menus[i].count;
		return 0;
	}
	case MENU_FILE: return 4;
	case MENU_EDIT: return 6;
	case MENU_VIEW: return 4;
	case MENU_WINDOW: return 5;
	case MENU_CLOCK: return 3;
	case MENU_DESKTOP: return 6;
	default: return 0;
	}
}

/* ── menu item labels ── */
const char *menu_item_label(enum panel_menu menu, int item) {
	switch (menu) {
	case MENU_SYSTEM: {
		static const char *labels[] = {
		    "About b1nix", "Terminal", "Paint", "Clock App",
		    "", "Next Window", "Close Window"};
		return labels[item];
	}
	case MENU_APP: {
		if (focus_slot < 0) return "";
		for (int i = 0; i < MAX_TOPLEVELS; i++)
			if (toplevels[i].used && toplevels[i].surface &&
			    (int)(toplevels[i].surface - surfaces) == focus_slot) {
				if (item < app_menus[i].count) return app_menus[i].items[item].label;
				break;
			}
		return "";
	}
	case MENU_FILE: {
		static const char *labels[] = {"New Terminal", "Close Window", "", "Quit"};
		return labels[item];
	}
	case MENU_EDIT: {
		static const char *labels[] = {"Undo", "", "Cut", "Copy", "Paste", "Select All"};
		return labels[item];
	}
	case MENU_VIEW: {
		static const char *labels[] = {"Fullscreen", "", "Cascade Windows", "Tile Windows"};
		return labels[item];
	}
	case MENU_WINDOW: {
		static const char *labels[] = {"Minimize", "Zoom (Maximize)", "", "Bring All to Front", "Close"};
		return labels[item];
	}
	case MENU_CLOCK: {
		if (item == 0) return "Open Clock App";
		if (item == 1) return clock_24h ? "[X] 24-hour clock" : "[ ] 24-hour clock";
		if (item == 2) return "b1nix local time";
		return "";
	}
	case MENU_DESKTOP: {
		static const char *labels[] = {"New Terminal", "New Paint", "", "Cascade Windows", "Tile Windows", "About b1nix"};
		return labels[item];
	}
	default: return "";
	}
}

/* ── separator / enabled checks ── */
int menu_item_is_separator(enum panel_menu menu, int item) {
	if (menu == MENU_SYSTEM && item == 4) return 1;
	if (menu == MENU_FILE && item == 2) return 1;
	if (menu == MENU_EDIT && item == 1) return 1;
	if (menu == MENU_VIEW && item == 1) return 1;
	if (menu == MENU_WINDOW && item == 2) return 1;
	if (menu == MENU_DESKTOP && item == 2) return 1;
	if (menu == MENU_APP) {
		if (focus_slot < 0) return 0;
		for (int i = 0; i < MAX_TOPLEVELS; i++)
			if (toplevels[i].used && toplevels[i].surface &&
			    (int)(toplevels[i].surface - surfaces) == focus_slot)
				return (app_menus[i].items[item].flags & 0x02) != 0;
	}
	return 0;
}

int menu_item_enabled(enum panel_menu menu, int item) {
	if (menu_item_is_separator(menu, item)) return 0;
	if (menu == MENU_CLOCK || menu == MENU_DESKTOP) return 1;
	if ((menu == MENU_APP || menu == MENU_FILE || menu == MENU_EDIT ||
	     menu == MENU_WINDOW) && !slot_surface(focus_slot))
		return 0;
	if (menu == MENU_SYSTEM) {
		if (item == 5 && zcount == 0) return 0;
		if (item == 6 && !slot_surface(focus_slot)) return 0;
	}
	if (menu == MENU_APP && focus_slot >= 0) {
		for (int i = 0; i < MAX_TOPLEVELS; i++)
			if (toplevels[i].used && toplevels[i].surface &&
			    (int)(toplevels[i].surface - surfaces) == focus_slot)
				return (app_menus[i].items[item].flags & 0x01) == 0;
	}
	return 1;
}

/* ── menu item hit test ── */
int menu_item_at(int x, int y) {
	if (open_menu == MENU_NONE) return -1;
	int mx = menu_x(open_menu);
	int my = (open_menu == MENU_DESKTOP) ? desktop_menu_y : PANEL_H;
	int mh = menu_h(open_menu);
	if (x < mx || x >= mx + MENU_W || y < my + 4 || y >= my + mh - 4)
		return -1;
	int item = (y - my - 4) / MENU_ITEM_H;
	return item < menu_item_count(open_menu) ? item : -1;
}

/* ── open / close ── */
void open_panel_menu(enum panel_menu menu) {
	open_menu = menu;
	if (menu == MENU_DESKTOP) { desktop_menu_x = px; desktop_menu_y = py; }
	menu_hover = menu_item_at(px, py);
	composite_rect(0, 0, (int)scr_w, (int)scr_h);
}

void close_panel_menu(void) {
	if (open_menu == MENU_NONE) return;
	open_menu = MENU_NONE;
	menu_hover = -1;
	composite_rect(0, 0, (int)scr_w, (int)scr_h);
}

/* ── draw the dropdown overlay ── */
void draw_panel_overlay(int rx, int ry, int rw, int rh) {
	if (open_menu == MENU_NONE) return;
	int mx = menu_x(open_menu);
	int mh = menu_h(open_menu);
	int my = (open_menu == MENU_DESKTOP) ? desktop_menu_y : PANEL_H;
	int x0 = mx - 2, x1 = mx + MENU_W + 3;
	int y0 = my, y1 = my + mh + 3;
	for (int y = ry; y < ry + rh; y++) {
		if (y < y0 || y >= y1) continue;
		uint32_t *row = fb + (uint32_t)y * scr_w;
		for (int x = rx; x < rx + rw; x++) {
			if (x < x0 || x >= x1) continue;
			if (x >= mx + 3 && x < mx + MENU_W + 3 && y >= my + 3)
				row[x] = 0x00060A0Du;
			if (x >= mx && x < mx + MENU_W && y < my + mh) {
				int border = x == mx || x == mx + MENU_W - 1 ||
				             y == my || y == my + mh - 1;
				row[x] = border ? 0x006B7780u : MENU_BG_COLOR;
			}
		}
		for (int item = 0; item < menu_item_count(open_menu); item++) {
			int iy = my + 4 + item * MENU_ITEM_H;
			if (y < iy || y >= iy + MENU_ITEM_H) continue;
			if (menu_item_is_separator(open_menu, item)) {
				if (y == iy + MENU_ITEM_H / 2)
					for (int x = mx + 8; x < mx + MENU_W - 8; x++)
						if (x >= rx && x < rx + rw) row[x] = 0x00A0A8B0u;
				continue;
			}
			int enabled = menu_item_enabled(open_menu, item);
			if (enabled && item == menu_hover)
				for (int x = mx + 4; x < mx + MENU_W - 4; x++)
					if (x >= rx && x < rx + rw) row[x] = MENU_HOVER_COLOR;
			uint32_t color = !enabled ? MENU_DISABLED_COLOR
			                         : item == menu_hover ? 0x00FFFFFFu : MENU_TEXT_COLOR;
			draw_text_clipped(row, y, rx, rw, mx + 12, iy + 5,
			                  menu_item_label(open_menu, item), color);
			/* accelerator text (app menu) */
			const char *accel = "";
			if (open_menu == MENU_APP && focus_slot >= 0) {
				for (int i = 0; i < MAX_TOPLEVELS; i++)
					if (toplevels[i].used && toplevels[i].surface &&
					    (int)(toplevels[i].surface - surfaces) == focus_slot) {
						if (item < app_menus[i].count) accel = app_menus[i].items[item].accel;
						break;
					}
			}
			if (accel[0]) {
				int alen = text_len(accel, 10);
				draw_text_clipped(row, y, rx, rw, mx + MENU_W - alen * 8 - 12,
				                  iy + 5, accel, color);
			}
		}
	}
}

/* ── send app menu event to the focused client ── */
static void send_app_menu_event(uint16_t item_id) {
	struct dsurface *f = slot_surface(focus_slot);
	if (!f) return;
	struct dtoplevel *t = surface_toplevel(f);
	if (!t) return;
	uint32_t args[1] = { (uint32_t)item_id };
	send_msg(t->client, t->id, 20, args, 1);
}

/* ── activate a menu item ── */
void activate_menu_item(enum panel_menu menu, int item) {
	if (!menu_item_enabled(menu, item)) return;
	close_panel_menu();

	if (menu == MENU_SYSTEM) {
		if (item == 0) spawn_app("/bin/gabout", NULL);
		else if (item == 1) spawn_app("/bin/gterm", NULL);
		else if (item == 2) spawn_app("/bin/gpaint", NULL);
		else if (item == 3) spawn_app("/bin/gclock", NULL);
		else if (item == 5) focus_cycle();
		else if (item == 6) close_focused_window();
	}
	else if (menu == MENU_APP) {
		if (focus_slot >= 0) {
			for (int i = 0; i < MAX_TOPLEVELS; i++)
				if (toplevels[i].used && toplevels[i].surface &&
				    (int)(toplevels[i].surface - surfaces) == focus_slot) {
					if (item < app_menus[i].count)
						send_app_menu_event(app_menus[i].items[item].id);
					break;
				}
		}
	}
	else if (menu == MENU_FILE) {
		if (item == 0) spawn_app("/bin/gterm", NULL);
		else if (item == 1) close_focused_window();
		else if (item == 3) close_focused_window();
	}
	else if (menu == MENU_EDIT) {
		if (item == 0) send_focused_shortcut(0x2c);      /* Ctrl+Z */
		else if (item == 2) send_focused_shortcut(0x2d);  /* Ctrl+X */
		else if (item == 3) send_focused_shortcut(0x2e);  /* Ctrl+C */
		else if (item == 4) send_focused_shortcut(0x2f);  /* Ctrl+V */
		else if (item == 5) send_focused_shortcut(0x1e);  /* Ctrl+A */
	}
	else if (menu == MENU_VIEW) {
		if (item == 0 && focus_slot >= 0) {
			struct dtoplevel *t = surface_toplevel(slot_surface(focus_slot));
			if (t) toplevel_set_state(t, 0, t->fullscreen ? 0 : 1);
		}
		else if (item == 2) {
			int off = 0;
			for (int i = 0; i < MAX_TOPLEVELS; i++)
				if (toplevels[i].used && toplevels[i].surface) {
					toplevels[i].surface->x = 48 + off * 32;
					toplevels[i].surface->y = PANEL_H + 24 + off * 24;
					off++;
				}
			composite_rect(0, PANEL_H, (int)scr_w, (int)scr_h - PANEL_H - DOCK_H);
		}
		else if (item == 3) {
			int count = 0;
			for (int i = 0; i < MAX_TOPLEVELS; i++)
				if (toplevels[i].used && toplevels[i].surface) count++;
			if (count > 0) {
				int cols = count <= 2 ? count : (count <= 4 ? 2 : 3);
				int rows = (count + cols - 1) / cols;
				int cw = (int)work_w() / cols;
				int ch = (int)work_h() / rows;
				int idx = 0;
				for (int i = 0; i < MAX_TOPLEVELS; i++)
					if (toplevels[i].used && toplevels[i].surface) {
						int r = idx / cols, c = idx % cols;
						toplevels[i].surface->x = c * cw;
						toplevels[i].surface->y = (int)PANEL_H + r * ch;
						idx++;
					}
				composite_rect(0, PANEL_H, (int)scr_w, (int)scr_h - PANEL_H - DOCK_H);
			}
		}
	}
	else if (menu == MENU_WINDOW) {
		struct dsurface *f = slot_surface(focus_slot);
		struct dtoplevel *t = f ? surface_toplevel(f) : 0;
		if (item == 0 && t) minimize_toplevel(t);
		else if (item == 1 && t) toplevel_set_state(t, t->maximized ? 0 : 1, 0);
		else if (item == 3) {
			for (int i = 0; i < MAX_TOPLEVELS; i++)
				if (toplevels[i].used && toplevels[i].surface && toplevels[i].minimized)
					restore_toplevel(&toplevels[i]);
			composite_rect(0, 0, (int)scr_w, (int)scr_h);
		}
		else if (item == 4) close_focused_window();
	}
	else if (menu == MENU_CLOCK) {
		if (item == 0) spawn_app("/bin/gclock", NULL);
		else if (item == 1) {
			clock_24h = !clock_24h;
			clock_last_min = -1;
			update_clock();
			composite_rect(0, 0, (int)scr_w, PANEL_H);
		} else if (item == 2) spawn_app("/bin/gabout", "date");
	}
	else if (menu == MENU_DESKTOP) {
		if (item == 0) spawn_app("/bin/gterm", NULL);
		else if (item == 1) spawn_app("/bin/gpaint", NULL);
		else if (item == 3) {
			int off = 0;
			for (int i = 0; i < MAX_TOPLEVELS; i++)
				if (toplevels[i].used && toplevels[i].surface) {
					toplevels[i].surface->x = 48 + off * 32;
					toplevels[i].surface->y = PANEL_H + 24 + off * 24;
					off++;
				}
			composite_rect(0, PANEL_H, (int)scr_w, (int)scr_h - PANEL_H - DOCK_H);
		}
		else if (item == 4) {
			int count = 0;
			for (int i = 0; i < MAX_TOPLEVELS; i++)
				if (toplevels[i].used && toplevels[i].surface) count++;
			if (count > 0) {
				int cols = count <= 2 ? count : (count <= 4 ? 2 : 3);
				int rows = (count + cols - 1) / cols;
				int cw = (int)work_w() / cols;
				int ch = (int)work_h() / rows;
				int idx = 0;
				for (int i = 0; i < MAX_TOPLEVELS; i++)
					if (toplevels[i].used && toplevels[i].surface) {
						int r = idx / cols, c = idx % cols;
						toplevels[i].surface->x = c * cw;
						toplevels[i].surface->y = (int)PANEL_H + r * ch;
						idx++;
					}
				composite_rect(0, PANEL_H, (int)scr_w, (int)scr_h - PANEL_H - DOCK_H);
			}
		}
		else if (item == 5) spawn_app("/bin/gabout", NULL);
	}
}
