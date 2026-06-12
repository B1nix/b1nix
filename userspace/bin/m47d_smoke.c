/*
 * M47 Phase-2 Smoke Test — b1display protocol client against displayd.
 *
 * Exercises the full client path: connect to /run/b1display.sock (retry
 * while displayd starts), HELLO→INFO roundtrip, SHM buffer + surface
 * creation, attach/damage/commit with a frame callback, a SYNC roundtrip,
 * and seat input (the kernel m47-inject thread keeps feeding mouse bursts:
 * the cursor starts inside our centered surface, so displayd must deliver
 * pointer enter/motion and button+focus events to us).
 *
 * Finishes with B1D_REQ_DISPLAY_SHUTDOWN so displayd exits cleanly.
 * Emits M47-DSP markers consumed by tests/smoke.sh.
 */
#include <b1nix/display.h>
#include <b1nix/gui.h>
#include <b1nix/input.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syscall.h>
#include <unistd.h>

#define SURF_W 200
#define SURF_H 120
#define SURF_ID 0x10
#define BUF_ID 0x11
#define FRAME_CB_ID 0x12
#define SYNC_CB_ID 0x13
#define TOPLEVEL_ID 0x14
#define CHECKSUM_CB_ID 0x15
#define SHM_KEY 0x47D15000u

static void marker(const char *text) {
	write(1, text, strlen(text));
}

static int send_req(int fd, uint32_t obj, uint16_t op, const uint32_t *words,
                    unsigned nwords) {
	uint8_t msg[B1D_MAX_MSG];
	struct b1d_hdr h;
	h.object_id = obj;
	h.opcode = op;
	h.size = (uint16_t)(sizeof(h) + nwords * 4);
	memcpy(msg, &h, sizeof(h));
	memcpy(msg + sizeof(h), words, nwords * 4);
	return (int)send(fd, msg, h.size, 0) == (int)h.size ? 0 : -1;
}

/* Incoming event, decoded. */
struct ev {
	uint32_t obj;
	uint16_t op;
	uint32_t args[8];
	unsigned nargs;
};

static uint8_t inbuf[512];
static unsigned inlen;

/* Seat events can be delivered (and consumed) during any wait loop — e.g. the
 * injected pointer motion arrives while we are still waiting for the
 * commit-frame callback. Latch the pointer/button/focus signals here so a
 * later step still sees them instead of relying on fresh events arriving at
 * exactly the right moment. */
static int g_seen_pointer, g_seen_button, g_seen_focus;

/* Pull one event from the stream; poll up to timeout_ms. 1 = got one,
 * 0 = timeout, -1 = connection error. */
static int next_event(int fd, struct ev *out, int timeout_ms) {
	for (;;) {
		if (inlen >= sizeof(struct b1d_hdr)) {
			struct b1d_hdr h;
			memcpy(&h, inbuf, sizeof(h));
			if (h.size < sizeof(h) || h.size > B1D_MAX_MSG || (h.size & 3))
				return -1;
			if (inlen >= h.size) {
				out->obj = h.object_id;
				out->op = h.opcode;
				out->nargs = (h.size - sizeof(h)) / 4;
				if (out->nargs > 8)
					out->nargs = 8;
				memcpy(out->args, inbuf + sizeof(h), out->nargs * 4);
				memmove(inbuf, inbuf + h.size, inlen - h.size);
				inlen -= h.size;
				if (out->obj == B1D_OBJ_SEAT) {
					if (out->op == B1D_EV_SEAT_POINTER_ENTER ||
					    out->op == B1D_EV_SEAT_POINTER_MOTION)
						g_seen_pointer = 1;
					if (out->op == B1D_EV_SEAT_POINTER_BUTTON &&
					    out->nargs >= 2 && out->args[0] == B1NIX_BTN_LEFT &&
					    out->args[1] == 1)
						g_seen_button = 1;
					if (out->op == B1D_EV_SEAT_FOCUS_ENTER &&
					    out->nargs >= 1 && out->args[0] == SURF_ID)
						g_seen_focus = 1;
				}
				return 1;
			}
		}
		struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
		int pr = poll(&pfd, 1, timeout_ms);
		if (pr <= 0)
			return 0;
		ssize_t n = recv(fd, inbuf + inlen, sizeof(inbuf) - inlen, 0);
		if (n <= 0)
			return -1;
		inlen += (unsigned)n;
	}
}

int main(int argc, char **argv) {
	int probe = argc > 1 && strcmp(argv[1], "probe") == 0;

	marker("M47-DSP: start\n");

	/* 1: connect, retrying while displayd boots */
	int fd = -1;
	for (int tries = 0; tries < 100; tries++) {
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0)
			break;
		struct sockaddr_un addr;
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strcpy(addr.sun_path, B1D_SOCKET_PATH);
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
			break;
		close(fd);
		fd = -1;
		usleep(100000);
	}
	if (fd < 0) {
		marker("M47-DSP: fail connect\n");
		return 1;
	}
	marker("M47-DSP: ok connect\n");
	if (probe) {
		send_req(fd, B1D_OBJ_DISPLAY, B1D_REQ_DISPLAY_HELLO, 0, 0);
		struct ev probe_event;
		if (next_event(fd, &probe_event, 2000) != 1 ||
		    probe_event.op != B1D_EV_DISPLAY_INFO)
			return 1;
		send_req(fd, B1D_OBJ_DISPLAY, B1D_REQ_DISPLAY_SHUTDOWN, 0, 0);
		close(fd);
		marker("M47-DSP: ok server-restart\n");
		return 0;
	}

	/* 2: HELLO → INFO */
	send_req(fd, B1D_OBJ_DISPLAY, B1D_REQ_DISPLAY_HELLO, 0, 0);
	struct ev e;
	uint32_t scr_w = 0, scr_h = 0;
	if (next_event(fd, &e, 3000) == 1 && e.obj == B1D_OBJ_DISPLAY &&
	    e.op == B1D_EV_DISPLAY_INFO && e.nargs >= 3 && e.args[0] > 0 &&
	    e.args[1] > 0 && e.args[2] == B1D_FORMAT_XRGB8888) {
		scr_w = e.args[0];
		scr_h = e.args[1];
		marker("M47-DSP: ok info\n");
	} else {
		marker("M47-DSP: fail info\n");
		return 1;
	}
	(void)scr_w;
	(void)scr_h;

	/* 3: SHM buffer + surface, draw, attach/damage/frame/commit */
	int shmid = (int)syscall(SYS_SHMGET, SHM_KEY,
	                         (long)(SURF_W * SURF_H * 4), 0x1000 | 0666);
	if (shmid < 0) {
		marker("M47-DSP: fail shm\n");
		return 1;
	}
	uint32_t *pix = (uint32_t *)syscall(SYS_SHMAT, shmid, 0, 0);
	if (pix == (void *)-1) {
		marker("M47-DSP: fail shm\n");
		return 1;
	}
	for (int y = 0; y < SURF_H; y++)
		for (int x = 0; x < SURF_W; x++)
			pix[y * SURF_W + x] =
			    (y < 16) ? 0x00355085u : (0x00C0C8D0u ^ (uint32_t)((x ^ y) & 0x1F));

	uint32_t sid[1] = {SURF_ID};
	send_req(fd, B1D_OBJ_DISPLAY, B1D_REQ_DISPLAY_CREATE_SURFACE, sid, 1);
	/* Place our window centered on the cursor's start position so the
	 * injected pointer motion lands inside it (the server's default
	 * placement would push us to a corner the cursor never reaches). */
	uint32_t pos[2] = {(uint32_t)((int)scr_w / 2 - SURF_W / 2),
	                   (uint32_t)((int)scr_h / 2 - SURF_H / 2)};
	send_req(fd, SURF_ID, B1D_REQ_SURFACE_SET_POSITION, pos, 2);
	uint32_t bargs[6] = {BUF_ID, SHM_KEY, 0, SURF_W, SURF_H, SURF_W * 4};
	send_req(fd, B1D_OBJ_DISPLAY, B1D_REQ_DISPLAY_CREATE_BUFFER, bargs, 6);
	uint32_t top[2] = {TOPLEVEL_ID, SURF_ID};
	send_req(fd, B1D_OBJ_DISPLAY, B1D_REQ_DISPLAY_CREATE_TOPLEVEL, top, 2);
	uint32_t title[3] = {7, 0, 0};
	memcpy(&title[1], "M47 one", 7);
	send_req(fd, TOPLEVEL_ID, B1D_REQ_TOPLEVEL_SET_TITLE, title, 3);
	uint32_t aid[1] = {BUF_ID};
	send_req(fd, SURF_ID, B1D_REQ_SURFACE_ATTACH, aid, 1);
	uint32_t dmg[4] = {0, 0, SURF_W, SURF_H};
	send_req(fd, SURF_ID, B1D_REQ_SURFACE_DAMAGE, dmg, 4);
	uint32_t fcb[1] = {FRAME_CB_ID};
	send_req(fd, SURF_ID, B1D_REQ_SURFACE_FRAME, fcb, 1);
	send_req(fd, SURF_ID, B1D_REQ_SURFACE_COMMIT, 0, 0);

	/* expect the frame callback (errors would arrive as DISPLAY_ERROR) */
	int got_frame = 0;
	for (int spins = 0; spins < 30 && !got_frame; spins++) {
		int rc = next_event(fd, &e, 200);
		if (rc < 0)
			break;
		if (rc == 1 && e.obj == FRAME_CB_ID && e.op == B1D_EV_CALLBACK_DONE)
			got_frame = 1;
		if (rc == 1 && e.obj == B1D_OBJ_DISPLAY && e.op == B1D_EV_DISPLAY_ERROR)
			break;
	}
	marker(got_frame ? "M47-DSP: ok commit-frame\n" : "M47-DSP: fail commit-frame\n");
	if (!got_frame)
		return 1;

	/* 4: SYNC roundtrip */
	uint32_t scb[1] = {SYNC_CB_ID};
	send_req(fd, B1D_OBJ_DISPLAY, B1D_REQ_DISPLAY_SYNC, scb, 1);
	int got_sync = 0;
	for (int spins = 0; spins < 15 && !got_sync; spins++) {
		int rc = next_event(fd, &e, 200);
		if (rc < 0)
			break;
		if (rc == 1 && e.obj == SYNC_CB_ID && e.op == B1D_EV_CALLBACK_DONE)
			got_sync = 1;
	}
	marker(got_sync ? "M47-DSP: ok sync\n" : "M47-DSP: fail sync\n");
	if (!got_sync)
		return 1;

	/* 5: a second independent GUI client and framebuffer checksum. */
	struct b1gui_window second;
	if (b1gui_connect(&second) ||
	    b1gui_create_window(&second, 128, 80, "M47 two")) {
		marker("M47-DSP: fail two-clients\n");
		return 1;
	}
	for (unsigned i = 0; i < second.width * second.height; i++)
		second.pixels[i] = 0x00804070u ^ (i * 17u);
	b1gui_present(&second, 0, 0, second.width, second.height);
	marker("M47-DSP: ok two-clients\n");

	uint32_t checksum_cb[1] = {CHECKSUM_CB_ID};
	send_req(fd, B1D_OBJ_DISPLAY, B1D_REQ_DISPLAY_CHECKSUM, checksum_cb, 1);
	int got_checksum = 0;
	for (int spins = 0; spins < 20 && !got_checksum; spins++) {
		int rc = next_event(fd, &e, 100);
		if (rc == 1 && e.obj == CHECKSUM_CB_ID &&
		    e.op == B1D_EV_CALLBACK_VALUE && e.nargs && e.args[0] != 0)
			got_checksum = 1;
	}
	marker(got_checksum ? "M47-DSP: ok checksum\n"
	                    : "M47-DSP: fail checksum\n");
	if (!got_checksum)
		return 1;

	/* 6: seat input. We placed our window (step 3) centered on the cursor's
	 * start position, so the kernel injector's periodic burst (REL motion +
	 * BTN_LEFT press) is delivered to us as pointer enter/motion plus button
	 * & focus events. next_event() latches those signals across every wait
	 * loop (g_seen_*), so events consumed while waiting for earlier callbacks
	 * still count here. */
	int saw_alt_tab = 0;
	for (int spins = 0; spins < 140 &&
	                    !(g_seen_pointer && g_seen_button && g_seen_focus &&
	                      saw_alt_tab);
	     spins++) {
		next_event(fd, &e, 100); /* latches g_seen_* internally */
		struct b1gui_event ge;
		while (b1gui_next_event(&second, &ge, 0) == 1)
			if (ge.object_id == B1D_OBJ_SEAT &&
			    ge.opcode == B1D_EV_SEAT_FOCUS_ENTER &&
			    ge.nargs && ge.args[0] == second.surface_id)
				saw_alt_tab = 1;
	}
	marker(g_seen_pointer ? "M47-DSP: ok pointer\n" : "M47-DSP: fail pointer\n");
	marker(g_seen_button && g_seen_focus ? "M47-DSP: ok button-focus\n"
	                                     : "M47-DSP: fail button-focus\n");
	marker(saw_alt_tab ? "M47-DSP: ok alt-tab\n"
	                   : "M47-DSP: fail alt-tab\n");

	/* 7: clean shutdown of the server */
	b1gui_destroy(&second);
	send_req(fd, B1D_OBJ_DISPLAY, B1D_REQ_DISPLAY_SHUTDOWN, 0, 0);
	close(fd);
	syscall(SYS_SHMDT, (long)(uintptr_t)pix, 0, 0);
	syscall(SYS_SHMCTL, shmid, 0, 0);

	int ok = g_seen_pointer && g_seen_button && g_seen_focus && saw_alt_tab;
	marker("M47-DSP: done\n");
	return ok ? 0 : 1;
}
