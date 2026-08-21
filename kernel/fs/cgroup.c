/*
 * cgroup v2 — the unified hierarchy.
 *
 * systemd is the reason this exists. It mounts "cgroup2" on /sys/fs/cgroup
 * before it starts a single unit, makes one directory per unit, and moves the
 * unit's processes into it by writing their pid to that directory's
 * cgroup.procs. Everything it later does with a unit's processes — listing
 * them, learning that the last one has gone, killing what is left — is a read
 * or a write of a file in this tree. Without the filesystem, PID 1 falls back
 * to the v1 hierarchy, fails to mount that either, and freezes.
 *
 * Shape
 * -----
 * A mount is a directory tree in the VFS's own in-memory node graph, the same
 * way tmpfs is. Each directory IS a cgroup and carries the control files that
 * describe it; mkdir(2) creates a cgroup and rmdir(2) destroys one, which is
 * the whole of the cgroup v2 creation API.
 *
 * Membership is a side table keyed by task id (struct task must not grow — see
 * kernel/sched/namespace.c for the same constraint). "No entry" means the root
 * cgroup, so a machine that never mounts cgroup2 allocates nothing and every
 * task reports "0::/", which is what a kernel with the hierarchy unmounted
 * reports too.
 *
 * Controllers
 * -----------
 * One: `pids`, and it is enforced — a fork that would exceed a pids.max
 * anywhere between the new task's cgroup and the root fails with EAGAIN, which
 * is what Linux's pids controller returns. `memory`, `cpu` and `io` are NOT
 * advertised, because advertising a controller means accepting writes to
 * memory.max or cpu.weight, and accepting a limit that nothing enforces is a
 * lie told to the process that set it. cgroup.controllers therefore lists what
 * this kernel can actually do, and systemd logs the rest as unavailable — the
 * same thing it does on a Linux kernel built without those controllers.
 */

#include <b1nix/cgroup.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/inotify.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <string.h>

/* ── controllers ─────────────────────────────────────────────────────────── */

#define CG_CTRL_PIDS 0x1u

static const struct {
  const char *name;
  u32 bit;
} cg_controllers[] = {
    {"pids", CG_CTRL_PIDS},
};

#define CG_PIDS_MAX_UNSET 0xFFFFFFFFu

const char *cgroup_available_controllers(void) { return "pids"; }

/* ── the tree ────────────────────────────────────────────────────────────── */

struct cgroup {
  struct cgroup *parent;
  struct cgroup *next; /* global list, for descendant walks */
  struct vfs_node *dir;
  struct vfs_node *events_node; /* cgroup.events, for the inotify edge */
  u32 subtree_control;
  u32 pids_max;
  u64 pids_denied;      /* pids.events: max */
  u32 max_depth;        /* cgroup.max.depth, CG_PIDS_MAX_UNSET = "max" */
  u32 max_descendants;  /* cgroup.max.descendants */
  int populated;        /* last value published in cgroup.events */
  int scratch;          /* cg_events_refresh's single-pass accumulator */
  int is_root;
};

static struct cgroup *cg_root;
static struct cgroup *cg_all; /* singly linked list of every live cgroup */
static spinlock_t cg_lock = SPINLOCK_INIT;

/* ── membership ──────────────────────────────────────────────────────────── */

/* One entry per task that is NOT in the root cgroup. 4096 is the kernel's own
 * task ceiling (TASK_CHUNK_SIZE * TASK_MAX_CHUNKS), so the table can always
 * hold every task that exists; the hash keeps the common lookup to one probe.
 * Deletion shifts the probe chain back rather than leaving a tombstone, so a
 * long boot cannot fill the table with debris. */
#define CG_SLOTS 8192u

struct cg_member {
  usize pid; /* 0 = free */
  struct cgroup *cg;
};

static struct cg_member cg_members[CG_SLOTS];

static inline u32 cg_hash(usize pid) {
  u64 h = (u64)pid * 0x9E3779B97F4A7C15ull;
  return (u32)((h >> 32) & (CG_SLOTS - 1));
}

/* Caller holds cg_lock. */
static struct cgroup *cg_member_get(usize pid) {
  u32 i = cg_hash(pid);
  for (u32 n = 0; n < CG_SLOTS; n++) {
    u32 s = (i + n) & (CG_SLOTS - 1);
    if (cg_members[s].pid == 0)
      return 0;
    if (cg_members[s].pid == pid)
      return cg_members[s].cg;
  }
  return 0;
}

/* Caller holds cg_lock. cg == NULL removes the entry (back to the root). */
static void cg_member_set(usize pid, struct cgroup *cg) {
  u32 i = cg_hash(pid);
  u32 found = CG_SLOTS;
  for (u32 n = 0; n < CG_SLOTS; n++) {
    u32 s = (i + n) & (CG_SLOTS - 1);
    if (cg_members[s].pid == pid) {
      found = s;
      break;
    }
    if (cg_members[s].pid == 0)
      break;
  }

  if (!cg) {
    if (found == CG_SLOTS)
      return;
    /* Backward-shift deletion: close the hole by pulling forward any entry
     * whose ideal slot is at or before it, so no probe chain is broken. */
    u32 hole = found;
    cg_members[hole].pid = 0;
    cg_members[hole].cg = 0;
    for (u32 n = 1; n < CG_SLOTS; n++) {
      u32 s = (hole + n) & (CG_SLOTS - 1);
      if (cg_members[s].pid == 0)
        break;
      u32 ideal = cg_hash(cg_members[s].pid);
      /* Is `ideal` cyclically within (hole, s]? If not, the entry may move. */
      u32 d_hole = (s - hole) & (CG_SLOTS - 1);
      u32 d_ideal = (s - ideal) & (CG_SLOTS - 1);
      if (d_ideal >= d_hole) {
        cg_members[hole] = cg_members[s];
        cg_members[s].pid = 0;
        cg_members[s].cg = 0;
        hole = s;
      }
    }
    return;
  }

  if (found != CG_SLOTS) {
    cg_members[found].cg = cg;
    return;
  }
  for (u32 n = 0; n < CG_SLOTS; n++) {
    u32 s = (i + n) & (CG_SLOTS - 1);
    if (cg_members[s].pid == 0) {
      cg_members[s].pid = pid;
      cg_members[s].cg = cg;
      return;
    }
  }
}

/* The cgroup a task belongs to, never NULL once a hierarchy is mounted. */
static struct cgroup *cg_of(usize pid) {
  struct cgroup *cg = cg_member_get(pid);
  return cg ? cg : cg_root;
}

static int cg_is_ancestor(const struct cgroup *anc, const struct cgroup *cg) {
  for (const struct cgroup *c = cg; c; c = c->parent)
    if (c == anc)
      return 1;
  return 0;
}

/* ── task counting ───────────────────────────────────────────────────────── */

static int cg_task_live(const struct task *t) {
  return t && t->id && t->state != TASK_UNUSED && t->state != TASK_DEAD &&
         t->state != TASK_REAPING;
}

/* Tasks in `cg` and, when `recurse`, everything below it. Caller holds
 * cg_lock. */
static usize cg_count_tasks(struct cgroup *cg, int recurse) {
  usize n = 0;
  usize slots = scheduler_task_slots();
  for (usize i = 0; i < slots; i++) {
    struct task *t = scheduler_task_slot(i);
    if (!cg_task_live(t))
      continue;
    struct cgroup *tc = cg_of(t->id);
    if (tc == cg || (recurse && cg_is_ancestor(cg, tc)))
      n++;
  }
  return n;
}

static usize cg_count_descendants(struct cgroup *cg) {
  usize n = 0;
  for (struct cgroup *c = cg_all; c; c = c->next)
    if (c != cg && cg_is_ancestor(cg, c))
      n++;
  return n;
}

static u32 cg_depth(const struct cgroup *cg) {
  u32 d = 0;
  for (const struct cgroup *c = cg; c && c->parent; c = c->parent)
    d++;
  return d;
}

/* ── cgroup.events: the edge systemd waits on ────────────────────────────── */

/* systemd watches cgroup.events with inotify to learn that a unit's last
 * process has exited. Recompute every cgroup's "populated" and report the ones
 * that changed; without the notification a service that has finished stays
 * "deactivating" until its stop timeout fires. */
static void cg_events_refresh(void) {
  struct vfs_node *changed[32];
  int nchanged = 0;

  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  /* One pass over the task table, not one per cgroup: this runs on every fork
   * and every exit, with interrupts off. */
  for (struct cgroup *c = cg_all; c; c = c->next)
    c->scratch = 0;
  usize slots = scheduler_task_slots();
  for (usize i = 0; i < slots; i++) {
    struct task *t = scheduler_task_slot(i);
    if (!cg_task_live(t))
      continue;
    for (struct cgroup *c = cg_of(t->id); c; c = c->parent)
      c->scratch = 1;
  }
  for (struct cgroup *c = cg_all; c; c = c->next) {
    if (c->is_root)
      continue;
    if (c->scratch != c->populated) {
      c->populated = c->scratch;
      if (c->events_node && nchanged < (int)(sizeof(changed) / sizeof(changed[0])))
        changed[nchanged++] = c->events_node;
    }
  }
  spin_unlock_irqrestore(&cg_lock, flags);

  for (int i = 0; i < nchanged; i++)
    vfs_inotify_notify(changed[i], IN_MODIFY, 0);
}

/* ── control files ───────────────────────────────────────────────────────── */

enum cg_file {
  CGF_PROCS = 1,
  CGF_THREADS,
  CGF_CONTROLLERS,
  CGF_SUBTREE_CONTROL,
  CGF_EVENTS,
  CGF_TYPE,
  CGF_STAT,
  CGF_MAX_DEPTH,
  CGF_MAX_DESCENDANTS,
  CGF_PIDS_CURRENT,
  CGF_PIDS_MAX,
  CGF_PIDS_EVENTS,
  CGF_PIDS_PEAK,
};

struct cg_filenode {
  struct cgroup *cg;
  enum cg_file kind;
};

/* Copy the slice of `src` that a read at `offset` asked for. */
static isize cg_emit(const char *src, usize len, u64 offset, char *out,
                     usize size) {
  if (offset >= len)
    return 0;
  usize avail = len - (usize)offset;
  usize n = size < avail ? size : avail;
  memcpy(out, src + offset, n);
  return (isize)n;
}

static usize cg_append(char *buf, usize cap, usize len, const char *s) {
  while (*s && len + 1 < cap)
    buf[len++] = *s++;
  if (len < cap)
    buf[len] = '\0';
  return len;
}

static usize cg_append_u64(char *buf, usize cap, usize len, u64 v) {
  char tmp[24];
  int n = 0;
  if (v == 0)
    tmp[n++] = '0';
  while (v) {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while (n > 0 && len + 1 < cap)
    buf[len++] = tmp[--n];
  if (len < cap)
    buf[len] = '\0';
  return len;
}

/* The absolute v2 path of a cgroup, "/" for the root. */
static int cg_path(struct cgroup *cg, char *buf, usize len) {
  const char *parts[32];
  int n = 0;
  for (struct cgroup *c = cg; c && !c->is_root && n < 32; c = c->parent)
    parts[n++] = c->dir ? c->dir->name : "?";
  if (n == 0) {
    if (len < 2)
      return -ENAMETOOLONG;
    buf[0] = '/';
    buf[1] = '\0';
    return 1;
  }
  usize pos = 0;
  for (int i = n - 1; i >= 0; i--) {
    usize pl = strlen(parts[i]);
    if (pos + 1 + pl + 1 > len)
      return -ENAMETOOLONG;
    buf[pos++] = '/';
    memcpy(buf + pos, parts[i], pl);
    pos += pl;
  }
  buf[pos] = '\0';
  return (int)pos;
}

/* Render one control file into `buf`; returns the length written. */
static usize cg_render(struct cgroup *cg, enum cg_file kind, char *buf,
                       usize cap) {
  usize len = 0;
  u64 flags;

  switch (kind) {
  case CGF_PROCS:
  case CGF_THREADS: {
    spin_lock_irqsave(&cg_lock, &flags);
    usize slots = scheduler_task_slots();
    for (usize i = 0; i < slots; i++) {
      struct task *t = scheduler_task_slot(i);
      if (!cg_task_live(t))
        continue;
      if (cg_of(t->id) != cg)
        continue;
      /* cgroup.procs lists processes, cgroup.threads lists every thread. */
      if (kind == CGF_PROCS && task_tgid(t) != t->id)
        continue;
      len = cg_append_u64(buf, cap, len, (u64)t->id);
      len = cg_append(buf, cap, len, "\n");
    }
    spin_unlock_irqrestore(&cg_lock, flags);
    break;
  }
  case CGF_CONTROLLERS: {
    /* A cgroup's available controllers are the ones its parent enabled for its
     * children; the root's are everything the kernel implements. */
    spin_lock_irqsave(&cg_lock, &flags);
    u32 avail = cg->is_root ? CG_CTRL_PIDS
                            : (cg->parent ? cg->parent->subtree_control : 0);
    spin_unlock_irqrestore(&cg_lock, flags);
    int first = 1;
    for (usize i = 0; i < sizeof(cg_controllers) / sizeof(cg_controllers[0]); i++)
      if (avail & cg_controllers[i].bit) {
        if (!first)
          len = cg_append(buf, cap, len, " ");
        len = cg_append(buf, cap, len, cg_controllers[i].name);
        first = 0;
      }
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_SUBTREE_CONTROL: {
    spin_lock_irqsave(&cg_lock, &flags);
    u32 en = cg->subtree_control;
    spin_unlock_irqrestore(&cg_lock, flags);
    int first = 1;
    for (usize i = 0; i < sizeof(cg_controllers) / sizeof(cg_controllers[0]); i++)
      if (en & cg_controllers[i].bit) {
        if (!first)
          len = cg_append(buf, cap, len, " ");
        len = cg_append(buf, cap, len, cg_controllers[i].name);
        first = 0;
      }
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_EVENTS: {
    spin_lock_irqsave(&cg_lock, &flags);
    int pop = cg_count_tasks(cg, 1) > 0;
    cg->populated = pop;
    spin_unlock_irqrestore(&cg_lock, flags);
    len = cg_append(buf, cap, len, "populated ");
    len = cg_append_u64(buf, cap, len, (u64)pop);
    len = cg_append(buf, cap, len, "\nfrozen 0\n");
    break;
  }
  case CGF_TYPE:
    len = cg_append(buf, cap, len, "domain\n");
    break;
  case CGF_STAT: {
    spin_lock_irqsave(&cg_lock, &flags);
    usize nd = cg_count_descendants(cg);
    spin_unlock_irqrestore(&cg_lock, flags);
    len = cg_append(buf, cap, len, "nr_descendants ");
    len = cg_append_u64(buf, cap, len, (u64)nd);
    len = cg_append(buf, cap, len, "\nnr_dying_descendants 0\n");
    break;
  }
  case CGF_MAX_DEPTH:
  case CGF_MAX_DESCENDANTS: {
    spin_lock_irqsave(&cg_lock, &flags);
    u32 v = (kind == CGF_MAX_DEPTH) ? cg->max_depth : cg->max_descendants;
    spin_unlock_irqrestore(&cg_lock, flags);
    if (v == CG_PIDS_MAX_UNSET)
      len = cg_append(buf, cap, len, "max\n");
    else {
      len = cg_append_u64(buf, cap, len, (u64)v);
      len = cg_append(buf, cap, len, "\n");
    }
    break;
  }
  case CGF_PIDS_CURRENT:
  case CGF_PIDS_PEAK: {
    spin_lock_irqsave(&cg_lock, &flags);
    usize n = cg_count_tasks(cg, 1);
    spin_unlock_irqrestore(&cg_lock, flags);
    len = cg_append_u64(buf, cap, len, (u64)n);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  case CGF_PIDS_MAX: {
    spin_lock_irqsave(&cg_lock, &flags);
    u32 v = cg->pids_max;
    spin_unlock_irqrestore(&cg_lock, flags);
    if (v == CG_PIDS_MAX_UNSET)
      len = cg_append(buf, cap, len, "max\n");
    else {
      len = cg_append_u64(buf, cap, len, (u64)v);
      len = cg_append(buf, cap, len, "\n");
    }
    break;
  }
  case CGF_PIDS_EVENTS: {
    spin_lock_irqsave(&cg_lock, &flags);
    u64 d = cg->pids_denied;
    spin_unlock_irqrestore(&cg_lock, flags);
    len = cg_append(buf, cap, len, "max ");
    len = cg_append_u64(buf, cap, len, d);
    len = cg_append(buf, cap, len, "\n");
    break;
  }
  default:
    break;
  }
  return len;
}

static isize cg_read_cb(struct vfs_node *node, u64 offset, char *buffer,
                        usize size, int flags) {
  (void)flags;
  struct cg_filenode *fn = node->inode ? node->inode->data : 0;
  if (!fn || !fn->cg)
    return -EIO;

  /* cgroup.procs of the root can name every task in the machine, so the buffer
   * is sized for the task table rather than guessed. */
  usize cap = 4096 + scheduler_task_slots() * 12;
  char *buf = kmalloc(cap);
  if (!buf)
    return -ENOMEM;
  buf[0] = '\0';
  usize len = cg_render(fn->cg, fn->kind, buf, cap);
  isize r = cg_emit(buf, len, offset, buffer, size);
  kfree(buf);
  return r;
}

/* ── writes ──────────────────────────────────────────────────────────────── */

static u64 cg_parse_u64(const char *s, usize len, int *ok) {
  u64 v = 0;
  usize i = 0;
  int digits = 0;
  while (i < len && (s[i] == ' ' || s[i] == '\t'))
    i++;
  while (i < len && s[i] >= '0' && s[i] <= '9') {
    v = v * 10 + (u64)(s[i] - '0');
    i++;
    digits++;
  }
  *ok = digits > 0;
  return v;
}

/* Move every thread of `pid`'s thread group into `cg`. */
static int cg_attach_process(struct cgroup *cg, usize pid, int threads_only) {
  struct task *t = scheduler_task_by_pid(pid);
  if (!cg_task_live(t))
    return -ESRCH;
  usize tgid = threads_only ? 0 : task_tgid(t);

  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  if (threads_only) {
    cg_member_set(pid, cg->is_root ? 0 : cg);
  } else {
    usize slots = scheduler_task_slots();
    for (usize i = 0; i < slots; i++) {
      struct task *o = scheduler_task_slot(i);
      if (cg_task_live(o) && task_tgid(o) == tgid)
        cg_member_set(o->id, cg->is_root ? 0 : cg);
    }
  }
  spin_unlock_irqrestore(&cg_lock, flags);
  cg_events_refresh();
  return 0;
}

static isize cg_write_cb(struct vfs_node *node, u64 offset, const char *buffer,
                         usize size, int flags) {
  (void)offset;
  (void)flags;
  struct cg_filenode *fn = node->inode ? node->inode->data : 0;
  if (!fn || !fn->cg)
    return -EIO;
  if (size == 0)
    return 0;
  struct cgroup *cg = fn->cg;
  u64 lock_flags;

  switch (fn->kind) {
  case CGF_PROCS:
  case CGF_THREADS: {
    int ok = 0;
    u64 pid = cg_parse_u64(buffer, size, &ok);
    if (!ok)
      return -EINVAL;
    int r = cg_attach_process(cg, (usize)pid, fn->kind == CGF_THREADS);
    return r < 0 ? r : (isize)size;
  }
  case CGF_SUBTREE_CONTROL: {
    /* "+pids -memory": a list of signed controller names. An unknown or
     * unavailable controller is ENOENT, as on Linux. */
    usize i = 0;
    u32 add = 0, del = 0;
    while (i < size) {
      while (i < size && (buffer[i] == ' ' || buffer[i] == '\n' ||
                          buffer[i] == '\t'))
        i++;
      if (i >= size)
        break;
      char sign = buffer[i];
      if (sign != '+' && sign != '-')
        return -EINVAL;
      i++;
      usize start = i;
      while (i < size && buffer[i] != ' ' && buffer[i] != '\n' &&
             buffer[i] != '\t')
        i++;
      usize nlen = i - start;
      u32 bit = 0;
      for (usize c = 0; c < sizeof(cg_controllers) / sizeof(cg_controllers[0]);
           c++)
        if (strlen(cg_controllers[c].name) == nlen &&
            strncmp(buffer + start, cg_controllers[c].name, nlen) == 0)
          bit = cg_controllers[c].bit;
      if (!bit)
        return -ENOENT;
      /* Only a controller this cgroup itself has available may be delegated. */
      spin_lock_irqsave(&cg_lock, &lock_flags);
      u32 avail = cg->is_root ? CG_CTRL_PIDS
                              : (cg->parent ? cg->parent->subtree_control : 0);
      spin_unlock_irqrestore(&cg_lock, lock_flags);
      if (!(avail & bit))
        return -ENOENT;
      if (sign == '+')
        add |= bit;
      else
        del |= bit;
    }
    spin_lock_irqsave(&cg_lock, &lock_flags);
    cg->subtree_control = (cg->subtree_control | add) & ~del;
    spin_unlock_irqrestore(&cg_lock, lock_flags);
    return (isize)size;
  }
  case CGF_PIDS_MAX:
  case CGF_MAX_DEPTH:
  case CGF_MAX_DESCENDANTS: {
    u32 v;
    usize i = 0;
    while (i < size && (buffer[i] == ' ' || buffer[i] == '\t'))
      i++;
    if (size - i >= 3 && strncmp(buffer + i, "max", 3) == 0) {
      v = CG_PIDS_MAX_UNSET;
    } else {
      int ok = 0;
      u64 n = cg_parse_u64(buffer + i, size - i, &ok);
      if (!ok)
        return -EINVAL;
      v = (n > 0xFFFFFFFEull) ? CG_PIDS_MAX_UNSET : (u32)n;
    }
    spin_lock_irqsave(&cg_lock, &lock_flags);
    if (fn->kind == CGF_PIDS_MAX)
      cg->pids_max = v;
    else if (fn->kind == CGF_MAX_DEPTH)
      cg->max_depth = v;
    else
      cg->max_descendants = v;
    spin_unlock_irqrestore(&cg_lock, lock_flags);
    return (isize)size;
  }
  default:
    return -EACCES;
  }
}

/* ── node construction ───────────────────────────────────────────────────── */

static int cg_statfs(struct vfs_node *node, struct b1nix_statfs *st) {
  (void)node;
  if (!st)
    return -EINVAL;
  memset(st, 0, sizeof(*st));
  st->f_type = 0x63677270; /* CGROUP2_SUPER_MAGIC — what systemd looks for */
  st->f_bsize = 4096;
  st->f_namelen = 255;
  return 0;
}

static struct vfs_node *cg_find_child(struct vfs_node *dir, const char *name) {
  for (struct vfs_node *c = dir->first_child; c; c = c->next_sibling)
    if (!c->deleted && strcmp(c->name, name) == 0)
      return c;
  return 0;
}

static struct vfs_node *cg_mkfile(struct cgroup *cg, const char *name,
                                  enum cg_file kind, int writable) {
  if (cg_find_child(cg->dir, name))
    return 0;
  struct vfs_node *n = vfs_create_node(VFS_DEVICE);
  if (!n)
    return 0;
  usize nl = strlen(name);
  if (nl > VFS_NAME_MAX - 1)
    nl = VFS_NAME_MAX - 1;
  memcpy(n->name, name, nl);
  n->name[nl] = '\0';
  n->inode->mode = writable ? 0644 : 0444;
  n->inode->nlink = 1;
  n->inode->uid = 0;
  n->inode->gid = 0;
  n->inode->flags |= VFS_NODE_PSEUDO_REG;
  struct cg_filenode *fn = kzalloc(sizeof(*fn));
  if (!fn) {
    n->deleted = 1;
    vfs_node_put(n);
    return 0;
  }
  fn->cg = cg;
  fn->kind = kind;
  n->inode->data = fn;
  n->inode->read_cb = cg_read_cb;
  if (writable)
    n->inode->write_cb = cg_write_cb;
  n->parent = cg->dir;
  n->refcount++;
  vfs_attach_child(cg->dir, n);
  return n;
}

static void cg_rmfile(struct cgroup *cg, const char *name) {
  struct vfs_node *n = cg_find_child(cg->dir, name);
  if (!n)
    return;
  vfs_detach_child(cg->dir, n);
  if (n->inode && n->inode->data) {
    kfree(n->inode->data);
    n->inode->data = 0;
  }
  n->deleted = 1;
  vfs_node_put(n);
}

/* Create or remove the per-controller interface files, which exist in a cgroup
 * exactly when its parent has that controller in cgroup.subtree_control. */
static void cg_sync_controller_files(struct cgroup *cg) {
  if (cg->is_root)
    return;
  u32 avail = cg->parent ? cg->parent->subtree_control : 0;
  if (avail & CG_CTRL_PIDS) {
    cg_mkfile(cg, "pids.current", CGF_PIDS_CURRENT, 0);
    cg_mkfile(cg, "pids.peak", CGF_PIDS_PEAK, 0);
    cg_mkfile(cg, "pids.max", CGF_PIDS_MAX, 1);
    cg_mkfile(cg, "pids.events", CGF_PIDS_EVENTS, 0);
  } else {
    cg_rmfile(cg, "pids.current");
    cg_rmfile(cg, "pids.peak");
    cg_rmfile(cg, "pids.max");
    cg_rmfile(cg, "pids.events");
  }
}

static void cg_populate(struct cgroup *cg) {
  cg_mkfile(cg, "cgroup.procs", CGF_PROCS, 1);
  cg_mkfile(cg, "cgroup.threads", CGF_THREADS, 1);
  cg_mkfile(cg, "cgroup.controllers", CGF_CONTROLLERS, 0);
  cg_mkfile(cg, "cgroup.subtree_control", CGF_SUBTREE_CONTROL, 1);
  cg_mkfile(cg, "cgroup.stat", CGF_STAT, 0);
  if (!cg->is_root) {
    cg->events_node = cg_mkfile(cg, "cgroup.events", CGF_EVENTS, 0);
    cg_mkfile(cg, "cgroup.type", CGF_TYPE, 0);
    cg_mkfile(cg, "cgroup.max.depth", CGF_MAX_DEPTH, 1);
    cg_mkfile(cg, "cgroup.max.descendants", CGF_MAX_DESCENDANTS, 1);
  }
  cg_sync_controller_files(cg);
}

static int cg_mkdir_cb(struct vfs_node *dir, const char *name, u32 mode);
static int cg_rmdir_cb(struct vfs_node *dir, const char *name);
static int cg_unlink_cb(struct vfs_node *dir, const char *name);

static void cg_dir_init(struct cgroup *cg, struct vfs_node *dir) {
  cg->dir = dir;
  dir->inode->data = cg;
  dir->inode->statfs_cb = cg_statfs;
  dir->inode->mkdir_cb = cg_mkdir_cb;
  dir->inode->rmdir_cb = cg_rmdir_cb;
  dir->inode->unlink_cb = cg_unlink_cb;
  /* Its control files are the filesystem's, not the directory's contents:
   * rmdir must not see them as "not empty" (see vfs_remove_child_locked). */
  dir->inode->flags |= VFS_NODE_CTRL_CHILDREN;
}

static struct cgroup *cg_new(struct cgroup *parent, struct vfs_node *dir) {
  struct cgroup *cg = kzalloc(sizeof(*cg));
  if (!cg)
    return 0;
  cg->parent = parent;
  cg->pids_max = CG_PIDS_MAX_UNSET;
  cg->max_depth = CG_PIDS_MAX_UNSET;
  cg->max_descendants = CG_PIDS_MAX_UNSET;
  cg->is_root = parent ? 0 : 1;
  cg_dir_init(cg, dir);

  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  cg->next = cg_all;
  cg_all = cg;
  spin_unlock_irqrestore(&cg_lock, flags);
  return cg;
}

static int cg_mkdir_cb(struct vfs_node *dir, const char *name, u32 mode) {
  (void)mode;
  struct cgroup *parent = dir->inode ? dir->inode->data : 0;
  if (!parent)
    return -EIO;

  /* cgroup.max.depth / cgroup.max.descendants, enforced on the ancestor that
   * set them — the only moment either limit means anything. */
  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  for (struct cgroup *a = parent; a; a = a->parent) {
    if (a->max_depth != CG_PIDS_MAX_UNSET &&
        cg_depth(parent) + 1 - cg_depth(a) > a->max_depth) {
      spin_unlock_irqrestore(&cg_lock, flags);
      return -EAGAIN;
    }
    if (a->max_descendants != CG_PIDS_MAX_UNSET &&
        cg_count_descendants(a) + 1 > a->max_descendants) {
      spin_unlock_irqrestore(&cg_lock, flags);
      return -EAGAIN;
    }
  }
  spin_unlock_irqrestore(&cg_lock, flags);

  struct vfs_node *child = cg_find_child(dir, name);
  if (!child)
    return -EIO;
  child->inode->mode = 0755;
  child->inode->nlink = 2;
  struct cgroup *cg = cg_new(parent, child);
  if (!cg)
    return -ENOMEM;
  cg_populate(cg);
  return 0;
}

static int cg_unlink_cb(struct vfs_node *dir, const char *name) {
  (void)dir;
  (void)name;
  /* A control file cannot be removed; only the directory it describes can. */
  return -EPERM;
}

static void cg_forget(struct cgroup *cg) {
  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup **pp = &cg_all;
  while (*pp) {
    if (*pp == cg) {
      *pp = cg->next;
      break;
    }
    pp = &(*pp)->next;
  }
  /* Anything still pointing here belongs to the parent now, exactly as Linux
   * refuses the rmdir until the cgroup is empty and then has nothing to move. */
  for (u32 i = 0; i < CG_SLOTS; i++)
    if (cg_members[i].pid && cg_members[i].cg == cg)
      cg_members[i].cg = cg->parent && !cg->parent->is_root ? cg->parent : 0;
  spin_unlock_irqrestore(&cg_lock, flags);
}

static int cg_rmdir_cb(struct vfs_node *dir, const char *name) {
  struct vfs_node *child = cg_find_child(dir, name);
  if (!child)
    return -ENOENT;
  struct cgroup *cg = child->inode ? child->inode->data : 0;
  if (!cg)
    return -EIO;

  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  if (cg_count_descendants(cg) > 0) {
    spin_unlock_irqrestore(&cg_lock, flags);
    return -ENOTEMPTY;
  }
  if (cg_count_tasks(cg, 1) > 0) {
    spin_unlock_irqrestore(&cg_lock, flags);
    return -EBUSY;
  }
  spin_unlock_irqrestore(&cg_lock, flags);

  /* Drop the control files, so the directory the VFS is about to unlink really
   * has no children left. */
  for (struct vfs_node *c = child->first_child; c;) {
    struct vfs_node *next = c->next_sibling;
    vfs_detach_child(child, c);
    if (c->inode && c->inode->data) {
      kfree(c->inode->data);
      c->inode->data = 0;
    }
    c->deleted = 1;
    vfs_node_put(c);
    c = next;
  }
  child->inode->data = 0;
  cg_forget(cg);
  kfree(cg);
  return 0;
}

/* ── mount ───────────────────────────────────────────────────────────────── */

static struct vfs_node *cg_mount_cb(const char *source, u64 flags, void *data) {
  (void)source;
  (void)flags;
  (void)data;
  if (cg_root)
    return ERR_PTR(-EBUSY); /* one unified hierarchy, as on Linux */
  struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
  if (!root)
    return ERR_PTR(-ENOMEM);
  root->inode->mode = 0755;
  root->inode->uid = 0;
  root->inode->gid = 0;
  root->inode->nlink = 2;
  struct cgroup *cg = cg_new(0, root);
  if (!cg) {
    root->deleted = 1;
    vfs_node_put(root);
    return ERR_PTR(-ENOMEM);
  }
  cg_root = cg;
  cg_populate(cg);
  return root;
}

static int cg_umount_cb(struct vfs_node *root_node) {
  (void)root_node;
  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  for (u32 i = 0; i < CG_SLOTS; i++) {
    cg_members[i].pid = 0;
    cg_members[i].cg = 0;
  }
  struct cgroup *c = cg_all;
  cg_all = 0;
  cg_root = 0;
  spin_unlock_irqrestore(&cg_lock, flags);
  while (c) {
    struct cgroup *next = c->next;
    kfree(c);
    c = next;
  }
  return 0;
}

static struct vfs_fs cgroup2_fs = {
    .name = "cgroup2", .mount = cg_mount_cb, .umount = cg_umount_cb,
    .flags = VFS_FS_NODEV};

void cgroup_init(void) { vfs_register_fs(&cgroup2_fs); }

/* ── task lifetime ───────────────────────────────────────────────────────── */

void cgroup_fork_inherit(usize parent_pid, usize child_pid) {
  if (!cg_root)
    return;
  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup *cg = cg_member_get(parent_pid);
  if (cg)
    cg_member_set(child_pid, cg);
  spin_unlock_irqrestore(&cg_lock, flags);
  if (cg)
    cg_events_refresh();
}

void cgroup_task_exit(usize pid) {
  if (!cg_root)
    return;
  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup *cg = cg_member_get(pid);
  if (cg)
    cg_member_set(pid, 0);
  spin_unlock_irqrestore(&cg_lock, flags);
  if (cg)
    cg_events_refresh();
}

int cgroup_fork_allowed(usize parent_pid) {
  if (!cg_root)
    return 0;
  u64 flags;
  int denied = 0;
  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup *cg = cg_member_get(parent_pid);
  for (struct cgroup *a = cg; a; a = a->parent) {
    if (a->pids_max == CG_PIDS_MAX_UNSET)
      continue;
    if (cg_count_tasks(a, 1) >= a->pids_max) {
      a->pids_denied++;
      denied = 1;
      break;
    }
  }
  spin_unlock_irqrestore(&cg_lock, flags);
  return denied ? -EAGAIN : 0;
}

int cgroup_path_of(usize pid, char *buf, usize len) {
  if (!buf || len < 2)
    return -EINVAL;
  if (!cg_root) {
    buf[0] = '/';
    buf[1] = '\0';
    return 1;
  }
  u64 flags;
  spin_lock_irqsave(&cg_lock, &flags);
  struct cgroup *cg = cg_of(pid);
  int r = cg_path(cg, buf, len);
  spin_unlock_irqrestore(&cg_lock, flags);
  return r;
}
