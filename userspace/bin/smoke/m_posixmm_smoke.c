/* m_posixmm_smoke — verifies three POSIX memory/signal primitives:
 *   1. madvise(MADV_DONTNEED) drops a written anonymous page so it refaults zero
 *   2. MAP_NORESERVE large anonymous mmap succeeds and commits on touch
 *   3. sigaltstack set/get/disable round-trip + an SA_ONSTACK handler observed
 *      running on the registered alternate stack
 *
 * Markers (only emitted on verified success):
 *   MM-SMOKE: start
 *   MM-SMOKE: ok madvise
 *   MM-SMOKE: ok noreserve
 *   MM-SMOKE: ok sigaltstack
 *   MM-SMOKE: done
 */
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void marker(const char *s) { write(1, s, strlen(s)); }

/* ---- test 1: madvise(MADV_DONTNEED) zeroes an anonymous page on refault ---- */
static int test_madvise(void) {
	size_t len = 4096 * 4;
	unsigned char *p =
	    mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return 1;

	/* Dirty every byte with a non-zero pattern. */
	for (size_t i = 0; i < len; i++)
		p[i] = (unsigned char)(0xA5 + i);
	/* Confirm the write took. */
	if (p[0] != 0xA5 || p[4096] != (unsigned char)(0xA5 + 4096))
		return 2;

	/* Drop the backing pages. */
	if (madvise(p, len, MADV_DONTNEED) != 0)
		return 3;

	/* Next read must refault to fresh zeroed pages. */
	for (size_t i = 0; i < len; i++) {
		if (p[i] != 0)
			return 4;
	}

	/* Writing again after DONTNEED must still work (page is re-armed). */
	p[100] = 0x77;
	if (p[100] != 0x77)
		return 5;

	/* Advisory hints V8 uses must be accepted as a no-op, not EINVAL. */
	if (madvise(p, len, MADV_DONTFORK) != 0)
		return 6;
	if (madvise(p, len, MADV_HUGEPAGE) != 0)
		return 7;

	munmap(p, len);
	return 0;
}

/* ---- test 2: MAP_NORESERVE large mapping, commit-on-touch ---- */
static int test_noreserve(void) {
	/* 64 MiB anonymous NORESERVE — larger than we will touch, so it can only
	 * succeed if pages are committed lazily rather than reserved up front. */
	size_t len = 64ULL * 1024 * 1024;
	unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
	                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED)
		return 1;

	/* Touch a few sparse pages across the range. */
	p[0] = 0x10;
	p[4096 * 1000] = 0x20;        /* ~4 MiB in */
	p[len - 1] = 0x30;            /* last page */

	if (p[0] != 0x10 || p[4096 * 1000] != 0x20 || p[len - 1] != 0x30) {
		munmap(p, len);
		return 2;
	}
	/* An untouched page in between must read back as zero (fresh commit). */
	if (p[4096 * 500] != 0) {
		munmap(p, len);
		return 3;
	}

	munmap(p, len);
	return 0;
}

/* ---- test 3: sigaltstack round-trip + SA_ONSTACK delivery ---- */
static volatile uintptr_t g_altstack_lo;
static volatile uintptr_t g_altstack_hi;
static volatile int g_handler_ran;
static volatile int g_handler_on_alt;

static void onstack_handler(int sig) {
	(void)sig;
	int local; /* a stack object inside the handler */
	uintptr_t sp = (uintptr_t)&local;
	g_handler_ran = 1;
	g_handler_on_alt = (sp >= g_altstack_lo && sp < g_altstack_hi);
}

static int test_sigaltstack(void) {
	/* a) get the default (no alt stack) → SS_DISABLE */
	stack_t old;
	memset(&old, 0, sizeof(old));
	if (sigaltstack(NULL, &old) != 0)
		return 1;
	if (!(old.ss_flags & SS_DISABLE))
		return 2;

	/* b) register an alt stack on a fresh anonymous mapping */
	size_t ss_size = SIGSTKSZ;
	void *base = mmap(0, ss_size, PROT_READ | PROT_WRITE,
	                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return 3;
	g_altstack_lo = (uintptr_t)base;
	g_altstack_hi = (uintptr_t)base + ss_size;

	stack_t ss;
	memset(&ss, 0, sizeof(ss));
	ss.ss_sp = base;
	ss.ss_flags = 0;
	ss.ss_size = ss_size;
	if (sigaltstack(&ss, NULL) != 0)
		return 4;

	/* c) get it back → ss_sp/ss_size round-trip, not disabled */
	stack_t got;
	memset(&got, 0, sizeof(got));
	if (sigaltstack(NULL, &got) != 0)
		return 5;
	if (got.ss_sp != base || got.ss_size != ss_size ||
	    (got.ss_flags & SS_DISABLE))
		return 6;

	/* d) install an SA_ONSTACK handler and raise the signal; verify the handler
	 *    ran with its stack pointer inside the registered alt-stack range. */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = onstack_handler;
	sa.sa_flags = SA_ONSTACK;
	if (sigaction(SIGUSR1, &sa, NULL) != 0)
		return 7;

	g_handler_ran = 0;
	g_handler_on_alt = 0;
	raise(SIGUSR1);
	/* raise() delivers synchronously before returning here. */
	if (!g_handler_ran)
		return 8;
	if (!g_handler_on_alt)
		return 9;

	/* e) reset to default handler so it can't fire again, then disable */
	sa.sa_handler = SIG_DFL;
	sa.sa_flags = 0;
	sigaction(SIGUSR1, &sa, NULL);

	memset(&ss, 0, sizeof(ss));
	ss.ss_flags = SS_DISABLE;
	if (sigaltstack(&ss, NULL) != 0)
		return 10;
	memset(&got, 0, sizeof(got));
	if (sigaltstack(NULL, &got) != 0)
		return 11;
	if (!(got.ss_flags & SS_DISABLE))
		return 12;

	munmap(base, ss_size);
	return 0;
}

/* POSIX shared memory. musl's shm_open() opens /dev/shm/<name>, so this fails
 * at the first call if that directory does not exist — which is exactly how a
 * Wayland compositor ended up with no output at all: wlroots allocates every
 * output buffer this way and reports only "Failed to allocate buffer". */
static int test_shm_open(void) {
	const char *name = "/mm_smoke_shm";
	shm_unlink(name);
	int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		return 1;
	shm_unlink(name);
	size_t size = 256 * 1024;
	if (ftruncate(fd, (off_t)size) != 0) {
		close(fd);
		return 2;
	}
	unsigned char *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return 3;
	}
	p[0] = 0xA5;
	p[size - 1] = 0x5A;
	int ok = (p[0] == 0xA5 && p[size - 1] == 0x5A);
	munmap(p, size);
	close(fd);
	return ok ? 0 : 4;
}

/* readdir must terminate. The in-memory readdir resumes from "children whose
 * dir_seq is below the last emitted", and a child left at dir_seq 0 collapsed
 * that bound to "no bound" — the walk restarted from the head of the sibling
 * list on every call and the directory repeated one entry forever, so `ls` on
 * it never returned. Nodes not built through vfs_create_node (mkdir among
 * them) were exactly that case. */
static int test_readdir_terminates(void) {
	const char *dir = "/tmp/mm_readdir";
	char p[128];
	mkdir(dir, 0755);
	for (int i = 0; i < 3; i++) {
		snprintf(p, sizeof(p), "%s/sub%d", dir, i);
		mkdir(p, 0755);
	}
	snprintf(p, sizeof(p), "%s/file", dir);
	int fd = open(p, O_CREAT | O_WRONLY, 0644);
	if (fd >= 0)
		close(fd);

	DIR *d = opendir(dir);
	if (!d)
		return 1;
	int n = 0, saw_file = 0, saw_sub = 0;
	struct dirent *e;
	/* A repeat-forever bug shows up as the count running away; cap it well
	 * above the real entry count so a correct walk is never cut short. */
	while ((e = readdir(d)) != NULL && n < 200) {
		n++;
		if (strcmp(e->d_name, "file") == 0)
			saw_file++;
		if (strncmp(e->d_name, "sub", 3) == 0)
			saw_sub++;
	}
	closedir(d);
	/* ".", "..", three subdirs and one file — and each exactly once. */
	return (n <= 8 && saw_file == 1 && saw_sub == 3) ? 0 : 2;
}

int main(void) {
	marker("MM-SMOKE: start\n");

	int rdrc = test_readdir_terminates();
	if (rdrc != 0) {
		marker("MM-SMOKE: fail readdir-terminates\n");
		return 50 + rdrc;
	}
	marker("MM-SMOKE: ok readdir-terminates\n");

	int shmrc = test_shm_open();
	if (shmrc != 0) {
		marker("MM-SMOKE: fail shm-open\n");
		return 40 + shmrc;
	}
	marker("MM-SMOKE: ok shm-open\n");

	int rc = test_madvise();
	if (rc != 0) {
		marker("MM-SMOKE: fail madvise\n");
		return 10 + rc;
	}
	marker("MM-SMOKE: ok madvise\n");

	rc = test_noreserve();
	if (rc != 0) {
		marker("MM-SMOKE: fail noreserve\n");
		return 20 + rc;
	}
	marker("MM-SMOKE: ok noreserve\n");

	rc = test_sigaltstack();
	if (rc != 0) {
		marker("MM-SMOKE: fail sigaltstack\n");
		return 30 + rc;
	}
	marker("MM-SMOKE: ok sigaltstack\n");

	marker("MM-SMOKE: done\n");
	return 0;
}
