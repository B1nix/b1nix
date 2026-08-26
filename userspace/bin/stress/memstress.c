/* Heap churn with every block verified.
 *
 * The open compositor failure is musl's own allocator detecting that its
 * metadata has been overwritten, with terminal pixels found inside it — so the
 * writer is some other part of the system writing through a stale or
 * mis-shared mapping, not the allocator. This is that shape of work with none
 * of the graphics: several threads allocating, filling, verifying and freeing
 * blocks of every size class, so a foreign write lands in a block whose
 * expected contents are known exactly.
 *
 * A mismatch prints the owning thread, the slot, the offset and both bytes.
 * That is enough to say whether the damage is a shifted copy of our own
 * pattern (an allocator or memcpy bug) or data belonging to nobody here (a
 * mapping bug).
 */
#include "stress.h"
#include <pthread.h>

#define SLOTS 64

static long g_rounds;
static unsigned long long g_deadline;
static int g_threads;

static volatile int g_fail;
static unsigned long long g_allocs;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

struct blk {
	unsigned char *p;
	size_t len;
	unsigned long owner;
};

static void *worker(void *arg)
{
	unsigned long tid = (unsigned long)(uintptr_t)arg;
	unsigned long seed = tid * 2654435761UL + 12345UL;
	struct blk slot[SLOTS];
	unsigned long long allocs = 0;

	memset(slot, 0, sizeof(slot));

	for (long round = 0; round < g_rounds && !g_fail; round++) {
		if ((round & 0x3f) == 0 && now_ms() > g_deadline)
			break;

		unsigned i = (unsigned)(rnd(&seed) % SLOTS);

		if (slot[i].p) {
			size_t bad = pat_check(slot[i].p, slot[i].owner, slot[i].len);
			if (bad != (size_t)-1) {
				fprintf(stderr,
				        "MEMSTRESS: FAIL thread %lu slot %u off %zu len %zu at %p: read 0x%02x want 0x%02x\n",
				        tid, i, bad, slot[i].len, (void *)(slot[i].p + bad),
				        slot[i].p[bad], pat(slot[i].owner, bad));
				g_fail = 1;
				break;
			}
			free(slot[i].p);
			slot[i].p = 0;
		}

		/* Every size class musl distinguishes: a few bytes, the small bins,
		 * the boundary where it stops using bins at all, and sizes big enough
		 * that each allocation is its own mmap. The last group is the one that
		 * reaches the kernel's VMA code on every call. */
		unsigned long r = rnd(&seed);
		size_t len;
		switch (r % 8) {
		case 0: len = 1 + (r >> 8) % 32;      break;
		case 1: len = 32 + (r >> 8) % 224;    break;
		case 2: len = 256 + (r >> 8) % 768;   break;
		case 3: len = 1024 + (r >> 8) % 3072; break;
		case 4: len = 4096 + (r >> 8) % 12288; break;
		case 5: len = 16384 + (r >> 8) % 49152; break;
		case 6: len = 65536 + (r >> 8) % 65536; break;
		default: len = 131072 + (r >> 8) % 262144; break;
		}

		unsigned char *p = malloc(len);
		if (!p) {
			/* Out of memory is a legitimate answer under this much churn, and
			 * only a failure if it happens with a small request. */
			if (len < 4096) {
				fprintf(stderr, "MEMSTRESS: FAIL thread %lu malloc(%zu) returned NULL\n", tid, len);
				g_fail = 1;
				break;
			}
			continue;
		}
		allocs++;
		unsigned long owner = tid * 1000003UL + (unsigned long)round;
		pat_fill(p, owner, len);
		/* Read it straight back. A mapping that is not really there fails here
		 * rather than a round later, which keeps the reported round useful. */
		size_t bad = pat_check(p, owner, len);
		if (bad != (size_t)-1) {
			fprintf(stderr,
			        "MEMSTRESS: FAIL thread %lu fresh block %p+%zu (len %zu): read 0x%02x want 0x%02x\n",
			        tid, (void *)p, bad, len, p[bad], pat(owner, bad));
			g_fail = 1;
			free(p);
			break;
		}
		slot[i].p = p;
		slot[i].len = len;
		slot[i].owner = owner;

		/* Grow a live block now and then: realloc moves data between size
		 * classes, which is where a copy that runs past the end shows up. */
		if ((r >> 24) % 16 == 0 && slot[i].len < 65536) {
			size_t nlen = slot[i].len * 2 + 17;
			unsigned char *np = realloc(slot[i].p, nlen);
			if (np) {
				size_t b2 = pat_check(np, slot[i].owner, slot[i].len);
				if (b2 != (size_t)-1) {
					fprintf(stderr,
					        "MEMSTRESS: FAIL thread %lu realloc %zu->%zu lost data at %zu: 0x%02x want 0x%02x\n",
					        tid, slot[i].len, nlen, b2, np[b2], pat(slot[i].owner, b2));
					g_fail = 1;
				}
				pat_fill(np, slot[i].owner, nlen);
				slot[i].p = np;
				slot[i].len = nlen;
			}
		}
	}

	for (unsigned i = 0; i < SLOTS; i++) {
		if (!slot[i].p)
			continue;
		size_t bad = pat_check(slot[i].p, slot[i].owner, slot[i].len);
		if (bad != (size_t)-1 && !g_fail) {
			fprintf(stderr,
			        "MEMSTRESS: FAIL thread %lu drain slot %u off %zu: read 0x%02x want 0x%02x\n",
			        tid, i, bad, slot[i].p[bad], pat(slot[i].owner, bad));
			g_fail = 1;
		}
		free(slot[i].p);
	}

	pthread_mutex_lock(&g_lock);
	g_allocs += allocs;
	pthread_mutex_unlock(&g_lock);
	return 0;
}

int main(void)
{
	g_threads = (int)env_long("SOAK_THREADS", sysconf(_SC_NPROCESSORS_ONLN));
	if (g_threads < 1)
		g_threads = 1;
	if (g_threads > 32)
		g_threads = 32;
	g_rounds = scaled(20000);
	g_deadline = now_ms() + budget_ms();

	printf("MEMSTRESS: start %d threads, %ld rounds each\n", g_threads, g_rounds);
	fflush(stdout);

	unsigned long long t0 = now_ms();
	pthread_t th[32];
	int made = 0;
	for (int i = 0; i < g_threads; i++) {
		if (pthread_create(&th[i], 0, worker, (void *)(uintptr_t)i) != 0) {
			fprintf(stderr, "MEMSTRESS: FAIL pthread_create %d: %s\n", i, strerror(errno));
			g_fail = 1;
			break;
		}
		made++;
	}
	for (int i = 0; i < made; i++)
		pthread_join(th[i], 0);

	unsigned long long ms = now_ms() - t0;
	if (g_fail) {
		printf("MEMSTRESS: FAIL after %llu allocations in %llu ms\n", g_allocs, ms);
		return 1;
	}
	printf("MEMSTRESS: ok %llu allocations verified, %d threads, ms=%llu\n", g_allocs, g_threads, ms);
	return 0;
}
