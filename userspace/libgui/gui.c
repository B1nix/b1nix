#include <b1nix/display.h>
#include <b1nix/gui.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syscall.h>
#include <time.h>
#include <unistd.h>

static int request(int fd, uint32_t object_id, uint16_t opcode,
                   const uint32_t *words, unsigned nwords) {
	uint8_t msg[B1D_MAX_MSG];
	struct b1d_hdr header = {
	    object_id, opcode, (uint16_t)(sizeof(struct b1d_hdr) + nwords * 4)};
	if (header.size > sizeof(msg))
		return -1;
	memcpy(msg, &header, sizeof(header));
	if (nwords)
		memcpy(msg + sizeof(header), words, nwords * 4);
	return send(fd, msg, header.size, 0) == header.size ? 0 : -1;
}

static int request_fd(int fd, uint32_t object_id, uint16_t opcode,
                      const uint32_t *words, unsigned nwords, int passed_fd) {
	uint8_t msg[B1D_MAX_MSG];
	char control[CMSG_SPACE(sizeof(int))];
	struct b1d_hdr header = {
	    object_id, opcode, (uint16_t)(sizeof(struct b1d_hdr) + nwords * 4)};
	if (header.size > sizeof(msg))
		return -1;
	memcpy(msg, &header, sizeof(header));
	if (nwords)
		memcpy(msg + sizeof(header), words, nwords * 4);
	memset(control, 0, sizeof(control));
	struct iovec iov = {msg, header.size};
	struct msghdr mh;
	memset(&mh, 0, sizeof(mh));
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = control;
	mh.msg_controllen = sizeof(control);
	struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &passed_fd, sizeof(passed_fd));
	return sendmsg(fd, &mh, 0) == header.size ? 0 : -1;
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
	strcpy(addr.sun_path, B1D_SOCKET_PATH);
	if (connect(win->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(win->fd);
		win->fd = -1;
		return -1;
	}
	/* Don't let a fork()+exec child (e.g. gterm's shell) inherit the display
	 * connection: an inherited fd keeps the socket — and thus the client's
	 * surfaces and server slot — alive after the GUI process exits, which
	 * leaks slots and leaves ghost windows on screen. */
	fcntl(win->fd, F_SETFD, FD_CLOEXEC);
	return 0;
}

int b1gui_create_window(struct b1gui_window *win, uint32_t width,
                        uint32_t height, const char *title) {
	if (!win || win->fd < 0 || width == 0 || height == 0)
		return -1;
	win->surface_id = B1D_CLIENT_ID_BASE;
	win->buffer_id = B1D_CLIENT_ID_BASE + 1;
	win->toplevel_id = B1D_CLIENT_ID_BASE + 2;
	win->next_id = B1D_CLIENT_ID_BASE + 3;
	win->width = width;
	win->height = height;

	size_t buffer_size = (size_t)width * height * 4;
	win->buffer_fd = memfd_create("b1display-buffer", MFD_CLOEXEC);
	if (win->buffer_fd < 0 || ftruncate(win->buffer_fd, (off_t)buffer_size) < 0)
		return -1;
	win->pixels = mmap(0, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	                   win->buffer_fd, 0);
	if (win->pixels == MAP_FAILED) {
		win->pixels = 0;
		return -1;
	}

	uint32_t surface[1] = {win->surface_id};
	uint32_t buffer[5] = {win->buffer_id, 0, width, height, width * 4};
	uint32_t top[2] = {win->toplevel_id, win->surface_id};
	if (request(win->fd, B1D_OBJ_DISPLAY, B1D_REQ_DISPLAY_CREATE_SURFACE,
	            surface, 1) ||
	    request_fd(win->fd, B1D_OBJ_DISPLAY, B1D_REQ_DISPLAY_CREATE_BUFFER,
	               buffer, 5, win->buffer_fd) ||
	    request(win->fd, B1D_OBJ_DISPLAY, B1D_REQ_DISPLAY_CREATE_TOPLEVEL,
	            top, 2))
		return -1;

	uint32_t title_words[9];
	memset(title_words, 0, sizeof(title_words));
	unsigned len = title ? strlen(title) : 0;
	if (len > 31)
		len = 31;
	title_words[0] = len;
	if (len)
		memcpy(&title_words[1], title, len);
	return request(win->fd, win->toplevel_id, B1D_REQ_TOPLEVEL_SET_TITLE,
	               title_words, 1 + (len + 3) / 4);
}

int b1gui_present(struct b1gui_window *win, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height) {
	uint32_t attach[1] = {win->buffer_id};
	uint32_t damage[4] = {x, y, width, height};
	uint32_t callback[1] = {win->next_id++};
	return request(win->fd, win->surface_id, B1D_REQ_SURFACE_ATTACH, attach, 1) ||
	       request(win->fd, win->surface_id, B1D_REQ_SURFACE_DAMAGE, damage, 4) ||
	       request(win->fd, win->surface_id, B1D_REQ_SURFACE_FRAME, callback, 1) ||
	       request(win->fd, win->surface_id, B1D_REQ_SURFACE_COMMIT, 0, 0)
	           ? -1 : 0;
}

int b1gui_next_event(struct b1gui_window *win, struct b1gui_event *event,
                     int timeout_ms) {
	for (;;) {
		if (win->inlen >= sizeof(struct b1d_hdr)) {
			struct b1d_hdr header;
			memcpy(&header, win->inbuf, sizeof(header));
			if (header.size < sizeof(header) || header.size > B1D_MAX_MSG ||
			    (header.size & 3))
				return -1;
			if (win->inlen >= header.size) {
				event->object_id = header.object_id;
				event->opcode = header.opcode;
				event->nargs = (header.size - sizeof(header)) / 4;
				if (event->nargs > 8)
					event->nargs = 8;
				memcpy(event->args, win->inbuf + sizeof(header),
				       event->nargs * 4);
				memmove(win->inbuf, win->inbuf + header.size,
				        win->inlen - header.size);
				win->inlen -= header.size;
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

uint32_t b1gui_checksum(struct b1gui_window *win) {
	uint32_t callback = win->next_id++;
	request(win->fd, B1D_OBJ_DISPLAY, B1D_REQ_DISPLAY_CHECKSUM, &callback, 1);
	struct b1gui_event event;
	for (int i = 0; i < 20; i++)
		if (b1gui_next_event(win, &event, 100) == 1 &&
		    event.object_id == callback && event.opcode == B1D_EV_CALLBACK_VALUE &&
		    event.nargs)
			return event.args[0];
	return 0;
}

void b1gui_destroy(struct b1gui_window *win) {
	if (!win)
		return;
	if (win->fd >= 0) {
		request(win->fd, win->toplevel_id, B1D_REQ_TOPLEVEL_DESTROY, 0, 0);
		request(win->fd, win->surface_id, B1D_REQ_SURFACE_DESTROY, 0, 0);
		request(win->fd, win->buffer_id, B1D_REQ_BUFFER_DESTROY, 0, 0);
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
