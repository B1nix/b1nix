/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_RBTREE_H
#define LKPI_LINUX_RBTREE_H
#include <lkpi/rbtree.h>
#include <linux/kernel.h>
/*
 * The cached root, its insert/erase helpers and the augmented forms all live in
 * <lkpi/rbtree.h>, so b1nix's own self-tests can drive the same code imported
 * code runs on. This header is the Linux-named front for them.
 */

#include <linux/rbtree_augmented.h>

#define rb_parent(r) ((r)->__rb_parent)

#define RB_ROOT ((struct rb_root){ 0 })
#define RB_ROOT_CACHED ((struct rb_root_cached){ { 0 }, 0 })
/* A node not in any tree points at itself. The member is __rb_parent here;
 * see <lkpi/rbtree.h> for why it carries the underscores. */
#define RB_EMPTY_NODE(node) ((node)->__rb_parent == (node))
#define RB_CLEAR_NODE(node) ((node)->__rb_parent = (node))
#endif
