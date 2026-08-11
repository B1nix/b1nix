/* SPDX-License-Identifier: MIT
 *
 * M101t: /dev/dri/card1 — the imported DRM core, exposed to userspace.
 *
 * card0 stays what it has always been: b1nix's own small DRM device, which the
 * display server and the M50 tests are written against. This is a second node
 * beside it, served by upstream's file operations through the bridge in
 * kernel/lkpi/drm_chardev.c.
 *
 * Two nodes rather than one replacing the other, deliberately. The imported
 * core's ioctl surface is upstream's, down to the argument structures and the
 * error codes; anything already speaking to card0 would have to be rewritten in
 * the same commit that introduced the new path, and a regression in either
 * would be indistinguishable from a regression in the other. Keeping both means
 * the new surface is proved on its own before anything is moved onto it.
 *
 * Nothing of the DRM core appears in this file. It holds the VFS shapes b1nix
 * needs — a node, a handle, file ops, the per-page mmap callback — and forwards
 * across the boundary, because a translation unit that saw both header sets
 * could not be compiled at all. See the note in <lkpi/env.h>.
 */

#include <b1nix/console.h>
#include <b1nix/drm.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/vfs.h>
#include <lkpi/drmdev.h>

static struct vfs_node *card1_node;

/* ── file operations ────────────────────────────────────────────── */

/* read_user, not read: drm_read copies into the caller's buffer itself, through
 * the uaccess shim that validates the pointer against the calling process. Hand
 * it the syscall layer's kernel bounce buffer instead and every read fails with
 * EFAULT — correctly, since a kernel address is not a user one. */
static isize card1_read_user(struct vfs_handle *h, void *user_buf, usize len) {
  if (!h || !h->private_data)
    return -EBADF;
  return lkpi_drm_read(h->private_data, user_buf, len);
}

static int card1_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  if (!h || !pfd)
    return -EINVAL;
  pfd->revents = (h->private_data && lkpi_drm_readable(h->private_data))
                     ? B1NIX_POLLIN
                     : 0;
  return 0;
}

static int card1_ioctl(struct vfs_handle *h, u64 request, void *arg) {
  if (!h || !h->private_data)
    return -EBADF;
  isize rc = lkpi_drm_ioctl(h->private_data, request, arg);
  /* The core returns a long; b1nix's ioctl is an int. Every DRM ioctl returns
   * 0 or a negative errno, so nothing is lost — but clamp rather than truncate,
   * so a value that did not fit could never arrive as a plausible success. */
  if (rc < -0x7fffffff)
    return -EIO;
  if (rc > 0x7fffffff)
    return -EIO;
  return (int)rc;
}

static void card1_release(struct vfs_handle *h) {
  if (!h)
    return;
  if (h->private_data) {
    lkpi_drm_close(h->private_data);
    h->private_data = 0;
  }
  if (h->node)
    vfs_node_put(h->node);
}

static const struct vfs_file_ops card1_ops = {
    .read_user = card1_read_user,
    .poll = card1_poll,
    .release = card1_release,
    .ioctl = card1_ioctl,
};

/* One page of a buffer object. The offset is the fake mmap offset the core gave
 * userspace out of DRM_IOCTL_MODE_MAP_DUMB; resolving it — including whether
 * this client is allowed the object at all — belongs to the core, so all this
 * does is carry the question across. */
static int card1_mmap_page_phys(struct vfs_handle *handle, u64 offset,
                                u64 *out_phys) {
  if (!handle || !handle->private_data || !out_phys)
    return -EINVAL;
  return lkpi_drm_mmap_page_phys(handle->private_data, offset, out_phys);
}

/* ── open ───────────────────────────────────────────────────────── */

int drm_card1_open(int flags) {
  if (!card1_node)
    return -ENODEV;
  if (!lkpi_drm_have_device())
    return -ENODEV;

  u32 lkpi_flags = 0;
  if (flags & B1NIX_O_NONBLOCK)
    lkpi_flags |= LKPI_DRM_O_NONBLOCK;
  if (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))
    lkpi_flags |= LKPI_DRM_O_WRITE;

  void *file = 0;
  int rc = lkpi_drm_open(lkpi_drm_primary_minor(), lkpi_flags, &file);
  if (rc != 0)
    return rc;

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_NODE);
  if (!h) {
    lkpi_drm_close(file);
    return -ENFILE;
  }
  h->node = vfs_node_get(card1_node);
  h->private_data = file;
  h->ops = &card1_ops;
  h->flags = flags;

  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h);
    return -EMFILE;
  }
  if (flags & B1NIX_O_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

int drm_card1_present(void) { return card1_node != 0; }

/* ── registration ───────────────────────────────────────────────── */

void drm_card1_init(void) {
  /* The device the imported core registered is what makes this node meaningful.
   * Without it the node would open and every ioctl would fail, which is a worse
   * answer than the node not being there. */
  if (!lkpi_drm_have_device())
    return;

  struct vfs_node *dir = vfs_add_node("/dev/dri", VFS_DIRECTORY, 0, 0, 0);
  if (dir && !IS_ERR(dir))
    vfs_node_put(dir);

  card1_node = vfs_add_node("/dev/dri/card1", VFS_DEVICE, 0, 0, 0);
  if (!card1_node || IS_ERR(card1_node)) {
    card1_node = 0;
    console_write("drm: failed to register /dev/dri/card1\n");
    return;
  }
  card1_node->inode->mode = 0600;
  card1_node->inode->mmap_handle_page_phys_cb = card1_mmap_page_phys;
  console_write("drm: /dev/dri/card1 ready (imported DRM core)\n");
}
