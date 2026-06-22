/* libnsfb displayd surface — runs the NetSurf framebuffer frontend as a windowed
 * client of the b1nix display server (displayd) over the b1display/libgui
 * (Wayland-shaped) protocol. NetSurf draws into the window's shared buffer and
 * presents damage with b1gui_present; pointer/keyboard events from the
 * compositor are translated to libnsfb events, so the window is interactive.
 * Copied into libnsfb's src/surface/ by tools/ports/build-libnsfb.sh; registered as
 * "displayd" on the NSFB_SURFACE_WL slot. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <b1nix/gui.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_event.h"

#include "nsfb.h"
#include "surface.h"
#include "plot.h"
#include "libnsfb-b1keymap.h"

#define UNUSED(x) ((x) = (x))

static int displayd_defaults(nsfb_t *nsfb)
{
    nsfb->width = 800;
    nsfb->height = 600;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    return 0;
}

static int displayd_initialise(nsfb_t *nsfb)
{
    struct b1gui_window *win = calloc(1, sizeof(*win));
    if (win == NULL)
        return -1;
    if (b1gui_connect(win) != 0) {
        free(win);
        return -1;
    }
    uint32_t w = nsfb->width > 0 ? (uint32_t)nsfb->width : 800;
    uint32_t h = nsfb->height > 0 ? (uint32_t)nsfb->height : 600;
    if (b1gui_create_window(win, w, h, "NetSurf") != 0) {
        b1gui_destroy(win);
        free(win);
        return -1;
    }

    nsfb->surface_priv = win;
    nsfb->ptr = (uint8_t *)win->pixels;
    nsfb->width = (int)win->width;
    nsfb->height = (int)win->height;
    nsfb->bpp = 32;
    nsfb->linelen = (int)(win->width * 4);
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    return 0;
}

static int displayd_finalise(nsfb_t *nsfb)
{
    struct b1gui_window *win = nsfb->surface_priv;
    if (win != NULL) {
        b1gui_destroy(win);
        free(win);
        nsfb->surface_priv = NULL;
        nsfb->ptr = NULL;
    }
    return 0;
}

static int displayd_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct b1gui_window *win = nsfb->surface_priv;
    int x, y;
    if (win == NULL)
        return -1;
    x = box->x0 < 0 ? 0 : box->x0;
    y = box->y0 < 0 ? 0 : box->y0;
    b1gui_present(win, (uint32_t)x, (uint32_t)y,
                  (uint32_t)(box->x1 - box->x0), (uint32_t)(box->y1 - box->y0));
    return 0;
}

static bool displayd_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    struct b1gui_window *win = nsfb->surface_priv;
    struct b1gui_event gev;

    if (win == NULL)
        return false;
    if (b1gui_next_event(win, &gev, timeout) != 1)
        return false;

    switch (gev.type) {
    case B1GUI_EV_POINTER_MOTION:
        if (gev.nargs < 2)
            return false;
        event->type = NSFB_EVENT_MOVE_ABSOLUTE;
        event->value.vector.x = (int)gev.args[0];
        event->value.vector.y = (int)gev.args[1];
        event->value.vector.z = 0;
        return true;
    case B1GUI_EV_POINTER_BUTTON:
        if (gev.nargs < 2)
            return false;
        event->type = (gev.args[1] != 0) ? NSFB_EVENT_KEY_DOWN
                                         : NSFB_EVENT_KEY_UP;
        event->value.keycode = NSFB_KEY_MOUSE_1;
        return true;
    case B1GUI_EV_KEY: {
        /* args[0] = evdev scancode, args[1] = pressed. Track shift and map to a
         * libnsfb keysym (shared with the /dev/fb0 surface). */
        static int displayd_shift; /* one window per process */
        if (gev.nargs < 2)
            return false;
        if (b1nix_is_shift_scancode(gev.args[0])) {
            displayd_shift = (gev.args[1] != 0);
            return false;
        }
        enum nsfb_key_code_e kc =
            b1nix_scancode_to_nsfb(gev.args[0], displayd_shift);
        if (kc == NSFB_KEY_UNKNOWN)
            return false;
        event->type = (gev.args[1] != 0) ? NSFB_EVENT_KEY_DOWN
                                         : NSFB_EVENT_KEY_UP;
        event->value.keycode = kc;
        return true;
    }
    case B1GUI_EV_CLOSE:
        event->type = NSFB_EVENT_CONTROL;
        event->value.controlcode = NSFB_CONTROL_QUIT;
        return true;
    default:
        break;
    }
    return false;
}

static int displayd_geometry(nsfb_t *nsfb, int width, int height,
                             enum nsfb_format_e format)
{
    UNUSED(width);
    UNUSED(height);
    /* The window is a fixed size negotiated at create; keep it, just (re)select
     * the format/plotters. */
    if (format != NSFB_FMT_ANY)
        nsfb->format = format;
    select_plotters(nsfb);
    return 0;
}

const nsfb_surface_rtns_t displayd_rtns = {
    .defaults = displayd_defaults,
    .initialise = displayd_initialise,
    .finalise = displayd_finalise,
    .input = displayd_input,
    .update = displayd_update,
    .geometry = displayd_geometry,
};

NSFB_SURFACE_DEF(displayd, NSFB_SURFACE_WL, &displayd_rtns)
