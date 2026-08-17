/* Lost-wakeup torture for the futex path.
 *
 * Written after a browser's main thread was found parked forever on a futex
 * with no timeout while another of its threads had already moved on: the wake
 * either never reached it or arrived while it was between the value check and
 * the sleep. Reproducing that through the browser cost ten minutes a run and
 * mostly measured the tracing; this reproduces the same race directly, in
 * seconds, and says which of the two it is.
 *
 * Three shapes, each hammering the window between "decide to sleep" and
 * "asleep":
 *
 *   handoff  — two threads ping-pong a value, every wake having exactly one
 *              waiter to find. A single lost wake stops the round trip dead.
 *   broadcast— one waker, many waiters, woken all at once. Checks that a wake
 *              of N reaches N and not merely the first.
 *   timed    — waits that carry a deadline, satisfied well before it. Any that
 *              come back as ETIMEDOUT were woken late or not at all, which is
 *              the failure the browser showed.
 *
 * Every wait is bounded so the test reports rather than hangs: a run that
 * proves the bug must still finish.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#define ROUNDS      2000
#define WAITERS     8
#define TIMED_LOOPS 500

static int futex_wait(volatile int *addr, int expect, int ms)
{
	struct timespec ts = { .tv_sec = ms / 1000,
	                       .tv_nsec = (long)(ms % 1000) * 1000000L };
	return (int)syscall(SYS_futex, (int *)addr, FUTEX_WAIT_PRIVATE, expect,
	                    ms ? &ts : 0, 0, 0);
}

static int futex_wake(volatile int *addr, int n)
{
	return (int)syscall(SYS_futex, (int *)addr, FUTEX_WAKE_PRIVATE, n, 0, 0, 0);
}

/* ── handoff ─────────────────────────────────────────────────────────────── */

static volatile int hand_turn;   /* whose turn it is: 0 = A, 1 = B */
static volatile int hand_stop;
static volatile int hand_b_done;

static void *handoff_peer(void *arg)
{
	(void)arg;
	for (int i = 0; i < ROUNDS && !hand_stop; i++) {
		/* Wait for our turn. The bounded wait is what turns a lost wake into
		 * a report instead of a hang. */
		while (hand_turn != 1 && !hand_stop) {
			if (futex_wait(&hand_turn, 0, 2000) < 0 && errno == ETIMEDOUT)
				break;
		}
		if (hand_turn != 1)
			break; /* never handed over: give up and let the main side report */
		hand_turn = 0;
		futex_wake(&hand_turn, 1);
	}
	hand_b_done = 1;
	return 0;
}

static int test_handoff(void)
{
	pthread_t th;
	int completed = 0;

	hand_turn = 0;
	hand_stop = 0;
	hand_b_done = 0;

	if (pthread_create(&th, 0, handoff_peer, 0) != 0) {
		printf("FUTEX-STRESS: FAIL handoff (cannot create thread)\n");
		return 1;
	}

	for (int i = 0; i < ROUNDS; i++) {
		hand_turn = 1;
		futex_wake(&hand_turn, 1);

		while (hand_turn != 0) {
			if (futex_wait(&hand_turn, 1, 2000) < 0 && errno == ETIMEDOUT)
				break;
		}
		if (hand_turn != 0)
			break; /* the peer never took its turn back */
		completed++;
	}

	hand_stop = 1;
	futex_wake(&hand_turn, WAITERS + 1);
	pthread_join(th, 0);

	if (completed == ROUNDS) {
		printf("FUTEX-STRESS: ok handoff (%d round trips)\n", completed);
		return 0;
	}
	printf("FUTEX-STRESS: FAIL handoff (stalled after %d of %d round trips)\n",
	       completed, ROUNDS);
	return 1;
}

/* ── broadcast ───────────────────────────────────────────────────────────── */

static volatile int bcast_gate;
static volatile int bcast_woken;

static void *broadcast_waiter(void *arg)
{
	(void)arg;
	while (bcast_gate == 0) {
		if (futex_wait(&bcast_gate, 0, 3000) < 0 && errno == ETIMEDOUT)
			return 0; /* not woken: counted as missing below */
	}
	__sync_fetch_and_add(&bcast_woken, 1);
	return 0;
}

static int test_broadcast(void)
{
	pthread_t th[WAITERS];
	int made = 0;

	bcast_gate = 0;
	bcast_woken = 0;

	for (int i = 0; i < WAITERS; i++) {
		if (pthread_create(&th[i], 0, broadcast_waiter, 0) == 0)
			made++;
	}
	if (made != WAITERS) {
		printf("FUTEX-STRESS: FAIL broadcast (only %d of %d threads)\n", made,
		       WAITERS);
		return 1;
	}

	/* Let them all reach the wait, then release everyone at once. */
	usleep(200 * 1000);
	bcast_gate = 1;
	futex_wake(&bcast_gate, WAITERS);

	for (int i = 0; i < WAITERS; i++)
		pthread_join(th[i], 0);

	if (bcast_woken == WAITERS) {
		printf("FUTEX-STRESS: ok broadcast (%d of %d woken)\n", bcast_woken,
		       WAITERS);
		return 0;
	}
	printf("FUTEX-STRESS: FAIL broadcast (%d of %d woken)\n", bcast_woken,
	       WAITERS);
	return 1;
}

/* ── timed ───────────────────────────────────────────────────────────────── */

static volatile int timed_val;
static volatile int timed_spurious;

static void *timed_waker(void *arg)
{
	(void)arg;
	for (int i = 0; i < TIMED_LOOPS; i++) {
		/* Deliberately racy: sometimes the waiter is already asleep, sometimes
		 * it is still on its way in. Both must end with it awake. */
		if (i & 1)
			usleep(1000);
		timed_val = 1;
		futex_wake(&timed_val, 1);
		while (timed_val != 0)
			usleep(500);
	}
	return 0;
}

static int test_timed(void)
{
	pthread_t th;
	int timeouts = 0;

	timed_val = 0;
	timed_spurious = 0;

	if (pthread_create(&th, 0, timed_waker, 0) != 0) {
		printf("FUTEX-STRESS: FAIL timed (cannot create thread)\n");
		return 1;
	}

	for (int i = 0; i < TIMED_LOOPS; i++) {
		while (timed_val == 0) {
			/* Five seconds is far longer than the microseconds this handoff
			 * needs; anything that expires here was not woken. */
			if (futex_wait(&timed_val, 0, 5000) < 0 && errno == ETIMEDOUT) {
				if (timed_val == 0) {
					timeouts++;
					break;
				}
			}
		}
		if (timed_val == 0)
			break;
		timed_val = 0;
		futex_wake(&timed_val, 1);
	}

	pthread_join(th, 0);

	if (timeouts == 0) {
		printf("FUTEX-STRESS: ok timed (%d handoffs, no timeouts)\n",
		       TIMED_LOOPS);
		return 0;
	}
	printf("FUTEX-STRESS: FAIL timed (%d waits expired that should have been "
	       "woken)\n", timeouts);
	return 1;
}

int main(void)
{
	int bad = 0;

	printf("FUTEX-STRESS: start\n");
	bad += test_handoff();
	bad += test_broadcast();
	bad += test_timed();
	printf("FUTEX-STRESS: done (%d failed)\n", bad);
	return bad ? 1 : 0;
}
