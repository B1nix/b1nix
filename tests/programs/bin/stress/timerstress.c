/* What a timer actually waits, measured against the clock it was set from.
 *
 * A compositor asks for the next frame in sixteen milliseconds and then waits
 * on a timerfd. If that timer comes back early — or immediately, over and over
 * — the compositor repaints in a tight loop and never gets round to its
 * clients or its IPC, which from outside looks exactly like a hang. Inferring
 * that from the compositor's behaviour takes a boot per guess; measuring the
 * timer takes one run.
 *
 * Three shapes, because they fail differently:
 *
 *   relative   timerfd_settime with a delay. The straightforward case.
 *   absolute   TFD_TIMER_ABSTIME with a deadline read from CLOCK_MONOTONIC —
 *              the shape a frame scheduler uses, and the one that breaks when
 *              the kernel resolves the deadline against a different clock.
 *   periodic   an interval timer, checked for drift and for firing early.
 *
 * Every wait is reported as measured against CLOCK_MONOTONIC, so an early
 * return is a number rather than an impression.
 */
#include "stress.h"
#include <fcntl.h>
#include <sys/timerfd.h>
#include <poll.h>

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Wait for one expiry; returns the nanoseconds it actually took, or 0 on
 * error. */
static uint64_t wait_one(int fd, uint64_t started)
{
	uint64_t ticks = 0;
	ssize_t n = read(fd, &ticks, sizeof(ticks));

	if (n != (ssize_t)sizeof(ticks))
		return 0;
	return now_ns() - started;
}

int main(void)
{
	int fails = 0;
	long rounds = scaled(20);

	if (rounds > 200)
		rounds = 200;
	printf("TIMERSTRESS: start %ld rounds\n", rounds);
	fflush(stdout);

	int fd = timerfd_create(CLOCK_MONOTONIC, 0);

	if (fd < 0) {
		printf("TIMERSTRESS: FAIL timerfd_create: %s\n", strerror(errno));
		return 1;
	}

	/* ── relative ── */
	{
		uint64_t total = 0, worst_early = 0;

		for (long i = 0; i < rounds; i++) {
			struct itimerspec its;

			memset(&its, 0, sizeof(its));
			its.it_value.tv_nsec = 16000000L; /* 16 ms, one frame at 60 Hz */
			uint64_t t0 = now_ns();

			if (timerfd_settime(fd, 0, &its, 0) != 0) {
				printf("TIMERSTRESS: FAIL settime relative: %s\n", strerror(errno));
				return 1;
			}
			uint64_t took = wait_one(fd, t0);

			if (took == 0) {
				printf("TIMERSTRESS: FAIL read relative\n");
				return 1;
			}
			total += took;
			if (took < 16000000ull && 16000000ull - took > worst_early)
				worst_early = 16000000ull - took;
		}
		printf("TIMERSTRESS: relative 16ms mean=%lluus worst-early=%lluus\n",
		       (unsigned long long)(total / (uint64_t)rounds / 1000),
		       (unsigned long long)(worst_early / 1000));
		if (worst_early > 2000000ull) { /* more than 2 ms early is broken */
			printf("TIMERSTRESS: FAIL relative timer fires early\n");
			fails++;
		}
	}

	/* ── absolute, the frame-scheduler shape ── */
	{
		uint64_t total = 0, immediate = 0;

		for (long i = 0; i < rounds; i++) {
			struct itimerspec its;
			struct timespec now;

			clock_gettime(CLOCK_MONOTONIC, &now);
			memset(&its, 0, sizeof(its));
			its.it_value = now;
			its.it_value.tv_nsec += 16000000L;
			if (its.it_value.tv_nsec >= 1000000000L) {
				its.it_value.tv_sec++;
				its.it_value.tv_nsec -= 1000000000L;
			}
			uint64_t t0 = now_ns();

			if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, 0) != 0) {
				printf("TIMERSTRESS: FAIL settime absolute: %s\n", strerror(errno));
				return 1;
			}
			uint64_t took = wait_one(fd, t0);

			if (took == 0) {
				printf("TIMERSTRESS: FAIL read absolute\n");
				return 1;
			}
			total += took;
			if (took < 4000000ull)
				immediate++;
		}
		printf("TIMERSTRESS: absolute 16ms mean=%lluus fired-at-once=%llu/%ld\n",
		       (unsigned long long)(total / (uint64_t)rounds / 1000),
		       (unsigned long long)immediate, rounds);
		if (immediate > (uint64_t)rounds / 4) {
			printf("TIMERSTRESS: FAIL absolute deadlines fire immediately — a frame"
			       " scheduler built on this spins\n");
			fails++;
		}
	}

	/* ── periodic ── */
	{
		struct itimerspec its;

		memset(&its, 0, sizeof(its));
		its.it_value.tv_nsec = 16000000L;
		its.it_interval.tv_nsec = 16000000L;
		uint64_t t0 = now_ns();

		if (timerfd_settime(fd, 0, &its, 0) != 0) {
			printf("TIMERSTRESS: FAIL settime periodic: %s\n", strerror(errno));
			return 1;
		}
		long n = rounds < 30 ? rounds : 30;

		for (long i = 0; i < n; i++) {
			if (wait_one(fd, t0) == 0) {
				printf("TIMERSTRESS: FAIL read periodic\n");
				return 1;
			}
		}
		uint64_t elapsed = now_ns() - t0;
		uint64_t expect = 16000000ull * (uint64_t)n;

		printf("TIMERSTRESS: periodic %ld ticks in %llums (expected %llums)\n", n,
		       (unsigned long long)(elapsed / 1000000),
		       (unsigned long long)(expect / 1000000));
		if (elapsed < expect / 2) {
			printf("TIMERSTRESS: FAIL periodic timer runs fast\n");
			fails++;
		}
	}

	close(fd);
	if (fails) {
		printf("TIMERSTRESS: FAIL %d checks\n", fails);
		return 1;
	}
	printf("TIMERSTRESS: ok\n");
	return 0;
}
