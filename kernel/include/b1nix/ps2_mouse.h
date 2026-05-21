#ifndef B1NIX_PS2_MOUSE_H
#define B1NIX_PS2_MOUSE_H

#include <b1nix/types.h>

struct ps2_mouse_state {
    int x;
    int y;
    u8 buttons;
};

void ps2_mouse_init(void);
void ps2_mouse_interrupt_handler(void);
void ps2_mouse_get_state(struct ps2_mouse_state *out);

#endif
