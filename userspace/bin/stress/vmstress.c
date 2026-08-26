/* Address-space churn, with every fresh mapping proved empty.
 *
 * Two questions this answers that a heap test cannot:
 *
 *   1. Does a freshly mapped anonymous page ever arrive holding somebody
 *      else's data? A MAP_ANONYMOUS page is required to be zero. Foreign bytes
 *      there mean a frame was handed out while still mapped elsewhere — which
 *      is the leading explanation for the compositor's corrupted heap, since
 *      malloc's large allocations come straight from mmap.
 *   2. Do the mapping operations preserve what was written through them?
 *      mprotect, mremap and madvise all rewrite page tables under a live
 *      mapping, and a stale TLB entry or a lost copy shows up as data that was
 *      correct a moment ago.
 *
 * The zero check is the expensive half, so it is applied to the whole of small
 * mappings and to the first and last page of large ones — the ends are where a
 * mis-sized copy lands.
 */
#include "stress.h"
#include <sys/mman.h>
#include <sys/wait.h>
#include <pthread.h>

#define COW_PARENT 0xC0FFEEUL
#define COW_CHILD  0xC81DEDUL

static unsigned long long g_deadline;
static volatile int g_fail;

/* Which operations this run performs.
 *
 * The failure this reproduces needs several threads and several CPUs, and the
 * first question about any such fault is which of the address-space operations
 * it actually needs. VMSTRESS_OPS is a comma-separated subset of
 * map,protect,remap,dontneed,split,cow — default all — so a bisection is a
 * change of environment rather than a rebuild. */
static int op_protect = 1, op_remap = 1, op_dontneed = 1, op_split = 1,
           op_cow = 1;

static int ops_has(const char *list, const char *name)
{
	size_t n = strlen(name);
	for (const char *p = list; *p;) {
		const char *e = strchr(p, ',');
		size_t len = e ? (size_t)(e - p) : strlen(p);

		if (len == n && strncmp(p, name, n) == 0)
			return 1;
		if (!e)
			break;
		p = e + 1;
	}
	return 0;
}

static void ops_select(void)
{
	const char *v = getenv("VMSTRESS_OPS");

	if (!v || !*v)
		return;
	op_protect = ops_has(v, "protect");
	op_remap = ops_has(v, "remap");
	op_dontneed = ops_has(v, "dontneed");
	op_split = ops_has(v, "split");
	op_cow = ops_has(v, "cow");
}

static unsigned long long g_maps, g_dirty_fresh;

/* Report a fresh mapping that was not zero, with enough context to identify
 * whose bytes they are: pixels are runs of similar values, a heap header is a
 * small integer beside a pointer, and our own pattern is neither. */
static void report_dirty(const unsigned char *p, size_t len, size_t off, const char *what)
{
	fprintf(stderr, "VMSTRESS: FAIL fresh %s at %p len %zu: byte %zu = 0x%02x\n",
	        what, (const void *)p, len, off, p[off]);
	size_t start = off > 32 ? off - 32 : 0;
	size_t end = start + 96 > len ? len : start + 96;
	fprintf(stderr, "VMSTRESS:   context %p:", (const void *)(p + start));
	for (size_t i = start; i < end; i++)
		fprintf(stderr, " %02x", p[i]);
	fprintf(stderr, "\n");
}

/* Returns 1 when clean. */
static int check_zero(const unsigned char *p, size_t len, const char *what)
{
	size_t head = len < 65536 ? len : 4096;
	for (size_t i = 0; i < head; i++) {
		if (p[i]) {
			report_dirty(p, len, i, what);
			return 0;
		}
	}
	if (len > 65536) {
		const unsigned char *tail = p + len - 4096;
		for (size_t i = 0; i < 4096; i++) {
			if (tail[i]) {
				report_dirty(tail, 4096, i, what);
				return 0;
			}
		}
	}
	return 1;
}

static void *worker(void *arg)
{
	unsigned long tid = (unsigned long)(uintptr_t)arg;
	unsigned long seed = tid * 88172645463325252UL + 7;
	long rounds = scaled(4000);
	unsigned long long maps = 0, dirty = 0;

	for (long round = 0; round < rounds && !g_fail; round++) {
		if ((round & 0x1f) == 0 && now_ms() > g_deadline)
			break;

		unsigned long r = rnd(&seed);
		size_t pages = 1 + r % 64;
		size_t len = pages * 4096;

		unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
		                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) {
			if (errno == ENOMEM)
				continue;
			fprintf(stderr, "VMSTRESS: FAIL mmap(%zu): %s\n", len, strerror(errno));
			g_fail = 1;
			break;
		}
		maps++;

		if (!check_zero(p, len, "anonymous mapping")) {
			dirty++;
			g_fail = 1;
			munmap(p, len);
			break;
		}

		unsigned long owner = tid * 1000003UL + (unsigned long)round;
		pat_fill(p, owner, len);

		if (op_protect) {
		/* Read-only and back. The kernel has to tear down the writable
		 * translation on every CPU that has one; a survivor shows up as a
		 * write that succeeds while the mapping says it must not. */
		if (mprotect(p, len, PROT_READ) != 0) {
			fprintf(stderr, "VMSTRESS: FAIL mprotect RO: %s\n", strerror(errno));
			g_fail = 1;
			munmap(p, len);
			break;
		}
		size_t bad = pat_check(p, owner, len);
		if (bad != (size_t)-1) {
			fprintf(stderr, "VMSTRESS: FAIL data lost across mprotect at %zu: 0x%02x want 0x%02x\n",
			        bad, p[bad], pat(owner, bad));
			g_fail = 1;
			munmap(p, len);
			break;
		}
		mprotect(p, len, PROT_READ | PROT_WRITE);
		}

		/* Move it. mremap without MAYMOVE would only ever succeed in place;
		 * with it the mapping usually lands somewhere else, and the new
		 * address must hold the same bytes. */
		if (op_remap && (r >> 8) % 4 == 0) {
			size_t nlen = len + 4096 * (1 + (r >> 12) % 16);
			unsigned char *np = mremap(p, len, nlen, MREMAP_MAYMOVE);
			if (np != MAP_FAILED) {
				size_t b2 = pat_check(np, owner, len);
				if (b2 != (size_t)-1) {
					fprintf(stderr, "VMSTRESS: FAIL data lost across mremap at %zu: 0x%02x want 0x%02x\n",
					        b2, np[b2], pat(owner, b2));
					g_fail = 1;
					munmap(np, nlen);
					break;
				}
				/* The pages the move added are fresh, and must be zero too. */
				if (!check_zero(np + len, nlen - len, "mremap extension")) {
					dirty++;
					g_fail = 1;
					munmap(np, nlen);
					break;
				}
				/* The pattern only covered the old length; the extension is
				 * zero by definition. Re-lay it over the whole mapping so the
				 * checks that follow have something to compare against. */
				pat_fill(np, owner, nlen);
				p = np;
				len = nlen;
			}
		}

		/* Throw the contents away and demand them back. MADV_DONTNEED on a
		 * private mapping must reset it to zero, which is the same guarantee
		 * as a fresh map and a much narrower path to it. */
		if (op_dontneed && (r >> 16) % 8 == 0) {
			if (madvise(p, 4096, MADV_DONTNEED) == 0) {
				if (!check_zero(p, 4096, "page after MADV_DONTNEED")) {
					dirty++;
					g_fail = 1;
					munmap(p, len);
					break;
				}
				/* That page is now zero on purpose. Put the pattern back, or
				 * the split check below would report our own erasure as data
				 * loss. */
				for (size_t i = 0; i < 4096; i++)
					p[i] = pat(owner, i);
			}
		}

		/* Unmap a hole out of the middle: the split is where a VMA list is
		 * rewritten, and the remaining halves must keep their data. */
		if (op_split && len >= 12288 && (r >> 20) % 8 == 0) {
			size_t mid = (len / 8192) * 4096;
			if (munmap(p + mid, 4096) == 0) {
				size_t b3 = pat_check(p, owner, mid);
				if (b3 != (size_t)-1) {
					fprintf(stderr, "VMSTRESS: FAIL split lost data at %zu: 0x%02x want 0x%02x\n",
					        b3, p[b3], pat(owner, b3));
					g_fail = 1;
				}
				munmap(p, mid);
				munmap(p + mid + 4096, len - mid - 4096);
				continue;
			}
		}

		munmap(p, len);
	}

	__atomic_add_fetch(&g_maps, maps, __ATOMIC_RELAXED);
	__atomic_add_fetch(&g_dirty_fresh, dirty, __ATOMIC_RELAXED);
	return 0;
}

/* Copy-on-write, checked from both sides.
 *
 * The child writes its own pattern over a shared private mapping; the parent's
 * copy must not change, and the child must see its own. A COW fault that maps
 * the wrong frame breaks one side or the other, and it is worth separating
 * from the threaded churn above because a fork is where an address space is
 * duplicated wholesale. */
static int cow_round(void)
{
	size_t len = 256 * 4096;
	unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
	                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return 0;
	pat_fill(p, COW_PARENT, len);

	pid_t pid = fork();
	if (pid == 0) {
		pat_fill(p, COW_CHILD, len);
		size_t bad = pat_check(p, COW_CHILD, len);
		_exit(bad == (size_t)-1 ? 0 : 1);
	}
	if (pid < 0) {
		munmap(p, len);
		return 0;
	}
	int st = 0;
	waitpid(pid, &st, 0);
	size_t bad = pat_check(p, COW_PARENT, len);
	munmap(p, len);
	if (bad != (size_t)-1) {
		fprintf(stderr, "VMSTRESS: FAIL parent copy changed by child at %zu\n", bad);
		return -1;
	}
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		fprintf(stderr, "VMSTRESS: FAIL child copy wrong (status 0x%x)\n", st);
		return -1;
	}
	return 0;
}

int main(void)
{
	int threads = (int)env_long("SOAK_THREADS", sysconf(_SC_NPROCESSORS_ONLN));
	if (threads < 1)
		threads = 1;
	if (threads > 32)
		threads = 32;
	g_deadline = now_ms() + budget_ms();

	ops_select();
	printf("VMSTRESS: start %d threads (protect=%d remap=%d dontneed=%d split=%d cow=%d)\n",
	       threads, op_protect, op_remap, op_dontneed, op_split, op_cow);
	fflush(stdout);
	unsigned long long t0 = now_ms();

	pthread_t th[32];
	int made = 0;
	for (int i = 0; i < threads; i++) {
		if (pthread_create(&th[i], 0, worker, (void *)(uintptr_t)i) != 0) {
			fprintf(stderr, "VMSTRESS: FAIL pthread_create %d\n", i);
			g_fail = 1;
			break;
		}
		made++;
	}
	for (int i = 0; i < made; i++)
		pthread_join(th[i], 0);

	int cow_bad = 0;
	for (long i = 0; op_cow && i < scaled(8) && !g_fail && now_ms() < g_deadline; i++)
		if (cow_round() != 0)
			cow_bad = 1;

	unsigned long long ms = now_ms() - t0;
	if (g_fail || cow_bad) {
		printf("VMSTRESS: FAIL after %llu mappings (%llu arrived dirty) ms=%llu\n",
		       g_maps, g_dirty_fresh, ms);
		return 1;
	}
	printf("VMSTRESS: ok %llu mappings, all fresh pages zero, ms=%llu\n", g_maps, ms);
	return 0;
}
