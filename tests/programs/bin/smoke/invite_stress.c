/* The handshake a multi-process program performs when it starts a helper.
 *
 * Modelled on what Chromium does and gets stuck in: the parent creates a
 * socketpair, puts one end on a fixed descriptor number, launches the helper as
 * a fresh image of itself, and sends an invitation carrying a descriptor. The
 * helper answers on the same socket, the parent replies, and only then does the
 * helper consider itself connected.
 *
 * The browser's helper never read its invitation at all and died on a fifteen
 * second deadline; this asks the same question in seconds and without a
 * browser. Each side bounds its waits, so a broken exchange is reported rather
 * than hung, and each step is checked separately so a failure names the step.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <pthread.h>

#define CHANNEL_FD  3
#define ROUNDS      200
#define WAIT_MS     4000

/* Send `len` bytes, optionally carrying one descriptor. */
static int send_msg(int fd, const void *buf, size_t len, int pass_fd)
{
	struct iovec iov = { (void *)buf, len };
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct msghdr m;

	memset(&m, 0, sizeof(m));
	m.msg_iov = &iov;
	m.msg_iovlen = 1;

	if (pass_fd >= 0) {
		struct cmsghdr *c;
		memset(cbuf, 0, sizeof(cbuf));
		m.msg_control = cbuf;
		m.msg_controllen = sizeof(cbuf);
		c = CMSG_FIRSTHDR(&m);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(c), &pass_fd, sizeof(pass_fd));
	}

	return (int)sendmsg(fd, &m, 0);
}

/* Wait for readiness, then receive; returns bytes, or -1 with errno set.
 * The readiness wait is the half that failed in the browser, so it is done
 * explicitly rather than left to a blocking read. */
static int recv_msg(int fd, void *buf, size_t len, int *got_fd)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	struct iovec iov = { buf, len };
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct msghdr m;
	struct cmsghdr *c;
	int n;

	if (got_fd)
		*got_fd = -1;

	n = poll(&p, 1, WAIT_MS);
	if (n <= 0) {
		errno = n == 0 ? ETIMEDOUT : errno;
		return -1;
	}

	memset(&m, 0, sizeof(m));
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = cbuf;
	m.msg_controllen = sizeof(cbuf);

	n = (int)recvmsg(fd, &m, 0);
	if (n <= 0)
		return -1;

	/* One control message is all this exchange ever sends, so take the first
	 * and skip the walk — CMSG_NXTHDR in this libc compares a size against a
	 * pointer difference and warns. */
	if (got_fd) {
		c = CMSG_FIRSTHDR(&m);
		if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
			memcpy(got_fd, CMSG_DATA(c), sizeof(*got_fd));
	}
	return n;
}

/* ── helper side ─────────────────────────────────────────────────────────── */

static int run_helper(void)
{
	char buf[512];
	int passed = -1;
	int n;

	/* 1. The invitation, carrying a descriptor. */
	n = recv_msg(CHANNEL_FD, buf, sizeof(buf), &passed);
	if (n <= 0) {
		fprintf(stderr, "helper: no invitation (errno %d)\n", errno);
		return 2;
	}
	if (passed < 0) {
		fprintf(stderr, "helper: invitation carried no descriptor\n");
		return 3;
	}

	/* The passed descriptor must be usable, not merely present. */
	if (write(passed, "", 0) < 0 && errno == EBADF) {
		fprintf(stderr, "helper: passed descriptor is not open\n");
		return 4;
	}

	/* 2. Answer, and keep answering: the point is that the exchange sustains,
	 *    not that one message crosses. */
	for (int i = 0; i < ROUNDS; i++) {
		if (send_msg(CHANNEL_FD, "hello", 5, -1) != 5) {
			fprintf(stderr, "helper: send failed at round %d\n", i);
			return 5;
		}
		n = recv_msg(CHANNEL_FD, buf, sizeof(buf), 0);
		if (n <= 0) {
			fprintf(stderr, "helper: no reply at round %d (errno %d)\n", i,
			        errno);
			return 6;
		}
	}
	return 0;
}

/* ── busy threads ────────────────────────────────────────────────────────── */

/* The browser forks its helpers out of a process with dozens of live threads,
 * and fork keeps only the calling one. Everything the others held at that
 * instant — allocator locks, buffers mid-write — is inherited frozen. A
 * handshake that works when forked from a single-threaded parent says nothing
 * about that case, and that case is the one the browser is in. */
static volatile int busy_stop;

static void *busy_thread(void *arg)
{
	(void)arg;
	while (!busy_stop) {
		void *p = malloc(4096);
		if (p) {
			memset(p, 0x5a, 4096);
			free(p);
		}
	}
	return 0;
}

/* ── parent side ─────────────────────────────────────────────────────────── */

/* ── credentials ─────────────────────────────────────────────────────────── */

/* A helper that answers its parent by identity, not just by content.
 *
 * The browser's zygote handshake is checked this way: the receiver asks the
 * kernel to attach the sender's identity to every message and refuses a reply
 * whose identity is missing — "did not receive ping from zygote child" is what
 * that refusal looks like from outside. Content arriving without identity is
 * therefore indistinguishable, to the reader, from nothing arriving at all,
 * which is why this is checked separately from the exchange above.
 *
 * The sender here has replaced its image first, as the zygote has. */
static int run_cred_helper(void)
{
	if (send_msg(CHANNEL_FD, "ping", 4, -1) != 4)
		return 2;
	return 0;
}

static int test_credentials(char *self)
{
	int sv[2];
	pid_t pid;
	char buf[64];
	char cbuf[CMSG_SPACE(sizeof(struct ucred))];
	struct iovec iov = { buf, sizeof(buf) };
	struct msghdr m;
	struct cmsghdr *c;
	struct ucred cred;
	int on = 1;
	int n, status = 0;
	struct pollfd p;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		printf("INVITE-STRESS: FAIL credentials (socketpair errno %d)\n", errno);
		return 1;
	}

	/* The reader asks for identities; nothing is required of the sender. */
	if (setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) != 0) {
		printf("INVITE-STRESS: FAIL credentials (SO_PASSCRED errno %d)\n", errno);
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		printf("INVITE-STRESS: FAIL credentials (fork errno %d)\n", errno);
		return 1;
	}
	if (pid == 0) {
		close(sv[0]);
		if (sv[1] != CHANNEL_FD) {
			dup2(sv[1], CHANNEL_FD);
			close(sv[1]);
		}
		execl(self, self, "cred-helper", (char *)0);
		_exit(127);
	}
	close(sv[1]);

	p.fd = sv[0];
	p.events = POLLIN;
	if (poll(&p, 1, WAIT_MS) <= 0) {
		printf("INVITE-STRESS: FAIL credentials (no message within %d ms)\n",
		       WAIT_MS);
		waitpid(pid, &status, 0);
		return 1;
	}

	memset(&m, 0, sizeof(m));
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = cbuf;
	m.msg_controllen = sizeof(cbuf);

	n = (int)recvmsg(sv[0], &m, 0);
	waitpid(pid, &status, 0);
	close(sv[0]);

	if (n != 4) {
		printf("INVITE-STRESS: FAIL credentials (recvmsg %d, errno %d)\n", n,
		       errno);
		return 1;
	}

	c = CMSG_FIRSTHDR(&m);
	if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_CREDENTIALS) {
		printf("INVITE-STRESS: FAIL credentials (message carried no identity)\n");
		return 1;
	}

	memcpy(&cred, CMSG_DATA(c), sizeof(cred));
	if ((pid_t)cred.pid != pid) {
		printf("INVITE-STRESS: FAIL credentials (identity says pid %d, sender "
		       "was %d)\n", (int)cred.pid, (int)pid);
		return 1;
	}

	printf("INVITE-STRESS: ok credentials (pid %d, uid %d, gid %d)\n",
	       (int)cred.pid, (int)cred.uid, (int)cred.gid);
	return 0;
}

int main(int argc, char **argv)
{
	int sv[2];
	pid_t pid;
	char buf[512];
	int status = 0;
	int rounds = 0;

	if (argc > 1 && strcmp(argv[1], "helper") == 0)
		return run_helper();
	if (argc > 1 && strcmp(argv[1], "cred-helper") == 0)
		return run_cred_helper();

	printf("INVITE-STRESS: start\n");

	/* Fork from a process that is genuinely multi-threaded and busy, because
	 * that is what the browser does. */
	{
		pthread_t th[6];
		int made = 0;
		for (int i = 0; i < 6; i++)
			if (pthread_create(&th[i], 0, busy_thread, 0) == 0)
				made++;
		if (made != 6) {
			printf("INVITE-STRESS: FAIL threads (%d of 6)\n", made);
			return 1;
		}
		/* Let them get into their loops before forking. */
		usleep(100 * 1000);
	}

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		printf("INVITE-STRESS: FAIL socketpair (errno %d)\n", errno);
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		printf("INVITE-STRESS: FAIL fork (errno %d)\n", errno);
		return 1;
	}
	if (pid == 0) {
		close(sv[0]);
		if (sv[1] != CHANNEL_FD) {
			dup2(sv[1], CHANNEL_FD);
			close(sv[1]);
		}
		execl(argv[0], argv[0], "helper", (char *)0);
		_exit(127);
	}
	close(sv[1]);

	/* The invitation goes out while the helper is still starting — the same
	 * order the browser uses, and the order that leaves the message waiting in
	 * the socket for a reader that has not arrived yet. */
	{
		int spare = dup(1);
		if (send_msg(sv[0], "invitation", 10, spare) != 10) {
			printf("INVITE-STRESS: FAIL send-invitation (errno %d)\n", errno);
			return 1;
		}
		close(spare);
	}

	for (rounds = 0; rounds < ROUNDS; rounds++) {
		int n = recv_msg(sv[0], buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		if (send_msg(sv[0], "ack", 3, -1) != 3)
			break;
	}

	waitpid(pid, &status, 0);

	if (rounds != ROUNDS) {
		printf("INVITE-STRESS: FAIL exchange (stalled after %d of %d rounds, "
		       "errno %d)\n", rounds, ROUNDS, errno);
		return 1;
	}
	if (status != 0) {
		printf("INVITE-STRESS: FAIL helper (exit status %d)\n", status);
		return 1;
	}

	busy_stop = 1;
	printf("INVITE-STRESS: ok invitation+%d rounds (forked from 7 threads)\n",
	       rounds);

	if (test_credentials(argv[0]) != 0) {
		printf("INVITE-STRESS: done (1 failed)\n");
		return 1;
	}

	printf("INVITE-STRESS: done (0 failed)\n");
	return 0;
}
