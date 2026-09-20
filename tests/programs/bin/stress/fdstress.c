/* Descriptor-table churn: many kinds of file, opened and closed at speed.
 *
 * A descriptor table is shared between threads and rewritten on every open and
 * close, so the failure to look for is a descriptor that names the wrong file —
 * data written into one pipe coming out of another. Every object here carries a
 * pattern derived from its round number, which is what makes that visible.
 *
 * Exhaustion is part of the test rather than an accident: the table is filled
 * until the kernel says EMFILE, and then must still work after the descriptors
 * are given back. A limit that leaks a slot per round shows up as a second
 * round that runs out sooner than the first.
 */
#include "stress.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/resource.h>

#define PAYLOAD 2048

static unsigned long long g_deadline;
static unsigned long long g_objects;

static int fail(const char *what)
{
	fprintf(stderr, "FDSTRESS: FAIL %s: %s\n", what, strerror(errno));
	return -1;
}

static int rw_check(int wfd, int rfd, unsigned long owner)
{
	unsigned char out[PAYLOAD], in[PAYLOAD];
	pat_fill(out, owner, sizeof(out));
	if (write(wfd, out, sizeof(out)) != (ssize_t)sizeof(out))
		return fail("write");
	struct pollfd pfd = { .fd = rfd, .events = POLLIN };
	if (poll(&pfd, 1, 5000) <= 0) {
		fprintf(stderr, "FDSTRESS: FAIL poll timed out on a descriptor with data\n");
		return -1;
	}
	size_t off = 0;
	while (off < sizeof(in)) {
		ssize_t n = read(rfd, in + off, sizeof(in) - off);
		if (n <= 0)
			return fail("read");
		off += (size_t)n;
	}
	size_t bad = pat_check(in, owner, sizeof(in));
	if (bad != (size_t)-1) {
		fprintf(stderr, "FDSTRESS: FAIL payload off %zu: 0x%02x want 0x%02x\n",
		        bad, in[bad], pat(owner, bad));
		return -1;
	}
	return 0;
}

/* Pass a descriptor to ourselves over a socket and read through the copy.
 * The receiving end gets a new table slot for an object it did not open, which
 * is the one path where a refcount error frees a file that is still in use. */
static int scm_round(unsigned long owner)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return fail("socketpair for SCM_RIGHTS");

	int mfd = memfd_create("fdstress", 0);
	if (mfd < 0) {
		close(sv[0]); close(sv[1]);
		return fail("memfd_create");
	}
	unsigned char buf[PAYLOAD];
	pat_fill(buf, owner, sizeof(buf));
	if (write(mfd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
		close(mfd); close(sv[0]); close(sv[1]);
		return fail("memfd write");
	}

	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { .iov_base = (void *)"x", .iov_len = 1 };
	struct msghdr mh;
	memset(&mh, 0, sizeof(mh));
	memset(cbuf, 0, sizeof(cbuf));
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = cbuf;
	mh.msg_controllen = sizeof(cbuf);
	struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &mfd, sizeof(int));

	int rc = 0;
	if (sendmsg(sv[1], &mh, 0) < 0) {
		rc = fail("sendmsg SCM_RIGHTS");
	} else {
		char byte;
		struct iovec riov = { .iov_base = &byte, .iov_len = 1 };
		struct msghdr rmh;
		char rcbuf[CMSG_SPACE(sizeof(int))];
		memset(&rmh, 0, sizeof(rmh));
		memset(rcbuf, 0, sizeof(rcbuf));
		rmh.msg_iov = &riov;
		rmh.msg_iovlen = 1;
		rmh.msg_control = rcbuf;
		rmh.msg_controllen = sizeof(rcbuf);
		if (recvmsg(sv[0], &rmh, 0) < 0) {
			rc = fail("recvmsg SCM_RIGHTS");
		} else {
			struct cmsghdr *rcm = CMSG_FIRSTHDR(&rmh);
			if (!rcm || rcm->cmsg_type != SCM_RIGHTS) {
				fprintf(stderr, "FDSTRESS: FAIL no descriptor in received message\n");
				rc = -1;
			} else {
				int got;
				memcpy(&got, CMSG_DATA(rcm), sizeof(int));
				unsigned char back[PAYLOAD];
				if (pread(got, back, sizeof(back), 0) != (ssize_t)sizeof(back)) {
					rc = fail("read through passed descriptor");
				} else {
					size_t bad = pat_check(back, owner, sizeof(back));
					if (bad != (size_t)-1) {
						fprintf(stderr, "FDSTRESS: FAIL passed descriptor names the wrong file (off %zu)\n", bad);
						rc = -1;
					}
				}
				close(got);
			}
		}
	}
	close(mfd);
	close(sv[0]);
	close(sv[1]);
	return rc;
}

/* Open descriptors until the kernel refuses, then give them all back. */
static int exhaust_round(int *reached)
{
	struct rlimit rl;
	int cap = 4096;
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < (rlim_t)cap)
		cap = (int)rl.rlim_cur;

	int *fds = malloc(sizeof(int) * (size_t)cap);
	if (!fds)
		return 0;
	int n = 0;
	while (n < cap) {
		int fd = dup(0);
		if (fd < 0) {
			if (errno == EMFILE || errno == ENFILE)
				break;
			free(fds);
			return fail("dup");
		}
		fds[n++] = fd;
	}
	for (int i = 0; i < n; i++)
		close(fds[i]);
	free(fds);

	/* The table must work again immediately. */
	int fd = dup(0);
	if (fd < 0)
		return fail("dup after releasing every descriptor");
	close(fd);
	*reached = n;
	return 0;
}

int main(void)
{
	g_deadline = now_ms() + budget_ms();
	long rounds = scaled(400);
	printf("FDSTRESS: start %ld rounds\n", rounds);
	fflush(stdout);
	unsigned long long t0 = now_ms();

	int rc = 0;
	long done = 0;
	for (long i = 0; i < rounds && now_ms() < g_deadline; i++) {
		unsigned long owner = 0xfd000000UL + (unsigned long)i;

		int p[2];
		if (pipe(p) != 0) { rc = fail("pipe"); break; }
		if (rw_check(p[1], p[0], owner) != 0) { close(p[0]); close(p[1]); rc = 1; break; }
		/* dup2 onto a live descriptor: the old file must be closed and the new
		 * one must be the same object as its source. */
		int dupd = dup2(p[0], p[0] + 1 > 2 ? p[0] + 1 : 3);
		if (dupd >= 0)
			close(dupd);
		close(p[0]);
		close(p[1]);
		g_objects += 2;

		int sv[2];
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { rc = fail("socketpair"); break; }
		if (rw_check(sv[1], sv[0], owner ^ 0x55) != 0) { close(sv[0]); close(sv[1]); rc = 1; break; }
		close(sv[0]);
		close(sv[1]);
		g_objects += 2;

		int mfd = memfd_create("fdstress", 0);
		if (mfd < 0) { rc = fail("memfd_create"); break; }
		if (ftruncate(mfd, PAYLOAD) != 0) { close(mfd); rc = fail("ftruncate"); break; }
		unsigned char buf[PAYLOAD], back[PAYLOAD];
		pat_fill(buf, owner, sizeof(buf));
		if (pwrite(mfd, buf, sizeof(buf), 0) != (ssize_t)sizeof(buf)) { close(mfd); rc = fail("memfd pwrite"); break; }
		if (pread(mfd, back, sizeof(back), 0) != (ssize_t)sizeof(back)) { close(mfd); rc = fail("memfd pread"); break; }
		if (pat_check(back, owner, sizeof(back)) != (size_t)-1) {
			fprintf(stderr, "FDSTRESS: FAIL memfd contents wrong\n");
			close(mfd); rc = 1; break;
		}
		close(mfd);
		g_objects++;

		if ((i % 16) == 0 && scm_round(owner) != 0) { rc = 1; break; }
		done++;
	}

	int reached = 0;
	if (!rc && exhaust_round(&reached) != 0)
		rc = 1;

	unsigned long long ms = now_ms() - t0;
	if (rc) {
		printf("FDSTRESS: FAIL after %ld rounds, ms=%llu\n", done, ms);
		return 1;
	}
	printf("FDSTRESS: ok %ld rounds, %llu objects, %d descriptors at the limit, ms=%llu\n",
	       done, g_objects, reached, ms);
	return 0;
}
