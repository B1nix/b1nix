/* SPDX-License-Identifier: GPL-2.0-only */
/* vDSO smoke: clock readings without a system call.
 *
 * Every marker is printed by the check that established it; a check that could
 * not run prints FAIL with the reason, never ok.
 *
 *   auxv-ehdr        AT_SYSINFO_EHDR names an ELF shared object whose dynamic
 *                    symbol table exports the clock functions under the version
 *                    libc asks for, with DT_HASH present (musl needs it).
 *   maps             /proc/self/maps shows [vdso] r-xp at that address and
 *                    [vvar] r--p directly below it.
 *   agree-<clock>    for REALTIME, MONOTONIC, MONOTONIC_RAW and BOOTTIME, every
 *                    vDSO reading lies between a raw system call made just
 *                    before and one made just after it — the two paths are the
 *                    same clock, not merely close ones.
 *   agree-other      gettimeofday, clock_getres (and time on x86_64) agree with
 *                    their system calls; the clocks the vDSO does not compute
 *                    (CPU time, COARSE) and bad arguments still get the kernel's
 *                    own answer and errors.
 *   no-syscall       with a seccomp filter that fails clock_gettime,
 *                    gettimeofday (and time) in the kernel, libc's
 *                    clock_gettime/gettimeofday/time keep working: they never
 *                    enter it. The filter is proven live by the raw call failing.
 *   monotonic        hundreds of thousands of consecutive readings never step
 *                    back, per clock.
 *   monotonic-threads threads spread over the CPUs read CLOCK_MONOTONIC in turn
 *                    under one lock (which orders the reads in real time); no
 *                    reading is below the one before it, whichever CPU took it.
 *   fork             a forked child has the same vDSO at the same address and
 *                    it works; so does a child that unmapped both mappings, and
 *                    the parent's still works after that child exits.
 *   exec             an exec'd image gets its own mapping and it works.
 *   mprotect         the mappings cannot be made writable (EACCES), and
 *                    [vvar] cannot be made executable.
 *   cost             per-call cost of libc clock_gettime against the raw system
 *                    call (printed; ok when the vDSO is the cheaper of the two).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <elf.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#if defined(__x86_64__)
#define VDSO_VERSION "LINUX_2.6"
#define SYM_CGT "__vdso_clock_gettime"
#define SYM_GTOD "__vdso_gettimeofday"
#define SYM_GETRES "__vdso_clock_getres"
#define SYM_TIME "__vdso_time"
#elif defined(__aarch64__)
#define VDSO_VERSION "LINUX_2.6.39"
#define SYM_CGT "__kernel_clock_gettime"
#define SYM_GTOD "__kernel_gettimeofday"
#define SYM_GETRES "__kernel_clock_getres"
#endif

#define PAGE 4096UL

static int g_fail;

static void ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void bad(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#include <stdarg.h>
static void ok(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("VDSO-SMOKE: ok ");
	vprintf(fmt, ap);
	printf("\n");
	fflush(stdout);
	va_end(ap);
}

static void bad(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("VDSO-SMOKE: FAIL ");
	vprintf(fmt, ap);
	printf("\n");
	fflush(stdout);
	va_end(ap);
	g_fail = 1;
}

typedef int (*cgt_fn)(clockid_t, struct timespec *);
typedef int (*gtod_fn)(struct timeval *, void *);
typedef time_t (*time_fn)(time_t *);

static cgt_fn g_cgt;
static gtod_fn g_gtod;
static cgt_fn g_getres;
#ifdef SYM_TIME
static time_fn g_time;
#endif

/* ── Reading the vDSO's ELF the way a dynamic linker does ──────────────────── */

/* Look `name` up in the object at `eh`, requiring version `ver`. Returns the
 * absolute address or NULL. Sets *have_hash when the object carries DT_HASH. */
static void *vdso_lookup(const Elf64_Ehdr *eh, const char *name, const char *ver,
                         int *have_hash)
{
	const Elf64_Phdr *ph = (const Elf64_Phdr *)((const char *)eh + eh->e_phoff);
	const Elf64_Dyn *dyn = NULL;
	uintptr_t base = (uintptr_t)-1;

	for (int i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type == PT_LOAD && base == (uintptr_t)-1)
			base = (uintptr_t)eh + ph[i].p_offset - ph[i].p_vaddr;
		else if (ph[i].p_type == PT_DYNAMIC)
			dyn = (const Elf64_Dyn *)((const char *)eh + ph[i].p_offset);
	}
	if (!dyn || base == (uintptr_t)-1)
		return NULL;

	const Elf32_Word *hash = NULL;
	const Elf64_Sym *syms = NULL;
	const char *strs = NULL;
	const Elf64_Half *versym = NULL;
	const Elf64_Verdef *verdef = NULL;

	for (; dyn->d_tag != DT_NULL; dyn++) {
		uintptr_t p = base + dyn->d_un.d_ptr;
		switch (dyn->d_tag) {
		case DT_HASH: hash = (const Elf32_Word *)p; break;
		case DT_SYMTAB: syms = (const Elf64_Sym *)p; break;
		case DT_STRTAB: strs = (const char *)p; break;
		case DT_VERSYM: versym = (const Elf64_Half *)p; break;
		case DT_VERDEF: verdef = (const Elf64_Verdef *)p; break;
		}
	}
	if (have_hash)
		*have_hash = hash != NULL;
	if (!hash || !syms || !strs || !versym || !verdef)
		return NULL;

	for (Elf32_Word i = 0; i < hash[1]; i++) {
		const Elf64_Sym *s = &syms[i];
		if (ELF64_ST_TYPE(s->st_info) != STT_FUNC || s->st_shndx == SHN_UNDEF)
			continue;
		if (strcmp(strs + s->st_name, name) != 0)
			continue;
		/* The symbol's version index, resolved through the definitions. */
		Elf64_Half vi = versym[i] & 0x7fff;
		const Elf64_Verdef *d = verdef;
		for (;;) {
			if (d->vd_ndx == vi && !(d->vd_flags & VER_FLG_BASE)) {
				const Elf64_Verdaux *aux =
					(const Elf64_Verdaux *)((const char *)d + d->vd_aux);
				if (strcmp(strs + aux->vda_name, ver) == 0)
					return (void *)(base + s->st_value);
			}
			if (!d->vd_next)
				break;
			d = (const Elf64_Verdef *)((const char *)d + d->vd_next);
		}
	}
	return NULL;
}

static int test_auxv(void)
{
	unsigned long ehdr = getauxval(AT_SYSINFO_EHDR);
	if (!ehdr) {
		bad("auxv-ehdr (no AT_SYSINFO_EHDR in the auxiliary vector)");
		return -1;
	}
	const Elf64_Ehdr *eh = (const Elf64_Ehdr *)ehdr;
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_type != ET_DYN) {
		bad("auxv-ehdr (0x%lx is not an ELF shared object)", ehdr);
		return -1;
	}
	int have_hash = 0;
	int found = 0, want = 0;

	want++; if ((g_cgt = vdso_lookup(eh, SYM_CGT, VDSO_VERSION, &have_hash))) found++;
	want++; if ((g_gtod = vdso_lookup(eh, SYM_GTOD, VDSO_VERSION, NULL))) found++;
	want++; if ((g_getres = vdso_lookup(eh, SYM_GETRES, VDSO_VERSION, NULL))) found++;
#ifdef SYM_TIME
	want++; if ((g_time = vdso_lookup(eh, SYM_TIME, VDSO_VERSION, NULL))) found++;
	/* Linux's x86_64 vDSO exports the plain names as well. */
	want++; if (vdso_lookup(eh, "clock_gettime", VDSO_VERSION, NULL) == (void *)g_cgt && g_cgt) found++;
#endif
	if (!have_hash) {
		bad("auxv-ehdr (no DT_HASH: musl cannot look symbols up)");
		return -1;
	}
	if (found != want) {
		bad("auxv-ehdr (%d of %d versioned %s symbols found)", found, want, VDSO_VERSION);
		return -1;
	}
	ok("auxv-ehdr at=0x%lx symbols=%d version=%s", ehdr, found, VDSO_VERSION);
	return 0;
}

/* The [vdso] line must start at `ehdr`, be r-xp, and have [vvar] r--p ending
 * exactly there. Returns 0 when both are found. */
static int check_maps(unsigned long ehdr, char *why, size_t whylen)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	int vdso_ok = 0, vvar_ok = 0;

	if (!f) {
		snprintf(why, whylen, "cannot open /proc/self/maps: %s", strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		unsigned long s, e;
		char perms[8];
		if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) != 3)
			continue;
		if (strstr(line, "[vdso]") && s == ehdr && strcmp(perms, "r-xp") == 0)
			vdso_ok = 1;
		if (strstr(line, "[vvar]") && e == ehdr && s == ehdr - PAGE &&
		    strcmp(perms, "r--p") == 0)
			vvar_ok = 1;
	}
	fclose(f);
	if (!vdso_ok || !vvar_ok) {
		snprintf(why, whylen, "[vdso] r-xp at 0x%lx %s, [vvar] r--p below it %s",
		         ehdr, vdso_ok ? "found" : "missing", vvar_ok ? "found" : "missing");
		return -1;
	}
	return 0;
}

static long long ts_ns(const struct timespec *t)
{
	return (long long)t->tv_sec * 1000000000LL + t->tv_nsec;
}

static const struct { clockid_t id; const char *name; } k_fast[] = {
	{CLOCK_REALTIME, "realtime"},
	{CLOCK_MONOTONIC, "monotonic"},
	{CLOCK_MONOTONIC_RAW, "monotonic-raw"},
	{CLOCK_BOOTTIME, "boottime"},
};

static void test_agree(void)
{
	for (size_t c = 0; c < sizeof(k_fast) / sizeof(k_fast[0]); c++) {
		long long worst = 0;
		int bad_at = -1;
		long long lo = 0, mid = 0, hi = 0;

		for (int i = 0; i < 2000; i++) {
			struct timespec a, v, l, b;
			if (syscall(SYS_clock_gettime, k_fast[c].id, &a) != 0 ||
			    g_cgt(k_fast[c].id, &v) != 0 ||
			    clock_gettime(k_fast[c].id, &l) != 0 ||
			    syscall(SYS_clock_gettime, k_fast[c].id, &b) != 0) {
				bad_at = i;
				break;
			}
			if (ts_ns(&a) > ts_ns(&v) || ts_ns(&v) > ts_ns(&l) ||
			    ts_ns(&l) > ts_ns(&b) || v.tv_nsec >= 1000000000L) {
				bad_at = i;
				lo = ts_ns(&a); mid = ts_ns(&v); hi = ts_ns(&b);
				break;
			}
			if (ts_ns(&b) - ts_ns(&a) > worst)
				worst = ts_ns(&b) - ts_ns(&a);
		}
		if (bad_at >= 0)
			bad("agree-%s (read %d: syscall %lld, vdso %lld, syscall %lld)",
			    k_fast[c].name, bad_at, lo, mid, hi);
		else
			ok("agree-%s bracket-max-ns=%lld", k_fast[c].name, worst);
	}
}

static void test_agree_other(void)
{
	char why[160] = "";

	/* gettimeofday: microseconds of the same wall clock. */
	for (int i = 0; i < 1000 && !why[0]; i++) {
		struct timespec a, b;
		struct timeval tv;
		struct { int mw, dst; } tz = {-1, -1};
		syscall(SYS_clock_gettime, CLOCK_REALTIME, &a);
		int r = g_gtod(&tv, &tz);
		syscall(SYS_clock_gettime, CLOCK_REALTIME, &b);
		long long us = (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
		if (r != 0 || us < ts_ns(&a) / 1000 || us > ts_ns(&b) / 1000 ||
		    tz.mw != 0 || tz.dst != 0)
			snprintf(why, sizeof(why), "gettimeofday r=%d us=%lld window=[%lld,%lld] tz=%d/%d",
			         r, us, ts_ns(&a) / 1000, ts_ns(&b) / 1000, tz.mw, tz.dst);
	}
#ifdef SYM_TIME
	for (int i = 0; i < 1000 && !why[0]; i++) {
		struct timespec a, b;
		time_t out = 0;
		syscall(SYS_clock_gettime, CLOCK_REALTIME, &a);
		time_t t = g_time(&out);
		syscall(SYS_clock_gettime, CLOCK_REALTIME, &b);
		if (t != out || t < a.tv_sec || t > b.tv_sec)
			snprintf(why, sizeof(why), "time %lld (stored %lld) outside [%lld,%lld]",
			         (long long)t, (long long)out, (long long)a.tv_sec,
			         (long long)b.tv_sec);
	}
#endif
	/* clock_getres: the same answer for every clock id, including the error. */
	for (int id = -1; id <= 12 && !why[0]; id++) {
		struct timespec k = {7, 7}, v = {7, 7};
		long kr = syscall(SYS_clock_getres, id, &k);
		int ke = kr ? errno : 0;
		int vr = g_getres(id, &v);
		if ((kr == 0) != (vr == 0) || (kr != 0 && -vr != ke) ||
		    (kr == 0 && (k.tv_sec != v.tv_sec || k.tv_nsec != v.tv_nsec)))
			snprintf(why, sizeof(why), "clock_getres(%d) syscall=%ld/%d {%lld,%ld} vdso=%d {%lld,%ld}",
			         id, kr, ke, (long long)k.tv_sec, k.tv_nsec, vr,
			         (long long)v.tv_sec, v.tv_nsec);
	}
	/* Clocks the vDSO hands to the kernel still answer, errors included. */
	if (!why[0]) {
		struct timespec cpu = {0, 0}, coarse, kcoarse;
		int r1 = g_cgt(CLOCK_PROCESS_CPUTIME_ID, &cpu);
		int r2 = g_cgt(CLOCK_MONOTONIC_COARSE, &coarse);
		syscall(SYS_clock_gettime, CLOCK_MONOTONIC_COARSE, &kcoarse);
		int r3 = g_cgt(CLOCK_MONOTONIC, NULL);
		/* An id the vDSO does not know gets whatever the kernel answers. */
		struct timespec u;
		long k4 = syscall(SYS_clock_gettime, 100, &u);
		int k4e = k4 ? errno : 0;
		int r4 = g_cgt(100, &u);
		if (r1 != 0 || ts_ns(&cpu) <= 0 || r2 != 0 || ts_ns(&coarse) > ts_ns(&kcoarse) ||
		    r3 != -EFAULT || (k4 == 0 ? r4 != 0 : r4 != -k4e))
			snprintf(why, sizeof(why), "fallback cpu=%d/%lld coarse=%d null=%d id100=%d (kernel %ld/%d)",
			         r1, ts_ns(&cpu), r2, r3, r4, k4, k4e);
	}
	if (why[0])
		bad("agree-other (%s)", why);
	else
		ok("agree-other");
}

/* ── no system call ─────────────────────────────────────────────────────────── */

static int install_time_filter(void)
{
	struct sock_filter prog[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0), /* A = seccomp_data.nr */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clock_gettime, 3, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_gettimeofday, 2, 0),
#ifdef SYS_time
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_time, 1, 0),
#else
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xffffffffu, 1, 0),
#endif
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOTRECOVERABLE),
	};
	struct sock_fprog fprog = {.len = sizeof(prog) / sizeof(prog[0]), .filter = prog};

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
		return -1;
	return (int)syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fprog);
}

static void test_no_syscall(void)
{
	struct timespec res;

	/* Only a counter-based clock source can be read without the kernel; the
	 * tick cannot. The kernel says which it is through the resolution. */
	if (clock_getres(CLOCK_MONOTONIC, &res) != 0 || res.tv_sec != 0 || res.tv_nsec != 1) {
		bad("no-syscall (clock source is not the counter: resolution %lld ns)",
		    ts_ns(&res));
		return;
	}

	pid_t c = fork();
	if (c < 0) {
		bad("no-syscall (fork: %s)", strerror(errno));
		return;
	}
	if (c == 0) {
		if (install_time_filter() != 0)
			_exit(10);
		struct timespec t;
		errno = 0;
		if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &t) != -1 ||
		    errno != ENOTRECOVERABLE)
			_exit(11); /* the filter is not in force: nothing would be proven */
		long long prev = 0;
		for (int i = 0; i < 100000; i++) {
			for (size_t k = 0; k < sizeof(k_fast) / sizeof(k_fast[0]); k++) {
				if (clock_gettime(k_fast[k].id, &t) != 0)
					_exit(20 + (int)k);
			}
			if (clock_gettime(CLOCK_MONOTONIC, &t) != 0 || ts_ns(&t) < prev)
				_exit(30);
			prev = ts_ns(&t);
		}
		struct timeval tv;
		if (gettimeofday(&tv, NULL) != 0 || tv.tv_sec < 1)
			_exit(40);
		if (time(NULL) == (time_t)-1)
			_exit(41);
		_exit(0);
	}
	int st = 0;
	if (waitpid(c, &st, 0) != c) {
		bad("no-syscall (waitpid: %s)", strerror(errno));
		return;
	}
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
		ok("no-syscall calls=500000 filter=clock_gettime,gettimeofday%s",
#ifdef SYS_time
		   ",time"
#else
		   ""
#endif
		);
	else
		bad("no-syscall (child status 0x%x: 10 filter, 11 filter inert, 2x libc clock_gettime entered the kernel, 30 went back, 40 gettimeofday, 41 time)",
		    st);
}

/* ── monotonicity ───────────────────────────────────────────────────────────── */

static void test_monotonic(void)
{
	static const clockid_t ids[] = {CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW, CLOCK_BOOTTIME};
	for (size_t k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) {
		struct timespec t;
		long long prev = -1;
		for (int i = 0; i < 200000; i++) {
			if (clock_gettime(ids[k], &t) != 0) {
				bad("monotonic (clock %d read %d failed: %s)", (int)ids[k], i, strerror(errno));
				return;
			}
			if (ts_ns(&t) < prev) {
				bad("monotonic (clock %d read %d: %lld after %lld)", (int)ids[k], i,
				    ts_ns(&t), prev);
				return;
			}
			prev = ts_ns(&t);
		}
	}
	ok("monotonic reads=600000");
}

static pthread_mutex_t g_mono_lock = PTHREAD_MUTEX_INITIALIZER;
static long long g_mono_last;
static int g_mono_back;
static long long g_mono_back_by;
static unsigned long g_cpus_seen;

struct mono_arg {
	int cpu;
	int iters;
	int pinned; /* sched_setaffinity to `cpu` was accepted */
};

static void *mono_thread(void *p)
{
	struct mono_arg *a = p;
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(a->cpu, &set);
	/* A CPU this process may not run on is refused (EINVAL); the thread then
	 * runs wherever it can, and only accepted pins are held to account. */
	a->pinned = sched_setaffinity(0, sizeof(set), &set) == 0;
	for (int i = 0; i < a->iters; i++) {
		struct timespec t;
		pthread_mutex_lock(&g_mono_lock);
		clock_gettime(CLOCK_MONOTONIC, &t);
		if (ts_ns(&t) < g_mono_last) {
			g_mono_back++;
			if (g_mono_last - ts_ns(&t) > g_mono_back_by)
				g_mono_back_by = g_mono_last - ts_ns(&t);
		} else {
			g_mono_last = ts_ns(&t);
		}
		int cpu = sched_getcpu();
		if (cpu >= 0 && cpu < 64)
			g_cpus_seen |= 1ul << cpu;
		pthread_mutex_unlock(&g_mono_lock);
	}
	return NULL;
}

static void test_monotonic_threads(void)
{
	enum { NT = 4 };
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	pthread_t th[NT];
	struct mono_arg args[NT];

	if (ncpu < 1)
		ncpu = 1;
	for (int i = 0; i < NT; i++) {
		args[i].cpu = (int)(i % ncpu);
		args[i].iters = 20000;
		if (pthread_create(&th[i], NULL, mono_thread, &args[i]) != 0) {
			bad("monotonic-threads (pthread_create: %s)", strerror(errno));
			for (int k = 0; k < i; k++)
				pthread_join(th[k], NULL);
			return;
		}
	}
	for (int i = 0; i < NT; i++)
		pthread_join(th[i], NULL);
	unsigned long pinned = 0;
	for (int i = 0; i < NT; i++)
		if (args[i].pinned)
			pinned |= 1ul << args[i].cpu;
	if (g_mono_back)
		bad("monotonic-threads (%d reads went back, by up to %lld ns)", g_mono_back,
		    g_mono_back_by);
	else if ((g_cpus_seen & pinned) != pinned)
		bad("monotonic-threads (threads pinned to cpus 0x%lx, readings seen on 0x%lx)",
		    pinned, g_cpus_seen);
	else
		ok("monotonic-threads threads=%d cpus-online=%ld cpus-seen=%d", NT, ncpu,
		   __builtin_popcountl(g_cpus_seen));
}

/* ── inheritance and protection ─────────────────────────────────────────────── */

/* What a process that received a vDSO checks about its own. 0 = all good. */
static int self_check(unsigned long expect_ehdr)
{
	unsigned long ehdr = getauxval(AT_SYSINFO_EHDR);
	char why[160];
	struct timespec a, v;

	if (!ehdr || (expect_ehdr && ehdr != expect_ehdr))
		return 2;
	if (check_maps(ehdr, why, sizeof(why)) != 0)
		return 3;
	cgt_fn f = vdso_lookup((const Elf64_Ehdr *)ehdr, SYM_CGT, VDSO_VERSION, NULL);
	if (!f)
		return 4;
	if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &a) != 0 || f(CLOCK_MONOTONIC, &v) != 0 ||
	    ts_ns(&v) < ts_ns(&a))
		return 5;
	return 0;
}

static int wait_child(pid_t c)
{
	int st = 0;
	if (c < 0 || waitpid(c, &st, 0) != c)
		return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : 0x100 | (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static void test_fork(void)
{
	unsigned long ehdr = getauxval(AT_SYSINFO_EHDR);
	pid_t c = fork();
	if (c == 0)
		_exit(self_check(ehdr));
	int rc = wait_child(c);

	if (rc != 0) {
		bad("fork (child check %d: 2 auxv, 3 maps, 4 symbol, 5 reading)", rc);
		return;
	}
	/* A child that throws both mappings away: the kernel's frames must survive
	 * it, and the parent's mapping of them must keep working. */
	c = fork();
	if (c == 0) {
		if (munmap((void *)(ehdr - PAGE), PAGE * 2) != 0)
			_exit(6);
		struct timespec t;
		if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &t) != 0)
			_exit(7);
		_exit(0);
	}
	rc = wait_child(c);
	struct timespec a, v;
	if (rc != 0 || syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &a) != 0 ||
	    g_cgt(CLOCK_MONOTONIC, &v) != 0 || ts_ns(&v) < ts_ns(&a)) {
		bad("fork (unmapping child %d, or the parent's vDSO broke after it)", rc);
		return;
	}
	ok("fork");
}

static void test_exec(const char *self)
{
	pid_t c = fork();
	if (c == 0) {
		execl(self, self, "--exec-child", (char *)NULL);
		_exit(9);
	}
	int rc = wait_child(c);
	if (rc != 0)
		bad("exec (child check %d: 2 auxv, 3 maps, 4 symbol, 5 reading, 9 exec failed)", rc);
	else
		ok("exec");
}

static void test_mprotect(void)
{
	unsigned long ehdr = getauxval(AT_SYSINFO_EHDR);
	char why[160] = "";

	errno = 0;
	if (mprotect((void *)(ehdr - PAGE), PAGE, PROT_READ | PROT_WRITE) == 0 || errno != EACCES)
		snprintf(why, sizeof(why), "[vvar] writable: errno %d", errno);
	errno = 0;
	if (!why[0] &&
	    (mprotect((void *)(ehdr - PAGE), PAGE, PROT_READ | PROT_EXEC) == 0 || errno != EACCES))
		snprintf(why, sizeof(why), "[vvar] executable: errno %d", errno);
	errno = 0;
	if (!why[0] && (mprotect((void *)ehdr, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC) == 0 ||
	                errno != EACCES))
		snprintf(why, sizeof(why), "[vdso] writable: errno %d", errno);
	struct timespec t;
	if (!why[0] && (g_cgt(CLOCK_MONOTONIC, &t) != 0 || check_maps(ehdr, why, sizeof(why)) != 0))
		snprintf(why, sizeof(why), "vDSO changed by a refused mprotect");
	if (why[0])
		bad("mprotect (%s)", why);
	else
		ok("mprotect");
}

/* ── cost ───────────────────────────────────────────────────────────────────── */

static void test_cost(void)
{
	enum { N = 200000 };
	struct timespec t, s0, s1;

	clock_gettime(CLOCK_MONOTONIC, &s0);
	for (int i = 0; i < N; i++)
		clock_gettime(CLOCK_MONOTONIC, &t);
	clock_gettime(CLOCK_MONOTONIC, &s1);
	long long vdso_ns = (ts_ns(&s1) - ts_ns(&s0)) / N;

	clock_gettime(CLOCK_MONOTONIC, &s0);
	for (int i = 0; i < N; i++)
		syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &t);
	clock_gettime(CLOCK_MONOTONIC, &s1);
	long long sys_ns = (ts_ns(&s1) - ts_ns(&s0)) / N;

	if (vdso_ns < sys_ns)
		ok("cost vdso-ns-per-call=%lld syscall-ns-per-call=%lld", vdso_ns, sys_ns);
	else
		bad("cost (vdso %lld ns per call is not cheaper than the syscall's %lld)",
		    vdso_ns, sys_ns);
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--exec-child") == 0)
		return self_check(0);

	printf("VDSO-SMOKE: start\n");
	fflush(stdout);
	if (test_auxv() != 0) {
		printf("VDSO-SMOKE: done fail=1\n");
		return 1;
	}
	{
		char why[160];
		unsigned long ehdr = getauxval(AT_SYSINFO_EHDR);
		if (check_maps(ehdr, why, sizeof(why)) != 0)
			bad("maps (%s)", why);
		else
			ok("maps");
	}
	test_agree();
	test_agree_other();
	test_no_syscall();
	test_monotonic();
	test_monotonic_threads();
	test_fork();
	test_exec(argv[0][0] == '/' ? argv[0] : "/proc/self/exe");
	test_mprotect();
	test_cost();
	printf("VDSO-SMOKE: done fail=%d\n", g_fail);
	return g_fail;
}
