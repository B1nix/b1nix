/*
 * displayd_main.c — globals, cursor bitmap, main loop, clock, spawn.
 */
#include "displayd.h"
#include "font8x8.h"
#include "xkb_keymap_us.h"
#include <sched.h>

/* ── globals ── */
struct dclient clients[MAX_CLIENTS];
struct dsurface surfaces[MAX_SURFACES];
struct dbuffer buffers[MAX_BUFFERS];
struct dtoplevel toplevels[MAX_TOPLEVELS];
struct wobject wobjects[MAX_WOBJECTS];
struct wpool wpools[MAX_WPOOLS];
struct app_menu app_menus[MAX_TOPLEVELS];
int zorder[MAX_SURFACES];
int zcount;

int fb_fd = -1;
uint32_t *fb;
uint32_t scr_w, scr_h;
int ev_fds[3] = {-1, -1, -1};
int listen_fd = -1;
int running = 1;
unsigned frame_serial;
int surfaces_created;

char clock_hhmm[10] = "--:--";
int clock_last_min = -1;
int clock_24h = 1;

enum panel_menu open_menu;
int menu_hover = -1;
int desktop_menu_x, desktop_menu_y;

int px, py;
int enter_slot = -1;
int focus_slot = -1;
int drag_slot = -1;
int resize_slot = -1;
uint32_t resize_edges;
int resize_ox, resize_oy, resize_ow, resize_oh, resize_wx, resize_wy;
int left_alt;
uint32_t input_serial;

uint32_t kbd_mods_depressed;
uint32_t kbd_mods_locked;

int ptr_acc_dx, ptr_acc_dy;
int ptr_abs_x, ptr_abs_y;
int ptr_have_abs;
int ptr_moved;

int tch_down, tch_x, tch_y, tch_client = -1;
uint32_t tch_surface_id;

int btn_on_decoration;
int btn_on_panel;

/* clipboard / DnD */
int sel_client = -1;
uint32_t sel_source;
char sel_mime[64];
uint32_t server_id_next = 0xff000000u;
int dnd_active;
int dnd_src_client = -1;
uint32_t dnd_source;
char dnd_mime[64];
uint32_t dnd_src_actions;
int dnd_target_client = -1;
uint32_t dnd_target_offer;
uint32_t dnd_accepted_action;
int dnd_in_surface = -1;

/* ── cursor bitmap ── */
const char *const cursor_bitmap[CURSOR_H] = {
	"B",
	"BB",
	"BWB",
	"BWWB",
	"BWWWB",
	"BWWWWB",
	"BWWWWWB",
	"BWWWWWWB",
	"BWWWWWWWB",
	"BWWWWWWWWB",
	"BWWWWWBBBBB",
	"BWWBWWB",
	"BWB BWWB",
	"BB  BWWB",
	"B    BWWB",
	"     BWWB",
	"      BWWB",
	"      BWWB",
	"       BB",
};

/* ── helpers ── */
void spawn_app(const char *path, const char *arg) {
	pid_t pid = fork();
	if (pid == 0) {
		if (arg)
			execlp(path, path, arg, (char *)0);
		else
			execlp(path, path, (char *)0);
		_exit(127);
	}
}

int update_clock(void) {
	time_t now = time(0);
	struct tm tmv;
	struct tm *t = localtime_r(&now, &tmv);
	if (!t)
		return 0;
	if (t->tm_min == clock_last_min && clock_last_min >= 0)
		return 0;
	clock_last_min = t->tm_min;
	int h = t->tm_hour, m = t->tm_min;
	if (clock_24h) {
		clock_hhmm[0] = (char)('0' + (h / 10) % 10);
		clock_hhmm[1] = (char)('0' + h % 10);
		clock_hhmm[2] = ':';
		clock_hhmm[3] = (char)('0' + (m / 10) % 10);
		clock_hhmm[4] = (char)('0' + m % 10);
		clock_hhmm[5] = 0;
	} else {
		int is_pm = h >= 12;
		int h12 = h % 12;
		if (h12 == 0) h12 = 12;
		clock_hhmm[0] = (char)('0' + (h12 / 10) % 10);
		clock_hhmm[1] = (char)('0' + h12 % 10);
		clock_hhmm[2] = ':';
		clock_hhmm[3] = (char)('0' + (m / 10) % 10);
		clock_hhmm[4] = (char)('0' + m % 10);
		clock_hhmm[5] = ' ';
		clock_hhmm[6] = is_pm ? 'P' : 'A';
		clock_hhmm[7] = 'M';
		clock_hhmm[8] = 0;
	}
	return 1;
}

static void out(const char *s) { write(1, s, strlen(s)); }
static void out_dec(unsigned v) {
	char b[12];
	int i = 12;
	b[--i] = 0;
	do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v && i);
	out(&b[i]);
}

void ping_clients(void) {
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (!clients[i].used)
			continue;
		struct wobject *wm = wobject_type_find(i, WOBJ_XDG_WM_BASE);
		if (!wm)
			continue;
		if (clients[i].ping_pending) {
			/*
			 * A missed pong means the client is busy, not that it is gone.
			 * Anything running a long synchronous software-GL render — OSMesa
			 * on softpipe is seconds per frame — cannot pump its event queue
			 * while it renders, and there is no timeout that separates "slow"
			 * from "dead": raising the allowance only moves the point at which
			 * a slower machine kills a live client mid-render, which is how
			 * this failed after every request the client made next returned
			 * ENOTCONN.
			 *
			 * So the ping is a liveness *probe*, not a verdict. The connection
			 * is closed by the paths that can actually tell it is gone — a
			 * send that fails, or a read that reports EOF. A client that never
			 * answers stays connected and is counted, which is visible in the
			 * log without costing it its window.
			 */
			if (++clients[i].ping_misses == 8) {
				out("displayd: client ");
				out_dec((unsigned)i);
				out(" unresponsive (still connected)\n");
			}
			continue;
		}
		clients[i].ping_misses = 0;
		clients[i].ping_serial = ++frame_serial;
		clients[i].ping_pending = 1;
		send_msg(i, wm->id, 0, &clients[i].ping_serial, 1);
	}
}

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0) {
		out("displayd: no /dev/fb0\n");
		return 1;
	}
	struct b1nix_fb_info info;
	if (ioctl(fb_fd, B1NIX_FBIOGET_INFO, &info) != 0) {
		out("displayd: FBIOGET_INFO failed\n");
		return 1;
	}
	scr_w = info.width;
	scr_h = info.height;
	fb = mmap(0, (size_t)info.pitch * scr_h, PROT_READ | PROT_WRITE,
	          MAP_SHARED, fb_fd, 0);
	if (fb == MAP_FAILED) {
		out("displayd: fb mmap failed\n");
		return 1;
	}

	ev_fds[0] = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	ev_fds[1] = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
	ev_fds[2] = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);

	px = (int)scr_w / 2;
	py = (int)scr_h / 2;
	long last_ping = (long)time(0);
	update_clock();
	composite_rect(0, 0, (int)scr_w, (int)scr_h);

	out("displayd: ready ");
	out_dec(scr_w);
	out("x");
	out_dec(scr_h);
	out("\n");

	/* Publish the socket only after framebuffer and compositor state are ready. */
	unlink(WAYLAND_SOCKET_PATH);
	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		out("displayd: socket failed\n");
		return 1;
	}
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, WAYLAND_SOCKET_PATH);
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(listen_fd, MAX_CLIENTS) < 0) {
		out("displayd: bind/listen failed\n");
		return 1;
	}
	fcntl(listen_fd, F_SETFL, O_NONBLOCK);

	while (running) {
		/* Drain a queued connector before sleeping so AF_UNIX connect() cannot
		 * wait behind a poll readiness race during compositor startup. */
		accept_client(listen_fd);
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (clients[i].used)
				client_data(i);
		struct pollfd pfds[4 + MAX_CLIENTS];
		pfds[0].fd = listen_fd;
		pfds[0].events = POLLIN;
		pfds[1].fd = ev_fds[0];
		pfds[1].events = POLLIN;
		pfds[2].fd = ev_fds[1];
		pfds[2].events = POLLIN;
		pfds[3].fd = ev_fds[2];
		pfds[3].events = POLLIN;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			pfds[4 + i].fd = clients[i].used ? clients[i].fd : -1;
			pfds[4 + i].events = POLLIN;
		}
		for (int i = 0; i < 4 + MAX_CLIENTS; i++)
			pfds[i].revents = 0;

		int pr = poll(pfds, 4 + MAX_CLIENTS, 500);

		if (update_clock())
			composite_rect(0, 0, (int)scr_w, PANEL_H);

		long now = (long)time(0);
		if (now - last_ping >= 5) {
			last_ping = now;
			ping_clients();
		}

		if (pr < 0)
			continue;

		if (pfds[0].revents & POLLIN)
			accept_client(listen_fd);
		if (pfds[1].revents & POLLIN)
			input_drain(0);
		if (pfds[2].revents & POLLIN)
			input_drain(1);
		if (pfds[3].revents & POLLIN)
			touch_drain();
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (pfds[4 + i].fd >= 0 &&
			    (pfds[4 + i].revents & (POLLIN | POLLHUP)))
				client_data(i);

		/* Reap connections that failed on a write, now that nothing is walking
		 * their state. A client is closed here or by its own hangup — never
		 * because it was slow to answer a ping. */
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (clients[i].used && clients[i].dead)
				client_disconnect(i);
	}

	for (int i = 0; i < MAX_CLIENTS; i++)
		client_disconnect(i);
	unlink(WAYLAND_SOCKET_PATH);
	out("displayd: bye\n");
	return 0;
}
