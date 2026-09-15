/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SRCU_H
#define LKPI_LINUX_SRCU_H
#include <lkpi/rcu.h>
#include <linux/types.h>
/*
 * Sleepable RCU, per domain.
 *
 * An SRCU reader may sleep, which is the one thing it exists to allow, so a
 * read section here disables nothing: it counts itself into the domain's
 * current bucket and out again, on whatever CPU it finishes on. i915 sleeps
 * inside its reset-backoff section; mapped onto rcu_read_lock (interrupts off,
 * nesting counted per CPU), the task migrated mid-section, the old CPU's
 * nesting never came back to zero, and every later RCU reader there left
 * interrupts off for good.
 *
 * synchronize_srcu() flips the domain to the other bucket and waits for the
 * old one to drain, twice: a reader that read the index just before a flip and
 * counted itself just after lands in the bucket the second flip waits for.
 */
struct srcu_struct {
	volatile u32 idx;
	volatile s64 readers[2];
};
static inline int init_srcu_struct(struct srcu_struct *s)
{
	s->idx = 0;
	s->readers[0] = 0;
	s->readers[1] = 0;
	return 0;
}
static inline void cleanup_srcu_struct(struct srcu_struct *s) { (void)s; }
static inline int srcu_read_lock(struct srcu_struct *s)
{
	int idx = (int)(__atomic_load_n(&s->idx, __ATOMIC_ACQUIRE) & 1u);

	__atomic_add_fetch(&s->readers[idx], 1, __ATOMIC_ACQ_REL);
	return idx;
}
static inline void srcu_read_unlock(struct srcu_struct *s, int idx)
{
	__atomic_sub_fetch(&s->readers[idx & 1], 1, __ATOMIC_ACQ_REL);
}
/* A file-scope srcu domain: zero-initialised is initialised. */
#define DEFINE_STATIC_SRCU(name) static struct srcu_struct name
#define DEFINE_SRCU(name)        struct srcu_struct name

void synchronize_srcu(struct srcu_struct *s);

/* Same reasoning as synchronize_rcu_expedited(): there is no slower path here
 * for the expedited form to be faster than. */
#define synchronize_srcu_expedited(sp) synchronize_srcu(sp)

/* Fetch an SRCU-protected pointer. */
#define srcu_dereference_check(p, ssp, c) ({ (void)(ssp); (void)(c); rcu_dereference(p); })
#define srcu_dereference(p, ssp)          srcu_dereference_check((p), (ssp), 0)

#endif
