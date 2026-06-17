#ifndef B1NIX_U_VIRGL_H
#define B1NIX_U_VIRGL_H

/* Userspace ABI for /dev/virtio-gpu — the b1nix VirGL 3D transport.
 *
 * This exposes the kernel's VirGL command path (negotiated with a host
 * virglrenderer-backed device, M52) to userspace so a renderer can drive
 * host-GPU-accelerated 3D: create a render-target resource, submit a virgl
 * command stream (the same byte format Mesa's virgl driver emits), copy the
 * GPU-rendered result back, and mmap the resource backing to read pixels.
 *
 * v1 uses a single implicit 3D context (created by the kernel when the device
 * initialises), which is enough to prove the userspace -> kernel -> host-GPU
 * path and to back a Mesa virgl winsys. Keep these structs matched with the
 * kernel copy in kernel/dev/virtio_gpu.c. */

#include <stdint.h>

#define B1NIX_VIRGL_GET_CAPS 0x7601           /* arg: struct b1nix_virgl_caps*  */
#define B1NIX_VIRGL_GET_CAPS_DATA 0x7602      /* arg: struct b1nix_virgl_caps_data* */
#define B1NIX_VIRGL_RES_CREATE 0x7603         /* arg: struct b1nix_virgl_res_create* */
#define B1NIX_VIRGL_SUBMIT 0x7604             /* arg: struct b1nix_virgl_submit* */
#define B1NIX_VIRGL_TRANSFER_FROM_HOST 0x7605 /* arg: struct b1nix_virgl_transfer* */

struct b1nix_virgl_caps {
  uint32_t capset_id;      /* out: VIRGL capset id (1 = VIRGL, 2 = VIRGL2) */
  uint32_t capset_version; /* out */
  uint32_t capset_size;    /* out: bytes of capset data on the host */
  uint32_t _pad;
};

/* GET_CAPS_DATA: fetch the full capset blob (virgl_caps_v2) into caps_ptr.
 * A Mesa virgl winsys reads this at screen-create to learn the host GPU's
 * feature/format/limit set. */
struct b1nix_virgl_caps_data {
  uint32_t capset_id;      /* out */
  uint32_t capset_version; /* out */
  uint32_t size;           /* in: caps_ptr buffer bytes; out: host blob size */
  uint32_t _pad;
  uint64_t caps_ptr;       /* in: userspace buffer to receive the blob */
};

struct b1nix_virgl_res_create {
  uint32_t target;      /* in: pipe_texture_target (2 = PIPE_TEXTURE_2D) */
  uint32_t format;      /* in: virgl format (1 = B8G8R8A8_UNORM) */
  uint32_t bind;        /* in: VIRGL_BIND_* mask */
  uint32_t width;       /* in */
  uint32_t height;      /* in */
  uint32_t depth;       /* in (1 for 2D) */
  uint32_t array_size;  /* in (1) */
  uint32_t res_id;      /* out: assigned resource id */
  uint64_t mmap_offset; /* out: offset to pass to mmap() of /dev/virtio-gpu */
  uint64_t size;        /* out: backing size in bytes */
};

struct b1nix_virgl_box {
  uint32_t x, y, z, w, h, d;
};

struct b1nix_virgl_transfer {
  uint32_t res_id; /* in */
  uint32_t level;  /* in (0) */
  struct b1nix_virgl_box box;
  uint64_t offset; /* in (0) */
};

struct b1nix_virgl_submit {
  uint32_t cmd_size; /* in: command stream size in bytes */
  uint32_t _pad;
  uint64_t cmd_ptr; /* in: userspace pointer to the virgl command dwords */
};

#endif /* B1NIX_U_VIRGL_H */
