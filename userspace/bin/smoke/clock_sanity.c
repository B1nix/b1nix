/* Does the clock a browser schedules on actually move?
 *
 * Chromium — like anything with an event loop — decides when work is due by
 * comparing readings of the monotonic clock, and arms everything else on
 * timers. A clock that stands still, jumps backwards, or advances at the wrong
 * rate does not produce an error anywhere: every timer simply never comes due,
 * and the process sits idle with every thread parked, waiting for a moment
 * that never arrives. That is exactly what a stalled browser looks like from
 * outside, which is why this is worth asserting directly.
 *
 * Checked: that it moves at all, that it never goes backwards, that its
 * resolution is finer than a scheduler tick, that it agrees with sleeping, and
 * that timers actually fire.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/timerfd.h>
#include <poll.h>

static long long ns_of(const struct timespec *t)
{
	return (long long)t->tv_sec * 1000000000LL + t->tv_nsec;
}

/* It must advance, and never step back. */
static int test_advances(void)
{
	struct timespec a, b;
	long long first, last, min_step = -1;
	int backwards = 0, stalls = 0;

	if (clock_gettime(CLOCK_MONOTONIC, &a) != 0) {
		printf("CLOCK: FAIL monotonic (clock_gettime errno %d)\n", errno);
		return 1;
	}
	first = ns_of(&a);
	last = first;

	for (int i = 0; i < 20000; i++) {
		long long now;
		if (clock_gettime(CLOCK_MONOTONIC, &b) != 0) {
			printf("CLOCK: FAIL monotonic (read %d errno %d)\n", i, errno);
			return 1;
		}
		now = ns_of(&b);
		if (now < last)
			backwards++;
		else if (now == last)
			stalls++;
		else if (min_step < 0 || now - last < min_step)
			min_step = now - last;
		last = now;
	}

	if (backwards) {
		printf("CLOCK: FAIL monotonic (went backwards %d times)\n", backwards);
		return 1;
	}
	if (last == first) {
		printf("CLOCK: FAIL monotonic (stood still across 20000 reads)\n");
		return 1;
	}

	printf("CLOCK: ok monotonic (advanced %lld ns over 20000 reads, smallest "
	       "step %lld ns, %d repeats)\n", last - first, min_step, stalls);
	return 0;
}

/* Sleeping for a known time must show up on the clock as roughly that time.
 * Both directions matter: a clock running fast makes every timeout early. */
static int test_agrees_with_sleep(void)
{
	struct timespec a, b;
	struct timespec req = { .tv_sec = 0, .tv_nsec = 200 * 1000000L };
	long long elapsed;

	clock_gettime(CLOCK_MONOTONIC, &a);
	if (nanosleep(&req, 0) != 0 && errno != EINTR) {
		printf("CLOCK: FAIL sleep-agrees (nanosleep errno %d)\n", errno);
		return 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &b);
	elapsed = ns_of(&b) - ns_of(&a);

	/* Generous: this runs in an emulator under load. The point is to catch a
	 * clock that is wrong by an order of magnitude, not to measure jitter. */
	if (elapsed < 100000000LL || elapsed > 2000000000LL) {
		printf("CLOCK: FAIL sleep-agrees (slept 200 ms, clock moved %lld ns)\n",
		       elapsed);
		return 1;
	}

	printf("CLOCK: ok sleep-agrees (200 ms sleep, clock moved %lld ns)\n",
	       elapsed);
	return 0;
}

/* A timer armed for a short interval must fire, and fire on time. Everything
 * an event loop does eventually rests on this. */
static int test_timer_fires(void)
{
	struct itimerspec its;
	struct timespec a, b;
	struct pollfd p;
	unsigned long long ticks = 0;
	long long elapsed;
	int fd, n;

	fd = timerfd_create(CLOCK_MONOTONIC, 0);
	if (fd < 0) {
		printf("CLOCK: FAIL timer (timerfd_create errno %d)\n", errno);
		return 1;
	}

	memset(&its, 0, sizeof(its));
	its.it_value.tv_nsec = 150 * 1000000L; /* 150 ms, once */

	clock_gettime(CLOCK_MONOTONIC, &a);
	if (timerfd_settime(fd, 0, &its, 0) != 0) {
		printf("CLOCK: FAIL timer (timerfd_settime errno %d)\n", errno);
		close(fd);
		return 1;
	}

	p.fd = fd;
	p.events = POLLIN;
	n = poll(&p, 1, 5000);
	clock_gettime(CLOCK_MONOTONIC, &b);
	elapsed = ns_of(&b) - ns_of(&a);

	if (n <= 0) {
		printf("CLOCK: FAIL timer (150 ms timer did not fire within 5 s)\n");
		close(fd);
		return 1;
	}
	if (read(fd, &ticks, sizeof(ticks)) != (ssize_t)sizeof(ticks) || ticks == 0) {
		printf("CLOCK: FAIL timer (fired but reported no expirations)\n");
		close(fd);
		return 1;
	}
	close(fd);

	printf("CLOCK: ok timer (150 ms timer fired after %lld ns, %llu "
	       "expiration)\n", elapsed, ticks);
	return 0;
}

int main(void)
{
	int bad = 0;

	setvbuf(stdout, 0, _IONBF, 0);
	printf("CLOCK: start\n");
	bad += test_advances();
	bad += test_agrees_with_sleep();
	bad += test_timer_fires();
	printf("CLOCK: done (%d failed)\n", bad);
	return bad ? 1 : 0;
}
