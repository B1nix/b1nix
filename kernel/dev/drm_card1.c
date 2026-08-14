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

#include <stdio.h>
#include <string.h>
#include <lkpi/drm_bridge.h>
#include <b1nix/console.h>
#include <b1nix/drm.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/vfs.h>
#include <lkpi/drmdev.h>
#include <lkpi/env.h>

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
  int watch_commit = ((request & 0xffffffffu) == 0xc03864bcu &&
                      bootinfo_has_flag("b1nix.drm-debug"));
  extern void lkpi_i915_dump_port_state_pub(void) __attribute__((weak));

  if (watch_commit && lkpi_i915_dump_port_state_pub) {
    console_write("drm: before atomic commit:\n");
    lkpi_i915_dump_port_state_pub();
  }

  u64 t0 = lkpi_monotonic_ns();
  /* An ioctl that never returns prints nothing at all, and "the compositor is
   * idle" and "the compositor is stuck inside the driver" look identical from
   * the log. The watchdog thread reports the difference. */
  lkpi_diag_watch_begin("drm ioctl", request & 0xffffffffu);
  isize rc = lkpi_drm_ioctl(h->private_data, request, arg);
  lkpi_diag_watch_end();
  /* Name any call that takes longer than a person would wait for a keypress.
   * Connector probing on this machine costs minutes somewhere, and knowing
   * which command spends them is the whole question. */
  {
    u64 ms = (lkpi_monotonic_ns() - t0) / 1000000ull;

    if (ms >= 200) {
      char line[96];

      snprintf(line, sizeof(line), "drm: ioctl %x took %llu ms\n",
               (unsigned)(request & 0xffffffffu), (unsigned long long)ms);
      console_write(line);
    }
  }
  /* The display's registers at the instant an atomic commit returns.
   *
   * A commit that reports success and leaves every transcoder disabled is the
   * whole question here, and a periodic dump cannot answer it: the pipe may be
   * enabled and torn down again between two samples. */
  if (watch_commit) {
    console_write("drm: after atomic commit:\n");
    if (lkpi_i915_dump_port_state_pub)
      lkpi_i915_dump_port_state_pub();
    else
      console_write("drm: (no i915 dump in this build)\n");
  }
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
  lkpi_drm_file_set_handle(file, h);
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

/*
 * One node per device the imported core registered.
 *
 * card1 used to be the only one, pointed at whichever device registered last.
 * With a vendor driver bound to real hardware there is more than one, and the
 * offsets userspace maps are per-device — so each gets its own node and each
 * node remembers the minor it opens, rather than all of them resolving through
 * a single global.
 *
 * Numbering starts at 1: card0 is b1nix's own DRM device, not this core's.
 */
#define DRM_IMPORTED_MAX 4
static struct {
	struct vfs_node *node;
	u32 minor;
	char path[24];
} g_cards[DRM_IMPORTED_MAX];
static unsigned g_card_count;

/* The node published for an imported device's minor, or NULL. */
struct vfs_node *drm_card_node_for_minor(u32 minor) {
  for (unsigned i = 0; i < g_card_count; i++)
    if (g_cards[i].minor == minor)
      return g_cards[i].node;
  return 0;
}

/* Make a descriptor the DRM core created for itself behave as the card it came
 * from: same node, so it stats as that device, and the same file operations, so
 * ioctls reach the driver. With the node alone it identified correctly and then
 * failed every ioctl, which reads as a device with no capabilities. */
void drm_card_attach_handle(struct vfs_handle *h, u32 minor) {
  struct vfs_node *node = drm_card_node_for_minor(minor);

  if (!h || !node || h->node)
    return;
  h->node = vfs_node_get(node);
  h->ops = &card1_ops;
}


/* The card whose node matches this path, or NULL. */
static int card_index_for_path(const char *path) {
  for (unsigned i = 0; i < g_card_count; i++)
    if (strcmp(g_cards[i].path, path) == 0)
      return (int)i;
  return -1;
}

int drm_imported_card_present(const char *path) {
  return card_index_for_path(path) >= 0;
}

int drm_imported_card_open(const char *path, int flags) {
  int idx = card_index_for_path(path);

  if (idx < 0)
    return -ENODEV;

  u32 lkpi_flags = 0;
  if (flags & B1NIX_O_NONBLOCK)
    lkpi_flags |= LKPI_DRM_O_NONBLOCK;
  if (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))
    lkpi_flags |= LKPI_DRM_O_WRITE;

  void *file = 0;
  int rc = lkpi_drm_open(g_cards[idx].minor, lkpi_flags, &file);
  if (rc != 0)
    return rc;

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_NODE);
  if (!h) {
    lkpi_drm_close(file);
    return -ENFILE;
  }
  h->node = vfs_node_get(g_cards[idx].node);
  h->private_data = file;
  lkpi_drm_file_set_handle(file, h);
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

void drm_card1_init(void) {
  /* The device the imported core registered is what makes this node meaningful.
   * Without it the node would open and every ioctl would fail, which is a worse
   * answer than the node not being there. */
  if (!lkpi_drm_have_device())
    return;

  struct vfs_node *dir = vfs_add_node("/dev/dri", VFS_DIRECTORY, 0, 0, 0);
  if (dir && !IS_ERR(dir))
    vfs_node_put(dir);

  /*
   * Called a second time once the real root is mounted, for the same reason
   * fb_dev_init and input_init are: nodes created during early boot live on the
   * initramfs root, which stops being reachable when "/" redirects to the ext4
   * filesystem. Start the registry over so the cards keep the names they had —
   * appending instead would publish the same devices again as card3 and card4,
   * and leave nothing at all where userspace looks for them.
   */
  g_card_count = 0;
  card1_node = 0;

  unsigned devices = lkpi_drm_device_count();

  for (unsigned i = 0; i < devices && g_card_count < DRM_IMPORTED_MAX; i++) {
    u32 minor = 0;

    if (lkpi_drm_minor_at(i, &minor) != 0)
      continue;

    char *p = g_cards[g_card_count].path;
    const char *prefix = "/dev/dri/card";
    usize n = 0;
    while (prefix[n]) { p[n] = prefix[n]; n++; }
    /* Numbered from 1 — card0 belongs to b1nix's own device. */
    unsigned num = g_card_count + 1;
    if (num >= 10)
      p[n++] = (char)('0' + (num / 10));
    p[n++] = (char)('0' + (num % 10));
    p[n] = 0;

    struct vfs_node *node = vfs_add_node(p, VFS_DEVICE, 0, 0, 0);
    if (!node || IS_ERR(node)) {
      console_write("drm: failed to register ");
      console_write(p);
      console_write("\n");
      continue;
    }
    node->inode->mode = 0600;
    /* DRM's device numbers, which is how a program tells a card from anything
     * else: seatd classifies a device by major before agreeing to open it, and
     * card0 above carries the same. inode->rdev is what fstat reports as
     * st_rdev. */
    node->inode->rdev = ((u64)226 << 8) | (u64)num;
    drm_sysfs_publish_card(num);
    node->inode->mmap_handle_page_phys_cb = card1_mmap_page_phys;
    g_cards[g_card_count].node = node;
    g_cards[g_card_count].minor = minor;
    g_card_count++;
    if (g_card_count == 1)
      card1_node = node;
    console_write("drm: ");
    console_write(p);
    console_write(" ready (imported DRM core)\n");
  }
  if (g_card_count == 0)
    return;
}
