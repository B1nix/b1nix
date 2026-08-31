/* The new mount API — fsopen, fsconfig, fsmount, open_tree, move_mount and
 * mount_setattr.
 *
 * mount(2) names a filesystem, its options and its place in one call, and
 * reports one error for all three. The API that replaced it splits them:
 *
 *   fd  = fsopen("tmpfs")            a filesystem type, not yet a filesystem
 *   fsconfig(fd, SET_STRING, ...)    one option, with its own error
 *   fsconfig(fd, CMD_CREATE)         now the superblock exists
 *   mfd = fsmount(fd, ..., attrs)    a mount, attached NOWHERE
 *   move_mount(mfd, "", …, target)   and now it is somewhere
 *
 * systemd 254 and later use it for every unit sandbox it builds — credentials
 * directories, RuntimeDirectory=, ProtectSystem= — and fall back to mount(2)
 * only when the whole family is absent. Arch's systemd 261 got as far as
 * "Failed at step CREDENTIALS ... Function not implemented" on this kernel and
 * stopped there, taking journald, logind, udevd and dbus-broker with it.
 *
 * The interesting half is not here: it is the mount that exists while attached
 * to nothing, which b1nix's path-keyed mount table could not express. See the
 * detached-mount table in kernel/fs/vfs.c. This file is the descriptor side —
 * what a caller holds between the steps above.
 *
 * A partial implementation of this family is worse than none, and that is not
 * a guess: implementing open_tree and move_mount alone once took Debian's
 * systemd suite from 32 checks to 18, because systemd probes for the calls and
 * switches strategy wholesale when it finds them. So either every call here
 * works or the whole family must be withdrawn together.
 */

#include <b1nix/vfs.h>
#include <b1nix/sched.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <string.h>
#include <b1nix/bootinfo.h>
#include <b1nix/klog.h>
#include <stdio.h>

/* `b1nix.trace-mount` also covers this file: it names the call, the command and
 * the key, which is the only way to tell WHICH step of a caller's sequence a
 * refusal came from — the syscall number alone says "fsconfig" for eight
 * different operations. */
static void mount_api_trace(const char *what, u32 cmd, const char *key,
                            int rc) {
  if (!bootinfo_has_flag("b1nix.trace-mount"))
    return;

  char line[192];
  snprintf(line, sizeof(line), "mount-api: %s cmd=%u key='%s' -> %d", what, cmd,
           key ? key : "", rc);
  klog_info(line);
}

/* fsconfig(2) commands. */
#define FSCONFIG_SET_FLAG       0
#define FSCONFIG_SET_STRING     1
#define FSCONFIG_SET_BINARY     2
#define FSCONFIG_SET_PATH       3
#define FSCONFIG_SET_PATH_EMPTY 4
#define FSCONFIG_SET_FD         5
#define FSCONFIG_CMD_CREATE     6
#define FSCONFIG_CMD_RECONFIGURE 7

#define FSOPEN_CLOEXEC  0x00000001
#define FSMOUNT_CLOEXEC 0x00000001

#define OPEN_TREE_CLONE 1

/* AT_RECURSIVE: the call applies to everything mounted under the path too. */
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif

/* A filesystem between fsopen and fsconfig(CMD_CREATE): a type, the options
 * gathered so far, and — once created — the detached mount they produced. */
struct fsctx_state {
  char fstype[16];
  char source[VFS_MAX_PATH];
  u64 flags;       /* MS_* accumulated from SET_FLAG/SET_STRING */
  int detached_id; /* -1 until CMD_CREATE succeeds */
  /* Whether closing this context tears the filesystem down. fsmount hands the
   * mount to a descriptor, but the context KEEPS REFERRING to the filesystem:
   * systemd sets `ro` and calls fsconfig(CMD_RECONFIGURE) on the context after
   * fsmount, and dropping the reference there made that last step fail with
   * EINVAL — the whole credentials sequence, one call from done. */
  int owns_mount;
};

/* What fsmount and open_tree hand back.
 *
 * The descriptor is an ordinary O_PATH directory descriptor onto the mount's
 * private path, because callers use it as one: systemd populates a credentials
 * directory with openat(mfd, ".", ...) before moving it into place, and a
 * descriptor with nothing behind it answers EBADF — which is exactly how the
 * first version of this failed. The detached id rides along so close() knows
 * there is a mount to tear down if move_mount never came. */
struct mountfd_state {
  int detached_id;
  int attached; /* move_mount consumed it; closing must not release it again */
};

static void fsctx_release(struct vfs_handle *h) {
  struct fsctx_state *ctx = h->private_data;

  if (!ctx)
    return;
  /* A superblock created but never turned into a mount dies with the context.
   * Linux does the same: the fs context owns the superblock until fsmount
   * takes it. */
  if (ctx->owns_mount && ctx->detached_id >= 0)
    vfs_detached_release(ctx->detached_id);
  kfree(ctx);
  h->private_data = 0;
}

static void mountfd_release(struct vfs_handle *h) {
  struct mountfd_state *m = h->private_data;

  if (!m)
    return;
  if (!m->attached && m->detached_id >= 0)
    vfs_detached_release(m->detached_id);
  kfree(m);
  h->private_data = 0;
}

static const struct vfs_file_ops fsctx_ops = {
    .release = fsctx_release,
};

static const struct vfs_file_ops mountfd_ops = {
    .release = mountfd_release,
};

/* One option, as fsconfig names them.
 *
 * Three kinds have to be told apart, because a caller reads the difference:
 *
 *   - an option that changes SAFETY (ro, nosuid, nodev, noexec) maps onto a
 *     real MS_* bit and is applied;
 *   - an option a filesystem understands but this kernel does not act on
 *     (tmpfs sizing and ownership) is accepted, because failing a unit over a
 *     sizing hint helps nobody;
 *   - anything else is EINVAL, which is what Linux answers for a parameter a
 *     filesystem does not have. systemd PROBES with a deliberately absent
 *     option — `adefinitelynotexistingmountoption` — to find out whether the
 *     kernel validates its options at all, and an implementation that accepts
 *     everything tells it the wrong thing about every option after that.
 */
static int fsctx_apply_option(struct fsctx_state *ctx, const char *key,
                              const char *value) {
  static const char *const accepted_hints[] = {
      "size",  "nr_blocks", "nr_inodes", "mode", "uid",
      "gid",   "huge",      "noswap",    "mpol", "inode32",
      "inode64", "quota",   "usrquota",  "grpquota",
  };

  if (!key || !key[0])
    return -EINVAL;

  if (strcmp(key, "ro") == 0) {
    ctx->flags |= MS_RDONLY;
    return 0;
  }
  if (strcmp(key, "rw") == 0) {
    ctx->flags &= ~(u64)MS_RDONLY;
    return 0;
  }
  if (strcmp(key, "nosuid") == 0) {
    ctx->flags |= MS_NOSUID;
    return 0;
  }
  if (strcmp(key, "nodev") == 0) {
    ctx->flags |= MS_NODEV;
    return 0;
  }
  if (strcmp(key, "noexec") == 0) {
    ctx->flags |= MS_NOEXEC;
    return 0;
  }
  if (strcmp(key, "source") == 0) {
    if (!value)
      return -EINVAL;
    strncpy(ctx->source, value, sizeof(ctx->source) - 1);
    ctx->source[sizeof(ctx->source) - 1] = '\0';
    return 0;
  }

  for (usize i = 0; i < sizeof(accepted_hints) / sizeof(accepted_hints[0]); i++)
    if (strcmp(key, accepted_hints[i]) == 0)
      return 0;

  return -EINVAL; /* no such parameter */
}

/* Build the descriptor for a detached mount: an O_PATH directory descriptor on
 * its private path, tagged so close() can tear the mount down if move_mount
 * never claimed it. `owned_id` is cleared on success — the fs context that
 * handed the mount over must no longer release it. */
static int mountfd_open(int detached_id, int cloexec, int *owned_id) {
  char path[VFS_MAX_PATH];
  int rc = vfs_detached_path(detached_id, path, sizeof(path));

  if (rc < 0)
    return rc;

  int fd = vfs_open_flags(path, B1NIX_O_PATH | B1NIX_O_DIRECTORY);
  if (fd < 0)
    return fd;

  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h) {
    vfs_close(fd);
    return -EBADF;
  }

  struct mountfd_state *m = kzalloc(sizeof(*m));
  if (!m) {
    vfs_close(fd);
    return -ENOMEM;
  }
  m->detached_id = detached_id;
  m->attached = 0;

  /* The handle keeps its node and its ordinary kind — that is the whole point,
   * it has to behave like a directory descriptor. The mount state rides in
   * private_data, and the release hook runs on close. */
  h->private_data = m;
  h->ops = &mountfd_ops;

  if (cloexec)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  if (owned_id)
    *owned_id = -1;
  return fd;
}

int vfs_fsopen(const char *fstype, u32 flags) {
  if (!fstype || !fstype[0])
    return -EINVAL;
  if (flags & ~(u32)FSOPEN_CLOEXEC)
    return -EINVAL;

  struct fsctx_state *ctx = kzalloc(sizeof(*ctx));
  if (!ctx)
    return -ENOMEM;
  strncpy(ctx->fstype, fstype, sizeof(ctx->fstype) - 1);
  ctx->fstype[sizeof(ctx->fstype) - 1] = '\0';
  ctx->detached_id = -1;

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_FSCTX);
  if (!h) {
    kfree(ctx);
    return -ENFILE;
  }
  h->private_data = ctx;
  h->ops = &fsctx_ops;
  h->flags = B1NIX_O_RDWR;

  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h);
    return fd == -ENOMEM ? -ENOMEM : -EMFILE;
  }
  if (flags & FSOPEN_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

int vfs_fsconfig(int fd, u32 cmd, const char *key, const char *value,
                 int aux) {
  (void)aux;
  struct vfs_handle *h = scheduler_fd_get(fd);

  if (!h || h->kind != VFS_HANDLE_FSCTX)
    return -EINVAL; /* Linux: not a filesystem context */
  struct fsctx_state *ctx = h->private_data;
  if (!ctx)
    return -EINVAL;

  int rc;

  switch (cmd) {
  case FSCONFIG_SET_FLAG:
    rc = fsctx_apply_option(ctx, key, 0);
    mount_api_trace("fsconfig", cmd, key, rc);
    return rc;
  case FSCONFIG_SET_STRING:
    rc = fsctx_apply_option(ctx, key, value);
    mount_api_trace("fsconfig", cmd, key, rc);
    return rc;
  case FSCONFIG_SET_PATH:
  case FSCONFIG_SET_PATH_EMPTY:
    /* A path-valued option names a source. Nothing here takes another kind. */
    rc = fsctx_apply_option(ctx, key, value);
    mount_api_trace("fsconfig", cmd, key, rc);
    return rc;
  case FSCONFIG_CMD_CREATE:
    if (ctx->detached_id >= 0)
      return -EBUSY; /* already created */
    {
      int id = vfs_detached_create(ctx->fstype, ctx->source[0] ? ctx->source : 0,
                                   ctx->flags);
      mount_api_trace("fsconfig-create", cmd, ctx->fstype, id);
      if (id < 0)
        return id;
      ctx->detached_id = id;
      ctx->owns_mount = 1;
    }
    return 0;
  case FSCONFIG_CMD_RECONFIGURE:
    /* Apply the options gathered since CMD_CREATE to the filesystem that call
     * built. This is how a caller seals a mount after filling it: systemd
     * creates the credentials tmpfs, writes the credentials into it, then sets
     * `ro` and reconfigures — so refusing this refused the whole sequence one
     * step from the end. Only a context that still owns its filesystem can do
     * it; once fsmount has taken it, the mount is the caller's to change
     * through mount_setattr. */
    if (ctx->detached_id < 0) {
      mount_api_trace("fsconfig-reconfigure", cmd, key, -EINVAL);
      return -EINVAL;
    }
    {
      u64 set = 0;
      u64 clr = 0;

      if (ctx->flags & MS_RDONLY)
        set |= MOUNT_ATTR_RDONLY;
      else
        clr |= MOUNT_ATTR_RDONLY;
      if (ctx->flags & MS_NOSUID)
        set |= MOUNT_ATTR_NOSUID;
      if (ctx->flags & MS_NODEV)
        set |= MOUNT_ATTR_NODEV;
      if (ctx->flags & MS_NOEXEC)
        set |= MOUNT_ATTR_NOEXEC;
      rc = vfs_detached_set_attr(ctx->detached_id, set, clr);
    }
    mount_api_trace("fsconfig-reconfigure", cmd, key, rc);
    return rc;
  case FSCONFIG_SET_BINARY:
  case FSCONFIG_SET_FD:
    mount_api_trace("fsconfig-unsupported", cmd, key, -EOPNOTSUPP);
    return -EOPNOTSUPP;
  default:
    mount_api_trace("fsconfig-unknown", cmd, key, -EINVAL);
    return -EINVAL;
  }
}

int vfs_fsmount(int fsfd, u32 flags, u32 attr_flags) {
  if (flags & ~(u32)FSMOUNT_CLOEXEC)
    return -EINVAL;
  /* Only the attributes that mean something get through: an unknown bit is a
   * caller asking for a guarantee this kernel would not be providing. */
  if (attr_flags & ~(u32)(MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID |
                          MOUNT_ATTR_NODEV | MOUNT_ATTR_NOEXEC |
                          MOUNT_ATTR__ATIME | MOUNT_ATTR_NODIRATIME |
                          MOUNT_ATTR_NOSYMFOLLOW))
    return -EINVAL;

  struct vfs_handle *h = scheduler_fd_get(fsfd);
  if (!h || h->kind != VFS_HANDLE_FSCTX)
    return -EINVAL;
  struct fsctx_state *ctx = h->private_data;
  if (!ctx)
    return -EINVAL;
  if (ctx->detached_id < 0)
    return -EINVAL; /* fsconfig(CMD_CREATE) has not run */

  int rc = vfs_detached_set_attr(ctx->detached_id, attr_flags, 0);
  if (rc < 0)
    return rc;

  /* The descriptor owns the mount from here; the context keeps referring to
   * the filesystem so it can still be reconfigured. */
  int fd = mountfd_open(ctx->detached_id, (flags & FSMOUNT_CLOEXEC) ? 1 : 0, 0);
  if (fd >= 0)
    ctx->owns_mount = 0;
  return fd;
}

int vfs_open_tree(const char *path, u32 flags) {
  if (!path || !path[0])
    return -EINVAL;

  /* Without OPEN_TREE_CLONE the call is a path-reference descriptor — exactly
   * open(O_PATH), which this kernel already has. */
  if (!(flags & OPEN_TREE_CLONE))
    return vfs_open_flags(path, B1NIX_O_PATH | B1NIX_O_DIRECTORY);

  int id = vfs_detached_clone_path(path, (flags & AT_RECURSIVE) ? 1 : 0);
  if (id < 0)
    return id;

  int owned = id;
  int fd = mountfd_open(id, 1, &owned);
  if (fd < 0)
    vfs_detached_release(id);
  return fd;
}

int vfs_move_mount_fd(int from_fd, const char *from_path, const char *to_path,
                      u32 flags) {
  (void)flags;
  if (!to_path || !to_path[0])
    return -EINVAL;

  /* The descriptor form: a detached mount finally gets a place. This is the
   * one that matters — it is how everything fsopen built becomes reachable. */
  struct vfs_handle *h = from_fd >= 0 ? scheduler_fd_get(from_fd) : 0;
  if (h && h->ops == &mountfd_ops) {
    struct mountfd_state *m = h->private_data;

    if (!m || m->attached || m->detached_id < 0)
      return -EINVAL;
    int rc = vfs_detached_attach(m->detached_id, to_path);
    if (rc < 0)
      return rc;
    m->attached = 1;
    return 0;
  }

  /* The path form is MS_MOVE by another name. */
  if (!from_path || !from_path[0])
    return -EINVAL;
  return vfs_move_mount(from_path, to_path);
}

int vfs_mount_setattr_fd(int fd, const char *path, u32 flags,
                         const struct b1nix_mount_attr *attr) {
  if (!attr)
    return -EINVAL;
  /* There is no mount idmapping here, and a caller asking for one is asking
   * for a guarantee about who owns the files it is about to see. */
  if (attr->userns_fd)
    return -EOPNOTSUPP;
  /* propagation is the same set mount(MS_SHARED|MS_SLAVE|...) takes, and
   * systemd passes MOUNT_ATTR_* and propagation in ONE call for every sandbox
   * it builds — refusing the pair failed the unit over the half this kernel
   * has implemented all along. */
  if (attr->propagation &&
      (attr->propagation & ~(u64)MS_PROPAGATION_MASK) != 0)
    return -EINVAL;
  if ((attr->attr_set | attr->attr_clr) &
      ~(u64)(MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV |
             MOUNT_ATTR_NOEXEC | MOUNT_ATTR__ATIME | MOUNT_ATTR_NODIRATIME |
             MOUNT_ATTR_NOSYMFOLLOW))
    return -EINVAL;
  if (attr->attr_set & attr->attr_clr)
    return -EINVAL; /* setting and clearing the same bit */

  int recursive = (flags & AT_RECURSIVE) ? 1 : 0;

  /* A descriptor from fsmount/open_tree: the mount is not attached yet, so the
   * attributes are recorded on the detached entry and applied when it lands. */
  struct vfs_handle *h = fd >= 0 ? scheduler_fd_get(fd) : 0;
  if (h && h->ops == &mountfd_ops) {
    struct mountfd_state *m = h->private_data;

    if (!m || m->attached || m->detached_id < 0)
      return -EINVAL;
    int rc = vfs_detached_set_attr(m->detached_id, attr->attr_set,
                                   attr->attr_clr);
    if (rc < 0)
      return rc;
    if (attr->propagation) {
      char dpath[VFS_MAX_PATH];

      if (vfs_detached_path(m->detached_id, dpath, sizeof(dpath)) == 0)
        rc = vfs_set_propagation(dpath, attr->propagation |
                                            (recursive ? MS_REC : 0));
    }
    return rc;
  }

  if (!path || !path[0])
    return -EINVAL;

  int rc = vfs_mount_setattr_path(path, attr->attr_set, attr->attr_clr,
                                  recursive);
  if (rc < 0)
    return rc;
  if (attr->propagation)
    rc = vfs_set_propagation(path, attr->propagation | (recursive ? MS_REC : 0));
  return rc;
}
