/*
 * displayd_protocol.c — Wayland wire format, registry, message dispatch.
 */
#include "displayd.h"

/* ── wire helpers ── */
void send_msg(int client, uint32_t obj, uint16_t opcode,
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

void send_msg_fd(int client, uint32_t obj, uint16_t opcode,
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

void wl_delete_id(int client, uint32_t id) {
	send_msg(client, 1, 1, &id, 1);
}

void send_kbd_modifiers(int client, uint32_t id) {
	uint32_t ev[5] = {input_serial, kbd_mods_depressed, 0, kbd_mods_locked, 0};
	send_msg(client, id, 4, ev, 5);
}

void send_seat_event(int client, uint16_t opcode, const uint32_t *words,
                     unsigned nwords) {
	struct wobject *obj = 0;
	uint32_t event[5];
	unsigned count = 0;
	uint16_t wl_opcode = 0;
	if (opcode <= SEAT_POINTER_BUTTON) {
		obj = wobject_type_find(client, WOBJ_POINTER);
		if (!obj) return;
		if (opcode == SEAT_POINTER_ENTER && nwords >= 3) {
			event[0] = input_serial; event[1] = words[0];
			event[2] = words[1] << 8; event[3] = words[2] << 8;
			count = 4;
		} else if (opcode == SEAT_POINTER_LEAVE && nwords >= 1) {
			wl_opcode = 1; event[0] = input_serial; event[1] = words[0]; count = 2;
		} else if (opcode == SEAT_POINTER_MOTION && nwords >= 2) {
			wl_opcode = 2; event[0] = frame_serial;
			event[1] = words[0] << 8; event[2] = words[1] << 8; count = 3;
		} else if (opcode == SEAT_POINTER_BUTTON && nwords >= 2) {
			wl_opcode = 3; event[0] = input_serial; event[1] = frame_serial;
			event[2] = words[0]; event[3] = words[1]; count = 4;
		}
	} else {
		obj = wobject_type_find(client, WOBJ_KEYBOARD);
		if (!obj) return;
		if (opcode == SEAT_KEY && nwords >= 2) {
			wl_opcode = 3; event[0] = input_serial; event[1] = frame_serial;
			event[2] = words[0]; event[3] = words[1]; count = 4;
		} else if (opcode == SEAT_FOCUS_ENTER && nwords >= 1) {
			wl_opcode = 1; event[0] = input_serial; event[1] = words[0];
			event[2] = 0; count = 3;
		} else if (opcode == SEAT_FOCUS_LEAVE && nwords >= 1) {
			wl_opcode = 2; event[0] = input_serial; event[1] = words[0]; count = 2;
		}
	}
	if (obj && count)
		send_msg(client, obj->id, wl_opcode, event, count);
}

void wl_send_string(int client, uint32_t obj, uint16_t opcode,
                    const uint32_t *prefix, unsigned nprefix,
                    const char *text, const uint32_t *suffix,
                    unsigned nsuffix) {
	uint32_t words[32];
	unsigned len = (unsigned)strlen(text) + 1;
	unsigned ntext = (len + 3) / 4;
	if (nprefix + 1 + ntext + nsuffix > 32) return;
	memcpy(words, prefix, nprefix * 4);
	words[nprefix] = len;
	memset(&words[nprefix + 1], 0, ntext * 4);
	memcpy(&words[nprefix + 1], text, len);
	memcpy(&words[nprefix + 1 + ntext], suffix, nsuffix * 4);
	send_msg(client, obj, opcode, words, nprefix + 1 + ntext + nsuffix);
}

unsigned wl_pack_string(uint32_t *w, unsigned i, const char *s) {
	unsigned len = (unsigned)strlen(s) + 1;
	w[i++] = len;
	unsigned words = (len + 3) / 4;
	memset(&w[i], 0, words * 4);
	memcpy(&w[i], s, len - 1);
	return i + words;
}

void wl_send_output_events(int ci, uint32_t id) {
	uint32_t geo[32];
	unsigned k = 0;
	geo[k++] = 0; geo[k++] = 0; geo[k++] = 270; geo[k++] = 203;
	geo[k++] = 0;
	k = wl_pack_string(geo, k, "b1nix");
	k = wl_pack_string(geo, k, "b1nix-display");
	geo[k++] = 0;
	send_msg(ci, id, 0, geo, k);
	uint32_t mode[4] = {0x3, 1024, 768, 60000};
	send_msg(ci, id, 1, mode, 4);
	uint32_t scale = 1;
	send_msg(ci, id, 3, &scale, 1);
	send_msg(ci, id, 2, 0, 0);
}

void wl_registry_globals(int ci, uint32_t registry) {
	static const struct {
		uint32_t name;
		const char *interface;
		uint32_t version;
	} globals[] = {
	    {1, "wl_compositor", 4}, {2, "wl_shm", 1}, {3, "wl_seat", 5},
	    {4, "xdg_wm_base", 1}, {5, "wl_output", 2},
	    {6, "wl_data_device_manager", 3}, {7, "zxdg_decoration_manager_v1", 1},
	    {8, "wp_viewporter", 1}, {9, "wl_subcompositor", 1},
	    {10, "wp_presentation", 1}, {11, "zwp_linux_dmabuf_v1", 3},
	};
	for (unsigned i = 0; i < sizeof(globals) / sizeof(globals[0]); i++) {
		uint32_t prefix = globals[i].name;
		uint32_t suffix = globals[i].version;
		wl_send_string(ci, registry, 0, &prefix, 1, globals[i].interface,
		               &suffix, 1);
	}
}

/* ── wobject / wpool helpers ── */
struct wobject *wobject_find(int client, uint32_t id) {
	for (int i = 0; i < MAX_WOBJECTS; i++)
		if (wobjects[i].used && wobjects[i].client == client &&
		    wobjects[i].id == id)
			return &wobjects[i];
	return 0;
}

struct wobject *wobject_add(int client, uint32_t id,
                            enum wobject_type type, uint32_t link) {
	if (id < 2 || wobject_find(client, id)) return 0;
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

void wobject_remove(struct wobject *obj) {
	uint32_t id = obj->id;
	int client = obj->client;
	memset(obj, 0, sizeof(*obj));
	wl_delete_id(client, id);
}

struct wobject *wobject_type_find(int client, enum wobject_type type) {
	for (int i = 0; i < MAX_WOBJECTS; i++)
		if (wobjects[i].used && wobjects[i].client == client &&
		    wobjects[i].type == type)
			return &wobjects[i];
	return 0;
}

struct wpool *wpool_find(int client, uint32_t id) {
	for (int i = 0; i < MAX_WPOOLS; i++)
		if (wpools[i].used && wpools[i].client == client &&
		    wpools[i].id == id)
			return &wpools[i];
	return 0;
}

/* ── client connect/disconnect ── */
void selection_client_gone(int ci) {
	if (sel_client == ci) {
		sel_client = -1; sel_source = 0; sel_mime[0] = 0;
	}
	if (dnd_active && dnd_src_client == ci) {
		if (dnd_target_client >= 0 && dnd_target_client != ci) {
			struct wobject *dev = dnd_target_device(dnd_target_client);
			if (dev) send_msg(dnd_target_client, dev->id, 2, 0, 0);
		}
		dnd_active = 0; dnd_src_client = -1; dnd_source = 0;
		dnd_src_actions = 0; dnd_target_client = -1;
		dnd_target_offer = 0; dnd_in_surface = -1;
	} else if (dnd_target_client == ci) {
		dnd_target_client = -1; dnd_target_offer = 0; dnd_in_surface = -1;
	}
}

void client_disconnect(int ci) {
	if (!clients[ci].used) return;
	selection_client_gone(ci);
	for (int i = 0; i < MAX_SURFACES; i++)
		if (surfaces[i].used && surfaces[i].client == ci)
			surface_destroy(&surfaces[i]);
	for (int i = 0; i < MAX_BUFFERS; i++)
		if (buffers[i].used && buffers[i].client == ci)
			buffer_destroy(&buffers[i]);
	for (int i = 0; i < MAX_TOPLEVELS; i++)
		if (toplevels[i].used && toplevels[i].client == ci) {
			memset(&toplevels[i], 0, sizeof(toplevels[i]));
			memset(&app_menus[i], 0, sizeof(app_menus[i]));
		}
	for (int i = 0; i < MAX_WOBJECTS; i++)
		if (wobjects[i].used && wobjects[i].client == ci)
			memset(&wobjects[i], 0, sizeof(wobjects[i]));
	for (int i = 0; i < MAX_WPOOLS; i++)
		if (wpools[i].used && wpools[i].client == ci) {
			close(wpools[i].fd);
			memset(&wpools[i], 0, sizeof(wpools[i]));
		}
	if (clients[ci].pending_fd >= 0) close(clients[ci].pending_fd);
	close(clients[ci].fd);
	memset(&clients[ci], 0, sizeof(clients[ci]));
}

void accept_client(int fd) {
	int cfd = accept(fd, 0, 0);
	if (cfd < 0) return;
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

void client_data(int ci) {
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
	if (n <= 0) { client_disconnect(ci); return; }
	for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm;
	     cm = CMSG_NXTHDR(&mh, cm))
		if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS &&
		    cm->cmsg_len >= CMSG_LEN(sizeof(int))) {
			if (c->pending_fd >= 0) close(c->pending_fd);
			memcpy(&c->pending_fd, CMSG_DATA(cm), sizeof(int));
		}
	c->inlen += (unsigned)n;
	for (;;) {
		if (c->inlen < sizeof(struct wl_hdr)) break;
		struct wl_hdr h;
		memcpy(&h, c->inbuf, sizeof(h));
		if (h.size < sizeof(h) || h.size > MAX_MSG || (h.size & 3)) {
			client_disconnect(ci); return;
		}
		if (c->inlen < h.size) break;
		uint32_t args[(MAX_MSG - sizeof(struct wl_hdr)) / 4];
		unsigned nargs = (h.size - sizeof(h)) / 4;
		memcpy(args, c->inbuf + sizeof(h), nargs * 4);
		memmove(c->inbuf, c->inbuf + h.size, c->inlen - h.size);
		c->inlen -= h.size;
		handle_wayland_msg(ci, &h, args, nargs);
		if (!clients[ci].used) return;
	}
}

/* ── clipboard / DnD helpers ── */
struct wobject *dnd_target_device(int ci) {
	return wobject_type_find(ci, WOBJ_DATA_DEVICE);
}

void clipboard_offer_to(int ci, uint32_t device_id) {
	if (sel_client < 0 || ci == sel_client) return;
	uint32_t offer_id = server_id_next++;
	wobject_add(ci, offer_id, WOBJ_DATA_OFFER, 0);
	send_msg(ci, device_id, 0, &offer_id, 1);
	uint32_t buf[20];
	unsigned k = wl_pack_string(buf, 0, sel_mime);
	send_msg(ci, offer_id, 0, buf, k);
	send_msg(ci, device_id, 5, &offer_id, 1);
}

void dnd_offer_enter(struct dsurface *s) {
	if (!dnd_active || !s) return;
	int ci = s->client;
	struct wobject *dev = dnd_target_device(ci);
	if (!dev) return;
	uint32_t offer_id = server_id_next++;
	if (!wobject_add(ci, offer_id, WOBJ_DATA_OFFER, 1)) return;
	dnd_target_client = ci;
	dnd_target_offer = offer_id;
	dnd_accepted_action = 0;
	dnd_in_surface = (int)(s - surfaces);
	send_msg(ci, dev->id, 0, &offer_id, 1);
	uint32_t obuf[20];
	unsigned k = wl_pack_string(obuf, 0, dnd_mime);
	send_msg(ci, offer_id, 0, obuf, k);
	uint32_t acts = dnd_src_actions;
	send_msg(ci, offer_id, 1, &acts, 1);
	uint32_t enter[5] = {input_serial, s->id,
	                     (uint32_t)((px - s->x) << 8),
	                     (uint32_t)((py - s->y) << 8), offer_id};
	send_msg(ci, dev->id, 1, enter, 5);
}

void dnd_offer_leave(void) {
	if (dnd_target_client < 0) return;
	struct wobject *dev = dnd_target_device(dnd_target_client);
	if (dev) send_msg(dnd_target_client, dev->id, 2, 0, 0);
	dnd_target_client = -1;
	dnd_target_offer = 0;
	dnd_in_surface = -1;
}

void dnd_motion(void) {
	if (!dnd_active) return;
	int slot = surface_at(px, py);
	struct dsurface *s = slot_surface(slot);
	if (slot != dnd_in_surface) {
		if (dnd_target_client >= 0) dnd_offer_leave();
		if (s) dnd_offer_enter(s);
		return;
	}
	if (s && dnd_target_client >= 0) {
		struct wobject *dev = dnd_target_device(dnd_target_client);
		if (dev) {
			uint32_t m[3] = {frame_serial, (uint32_t)((px - s->x) << 8),
			                 (uint32_t)((py - s->y) << 8)};
			send_msg(dnd_target_client, dev->id, 3, m, 3);
		}
	}
}

void dnd_finish_drag(void) {
	if (!dnd_active) return;
	int dropped = 0;
	if (dnd_target_client >= 0 && dnd_accepted_action != 0) {
		struct wobject *dev = dnd_target_device(dnd_target_client);
		if (dev) {
			send_msg(dnd_target_client, dev->id, 4, 0, 0);
			dropped = 1;
		}
	}
	if (dnd_src_client >= 0) {
		if (dropped) {
			send_msg(dnd_src_client, dnd_source, 3, 0, 0);
			uint32_t act = dnd_accepted_action;
			send_msg(dnd_src_client, dnd_source, 5, &act, 1);
		} else {
			send_msg(dnd_src_client, dnd_source, 2, 0, 0);
			dnd_offer_leave();
		}
	}
	dnd_active = 0; dnd_src_client = -1; dnd_source = 0;
	dnd_src_actions = 0;
}
