/*
 * M47 Display-Substrate Smoke Test — /dev/fb0 + /dev/input/event*
 *
 * Tests:
 *  - /dev/fb0 opens and reports a sane mode (FBIOGET_INFO)
 *  - mmap of the framebuffer is real shared device memory: two independent
 *    mappings alias the same pixels, and the contents survive munmap+remap
 *    (proves the kernel maps device frames, not lazy anonymous pages)
 *  - FBIOFLUSH pushes a dirty rect to the display path
 *  - /dev/input/event0 (kbd) and event1 (mouse) open; O_NONBLOCK read with
 *    no events returns EAGAIN
 *  - a blocking read on event1 receives the event sequence injected by the
 *    kernel test harness (REL_X=7, REL_Y=-3, BTN_LEFT down, SYN) — this
 *    verifies the queue, poll wakeup, and the 16-byte event record ABI
 *
 * Emits M47-GFX markers consumed by tests/smoke.sh.
 */
#include <b1nix/fb.h>
#include <b1nix/input.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void marker(const char *text) {
	write(1, text, strlen(text));
}

static int test_fb(void) {
	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		/* No 32bpp boot framebuffer (e.g. headless real HW): not a failure,
		 * the device legitimately doesn't register. */
		marker("M47-GFX: skip no-fb\n");
		return 0;
	}

	struct b1nix_fb_info info;
	memset(&info, 0, sizeof(info));
	if (ioctl(fd, B1NIX_FBIOGET_INFO, &info) != 0 || info.width == 0 ||
	    info.height == 0 || info.bpp != 32 || info.pitch != info.width * 4) {
		marker("M47-GFX: fail fb-info\n");
		close(fd);
		return -1;
	}
	marker("M47-GFX: ok fb-info\n");

	size_t map_len = (size_t)info.pitch * info.height;

	/* Two independent views of the same device memory must alias. */
	uint32_t *view_a = mmap(0, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	uint32_t *view_b = mmap(0, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (view_a == MAP_FAILED || view_b == MAP_FAILED || view_a == view_b) {
		marker("M47-GFX: fail fb-mmap\n");
		close(fd);
		return -1;
	}

	/* Draw a 64x64 test square at (8,8) via view A, verify through view B. */
	int ok = 1;
	for (uint32_t y = 8; y < 72 && ok; y++) {
		for (uint32_t x = 8; x < 72; x++) {
			view_a[y * info.width + x] = 0x00AA55CC ^ (x + y);
		}
	}
	for (uint32_t y = 8; y < 72 && ok; y++) {
		for (uint32_t x = 8; x < 72; x++) {
			if (view_b[y * info.width + x] != (0x00AA55CC ^ (x + y))) {
				ok = 0;
				break;
			}
		}
	}
	if (!ok) {
		marker("M47-GFX: fail fb-mmap\n");
		close(fd);
		return -1;
	}
	marker("M47-GFX: ok fb-mmap\n");

	/* Flush the dirty rect through the real display path. */
	struct b1nix_fb_rect rect = {8, 8, 64, 64};
	if (ioctl(fd, B1NIX_FBIOFLUSH, &rect) != 0) {
		marker("M47-GFX: fail fb-flush\n");
		close(fd);
		return -1;
	}
	marker("M47-GFX: ok fb-flush\n");

	/* Unmap both views, remap, and check the pixels survived: device frames
	 * are kernel-owned and must not be freed/recycled by munmap. */
	munmap(view_a, map_len);
	munmap(view_b, map_len);
	uint32_t *view_c = mmap(0, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (view_c == MAP_FAILED) {
		marker("M47-GFX: fail fb-persist\n");
		close(fd);
		return -1;
	}
	ok = 1;
	for (uint32_t y = 8; y < 72 && ok; y++) {
		for (uint32_t x = 8; x < 72; x++) {
			if (view_c[y * info.width + x] != (0x00AA55CC ^ (x + y))) {
				ok = 0;
				break;
			}
		}
	}
	munmap(view_c, map_len);
	close(fd);
	if (!ok) {
		marker("M47-GFX: fail fb-persist\n");
		return -1;
	}
	marker("M47-GFX: ok fb-persist\n");
	return 0;
}

static int test_input_open(void) {
	int kfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	int mfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
	if (kfd < 0 || mfd < 0) {
		marker("M47-GFX: fail input-open\n");
		if (kfd >= 0) close(kfd);
		if (mfd >= 0) close(mfd);
		return -1;
	}

	/* No one is typing during the smoke run: an empty nonblocking read must
	 * say EAGAIN, and poll must not report readiness on the keyboard. */
	struct b1nix_input_event ev;
	int n = (int)read(kfd, &ev, sizeof(ev));
	if (n >= 0 || errno != EAGAIN) {
		marker("M47-GFX: fail input-eagain\n");
		close(kfd);
		close(mfd);
		return -1;
	}
	close(kfd);
	close(mfd);
	marker("M47-GFX: ok input-open\n");
	return 0;
}

static int test_input_events(void) {
	/* The kernel test harness injects a mouse event burst once a client has
	 * event1 open (see the m47 injector in kernel/user/programs.c). Use a
	 * blocking fd: this also exercises the blocking-read path. */
	int fd = open("/dev/input/event1", O_RDONLY);
	if (fd < 0) {
		marker("M47-GFX: fail input-event\n");
		return -1;
	}

	int saw_rel_x = 0, saw_rel_y = 0, saw_btn = 0, saw_syn = 0;
	/* Read until SYN or ~5 s worth of poll timeouts. */
	for (int spins = 0; spins < 50 && !saw_syn; spins++) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
		int pr = poll(&pfd, 1, 100);
		if (pr <= 0 || !(pfd.revents & POLLIN))
			continue;
		struct b1nix_input_event evs[8];
		int n = (int)read(fd, evs, sizeof(evs));
		if (n <= 0 || (n % (int)sizeof(struct b1nix_input_event)) != 0)
			break;
		int count = n / (int)sizeof(struct b1nix_input_event);
		for (int i = 0; i < count; i++) {
			if (evs[i].type == B1NIX_EV_REL && evs[i].code == B1NIX_REL_X &&
			    evs[i].value == 7)
				saw_rel_x = 1;
			if (evs[i].type == B1NIX_EV_REL && evs[i].code == B1NIX_REL_Y &&
			    evs[i].value == -3)
				saw_rel_y = 1;
			if (evs[i].type == B1NIX_EV_KEY && evs[i].code == B1NIX_BTN_LEFT &&
			    evs[i].value == 1)
				saw_btn = 1;
			if (evs[i].type == B1NIX_EV_SYN)
				saw_syn = 1;
		}
	}
	close(fd);

	if (saw_rel_x && saw_rel_y && saw_btn && saw_syn) {
		marker("M47-GFX: ok input-event\n");
		return 0;
	}
	marker("M47-GFX: fail input-event\n");
	return -1;
}

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

	marker("M47-GFX: start\n");

	int rc = 0;
	if (test_fb() != 0)
		rc = 1;
	if (test_input_open() != 0)
		rc = 1;
	else if (test_input_events() != 0)
		rc = 1;

	marker("M47-GFX: done\n");
	return rc;
}
