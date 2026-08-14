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
struct static_key { bool enabled; };
struct static_key_false { struct static_key key; };
struct static_key_true  { struct static_key key; };

#define STATIC_KEY_INIT_FALSE { .key = { .enabled = false } }
#define STATIC_KEY_INIT_TRUE  { .key = { .enabled = true } }
#define DEFINE_STATIC_KEY_FALSE(name) struct static_key_false name = STATIC_KEY_INIT_FALSE
#define DEFINE_STATIC_KEY_TRUE(name)  struct static_key_true  name = STATIC_KEY_INIT_TRUE

#define static_branch_likely(k)   (__builtin_expect(!!((k)->key.enabled), 1))
#define static_branch_unlikely(k) (__builtin_expect(!!((k)->key.enabled), 0))
#define static_branch_enable(k)   do { (k)->key.enabled = true; } while (0)
#define static_branch_disable(k)  do { (k)->key.enabled = false; } while (0)
#define static_key_enable(k)      do { (k)->enabled = true; } while (0)
#define static_key_disable(k)     do { (k)->enabled = false; } while (0)

#endif
