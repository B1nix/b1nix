/* SPDX-License-Identifier: MIT */
#ifndef LKPI_RCU_H
#define LKPI_RCU_H

#include <lkpi/types.h>

/*
 * RCU — read without locking, free after every reader has finished.
 *
 * What a driver uses it for. A fence, a GEM object or a context is looked up far
 * more often than it is replaced, and the lookup is on a path that cannot afford
 * a lock. RCU lets the reader dereference a pointer with no atomic at all, and
 * makes the writer's job "publish the new value, then wait until nobody can
 * still be looking at the old one, then free it".
 *
 * The dangerous part, and why this is implemented rather than stubbed. A
 * synchronize_rcu() that returns too early does not crash: it frees memory a
 * reader is still walking, and the corruption surfaces somewhere else, later,
 * under load. A shim that makes it a no-op passes every test that does not
 * actually race. So the grace period here is real, and the self-test proves it
 * by holding a reader across a synchronize_rcu() and checking that the call did
 * not return until the reader let go.
 *
 * How the grace period is decided. Readers are counted in one of two buckets;
 * which bucket is current is a single global index. To start a grace period the
 * writer flips the index and then waits for the *old* bucket to drain. Every
 * reader that could have seen the pre-flip value counted itself in the old
 * bucket, so draining it means exactly "every such reader has finished"; readers
 * arriving afterwards land in the new bucket and cannot hold the writer up. This
 * terminates as long as read-side sections do, which is guaranteed below.
 *
 * Read-side rules, enforced rather than documented. rcu_read_lock() disables
 * interrupts on the calling CPU. That pins the reader to its CPU, keeps the
 * section short by construction, and makes sleeping inside one impossible rather
 * than merely forbidden — the same rule that already governs every spinlock in
 * this kernel. Sections nest.
 *
 * synchronize_rcu() sleeps and must not be called from an interrupt handler,
 * from inside a read-side section, or with a spinlock held. call_rcu() does not
 * sleep and may be called from anywhere.
 */

struct rcu_head;
typedef void (*rcu_callback_t)(struct rcu_head *head);

struct rcu_head {
	struct rcu_head *next;
	rcu_callback_t func;
};

/* Enter/leave a read-side critical section. Nests. Does not sleep. */
void rcu_read_lock(void);
void rcu_read_unlock(void);

/* 1 when the caller is inside a read-side section on this CPU. */
int rcu_read_lock_held(void);

/*
 * Wait until every read-side section that was in progress when this was called
 * has finished. Sleeps. Returns after the grace period, never before.
 */
void synchronize_rcu(void);

/*
 * Run `func(head)` once a grace period has elapsed. Does not sleep, so it is the
 * form to use from an interrupt handler or while holding a lock. `head` must
 * stay allocated until the callback runs — it is normally embedded in the object
 * being freed.
 */
void call_rcu(struct rcu_head *head, rcu_callback_t func);

/* Wait until every callback queued before this call has run. Sleeps. */
void rcu_barrier(void);

/* Grace periods completed, and callbacks invoked. Diagnostics and self-test. */
u64 rcu_grace_periods(void);
u64 rcu_callbacks_invoked(void);

/* Start the callback thread. Called once during kernel init; call_rcu before
 * this simply queues, and the queue is drained when the thread appears. */
void rcu_init(void);

/*
 * Publish `ptr` into `*slot` so a reader that sees the new pointer also sees
 * everything written into the object before it. The release store is the half of
 * the barrier the writer owes; the reader's half is rcu_dereference.
 */
#define rcu_assign_pointer(slot, ptr) \
	__atomic_store_n(&(slot), (ptr), __ATOMIC_RELEASE)

/*
 * Read a pointer published that way. Must be called inside a read-side section:
 * the pointer is only guaranteed to stay alive for as long as that section.
 */
#define rcu_dereference(slot) __atomic_load_n(&(slot), __ATOMIC_ACQUIRE)

/*
 * Read a pointer while holding the lock that guards updates to it, rather than
 * inside a read-side section. The condition argument documents which lock that
 * is; it is not checked here, because b1nix's lock checker knows nothing about
 * the classes imported code declares — see <linux/lockdep.h>.
 */
#define rcu_dereference_protected(slot, cond) \
	((void)(cond), __atomic_load_n(&(slot), __ATOMIC_RELAXED))
#define rcu_dereference_raw(slot) rcu_dereference(slot)
#define rcu_replace_pointer(slot, ptr, cond)                        \
	({                                                              \
		typeof(slot) __old = (slot);                                \
		(void)(cond);                                               \
		rcu_assign_pointer(slot, ptr);                              \
		__old;                                                      \
	})


/*
 * Publishing and reading a pointer under RCU.
 *
 * The barriers are the whole content: the writer must not let the store of the
 * pointer be seen before the stores that initialised what it points at, and the
 * reader must not let the load of the target be hoisted above the load of the
 * pointer. Both are real here, not decoration — b1nix runs SMP with a weakly
 * ordered compiler even where x86 gives ordering for free.
 */
#define rcu_dereference(p)        __atomic_load_n(&(p), __ATOMIC_CONSUME)
#define rcu_dereference_raw(p)    rcu_dereference(p)
#define rcu_dereference_protected(p, c) ({ (void)(c); (p); })
/* Reading only to compare or to test for NULL: no dereference follows, so no
 * ordering is needed, and saying so keeps the distinction upstream draws. */
#define rcu_access_pointer(p)     __atomic_load_n(&(p), __ATOMIC_RELAXED)
#define rcu_assign_pointer(p, v)  __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)
/* Initialising a pointer nobody can be reading yet — no release needed, and
 * upstream keeps the separate name so that claim stays visible at the call. */
#define RCU_INIT_POINTER(p, v)    do { (p) = (v); } while (0)


/* Free after a grace period. b1nix's synchronize_rcu is a real wait (see the
 * proof in the M101 self-test), so this frees synchronously after it rather
 * than queueing a callback — which is slower for the caller and identical in
 * effect. It must therefore not be called from a context that cannot sleep,
 * and neither may the queueing form upstream, so the constraint is not new. */
#define kfree_rcu(ptr, rcu_member) \
	do { synchronize_rcu(); lkpi_kfree(ptr); } while (0)
#define kvfree_rcu(ptr, rcu_member) kfree_rcu(ptr, rcu_member)

#endif
