/* libnsfb b1nix /dev/fb0 surface — draws NetSurf directly to the real hardware
 * framebuffer (M47 fb device), making the framebuffer frontend an on-screen
 * browser rather than an off-screen RAM render. Copied into libnsfb's
 * src/surface/ by tools/build-libnsfb.sh.
 *
 * The b1nix fb is 32bpp B8G8R8A8 (virtio-gpu), i.e. memory order B,G,R,A — which
 * matches NetSurf's NSFB_FMT_XRGB8888 software plotter. Damage is pushed to the
 * display with B1NIX_FBIOFLUSH, the same path the b1nix compositor uses. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <b1nix/fb.h>
#include <b1nix/input.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_event.h"

#include "nsfb.h"
#include "surface.h"
#include "plot.h"
#include "libnsfb-b1keymap.h"

#define UNUSED(x) ((x) = (x))

struct b1nixfb_priv {
    int fd;
    size_t maplen;
    int kfd;   /* /dev/input/event0 (keyboard), -1 if unavailable */
    int mfd;   /* /dev/input/event1 (mouse),    -1 if unavailable */
    int px, py; /* tracked absolute pointer position */
    int shift;  /* shift held? for the scancode->keysym map */
};

static int b1nixfb_defaults(nsfb_t *nsfb)
{
    nsfb->width = 0;
    nsfb->height = 0;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    return 0;
}

static int b1nixfb_initialise(nsfb_t *nsfb)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0)
        return -1;

    struct b1nix_fb_info info;
    if (ioctl(fd, B1NIX_FBIOGET_INFO, &info) != 0 ||
        info.width == 0 || info.height == 0) {
        close(fd);
        return -1;
    }

    size_t maplen = (size_t)info.pitch * info.height;
    void *fb = mmap(0, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        close(fd);
        return -1;
    }

    struct b1nixfb_priv *priv = malloc(sizeof(*priv));
    if (priv == NULL) {
        munmap(fb, maplen);
        close(fd);
        return -1;
    }
    priv->fd = fd;
    priv->maplen = maplen;
    /* Open the input devices so the on-screen frontend is interactive. */
    priv->kfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    priv->mfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    priv->px = (int)info.width / 2;
    priv->py = (int)info.height / 2;
    priv->shift = 0;

    nsfb->surface_priv = priv;
    nsfb->ptr = fb;
    nsfb->width = (int)info.width;
    nsfb->height = (int)info.height;
    nsfb->bpp = 32;
    nsfb->linelen = (int)info.pitch;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    return 0;
}

static int b1nixfb_finalise(nsfb_t *nsfb)
{
    struct b1nixfb_priv *priv = nsfb->surface_priv;
    if (priv != NULL) {
        if (nsfb->ptr != NULL)
            munmap(nsfb->ptr, priv->maplen);
        if (priv->kfd >= 0)
            close(priv->kfd);
        if (priv->mfd >= 0)
            close(priv->mfd);
        close(priv->fd);
        free(priv);
        nsfb->surface_priv = NULL;
        nsfb->ptr = NULL;
    }
    return 0;
}

static int b1nixfb_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct b1nixfb_priv *priv = nsfb->surface_priv;
    struct b1nix_fb_rect r;

    if (priv == NULL)
        return -1;

    r.x = (uint32_t)(box->x0 < 0 ? 0 : box->x0);
    r.y = (uint32_t)(box->y0 < 0 ? 0 : box->y0);
    r.w = (uint32_t)(box->x1 - box->x0);
    r.h = (uint32_t)(box->y1 - box->y0);
    ioctl(priv->fd, B1NIX_FBIOFLUSH, &r);
    return 0;
}

static int
b1nixfb_geometry(nsfb_t *nsfb, int width, int height, enum nsfb_format_e format)
{
    UNUSED(width);
    UNUSED(height);
    /* The display is a fixed-size hardware framebuffer — keep the geometry the
     * device reported at initialise; just (re)select the format/plotters. */
    if (format != NSFB_FMT_ANY)
        nsfb->format = format;
    select_plotters(nsfb);
    return 0;
}

static bool b1nixfb_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    struct b1nixfb_priv *priv = nsfb->surface_priv;
    struct b1nix_input_event ie;
    UNUSED(timeout);

    if (priv == NULL)
        return false;

    /* Mouse: absolute position + left button. (One libnsfb event per call; the
     * frontend's loop calls us repeatedly.) */
    if (priv->mfd >= 0 && read(priv->mfd, &ie, sizeof(ie)) == (int)sizeof(ie)) {
        if (ie.type == B1NIX_EV_ABS) {
            if (ie.code == B1NIX_ABS_X) priv->px = ie.value;
            else if (ie.code == B1NIX_ABS_Y) priv->py = ie.value;
            event->type = NSFB_EVENT_MOVE_ABSOLUTE;
            event->value.vector.x = priv->px;
            event->value.vector.y = priv->py;
            event->value.vector.z = 0;
            return true;
        }
        if (ie.type == B1NIX_EV_REL) {
            if (ie.code == B1NIX_REL_X) priv->px += ie.value;
            else if (ie.code == B1NIX_REL_Y) priv->py += ie.value;
            event->type = NSFB_EVENT_MOVE_ABSOLUTE;
            event->value.vector.x = priv->px;
            event->value.vector.y = priv->py;
            event->value.vector.z = 0;
            return true;
        }
        if (ie.type == B1NIX_EV_KEY && ie.code == B1NIX_BTN_LEFT) {
            event->type = ie.value ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
            event->value.keycode = NSFB_KEY_MOUSE_1;
            return true;
        }
        /* EV_SYN or other: no libnsfb event this call. */
    }

    /* Keyboard: PS/2 set-1 scancode -> keysym (shift-aware). */
    if (priv->kfd >= 0 && read(priv->kfd, &ie, sizeof(ie)) == (int)sizeof(ie)) {
        if (ie.type == B1NIX_EV_KEY) {
            if (b1nix_is_shift_scancode(ie.code)) {
                priv->shift = (ie.value != 0);
            } else {
                enum nsfb_key_code_e kc =
                    b1nix_scancode_to_nsfb(ie.code, priv->shift);
                if (kc != NSFB_KEY_UNKNOWN) {
                    event->type = ie.value ? NSFB_EVENT_KEY_DOWN
                                           : NSFB_EVENT_KEY_UP;
                    event->value.keycode = kc;
                    return true;
                }
            }
        }
    }
    return false;
}

const nsfb_surface_rtns_t b1nixfb_rtns = {
    .defaults = b1nixfb_defaults,
    .initialise = b1nixfb_initialise,
    .finalise = b1nixfb_finalise,
    .input = b1nixfb_input,
    .update = b1nixfb_update,
    .geometry = b1nixfb_geometry,
};

NSFB_SURFACE_DEF(b1nix, NSFB_SURFACE_LINUX, &b1nixfb_rtns)
