/* Does yielding the CPU actually let someone else run?
 *
 * A thread that cannot proceed until another thread does something often waits
 * by spinning and yielding rather than by sleeping — every large program does
 * this somewhere. That is only correct if yielding really hands the processor
 * to a thread that is ready to run. If it returns to the caller while a ready
 * thread stays unscheduled, the spinner starves the very thread it is waiting
 * for: both are runnable, neither progresses, and nothing in the system reports
 * an error. From outside it looks like a process burning CPU and going nowhere,
 * which is what a stalled browser looks like here.
 *
 * The test is deliberately harsh: the waiter yields in a tight loop while the
 * worker only has to run once. On more threads than CPUs, so the scheduler has
 * to make a real choice.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

#define SPINNERS 6

static volatile int worker_ran;
static volatile int stop;
static volatile unsigned long long spins;

static void *worker(void *arg)
{
	(void)arg;
	/* Nothing clever: just be runnable and set a flag. Whether this thread ever
	 * gets the processor is the entire question. */
	worker_ran = 1;
	return 0;
}

static void *spinner(void *arg)
{
	(void)arg;
	while (!stop && !worker_ran) {
		__atomic_fetch_add(&spins, 1, __ATOMIC_RELAXED);
		sched_yield();
	}
	return 0;
}

static long long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(void)
{
	pthread_t sp[SPINNERS], w;
	long long start;
	int made = 0;
	int bad = 0;

	setvbuf(stdout, 0, _IONBF, 0);
	printf("YIELD: start\n");

	for (int i = 0; i < SPINNERS; i++)
		if (pthread_create(&sp[i], 0, spinner, 0) == 0)
			made++;
	if (made != SPINNERS) {
		printf("YIELD: FAIL setup (%d of %d spinners)\n", made, SPINNERS);
		return 1;
	}

	/* Let the spinners get going, so the worker starts into a busy system. */
	usleep(100 * 1000);

	start = now_ms();
	if (pthread_create(&w, 0, worker, 0) != 0) {
		printf("YIELD: FAIL setup (cannot create worker)\n");
		return 1;
	}

	while (!worker_ran && now_ms() - start < 5000)
		usleep(1000);

	if (!worker_ran) {
		printf("YIELD: FAIL starvation (worker never ran in 5 s while %d "
		       "threads yielded %llu times)\n", SPINNERS, spins);
		bad = 1;
	} else {
		printf("YIELD: ok worker ran after %lld ms (%llu yields)\n",
		       now_ms() - start, spins);
	}

	stop = 1;
	pthread_join(w, 0);
	for (int i = 0; i < SPINNERS; i++)
		pthread_join(sp[i], 0);

	printf("YIELD: done (%d failed)\n", bad);
	return bad ? 1 : 0;
}
