/* Scheduler and timekeeping under more runnable threads than cores.
 *
 * Three separate claims are checked, because a scheduler can fail in ways that
 * do not look like a crash:
 *
 *   progress  — every thread gets time. A thread that never runs looks exactly
 *               like a slow machine until its counter is compared with its
 *               siblings', so the spread between the busiest and the idlest
 *               thread is reported and a thread stuck at zero is a failure.
 *   sleeping  — a sleep of N milliseconds takes at least N. This is not
 *               pedantry: a sleep that returned immediately when nothing else
 *               was runnable once made a display hold of a hundred seconds fly
 *               past in microseconds, and nothing else in the system noticed.
 *   the clock — CLOCK_MONOTONIC never goes backwards, on any CPU. A per-CPU
 *               counter read without a common reference steps backwards when a
 *               thread migrates, and every timeout in userspace is built on it.
 */
#include "stress.h"
#include <pthread.h>
#include <sched.h>

static unsigned long long g_deadline;
static volatile int g_stop;
static volatile int g_fail;

static unsigned long long g_iters[64];
static unsigned long long g_backwards;

/* Handoff pair: two threads passing a token through a mutex and condition
 * variable. Every round is a park and a wake, so this is the futex path as the
 * scheduler sees it rather than as a futex microbenchmark. */
static pthread_mutex_t hand_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hand_c = PTHREAD_COND_INITIALIZER;
static int hand_turn;
static unsigned long long hand_rounds;

static void *handoff(void *arg)
{
	int me = (int)(uintptr_t)arg;
	while (!g_stop) {
		pthread_mutex_lock(&hand_m);
		while (hand_turn != me && !g_stop) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 200000000L;
			if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
			pthread_cond_timedwait(&hand_c, &hand_m, &ts);
		}
		hand_turn = !me;
		if (me == 0)
			hand_rounds++;
		pthread_cond_broadcast(&hand_c);
		pthread_mutex_unlock(&hand_m);
	}
	pthread_mutex_lock(&hand_m);
	hand_turn = 0;
	pthread_cond_broadcast(&hand_c);
	pthread_mutex_unlock(&hand_m);
	return 0;
}

static void *burner(void *arg)
{
	unsigned long idx = (unsigned long)(uintptr_t)arg;
	unsigned long seed = idx * 2654435761UL + 1;
	unsigned long long iters = 0;
	unsigned long long last = now_ms();
	unsigned long acc = 0;

	while (!g_stop) {
		for (int i = 0; i < 4096; i++)
			acc += rnd(&seed);
		iters++;

		unsigned long long t = now_ms();
		if (t < last)
			__atomic_add_fetch(&g_backwards, 1, __ATOMIC_RELAXED);
		last = t;

		/* Give the runqueue a reason to reorder. Without this the test
		 * measures throughput; with it, it measures whether the scheduler
		 * can pick anything else up. */
		if ((iters & 0xf) == 0)
			sched_yield();
	}
	g_iters[idx] = iters + (acc & 1);
	return 0;
}

/* A sleep that returns early is a bug; a sleep that returns very late is a
 * loaded machine. Only the first is failed on, and the measured time is
 * reported so lateness can still be seen. */
static int sleep_check(void)
{
	static const int ms_list[] = { 1, 5, 20, 100 };
	for (unsigned i = 0; i < sizeof(ms_list) / sizeof(ms_list[0]); i++) {
		int ms = ms_list[i];
		struct timespec req = { ms / 1000, (long)(ms % 1000) * 1000000L };
		unsigned long long t0 = now_ms();
		nanosleep(&req, 0);
		unsigned long long took = now_ms() - t0;
		/* One millisecond of slack for the clock's own granularity. */
		if (took + 1 < (unsigned long long)ms) {
			fprintf(stderr, "CPUSTRESS: FAIL nanosleep(%d ms) returned after %llu ms\n", ms, took);
			return -1;
		}
	}
	return 0;
}

int main(void)
{
	long cores = sysconf(_SC_NPROCESSORS_ONLN);
	if (cores < 1)
		cores = 1;
	/* Deliberately more threads than CPUs: the interesting behaviour is what
	 * happens when the runqueue cannot be emptied. */
	int threads = (int)env_long("SOAK_THREADS", cores * 3);
	if (threads < 2)
		threads = 2;
	if (threads > 64)
		threads = 64;

	unsigned long long run_ms = budget_ms();
	if (run_ms > 60000)
		run_ms = 60000;
	g_deadline = now_ms() + run_ms;

	printf("CPUSTRESS: start %d threads on %ld cpus for %llu ms\n", threads, cores, run_ms);
	fflush(stdout);
	unsigned long long t0 = now_ms();

	pthread_t th[64], hb[2];
	int made = 0;
	for (int i = 0; i < threads; i++) {
		if (pthread_create(&th[i], 0, burner, (void *)(uintptr_t)i) != 0) {
			fprintf(stderr, "CPUSTRESS: FAIL pthread_create %d\n", i);
			g_fail = 1;
			break;
		}
		made++;
	}
	int hmade = 0;
	for (int i = 0; i < 2 && !g_fail; i++) {
		if (pthread_create(&hb[i], 0, handoff, (void *)(uintptr_t)i) == 0)
			hmade++;
	}

	if (sleep_check() != 0)
		g_fail = 1;

	while (now_ms() < g_deadline && !g_fail)
		usleep(20000);

	g_stop = 1;
	for (int i = 0; i < hmade; i++)
		pthread_join(hb[i], 0);
	for (int i = 0; i < made; i++)
		pthread_join(th[i], 0);

	unsigned long long lo = ~0ULL, hi = 0, total = 0;
	int starved = 0;
	for (int i = 0; i < made; i++) {
		unsigned long long v = g_iters[i];
		if (v == 0)
			starved++;
		if (v < lo) lo = v;
		if (v > hi) hi = v;
		total += v;
	}
	unsigned long long ms = now_ms() - t0;

	if (starved) {
		fprintf(stderr, "CPUSTRESS: FAIL %d of %d threads never ran\n", starved, made);
		g_fail = 1;
	}
	if (g_backwards) {
		fprintf(stderr, "CPUSTRESS: FAIL monotonic clock stepped backwards %llu times\n", g_backwards);
		g_fail = 1;
	}
	if (hmade == 2 && hand_rounds == 0) {
		fprintf(stderr, "CPUSTRESS: FAIL condition-variable handoff never completed a round\n");
		g_fail = 1;
	}

	if (g_fail) {
		printf("CPUSTRESS: FAIL ms=%llu\n", ms);
		return 1;
	}
	printf("CPUSTRESS: ok %d threads, %llu iters (min %llu max %llu), %llu handoffs, ms=%llu\n",
	       made, total, lo, hi, hand_rounds, ms);
	return 0;
}
