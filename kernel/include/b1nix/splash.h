/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_SPLASH_H
#define B1NIX_SPLASH_H

/*
 * B1nix unified boot splash.
 *
 * Rendered to the framebuffer console during early boot while the kernel
 * brings up hardware, splitting the screen so the boot log scrolls below the
 * artwork. Dismissed when PID 1 (init) begins execution, returning the full
 * display to userspace.
 */
void b1nix_splash_show(void);
void b1nix_splash_dismiss(void);

/* Backwards-compatibility aliases */
static inline void demon_splash_show(void) { b1nix_splash_show(); }
static inline void demon_splash_dismiss(void) { b1nix_splash_dismiss(); }

#endif /* B1NIX_SPLASH_H */
