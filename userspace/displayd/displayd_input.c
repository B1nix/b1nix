/*
 * displayd_input.c — keyboard, mouse, touch input handling.
 */
#include "displayd.h"
#include "xkb_keymap_us.h"

/* ── modifier tracking ── */
static int update_mods(uint16_t code, int value) {
	uint32_t bit;
	switch (code) {
	case 0x2a: case 0x36: bit = 1; break;   /* L/R Shift   */
	case 0x1d: case 0x61: bit = 4; break;   /* L/R Control */
	case 0x38: case 0x64: bit = 8; break;   /* L/R Alt (Mod1) */
	case 0x7d: case 0x7e: bit = 64; break;  /* L/R Meta (Mod4) */
	case 0x3a:
		if (value == 1) { kbd_mods_locked ^= 2; return 1; }
		return 0;
	default: return 0;
	}
	uint32_t before = kbd_mods_depressed;
	if (value == 0) kbd_mods_depressed &= ~bit;
	else kbd_mods_depressed |= bit;
	return kbd_mods_depressed != before;
}

/* ── keyboard input ── */
void keyboard_init(int client, uint32_t id) {
	size_t size = sizeof(xkb_keymap_us);
	int fd = memfd_create("wayland-keymap", MFD_CLOEXEC);
	if (fd >= 0) {
		size_t off = 0;
		while (off < size) {
			ssize_t w = write(fd, xkb_keymap_us + off, size - off);
			if (w <= 0) break;
			off += (size_t)w;
		}
		if (off == size) {
			uint32_t keymap[2] = {1, (uint32_t)size};
			send_msg_fd(client, id, 0, keymap, 2, fd);
		} else {
			uint32_t keymap[2] = {0, 0};
			send_msg_fd(client, id, 0, keymap, 2, fd);
		}
		close(fd);
	}
	uint32_t repeat[2] = {25, 400};
	send_msg(client, id, 5, repeat, 2);
	send_kbd_modifiers(client, id);
}

/* ── input drain (keyboard + mouse) ── */
void input_drain(int which) {
	struct b1nix_input_event evs[16];
	for (;;) {
		ssize_t n = read(ev_fds[which], evs, sizeof(evs));
		if (n <= 0) break;
		int count = (int)(n / (ssize_t)sizeof(evs[0]));
		for (int i = 0; i < count; i++) {
			struct b1nix_input_event *e = &evs[i];
			if (which == 0) { /* keyboard */
				if (e->type == B1NIX_EV_KEY) {
					input_serial++;
					if (update_mods((uint16_t)e->code, (int)e->value))
						send_focus_modifiers();
					if (e->value && e->code == 0x01 && open_menu != MENU_NONE) {
						close_panel_menu();
						continue;
					}
					if (e->code == 0x38) { left_alt = e->value != 0; continue; }
					if (left_alt && e->value && e->code == 0x0f) {
						focus_cycle(); continue;
					}
					struct dsurface *f = slot_surface(focus_slot);
					if (f) {
						if (left_alt && e->value && e->code == 0x3e) {
							struct dtoplevel *t = surface_toplevel(f);
							if (t) send_msg(t->client, t->id, 1, 0, 0);
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
				if (e->code == B1NIX_REL_X) ptr_acc_dx += e->value;
				if (e->code == B1NIX_REL_Y) ptr_acc_dy += e->value;
				ptr_moved = 1;
				break;
			case B1NIX_EV_ABS:
				if (e->code == B1NIX_ABS_X) ptr_abs_x = e->value;
				if (e->code == B1NIX_ABS_Y) ptr_abs_y = e->value;
				ptr_have_abs = 1;
				ptr_moved = 1;
				break;
			case B1NIX_EV_KEY:
				apply_pointer_motion();
				pointer_button(e->code, e->value);
				break;
			default: break;
			}
		}
	}
	apply_pointer_motion();
}

/* ── touch input ── */
static void send_touch(int client, uint16_t opcode, const uint32_t *words,
                       unsigned nwords) {
	struct wobject *t = wobject_type_find(client, WOBJ_TOUCH);
	if (t) send_msg(client, t->id, opcode, words, nwords);
}

void touch_drain(void) {
	struct b1nix_input_event evs[16];
	int pend_x = tch_x, pend_y = tch_y, have_xy = 0;
	int pend_down = -1;
	for (;;) {
		ssize_t n = read(ev_fds[2], evs, sizeof(evs));
		if (n <= 0) break;
		int count = (int)(n / (ssize_t)sizeof(evs[0]));
		for (int i = 0; i < count; i++) {
			struct b1nix_input_event *e = &evs[i];
			switch (e->type) {
			case B1NIX_EV_ABS:
				if (e->code == B1NIX_ABS_X) { pend_x = e->value; have_xy = 1; }
				if (e->code == B1NIX_ABS_Y) { pend_y = e->value; have_xy = 1; }
				break;
			case B1NIX_EV_KEY:
				if (e->code == B1NIX_BTN_TOUCH) pend_down = e->value ? 1 : 0;
				break;
			case B1NIX_EV_SYN: {
				if (have_xy) { tch_x = pend_x; tch_y = pend_y; }
				if (tch_x < 0) tch_x = 0;
				if (tch_y < 0) tch_y = 0;
				if (tch_x >= (int)scr_w) tch_x = (int)scr_w - 1;
				if (tch_y >= (int)scr_h) tch_y = (int)scr_h - 1;
				input_serial++;
				frame_serial++;
				if (pend_down == 1 && !tch_down) {
					int slot = surface_at(tch_x, tch_y);
					struct dsurface *s = slot_surface(slot);
					if (s) {
						tch_down = 1;
						tch_client = s->client;
						tch_surface_id = s->id;
						uint32_t d[6] = {input_serial, frame_serial, s->id, 0,
						                 (uint32_t)((tch_x - s->x) << 8),
						                 (uint32_t)((tch_y - s->y) << 8)};
						send_touch(tch_client, 0, d, 6);
						send_touch(tch_client, 3, 0, 0);
					}
				} else if (pend_down == 0 && tch_down) {
					uint32_t u[3] = {input_serial, frame_serial, 0};
					send_touch(tch_client, 1, u, 3);
					send_touch(tch_client, 3, 0, 0);
					tch_down = 0;
					tch_client = -1;
				} else if (tch_down && have_xy) {
					struct dsurface *s = slot_surface(surface_at(tch_x, tch_y));
					int ox = 0, oy = 0;
					if (s && s->client == tch_client && s->id == tch_surface_id) {
						ox = s->x; oy = s->y;
					}
					uint32_t m[4] = {frame_serial, 0,
					                 (uint32_t)((tch_x - ox) << 8),
					                 (uint32_t)((tch_y - oy) << 8)};
					send_touch(tch_client, 2, m, 4);
					send_touch(tch_client, 3, 0, 0);
				}
				pend_down = -1;
				have_xy = 0;
				break;
			}
			default: break;
			}
		}
	}
}

/* ── pointer button ── */
void pointer_button(uint16_t code, int state) {
	input_serial++;
	int on_decoration = 0;
	if (code == B1NIX_BTN_LEFT) {
		if (state) {
			enum panel_menu header = py < PANEL_H ? panel_menu_at(px) : MENU_NONE;
			if (header != MENU_NONE) {
				btn_on_panel = 1;
				if (open_menu == header) close_panel_menu();
				else open_panel_menu(header);
				return;
			}
			/* Taskbar button */
			if (py < PANEL_H) {
				int tb = taskbar_button_at(px, py);
				if (tb >= 0) {
					btn_on_panel = 1;
					close_panel_menu();
					int tslot = taskbar_slot(tb);
					if (tslot >= 0) {
						struct dtoplevel *t = &toplevels[tslot];
						if (t->minimized) restore_toplevel(t);
						else if (t->surface) {
							int sl = (int)(t->surface - surfaces);
							if (sl != focus_slot) {
								struct dsurface *old = slot_surface(focus_slot);
								if (old) {
									uint32_t lv[1] = {old->id};
									send_seat_event(old->client, SEAT_FOCUS_LEAVE, lv, 1);
								}
								focus_slot = sl;
								uint32_t en[1] = {t->surface->id};
								send_seat_event(t->surface->client, SEAT_FOCUS_ENTER, en, 1);
							}
							zorder_raise(sl);
							composite_rect(0, 0, (int)scr_w, (int)scr_h);
						}
					}
					return;
				}
			}
			/* Menu item click */
			if (open_menu != MENU_NONE) {
				enum panel_menu menu = open_menu;
				int item = menu_item_at(px, py);
				btn_on_panel = 1;
				if (item >= 0) activate_menu_item(menu, item);
				else close_panel_menu();
				return;
			}
			/* Dock click */
			if (py >= (int)scr_h - DOCK_H) {
				close_panel_menu();
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
						if (px >= ix && px < ix + DOCK_ICON_W) {
							if (t->minimized) restore_toplevel(t);
							else if (t->surface) {
								int sl = (int)(t->surface - surfaces);
								if (sl != focus_slot) {
									struct dsurface *old = slot_surface(focus_slot);
									if (old) {
										uint32_t lv[1] = {old->id};
										send_seat_event(old->client, SEAT_FOCUS_LEAVE, lv, 1);
									}
									focus_slot = sl;
									uint32_t en[1] = {t->surface->id};
									send_seat_event(t->surface->client, SEAT_FOCUS_ENTER, en, 1);
								}
								zorder_raise(sl);
							}
							composite_rect(0, 0, (int)scr_w, (int)scr_h);
							return;
						}
						di++;
					}
				}
				return;
			}
		} else if (btn_on_panel) {
			btn_on_panel = 0;
			return;
		}
	}
	if (state) {
		/* Right-click on desktop background */
		if (code == B1NIX_BTN_RIGHT && py >= PANEL_H && py < (int)scr_h - DOCK_H) {
			int slot = surface_at(px, py);
			if (slot < 0) {
				close_panel_menu();
				open_menu = MENU_DESKTOP;
				desktop_menu_x = px;
				desktop_menu_y = py;
				menu_hover = -1;
				composite_rect(0, 0, (int)scr_w, (int)scr_h);
				return;
			}
		}
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
			if (old) composite_surface_region(old);
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
		if (code == B1NIX_BTN_LEFT) btn_on_decoration = on_decoration;
	} else {
		if (dnd_active) {
			dnd_finish_drag();
			if (code == B1NIX_BTN_LEFT) btn_on_decoration = 0;
			return;
		}
		if (code == B1NIX_BTN_LEFT) {
			drag_slot = -1;
			resize_slot = -1;
			on_decoration = btn_on_decoration;
			btn_on_decoration = 0;
		}
	}
	if (on_decoration) return;
	struct dsurface *f = slot_surface(focus_slot);
	if (f) {
		uint32_t w[2] = {code, (uint32_t)state};
		send_seat_event(f->client, SEAT_POINTER_BUTTON, w, 2);
	}
}

/* ── pointer motion ── */
static void pointer_moved_inner(void) {
	if (open_menu != MENU_NONE) {
		if (open_menu != MENU_DESKTOP) {
			enum panel_menu header = py < PANEL_H ? panel_menu_at(px) : MENU_NONE;
			if (header != MENU_NONE && header != open_menu) {
				open_panel_menu(header);
				return;
			}
		}
		int hover = menu_item_at(px, py);
		if (hover != menu_hover) {
			menu_hover = hover;
			int my = (open_menu == MENU_DESKTOP) ? desktop_menu_y : PANEL_H;
			composite_rect(menu_x(open_menu) - 2, my,
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

void pointer_moved(void) { pointer_moved_inner(); }

void apply_pointer_motion(void) {
	if (!ptr_moved) return;
	int ox = px, oy = py;
	if (ptr_have_abs) {
		px = ptr_abs_x; py = ptr_abs_y;
		ptr_have_abs = 0;
	} else {
		px += ptr_acc_dx; py += ptr_acc_dy;
	}
	ptr_acc_dx = ptr_acc_dy = 0;
	ptr_moved = 0;
	if (px < 0) px = 0;
	if (py < 0) py = 0;
	if (px >= (int)scr_w) px = (int)scr_w - 1;
	if (py >= (int)scr_h) py = (int)scr_h - 1;
	if (px == ox && py == oy) return;

	/* interactive resize */
	struct dsurface *rz = slot_surface(resize_slot);
	if (rz) {
		struct dtoplevel *t = surface_toplevel(rz);
		int dx = px - resize_ox, dy = py - resize_oy;
		int nw = resize_ow, nh = resize_oh;
		if (resize_edges & 8) nw = resize_ow + dx;
		else if (resize_edges & 4) nw = resize_ow - dx;
		if (resize_edges & 2) nh = resize_oh + dy;
		else if (resize_edges & 1) nh = resize_oh - dy;
		if (nw < 64) nw = 64;
		if (nh < 48) nh = 48;
		if (resize_edges & 4) rz->x = (resize_wx + resize_ow) - nw;
		if (resize_edges & 1) rz->y = (resize_wy + resize_oh) - nh;
		if (t) {
			uint32_t st[2] = {3, 4};
			send_state_configure(t, (uint32_t)nw, (uint32_t)nh, st, 2);
		}
		composite_rect(ox, oy, CURSOR_W + 1, CURSOR_H + 1);
		composite_rect(px, py, CURSOR_W + 1, CURSOR_H + 1);
		pointer_moved_inner();
		return;
	}

	struct dsurface *drag = slot_surface(drag_slot);
	if (drag) {
		int ox_pos = drag->x, oy_pos = drag->y;
		drag->x += px - ox;
		drag->y += py - oy;
		int dw = drag->buf ? (int)drag->buf->w : 0;
		int dh = drag->buf ? (int)drag->buf->h : 0;
		int min_y = PANEL_H + TITLE_H;
		if (drag->y < min_y) drag->y = min_y;
		if (drag->y > (int)scr_h - DOCK_H - 24) drag->y = (int)scr_h - DOCK_H - 24;
		if (drag->x < -(dw - 48)) drag->x = -(dw - 48);
		if (drag->x > (int)scr_w - 48) drag->x = (int)scr_w - 48;

		int top = 1;
		for (int i = 0; i < MAX_TOPLEVELS; i++)
			if (toplevels[i].used && toplevels[i].surface == drag) top = TITLE_H;

		int overlap = !(drag->x >= ox_pos + dw || drag->x + dw <= ox_pos ||
		                drag->y >= oy_pos + dh || drag->y + dh <= oy_pos);
		if (overlap) {
			int x0 = ox_pos < drag->x ? ox_pos : drag->x;
			int y0 = (oy_pos < drag->y ? oy_pos : drag->y) - top;
			int x1 = ox_pos + dw > drag->x + dw ? ox_pos + dw : drag->x + dw;
			int y1 = oy_pos + dh > drag->y + dh ? oy_pos + dh : drag->y + dh;
			composite_rect(x0 - 1, y0, (x1 - x0) + 2, (y1 - y0) + top + 1);
		} else {
			composite_rect(ox_pos - 1, oy_pos - top, dw + 2, dh + top + 1);
			composite_surface_region(drag);
		}
		composite_rect(ox, oy, CURSOR_W + 1, CURSOR_H + 1);
		composite_rect(px, py, CURSOR_W + 1, CURSOR_H + 1);
	} else {
		composite_rect(ox, oy, CURSOR_W + 1, CURSOR_H + 1);
		composite_rect(px, py, CURSOR_W + 1, CURSOR_H + 1);
	}
	if (dnd_active) dnd_motion();
	pointer_moved_inner();
}
