/*
 * b1nix VirGL winsys — drives Mesa's gallium `virgl` driver on the host GPU
 * through the b1nix /dev/virtio-gpu transport (userspace/include/b1nix/virgl.h)
 * instead of a Linux DRM virtio-gpu node + libdrm. A GEM "handle" maps 1:1 to a
 * b1nix res_id; PRIME/dma-buf/blob and the resource cache are dropped. SUBMIT
 * and TRANSFER are synchronous on b1nix (the kernel blocks on the host fence),
 * so fences and resource_wait/is_busy are trivial.
 *
 * This file is b1nix-owned and installed into the Mesa tree by
 * tools/ports/build-mesa.sh (tools/patches/mesa/files/...). It mirrors the role of
 * src/gallium/winsys/virgl/drm/virgl_drm_winsys.c.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "util/u_memory.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "pipe/p_state.h"

#include "virgl/virgl_winsys.h"
#include "virgl/virgl_public.h"
#include "virgl_b1nix_public.h"

#include <b1nix/virgl.h>

struct virgl_b1nix_winsys {
   struct virgl_winsys base;
   int fd;
   struct virgl_drm_caps caps;
};


/* The winsys-private resource handle the gallium virgl driver passes around. */
struct virgl_hw_res {
   struct pipe_reference reference;
   uint32_t res_handle;   /* b1nix res_id */
   void *ptr;             /* mmap'd backing (NULL until mapped) */
   int size;
   uint32_t format;
   uint32_t stride;
   uint32_t width;
   uint32_t height;
   uint32_t depth;
   uint32_t bind;
   uint64_t mmap_offset;
   int num_cs_references;
};

/* cmd buffer: a flat dword array submitted via B1NIX_VIRGL_SUBMIT, plus the
 * list of resources the stream references (for res_is_referenced). */
struct virgl_b1nix_cmd_buf {
   struct virgl_cmd_buf base;
   uint32_t *buf;
   struct virgl_winsys *ws;
   unsigned nres;
   unsigned cres;
   struct virgl_hw_res **res_bo;
};

static inline struct virgl_b1nix_winsys *
virgl_b1nix_winsys(struct virgl_winsys *iws)
{
   return (struct virgl_b1nix_winsys *)iws;
}

static inline struct virgl_b1nix_cmd_buf *
virgl_b1nix_cmd_buf(struct virgl_cmd_buf *cbuf)
{
   return (struct virgl_b1nix_cmd_buf *)cbuf;
}

/* ── resources ── */

static void virgl_b1nix_resource_destroy(struct virgl_winsys *vws,
                                         struct virgl_hw_res *res)
{
   struct virgl_b1nix_winsys *vws_b = virgl_b1nix_winsys(vws);
   uint32_t id = res->res_handle;

   if (res->ptr)
      munmap(res->ptr, res->size);
   /* Release the host resource + the device's 16-slot table entry. */
   ioctl(vws_b->fd, B1NIX_VIRGL_RES_UNREF, &id);
   FREE(res);
}

static struct virgl_hw_res *
virgl_b1nix_resource_create(struct virgl_winsys *vws,
                            enum pipe_texture_target target,
                            const void *map_front_private,
                            uint32_t format, uint32_t bind,
                            uint32_t width, uint32_t height,
                            uint32_t depth, uint32_t array_size,
                            uint32_t last_level, uint32_t nr_samples,
                            uint32_t flags, uint32_t size)
{
   struct virgl_b1nix_winsys *vws_b = virgl_b1nix_winsys(vws);
   struct virgl_hw_res *res;
   struct b1nix_virgl_res_create rc;

   (void)map_front_private;
   (void)last_level;
   (void)nr_samples;
   (void)flags;

   res = CALLOC_STRUCT(virgl_hw_res);
   if (!res)
      return NULL;

   memset(&rc, 0, sizeof(rc));
   rc.target = target;
   /* The driver passes a PIPE format here (bind is already a virgl bind); the
    * host RESOURCE_CREATE_3D wants a VIRGL format, so convert it — otherwise the
    * resource and the surface that wraps it disagree and the clear reads back
    * as zero. */
   rc.format = pipe_to_virgl_format(format);
   rc.bind = bind;
   rc.width = width ? width : 1;
   rc.height = height ? height : 1;
   rc.depth = depth ? depth : 1;
   rc.array_size = array_size ? array_size : 1;
   if (ioctl(vws_b->fd, B1NIX_VIRGL_RES_CREATE, &rc) != 0) {
      FREE(res);
      return NULL;
   }

   pipe_reference_init(&res->reference, 1);
   res->res_handle = rc.res_id;
   res->mmap_offset = rc.mmap_offset;
   res->size = (int)rc.size;
   res->format = format;
   res->bind = bind;
   res->width = rc.width;
   res->height = rc.height;
   res->depth = rc.depth;
   res->stride = util_format_get_stride(format, rc.width);
   return res;
}

static void *virgl_b1nix_resource_map(struct virgl_winsys *vws,
                                      struct virgl_hw_res *res)
{
   struct virgl_b1nix_winsys *vws_b = virgl_b1nix_winsys(vws);

   if (!res->ptr) {
      void *p = mmap(NULL, res->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     vws_b->fd, (long)res->mmap_offset);
      if (p == MAP_FAILED)
         return NULL;
      res->ptr = p;
   }
   return res->ptr;
}

static void virgl_b1nix_resource_reference(struct virgl_winsys *vws,
                                           struct virgl_hw_res **dres,
                                           struct virgl_hw_res *sres)
{
   struct virgl_hw_res *old = *dres;
   if (pipe_reference(&(*dres)->reference, &sres->reference))
      virgl_b1nix_resource_destroy(vws, old);
   *dres = sres;
}

static void virgl_b1nix_resource_wait(struct virgl_winsys *vws,
                                      struct virgl_hw_res *res)
{
   (void)vws; (void)res; /* SUBMIT/TRANSFER are synchronous */
}

static bool virgl_b1nix_resource_is_busy(struct virgl_winsys *vws,
                                         struct virgl_hw_res *res)
{
   (void)vws; (void)res;
   return false;
}

static uint32_t virgl_b1nix_resource_get_storage_size(struct virgl_winsys *vws,
                                                      struct virgl_hw_res *res)
{
   (void)vws;
   return res->size;
}

/* ── transfers ── */

static int virgl_b1nix_transfer_put(struct virgl_winsys *vws,
                                    struct virgl_hw_res *res,
                                    const struct pipe_box *box,
                                    uint32_t stride, uint32_t layer_stride,
                                    uint32_t buf_offset, uint32_t level)
{
   struct virgl_b1nix_winsys *vws_b = virgl_b1nix_winsys(vws);
   struct b1nix_virgl_transfer t;

   (void)stride; (void)layer_stride;
   memset(&t, 0, sizeof(t));
   t.res_id = res->res_handle;
   t.level = level;
   t.box.x = box->x; t.box.y = box->y; t.box.z = box->z;
   t.box.w = box->width; t.box.h = box->height; t.box.d = box->depth;
   t.offset = buf_offset;
   return ioctl(vws_b->fd, B1NIX_VIRGL_TRANSFER_TO_HOST, &t) == 0 ? 0 : -1;
}

static int virgl_b1nix_transfer_get(struct virgl_winsys *vws,
                                    struct virgl_hw_res *res,
                                    const struct pipe_box *box,
                                    uint32_t stride, uint32_t layer_stride,
                                    uint32_t buf_offset, uint32_t level)
{
   struct virgl_b1nix_winsys *vws_b = virgl_b1nix_winsys(vws);
   struct b1nix_virgl_transfer t;

   (void)stride; (void)layer_stride;
   memset(&t, 0, sizeof(t));
   t.res_id = res->res_handle;
   t.level = level;
   t.box.x = box->x; t.box.y = box->y; t.box.z = box->z;
   t.box.w = box->width; t.box.h = box->height; t.box.d = box->depth;
   t.offset = buf_offset;
   return ioctl(vws_b->fd, B1NIX_VIRGL_TRANSFER_FROM_HOST, &t) == 0 ? 0 : -1;
}

/* ── command buffers ── */

static struct virgl_cmd_buf *virgl_b1nix_cmd_buf_create(struct virgl_winsys *ws,
                                                        uint32_t size)
{
   struct virgl_b1nix_cmd_buf *cbuf;

   cbuf = CALLOC_STRUCT(virgl_b1nix_cmd_buf);
   if (!cbuf)
      return NULL;
   cbuf->ws = ws;
   cbuf->cres = 0;
   cbuf->nres = 0;
   cbuf->res_bo = NULL;
   cbuf->buf = MALLOC(size * sizeof(uint32_t));
   if (!cbuf->buf) {
      FREE(cbuf);
      return NULL;
   }
   cbuf->base.buf = cbuf->buf;
   cbuf->base.cdw = 0;
   return &cbuf->base;
}

static void virgl_b1nix_cmd_buf_destroy(struct virgl_cmd_buf *_cbuf)
{
   struct virgl_b1nix_cmd_buf *cbuf = virgl_b1nix_cmd_buf(_cbuf);
   FREE(cbuf->res_bo);
   FREE(cbuf->buf);
   FREE(cbuf);
}

static void virgl_b1nix_emit_res(struct virgl_winsys *vws,
                                 struct virgl_cmd_buf *_cbuf,
                                 struct virgl_hw_res *res, bool write_buf)
{
   struct virgl_b1nix_cmd_buf *cbuf = virgl_b1nix_cmd_buf(_cbuf);
   unsigned i;

   (void)vws;
   /* Emit the resource's host id into the command stream — this is how the
    * stream references resources (the surface/framebuffer/clear targets). The
    * b1nix res_id IS the host virtio-gpu resource id (no GEM-handle layer). */
   if (write_buf)
      cbuf->buf[cbuf->base.cdw++] = res->res_handle;

   for (i = 0; i < cbuf->nres; i++)
      if (cbuf->res_bo[i] == res)
         return;
   if (cbuf->nres >= cbuf->cres) {
      unsigned ncres = cbuf->cres ? cbuf->cres * 2 : 16;
      struct virgl_hw_res **n =
         REALLOC(cbuf->res_bo, cbuf->cres * sizeof(*n), ncres * sizeof(*n));
      if (!n)
         return;
      cbuf->res_bo = n;
      cbuf->cres = ncres;
   }
   cbuf->res_bo[cbuf->nres++] = res;
}

static bool virgl_b1nix_res_is_ref(struct virgl_winsys *vws,
                                   struct virgl_cmd_buf *_cbuf,
                                   struct virgl_hw_res *res)
{
   struct virgl_b1nix_cmd_buf *cbuf = virgl_b1nix_cmd_buf(_cbuf);
   unsigned i;
   (void)vws;
   for (i = 0; i < cbuf->nres; i++)
      if (cbuf->res_bo[i] == res)
         return true;
   return false;
}

static int virgl_b1nix_submit_cmd(struct virgl_winsys *vws,
                                  struct virgl_cmd_buf *_cbuf,
                                  struct pipe_fence_handle **fence)
{
   struct virgl_b1nix_winsys *vws_b = virgl_b1nix_winsys(vws);
   struct virgl_b1nix_cmd_buf *cbuf = virgl_b1nix_cmd_buf(_cbuf);
   struct b1nix_virgl_submit sub;
   int ret;

   memset(&sub, 0, sizeof(sub));
   sub.cmd_size = cbuf->base.cdw * sizeof(uint32_t);
   sub.cmd_ptr = (uint64_t)(uintptr_t)cbuf->buf;
   ret = ioctl(vws_b->fd, B1NIX_VIRGL_SUBMIT, &sub);

   cbuf->base.cdw = 0;
   cbuf->nres = 0;

   /* Synchronous: the GPU work is done. Hand back a non-NULL sentinel fence
    * (already signalled). */
   if (fence)
      *fence = (struct pipe_fence_handle *)(uintptr_t)1;
   return ret == 0 ? 0 : -1;
}

/* ── caps ── */

static int virgl_b1nix_get_caps(struct virgl_winsys *vws,
                                struct virgl_drm_caps *caps)
{
   struct virgl_b1nix_winsys *vws_b = virgl_b1nix_winsys(vws);
   struct b1nix_virgl_caps_data cd;

   virgl_ws_fill_new_caps_defaults(caps);
   memset(&cd, 0, sizeof(cd));
   cd.size = sizeof(caps->caps);
   cd.caps_ptr = (uint64_t)(uintptr_t)&caps->caps;
   if (ioctl(vws_b->fd, B1NIX_VIRGL_GET_CAPS_DATA, &cd) != 0)
      return -1;
   return 0;
}

/* ── fences (synchronous → trivial) ── */

static struct pipe_fence_handle *virgl_b1nix_cs_create_fence(struct virgl_winsys *vws,
                                                             int fd)
{
   (void)vws; (void)fd;
   return (struct pipe_fence_handle *)(uintptr_t)1;
}

static bool virgl_b1nix_fence_wait(struct virgl_winsys *vws,
                                   struct pipe_fence_handle *fence,
                                   uint64_t timeout)
{
   (void)vws; (void)fence; (void)timeout;
   return true; /* already signalled */
}

static void virgl_b1nix_fence_reference(struct virgl_winsys *vws,
                                        struct pipe_fence_handle **dst,
                                        struct pipe_fence_handle *src)
{
   (void)vws;
   *dst = src;
}

static int virgl_b1nix_fence_get_fd(struct virgl_winsys *vws,
                                    struct pipe_fence_handle *fence)
{
   (void)vws; (void)fence;
   return -1;
}

static void virgl_b1nix_winsys_destroy(struct virgl_winsys *vws)
{
   struct virgl_b1nix_winsys *vws_b = virgl_b1nix_winsys(vws);
   close(vws_b->fd);
   FREE(vws_b);
}

static struct virgl_winsys *virgl_b1nix_winsys_create(int fd)
{
   struct virgl_b1nix_winsys *vws_b;

   vws_b = CALLOC_STRUCT(virgl_b1nix_winsys);
   if (!vws_b)
      return NULL;
   vws_b->fd = fd;

   vws_b->base.destroy = virgl_b1nix_winsys_destroy;
   vws_b->base.transfer_put = virgl_b1nix_transfer_put;
   vws_b->base.transfer_get = virgl_b1nix_transfer_get;
   vws_b->base.resource_create = virgl_b1nix_resource_create;
   vws_b->base.resource_reference = virgl_b1nix_resource_reference;
   vws_b->base.resource_map = virgl_b1nix_resource_map;
   vws_b->base.resource_wait = virgl_b1nix_resource_wait;
   vws_b->base.resource_is_busy = virgl_b1nix_resource_is_busy;
   vws_b->base.resource_get_storage_size = virgl_b1nix_resource_get_storage_size;
   vws_b->base.cmd_buf_create = virgl_b1nix_cmd_buf_create;
   vws_b->base.cmd_buf_destroy = virgl_b1nix_cmd_buf_destroy;
   vws_b->base.emit_res = virgl_b1nix_emit_res;
   vws_b->base.res_is_referenced = virgl_b1nix_res_is_ref;
   vws_b->base.submit_cmd = virgl_b1nix_submit_cmd;
   vws_b->base.get_caps = virgl_b1nix_get_caps;
   vws_b->base.cs_create_fence = virgl_b1nix_cs_create_fence;
   vws_b->base.fence_wait = virgl_b1nix_fence_wait;
   vws_b->base.fence_reference = virgl_b1nix_fence_reference;
   vws_b->base.fence_get_fd = virgl_b1nix_fence_get_fd;
   vws_b->base.supports_fences = 0;
   vws_b->base.supports_encoded_transfers = 0;
   return &vws_b->base;
}

/* Public entry: open /dev/virtio-gpu and wrap it as a gallium pipe_screen
 * running on the host GPU. Returns NULL if the device is absent (no virgl
 * host) so callers can fall back to softpipe. */
struct pipe_screen *virgl_b1nix_screen_create(void)
{
   int fd = open("/dev/virtio-gpu", O_RDWR);
   struct virgl_winsys *vws;
   struct pipe_screen *screen;

   if (fd < 0)
      return NULL;
   vws = virgl_b1nix_winsys_create(fd);
   if (!vws) {
      close(fd);
      return NULL;
   }
   screen = virgl_create_screen(vws, NULL);
   if (!screen)
      vws->destroy(vws);
   return screen;
}
