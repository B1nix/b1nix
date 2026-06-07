/* procfs — synthetic /proc filesystem (M34).
 *
 * Exposes kernel and per-process state as read-on-demand pseudo-files. Unlike
 * a real file, content is regenerated every read() from live kernel data
 * structures (the PMM, the scheduler task table, the current task's VMAs).
 *
 * Design notes:
 *  - Pseudo-files are VFS_DEVICE nodes carrying a read_cb. The VFS read path
 *    routes VFS_FILE+read_cb through the demand-paging page cache (which would
 *    freeze the first read's bytes); VFS_DEVICE+read_cb takes the simple
 *    "call read_cb(offset,size) every time" path, which is what synthetic
 *    /proc files need (e.g. /proc/uptime must advance between reads).
 *  - Path resolution (find_child) walks physical child nodes, so every file
 *    we expose is linked as a real child of the procfs root (or a pid dir).
 *  - Per-pid directories are materialised lazily when /proc is listed
 *    (procfs_refresh from the root readdir_cb). Their per-file read_cb derives
 *    the target pid from the parent directory's name and looks the task up by
 *    pid at read time, so a slot that is reused by a new task Just Works and a
 *    dead pid reports state 'Z'. Dirs are never pruned (bounded by distinct
 *    pids seen over a boot), which avoids freeing a node a reader still holds.
 */

#include <b1nix/bootinfo.h>
#include <b1nix/errno.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/vfs.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* 100 Hz LAPIC tick (see kernel_main: lapic_timer_start_periodic_ms(10)). */
#define PROCFS_HZ 100u

/* ── tiny string builder over a fixed buffer ── */
struct sbuf {
  char *p;
  usize cap;
  usize len;
};

static void sb_init(struct sbuf *s, char *buf, usize cap) {
  s->p = buf;
  s->cap = cap;
  s->len = 0;
  if (cap)
    buf[0] = '\0';
}

static void sb_puts(struct sbuf *s, const char *str) {
  while (*str && s->len + 1 < s->cap)
    s->p[s->len++] = *str++;
  s->p[s->len] = '\0';
}

/* Render content built into `buf` (len bytes) into the caller's [offset,size). */
__attribute__((format(printf, 2, 3))) static void sb_addf(struct sbuf *s,
                                                          const char *fmt,
                                                          ...) {
  if (s->len + 1 >= s->cap)
    return;
  va_list ap;
  va_start(ap, fmt);
  char scratch[256];
  int n = vsnprintf(scratch, sizeof(scratch), fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  sb_puts(s, scratch);
}

static isize procfs_emit(const char *buf, usize len, u64 offset, char *out,
                         usize size) {
  if (offset >= (u64)len)
    return 0;
  usize avail = len - (usize)offset;
  usize n = avail < size ? avail : size;
  memcpy(out, buf + (usize)offset, n);
  return (isize)n;
}

/* ── per-node payload ──
 * Stashed in inode->data. We never set VFS_NODE_OWNS_DATA, and procfs nodes
 * are permanent, so the VFS never frees this. */
typedef int (*procfs_render)(usize pid, struct sbuf *s);

struct procfs_node {
  procfs_render render; /* content generator */
  usize pid;            /* 0 = system file / derive from parent for pid files */
};

#define PROCFS_BUF 8192

static struct procfs_node *pn_of(struct vfs_node *node) {
  return (struct procfs_node *)node->inode->data;
}

/* Derive the pid for a per-process file from its parent directory's name. */
static usize pid_from_parent(struct vfs_node *node) {
  if (!node->parent)
    return 0;
  const char *name = node->parent->name;
  if (name[0] == 's') /* "self" */
    return scheduler_get_pid();
  usize v = 0;
  for (const char *c = name; *c >= '0' && *c <= '9'; c++)
    v = v * 10 + (usize)(*c - '0');
  return v;
}

static isize procfs_read_cb(struct vfs_node *node, u64 offset, char *buffer,
                            usize size, int flags) {
  (void)flags;
  struct procfs_node *pn = pn_of(node);
  if (!pn || !pn->render)
    return 0;
  char *tmp = kmalloc(PROCFS_BUF);
  if (!tmp)
    return -ENOMEM;
  struct sbuf s;
  sb_init(&s, tmp, PROCFS_BUF);
  usize pid = pn->pid ? pn->pid : pid_from_parent(node);
  int rc = pn->render(pid, &s);
  isize res;
  if (rc < 0)
    res = rc;
  else
    res = procfs_emit(tmp, s.len, offset, buffer, size);
  kfree(tmp);
  return res;
}

/* ── node construction ── */
static struct vfs_node *procfs_root;

static struct vfs_node *procfs_mkchild(struct vfs_node *parent,
                                       const char *name,
                                       enum vfs_node_type type,
                                       procfs_render render, usize pid) {
  struct vfs_node *n = vfs_create_node(type);
  if (!n)
    return 0;
  usize nl = strlen(name);
  if (nl > 63)
    nl = 63;
  memcpy(n->name, name, nl);
  n->name[nl] = '\0';
  n->inode->mode = (type == VFS_DIRECTORY) ? 0555 : 0444;
  n->inode->uid = 0;
  n->inode->gid = 0;
  n->inode->nlink = (type == VFS_DIRECTORY) ? 2 : 1;
  if (render) {
    struct procfs_node *pn = kzalloc(sizeof(*pn));
    if (pn) {
      pn->render = render;
      pn->pid = pid;
    }
    n->inode->data = pn;
    n->inode->read_cb = procfs_read_cb;
  }
  n->parent = parent;
  n->refcount++;
  vfs_attach_child(parent, n);
  return n;
}

/* ──────────────────────────────────────────────────────────────────────────
 * System files
 * ────────────────────────────────────────────────────────────────────────── */

static int r_meminfo(usize pid, struct sbuf *s) {
  (void)pid;
  u64 total = pmm_total_usable_memory();
  u64 freeb = pmm_free_memory_estimate();
  u64 used = total > freeb ? total - freeb : 0;
  sb_addf(s, "MemTotal:       %lu kB\n", (unsigned long)(total / 1024));
  sb_addf(s, "MemFree:        %lu kB\n", (unsigned long)(freeb / 1024));
  sb_addf(s, "MemAvailable:   %lu kB\n", (unsigned long)(freeb / 1024));
  sb_addf(s, "MemUsed:        %lu kB\n", (unsigned long)(used / 1024));
  return 0;
}

static int r_uptime(usize pid, struct sbuf *s) {
  (void)pid;
  u64 ticks = scheduler_get_uptime_ticks();
  u64 sec = ticks / PROCFS_HZ;
  u64 cs = (ticks % PROCFS_HZ); /* centiseconds at 100 Hz */
  sb_addf(s, "%lu.%lu %lu.%lu\n", (unsigned long)sec, (unsigned long)cs,
          (unsigned long)sec, (unsigned long)cs);
  return 0;
}

static int r_loadavg(usize pid, struct sbuf *s) {
  (void)pid;
  usize nproc = scheduler_task_count();
  sb_addf(s, "0.00 0.00 0.00 1/%lu %lu\n", (unsigned long)nproc,
          (unsigned long)nproc);
  return 0;
}

static int r_version(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "B1NIX version 0.22.0 (b1nix@localhost) (clang) #1 SMP\n");
  return 0;
}

static int r_cpuinfo(usize pid, struct sbuf *s) {
  (void)pid;
  usize n = (g_max_cpus > 0) ? (usize)g_max_cpus : 1;
  for (usize i = 0; i < n; i++) {
    sb_addf(s, "processor\t: %lu\n", (unsigned long)i);
    sb_puts(s, "vendor_id\t: B1NIX\n");
    sb_puts(s, "model name\t: b1nix virtual CPU\n");
    sb_puts(s, "\n");
  }
  return 0;
}

static int r_stat(usize pid, struct sbuf *s) {
  (void)pid;
  u64 ticks = scheduler_get_uptime_ticks();
  sb_puts(s, "cpu  0 0 0 0 0 0 0 0 0 0\n");
  sb_addf(s, "ctxt %lu\n", (unsigned long)ticks);
  sb_addf(s, "btime 0\n");
  sb_addf(s, "processes %lu\n", (unsigned long)scheduler_task_count());
  return 0;
}

static int r_filesystems(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "nodev\tprocfs\n");
  sb_puts(s, "nodev\tsysfs\n");
  sb_puts(s, "nodev\tinitramfs\n");
  sb_puts(s, "\text2\n");
  sb_puts(s, "\text4\n");
  sb_puts(s, "\tfat32\n");
  return 0;
}

static int r_mounts(usize pid, struct sbuf *s) {
  (void)pid;
  struct b1nix_mount_entry ents[MAX_MOUNTS];
  isize n = vfs_mounts(ents, MAX_MOUNTS);
  for (isize i = 0; i < n; i++) {
    const char *src = ents[i].source[0] ? ents[i].source : "none";
    const char *tgt = ents[i].target[0] ? ents[i].target : "/";
    const char *fstype = ents[i].fstype[0] ? ents[i].fstype : "none";
    const char *opts = (ents[i].flags & B1NIX_MS_RDONLY) ? "ro" : "rw";
    /* device mountpoint fstype options dump pass */
    sb_addf(s, "%s %s %s %s 0 0\n", src, tgt, fstype, opts);
  }
  return 0;
}

static int r_cmdline(usize pid, struct sbuf *s) {
  (void)pid;
  const char *cmd = bootinfo_cmdline();
  if (cmd) {
    sb_puts(s, cmd);
    sb_puts(s, "\n");
  }
  return 0;
}

/* ── per-process files ── */

/* Linux /proc/<pid>/stat and /proc/<pid>/comm expose the process "comm": the
 * basename of the executable, truncated to TASK_COMM_LEN-1 (15) chars — NOT the
 * full exec path. b1nix stores the exec path in t->name (e.g.
 * "/opt/busybox/bin/busybox"), so derive comm here. BusyBox procps
 * (pidof/pgrep/pkill/ps) match on this field, so getting it wrong silently
 * breaks process lookup by name. `out` must hold at least 16 bytes. */
#define PROC_COMM_LEN 16
static void proc_comm(const struct task *t, char out[PROC_COMM_LEN]) {
  const char *name = (t && t->name) ? t->name : "?";
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  if (!*base) /* trailing slash, e.g. a directory exec path */
    base = name;
  usize i = 0;
  while (base[i] && i < PROC_COMM_LEN - 1) {
    out[i] = base[i];
    i++;
  }
  out[i] = '\0';
}

static const char *state_long(const char *abbr) {
  switch (abbr[0]) {
  case 'R': return "R (running)";
  case 'S': return "S (sleeping)";
  case 'D': return "D (disk sleep)";
  case 'T': return "T (stopped)";
  case 'Z': return "Z (zombie)";
  default:  return "? (unknown)";
  }
}

static int r_pid_status(usize pid, struct sbuf *s) {
  struct task *t = scheduler_task_by_pid(pid);
  if (!t) {
    sb_addf(s, "Name:\t(gone)\nState:\tZ (zombie)\nPid:\t%lu\n",
            (unsigned long)pid);
    return 0;
  }
  const char *st = scheduler_state_name((int)t->state);
  char comm[PROC_COMM_LEN];
  proc_comm(t, comm);
  sb_addf(s, "Name:\t%s\n", comm);
  sb_addf(s, "State:\t%s\n", state_long(st));
  sb_addf(s, "Pid:\t%lu\n", (unsigned long)t->id);
  sb_addf(s, "PPid:\t%lu\n", (unsigned long)t->parent_id);
  if (t->cred)
    sb_addf(s, "Uid:\t%u\t%u\t%u\n", t->cred->uid, t->cred->euid,
            t->cred->suid);
  sb_addf(s, "PGid:\t%lu\n", (unsigned long)t->process_group_id);
  sb_addf(s, "Sid:\t%lu\n", (unsigned long)t->session_id);
  /* Heap span as a rough VmSize. */
  u64 vm = t->user_brk > t->heap_start ? t->user_brk - t->heap_start : 0;
  sb_addf(s, "VmData:\t%lu kB\n", (unsigned long)(vm / 1024));
  return 0;
}

static int r_pid_cmdline(usize pid, struct sbuf *s) {
  struct task *t = scheduler_task_by_pid(pid);
  if (t && t->name)
    sb_puts(s, t->name);
  return 0;
}

static int r_pid_comm(usize pid, struct sbuf *s) {
  struct task *t = scheduler_task_by_pid(pid);
  if (!t) {
    sb_puts(s, "(gone)\n");
    return 0;
  }
  char comm[PROC_COMM_LEN];
  proc_comm(t, comm);
  sb_addf(s, "%s\n", comm);
  return 0;
}

static int r_pid_stat(usize pid, struct sbuf *s) {
  struct task *t = scheduler_task_by_pid(pid);
  if (!t) {
    sb_addf(s, "%lu (gone) Z 0\n", (unsigned long)pid);
    return 0;
  }
  sb_addf(s, "%lu (%s) %s %lu\n", (unsigned long)t->id,
          t->name ? t->name : "?", scheduler_state_name((int)t->state),
          (unsigned long)t->parent_id);
  u64 vsz = t->user_brk > t->heap_start ? t->user_brk - t->heap_start : 0;
  char comm[PROC_COMM_LEN];
  proc_comm(t, comm);
  sb_addf(s,
          "%lu (%s) %s %lu %lu %lu 0 -1 0 0 0 0 0 0 0 0 0 %d 0 1 0 0 %lu %lu\n",
          (unsigned long)t->id, comm,
          scheduler_state_name((int)t->state), (unsigned long)t->parent_id,
          (unsigned long)t->process_group_id, (unsigned long)t->session_id,
          t->priority, (unsigned long)vsz, (unsigned long)(vsz / 4096));
  return 0;
}

static int r_pid_maps(usize pid, struct sbuf *s) {
  struct task *t = scheduler_task_by_pid(pid);
  if (!t)
    return 0;
  for (struct vm_area *v = t->vma_list; v; v = v->next) {
    char perms[5];
    perms[0] = (v->prot & 0x1) ? 'r' : '-'; /* PROT_READ  */
    perms[1] = (v->prot & 0x2) ? 'w' : '-'; /* PROT_WRITE */
    perms[2] = (v->prot & 0x4) ? 'x' : '-'; /* PROT_EXEC  */
    perms[3] = (v->flags & 0x1) ? 's' : 'p'; /* MAP_SHARED/PRIVATE */
    perms[4] = '\0';
    sb_addf(s, "%lx-%lx %s %lx %s\n", (unsigned long)v->start,
            (unsigned long)v->end, perms, (unsigned long)v->offset,
            v->node ? "[file]" : "[anon]");
  }
  return 0;
}

/* ── /proc/<pid> directory builder ── */
static struct vfs_node *procfs_make_piddir(struct vfs_node *parent,
                                           const char *name, usize pid) {
  struct vfs_node *d = procfs_mkchild(parent, name, VFS_DIRECTORY, 0, 0);
  if (!d)
    return 0;
  procfs_mkchild(d, "status", VFS_DEVICE, r_pid_status, pid);
  procfs_mkchild(d, "cmdline", VFS_DEVICE, r_pid_cmdline, pid);
  procfs_mkchild(d, "comm", VFS_DEVICE, r_pid_comm, pid);
  procfs_mkchild(d, "stat", VFS_DEVICE, r_pid_stat, pid);
  procfs_mkchild(d, "maps", VFS_DEVICE, r_pid_maps, pid);
  return d;
}

/* Ensure a /proc/<pid> directory exists for every live task. Called from the
 * root readdir_cb so `ls /proc` / `ps` see fresh pids. */
static volatile int procfs_refresh_lock;

static void procfs_refresh(void) {
  while (__sync_lock_test_and_set(&procfs_refresh_lock, 1))
    ;
  usize slots = scheduler_task_slots();
  for (usize i = 0; i < slots; i++) {
    struct task *t = scheduler_task_slot(i);
    if (!t || t->id == 0)
      continue;
    char name[16];
    snprintf(name, sizeof(name), "%lu", (unsigned long)t->id);
    if (find_child(procfs_root, name))
      continue;
    procfs_make_piddir(procfs_root, name, 0 /* derive from name */);
  }
  __sync_lock_release(&procfs_refresh_lock);
}

/* Root directory listing: materialise pid dirs, then mirror the default
 * ramfs-style child walk. */
static isize procfs_root_readdir(struct vfs_node *dir, usize offset,
                                 struct dirent *buf, usize max_entries) {
  procfs_refresh();
  return vfs_readdir_children(dir, offset, buf, max_entries);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Mount
 * ────────────────────────────────────────────────────────────────────────── */
static struct vfs_node *procfs_mount_cb(const char *source, u64 flags,
                                        void *data) {
  (void)source;
  (void)flags;
  (void)data;
  struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
  if (!root)
    return ERR_PTR(-ENOMEM);
  root->inode->mode = 0555;
  root->inode->readdir_cb = procfs_root_readdir;
  procfs_root = root;

  procfs_mkchild(root, "meminfo", VFS_DEVICE, r_meminfo, 0);
  procfs_mkchild(root, "uptime", VFS_DEVICE, r_uptime, 0);
  procfs_mkchild(root, "loadavg", VFS_DEVICE, r_loadavg, 0);
  procfs_mkchild(root, "version", VFS_DEVICE, r_version, 0);
  procfs_mkchild(root, "cpuinfo", VFS_DEVICE, r_cpuinfo, 0);
  procfs_mkchild(root, "stat", VFS_DEVICE, r_stat, 0);
  procfs_mkchild(root, "filesystems", VFS_DEVICE, r_filesystems, 0);
  procfs_mkchild(root, "mounts", VFS_DEVICE, r_mounts, 0);
  procfs_mkchild(root, "cmdline", VFS_DEVICE, r_cmdline, 0);

  /* /proc/self — per-process view of the *calling* task (pid resolved at read
   * time via pid_from_parent → scheduler_get_pid). */
  procfs_make_piddir(root, "self", 0);
  return root;
}

static struct vfs_fs procfs_fs = {
    .name = "procfs",
    .mount = procfs_mount_cb,
};

void procfs_init(void) { vfs_register_fs(&procfs_fs); }
