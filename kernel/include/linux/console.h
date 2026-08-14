/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CONSOLE_H
#define LKPI_LINUX_CONSOLE_H
#include <linux/types.h>
/* The text-console layer DRM takes over from when it binds fbdev emulation.
 * b1nix's console is its own (kernel/arch/x86_64/console.c) and nothing hands
 * it over yet, so these are no-ops — the handover is a decision for the first
 * driver that wants the screen. */
struct console;
static inline void console_lock(void) { }
static inline void console_unlock(void) { }
static inline int console_trylock(void) { return 1; }
#endif
