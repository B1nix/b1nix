/*
 * shmshare_smoke — is a shared mapping actually shared between two processes?
 *
 * Every Wayland client on this system depends on one thing: it creates an
 * anonymous file, maps it MAP_SHARED, paints into the mapping, and hands the
 * descriptor to the compositor, which maps the same file and reads what was
 * painted. If those two mappings are not the same memory, everything still
 * *works* — the descriptor passes, the buffer is accepted, frame callbacks
 * fire, the compositor even scans the buffer out — and the screen is blank.
 * That is the failure this exists to catch, and nothing else here catches it:
 * M49's shm-frame check asserts a frame callback arrived, not that any pixel
 * the client wrote was visible to the server.
 *
 * Three shapes, because they fail independently:
 *
 *   1. memfd across fork — the simplest, and the one an inherited descriptor
 *      relies on.
 *   2. memfd across an AF_UNIX SCM_RIGHTS pass, between processes with no
 *      inherited mapping at all. This is exactly what libwayland does.
 *   3. shm_open under /dev/shm, which wlroots' allocator uses in preference to
 *      memfd.
 *
 * In each, the *reader* writes second, so a stale copy in the writer's mapping
 * cannot pass: the parent writes a pattern, the child overwrites it with a
 * different one, and the parent must observe the child's.
 */

#define _GNU_SOURCE 1

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHM_SIZE 8192
/* Big enough that backing it eagerly is unmistakable against any plausible
 * machine, and far larger than this test ever touches. */
#define SPARSE_SIZE ((off_t)512 * 1024 * 1024)
#define PARENT_BYTE 0xA5
#define CHILD_BYTE  0x5C

static void ok(const char *name) { printf("SHMSHARE: ok %s\n", name); fflush(stdout); }
static void bad(const char *name, const char *why) {
	printf("SHMSHARE: fail %s (%s)\n", name, why);
	fflush(stdout);
}

/* Fill, then check that every byte is what the other side wrote. */
static int all_bytes_are(const unsigned char *p, size_t n, unsigned char want) {
	for (size_t i = 0; i < n; i++)
		if (p[i] != want)
			return 0;
	return 1;
}

static int make_memfd(size_t size) {
	int fd = memfd_create("shmshare", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* ── 1. memfd across fork ───────────────────────────────────────── */

static void test_fork_shared(void) {
	int fd = make_memfd(SHM_SIZE);
	if (fd < 0) { bad("memfd-fork", "memfd_create"); return; }

	unsigned char *p = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) { bad("memfd-fork", "mmap"); close(fd); return; }
	memset(p, PARENT_BYTE, SHM_SIZE);

	pid_t pid = fork();
	if (pid < 0) { bad("memfd-fork", "fork"); munmap(p, SHM_SIZE); close(fd); return; }
	if (pid == 0) {
		/* A mapping of its own, not the inherited one: two mmap() calls on the
		 * same file must land on the same pages. */
		unsigned char *c = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		                        fd, 0);
		if (c == MAP_FAILED)
			_exit(2);
		if (!all_bytes_are(c, SHM_SIZE, PARENT_BYTE))
			_exit(3); /* the parent's writes never reached the child */
		memset(c, CHILD_BYTE, SHM_SIZE);
		_exit(0);
	}

	int status = 0;
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		bad("memfd-fork", WEXITSTATUS(status) == 3 ? "child saw stale data"
		                                           : "child failed");
	} else if (!all_bytes_are(p, SHM_SIZE, CHILD_BYTE)) {
		bad("memfd-fork", "parent did not see the child's writes");
	} else {
		ok("memfd-fork");
	}
	munmap(p, SHM_SIZE);
	close(fd);
}

/* ── 2. memfd passed over AF_UNIX ───────────────────────────────── */

static int send_fd(int sock, int fd) {
	char dummy = 'x';
	struct iovec iov = { &dummy, 1 };
	char control[CMSG_SPACE(sizeof(int))];
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
	memcpy(CMSG_DATA(cm), &fd, sizeof(int));
	return sendmsg(sock, &mh, 0) == 1 ? 0 : -1;
}

static int recv_fd(int sock) {
	char dummy = 0;
	struct iovec iov = { &dummy, 1 };
	char control[CMSG_SPACE(sizeof(int))];
	struct msghdr mh;
	memset(&mh, 0, sizeof(mh));
	memset(control, 0, sizeof(control));
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = control;
	mh.msg_controllen = sizeof(control);
	if (recvmsg(sock, &mh, 0) != 1)
		return -1;
	struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
	if (!cm || cm->cmsg_type != SCM_RIGHTS)
		return -1;
	int fd = -1;
	memcpy(&fd, CMSG_DATA(cm), sizeof(int));
	return fd;
}

static void test_scm_rights_shared(void) {
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		bad("memfd-scm-rights", "socketpair");
		return;
	}

	pid_t pid = fork();
	if (pid < 0) { bad("memfd-scm-rights", "fork"); return; }
	if (pid == 0) {
		close(sv[0]);
		int fd = recv_fd(sv[1]);
		if (fd < 0)
			_exit(2);
		unsigned char *c = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		                        fd, 0);
		if (c == MAP_FAILED)
			_exit(3);
		if (!all_bytes_are(c, SHM_SIZE, PARENT_BYTE))
			_exit(4);
		memset(c, CHILD_BYTE, SHM_SIZE);
		/* Tell the parent the write is done before it looks. */
		char done = 'd';
		write(sv[1], &done, 1);
		_exit(0);
	}

	close(sv[1]);
	int fd = make_memfd(SHM_SIZE);
	if (fd < 0) { bad("memfd-scm-rights", "memfd_create"); return; }
	unsigned char *p = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) { bad("memfd-scm-rights", "mmap"); return; }
	memset(p, PARENT_BYTE, SHM_SIZE);

	if (send_fd(sv[0], fd) < 0) { bad("memfd-scm-rights", "sendmsg"); return; }

	char done = 0;
	read(sv[0], &done, 1);
	int status = 0;
	waitpid(pid, &status, 0);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		int rc = WEXITSTATUS(status);
		bad("memfd-scm-rights",
		    rc == 4 ? "receiver saw stale data" : "receiver failed");
	} else if (!all_bytes_are(p, SHM_SIZE, CHILD_BYTE)) {
		bad("memfd-scm-rights", "sender did not see the receiver's writes");
	} else {
		ok("memfd-scm-rights");
	}
	munmap(p, SHM_SIZE);
	close(fd);
	close(sv[0]);
}

/* ── 3. shm_open under /dev/shm ─────────────────────────────────── */

static void test_shm_open_shared(void) {
	const char *name = "/shmshare-probe";
	shm_unlink(name);
	int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
	if (fd < 0) { bad("shm-open-shared", "shm_open"); return; }
	if (ftruncate(fd, SHM_SIZE) < 0) { bad("shm-open-shared", "ftruncate"); close(fd); return; }

	unsigned char *p = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) { bad("shm-open-shared", "mmap"); close(fd); return; }
	memset(p, PARENT_BYTE, SHM_SIZE);

	pid_t pid = fork();
	if (pid < 0) { bad("shm-open-shared", "fork"); return; }
	if (pid == 0) {
		/* Open it by name, the way an unrelated process would. */
		int cfd = shm_open(name, O_RDWR, 0600);
		if (cfd < 0)
			_exit(2);
		unsigned char *c = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		                        cfd, 0);
		if (c == MAP_FAILED)
			_exit(3);
		if (!all_bytes_are(c, SHM_SIZE, PARENT_BYTE))
			_exit(4);
		memset(c, CHILD_BYTE, SHM_SIZE);
		_exit(0);
	}

	int status = 0;
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		int rc = WEXITSTATUS(status);
		bad("shm-open-shared",
		    rc == 4 ? "opener saw stale data" : "opener failed");
	} else if (!all_bytes_are(p, SHM_SIZE, CHILD_BYTE)) {
		bad("shm-open-shared", "creator did not see the opener's writes");
	} else {
		ok("shm-open-shared");
	}
	munmap(p, SHM_SIZE);
	close(fd);
	shm_unlink(name);
}

/* Reading MemFree, in kB. Negative if it cannot be read. */
static long mem_free_kb(void) {
	FILE *f = fopen("/proc/meminfo", "r");
	char line[128];
	long kb = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "MemFree:", 8) == 0) {
			kb = strtol(line + 8, NULL, 10);
			break;
		}
	}
	fclose(f);
	return kb;
}

/*
 * Declaring a size must not spend the memory, and writing after declaring it
 * must not walk off the buffer.
 *
 * A compositor sizes a buffer pool once and then paints small pieces of it.
 * When ftruncate allocated the whole length up front, a few such pools
 * exhausted a 4 GiB machine about twenty seconds into a desktop session, and
 * what failed afterwards was never the allocation itself -- it was a lazy page
 * with no frame behind it, or a shootdown stalled behind a starved CPU. None
 * of the existing checks noticed, because none of them declares a size it does
 * not then fill.
 *
 * The write is the second half and its own trap: once the declared size stops
 * matching the buffer, a grow path that copies `size` rather than the buffer's
 * real extent reads and writes far past both.
 */
static void test_large_sparse(void) {
	long before, after;
	int fd = memfd_create("shm-sparse", MFD_CLOEXEC);
	unsigned char buf[64];
	static const unsigned char pattern[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	if (fd < 0) { bad("sparse-ftruncate", "memfd_create"); return; }

	before = mem_free_kb();
	if (ftruncate(fd, SPARSE_SIZE) != 0) {
		bad("sparse-ftruncate", "ftruncate");
		close(fd);
		return;
	}
	after = mem_free_kb();

	if (before < 0 || after < 0) {
		bad("sparse-ftruncate", "cannot read MemFree");
	} else if (before - after > (long)(SPARSE_SIZE / 1024 / 4)) {
		/* A quarter of the declared size is far beyond any bookkeeping and
		 * far below the whole thing, so this separates "lazy" from "eager"
		 * without depending on how much else the machine is doing. */
		bad("sparse-ftruncate", "declaring a size consumed the memory");
	} else {
		ok("sparse-ftruncate");
	}

	/* The overflow case: a tiny write into a hugely declared file. */
	if (pwrite(fd, pattern, sizeof(pattern), 0) != (ssize_t)sizeof(pattern)) {
		bad("sparse-write", "pwrite");
	} else {
		memset(buf, 0xEE, sizeof(buf));
		if (pread(fd, buf, sizeof(pattern), 0) != (ssize_t)sizeof(pattern))
			bad("sparse-write", "pread");
		else if (memcmp(buf, pattern, sizeof(pattern)) != 0)
			bad("sparse-write", "read back what was not written");
		else
			ok("sparse-write");
	}

	/* Everything not written reads as zeroes, including far past any buffer
	 * the write above may have caused to exist. */
	memset(buf, 0xEE, sizeof(buf));
	if (pread(fd, buf, sizeof(buf), SPARSE_SIZE - 4096) != (ssize_t)sizeof(buf)) {
		bad("sparse-hole", "pread past the written region");
	} else {
		size_t i;

		for (i = 0; i < sizeof(buf); i++)
			if (buf[i] != 0)
				break;
		if (i != sizeof(buf))
			bad("sparse-hole", "a hole did not read as zeroes");
		else
			ok("sparse-hole");
	}
	close(fd);
}

int main(void) {
	printf("SHMSHARE: start\n");
	fflush(stdout);
	test_fork_shared();
	test_scm_rights_shared();
	test_shm_open_shared();
	test_large_sparse();
	printf("SHMSHARE: done\n");
	fflush(stdout);
	return 0;
}
