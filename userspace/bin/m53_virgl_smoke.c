/* M53 userspace VirGL smoke: drive host-GPU-accelerated 3D from USERSPACE
 * through /dev/virtio-gpu. This is the M52 kernel selftest's accelerated clear,
 * but issued by a userspace program over the device's ioctl/mmap ABI — exactly
 * the kernel/userspace split a Mesa virgl winsys uses. The program creates a
 * render-target resource, builds a virgl command stream (CREATE_OBJECT SURFACE +
 * SET_FRAMEBUFFER_STATE + CLEAR — the same bytes Mesa's virgl driver emits),
 * submits it, copies the GPU-rendered pixels back, mmaps the backing, and
 * verifies the exact clear colour. On a host without VirGL (plain virtio-gpu)
 * the device node is absent and the test reports an honest skip.
 *
 * Markers (M53-VIRGL: ...) consumed by smoke.sh. */

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <b1nix/virgl.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

/* virgl protocol constants (match kernel / Mesa virgl_protocol.h, virgl_hw.h). */
#define VIRGL_CMD0(cmd, obj, len) ((uint32_t)(cmd) | ((uint32_t)(obj) << 8) | ((uint32_t)(len) << 16))
#define VIRGL_CCMD_CREATE_OBJECT 1
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5
#define VIRGL_CCMD_CLEAR 7
#define VIRGL_OBJECT_SURFACE 8
#define VIRGL_FORMAT_B8G8R8A8_UNORM 1
#define PIPE_TEXTURE_2D 2
#define VIRGL_BIND_RENDER_TARGET (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW (1u << 3)
#define PIPE_CLEAR_COLOR0 (1u << 2)

#define DIM 64
#define SURF_HANDLE 1

int main(void) {
  emit("M53-VIRGL: start\n");

  int fd = open("/dev/virtio-gpu", O_RDWR);
  if (fd < 0) {
    /* No virgl-capable host GPU — honest skip, software path is the verified
     * one there. */
    emit("M53-VIRGL: skip no-device\n");
    return 0;
  }

  struct b1nix_virgl_caps caps;
  memset(&caps, 0, sizeof(caps));
  if (ioctl(fd, B1NIX_VIRGL_GET_CAPS, &caps) != 0 || caps.capset_id == 0) {
    emit("M53-VIRGL: fail caps\n");
    return 1;
  }
  emit("M53-VIRGL: ok caps\n");

  /* Fetch the full capset blob (a Mesa virgl winsys reads this at
   * screen-create). virgl_caps_v1/v2 begins with a u32 max_version (1 or 2),
   * so a sane value proves the host actually filled the blob. */
  {
    static uint8_t blob[4096];
    struct b1nix_virgl_caps_data cd;
    memset(&cd, 0, sizeof(cd));
    cd.size = sizeof(blob);
    cd.caps_ptr = (uint64_t)(uintptr_t)blob;
    if (ioctl(fd, B1NIX_VIRGL_GET_CAPS_DATA, &cd) != 0 || cd.size == 0) {
      emit("M53-VIRGL: fail caps-data\n");
      return 1;
    }
    uint32_t maxver = *(uint32_t *)blob;
    if (maxver < 1 || maxver > 100) {
      emit("M53-VIRGL: fail caps-data-blob\n");
      return 1;
    }
    emit("M53-VIRGL: ok caps-data\n");
  }

  struct b1nix_virgl_res_create rc;
  memset(&rc, 0, sizeof(rc));
  rc.target = PIPE_TEXTURE_2D;
  rc.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
  rc.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW;
  rc.width = DIM;
  rc.height = DIM;
  rc.depth = 1;
  rc.array_size = 1;
  if (ioctl(fd, B1NIX_VIRGL_RES_CREATE, &rc) != 0 || rc.size == 0) {
    emit("M53-VIRGL: fail resource\n");
    return 1;
  }
  emit("M53-VIRGL: ok resource\n");

  /* Map the resource backing so we can read the rendered pixels. */
  volatile uint32_t *pixels =
      mmap(0, (size_t)rc.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
           (long)rc.mmap_offset);
  if (pixels == (void *)-1) {
    emit("M53-VIRGL: fail mmap\n");
    return 1;
  }

  /* Build the virgl command stream: wrap the resource as a render surface, bind
   * it as the framebuffer, and CLEAR to (R=0.25, G=0.5, B=0.75, A=1.0). */
  uint32_t cmd[32];
  int n = 0;
  cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
  cmd[n++] = SURF_HANDLE;
  cmd[n++] = rc.res_id;
  cmd[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
  cmd[n++] = 0; /* texture level */
  cmd[n++] = 0; /* first_layer | last_layer << 16 */
  cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
  cmd[n++] = 1; /* nr_cbufs */
  cmd[n++] = 0; /* zsurf handle */
  cmd[n++] = SURF_HANDLE;
  cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
  cmd[n++] = PIPE_CLEAR_COLOR0;
  cmd[n++] = 0x3E800000; /* 0.25f */
  cmd[n++] = 0x3F000000; /* 0.50f */
  cmd[n++] = 0x3F400000; /* 0.75f */
  cmd[n++] = 0x3F800000; /* 1.00f */
  cmd[n++] = 0; /* depth lo */
  cmd[n++] = 0; /* depth hi */
  cmd[n++] = 0; /* stencil */

  struct b1nix_virgl_submit sub;
  memset(&sub, 0, sizeof(sub));
  sub.cmd_size = (uint32_t)(n * sizeof(uint32_t));
  sub.cmd_ptr = (uint64_t)(uintptr_t)cmd;
  if (ioctl(fd, B1NIX_VIRGL_SUBMIT, &sub) != 0) {
    emit("M53-VIRGL: fail submit\n");
    return 1;
  }
  emit("M53-VIRGL: ok submit\n");

  /* Copy the GPU-rendered surface back into the resource backing. */
  struct b1nix_virgl_transfer xfer;
  memset(&xfer, 0, sizeof(xfer));
  xfer.res_id = rc.res_id;
  xfer.box.w = DIM;
  xfer.box.h = DIM;
  xfer.box.d = 1;
  if (ioctl(fd, B1NIX_VIRGL_TRANSFER_FROM_HOST, &xfer) != 0) {
    emit("M53-VIRGL: fail transfer\n");
    return 1;
  }

  /* B8G8R8A8 memory order is B,G,R,A. 0.25/0.5/0.75/1.0 * 255 ~ 64/128/191/255. */
  uint32_t px = pixels[0];
  uint32_t b = px & 0xFF, g = (px >> 8) & 0xFF, r = (px >> 16) & 0xFF,
           a = (px >> 24) & 0xFF;
  if (a >= 250 && r >= 60 && r <= 68 && g >= 124 && g <= 132 && b >= 187 &&
      b <= 195) {
    emit("M53-VIRGL: ok path-accelerated\n");
  } else {
    emit("M53-VIRGL: fail path-accelerated\n");
    return 1;
  }

  emit("M53-VIRGL: done\n");
  return 0;
}
