#ifndef B1NIX_COMPOSITOR_H
#define B1NIX_COMPOSITOR_H

#include <b1nix/types.h>

struct compositor_window;

void compositor_init(void);
void compositor_wake(void);
void compositor_reclaim_display(void);
struct compositor_window *compositor_window_create(int x, int y, int width, int height);
void compositor_window_destroy(struct compositor_window *win);
void compositor_window_move(struct compositor_window *win, int x, int y);
void compositor_window_resize(struct compositor_window *win, int width, int height);
void compositor_window_raise(struct compositor_window *win);
void compositor_window_invalidate(struct compositor_window *win);
u32 *compositor_window_buffer(struct compositor_window *win);

#endif
