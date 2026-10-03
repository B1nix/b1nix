/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_JUMP_LABEL_H
#define LKPI_LINUX_JUMP_LABEL_H

#include <linux/types.h>
/*
 * Static keys: a branch upstream patches out of the instruction stream so a
 * rarely-taken path costs nothing when disabled. b1nix does not patch code at
 * runtime, so a key here is a plain bool and the branch is an ordinary one.
 * What is lost is the zero-cost property, not the behaviour: the same paths run
 * under the same conditions.
 */
/* A count, not a flag: the refcounted forms (static_branch_inc/dec) need one,
 * and a key is enabled while its count is above zero. */
struct static_key { int enabled; };
struct static_key_false { struct static_key key; };
struct static_key_true  { struct static_key key; };

#define STATIC_KEY_INIT_FALSE { .key = { .enabled = false } }
#define STATIC_KEY_INIT_TRUE  { .key = { .enabled = true } }
#define STATIC_KEY_FALSE_INIT STATIC_KEY_INIT_FALSE
#define STATIC_KEY_TRUE_INIT  STATIC_KEY_INIT_TRUE
#define DEFINE_STATIC_KEY_FALSE(name) struct static_key_false name = STATIC_KEY_INIT_FALSE
#define DEFINE_STATIC_KEY_TRUE(name)  struct static_key_true  name = STATIC_KEY_INIT_TRUE

#define static_branch_likely(k)   (__builtin_expect(!!((k)->key.enabled), 1))
#define static_branch_unlikely(k) (__builtin_expect(!!((k)->key.enabled), 0))
#define static_branch_enable(k)   do { (k)->key.enabled = true; } while (0)
#define static_branch_disable(k)  do { (k)->key.enabled = false; } while (0)
#define static_key_enable(k)      do { (k)->enabled = true; } while (0)
#define static_key_disable(k)     do { (k)->enabled = false; } while (0)
#define static_key_enabled(k)     ((k)->key.enabled > 0)
#define static_key_count(k)       ((k)->enabled)
#define static_branch_inc(k)      __atomic_fetch_add(&(k)->key.enabled, 1, __ATOMIC_SEQ_CST)
#define static_branch_dec(k)      __atomic_fetch_sub(&(k)->key.enabled, 1, __ATOMIC_SEQ_CST)
#define static_key_slow_inc(k)    __atomic_fetch_add(&(k)->enabled, 1, __ATOMIC_SEQ_CST)
#define static_key_slow_dec(k)    __atomic_fetch_sub(&(k)->enabled, 1, __ATOMIC_SEQ_CST)
#define DECLARE_STATIC_KEY_FALSE(name) extern struct static_key_false name
#define DECLARE_STATIC_KEY_TRUE(name)  extern struct static_key_true name
#define DEFINE_STATIC_KEY_FALSE_RO(name) DEFINE_STATIC_KEY_FALSE(name)
#define EXPORT_STATIC_KEY_GPL(name)

/* Deferred keys rate-limit the disable; with no code patching the disable
 * is free, so it simply happens at once. */
struct static_key_false_deferred {
	struct static_key_false key;
	unsigned long timeout;
};
#define DEFINE_STATIC_KEY_DEFERRED_FALSE(name, rl) \
	struct static_key_false_deferred name = { .key = STATIC_KEY_INIT_FALSE, .timeout = (rl) }
#define static_branch_deferred_inc(x)      static_branch_inc(&(x)->key)
#define static_branch_slow_dec_deferred(x) static_branch_dec(&(x)->key)
#define static_key_deferred_flush(x)       do { (void)(x); } while (0)
#define jump_label_rate_limit(x, rl)       do { (x)->timeout = (rl); } while (0)

#endif
