/* The socket layer under concurrent load, over every transport in the image.
 *
 * Loopback rather than the wire: the question is whether the kernel's socket,
 * buffer and poll code holds up when many connections are open at once, and a
 * virtio link only adds a driver's timing to that. The driver has its own
 * exercise in the smoke suite.
 *
 * Every payload carries a pattern derived from the connection number, so a
 * message delivered to the wrong socket is reported as such — the failure that
 * matters most here and the one a plain echo test cannot see.
 */
#include "stress.h"
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define MSG_LEN 4096

static volatile int g_fail;
static unsigned long long g_msgs, g_bytes;
static unsigned long long g_deadline;

static int fail(const char *what)
{
	fprintf(stderr, "NETSTRESS: FAIL %s: %s\n", what, strerror(errno));
	g_fail = 1;
	return -1;
}

static int read_full(int fd, void *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t n = read(fd, (char *)buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, (const char *)buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

/* ── TCP over loopback ───────────────────────────────────────────────────── */

static int tcp_port;

static void *tcp_server(void *arg)
{
	int lfd = (int)(intptr_t)arg;
	unsigned char buf[MSG_LEN];

	for (;;) {
		int fd = accept(lfd, 0, 0);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		/* Echo until the client hangs up. The server does not interpret the
		 * payload — the client is the one that knows what it sent. */
		for (;;) {
			ssize_t n = read(fd, buf, sizeof(buf));
			if (n <= 0)
				break;
			if (write_full(fd, buf, (size_t)n) != 0)
				break;
		}
		close(fd);
	}
	return 0;
}

static int tcp_round(unsigned long conn, int msgs)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return fail("socket");

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)tcp_port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		close(fd);
		return fail("connect");
	}
	/* Nagle off: the test sends small messages and waits for each one back,
	 * which is precisely the pattern Nagle delays. */
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	unsigned char *out = malloc(MSG_LEN), *in = malloc(MSG_LEN);
	if (!out || !in) {
		free(out); free(in); close(fd);
		return 0;
	}
	int rc = 0;
	for (int i = 0; i < msgs; i++) {
		unsigned long owner = conn * 7919UL + (unsigned long)i;
		pat_fill(out, owner, MSG_LEN);
		if (write_full(fd, out, MSG_LEN) != 0) { rc = fail("send"); break; }
		if (read_full(fd, in, MSG_LEN) != 0) { rc = fail("recv"); break; }
		size_t bad = pat_check(in, owner, MSG_LEN);
		if (bad != (size_t)-1) {
			fprintf(stderr, "NETSTRESS: FAIL tcp conn %lu msg %d off %zu: 0x%02x want 0x%02x\n",
			        conn, i, bad, in[bad], pat(owner, bad));
			g_fail = 1;
			rc = -1;
			break;
		}
		__atomic_add_fetch(&g_msgs, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&g_bytes, MSG_LEN * 2, __ATOMIC_RELAXED);
	}
	free(out);
	free(in);
	close(fd);
	return rc;
}

struct tcp_arg { unsigned long conn; int msgs; };

static void *tcp_client_thread(void *arg)
{
	struct tcp_arg *a = arg;
	for (int i = 0; i < 4 && !g_fail && now_ms() < g_deadline; i++)
		if (tcp_round(a->conn * 4 + (unsigned long)i, a->msgs) != 0)
			break;
	return 0;
}

/* ── UDP over loopback ───────────────────────────────────────────────────── */

static int udp_round(int datagrams)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	int c = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0 || c < 0) {
		if (s >= 0) close(s);
		if (c >= 0) close(c);
		return fail("udp socket");
	}
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		close(s); close(c);
		return fail("udp bind");
	}
	socklen_t sl = sizeof(sa);
	getsockname(s, (struct sockaddr *)&sa, &sl);

	unsigned char out[1400], in[1400];
	int rc = 0;
	for (int i = 0; i < datagrams; i++) {
		unsigned long owner = 0xda7a0000UL + (unsigned long)i;
		pat_fill(out, owner, sizeof(out));
		if (sendto(c, out, sizeof(out), 0, (struct sockaddr *)&sa, sl) < 0) {
			rc = fail("sendto");
			break;
		}
		/* A datagram may legitimately be dropped when the receive buffer is
		 * full, so a timeout is not a failure — receiving the wrong bytes is. */
		struct pollfd pfd = { .fd = s, .events = POLLIN };
		if (poll(&pfd, 1, 2000) <= 0)
			continue;
		ssize_t n = recv(s, in, sizeof(in), 0);
		if (n < 0) { rc = fail("recv udp"); break; }
		if ((size_t)n != sizeof(out)) {
			fprintf(stderr, "NETSTRESS: FAIL udp datagram truncated: %zd of %zu\n", n, sizeof(out));
			g_fail = 1; rc = -1; break;
		}
		size_t bad = pat_check(in, owner, sizeof(out));
		if (bad != (size_t)-1) {
			fprintf(stderr, "NETSTRESS: FAIL udp payload off %zu: 0x%02x want 0x%02x\n",
			        bad, in[bad], pat(owner, bad));
			g_fail = 1; rc = -1; break;
		}
		__atomic_add_fetch(&g_msgs, 1, __ATOMIC_RELAXED);
	}
	close(s);
	close(c);
	return rc;
}

/* ── AF_UNIX, blocking and not ───────────────────────────────────────────── */

static int unix_round(int msgs)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return fail("socketpair");

	/* Non-blocking on the reader: a recv that ignores O_NONBLOCK and parks is
	 * exactly the fault that stalled a browser's IPC, and it shows up here as
	 * a read that blocks instead of returning EAGAIN. */
	fcntl(sv[0], F_SETFL, O_NONBLOCK);
	unsigned char out[1024], in[1024];
	int rc = 0;

	ssize_t n = recv(sv[0], in, sizeof(in), MSG_DONTWAIT);
	if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
		fprintf(stderr, "NETSTRESS: FAIL empty non-blocking recv returned %zd (errno %d)\n", n, errno);
		g_fail = 1;
		close(sv[0]); close(sv[1]);
		return -1;
	}

	for (int i = 0; i < msgs; i++) {
		unsigned long owner = 0x0417C0DEUL + (unsigned long)i;
		pat_fill(out, owner, sizeof(out));
		if (write_full(sv[1], out, sizeof(out)) != 0) { rc = fail("unix write"); break; }
		struct pollfd pfd = { .fd = sv[0], .events = POLLIN };
		if (poll(&pfd, 1, 2000) <= 0) {
			fprintf(stderr, "NETSTRESS: FAIL unix poll timed out with data written\n");
			g_fail = 1; rc = -1; break;
		}
		if (read_full(sv[0], in, sizeof(in)) != 0) { rc = fail("unix read"); break; }
		size_t bad = pat_check(in, owner, sizeof(in));
		if (bad != (size_t)-1) {
			fprintf(stderr, "NETSTRESS: FAIL unix payload off %zu: 0x%02x want 0x%02x\n",
			        bad, in[bad], pat(owner, bad));
			g_fail = 1; rc = -1; break;
		}
		__atomic_add_fetch(&g_msgs, 1, __ATOMIC_RELAXED);
	}
	close(sv[0]);
	close(sv[1]);
	return rc;
}

int main(void)
{
	g_deadline = now_ms() + budget_ms();
	int conns = (int)env_long("SOAK_CONNS", 8);
	int msgs = (int)scaled(64);

	printf("NETSTRESS: start %d connections, %d messages each\n", conns, msgs);
	fflush(stdout);
	unsigned long long t0 = now_ms();

	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0) { fail("listen socket"); return 1; }
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { fail("bind"); return 1; }
	socklen_t sl = sizeof(sa);
	getsockname(lfd, (struct sockaddr *)&sa, &sl);
	tcp_port = ntohs(sa.sin_port);
	if (listen(lfd, 64) != 0) { fail("listen"); return 1; }

	pthread_t srv;
	if (pthread_create(&srv, 0, tcp_server, (void *)(intptr_t)lfd) != 0) {
		fail("server thread");
		return 1;
	}
	pthread_detach(srv);

	pthread_t cl[32];
	struct tcp_arg args[32];
	if (conns > 32)
		conns = 32;
	int made = 0;
	for (int i = 0; i < conns; i++) {
		args[i].conn = (unsigned long)i;
		args[i].msgs = msgs;
		if (pthread_create(&cl[i], 0, tcp_client_thread, &args[i]) != 0)
			break;
		made++;
	}
	for (int i = 0; i < made; i++)
		pthread_join(cl[i], 0);

	if (!g_fail)
		udp_round(msgs);
	if (!g_fail)
		unix_round(msgs);

	close(lfd);
	unsigned long long ms = now_ms() - t0;
	if (g_fail) {
		printf("NETSTRESS: FAIL after %llu messages, ms=%llu\n", g_msgs, ms);
		return 1;
	}
	printf("NETSTRESS: ok %llu messages, %llu KiB, %d connections, ms=%llu\n",
	       g_msgs, g_bytes / 1024, made * 4, ms);
	return 0;
}
