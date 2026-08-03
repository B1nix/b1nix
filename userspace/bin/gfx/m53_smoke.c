/* m53_smoke.c — M53 "browser platform" driver.
 *
 * The NetSurf render tests are all one binary (/bin/netsurf-fb) invoked with
 * different frontends and URLs; the browser's own -T/-I self-test prints the
 * markers (M53-NS / M53-FB / M53-INPUT / M53-WEB / M53-HTTPS / M53-WL — see
 * framebuffer_test_run in tools/ports/build-netsurf-fb.sh). This driver is
 * what runs them in the right order with the right servers alive, a job that
 * used to belong to the in-kernel dispatcher and was dropped when the test
 * runner moved to Ring 3.
 *
 * Ordering matters: the loopback HTTP/HTTPS servers must be listening before
 * the browser fetches from them, and each must be torn down afterwards so the
 * next one gets a free port.
 */

#include <b1nix/input.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define NSFB "/bin/netsurf-fb"
#define TEST_URL "file:///netsurf/test.html"

static void emit(const char *s) { write(1, s, strlen(s)); }

/* One browser run is a whole page load + layout + paint; on a loaded smoke
 * host that can take a while, but it must never outlive the host's 120s
 * serial-silence watchdog by itself. */
#define CHILD_TIMEOUT_DS 900 /* 90s, in 100ms ticks */

static pid_t spawn(char *const argv[]) {
	pid_t pid = fork();
	if (pid == 0) {
		char *envp[] = {(char *)"PATH=/bin", (char *)"HOME=/root", NULL};
		execve(argv[0], argv, envp);
		_exit(127);
	}
	return pid;
}

/* Wait with a watchdog: a browser run that wedges must cost only its own
 * markers, not every test after it in this instance. */
static int wait_bounded(pid_t pid, const char *label) {
	int status = 0;
	for (int ds = 0; ds < CHILD_TIMEOUT_DS; ds++) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid)
			return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
		if (r < 0)
			return -1;
		usleep(100000);
	}
	char msg[96];
	snprintf(msg, sizeof(msg), "M53-SMOKE: timeout %s\n", label);
	emit(msg);
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	return -1;
}

static void run_browser(const char *label, char *const argv[]) {
	pid_t pid = spawn(argv);
	if (pid < 0) {
		char msg[96];
		snprintf(msg, sizeof(msg), "M53-SMOKE: spawn-fail %s\n", label);
		emit(msg);
		return;
	}
	wait_bounded(pid, label);
}

/* Starts a loopback server and gives it a moment to bind+listen. The browser
 * tolerates a slow start (it re-navigates), but connecting to a closed port
 * costs a full retry cycle, so don't race it needlessly. */
static pid_t start_server(const char *path) {
	char *argv[] = {(char *)path, NULL};
	pid_t pid = spawn(argv);
	if (pid > 0)
		usleep(1000000);
	return pid;
}

static void stop_server(pid_t pid) {
	if (pid <= 0)
		return;
	kill(pid, SIGTERM);
	int status = 0;
	for (int ds = 0; ds < 50; ds++) {
		if (waitpid(pid, &status, WNOHANG) == pid)
			return;
		usleep(100000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
}

/* Feeds a repeating move / click / key sequence into the input devices for as
 * long as it runs. NetSurf's fbtk loop only reports what it actually receives,
 * so this drives the real evdev path (kernel input_write injection) rather
 * than any test-only shortcut. Runs as a child; the parent kills it once the
 * browser exits. */
static void inject_loop(void) {
	int mouse = open("/dev/input/event1", O_WRONLY);
	int kbd = open("/dev/input/event0", O_WRONLY);
	if (mouse < 0 && kbd < 0)
		return;
	struct b1nix_input_event ev[8];
	for (;;) {
		if (mouse >= 0) {
			memset(ev, 0, sizeof(ev));
			ev[0].type = B1NIX_EV_REL; ev[0].code = B1NIX_REL_X; ev[0].value = 7;
			ev[1].type = B1NIX_EV_REL; ev[1].code = B1NIX_REL_Y; ev[1].value = -3;
			ev[2].type = B1NIX_EV_KEY; ev[2].code = B1NIX_BTN_LEFT; ev[2].value = 1;
			ev[3].type = B1NIX_EV_SYN;
			write(mouse, ev, 4 * sizeof(ev[0]));
			memset(ev, 0, sizeof(ev));
			ev[0].type = B1NIX_EV_KEY; ev[0].code = B1NIX_BTN_LEFT; ev[0].value = 0;
			ev[1].type = B1NIX_EV_SYN;
			write(mouse, ev, 2 * sizeof(ev[0]));
		}
		if (kbd >= 0) {
			/* PS/2 set-1 scancodes: 'a' press + release. */
			memset(ev, 0, sizeof(ev));
			ev[0].type = B1NIX_EV_KEY; ev[0].code = 0x1e; ev[0].value = 1;
			ev[1].type = B1NIX_EV_SYN;
			ev[2].type = B1NIX_EV_KEY; ev[2].code = 0x1e; ev[2].value = 0;
			ev[3].type = B1NIX_EV_SYN;
			write(kbd, ev, 4 * sizeof(ev[0]));
		}
		usleep(50000);
	}
}

int main(void) {
	if (access(NSFB, X_OK) != 0) {
		emit("M53-SMOKE: unsupported netsurf-missing\n");
		return 0;
	}

	/* 1. Off-screen RAM surface, local file: the whole ported NetSurf lib
	 *    chain (parserutils/hubbub/libcss/libdom + the image decoders). */
	{
		char *argv[] = {(char *)NSFB, (char *)"-f", (char *)"ram",
		                (char *)"-w", (char *)"800", (char *)"-h",
		                (char *)"600", (char *)"-T", (char *)TEST_URL, NULL};
		run_browser("ram", argv);
	}

	/* 2. On-screen: paint straight to the real /dev/fb0 surface. */
	{
		char *argv[] = {(char *)NSFB, (char *)"-f", (char *)"b1nix",
		                (char *)"-T", (char *)TEST_URL, NULL};
		run_browser("fb", argv);
	}

	/* 3. Interactive: the same on-screen surface driven by injected keyboard
	 *    and mouse events written into /dev/input/event*. */
	{
		pid_t inj = fork();
		if (inj == 0) {
			inject_loop();
			_exit(0);
		}
		char *argv[] = {(char *)NSFB, (char *)"-f", (char *)"b1nix",
		                (char *)"-I", (char *)TEST_URL, NULL};
		run_browser("input", argv);
		if (inj > 0) {
			int st = 0;
			kill(inj, SIGKILL);
			waitpid(inj, &st, 0);
		}
	}

	/* 4. Real web access: fetch the page from a loopback HTTP server over a
	 *    genuine TCP connection through libcurl. */
	{
		pid_t httpd = start_server("/bin/m53_httpd");
		char *argv[] = {(char *)NSFB, (char *)"-f", (char *)"ram",
		                (char *)"-w", (char *)"800", (char *)"-h",
		                (char *)"600", (char *)"-T",
		                (char *)"http://127.0.0.1:8080/", NULL};
		run_browser("web", argv);
		stop_server(httpd);
	}

	/* 5. The same over TLS 1.2, with the server certificate verified against
	 *    the shipped test CA. */
	{
		pid_t httpsd = start_server("/bin/m53_httpsd");
		char *argv[] = {(char *)NSFB,
		                /* nsoption args (--name=value) must precede getopt flags. */
		                (char *)"--ca_bundle=/etc/tls-test/ca.pem",
		                (char *)"-f", (char *)"ram", (char *)"-w",
		                (char *)"800", (char *)"-h", (char *)"600",
		                (char *)"-T", (char *)"https://127.0.0.1:8443/", NULL};
		run_browser("https", argv);
		stop_server(httpsd);
	}

	/* 6. Windowed: NetSurf as a client of the display server. Only meaningful
	 *    when displayd is up (the graphics instance); the frontend fails fast
	 *    and prints its own marker otherwise. */
	{
		char *argv[] = {(char *)NSFB, (char *)"-f", (char *)"displayd",
		                (char *)"-T", (char *)TEST_URL, NULL};
		run_browser("wayland", argv);
	}

	/* 7. Public-internet HTTPS against the shipped Mozilla CA bundle. The
	 *    browser marks this one optional and prints "unsupported" when the
	 *    usernet has no off-link route, so the offline smoke stays green. */
	{
		char *argv[] = {(char *)NSFB, (char *)"-f", (char *)"ram",
		                (char *)"-w", (char *)"800", (char *)"-T",
		                (char *)"https://example.com/", NULL};
		run_browser("ext-https", argv);
	}

	return 0;
}
