/*
 * displayd_dispatch.c — Wayland message dispatch (handle_wayland_msg).
 */
#include "displayd.h"

/* ── wl_surface configure ── */
static void wl_surface_configure(int ci, struct wobject *xdg) {
	struct dsurface *surface = find_surface(ci, xdg->link);
	struct dtoplevel *t = surface ? surface_toplevel(surface) : 0;
	if (!t) return;
	uint32_t top[4] = {0, 0, 4, 4};
	send_msg(ci, t->id, 0, top, 4);
	uint32_t serial = ++frame_serial;
	send_msg(ci, xdg->id, 0, &serial, 1);
	xdg->configured = 1;
}

/* ── wl_create_buffer ── */
static void wl_create_buffer(int ci, struct wpool *pool, const uint32_t *a,
                             unsigned n) {
	if (n < 6 || (a[5] != 0 && a[5] != 1) || a[2] == 0 || a[3] == 0 ||
	    a[4] < a[2] * 4 ||
	    (uint64_t)a[1] + (uint64_t)a[4] * a[3] > pool->size)
		return;
	int slot = -1;
	for (int i = 0; i < MAX_BUFFERS; i++)
		if (!buffers[i].used) { slot = i; break; }
	if (slot < 0) return;
	size_t map_size = (size_t)a[1] + (size_t)a[4] * a[3];
	void *mem = mmap(0, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	                 pool->fd, 0);
	if (mem == MAP_FAILED) return;
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

/* ── surface action ── */
enum surface_action {
	SURFACE_ATTACH, SURFACE_DAMAGE, SURFACE_FRAME,
	SURFACE_COMMIT, SURFACE_DESTROY,
};

static void surface_action(int ci, struct dsurface *s, uint16_t op,
                           const uint32_t *a, unsigned n) {
	switch (op) {
	case SURFACE_ATTACH:
		if (n < 1) return;
		s->pend_buffer_id = a[0];
		s->pend_attach = 1;
		break;
	case SURFACE_DAMAGE: {
		if (n < 4) return;
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
		if (n < 1) return;
		s->frame_cb = a[0];
		s->has_frame_cb = 1;
		break;
	case SURFACE_COMMIT: {
		if (s->pend_attach) {
			struct dbuffer *nb = find_buffer(ci, s->pend_buffer_id);
			if (!nb) return;
			if (s->buf && s->buf != nb)
				send_msg(ci, s->buf->id, 0, 0, 0);
			s->buf = nb;
			s->pend_attach = 0;
			if (!s->mapped) {
				if (s->placement == 0) { s->x = 48; s->y = 92; }
				else if (s->placement == 1) {
					s->x = (int)scr_w - (int)nb->w - 48; s->y = 52;
				} else if (s->placement == 2) {
					s->x = ((int)scr_w - (int)nb->w) / 2;
					s->y = (int)scr_h - (int)nb->h - TITLE_H - 44;
				}
			}
		}
		if (!s->buf) return;
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
			if (old) composite_surface_region(old);
			composite_rect(0, 0, (int)scr_w, PANEL_H);
			composite_surface_region(s);
		} else if (s->pend_dmg_valid) {
			int dx = (int)s->dx0, dy = (int)s->dy0;
			int dw = (int)(s->dx1 - s->dx0), dh = (int)(s->dy1 - s->dy0);
			if (dw > 0 && dh > 0)
				composite_rect(s->x + dx, s->y + dy, dw, dh);
		}
		struct dtoplevel *gt = surface_toplevel(s);
		if (gt && gt->geom_dirty) {
			if (gt->fullscreen) { s->x = 0; s->y = 0; }
			else if (gt->maximized) { s->x = 0; s->y = (int)PANEL_H; }
			else if (gt->restoring) {
				s->x = gt->saved_x; s->y = gt->saved_y;
				gt->restoring = 0;
			}
			gt->geom_dirty = 0;
			composite_rect(0, 0, (int)scr_w, (int)scr_h);
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
	default: break;
	}
}

/* ── main dispatch ── */
void handle_wayland_msg(int ci, const struct wl_hdr *h,
                        const uint32_t *a, unsigned n) {
	/* wl_display */
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

	/* wl_surface */
	struct dsurface *surface = find_surface(ci, h->object_id);
	if (surface) {
		if (h->opcode == 0) surface_action(ci, surface, SURFACE_DESTROY, a, n);
		else if (h->opcode == 1 && n >= 1) surface_action(ci, surface, SURFACE_ATTACH, a, 1);
		else if (h->opcode == 2 || h->opcode == 9) surface_action(ci, surface, SURFACE_DAMAGE, a, n);
		else if (h->opcode == 3) surface_action(ci, surface, SURFACE_FRAME, a, n);
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

	/* wl_buffer */
	struct dbuffer *buffer = find_buffer(ci, h->object_id);
	if (buffer) {
		if (h->opcode == 0) buffer_destroy(buffer);
		return;
	}

	/* xdg_toplevel */
	struct dtoplevel *top = find_toplevel(ci, h->object_id);
	if (top) {
		if (h->opcode == 0) memset(top, 0, sizeof(*top));
		else if (h->opcode == 2 && n >= 1) { /* set_title */
			unsigned len = a[0];
			if (len > sizeof(top->title)) len = sizeof(top->title);
			if (len) memcpy(top->title, &a[1], len - 1);
			top->title[len ? len - 1 : 0] = 0;
		} else if (h->opcode == 3 && n >= 1) { /* set_app_id */
			unsigned len = a[0];
			if (len > sizeof(top->app_id)) len = sizeof(top->app_id);
			if (len) memcpy(top->app_id, &a[1], len - 1);
			top->app_id[len ? len - 1 : 0] = 0;
		} else if (h->opcode == 5 && top->surface) { /* move */
			drag_slot = (int)(top->surface - surfaces);
		} else if (h->opcode == 6 && n >= 3 && top->surface && top->surface->buf) {
			resize_slot = (int)(top->surface - surfaces);
			resize_edges = a[2];
			resize_ox = px; resize_oy = py;
			resize_ow = (int)top->surface->buf->w;
			resize_oh = (int)top->surface->buf->h;
			resize_wx = top->surface->x;
			resize_wy = top->surface->y;
		} else if (h->opcode == 9) toplevel_set_state(top, 1, 0);
		else if (h->opcode == 10) toplevel_set_state(top, 0, 0);
		else if (h->opcode == 11) toplevel_set_state(top, 0, 1);
		else if (h->opcode == 12) toplevel_set_state(top, 0, 0);
		else if (h->opcode == 13) minimize_toplevel(top);
		/* opcode 20: app menu registration (custom extension) */
		else if (h->opcode == 20 && n >= 1) {
			int tslot = (int)(top - toplevels);
			int count = (int)a[0];
			if (count > APP_MENU_MAX_ITEMS) count = APP_MENU_MAX_ITEMS;
			app_menus[tslot].count = count;
			app_menus[tslot].toplevel_slot = tslot;
			unsigned pos = 1;
			for (int i = 0; i < count && pos < n; i++) {
				app_menus[tslot].items[i].id = (uint16_t)(a[pos] >> 16);
				app_menus[tslot].items[i].flags = (uint16_t)(a[pos] & 0xFFFF);
				pos++;
				if (pos < n) {
					unsigned llen = a[pos++];
					if (llen > APP_MENU_LABEL_MAX - 1) llen = APP_MENU_LABEL_MAX - 1;
					unsigned lwords = (llen + 3) / 4;
					if (pos + lwords <= n) {
						memcpy(app_menus[tslot].items[i].label, &a[pos], llen);
						app_menus[tslot].items[i].label[llen] = 0;
						pos += lwords;
					}
				}
				if (pos < n) {
					unsigned alen = a[pos++];
					if (alen > APP_MENU_ACCEL_MAX - 1) alen = APP_MENU_ACCEL_MAX - 1;
					unsigned awords = (alen + 3) / 4;
					if (pos + awords <= n) {
						memcpy(app_menus[tslot].items[i].accel, &a[pos], alen);
						app_menus[tslot].items[i].accel[alen] = 0;
						pos += awords;
					}
				}
			}
			composite_rect(0, 0, (int)scr_w, PANEL_H);
		}
		return;
	}

	/* wl_shm_pool */
	struct wpool *pool = wpool_find(ci, h->object_id);
	if (pool) {
		if (h->opcode == 0) wl_create_buffer(ci, pool, a, n);
		else if (h->opcode == 1) {
			close(pool->fd);
			memset(pool, 0, sizeof(*pool));
			wl_delete_id(ci, h->object_id);
		} else if (h->opcode == 2 && n >= 1 && a[0] > pool->size)
			pool->size = a[0];
		return;
	}

	/* all other objects */
	struct wobject *obj = wobject_find(ci, h->object_id);
	if (!obj) return;

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
			else if (a[0] == 7) type = WOBJ_DECORATION_MANAGER;
			else if (a[0] == 8) type = WOBJ_VIEWPORTER;
			else if (a[0] == 9) type = WOBJ_SUBCOMPOSITOR;
			else if (a[0] == 10) type = WOBJ_PRESENTATION;
			else if (a[0] == 11) type = WOBJ_DMABUF;
			else break;
			if (wobject_add(ci, new_id, type, 0)) {
				if (type == WOBJ_SHM) {
					uint32_t format = 0;
					send_msg(ci, new_id, 0, &format, 1);
					format = 1;
					send_msg(ci, new_id, 0, &format, 1);
				} else if (type == WOBJ_SEAT) {
					uint32_t capabilities = 7;
					send_msg(ci, new_id, 0, &capabilities, 1);
					wl_send_string(ci, new_id, 1, 0, 0, "b1nix", 0, 0);
				} else if (type == WOBJ_OUTPUT) {
					wl_send_output_events(ci, new_id);
				} else if (type == WOBJ_PRESENTATION) {
					uint32_t clk = 1;
					send_msg(ci, new_id, 0, &clk, 1);
				} else if (type == WOBJ_DMABUF) {
					uint32_t fmt = 0x34325241;
					send_msg(ci, new_id, 0, &fmt, 1);
					fmt = 0x34325258;
					send_msg(ci, new_id, 0, &fmt, 1);
				}
			}
		}
		break;
	case WOBJ_COMPOSITOR:
		if (h->opcode == 0 && n >= 1) create_surface(ci, a[0]);
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
		if (h->opcode == 0) wobject_remove(obj);
		else if (h->opcode == 2 && n >= 2)
			wobject_add(ci, a[0], WOBJ_XDG_SURFACE, a[1]);
		else if (h->opcode == 3 && n >= 1) {
			if (clients[ci].ping_serial == a[0]) clients[ci].ping_pending = 0;
		}
		break;
	case WOBJ_XDG_SURFACE:
		if (h->opcode == 0) wobject_remove(obj);
		else if (h->opcode == 1 && n >= 1) {
			create_toplevel(ci, a[0], obj->link);
			struct dtoplevel *created = find_toplevel(ci, a[0]);
			if (created) strcpy(created->title, "Wayland");
		} else if (h->opcode == 4) obj->configured = 1;
		break;
	case WOBJ_REGION:
		if (h->opcode == 0) wobject_remove(obj);
		break;
	case WOBJ_SEAT:
		if (h->opcode == 0 && n >= 1)
			wobject_add(ci, a[0], WOBJ_POINTER, 0);
		else if (h->opcode == 1 && n >= 1) {
			if (wobject_add(ci, a[0], WOBJ_KEYBOARD, 0))
				keyboard_init(ci, a[0]);
		}
		else if (h->opcode == 2 && n >= 1)
			wobject_add(ci, a[0], WOBJ_TOUCH, 0);
		else if (h->opcode == 3) wobject_remove(obj);
		break;
	case WOBJ_POINTER:
	case WOBJ_KEYBOARD:
		if (h->opcode == 0) wobject_remove(obj);
		break;
	case WOBJ_DDM:
		if (h->opcode == 0 && n >= 1)
			wobject_add(ci, a[0], WOBJ_DATA_SOURCE, 0);
		else if (h->opcode == 1 && n >= 1) {
			if (wobject_add(ci, a[0], WOBJ_DATA_DEVICE, 0))
				clipboard_offer_to(ci, a[0]);
		}
		break;
	case WOBJ_DATA_SOURCE:
		if (h->opcode == 0 && n >= 1) {
			strncpy(obj->mime, (const char *)&a[1], sizeof(obj->mime) - 1);
			obj->mime[sizeof(obj->mime) - 1] = 0;
		} else if (h->opcode == 1) wobject_remove(obj);
		else if (h->opcode == 2 && n >= 1) obj->link = a[0];
		break;
	case WOBJ_DATA_DEVICE:
		if (h->opcode == 0 && n >= 1) {
			struct wobject *src = a[0] ? wobject_find(ci, a[0]) : 0;
			if (src && src->type == WOBJ_DATA_SOURCE) {
				dnd_active = 1;
				dnd_src_client = ci;
				dnd_source = a[0];
				dnd_src_actions = src->link;
				strncpy(dnd_mime, src->mime, sizeof(dnd_mime) - 1);
				dnd_mime[sizeof(dnd_mime) - 1] = 0;
				dnd_target_client = -1;
				dnd_target_offer = 0;
				dnd_accepted_action = 0;
				dnd_in_surface = -1;
				dnd_motion();
			}
		} else if (h->opcode == 1 && n >= 1) {
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
		} else if (h->opcode == 2) wobject_remove(obj);
		break;
	case WOBJ_DATA_OFFER:
		if (h->opcode == 0 && obj->link == 1 && n >= 1) { /* accept */ }
		else if (h->opcode == 1) { /* receive */
			if (obj->link == 1 && dnd_src_client >= 0 &&
			    clients[ci].pending_fd >= 0) {
				uint32_t buf[20];
				unsigned k = wl_pack_string(buf, 0, dnd_mime);
				send_msg_fd(dnd_src_client, dnd_source, 1, buf, k,
				            clients[ci].pending_fd);
			} else if (sel_client >= 0 && clients[ci].pending_fd >= 0) {
				uint32_t buf[20];
				unsigned k = wl_pack_string(buf, 0, sel_mime);
				send_msg_fd(sel_client, sel_source, 1, buf, k,
				            clients[ci].pending_fd);
			}
			if (clients[ci].pending_fd >= 0) {
				close(clients[ci].pending_fd);
				clients[ci].pending_fd = -1;
			}
		} else if (h->opcode == 2) wobject_remove(obj);
		else if (h->opcode == 3) {
			if (obj->link == 1 && dnd_src_client >= 0)
				send_msg(dnd_src_client, dnd_source, 4, 0, 0);
		} else if (h->opcode == 4 && obj->link == 1 && n >= 1) {
			dnd_accepted_action = a[n >= 2 ? 1 : 0];
			uint32_t act = dnd_accepted_action;
			send_msg(ci, obj->id, 2, &act, 1);
		}
		break;
	case WOBJ_DECORATION_MANAGER:
		if (h->opcode == 0) wobject_remove(obj);
		else if (h->opcode == 1 && n >= 1) {
			if (wobject_add(ci, a[0], WOBJ_TOPLEVEL_DECORATION, 0)) {
				uint32_t mode = 2;
				send_msg(ci, a[0], 0, &mode, 1);
			}
		}
		break;
	case WOBJ_TOPLEVEL_DECORATION:
		if (h->opcode == 0) wobject_remove(obj);
		else if (h->opcode == 1 || h->opcode == 2) {
			uint32_t mode = 2;
			send_msg(ci, obj->id, 0, &mode, 1);
		}
		break;
	case WOBJ_VIEWPORTER:
		if (h->opcode == 0) wobject_remove(obj);
		else if (h->opcode == 1 && n >= 2)
			wobject_add(ci, a[0], WOBJ_VIEWPORT, a[1]);
		break;
	case WOBJ_VIEWPORT:
		if (h->opcode == 0) wobject_remove(obj);
		else if (h->opcode == 1) { /* set_source: ignored */ }
		else if (h->opcode == 2 && n >= 2) {
			struct dsurface *s = find_surface(ci, obj->link);
			if (s) {
				s->vp_dst_w = (int)a[0] < 0 ? 0 : (int)a[0];
				s->vp_dst_h = (int)a[1] < 0 ? 0 : (int)a[1];
			}
		}
		break;
	case WOBJ_SUBCOMPOSITOR:
		if (h->opcode == 0) wobject_remove(obj);
		else if (h->opcode == 1 && n >= 3) {
			struct dsurface *child = find_surface(ci, a[1]);
			struct dsurface *parent = find_surface(ci, a[2]);
			if (child && parent && child != parent &&
			    wobject_add(ci, a[0], WOBJ_SUBSURFACE, a[1])) {
				child->parent = parent;
				child->sub_x = 0; child->sub_y = 0;
				zorder_remove((int)(child - surfaces));
			}
		}
		break;
	case WOBJ_SUBSURFACE:
		if (h->opcode == 0) {
			struct dsurface *child = find_surface(ci, obj->link);
			if (child) child->parent = 0;
			wobject_remove(obj);
		} else if (h->opcode == 1 && n >= 2) {
			struct dsurface *child = find_surface(ci, obj->link);
			if (child) { child->sub_x = (int)a[0]; child->sub_y = (int)a[1]; }
		}
		break;
	case WOBJ_PRESENTATION:
		if (h->opcode == 0) wobject_remove(obj);
		else if (h->opcode == 1 && n >= 2) {
			if (wobject_add(ci, a[1], WOBJ_PRESENTATION_FEEDBACK, a[0])) {
				struct timespec ts;
				clock_gettime(CLOCK_MONOTONIC, &ts);
				uint32_t fb[7];
				fb[0] = (uint32_t)((uint64_t)ts.tv_sec >> 32);
				fb[1] = (uint32_t)ts.tv_sec;
				fb[2] = (uint32_t)ts.tv_nsec;
				fb[3] = 16666666;
				fb[4] = 0; fb[5] = frame_serial; fb[6] = 0x9;
				send_msg(ci, a[1], 1, fb, 7);
				wobject_remove(wobject_find(ci, a[1]));
			}
		}
		break;
	case WOBJ_PRESENTATION_FEEDBACK:
		break;
	case WOBJ_DMABUF:
		if (h->opcode == 0) wobject_remove(obj);
		else if (h->opcode == 1 && n >= 1)
			wobject_add(ci, a[0], WOBJ_DMABUF_PARAMS, 0);
		break;
	case WOBJ_DMABUF_PARAMS:
		if (h->opcode == 0) wobject_remove(obj);
		else if (h->opcode == 1) {
			if (clients[ci].pending_fd >= 0) {
				close(clients[ci].pending_fd);
				clients[ci].pending_fd = -1;
			}
		} else if (h->opcode == 2 || h->opcode == 3) {
			if (h->opcode == 2) send_msg(ci, obj->id, 1, 0, 0);
		}
		break;
	case WOBJ_TOUCH:
		if (h->opcode == 0) wobject_remove(obj);
		break;
	default: break;
	}
}
