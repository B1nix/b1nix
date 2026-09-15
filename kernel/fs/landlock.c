/*
 * Landlock: unprivileged filesystem sandboxing (ABI version 3).
 *
 * A process builds a ruleset -- which access rights it wants restricted, and
 * for each directory hierarchy which of them stay allowed -- and then restricts
 * itself with it. Restrictions stack (up to 16 layers), survive fork and exec,
 * and can never be lifted. An access is allowed only if every layer that
 * handles it grants it through a rule whose hierarchy contains the file.
 *
 * Hierarchies are compared as canonical paths: a rule records the absolute
 * path of its directory with symlinks resolved, and a check resolves the file
 * (or, for a creation or removal, its parent directory) the same way, so a
 * symlink cannot lead out of a granted hierarchy or into one.
 *
 * Network rules (ABI 4) and scoping (ABI 6) are not implemented; a ruleset
 * that asks for them is refused as on a kernel whose ABI is 3.
 */
#include <b1nix/errno.h>
#include <b1nix/landlock.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/vfs.h>
#include <string.h>

#define LANDLOCK_ABI_VERSION 3
#define LANDLOCK_MAX_LAYERS  16
#define LANDLOCK_MAX_RULES   1024
#define LANDLOCK_CREATE_RULESET_VERSION (1u << 0)
#define LANDLOCK_RULE_PATH_BENEATH 1

#define LL_ACCESS_FS_ALL 0x7fffULL /* EXECUTE .. TRUNCATE */
/* Rights that make sense on a file (the rest need a directory). */
#define LL_ACCESS_FILE (LL_EXECUTE | LL_WRITE_FILE | LL_READ_FILE | LL_TRUNCATE)

struct ll_rule {
  char *path;
  u64 access;
};

struct ll_ruleset {
  volatile int refs;
  u64 handled_fs;
  u32 nrules;
  struct ll_rule rules[LANDLOCK_MAX_RULES];
};

/* A task's restriction: the newest layer and the chain beneath it. */
struct ll_domain {
  volatile int refs;
  struct ll_domain *parent;
  u32 depth;
  struct ll_ruleset *layer;
};

static struct ll_domain **g_task_domain;
static usize g_domain_rows;
static spinlock_t g_ll_lock = SPINLOCK_INIT;

static void ruleset_put(struct ll_ruleset *rs) {
  if (rs && __atomic_sub_fetch(&rs->refs, 1, __ATOMIC_ACQ_REL) == 0) {
    for (u32 i = 0; i < rs->nrules; i++)
      kfree(rs->rules[i].path);
    kfree(rs);
  }
}

static void domain_put(struct ll_domain *d) {
  while (d && __atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL) == 0) {
    struct ll_domain *p = d->parent;

    ruleset_put(d->layer);
    kfree(d);
    d = p;
  }
}

static struct ll_domain *domain_get_task(usize row) {
  struct ll_domain *d = 0;
  u64 f;

  if (!g_task_domain || row >= g_domain_rows)
    return 0;
  spin_lock_irqsave(&g_ll_lock, &f);
  d = g_task_domain[row];
  if (d)
    __atomic_add_fetch(&d->refs, 1, __ATOMIC_ACQ_REL);
  spin_unlock_irqrestore(&g_ll_lock, f);
  return d;
}

static struct ll_domain *domain_current(void) {
  if (!current_task || !g_task_domain)
    return 0;
  return domain_get_task(scheduler_task_index(current_task));
}

void landlock_fork(usize parent_row, usize child_row) {
  struct ll_domain *d = domain_get_task(parent_row);
  u64 f;

  if (!g_task_domain || child_row >= g_domain_rows) {
    domain_put(d);
    return;
  }
  spin_lock_irqsave(&g_ll_lock, &f);
  struct ll_domain *old = g_task_domain[child_row];
  g_task_domain[child_row] = d;
  spin_unlock_irqrestore(&g_ll_lock, f);
  domain_put(old);
}

void landlock_task_reset(usize row) {
  u64 f;

  if (!g_task_domain || row >= g_domain_rows)
    return;
  spin_lock_irqsave(&g_ll_lock, &f);
  struct ll_domain *old = g_task_domain[row];
  g_task_domain[row] = 0;
  spin_unlock_irqrestore(&g_ll_lock, f);
  domain_put(old);
}

/* ── canonical paths ─────────────────────────────────────────────── */

/* The canonical path of an existing file, symlinks followed. */
static int ll_canon_existing(const char *path, char *out, usize cap) {
  struct vfs_node *n = vfs_find_node(path);

  if (IS_ERR(n))
    return (int)PTR_ERR(n);
  int rc = vfs_get_node_path(n, out, cap);
  vfs_node_put(n);
  return rc < 0 ? rc : 0;
}

/* The canonical path of the directory a name is created in or removed from. */
static int ll_canon_parent(const char *path, char *out, usize cap) {
  char r[VFS_MAX_PATH];
  vfs_resolve_path(path, r);
  usize l = strlen(r);

  while (l > 1 && r[l - 1] == '/')
    r[--l] = '\0';
  char *slash = strrchr(r, '/');
  if (!slash)
    return -ENOENT;
  if (slash == r)
    r[1] = '\0';
  else
    *slash = '\0';
  return ll_canon_existing(r, out, cap);
}

static int ll_beneath(const char *path, const char *root) {
  usize rl = strlen(root);

  if (rl == 1 && root[0] == '/')
    return 1;
  return strncmp(path, root, rl) == 0 && (path[rl] == '\0' || path[rl] == '/');
}

/* 0 when every layer grants `access` on canonical path `canon`. */
static int ll_check_canon(struct ll_domain *d, const char *canon, u64 access) {
  for (; d; d = d->parent) {
    struct ll_ruleset *rs = d->layer;
    u64 need = access & rs->handled_fs;
    u64 granted = 0;

    if (!need)
      continue;
    for (u32 i = 0; i < rs->nrules; i++)
      if (ll_beneath(canon, rs->rules[i].path))
        granted |= rs->rules[i].access;
    if (need & ~granted)
      return -EACCES;
  }
  return 0;
}

int landlock_check_path(const char *path, u64 access) {
  struct ll_domain *d = domain_current();
  char canon[VFS_MAX_PATH];
  int rc;

  if (!d)
    return 0;
  rc = ll_canon_existing(path, canon, sizeof(canon));
  if (rc == 0)
    rc = ll_check_canon(d, canon, access);
  else if (rc == -ENOENT)
    rc = 0; /* nothing there: the operation fails on its own */
  domain_put(d);
  return rc;
}

int landlock_check_parent(const char *path, u64 access) {
  struct ll_domain *d = domain_current();
  char canon[VFS_MAX_PATH];
  int rc;

  if (!d)
    return 0;
  rc = ll_canon_parent(path, canon, sizeof(canon));
  if (rc == 0)
    rc = ll_check_canon(d, canon, access);
  else if (rc == -ENOENT)
    rc = 0;
  domain_put(d);
  return rc;
}

/* open(2): the rights an open asks for, on the file itself -- or, for a file
 * the open creates, on the directory it is created in (the new file lies
 * beneath it, so the same rules decide). */
int landlock_check_open(const char *path, int flags) {
  struct ll_domain *d;
  char canon[VFS_MAX_PATH];
  u64 access = 0;
  int acc = flags & 3;
  int rc;

  if (!g_task_domain || (flags & B1NIX_O_PATH))
    return 0;
  d = domain_current();
  if (!d)
    return 0;
  if (acc == B1NIX_O_WRONLY || acc == B1NIX_O_RDWR)
    access |= LL_WRITE_FILE;
  if (flags & B1NIX_O_TRUNC)
    access |= LL_TRUNCATE;
  struct vfs_node *n = vfs_find_node(path);
  if (!IS_ERR(n)) {
    int is_dir = n->inode && n->inode->type == VFS_DIRECTORY;

    if (acc == B1NIX_O_RDONLY || acc == B1NIX_O_RDWR)
      access |= is_dir ? LL_READ_DIR : LL_READ_FILE;
    rc = vfs_get_node_path(n, canon, sizeof(canon));
    vfs_node_put(n);
    rc = rc < 0 ? rc : (access ? ll_check_canon(d, canon, access) : 0);
  } else if (flags & B1NIX_O_CREAT) {
    if (acc == B1NIX_O_RDONLY || acc == B1NIX_O_RDWR)
      access |= LL_READ_FILE;
    rc = ll_canon_parent(path, canon, sizeof(canon));
    rc = rc < 0 ? (rc == -ENOENT ? 0 : rc)
                : ll_check_canon(d, canon, access | LL_MAKE_REG);
  } else {
    rc = 0; /* no such file: the open fails on its own */
  }
  domain_put(d);
  return rc;
}

/* rename(2)/link(2): a move between directories needs REFER on both, on top of
 * the removal and creation rights; without REFER in a layer that handles the
 * filesystem the answer is EXDEV, as Linux gives, so the caller copies. */
int landlock_check_move(const char *old_path, const char *new_path, int is_dir,
                        int is_link) {
  struct ll_domain *d = domain_current();
  char op[VFS_MAX_PATH], np[VFS_MAX_PATH];
  u64 make = is_dir ? LL_MAKE_DIR : LL_MAKE_REG;
  int rc = 0;

  if (!d)
    return 0;
  if (ll_canon_parent(old_path, op, sizeof(op)) ||
      ll_canon_parent(new_path, np, sizeof(np)))
    goto out;
  if (!is_link && (rc = ll_check_canon(d, op, is_dir ? LL_REMOVE_DIR
                                                      : LL_REMOVE_FILE)))
    goto out;
  if ((rc = ll_check_canon(d, np, make)))
    goto out;
  if (strcmp(op, np) && (ll_check_canon(d, op, LL_REFER) ||
                         ll_check_canon(d, np, LL_REFER)))
    rc = -EXDEV;
out:
  domain_put(d);
  return rc;
}

/* ── the system calls ────────────────────────────────────────────── */

static void ruleset_release(struct vfs_handle *h) {
  ruleset_put(h->private_data);
  h->private_data = 0;
}

static const struct vfs_file_ops ruleset_ops = {
    .release = ruleset_release,
};

static struct ll_ruleset *ruleset_from_fd(int fd, isize *err) {
  struct vfs_handle *h = scheduler_fd_get(fd);

  if (!h) {
    *err = -EBADF;
    return 0;
  }
  if (h->kind != VFS_HANDLE_LANDLOCK || !h->private_data) {
    *err = -EBADFD;
    return 0;
  }
  return h->private_data;
}

static isize ll_create_ruleset(u64 uattr, u64 size, u64 flags) {
  u64 attr[3] = {0, 0, 0};

  if (flags == LANDLOCK_CREATE_RULESET_VERSION) {
    if (uattr || size)
      return -EINVAL;
    return LANDLOCK_ABI_VERSION;
  }
  if (flags)
    return -EINVAL;
  if (!uattr)
    return -EFAULT;
  if (size < 8)
    return -EINVAL;
  if (size > PAGE_SIZE)
    return -E2BIG;
  if (syscall_copyin(attr, (const void *)(usize)uattr,
                     size < sizeof(attr) ? (usize)size : sizeof(attr)))
    return -EFAULT;
  for (u64 off = sizeof(attr); off < size; off++) {
    u8 b = 0;

    if (syscall_copyin(&b, (const void *)(usize)(uattr + off), 1))
      return -EFAULT;
    if (b)
      return -E2BIG;
  }
  if (attr[0] & ~LL_ACCESS_FS_ALL)
    return -EINVAL;
  if (attr[1] || attr[2])
    return -EINVAL; /* network rules and scopes: ABI 4 and 6 */
  if (!attr[0])
    return -ENOMSG;

  if (!g_task_domain) {
    usize n = scheduler_max_task_slots();
    struct ll_domain **t = kzalloc(n * sizeof(*t));

    if (!t)
      return -ENOMEM;
    u64 f;
    spin_lock_irqsave(&g_ll_lock, &f);
    if (!g_task_domain) {
      g_task_domain = t;
      g_domain_rows = n;
      t = 0;
    }
    spin_unlock_irqrestore(&g_ll_lock, f);
    if (t)
      kfree(t);
  }

  struct ll_ruleset *rs = kzalloc(sizeof(*rs));
  if (!rs)
    return -ENOMEM;
  rs->refs = 1;
  rs->handled_fs = attr[0];
  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_LANDLOCK);
  if (!h) {
    kfree(rs);
    return -ENFILE;
  }
  h->private_data = rs;
  h->ops = &ruleset_ops;
  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h);
    return fd;
  }
  scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

static isize ll_add_rule(u64 rfd, u64 type, u64 uattr, u64 flags) {
  struct {
    u64 allowed_access;
    i32 parent_fd;
  } __attribute__((packed)) pb;
  isize err;
  char canon[VFS_MAX_PATH];

  if (flags)
    return -EINVAL;
  struct ll_ruleset *rs = ruleset_from_fd((int)rfd, &err);
  if (!rs)
    return err;
  if (type != LANDLOCK_RULE_PATH_BENEATH)
    return -EINVAL;
  if (syscall_copyin(&pb, (const void *)(usize)uattr, sizeof(pb)))
    return -EFAULT;
  if (!pb.allowed_access)
    return -ENOMSG;
  if (pb.allowed_access & ~rs->handled_fs)
    return -EINVAL;
  struct vfs_node *n = vfs_find_node_by_fd(pb.parent_fd);
  if (IS_ERR(n))
    return -EBADF;
  int is_dir = n->inode && n->inode->type == VFS_DIRECTORY;
  int rc = vfs_get_node_path(n, canon, sizeof(canon)); /* n is borrowed */
  if (rc < 0)
    return rc;
  if (!is_dir && (pb.allowed_access & ~LL_ACCESS_FILE))
    return -EINVAL;
  u64 f;
  spin_lock_irqsave(&g_ll_lock, &f);
  if (rs->nrules >= LANDLOCK_MAX_RULES) {
    spin_unlock_irqrestore(&g_ll_lock, f);
    return -E2BIG;
  }
  usize l = strlen(canon);
  char *p = kmalloc(l + 1);
  if (!p) {
    spin_unlock_irqrestore(&g_ll_lock, f);
    return -ENOMEM;
  }
  memcpy(p, canon, l + 1);
  rs->rules[rs->nrules].path = p;
  rs->rules[rs->nrules].access = pb.allowed_access;
  rs->nrules++;
  spin_unlock_irqrestore(&g_ll_lock, f);
  return 0;
}

static isize ll_restrict_self(u64 rfd, u64 flags) {
  isize err;
  struct cred *c = scheduler_get_current_cred();

  if (flags)
    return -EINVAL;
  /* Unprivileged only once no_new_privs promises exec cannot undo it. */
  if (!task_no_new_privs(current_task) && !(c && cred_has_cap(c, CAP_SYS_ADMIN)))
    return -EPERM;
  struct ll_ruleset *src = ruleset_from_fd((int)rfd, &err);
  if (!src)
    return err;
  struct ll_domain *cur = domain_current();
  if (cur && cur->depth >= LANDLOCK_MAX_LAYERS) {
    domain_put(cur);
    return -E2BIG;
  }
  /* The layer is a snapshot: rules added to the ruleset afterwards do not
   * loosen a restriction already in force. */
  struct ll_ruleset *snap = kzalloc(sizeof(*snap));
  struct ll_domain *d = kzalloc(sizeof(*d));
  if (!snap || !d) {
    if (snap)
      kfree(snap);
    if (d)
      kfree(d);
    domain_put(cur);
    return -ENOMEM;
  }
  u64 f;
  spin_lock_irqsave(&g_ll_lock, &f);
  snap->refs = 1;
  snap->handled_fs = src->handled_fs;
  for (u32 i = 0; i < src->nrules; i++) {
    usize l = strlen(src->rules[i].path);
    char *p = kmalloc(l + 1);

    if (!p)
      break;
    memcpy(p, src->rules[i].path, l + 1);
    snap->rules[snap->nrules].path = p;
    snap->rules[snap->nrules].access = src->rules[i].access;
    snap->nrules++;
  }
  d->refs = 1;
  d->layer = snap;
  d->parent = cur; /* our reference on cur moves into the chain */
  d->depth = cur ? cur->depth + 1 : 1;
  usize row = scheduler_task_index(current_task);
  struct ll_domain *old = g_task_domain[row];
  g_task_domain[row] = d;
  spin_unlock_irqrestore(&g_ll_lock, f);
  domain_put(old);
  return 0;
}

#define LL_NR_create_ruleset 444
#define LL_NR_add_rule       445
#define LL_NR_restrict_self  446

int landlock_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 *ret) {
  isize r;

  switch (nr) {
  case LL_NR_create_ruleset:
    r = ll_create_ruleset(a0, a1, a2);
    break;
  case LL_NR_add_rule:
    r = ll_add_rule(a0, a1, a2, a3);
    break;
  case LL_NR_restrict_self:
    r = ll_restrict_self(a0, a1);
    break;
  default:
    return 0;
  }
  *ret = (u64)r;
  return 1;
}
