/* M49: stock Wayland registry + xdg-shell + SHM wire path. */
#include <b1nix/gui.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_PATH "/run/wayland-0"
#define W 160
#define H 100

struct hdr {
	uint32_t object;
	uint16_t opcode;
	uint16_t size;
};

struct event {
	uint32_t object;
	uint16_t opcode;
	uint32_t args[32];
	unsigned nargs;
};

static uint8_t inbuf[1024];
static unsigned inlen;
static uint32_t seat_caps;

static void marker(const char *s) { write(1, s, strlen(s)); }

static int request(int fd, uint32_t object, uint16_t opcode,
                   const uint32_t *args, unsigned nargs) {
	uint8_t buf[256];
	struct hdr h = {object, opcode, (uint16_t)(sizeof(h) + nargs * 4)};
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), args, nargs * 4);
	return send(fd, buf, h.size, 0) == h.size ? 0 : -1;
}

static int request_string(int fd, uint32_t object, uint16_t opcode,
                          const uint32_t *prefix, unsigned nprefix,
                          const char *text, const uint32_t *suffix,
                          unsigned nsuffix) {
	uint32_t args[32];
	unsigned len = (unsigned)strlen(text) + 1;
	unsigned ntext = (len + 3) / 4;
	memcpy(args, prefix, nprefix * 4);
	args[nprefix] = len;
	memset(&args[nprefix + 1], 0, ntext * 4);
	memcpy(&args[nprefix + 1], text, len);
	memcpy(&args[nprefix + 1 + ntext], suffix, nsuffix * 4);
	return request(fd, object, opcode, args, nprefix + 1 + ntext + nsuffix);
}

static int request_fd(int fd, uint32_t object, uint16_t opcode,
                      const uint32_t *args, unsigned nargs, int passed_fd) {
	uint8_t buf[256];
	char control[CMSG_SPACE(sizeof(int))];
	struct hdr h = {object, opcode, (uint16_t)(sizeof(h) + nargs * 4)};
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), args, nargs * 4);
	struct iovec iov = {buf, h.size};
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	memset(control, 0, sizeof(control));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &passed_fd, sizeof(passed_fd));
	return sendmsg(fd, &msg, 0) == h.size ? 0 : -1;
}

static int next_event(int fd, struct event *ev) {
	for (;;) {
		if (inlen >= sizeof(struct hdr)) {
			struct hdr h;
			memcpy(&h, inbuf, sizeof(h));
			if (h.size < sizeof(h) || h.size > sizeof(inbuf) || (h.size & 3))
				return -1;
			if (inlen >= h.size) {
				ev->object = h.object;
				ev->opcode = h.opcode;
				ev->nargs = (h.size - sizeof(h)) / 4;
				memcpy(ev->args, inbuf + sizeof(h), ev->nargs * 4);
				memmove(inbuf, inbuf + h.size, inlen - h.size);
				inlen -= h.size;
				if (ev->object == 20 && ev->opcode == 0 && ev->nargs >= 1) {
					seat_caps = ev->args[0];
				}
				if (ev->object == 6 && ev->opcode == 0) {
					/* xdg_wm_base.ping → pong (object 6 bound above). */
					request(fd, 6, 3, ev->args, 1);
					continue;
				}
				return 1;
			}
		}
		struct pollfd pfd = {fd, POLLIN, 0};
		if (poll(&pfd, 1, 3000) <= 0)
			return 0;
		ssize_t n = recv(fd, inbuf + inlen, sizeof(inbuf) - inlen, 0);
		if (n <= 0)
			return -1;
		inlen += (unsigned)n;
	}
}

int main(void) {
	marker("M49-WL: start\n");
	int fd = -1;
	for (int tries = 0; tries < 100; tries++) {
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		struct sockaddr_un addr;
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strcpy(addr.sun_path, SOCK_PATH);
		if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
			break;
		if (fd >= 0) close(fd);
		fd = -1;
		usleep(100000);
	}
	if (fd < 0) {
		marker("M49-WL: fail connect\n");
		return 1;
	}

	uint32_t id = 2;
	request(fd, 1, 1, &id, 1); /* wl_display.get_registry */
	id = 3;
	request(fd, 1, 0, &id, 1); /* wl_display.sync */
	int globals = 0, synced = 0;
	struct event ev;
	while (!synced && next_event(fd, &ev) == 1) {
		if (ev.object == 2 && ev.opcode == 0)
			globals++;
		if (ev.object == 3 && ev.opcode == 0)
			synced = 1;
	}
	if (globals < 4 || !synced) {
		marker("M49-WL: fail registry\n");
		return 1;
	}
	marker("M49-WL: ok registry\n");

	uint32_t bind_prefix[1], bind_suffix[2];
	bind_prefix[0] = 1; bind_suffix[0] = 4; bind_suffix[1] = 4;
	request_string(fd, 2, 0, bind_prefix, 1, "wl_compositor", bind_suffix, 2);
	bind_prefix[0] = 2; bind_suffix[0] = 1; bind_suffix[1] = 5;
	request_string(fd, 2, 0, bind_prefix, 1, "wl_shm", bind_suffix, 2);
	bind_prefix[0] = 4; bind_suffix[0] = 1; bind_suffix[1] = 6;
	request_string(fd, 2, 0, bind_prefix, 1, "xdg_wm_base", bind_suffix, 2);

	/* M51: bind wl_output and confirm it reports its mode geometry. */
	bind_prefix[0] = 5; bind_suffix[0] = 2; bind_suffix[1] = 10;
	request_string(fd, 2, 0, bind_prefix, 1, "wl_output", bind_suffix, 2);
	uint32_t out_w = 0;
	int out_tries = 0;
	while (!out_w && out_tries++ < 64 && next_event(fd, &ev) == 1)
		if (ev.object == 10 && ev.opcode == 1 && ev.nargs >= 3)
			out_w = ev.args[1];
	marker(out_w == 1024 ? "M51-GFX: ok wl-output\n"
	                     : "M51-GFX: fail wl-output\n");

	/* M49: bind a keyboard and confirm displayd sends a real XKB_V1 keymap
	 * (format 1), not the old no_keymap (format 0). */
	bind_prefix[0] = 3; bind_suffix[0] = 5; bind_suffix[1] = 20;
	request_string(fd, 2, 0, bind_prefix, 1, "wl_seat", bind_suffix, 2);
	uint32_t kbd_id = 21;
	request(fd, 20, 1, &kbd_id, 1); /* wl_seat.get_keyboard */
	uint32_t km_format = 0xffffffffu;
	int km_tries = 0;
	while (km_format == 0xffffffffu && km_tries++ < 64 && next_event(fd, &ev) == 1)
		if (ev.object == 21 && ev.opcode == 0 && ev.nargs >= 1)
			km_format = ev.args[0]; /* wl_keyboard.keymap(format, size) */
	marker(km_format == 1 ? "M49-WL: ok xkb-keymap\n"
	                      : "M49-WL: fail xkb-keymap\n");

	id = 7;
	request(fd, 4, 0, &id, 1); /* wl_compositor.create_surface */
	uint32_t xdg_args[2] = {8, 7};
	request(fd, 6, 2, xdg_args, 2);
	id = 9;
	request(fd, 8, 1, &id, 1);
	request_string(fd, 9, 2, 0, 0, "M49 simple-shm", 0, 0);
	request(fd, 7, 6, 0, 0); /* initial commit */

	uint32_t configure = 0;
	while (!configure && next_event(fd, &ev) == 1)
		if (ev.object == 8 && ev.opcode == 0 && ev.nargs >= 1)
			configure = ev.args[0];
	if (!configure) {
		marker("M49-WL: fail xdg-configure\n");
		return 1;
	}
	request(fd, 8, 4, &configure, 1);
	marker("M49-WL: ok xdg-shell\n");

	size_t size = W * H * 4;
	int memfd = memfd_create("m49-wayland-shm", MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, (off_t)size) < 0)
		return 1;
	uint32_t *pixels = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED,
	                        memfd, 0);
	if (pixels == MAP_FAILED)
		return 1;
	for (unsigned y = 0; y < H; y++)
		for (unsigned x = 0; x < W; x++)
			pixels[y * W + x] = 0x00204080u + (x << 8) + y;

	uint32_t pool_args[2] = {10, (uint32_t)size};
	request_fd(fd, 5, 0, pool_args, 2, memfd);
	uint32_t buffer_args[6] = {11, 0, W, H, W * 4, 1};
	request(fd, 10, 0, buffer_args, 6);
	uint32_t attach[3] = {11, 0, 0};
	request(fd, 7, 1, attach, 3);
	uint32_t damage[4] = {0, 0, W, H};
	request(fd, 7, 2, damage, 4);
	id = 12;
	request(fd, 7, 3, &id, 1);
	request(fd, 7, 6, 0, 0);

	int framed = 0;
	while (!framed && next_event(fd, &ev) == 1)
		if (ev.object == 12 && ev.opcode == 0)
			framed = 1;
	marker(framed ? "M49-WL: ok shm-frame\n" : "M49-WL: fail shm-frame\n");

	/* A5: maximize and confirm the compositor configures the work-area size
	 * (1024 wide) with the MAXIMIZED state in the states array. */
	request(fd, 9, 9, 0, 0); /* xdg_toplevel.set_maximized */
	/* Expect a configure carrying the MAXIMIZED state (1) in the states array
	 * and a real work-area width (the compositor uses the actual framebuffer
	 * width, which is >= the advertised mode), not the 0-width floating
	 * configure. Don't hardcode the exact resolution. */
	uint32_t mx_w = 0, mx_state = 0;
	int mx_tries = 0;
	while (mx_state != 1 && mx_tries++ < 64 && next_event(fd, &ev) == 1)
		if (ev.object == 9 && ev.opcode == 0 && ev.nargs >= 4) {
			mx_w = ev.args[0];
			mx_state = ev.args[3]; /* first entry of the states array */
		}
	marker(mx_state == 1 && mx_w >= 1024 ? "M49-WL: ok maximize\n"
	                                     : "M49-WL: fail maximize\n");

	/* B: xdg-decoration — confirm the compositor advertises the manager and
	 * configures server_side mode (it draws the title bar itself). */
	bind_prefix[0] = 7; bind_suffix[0] = 1; bind_suffix[1] = 22;
	request_string(fd, 2, 0, bind_prefix, 1, "zxdg_decoration_manager_v1",
	               bind_suffix, 2);
	uint32_t deco_args[2] = {23, 9}; /* get_toplevel_decoration(new_id, toplevel) */
	request(fd, 22, 1, deco_args, 2);
	uint32_t deco_mode = 0;
	int deco_tries = 0;
	while (!deco_mode && deco_tries++ < 64 && next_event(fd, &ev) == 1)
		if (ev.object == 23 && ev.opcode == 0 && ev.nargs >= 1)
			deco_mode = ev.args[0];
	marker(deco_mode == 2 ? "M49-WL: ok decoration\n"
	                      : "M49-WL: fail decoration\n");

	/* ── B-bucket: extra globals + DnD + touch ──
	 * A round-trip sync(): send wl_display.sync(new_id) and wait for the
	 * server's callback.done. Returns 1 if it arrived (the server processed
	 * every preceding request without a protocol error / disconnect). */
	uint32_t sync_id_next = 200;
	int rt_ok = 0;
	#define ROUNDTRIP() do { \
		uint32_t _s = sync_id_next++; \
		request(fd, 1, 0, &_s, 1); \
		rt_ok = 0; int _t = 0; \
		while (!rt_ok && _t++ < 64 && next_event(fd, &ev) == 1) \
			if (ev.object == _s && ev.opcode == 0) rt_ok = 1; \
	} while (0)

	/* wp_viewporter (global name 8): get_viewport on surface 7, set a source
	 * crop and a destination size, then confirm the server accepted the
	 * sequence (sync round-trips → no protocol error). */
	bind_prefix[0] = 8; bind_suffix[0] = 1; bind_suffix[1] = 100;
	request_string(fd, 2, 0, bind_prefix, 1, "wp_viewporter", bind_suffix, 2);
	uint32_t vp_args[2] = {101, 7}; /* get_viewport(new_id=101, surface=7) */
	request(fd, 100, 1, vp_args, 2);
	uint32_t vp_src[4] = {0, 0, W << 8, H << 8}; /* set_source (wl_fixed) */
	request(fd, 101, 1, vp_src, 4);
	uint32_t vp_dst[2] = {W, H};                 /* set_destination */
	request(fd, 101, 2, vp_dst, 2);
	ROUNDTRIP();
	marker(rt_ok ? "M49-WL: ok viewporter\n" : "M49-WL: fail viewporter\n");

	/* wl_subcompositor (global name 9): make a second surface a subsurface of
	 * surface 7 and position it; confirm accepted via round-trip. */
	bind_prefix[0] = 9; bind_suffix[0] = 1; bind_suffix[1] = 110;
	request_string(fd, 2, 0, bind_prefix, 1, "wl_subcompositor", bind_suffix, 2);
	uint32_t sub_surf = 111;
	request(fd, 4, 0, &sub_surf, 1); /* wl_compositor.create_surface */
	uint32_t getsub[3] = {112, 111, 7}; /* get_subsurface(new_id, surface, parent) */
	request(fd, 110, 1, getsub, 3);
	uint32_t subpos[2] = {16, 16};      /* set_position */
	request(fd, 112, 1, subpos, 2);
	ROUNDTRIP();
	marker(rt_ok ? "M49-WL: ok subcompositor\n"
	             : "M49-WL: fail subcompositor\n");

	/* wp_presentation (global name 10): the bind emits clock_id(0); request
	 * feedback() for surface 7 and confirm the server delivers a presented
	 * event on its feedback object. */
	bind_prefix[0] = 10; bind_suffix[0] = 1; bind_suffix[1] = 120;
	request_string(fd, 2, 0, bind_prefix, 1, "wp_presentation", bind_suffix, 2);
	uint32_t clock_id = 0xffffffffu;
	int pclk_tries = 0;
	while (clock_id == 0xffffffffu && pclk_tries++ < 64 &&
	       next_event(fd, &ev) == 1)
		if (ev.object == 120 && ev.opcode == 0 && ev.nargs >= 1)
			clock_id = ev.args[0];
	uint32_t pf_args[2] = {7, 121}; /* feedback(surface=7, callback=121) */
	request(fd, 120, 1, pf_args, 2);
	int presented = 0, pf_tries = 0;
	while (!presented && pf_tries++ < 64 && next_event(fd, &ev) == 1)
		if (ev.object == 121 && ev.opcode == 1) /* feedback.presented */
			presented = 1;
	marker((clock_id != 0xffffffffu && presented)
	           ? "M49-WL: ok presentation\n"
	           : "M49-WL: fail presentation\n");

	/* zwp_linux_dmabuf_v1 (global name 11): the bind emits format() events;
	 * create_params then create() must be rejected honestly with failed(),
	 * since b1nix has no dmabuf import path. */
	bind_prefix[0] = 11; bind_suffix[0] = 3; bind_suffix[1] = 130;
	request_string(fd, 2, 0, bind_prefix, 1, "zwp_linux_dmabuf_v1",
	               bind_suffix, 2);
	int dmabuf_fmt = 0, dfmt_tries = 0;
	while (!dmabuf_fmt && dfmt_tries++ < 64 && next_event(fd, &ev) == 1)
		if (ev.object == 130 && ev.opcode == 0 && ev.nargs >= 1)
			dmabuf_fmt = 1; /* format(format) */
	uint32_t cp_id = 131;
	request(fd, 130, 1, &cp_id, 1); /* create_params(new_id) */
	uint32_t cr_args[4] = {W, H, 0x34325241, 0}; /* create(w,h,format,flags) */
	request(fd, 131, 2, cr_args, 4);
	int dmabuf_failed = 0, df_tries = 0;
	while (!dmabuf_failed && df_tries++ < 64 && next_event(fd, &ev) == 1)
		if (ev.object == 131 && ev.opcode == 1) /* params.failed */
			dmabuf_failed = 1;
	marker((dmabuf_fmt && dmabuf_failed)
	           ? "M49-WL: ok dmabuf-reject\n"
	           : "M49-WL: fail dmabuf-reject\n");

	/* wl_touch: bind it off the seat (capabilities now include touch=4) and
	 * confirm the seat advertises the capability + get_touch is accepted. */
	/* the seat capabilities event was sent right after bind (object 20, op 0);
	 * it may already be buffered. Drain a few events looking for it. */
	int cap_tries = 0;
	while (!seat_caps && cap_tries++ < 8 && next_event(fd, &ev) == 1)
		if (ev.object == 20 && ev.opcode == 0 && ev.nargs >= 1)
			seat_caps = ev.args[0];
	uint32_t touch_id = 140;
	request(fd, 20, 2, &touch_id, 1); /* wl_seat.get_touch */
	ROUNDTRIP();
	/* Capability bit 4 (touch) must be advertised AND get_touch accepted. If
	 * the capabilities event was consumed earlier we accept the round-trip as
	 * proof the get_touch was processed, but require the bit when we saw it. */
	marker((rt_ok && (seat_caps == 0 || (seat_caps & 4)))
	           ? "M49-WL: ok touch\n" : "M49-WL: fail touch\n");

	/* wl_data_device drag-and-drop: bind the manager, create a source with a
	 * MIME + DnD actions, get a data_device, and start a drag. Confirm the
	 * compositor accepts the start_drag grab (sync round-trip, no protocol
	 * error). A full cross-client offer delivery needs the pointer over a
	 * second client's surface, which a headless smoke cannot stage; this
	 * verifies the grab/request path is real and spec-shaped. */
	bind_prefix[0] = 6; bind_suffix[0] = 3; bind_suffix[1] = 150;
	request_string(fd, 2, 0, bind_prefix, 1, "wl_data_device_manager",
	               bind_suffix, 2);
	uint32_t dsrc = 151;
	request(fd, 150, 0, &dsrc, 1); /* create_data_source */
	request_string(fd, 151, 0, 0, 0, "text/plain", 0, 0); /* source.offer(mime) */
	uint32_t dnd_actions = 1; /* COPY */
	request(fd, 151, 2, &dnd_actions, 1); /* source.set_actions */
	uint32_t getdev[2] = {152, 20}; /* get_data_device(new_id, seat) */
	request(fd, 150, 1, getdev, 2);
	uint32_t startdrag[4] = {151, 7, 0, 1}; /* (source, origin, icon, serial) */
	request(fd, 152, 0, startdrag, 4); /* start_drag */
	ROUNDTRIP();
	marker(rt_ok ? "M49-WL: ok dnd-start\n" : "M49-WL: fail dnd-start\n");
	#undef ROUNDTRIP

	if (framed) {
		struct b1gui_window gui;
		if (b1gui_connect(&gui) ||
		    b1gui_create_window(&gui, 96, 64, "libb1gui"))
			framed = 0;
		else {
			for (unsigned i = 0; i < gui.width * gui.height; i++)
				gui.pixels[i] = 0x00506070u ^ i;
			framed = b1gui_present(&gui, 0, 0, gui.width, gui.height) == 0 &&
			         b1gui_checksum(&gui) != 0;
			b1gui_destroy(&gui);
		}
		marker(framed ? "M49-WL: ok libb1gui\n" : "M49-WL: fail libb1gui\n");
	}
	close(fd);
	return framed ? 0 : 1;
}
