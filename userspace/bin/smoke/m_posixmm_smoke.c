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
 *   MM-SMOKE: ok ucontext-size
 *   MM-SMOKE: ok copyout-cow
 *   MM-SMOKE: ok copyout-readonly
 *   MM-SMOKE: ok file-map-privacy
 *   MM-SMOKE: ok rseq-after-sigkill
 *   MM-SMOKE: ok mmap-after-sigkill
 *   MM-SMOKE: ok wx-data-noexec
 *   MM-SMOKE: ok wx-exec-after-mprotect
 *   MM-SMOKE: ok wx-text-readonly
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
#include <sys/utsname.h>
#include <ucontext.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>

static void marker(const char *s) { write(1, s, strlen(s)); }

/* A bare "fail <name>" says only that the machine is wrong somewhere in a
 * forty-line test. Print the step that returned instead: the number is the
 * test's own return code, and it names the assertion. */
static void marker_fail(const char *name, int rc)
{
	char line[96];
	int n = snprintf(line, sizeof(line), "MM-SMOKE: fail %s rc=%d\n", name, rc);
	if (n > 0)
		write(1, line, (size_t)n);
}

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

/*
 * Does mmap ever hand out an address it has already given away?
 *
 * Every region gets a pattern of its own, and every pattern is re-read after
 * all of them exist. Two regions that overlap cannot both keep their pattern,
 * so a single mismatch proves the reuse — which is otherwise invisible until
 * something else's data is quietly destroyed. It reached us as a compositor
 * crashing inside malloc(), because musl keeps allocator metadata in mmap'd
 * pages and one of them had been handed out twice.
 *
 * The sizes vary and the regions are not unmapped in order, so the holes the
 * allocator has to search are not a simple ascending run.
 */
#define MM_REGIONS 96
static int test_mmap_no_overlap(void) {
	unsigned char *p[MM_REGIONS];
	size_t len[MM_REGIONS];
	int i;

	for (i = 0; i < MM_REGIONS; i++) {
		len[i] = (size_t)(1 + (i % 5)) * 4096;
		p[i] = mmap(0, len[i], PROT_READ | PROT_WRITE,
		            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p[i] == MAP_FAILED)
			return 1;
		memset(p[i], (unsigned char)(i + 1), len[i]);
	}

	/* Free every third one, so the next round has to place regions into holes
	 * rather than at the end. */
	for (i = 0; i < MM_REGIONS; i += 3) {
		munmap(p[i], len[i]);
		p[i] = 0;
	}
	for (i = 0; i < MM_REGIONS; i += 3) {
		len[i] = (size_t)(1 + (i % 3)) * 4096;
		p[i] = mmap(0, len[i], PROT_READ | PROT_WRITE,
		            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p[i] == MAP_FAILED)
			return 2;
		memset(p[i], (unsigned char)(i + 1), len[i]);
	}

	for (i = 0; i < MM_REGIONS; i++) {
		size_t j;

		for (j = 0; j < len[i]; j++) {
			if (p[i][j] != (unsigned char)(i + 1))
				return 3;
		}
	}
	for (i = 0; i < MM_REGIONS; i++)
		munmap(p[i], len[i]);
	return 0;
}


/* ---- test 4: what the kernel may write into, and what it must refuse -------
 *
 * The VMA list says what a program is ALLOWED to do; the page tables say what
 * the CPU will permit, and the two disagree routinely. Both halves of that
 * disagreement have to behave, and neither was tested:
 *
 *   a) a page that is read-only in the tables because it is copy-on-write,
 *      inside a mapping the program may write. The kernel's copy has to break
 *      the sharing exactly as a store from the program would, and the parent's
 *      copy must not change. Written from ring 0 without that step it faulted
 *      in supervisor mode with nothing to fix up, and the machine panicked --
 *      which is how every threaded program that exited at the wrong moment
 *      brought the system down, since musl points its thread-list lock there.
 *
 *   b) a page the program itself may NOT write. The kernel must answer EFAULT
 *      and leave the process running; it must not write anyway, and it must
 *      not die.
 *
 * uname() is the instrument: a fixed-size struct the kernel fills in one
 * copyout, with no side effects worth undoing. */
static int test_kernel_write_cow(void)
{
	/* A private anonymous page, dirtied so it holds a real frame, then forked
	 * so parent and child share that frame copy-on-write. */
	struct utsname *shared =
	    mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED)
		return 1;
	memset(shared, 0x5a, 4096);

	/* A second page, MAP_SHARED, so the child can report its verdict back. */
	volatile int *verdict =
	    mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (verdict == MAP_FAILED)
		return 2;
	*verdict = -1;

	pid_t pid = fork();
	if (pid < 0)
		return 3;
	if (pid == 0) {
		/* The child has NOT written to `shared`, so its pages are still the
		 * parent's frames, mapped read-only. This uname() is therefore a
		 * kernel write into a copy-on-write page. */
		int r = uname(shared);
		if (r != 0)
			*verdict = 10;
		else if (shared->sysname[0] == 0)
			*verdict = 11;
		else
			*verdict = 0;
		_exit(0);
	}
	int st = 0;
	/* Distinguish WHY the wait did not return this child: a bare "4" said only
	 * that it did not, which reads as a COW failure the test never reached. */
	pid_t w = waitpid(pid, &st, 0);
	if (w != pid) {
		if (w >= 0)
			return 40; /* some other pid came back */
		if (errno == EINTR)
			return 41;
		if (errno == ECHILD)
			return 42;
		return 43;
	}
	/* The child never got to write its verdict: it died on the uname(), which
	 * is the kernel write into the copy-on-write page this test is about.
	 * Report how it died -- a bare arithmetic code on a -1 verdict read as a
	 * waitpid failure for as long as this test existed. */
	if (*verdict == -1)
		return WIFSIGNALED(st) ? 20 + WTERMSIG(st) : 19;
	if (*verdict != 0)
		return 5 + (*verdict % 10);
	/* The child's write must not have reached the parent's copy. */
	for (int i = 0; i < 64; i++)
		if (((unsigned char *)shared)[i] != 0x5a)
			return 9;
	munmap((void *)shared, 4096);
	munmap((void *)verdict, 4096);
	return 0;
}

static int test_kernel_write_readonly(void)
{
	unsigned char *p =
	    mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return 1;
	memset(p, 0x3c, 4096);
	if (mprotect(p, 4096, PROT_READ) != 0)
		return 2;

	errno = 0;
	if (uname((struct utsname *)p) == 0)
		return 3; /* wrote into a page the program cannot write */
	if (errno != EFAULT)
		return 4;
	/* Nothing may have been written, and this process is still here to say so. */
	for (int i = 0; i < 4096; i++)
		if (p[i] != 0x3c)
			return 5;
	munmap(p, 4096);
	return 0;
}


/* ---- test 5: the signal frame gives a ucontext_t the size of a ucontext_t ---
 *
 * The kernel hands an SA_SIGINFO handler two pointers and lays both objects out
 * on the user stack itself. ucontext_t on x86_64 is 936 bytes, and the last 512
 * of those -- __fpregs_mem -- live INSIDE the object even when
 * uc_mcontext.fpregs is null. The kernel used to reserve 424 and place the
 * siginfo immediately above, so the rest of the ucontext overlapped the
 * siginfo, the red zone and the interrupted frame, canary included.
 *
 * The check is the contract itself: there must be room for a whole ucontext_t
 * between where the kernel put it and whatever it put next. Reading the tail
 * proves the pages are there; a handler is entitled to do that much. */
static volatile int uc_verdict = -1;
static volatile unsigned long uc_gap;

static void uc_handler(int sig, siginfo_t *si, void *ctx)
{
	(void)sig;
	unsigned char *uc = ctx;
	unsigned char *info = (unsigned char *)si;
	volatile unsigned long sum = 0;

	if (!uc || !info) {
		uc_verdict = 1;
		return;
	}
	/* The kernel places the siginfo above the ucontext. Whatever the order, the
	 * distance between them cannot be less than the object's own size. */
	uc_gap = (unsigned long)(info > uc ? (info - uc) : (uc - info));
	if (uc_gap < sizeof(ucontext_t)) {
		uc_verdict = 2;
		return;
	}
	/* Touch the whole object, including the tail that used to fall outside it. */
	for (size_t i = 0; i < sizeof(ucontext_t); i++)
		sum += uc[i];
	(void)sum;
	uc_verdict = 0;
}

static int test_ucontext_size(void)
{
	struct sigaction sa, old;
	volatile unsigned long canary_guard[8];

	for (int i = 0; i < 8; i++)
		canary_guard[i] = 0xA5A5A5A5UL + (unsigned long)i;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = uc_handler;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGUSR2, &sa, &old) != 0)
		return 1;
	uc_verdict = -1;
	if (raise(SIGUSR2) != 0)
		return 2;
	if (uc_verdict != 0)
		return 10 + (uc_verdict < 0 ? 9 : uc_verdict);
	/* And this frame is exactly as it was. */
	for (int i = 0; i < 8; i++)
		if (canary_guard[i] != 0xA5A5A5A5UL + (unsigned long)i)
			return 3;
	sigaction(SIGUSR2, &old, NULL);
	return 0;
}


/* ---- test: the address-space lock survives a SIGKILL taken inside mmap ----
 *
 * mmap/munmap/mprotect serialise on a per-address-space mutex, and the slot a
 * space uses is chosen by hashing its PML4 frame, so unrelated processes share
 * slots. The lock is yielding and is released only by the code that took it --
 * so a task killed while holding one leaves it held for ever, and every other
 * process whose address space hashes to that slot then blocks in mmap until
 * the machine goes quiet. There is no error and no panic: the symptom is a
 * guest that stops answering.
 *
 * scheduler_exit_current hands the lock back, but a task killed by SIGKILL is
 * marked dead from inside the scheduler and never runs that path. exit_group
 * posts SIGKILL to every sibling, so any multithreaded program that exits
 * while one of its threads is in mmap could do this -- which is every desktop
 * program there is.
 *
 * The test kills children at the moment they are looping through mmap, then
 * asks fresh children to map memory and reports how many of them came back. A
 * child that never returns from mmap is the leak, and because it hangs rather
 * than fails, the reaping is bounded and the count is what is reported.
 */
#define MMK_VICTIMS 120
#define MMK_PROBES 48

static void mmk_map_loop(int rounds)
{
	for (int i = 0; i < rounds; i++) {
		void *p = mmap(0, 64 * 4096, PROT_READ | PROT_WRITE,
		               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED)
			_exit(3);
		*(volatile unsigned char *)p = 1;
		munmap(p, 64 * 4096);
	}
}

static int test_mmap_lock_after_sigkill(void)
{
	/* Kill each victim while it is inside the mapping loop. The delay is a
	 * spin rather than a sleep: the window is the few microseconds a task
	 * spends holding the lock, and a 10 ms tick steps straight over it. */
	for (int v = 0; v < MMK_VICTIMS; v++) {
		pid_t pid = fork();
		if (pid < 0)
			return 1;
		if (pid == 0) {
			mmk_map_loop(100000);
			_exit(0);
		}
		volatile unsigned long spin = 0;
		unsigned long budget = 2000UL + (unsigned long)v * 350UL;
		while (spin < budget)
			spin++;
		kill(pid, SIGKILL);
		waitpid(pid, 0, 0);
	}

	/* Now ask fresh processes to map memory. Each has its own address space,
	 * so between them they cover the slot table; one that hashes to a leaked
	 * slot never returns from its first mmap. */
	pid_t probes[MMK_PROBES];
	for (int i = 0; i < MMK_PROBES; i++) {
		probes[i] = fork();
		if (probes[i] < 0)
			return 2;
		if (probes[i] == 0) {
			mmk_map_loop(40);
			_exit(0);
		}
	}

	/* Bounded, because the failure being tested for is a hang: a blocking
	 * waitpid here would take the whole suite down with it instead of
	 * reporting the number that matters. */
	int reaped = 0;
	for (int round = 0; round < 3000 && reaped < MMK_PROBES; round++) {
		for (int i = 0; i < MMK_PROBES; i++) {
			if (probes[i] <= 0)
				continue;
			int st = 0;
			pid_t r = waitpid(probes[i], &st, WNOHANG);
			if (r == probes[i]) {
				probes[i] = -1;
				reaped++;
			}
		}
		if (reaped < MMK_PROBES)
			usleep(5000);
	}
	if (reaped != MMK_PROBES) {
		char msg[96];
		int n = snprintf(msg, sizeof(msg),
		                 "MM-SMOKE: mmap-after-sigkill stuck=%d of %d\n",
		                 MMK_PROBES - reaped, MMK_PROBES);
		if (n > 0)
			write(1, msg, (size_t)n);
		for (int i = 0; i < MMK_PROBES; i++)
			if (probes[i] > 0)
				kill(probes[i], SIGKILL);
		return 3;
	}
	return 0;
}


/* ---- test: a file mapping's neighbours get the mapping's own protection ----
 *
 * A read fault on a file-backed page maps the pages around it too, straight
 * out of the page cache and without a fault of their own. Those frames are
 * shared with every other mapper of the file, so the protection they are
 * installed with is the whole question: a MAP_PRIVATE writable mapping that
 * gets one of them WRITABLE writes the process's private stores into the page
 * cache, and the next program to map that file reads them -- which for a
 * shared library means executing another process's relocated pointers.
 *
 * So: touch page 0 to bring a window in, store into a page the process never
 * faulted itself, and ask the file what it holds. Private stores must not
 * reach it; shared stores must.
 */
#define MMF_PAGES 32
#define MMF_TOUCH_PAGE 7

static int mmf_make(const char *path, unsigned char fill)
{
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	unsigned char page[4096];
	memset(page, fill, sizeof(page));
	for (int i = 0; i < MMF_PAGES; i++) {
		if (write(fd, page, sizeof(page)) != (ssize_t)sizeof(page)) {
			close(fd);
			return -1;
		}
	}
	return fd;
}

static int mmf_byte_in_file(const char *path, off_t off)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	unsigned char b = 0;
	ssize_t r = pread(fd, &b, 1, off);
	close(fd);
	return r == 1 ? (int)b : -1;
}

static int test_file_map_privacy(void)
{
	const char *priv = "/tmp/mm-map-private";
	const char *shar = "/tmp/mm-map-shared";
	size_t len = (size_t)MMF_PAGES * 4096;
	off_t off = (off_t)MMF_TOUCH_PAGE * 4096;

	int fd = mmf_make(priv, 0x5a);
	if (fd < 0)
		return 1;
	unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return 2;
	}
	/* Read page 0. Whatever the kernel maps around it is mapped now, without
	 * a fault of its own -- which is the case under test. */
	if (p[0] != 0x5a) {
		munmap(p, len);
		close(fd);
		return 3;
	}
	p[off] = 0xc3;
	if (p[off] != 0xc3) {
		munmap(p, len);
		close(fd);
		return 4;
	}
	int in_file = mmf_byte_in_file(priv, off);
	munmap(p, len);
	close(fd);
	unlink(priv);
	if (in_file != 0x5a)
		return 5; /* the private store escaped into the file */

	/* The same shape, shared: the store must reach the file. */
	fd = mmf_make(shar, 0x33);
	if (fd < 0)
		return 6;
	p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return 7;
	}
	if (p[0] != 0x33) {
		munmap(p, len);
		close(fd);
		return 8;
	}
	p[off] = 0x77;
	if (msync(p, len, MS_SYNC) != 0) {
		munmap(p, len);
		close(fd);
		return 9;
	}
	in_file = mmf_byte_in_file(shar, off);
	munmap(p, len);
	close(fd);
	unlink(shar);
	if (in_file != 0x77)
		return 10; /* the shared store never reached the file */
	return 0;
}


/* ---- test: a SIGKILLed task gives its rseq(2) area back ------------------
 *
 * glibc registers an rseq area for every thread it creates, and treats a
 * refusal on a thread as fatal -- it prints "Fatal glibc error: rseq
 * registration failed" and kills the process. So a kernel that loses track of
 * a registration does not degrade, it kills programs; Weston's terminal died
 * of exactly this.
 *
 * The registrations live in a table keyed by the task. The scheduler's
 * SIGKILL path releases futexes, timers and ptrace links by hand and did not
 * release this, so every task killed by a signal leaked its entry -- and once
 * its task slot was reused, the next thread's first registration looked like a
 * conflicting re-registration of a different area and was refused.
 *
 * Kill more children than the table has room for, then register from a fresh
 * process. With the leak the table is full (or the recycled slot conflicts)
 * and the registration is refused.
 */
/* rseq's number is per-architecture: 334 on x86_64, 293 on the asm-generic
 * table aarch64 uses. Hardcoding the x86 one made every aarch64 run report
 * "rseq is not implemented" for a kernel that implements it. */
#if defined(__aarch64__)
#define RSEQ_SYS 293
#else
#define RSEQ_SYS 334
#endif
#define RSEQ_KILLS 200
#define RSEQ_SIG 0x53053053u

struct mm_rseq_area {
	unsigned int cpu_id_start;
	unsigned int cpu_id;
	unsigned long long rseq_cs;
	unsigned int flags;
	unsigned int pad;
} __attribute__((aligned(32)));

static int mm_rseq_register(struct mm_rseq_area *a)
{
	return (int)syscall(RSEQ_SYS, a, (long)32, (long)0, (long)RSEQ_SIG);
}

static int test_rseq_after_sigkill(void)
{
	static struct mm_rseq_area probe;

	/* Is rseq implemented at all? A kernel that answers ENOSYS has nothing
	 * to leak and nothing to test; say so rather than passing quietly. */
	if (mm_rseq_register(&probe) != 0)
		return 1;
	/* Give it back, so the parent's own slot is not what runs out. */
	if ((int)syscall(RSEQ_SYS, &probe, (long)32, (long)1 /* UNREGISTER */,
	                 (long)RSEQ_SIG) != 0)
		return 2;

	for (int i = 0; i < RSEQ_KILLS; i++) {
		pid_t pid = fork();
		if (pid < 0)
			return 3;
		if (pid == 0) {
			static struct mm_rseq_area child_area;
			if (mm_rseq_register(&child_area) != 0)
				_exit(4);
			/* Sit still and be killed while registered. */
			for (;;)
				pause();
		}
		/* Long enough for the child to have registered: it does that as its
		 * first act, and the kill has to land after it. */
		usleep(2000);
		kill(pid, SIGKILL);
		waitpid(pid, 0, 0);
	}

	/* A fresh process must still be able to register. */
	pid_t pid = fork();
	if (pid < 0)
		return 5;
	if (pid == 0) {
		static struct mm_rseq_area after;
		_exit(mm_rseq_register(&after) == 0 ? 0 : 6);
	}
	int st = 0;
	if (waitpid(pid, &st, 0) != pid)
		return 7;
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
		return 8;
	return 0;
}

/* ---- W^X: a mapping is executable only if it asked to be ----------------
 *
 * Every userspace page used to be mapped RWX no matter what PROT_* the caller
 * passed: mmap and mprotect translated PROT_WRITE and nothing else, and the
 * ELF loader recorded every segment as RWX outright. NX was implemented and
 * used for MMIO and module images, so the bit worked — no process page ever
 * carried it.
 *
 * Each half runs in a child, because the passing outcome is a fatal signal. */

/* `ret` — the shortest thing that proves control reached the buffer. */
#define RET_OPCODE 0xC3

static int child_status(int (*body)(void *), void *arg) {
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0)
		_exit(body(arg) & 0x7F);

	int status = 0;
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return status;
}

static int child_call_buffer(void *p) {
	void (*fn)(void) = (void (*)(void))p;

	fn();
	return 42; /* reached only if the page really was executable */
}

static int child_write_text(void *p) {
	volatile unsigned char *code = (volatile unsigned char *)p;

	*code = RET_OPCODE;
	return 42; /* reached only if the text page really was writable */
}

/* A data mapping (PROT_READ|PROT_WRITE) must not be executable. */
static int test_wx_data_noexec(void) {
	size_t len = 4096;
	unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
	                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		return 1;
	p[0] = RET_OPCODE;

	int status = child_status(child_call_buffer, p);

	munmap(p, len);
	if (status < 0)
		return 2;
	if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV)
		return 3; /* it ran, or died some other way */
	return 0;
}

/* ...and mprotect must still be able to grant execute, dropping write: that is
 * the W^X flip every JIT performs, and breaking it would break V8 and rustc. */
static int test_wx_exec_after_mprotect(void) {
	size_t len = 4096;
	unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
	                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		return 1;
	p[0] = RET_OPCODE;
	if (mprotect(p, len, PROT_READ | PROT_EXEC) != 0) {
		munmap(p, len);
		return 2;
	}

	int status = child_status(child_call_buffer, p);

	munmap(p, len);
	if (status < 0)
		return 3;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 42)
		return 4; /* the call did not return normally */
	return 0;
}

/* This program's own .text must not be writable. */
static int test_wx_text_readonly(void) {
	int status = child_status(child_write_text, (void *)(uintptr_t)&marker);

	if (status < 0)
		return 1;
	if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV)
		return 2;
	return 0;
}

int main(void) {
	marker("MM-SMOKE: start\n");

	int overlaprc = test_mmap_no_overlap();
	if (overlaprc != 0) {
		marker_fail("mmap-no-overlap", overlaprc);
		return 60 + overlaprc;
	}
	marker("MM-SMOKE: ok mmap-no-overlap\n");

	int rdrc = test_readdir_terminates();
	if (rdrc != 0) {
		marker_fail("readdir-terminates", rdrc);
		return 50 + rdrc;
	}
	marker("MM-SMOKE: ok readdir-terminates\n");

	int shmrc = test_shm_open();
	if (shmrc != 0) {
		marker_fail("shm-open", shmrc);
		return 40 + shmrc;
	}
	marker("MM-SMOKE: ok shm-open\n");

	int rc = test_madvise();
	if (rc != 0) {
		marker_fail("madvise", rc);
		return 10 + rc;
	}
	marker("MM-SMOKE: ok madvise\n");

	rc = test_noreserve();
	if (rc != 0) {
		marker_fail("noreserve", rc);
		return 20 + rc;
	}
	marker("MM-SMOKE: ok noreserve\n");

	rc = test_sigaltstack();
	if (rc != 0) {
		marker_fail("sigaltstack", rc);
		return 30 + rc;
	}
	marker("MM-SMOKE: ok sigaltstack\n");

	rc = test_kernel_write_cow();
	if (rc != 0) {
		marker_fail("copyout-cow", rc);
		return 70 + rc;
	}
	marker("MM-SMOKE: ok copyout-cow\n");

	rc = test_ucontext_size();
	if (rc != 0) {
		marker_fail("ucontext-size", rc);
		return 90 + rc;
	}
	marker("MM-SMOKE: ok ucontext-size\n");

	rc = test_kernel_write_readonly();
	if (rc != 0) {
		marker_fail("copyout-readonly", rc);
		return 80 + rc;
	}
	marker("MM-SMOKE: ok copyout-readonly\n");

	rc = test_file_map_privacy();
	if (rc != 0) {
		marker_fail("file-map-privacy", rc);
		return 110 + rc;
	}
	marker("MM-SMOKE: ok file-map-privacy\n");

	rc = test_rseq_after_sigkill();
	if (rc != 0) {
		marker_fail("rseq-after-sigkill", rc);
		return 120 + rc;
	}
	marker("MM-SMOKE: ok rseq-after-sigkill\n");

	rc = test_mmap_lock_after_sigkill();
	if (rc != 0) {
		marker_fail("mmap-after-sigkill", rc);
		return 100 + rc;
	}
	marker("MM-SMOKE: ok mmap-after-sigkill\n");

	rc = test_wx_data_noexec();
	if (rc != 0) {
		marker("MM-SMOKE: fail wx-data-noexec\n");
		return 130 + rc;
	}
	marker("MM-SMOKE: ok wx-data-noexec\n");

	rc = test_wx_exec_after_mprotect();
	if (rc != 0) {
		marker("MM-SMOKE: fail wx-exec-after-mprotect\n");
		return 140 + rc;
	}
	marker("MM-SMOKE: ok wx-exec-after-mprotect\n");

	rc = test_wx_text_readonly();
	if (rc != 0) {
		marker("MM-SMOKE: fail wx-text-readonly\n");
		return 150 + rc;
	}
	marker("MM-SMOKE: ok wx-text-readonly\n");

	marker("MM-SMOKE: done\n");
	return 0;
}
