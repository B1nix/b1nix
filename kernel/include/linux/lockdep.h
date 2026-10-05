/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_LOCKDEP_H
#define LKPI_LINUX_LOCKDEP_H
/* Nothing from b1nix: its lock checker tracks b1nix locks by their own
 * identity and knows nothing about the classes imported code declares. */
/* b1nix has its own lock-order checker, but it tracks b1nix locks by their own
 * identity and knows nothing about the classes imported code declares. Rather
 * than register those classes as something they are not, the annotations
 * compile away and the assertions report "not checked" instead of "checked and
 * fine" — the difference matters when reading a bug report. */
struct lock_class_key { int unused; };
#define lockdep_assert_none_held_once() do { } while (0)
/* Lock-class annotations the ww_mutex headers emit. Nothing is recorded — see
 * the note above on why they are not claimed to have been checked. */
#define lock_acquire_shared_recursive(l, s, t, n, i) do { } while (0)
#define lock_acquire_exclusive(l, s, t, n, i)        do { } while (0)
#define lock_release(l, i)                           do { } while (0)
#define lockdep_init_map(l, n, k, s)                 do { } while (0)

#define lockdep_assert_once(cond)    lockdep_assert(cond)
#define might_lock(l) do { (void)(l); } while (0)
#define might_sleep() do { } while (0)

/* The cookie lockdep hands back from a pinned-lock section so it can check the
 * same lock is unpinned. Without lockdep there is nothing to carry, but the
 * type has to exist because callers keep one on the stack. */
struct pin_cookie { int unused; };


/* Lock assertions. There is no lockdep here, so these check nothing — and are
 * spelled as statements that consume their argument so an expression with a
 * side effect is still evaluated exactly as often as it would be upstream
 * (never, in a kernel built without lockdep). */
#ifndef lockdep_assert
#define lockdep_assert(cond)                                                  \
	do {                                                                   \
		if (!(cond))                                                   \
			lkpi_lockdep_assert_failed(0, #cond, __FILE__, __LINE__); \
	} while (0)
#define lockdep_assert_none_held_once() do { } while (0)
#endif


/*
 * Lock-held assertions, checked.
 *
 * The lock-order classes above are not modelled, but whether the CURRENT task
 * holds a given lock is: linuxkpi spinlocks, mutexes and rwsems all record
 * their holder. An assertion on one of those is therefore real, and a caller
 * that reaches a "must hold X" path without X panics there. A lock type with
 * no recorded holder answers "held" -- not checked, rather than refused -- so a
 * correct caller is never aborted by a question this shim cannot answer.
 */
int lkpi_lockdep_spin_held(const void *l);
int lkpi_lockdep_mutex_held(const void *m);
int lkpi_lockdep_rwsem_held(const void *s, int mode); /* 0 any, 1 write, 2 read */
void lkpi_lockdep_assert_failed(const void *l, const char *what,
                                const char *file, int line)
	__attribute__((noreturn));

#define lkpi_lockdep_held_mode(l, mode)                                       \
	_Generic((l),                                                          \
		struct lkpi_spinlock *: lkpi_lockdep_spin_held((const void *)(l)),      \
		const struct lkpi_spinlock *: lkpi_lockdep_spin_held((const void *)(l)),\
		struct mutex *: lkpi_lockdep_mutex_held((const void *)(l)),             \
		const struct mutex *: lkpi_lockdep_mutex_held((const void *)(l)),       \
		struct rw_semaphore *: lkpi_lockdep_rwsem_held((const void *)(l), (mode)),\
		const struct rw_semaphore *: lkpi_lockdep_rwsem_held((const void *)(l), (mode)),\
		default: 1)

#define lockdep_is_held(l)          lkpi_lockdep_held_mode((l), 0)
#define lockdep_is_held_type(l, r)  lkpi_lockdep_held_mode((l), (r) == 0 ? 1 : (r) == 1 ? 2 : 0)
#define lock_is_held(l)             1
#define lock_is_held_type(l, r)     1

#define lkpi_lockdep_assert_mode(l, mode, what)                               \
	do {                                                                   \
		if (!lkpi_lockdep_held_mode((l), (mode)))                      \
			lkpi_lockdep_assert_failed((const void *)(l), (what),  \
			                           __FILE__, __LINE__);        \
	} while (0)

#define lockdep_assert_held(l)       lkpi_lockdep_assert_mode((l), 0, "held")
#define lockdep_assert_held_once(l)  lkpi_lockdep_assert_mode((l), 0, "held")
#define lockdep_assert_held_write(l) lkpi_lockdep_assert_mode((l), 1, "held for write")
#define lockdep_assert_held_read(l)  lkpi_lockdep_assert_mode((l), 2, "held for read")
#define assert_spin_locked(l)        lkpi_lockdep_assert_mode((l), 0, "spin-locked")
/* "Not held" cannot answer "not checked" safely for unknown types, so only the
 * types with a recorded holder are tested. */
#define lockdep_assert_not_held(l)                                            \
	do {                                                                   \
		if (_Generic((l), struct lkpi_spinlock *: 1, struct mutex *: 1, \
		             struct rw_semaphore *: 1, default: 0) &&          \
		    lkpi_lockdep_held_mode((l), 1))                            \
			lkpi_lockdep_assert_failed((const void *)(l), "not held", \
			                           __FILE__, __LINE__);        \
	} while (0)


/* Pinning marks a lock that must still be held when the caller returns from a
 * scheduler callback. Without lockdep there is nothing to check, and the cookie
 * carries nothing — see struct pin_cookie above. */
#define lockdep_pin_lock(l)         ({ struct pin_cookie __c = { 0 }; (void)(l); __c; })
#define lockdep_unpin_lock(l, c)    do { (void)(l); (void)(c); } while (0)
#define lockdep_repin_lock(l, c)    do { (void)(l); (void)(c); } while (0)


#define lockdep_set_subclass(lock, sub)   do { (void)(sub); } while (0)
#define lockdep_set_class(lock, key)      do { } while (0)
#define lockdep_set_class_and_name(l, k, n) do { } while (0)

/*
 * How many nesting classes a lock may be given. b1nix has no lockdep, so a
 * subclass records nothing — but btrfs derives ARRAY SIZES from this constant
 * (its tree-lock nesting levels are checked against it), so the value has to be
 * upstream's rather than convenient.
 */
#ifndef MAX_LOCKDEP_SUBCLASSES
#define MAX_LOCKDEP_SUBCLASSES 8UL
#endif

/*
 * The lockdep annotations a filesystem makes by hand.
 *
 * jbd2 hands the journal's "transaction is open" state to lockdep as if it were
 * a rwsem, so a deadlock through it is reported like any other. There is no
 * lockdep here, so the annotations record nothing — but they must exist,
 * because they appear on paths that are compiled either way and an
 * implicitly-declared one links against nothing.
 */
#define rwsem_acquire(l, s, t, i)      do { } while (0)
#define rwsem_acquire_read(l, s, t, i) do { } while (0)
#define rwsem_release(l, i)            do { } while (0)
#define lock_map_acquire(l)            do { } while (0)
#define lock_map_release(l)            do { } while (0)
#define lockdep_is_held_type(l, r)     1

/* Whether the caller holds a given lock. With no lockdep there is nothing to
 * consult, and the answer is the one that makes an assertion pass rather than
 * fail — a false negative here would abort a correct caller. */
#define lock_is_held_type(l, r) 1

/* Dynamically allocated lock classes; nothing to register without lockdep. */
#define lockdep_register_key(key)   do { (void)(key); } while (0)
#define lockdep_unregister_key(key) do { (void)(key); } while (0)
#ifndef SINGLE_DEPTH_NESTING
#define SINGLE_DEPTH_NESTING 1
#endif

/* Might this touch user memory (and so sleep on a fault)? Checked only with
 * lockdep upstream; here it asserts what might_sleep asserts, nothing. */
#define might_fault() might_sleep()

#endif
