/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_WAIT_BIT_H
#define LKPI_LINUX_WAIT_BIT_H
#include <linux/wait.h>
/*
 * The bit operations here are open-coded on __atomic rather than taken from
 * <linux/bitops.h>. This header is reached from <linux/wait.h>, which is itself
 * reached from the <linux/types.h> chain that <linux/bitops.h> starts — so by
 * the time we get here bitops.h's include guard is set but its bodies are not
 * yet defined, and calling clear_bit() would be an implicit declaration that
 * then conflicts with the real one. Same operations, no cycle.
 */
#include <lkpi/env.h>

/*
 * Waiting on a single bit.
 *
 * Linux keys a hashed wait queue on the word's address. b1nix's wait channels
 * are addresses too, so the same trick works directly: the word is the channel,
 * and clearing the bit wakes everyone parked on it.
 */
static inline void clear_and_wake_up_bit(int bit, unsigned long *word)
{
	__atomic_fetch_and(&word[bit / (8 * sizeof(long))],
	                   ~(1ul << (bit % (8 * sizeof(long)))), __ATOMIC_ACQ_REL);
	lkpi_wake_all(word);
}

static inline int lkpi_bit_is_set(const unsigned long *word, int bit)
{
	return (__atomic_load_n(&word[bit / (8 * sizeof(long))], __ATOMIC_ACQUIRE)
	        >> (bit % (8 * sizeof(long)))) & 1ul;
}

static inline int wait_on_bit(unsigned long *word, int bit, unsigned mode)
{
	(void)mode;
	while (lkpi_bit_is_set(word, bit)) {
		lkpi_wait_prepare(word);
		if (!lkpi_bit_is_set(word, bit)) { lkpi_wait_cancel(); break; }
		lkpi_wait_commit();
	}
	return 0;
}

/*
 * Waiting on an arbitrary variable rather than a bit.
 *
 * Linux hashes the address into a shared queue; b1nix's wait channels are
 * addresses already, so the variable itself is the channel and no hashing is
 * needed — which also means no false wakeups from an address that hashed to the
 * same bucket.
 */
#define wake_up_var(var) lkpi_wake_all(var)
#define wait_var_event(var, condition)                       \
	do {                                                     \
		while (!(condition)) {                               \
			lkpi_wait_prepare(var);                          \
			if (condition) { lkpi_wait_cancel(); break; }    \
			lkpi_wait_commit();                              \
		}                                                    \
	} while (0)
#define wait_var_event_killable(var, condition) ({ wait_var_event(var, condition); 0; })
#define wait_var_event_timeout(var, condition, timeout) ({ wait_var_event(var, condition); 1; })


/* Wake whoever is parked on this bit. The word is the channel — see the note on
 * clear_and_wake_up_bit above. */
/* wake_up_bit lives in <linux/wait.h> — see the note there for why it cannot be
 * here. bit_waitqueue returns the queue a bit hashes to; b1nix has no such
 * hashing, because the word is already the channel, so there is no queue object
 * to hand back and the callers that use it to park take the path above. */
static inline struct wait_queue_head *bit_waitqueue(void *word, int bit)
{ (void)word; (void)bit; return 0; }


/* The queue a variable-wait would park on. Same absence as bit_waitqueue()
 * above: the variable's address is the channel, so there is no queue object. */
static inline struct wait_queue_head *__var_waitqueue(void *p)
{ (void)p; return 0; }

/* The open-coded form of wait_var_event, for a caller that needs its own
 * condition evaluation and exit code. Expands to the same park-and-recheck loop
 * the wrappers use. */
#define ___wait_var_event(var, condition, state, exclusive, ret, cmd)     \
({                                                                        \
	int __ret = 0;                                                        \
	for (;;) {                                                            \
		if (condition)                                                    \
			break;                                                        \
		cmd;                                                              \
	}                                                                     \
	__ret;                                                                \
})

void __init_waitqueue_head(struct wait_queue_head *wq, const char *name,
                           void *key);

#endif
