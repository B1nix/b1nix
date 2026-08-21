/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Runtime-registered /sys attribute files (M101).
 *
 * The rest of /sys (kernel/fs/sysfs.c) is a tree built once at mount from
 * render callbacks. This is the other half: entries a driver publishes when it
 * registers, which is necessarily after the kernel decided what /sys looks
 * like — and, just as often, before /sys is mounted at all.
 *
 * The registry is therefore the authority and the VFS tree is a projection of
 * it. Registering while unmounted records the entry; the mount materialises
 * everything recorded so far; registering afterwards materialises immediately.
 * Nothing in the caller has to know which of those happened, which matters
 * because probe order and mount order have no reason to agree.
 *
 * Reads are not cached. A file's value is whatever its callback returns at the
 * moment userspace reads it — for a status file, a cached answer would be a
 * wrong one.
 */

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/netlink.h>
#include <b1nix/posix.h>
#include <b1nix/sysfs_attr.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <string.h>

/* One sysfs read fills at most this much. Linux hands an attribute a page and
 * says so in its own documentation; matching that keeps ported show() functions
 * honest instead of letting them rely on a larger buffer here. */
#define SYSFS_ATTR_MAX 4096

struct sysfs_attr {
  struct sysfs_attr *next;
  char name[64];
  u16 mode;
  sysfs_attr_show show;       /* renders the whole value; offset served here */
  sysfs_attr_read_at read_at; /* or reads at an offset itself; never both */
  sysfs_attr_store store;
  sysfs_attr_release release;
  void *ctx;
  struct vfs_node *node; /* set once materialised */
};

struct sysfs_link {
  struct sysfs_link *next;
  char name[64];
  char *target;
  struct vfs_node *node;
};

struct sysfs_dir {
  struct sysfs_dir *parent;
  struct sysfs_dir *sibling;  /* next child of the same parent */
  struct sysfs_dir *children; /* first child */
  struct sysfs_attr *attrs;
  struct sysfs_link *links;
  char name[64];
  struct vfs_node *node; /* set once materialised */
  int adopted;           /* the node predates us: borrowed, not owned */
};

/* The registry's own root stands in for the /sys mount point. It exists from
 * the first registration; its `node` is filled in when /sys is mounted. */
static struct sysfs_dir g_root;
static struct sysfs_dir *g_debug_root;

static void copy_name(char *dst, usize cap, const char *src) {
  usize n = strlen(src);
  if (n > cap - 1)
    n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

/* ── the VFS projection ─────────────────────────────────────────── */

static isize sysfs_attr_read_cb(struct vfs_node *node, u64 offset, char *buffer,
                                usize size, int flags) {
  (void)flags;
  struct sysfs_attr *attr = node->inode ? node->inode->data : 0;
  if (!attr)
    return 0;

  /* A reader that takes the offset itself gets it: a dump larger than one
   * buffer is then continued from where the last read stopped, rather than
   * re-rendered and truncated at the same place forever. */
  if (attr->read_at)
    return attr->read_at(attr->ctx, buffer, size, offset);

  if (!attr->show)
    return 0;

  /* Render the whole value, then serve the requested window of it. A show()
   * that cannot see the offset is the contract Linux attributes are written
   * to, and re-rendering per read is what keeps the value current. */
  char *tmp = kmalloc(SYSFS_ATTR_MAX);
  if (!tmp)
    return -ENOMEM;
  isize len = attr->show(attr->ctx, tmp, SYSFS_ATTR_MAX);
  if (len < 0) {
    kfree(tmp);
    return len;
  }
  if (offset >= (u64)len) {
    kfree(tmp);
    return 0;
  }
  usize avail = (usize)len - (usize)offset;
  usize n = avail < size ? avail : size;
  memcpy(buffer, tmp + (usize)offset, n);
  kfree(tmp);
  return (isize)n;
}

static isize sysfs_attr_write_cb(struct vfs_node *node, u64 offset,
                                 const char *buffer, usize size, int flags) {
  (void)offset;
  (void)flags;
  struct sysfs_attr *attr = node->inode ? node->inode->data : 0;
  if (!attr || !attr->store)
    return -EACCES;
  return attr->store(attr->ctx, buffer, size);
}

static struct vfs_node *mk_node(struct vfs_node *parent, const char *name,
                                enum vfs_node_type type, u16 mode) {
  struct vfs_node *n = vfs_create_node(type);
  if (!n)
    return 0;
  copy_name(n->name, sizeof(n->name), name);
  n->inode->mode = mode;
  n->inode->nlink = (type == VFS_DIRECTORY) ? 2 : 1;
  /* A sysfs attribute is a regular file, not a character device. */
  if (type == VFS_DEVICE || type == VFS_FILE)
    n->inode->flags |= VFS_NODE_PSEUDO_REG;
  n->parent = parent;
  n->refcount++;
  vfs_attach_child(parent, n);
  return n;
}

static void materialise_attr(struct sysfs_dir *dir, struct sysfs_attr *attr) {
  if (attr->node || !dir->node)
    return;
  /* Never shadow a file the static tree already publishes: /sys is one
   * namespace, and a second node under the same name would win lookups by
   * accident of insertion order. */
  if (find_child(dir->node, attr->name))
    return;
  struct vfs_node *n =
      mk_node(dir->node, attr->name, VFS_DEVICE, attr->mode);
  if (!n)
    return;
  n->inode->data = attr;
  n->inode->read_cb = sysfs_attr_read_cb;
  if (attr->store)
    n->inode->write_cb = sysfs_attr_write_cb;
  attr->node = n;
}

static void materialise_link(struct sysfs_dir *dir, struct sysfs_link *link) {
  if (link->node || !dir->node || !link->target)
    return;
  if (find_child(dir->node, link->name))
    return;
  struct vfs_node *n = mk_node(dir->node, link->name, VFS_SYMLINK, 0777);
  if (!n)
    return;
  n->inode->data = link->target;
  n->inode->size = strlen(link->target);
  link->node = n;
}

/* Depth-first, parents before children: a child's VFS node needs its parent's
 * to exist first. */
static void materialise_dir(struct sysfs_dir *dir) {
  if (!dir->node && dir->parent && dir->parent->node) {
    /*
     * Adopt the directory if it is already there rather than creating a second
     * one. /sys/kernel, /sys/class and /sys/devices are built by the static
     * tree at mount; a driver that registered under one of those names before
     * the mount would otherwise get a same-named sibling, and every lookup for
     * the original — /sys/kernel/osrelease, /sys/class/net/lo — would land in
     * the empty one instead. The registry owns what it created and borrows the
     * rest, which is why `adopted` decides whether removal unlinks anything.
     */
    struct vfs_node *existing = find_child(dir->parent->node, dir->name);
    if (existing) {
      dir->node = existing;
      dir->adopted = 1;
    } else {
      dir->node = mk_node(dir->parent->node, dir->name, VFS_DIRECTORY, 0555);
    }
  }
  if (!dir->node)
    return;
  for (struct sysfs_attr *a = dir->attrs; a; a = a->next)
    materialise_attr(dir, a);
  for (struct sysfs_link *l = dir->links; l; l = l->next)
    materialise_link(dir, l);
  for (struct sysfs_dir *c = dir->children; c; c = c->sibling)
    materialise_dir(c);
}

void sysfs_reg_attach_root(struct vfs_node *sys_root) {
  if (!sys_root)
    return;
  g_root.node = sys_root;
  for (struct sysfs_dir *c = g_root.children; c; c = c->sibling)
    materialise_dir(c);
}

/* ── registration ───────────────────────────────────────────────── */

struct sysfs_dir *sysfs_reg_find(struct sysfs_dir *parent, const char *name) {
  if (!name)
    return 0;
  struct sysfs_dir *p = parent ? parent : &g_root;
  for (struct sysfs_dir *c = p->children; c; c = c->sibling) {
    if (strcmp(c->name, name) == 0)
      return c;
  }
  return 0;
}

struct sysfs_dir *sysfs_reg_dir(struct sysfs_dir *parent, const char *name) {
  if (!name || !*name)
    return 0;
  struct sysfs_dir *p = parent ? parent : &g_root;

  /* Naming an existing directory returns it. Two subsystems both publishing
   * under /sys/class must not end up with two /sys/class directories — which
   * is a mistake the network code made once already. */
  struct sysfs_dir *existing = sysfs_reg_find(p, name);
  if (existing)
    return existing;

  struct sysfs_dir *d = kzalloc(sizeof(*d));
  if (!d)
    return 0;
  copy_name(d->name, sizeof(d->name), name);
  d->parent = p;
  d->sibling = p->children;
  p->children = d;

  /* Already mounted: appear now rather than at the next mount. */
  if (p->node)
    materialise_dir(d);
  return d;
}

static int reg_attr(struct sysfs_dir *dir, const char *name, u16 mode,
                    sysfs_attr_show show, sysfs_attr_read_at read_at,
                    sysfs_attr_store store, void *ctx,
                    sysfs_attr_release release) {
  if (!dir || !name || !*name || (!show && !read_at))
    return -EINVAL;
  for (struct sysfs_attr *a = dir->attrs; a; a = a->next) {
    if (strcmp(a->name, name) == 0)
      return -EEXIST;
  }
  struct sysfs_attr *a = kzalloc(sizeof(*a));
  if (!a)
    return -ENOMEM;
  copy_name(a->name, sizeof(a->name), name);
  /* A file is writable only if there is something to write to. Advertising
   * 0644 with no store would offer userspace a write that goes nowhere. */
  a->mode = store ? (u16)(mode & 0666) : (u16)(mode & 0444);
  if (a->mode == 0)
    a->mode = store ? 0644 : 0444;
  a->show = show;
  a->read_at = read_at;
  a->store = store;
  a->release = release;
  a->ctx = ctx;
  a->next = dir->attrs;
  dir->attrs = a;

  materialise_attr(dir, a);
  return 0;
}

int sysfs_reg_attr(struct sysfs_dir *dir, const char *name, u16 mode,
                   sysfs_attr_show show, sysfs_attr_store store, void *ctx,
                   sysfs_attr_release release) {
  return reg_attr(dir, name, mode, show, 0, store, ctx, release);
}

int sysfs_reg_attr_at(struct sysfs_dir *dir, const char *name, u16 mode,
                      sysfs_attr_read_at read_at, sysfs_attr_store store,
                      void *ctx, sysfs_attr_release release) {
  return reg_attr(dir, name, mode, 0, read_at, store, ctx, release);
}

/* Unlink one attribute's file, if this registry created it, and release the
 * caller's context. Shared by the single-attribute removal and by tearing down
 * a whole directory. */
static void drop_attr(struct sysfs_dir *dir, struct sysfs_attr *a,
                      int unlink_node) {
  if (a->node && a->node->inode) {
    /* Drop the callbacks before the memory they read: a reader that arrives
     * between the release and the unlink would otherwise walk a dead
     * pointer. */
    a->node->inode->read_cb = 0;
    a->node->inode->write_cb = 0;
    a->node->inode->data = 0;
  }
  if (a->node && unlink_node && dir->node) {
    vfs_detach_child(dir->node, a->node);
    vfs_node_put(a->node);
  }
  if (a->release)
    a->release(a->ctx);
  kfree(a);
}

int sysfs_reg_attr_remove(struct sysfs_dir *dir, const char *name) {
  if (!dir || !name)
    return -EINVAL;
  struct sysfs_attr **link = &dir->attrs;
  while (*link && strcmp((*link)->name, name) != 0)
    link = &(*link)->next;
  if (!*link)
    return -ENOENT;
  struct sysfs_attr *a = *link;
  *link = a->next;
  drop_attr(dir, a, 1);
  return 0;
}

int sysfs_reg_link_remove(struct sysfs_dir *dir, const char *name) {
  if (!dir || !name)
    return -EINVAL;
  struct sysfs_link **link = &dir->links;
  while (*link && strcmp((*link)->name, name) != 0)
    link = &(*link)->next;
  if (!*link)
    return -ENOENT;
  struct sysfs_link *l = *link;
  *link = l->next;
  if (l->node && l->node->inode) {
    l->node->inode->data = 0;
    l->node->inode->size = 0;
  }
  if (l->node && dir->node) {
    vfs_detach_child(dir->node, l->node);
    vfs_node_put(l->node);
  }
  kfree(l->target);
  kfree(l);
  return 0;
}

int sysfs_reg_link(struct sysfs_dir *dir, const char *name,
                   const char *target) {
  if (!dir || !name || !*name || !target)
    return -EINVAL;
  struct sysfs_link *l = kzalloc(sizeof(*l));
  if (!l)
    return -ENOMEM;
  copy_name(l->name, sizeof(l->name), name);
  usize tl = strlen(target);
  l->target = kmalloc(tl + 1);
  if (!l->target) {
    kfree(l);
    return -ENOMEM;
  }
  memcpy(l->target, target, tl + 1);
  l->next = dir->links;
  dir->links = l;

  materialise_link(dir, l);
  return 0;
}

static void detach_dir(struct sysfs_dir *dir) {
  for (struct sysfs_dir *c = dir->children; c;) {
    struct sysfs_dir *next = c->sibling;
    detach_dir(c);
    c = next;
  }
  /* Under a borrowed directory the subtree is not going away with it, so each
   * file this registry added has to be unlinked on its own — otherwise a dead
   * attribute stays visible in /sys forever. Under one we created, the whole
   * subtree goes at the end and unlinking each child first would be wasted
   * work. */
  for (struct sysfs_attr *a = dir->attrs; a;) {
    struct sysfs_attr *next = a->next;
    drop_attr(dir, a, dir->adopted);
    a = next;
  }
  for (struct sysfs_link *l = dir->links; l;) {
    struct sysfs_link *next = l->next;
    if (l->node && l->node->inode) {
      l->node->inode->data = 0;
      l->node->inode->size = 0;
    }
    if (l->node && dir->adopted && dir->node) {
      vfs_detach_child(dir->node, l->node);
      vfs_node_put(l->node);
    }
    kfree(l->target);
    kfree(l);
    l = next;
  }
  /* Only unlink what this registry created. Detaching an adopted directory
   * would take the static tree's own subtree with it. */
  if (dir->node && !dir->adopted && dir->parent && dir->parent->node) {
    vfs_detach_child(dir->parent->node, dir->node);
    vfs_node_put(dir->node);
  }
  kfree(dir);
}

void sysfs_reg_remove(struct sysfs_dir *dir) {
  if (!dir)
    return;
  struct sysfs_dir *p = dir->parent ? dir->parent : &g_root;
  struct sysfs_dir **link = &p->children;
  while (*link && *link != dir)
    link = &(*link)->sibling;
  if (*link == dir)
    *link = dir->sibling;
  if (g_debug_root == dir)
    g_debug_root = 0;
  detach_dir(dir);
}

/* Build the path by walking up to the root and writing back to front: the
 * components are known in the wrong order, and reversing once at the end costs
 * less than a recursion per level. */
isize sysfs_reg_path(struct sysfs_dir *dir, char *buf, usize cap) {
  if (!dir || !buf || cap < 5)
    return -EINVAL;

  usize end = cap - 1;
  buf[end] = '\0';
  for (struct sysfs_dir *d = dir; d && d != &g_root; d = d->parent) {
    usize nl = strlen(d->name);
    if (end < nl + 1)
      return -ENAMETOOLONG;
    end -= nl;
    memcpy(buf + end, d->name, nl);
    buf[--end] = '/';
  }
  const char *root = "/sys";
  usize rl = strlen(root);
  if (end < rl)
    return -ENAMETOOLONG;
  end -= rl;
  memcpy(buf + end, root, rl);

  usize len = (cap - 1) - end;
  memmove(buf, buf + end, len + 1);
  return (isize)len;
}

/* ── self-test ──────────────────────────────────────────────────── */

/*
 * Read the files back through the VFS, by path, the way userspace does.
 *
 * Checking the registry's own bookkeeping would prove only that this file
 * agrees with itself. What has to be true is that an attribute registered by a
 * driver is a file that opens, reads the driver's current value, and — where it
 * has a store — takes a write and reflects it on the next read.
 */

static volatile u32 g_selftest_value = 7;

static isize selftest_show(void *ctx, char *buf, usize cap) {
  (void)ctx;
  return (isize)snprintf(buf, cap, "%u\n", (unsigned)g_selftest_value);
}

static isize selftest_store(void *ctx, const char *buf, usize len) {
  (void)ctx;
  u32 v = 0;
  usize i = 0;
  for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++)
    v = v * 10 + (u32)(buf[i] - '0');
  if (i == 0)
    return -EINVAL; /* nothing numeric: refuse rather than store a zero */
  g_selftest_value = v;
  return (isize)len;
}

/* The shim side registers the probe device; this side listens. The test spans
 * both naming worlds on purpose, and they meet through these two calls rather
 * than through one translation unit that includes both. */
int lkpi_selftest_uevent_device_add(void);
int lkpi_selftest_seq_file_create(void);
void lkpi_selftest_uevent_device_del(void);

static volatile u32 g_selftest_released;

static void selftest_release(void *ctx) {
  /* The registry hands back exactly the context that was registered; checking
   * that here is what proves the release is not called with something else. */
  if (ctx == (void *)&g_selftest_released)
    g_selftest_released++;
}

/* A value longer than one read, so a reader has to be told where it stopped. */
static isize selftest_read_at(void *ctx, char *buf, usize cap, u64 offset) {
  (void)ctx;
  usize n = 0;
  for (; n < cap; n++) {
    u64 pos = offset + n;
    if (pos >= 26)
      break;
    buf[n] = (char)('a' + (int)pos);
  }
  return (isize)n;
}

static void sysfs_report(const char *name, int ok, u64 detail) {
  console_write(ok ? "M101-SYSFS: ok " : "M101-SYSFS: FAIL ");
  console_write(name);
  console_write(" detail=");
  console_write_dec(detail);
  console_write("\n");
}

/* Read a whole file by path. Returns the length, or negative. */
static isize read_file(const char *path, char *buf, usize cap) {
  int fd = vfs_open(path);
  if (fd < 0)
    return -ENOENT;
  isize n = vfs_read(fd, buf, cap - 1);
  vfs_close(fd);
  if (n < 0)
    return n;
  buf[n] = '\0';
  return n;
}

void sysfs_attr_selftest(void) {
  char buf[128];

  /* The DRM core registered a class and a device through this registry; both
   * had to survive being registered before /sys was mounted. */
  isize n = read_file("/sys/class/drm/card0/dev", buf, sizeof(buf));
  /* DRM's major is 226, and this is the first minor. The value comes from the
   * device the driver registered, not from anything written here. */
  sysfs_report("card-dev", n > 0 && strcmp(buf, "226:0\n") == 0, (u64)(n > 0 ? n : 0));

  /* A file with a store: write, then read back through a fresh open, so the
   * value crosses the registry rather than a cached buffer. */
  struct sysfs_dir *d = sysfs_reg_dir(0, "b1nix-selftest");
  int reg = d ? sysfs_reg_attr(d, "value", 0644, selftest_show, selftest_store,
                               0, 0)
              : -ENOMEM;
  int rw_ok = 0;
  if (reg == 0) {
    n = read_file("/sys/b1nix-selftest/value", buf, sizeof(buf));
    int initial_ok = (n > 0 && strcmp(buf, "7\n") == 0);

    int fd = vfs_open_flags("/sys/b1nix-selftest/value", 1 /* O_WRONLY */);
    int wrote = 0;
    if (fd >= 0) {
      wrote = vfs_write(fd, "42", 2) == 2;
      vfs_close(fd);
    }
    n = read_file("/sys/b1nix-selftest/value", buf, sizeof(buf));
    rw_ok = initial_ok && wrote && n > 0 && strcmp(buf, "42\n") == 0;
  }
  sysfs_report("attr-store", rw_ok, g_selftest_value);

  /*
   * Removing one attribute must take that file and no other, and must release
   * the context the caller attached to it — a driver that unbinds does this
   * once per attribute it published, so a leak here is per-unbind.
   */
  int one_ok = 0;
  if (d) {
    g_selftest_released = 0;
    void *ctx = (void *)&g_selftest_released;
    int a = sysfs_reg_attr(d, "one", 0444, selftest_show, 0, ctx,
                           selftest_release);
    int b = sysfs_reg_attr(d, "two", 0444, selftest_show, 0, 0, 0);
    int removed = sysfs_reg_attr_remove(d, "one");
    int gone = read_file("/sys/b1nix-selftest/one", buf, sizeof(buf)) < 0;
    int sibling = read_file("/sys/b1nix-selftest/two", buf, sizeof(buf)) > 0;
    int absent = sysfs_reg_attr_remove(d, "one") == -ENOENT;
    one_ok = a == 0 && b == 0 && removed == 0 && gone && sibling && absent &&
             g_selftest_released == 1;
  }
  sysfs_report("attr-remove-one", one_ok, g_selftest_released);

  /*
   * The offset-aware read. The value is longer than a single read, so the only
   * way to see its tail is for the second read to be told where the first
   * stopped — which is exactly what a debugfs dump needs and what serving a
   * window of a re-rendered value cannot do.
   */
  int at_ok = 0;
  if (d && sysfs_reg_attr_at(d, "stream", 0444, selftest_read_at, 0, 0, 0) ==
                0) {
    int fd = vfs_open("/sys/b1nix-selftest/stream");
    if (fd >= 0) {
      char head[8];
      char tail[8];
      isize hn = vfs_read(fd, head, 4);
      isize tn = vfs_read(fd, tail, 4);
      vfs_close(fd);
      head[hn > 0 ? hn : 0] = '\0';
      tail[tn > 0 ? tn : 0] = '\0';
      /* The stream is "abcdefghij..." — the second read must continue, not
       * repeat the beginning. */
      at_ok = hn == 4 && tn == 4 && strcmp(head, "abcd") == 0 &&
              strcmp(tail, "efgh") == 0;
    }
  }
  sysfs_report("attr-read-at", at_ok, 0);

  if (d)
    sysfs_reg_remove(d);
  /* Removal must actually unlink it: a file that outlives its registration is
   * a dangling callback waiting to be called. */
  sysfs_report("attr-remove",
               read_file("/sys/b1nix-selftest/value", buf, sizeof(buf)) < 0, 0);

  /*
   * uevents. A listener binds NETLINK_KOBJECT_UEVENT with a group, the same
   * way mdev does, and then a real device registration has to reach it — the
   * announcement is raised by device_add(), not by anything here.
   */
  int nlfd = vfs_socket(B1NIX_AF_NETLINK, B1NIX_SOCK_DGRAM,
                        NETLINK_KOBJECT_UEVENT);
  int uevent_ok = 0;
  u64 devpath_ok = 0;
  if (nlfd >= 0) {
    struct b1nix_sockaddr_nl nl;
    memset(&nl, 0, sizeof(nl));
    nl.nl_family = B1NIX_AF_NETLINK;
    nl.nl_groups = 1;
    if (vfs_bind(nlfd, &nl, sizeof(nl)) == 0) {
      /* A real device registration, driven from the shim side: the
       * announcement comes out of device_add() itself, not from this test. */
      if (lkpi_selftest_uevent_device_add() == 0) {
        char ub[256];
        isize un = vfs_socket_recv(nlfd, ub, sizeof(ub) - 1, 0);
        if (un > 0) {
          ub[un] = '\0';
          /* The summary line, then the properties a helper reads. Each is
           * checked, because a message missing SUBSYSTEM or SEQNUM is one udev
           * drops and mdev mismatches. */
          int summary = strcmp(ub, "add@/class/b1nix-selftest/uevent0") == 0;
          int action = 0, subsystem = 0, seqnum = 0, devpath = 0;
          for (isize i = 0; i < un;) {
            const char *prop = ub + i;
            if (strcmp(prop, "ACTION=add") == 0)
              action = 1;
            else if (strcmp(prop, "SUBSYSTEM=b1nix-selftest") == 0)
              subsystem = 1;
            else if (strncmp(prop, "SEQNUM=", 7) == 0)
              seqnum = 1;
            else if (strcmp(prop, "DEVPATH=/class/b1nix-selftest/uevent0") == 0)
              devpath = 1;
            i += (isize)strlen(prop) + 1;
          }
          uevent_ok = summary && action && subsystem && seqnum && devpath;
          devpath_ok = (u64)un;
        }
        lkpi_selftest_uevent_device_del();
      }
    }
    vfs_close(nlfd);
  }
  sysfs_report("uevent-delivered", uevent_ok, devpath_ok);

  /*
   * A seq_file-backed debugfs file, the shape a driver actually writes: its
   * open() calls single_open() and its reads go through seq_read(). The file
   * prints far more than one read holds, so this is where a read that always
   * restarted at zero used to lose everything past the first buffer.
   */
  int seq_ok = 0;
  if (lkpi_selftest_seq_file_create() == 0) {
    int fd = vfs_open("/sys/kernel/debug/b1nix-seq/lines");
    if (fd >= 0) {
      char head[16];
      isize hn = vfs_read(fd, head, 7);
      /* Read to the end, counting: the file is ~1.6 KiB, so this only
       * terminates if later reads continue rather than repeat. */
      usize total = (hn > 0) ? (usize)hn : 0;
      char chunk[128];
      char tail[32];
      isize last = 0;
      for (int i = 0; i < 200; i++) {
        isize cn = vfs_read(fd, chunk, sizeof(chunk));
        if (cn <= 0)
          break;
        total += (usize)cn;
        last = cn;
        if ((usize)cn < sizeof(chunk))
          memcpy(tail, chunk, (usize)cn);
      }
      vfs_close(fd);
      head[hn > 0 ? hn : 0] = '\0';
      /* 200 lines of "line N\n": 10 one-digit, 90 two-digit, 100 three-digit. */
      usize expect = 10 * 7 + 90 * 8 + 100 * 9;
      seq_ok = hn == 7 && strcmp(head, "line 0\n") == 0 && total == expect &&
               last > 0;
    }
  }
  sysfs_report("debugfs-seq-file", seq_ok, 0);

  /* debugfs: the DRM core creates /sys/kernel/debug/dri from its own initcall,
   * so this is upstream's call reaching our registry rather than ours. */
  int fd = vfs_open("/sys/kernel/debug/dri");
  sysfs_report("debugfs-dri", fd >= 0, 0);
  if (fd >= 0)
    vfs_close(fd);

  console_write("M101-SYSFS: done\n");
}

struct sysfs_dir *sysfs_reg_parent(struct sysfs_dir *dir) {
  if (!dir || !dir->parent || dir->parent == &g_root)
    return 0;
  return dir->parent;
}

struct sysfs_dir *sysfs_reg_debug_root(void) {
  if (!g_debug_root) {
    struct sysfs_dir *kern = sysfs_reg_dir(0, "kernel");
    g_debug_root = kern ? sysfs_reg_dir(kern, "debug") : 0;
  }
  return g_debug_root;
}
