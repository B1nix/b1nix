/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_TTY_H
#define LKPI_LINUX_TTY_H

/*
 * Terminals, as the imported code uses them: one path only.
 *
 * The quota code writes "write failed, user block limit reached" to the
 * controlling terminal of the process that ran over its limit — a courtesy to
 * whoever is typing, not part of the accounting. b1nix's own terminals live on
 * the other side of the boundary and a task here has no controlling one, so
 * the lookup answers "none" and the message is simply not written. Nothing
 * about the quota decision depends on it.
 */

struct tty_struct;

static inline struct tty_struct *get_current_tty(void) { return 0; }
static inline void tty_kref_put(struct tty_struct *tty) { (void)tty; }
static inline int tty_write_message(struct tty_struct *tty, const char *msg)
{ (void)tty; (void)msg; return 0; }

#endif /* LKPI_LINUX_TTY_H */
