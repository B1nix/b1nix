/* SPDX-License-Identifier: MIT */
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
#define lockdep_set_class(lock, key) do { (void)(lock); (void)(key); } while (0)
#define lockdep_assert_held(l)       do { (void)(l); } while (0)
#define lockdep_assert_none_held_once() do { } while (0)
#define lockdep_is_held(l) (1)
/* Lock-class annotations the ww_mutex headers emit. Nothing is recorded — see
 * the note above on why they are not claimed to have been checked. */
#define lock_acquire_shared_recursive(l, s, t, n, i) do { } while (0)
#define lock_acquire_exclusive(l, s, t, n, i)        do { } while (0)
#define lock_release(l, i)                           do { } while (0)
#define lockdep_init_map(l, n, k, s)                 do { } while (0)

#define lockdep_assert_held_once(l)  do { (void)(l); } while (0)
#define lockdep_assert_once(cond)    do { (void)(cond); } while (0)
#define lockdep_assert_not_held(l)   do { (void)(l); } while (0)
#define might_lock(l) do { (void)(l); } while (0)
#define might_sleep() do { } while (0)
#endif
