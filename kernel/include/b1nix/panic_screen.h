#ifndef B1NIX_PANIC_SCREEN_H
#define B1NIX_PANIC_SCREEN_H

#include <b1nix/types.h>

/* The otter, generated from tools/build/kernel/art/otter-pitchfork.jpg by
 * tools/build/kernel/gen_panic_otter.py (kernel/dev/panic_otter.c). */
#define PANIC_OTTER_COLOURS 5
extern const u32 panic_otter_palette[PANIC_OTTER_COLOURS]; /* 0x00RRGGBB; [0] = background */
extern const u16 panic_otter_width;
extern const u16 panic_otter_height;
extern const u8 panic_otter_rle[];       /* index << 5 | (run - 1), row-major */
extern const usize panic_otter_rle_len;
extern const char *const panic_otter_ascii[]; /* NULL-terminated, <= 80 columns */

/*
 * Paint the full-screen panic screen on the framebuffer console: the otter,
 * the reason, CPU and task, and a pinned backtrace: `pc` first when nonzero,
 * then the frame-pointer chain from `frame_ptr` (0 = the caller's frame).
 * Console output that follows lands in the text area below it. Does nothing without a framebuffer console.
 *
 * Panic-path safe: no allocation, no locks, no scheduler, no device present
 * calls. Only the first call paints; a fault while painting turns the screen
 * off for the rest of the panic instead of recursing.
 */
void panic_screen_show(const char *reason, const char *file, int line, u64 pc,
                       u64 frame_ptr);

/* The architecture's own account of a fault -- vector, error code and CR2 on
 * x86_64, EC, ESR and FAR on aarch64 -- shown as FAULT on the panic screen.
 * Set by the exception handler before panic_screen_show; a software panic
 * leaves it empty. Copied, so a stack buffer is fine. */
void panic_screen_fault(const char *detail);

/* The ASCII otter, to the serial port only. */
void panic_screen_serial_banner(void);

/* b1nix.panic-demo: panic on purpose to show the screen (=fault takes an
 * unhandled kernel fault instead). Returns when the flag is absent. */
void panic_screen_demo(void);

#endif
