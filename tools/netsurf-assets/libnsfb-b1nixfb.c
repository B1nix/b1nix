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

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_event.h"

#include "nsfb.h"
#include "surface.h"
#include "plot.h"

#define UNUSED(x) ((x) = (x))

struct b1nixfb_priv {
    int fd;
    size_t maplen;
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
    UNUSED(nsfb);
    UNUSED(event);
    UNUSED(timeout);
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
