/* Shared scaffolding for the soak stressors.
 *
 * Every program here is run in a loop for hours, so three properties matter
 * more than what any single one exercises:
 *
 *   bounded    — a run that proves a bug must still finish. Each stressor has
 *                a wall-clock budget (SOAK_SECONDS) as well as an iteration
 *                count, and stops at whichever comes first. Without that a
 *                single slow instance eats the whole night's budget.
 *   scalable   — SOAK_SCALE dials the work without rebuilding the image, so
 *                the same binary serves a ten-second gate and a ten-minute
 *                torture run.
 *   self-checking — every byte written is verified against a pattern derived
 *                from its own address, so a mismatch names the writer's slot
 *                and offset rather than merely saying "corrupt".
 */
#ifndef B1NIX_STRESS_H
#define B1NIX_STRESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

/* Milliseconds on a clock that does not step. CLOCK_MONOTONIC is what the
 * budget is measured against; a wall clock corrected mid-run would either cut
 * a stressor short or let it run forever. */
static inline unsigned long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static inline long env_long(const char *name, long dflt)
{
	const char *v = getenv(name);
	if (!v || !*v)
		return dflt;
	char *end = 0;
	long n = strtol(v, &end, 10);
	if (end == v)
		return dflt;
	return n;
}

/* The work budget, in whatever unit the caller counts in, scaled by SOAK_SCALE
 * in percent. 100 is the default; a gate run passes 20, an overnight round
 * passes 300. Never returns zero — a stressor that does nothing reports "ok". */
static inline long scaled(long base)
{
	long pct = env_long("SOAK_SCALE", 100);
	if (pct < 1)
		pct = 1;
	long n = base * pct / 100;
	return n < 1 ? 1 : n;
}

static inline unsigned long long budget_ms(void)
{
	return (unsigned long long)env_long("SOAK_SECONDS", 30) * 1000ULL;
}

/* A byte that depends on where it lives.
 *
 * Two identities go in — the owner (thread, round, file) and the offset — so a
 * wrong byte says whose data landed there. A pattern of one repeated value
 * cannot distinguish a foreign write from a shifted one, which is exactly the
 * distinction the compositor's heap corruption turns on. */
static inline unsigned char pat(unsigned long owner, unsigned long off)
{
	unsigned long h = owner * 1103515245UL + off * 2654435761UL;
	h ^= h >> 13;
	return (unsigned char)(h & 0xff);
}

static inline void pat_fill(void *p, unsigned long owner, size_t len)
{
	unsigned char *b = (unsigned char *)p;
	for (size_t i = 0; i < len; i++)
		b[i] = pat(owner, i);
}

/* Returns the offset of the first wrong byte, or (size_t)-1 when clean. */
static inline size_t pat_check(const void *p, unsigned long owner, size_t len)
{
	const unsigned char *b = (const unsigned char *)p;
	for (size_t i = 0; i < len; i++)
		if (b[i] != pat(owner, i))
			return i;
	return (size_t)-1;
}

/* A cheap per-thread generator. rand() serialises on a lock inside musl and
 * turns a scheduler stress test into a lock-contention test. */
static inline unsigned long rnd(unsigned long *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

#endif /* B1NIX_STRESS_H */
