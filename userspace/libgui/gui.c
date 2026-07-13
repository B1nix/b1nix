#include <b1nix/gui.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syscall.h>
#include <unistd.h>

#define WAYLAND_SOCKET_PATH "/run/wayland-0"
#define MAX_MSG 256

struct wl_hdr {
	uint32_t object;
	uint16_t opcode;
	uint16_t size;
};

static int request(int fd, uint32_t object, uint16_t opcode,
                   const uint32_t *args, unsigned nargs) {
	uint8_t msg[MAX_MSG];
	struct wl_hdr h = {object, opcode, (uint16_t)(sizeof(h) + nargs * 4)};
	if (h.size > sizeof(msg))
		return -1;
	memcpy(msg, &h, sizeof(h));
	memcpy(msg + sizeof(h), args, nargs * 4);
	return send(fd, msg, h.size, 0) == h.size ? 0 : -1;
}

static int request_string(int fd, uint32_t object, uint16_t opcode,
                          const uint32_t *prefix, unsigned nprefix,
                          const char *text, const uint32_t *suffix,
                          unsigned nsuffix) {
	uint32_t args[32];
	unsigned len = (unsigned)strlen(text) + 1;
	unsigned ntext = (len + 3) / 4;
	if (nprefix + 1 + ntext + nsuffix > 32)
		return -1;
	memcpy(args, prefix, nprefix * 4);
	args[nprefix] = len;
	memset(&args[nprefix + 1], 0, ntext * 4);
	memcpy(&args[nprefix + 1], text, len);
	memcpy(&args[nprefix + 1 + ntext], suffix, nsuffix * 4);
	return request(fd, object, opcode, args, nprefix + 1 + ntext + nsuffix);
}

static int request_fd(int fd, uint32_t object, uint16_t opcode,
                      const uint32_t *args, unsigned nargs, int passed_fd) {
	uint8_t msg[MAX_MSG];
	char control[CMSG_SPACE(sizeof(int))];
	struct wl_hdr h = {object, opcode, (uint16_t)(sizeof(h) + nargs * 4)};
	memcpy(msg, &h, sizeof(h));
	memcpy(msg + sizeof(h), args, nargs * 4);
	struct iovec iov = {msg, h.size};
	struct msghdr mh;
	memset(&mh, 0, sizeof(mh));
	memset(control, 0, sizeof(control));
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = control;
	mh.msg_controllen = sizeof(control);
	struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &passed_fd, sizeof(passed_fd));
	return sendmsg(fd, &mh, 0) == h.size ? 0 : -1;
}

static int raw_event(struct b1gui_window *win, struct wl_hdr *h,
                     uint32_t *args, int timeout_ms) {
	for (;;) {
		if (win->inlen >= sizeof(*h)) {
			memcpy(h, win->inbuf, sizeof(*h));
			if (h->size < sizeof(*h) || h->size > MAX_MSG || (h->size & 3))
				return -1;
			if (win->inlen >= h->size) {
				memcpy(args, win->inbuf + sizeof(*h), h->size - sizeof(*h));
				memmove(win->inbuf, win->inbuf + h->size,
				        win->inlen - h->size);
				win->inlen -= h->size;
				return 1;
			}
		}
		struct pollfd pfd = {win->fd, POLLIN, 0};
		if (poll(&pfd, 1, timeout_ms) <= 0)
			return 0;
		ssize_t n = recv(win->fd, win->inbuf + win->inlen,
		                 sizeof(win->inbuf) - win->inlen, 0);
		if (n <= 0)
			return -1;
		win->inlen += (unsigned)n;
	}
}

static void bind_global(struct b1gui_window *win, uint32_t name,
                        const char *interface, uint32_t version, uint32_t id) {
	uint32_t prefix = name;
	uint32_t suffix[2] = {version, id};
	request_string(win->fd, win->registry_id, 0, &prefix, 1, interface,
	               suffix, 2);
}

int b1gui_connect(struct b1gui_window *win) {
	memset(win, 0, sizeof(*win));
	win->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	win->buffer_fd = -1;
	if (win->fd < 0)
		return -1;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, WAYLAND_SOCKET_PATH);
	if (connect(win->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(win->fd);
		win->fd = -1;
		return -1;
	}
	fcntl(win->fd, F_SETFD, FD_CLOEXEC);
	win->registry_id = 2;
	uint32_t sync_id = 3;
	request(win->fd, 1, 1, &win->registry_id, 1);
	request(win->fd, 1, 0, &sync_id, 1);

	struct wl_hdr h;
	uint32_t args[32];
	while (raw_event(win, &h, args, 3000) == 1) {
		if (h.object == win->registry_id && h.opcode == 0) {
			const char *interface = (const char *)&args[2];
			uint32_t version = args[2 + (args[1] + 3) / 4];
			if (!strcmp(interface, "wl_compositor")) {
				win->compositor_id = 4;
				bind_global(win, args[0], interface, version < 4 ? version : 4,
				            win->compositor_id);
			} else if (!strcmp(interface, "wl_shm")) {
				win->shm_id = 5;
				bind_global(win, args[0], interface, 1, win->shm_id);
			} else if (!strcmp(interface, "xdg_wm_base")) {
				win->wm_base_id = 6;
				bind_global(win, args[0], interface, 1, win->wm_base_id);
			} else if (!strcmp(interface, "wl_seat")) {
				win->seat_id = 7;
				bind_global(win, args[0], interface, version < 5 ? version : 5,
				            win->seat_id);
			}
		}
		if (h.object == sync_id && h.opcode == 0)
			break;
	}
	if (!win->compositor_id || !win->shm_id || !win->wm_base_id)
		return -1;
	win->next_id = 8;
	return 0;
}

int b1gui_create_window(struct b1gui_window *win, uint32_t width,
                        uint32_t height, const char *title) {
	if (!win || win->fd < 0 || !width || !height)
		return -1;
	win->surface_id = win->next_id++;
	win->xdg_surface_id = win->next_id++;
	win->toplevel_id = win->next_id++;
	win->pool_id = win->next_id++;
	win->buffer_id = win->next_id++;
	win->pointer_id = win->next_id++;
	win->keyboard_id = win->next_id++;
	win->width = width;
	win->height = height;

	request(win->fd, win->compositor_id, 0, &win->surface_id, 1);
	uint32_t xdg[2] = {win->xdg_surface_id, win->surface_id};
	request(win->fd, win->wm_base_id, 2, xdg, 2);
	request(win->fd, win->xdg_surface_id, 1, &win->toplevel_id, 1);
	request_string(win->fd, win->toplevel_id, 2, 0, 0,
	               title ? title : "b1nix", 0, 0);
	request(win->fd, win->surface_id, 6, 0, 0);

	struct wl_hdr h;
	uint32_t args[32];
	uint32_t serial = 0;
	while (!serial && raw_event(win, &h, args, 3000) == 1) {
		if (h.object == win->xdg_surface_id && h.opcode == 0)
			serial = args[0];
	}
	if (!serial)
		return -1;
	request(win->fd, win->xdg_surface_id, 4, &serial, 1);

	size_t size = (size_t)width * height * 4;
	win->buffer_fd = memfd_create("wayland-shm", MFD_CLOEXEC);
	if (win->buffer_fd < 0 || ftruncate(win->buffer_fd, (off_t)size) < 0)
		return -1;
	win->pixels = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED,
	                   win->buffer_fd, 0);
	if (win->pixels == MAP_FAILED) {
		win->pixels = 0;
		return -1;
	}
	uint32_t pool[2] = {win->pool_id, (uint32_t)size};
	request_fd(win->fd, win->shm_id, 0, pool, 2, win->buffer_fd);
	uint32_t buffer[6] = {win->buffer_id, 0, width, height, width * 4, 1};
	request(win->fd, win->pool_id, 0, buffer, 6);
	request(win->fd, win->pool_id, 1, 0, 0);
	if (win->seat_id) {
		request(win->fd, win->seat_id, 0, &win->pointer_id, 1);
		request(win->fd, win->seat_id, 1, &win->keyboard_id, 1);
	}
	return 0;
}

int b1gui_present(struct b1gui_window *win, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height) {
	uint32_t attach[3] = {win->buffer_id, 0, 0};
	uint32_t damage[4] = {x, y, width, height};
	uint32_t callback = win->next_id++;
	return request(win->fd, win->surface_id, 1, attach, 3) ||
	       request(win->fd, win->surface_id, 2, damage, 4) ||
	       request(win->fd, win->surface_id, 3, &callback, 1) ||
	       request(win->fd, win->surface_id, 6, 0, 0) ? -1 : 0;
}

int b1gui_next_event(struct b1gui_window *win, struct b1gui_event *event,
                     int timeout_ms) {
	struct wl_hdr h;
	uint32_t args[32];
	for (;;) {
		int rc = raw_event(win, &h, args, timeout_ms);
		if (rc != 1)
			return rc;
		memset(event, 0, sizeof(*event));
		if (h.object == win->wm_base_id && h.opcode == 0) {
			/* xdg_wm_base.ping → pong: prove we are alive. */
			request(win->fd, win->wm_base_id, 3, &args[0], 1);
			continue;
		}
		if (h.object == win->toplevel_id && h.opcode == 1)
			event->type = B1GUI_EV_CLOSE;
		else if (h.object == win->toplevel_id && h.opcode == 20) {
			event->type = B1GUI_EV_MENU_ITEM;
			event->args[0] = args[0];
			event->nargs = 1;
		} else if (h.object == win->toplevel_id && h.opcode == 21) {
			event->type = B1GUI_EV_DOCK_RESTORE;
		} else if (h.object == win->pointer_id && h.opcode <= 3) {
			if (h.opcode == 0) {
				event->type = B1GUI_EV_POINTER_ENTER;
				event->args[0] = args[2] >> 8;
				event->args[1] = args[3] >> 8;
				event->nargs = 2;
			} else if (h.opcode == 1) {
				event->type = B1GUI_EV_POINTER_LEAVE;
			} else if (h.opcode == 2) {
				event->type = B1GUI_EV_POINTER_MOTION;
				event->args[0] = args[1] >> 8;
				event->args[1] = args[2] >> 8;
				event->nargs = 2;
			} else {
				event->type = B1GUI_EV_POINTER_BUTTON;
				event->args[0] = args[2];
				event->args[1] = args[3];
				event->nargs = 2;
			}
		} else if (h.object == win->keyboard_id) {
			if (h.opcode == 1)
				event->type = B1GUI_EV_FOCUS_ENTER;
			else if (h.opcode == 2)
				event->type = B1GUI_EV_FOCUS_LEAVE;
			else if (h.opcode == 3) {
				event->type = B1GUI_EV_KEY;
				event->args[0] = args[2];
				event->args[1] = args[3];
				event->nargs = 2;
			}
		} else if (h.opcode == 0 && h.object >= 8) {
			event->type = B1GUI_EV_FRAME;
		}
		if (event->type)
			return 1;
	}
}

uint32_t b1gui_checksum(struct b1gui_window *win) {
	uint32_t sync = win->next_id++;
	request(win->fd, 1, 0, &sync, 1);
	struct wl_hdr h;
	uint32_t args[32];
	for (int i = 0; i < 20; i++)
		if (raw_event(win, &h, args, 100) == 1 &&
		    h.object == sync && h.opcode == 0)
			return 1;
	return 0;
}

int b1gui_register_menu(struct b1gui_window *win,
                        const struct b1gui_menu_item *items, int count) {
	if (!win || win->fd < 0 || !items || count <= 0 ||
	    count > B1GUI_MAX_MENU_ITEMS)
		return -1;
	/* Custom extension: opcode 20 on toplevel.
	 * Wire format: count(u32), then per item: id(u16)|flags(u16), label(str),
	 * accel(str). Pack into a uint32 buffer. */
	uint32_t args[128];
	unsigned n = 0;
	args[n++] = (uint32_t)count;
	for (int i = 0; i < count; i++) {
		args[n++] = ((uint32_t)items[i].id << 16) | (uint32_t)items[i].flags;
		/* label as wl_string: length then padded chars */
		unsigned llen = (unsigned)strlen(items[i].label) + 1;
		unsigned lwords = (llen + 3) / 4;
		args[n++] = llen;
		memset(&args[n], 0, lwords * 4);
		memcpy(&args[n], items[i].label, llen);
		n += lwords;
		/* accel as wl_string */
		unsigned alen = (unsigned)strlen(items[i].accel) + 1;
		unsigned awords = (alen + 3) / 4;
		args[n++] = alen;
		memset(&args[n], 0, awords * 4);
		memcpy(&args[n], items[i].accel, alen);
		n += awords;
	}
	if (n > 128) return -1;
	return request(win->fd, win->toplevel_id, 20, args, n);
}

void b1gui_destroy(struct b1gui_window *win) {
	if (!win)
		return;
	if (win->fd >= 0) {
		request(win->fd, win->toplevel_id, 0, 0, 0);
		request(win->fd, win->xdg_surface_id, 0, 0, 0);
		request(win->fd, win->surface_id, 0, 0, 0);
		request(win->fd, win->buffer_id, 0, 0, 0);
		close(win->fd);
	}
	if (win->pixels)
		munmap(win->pixels, (size_t)win->width * win->height * 4);
	if (win->buffer_fd >= 0)
		close(win->buffer_fd);
	memset(win, 0, sizeof(*win));
	win->fd = -1;
	win->buffer_fd = -1;
}
