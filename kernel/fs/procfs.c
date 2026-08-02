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

#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/module.h>
#include <b1nix/ptrace.h>
#include <b1nix/resource_caps.h>
#include <b1nix/sched.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/blk.h>
#include <b1nix/pci.h>
#include <b1nix/version.h>
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

/* Append one raw byte, including '\0' — /proc/<pid>/environ and cmdline are
 * NUL-separated, which sb_puts (string-terminated) cannot express. */
static void sb_putc(struct sbuf *s, char c) {
  if (s->len + 1 >= s->cap)
    return;
  s->p[s->len++] = c;
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
/* M77 writable sysctl: parse a write to a /proc/sys file. Returns the number of
 * bytes consumed on success (the whole write), or a negative errno. */
typedef int (*procfs_writer)(usize pid, const char *buf, usize len);

struct procfs_node {
  procfs_render render; /* content generator */
  procfs_writer write;  /* optional sysctl setter (M77) */
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

/* M77: writable sysctl write_cb. Routes a whole-file write to the node's
 * procfs_writer, which parses the value and clamps via the resource-caps
 * setters. Mirrors the sysfs drop_caches write pattern. */
static isize procfs_write_cb(struct vfs_node *node, u64 offset,
                             const char *buffer, usize size, int flags) {
  (void)offset;
  (void)flags;
  struct procfs_node *pn = pn_of(node);
  if (!pn || !pn->write)
    return -EINVAL;
  if (size == 0)
    return 0;
  usize pid = pn->pid ? pn->pid : pid_from_parent(node);
  return (isize)pn->write(pid, buffer, size);
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

/* Create a writable sysctl pseudo-file (mode 0644, read_cb + write_cb). */
static struct vfs_node *procfs_mkchild_writable(struct vfs_node *parent,
                                                const char *name,
                                                procfs_render render,
                                                procfs_writer write) {
  struct vfs_node *n = procfs_mkchild(parent, name, VFS_DEVICE, render, 0);
  if (!n)
    return 0;
  struct procfs_node *pn = pn_of(n);
  if (pn)
    pn->write = write;
  n->inode->mode = 0644;
  n->inode->write_cb = procfs_write_cb;
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
  sb_addf(s, "B1NIX version %s (b1nix@localhost) (clang) #1 SMP\n", B1NIX_VERSION_STR);
  return 0;
}

static int r_cpuinfo(usize pid, struct sbuf *s) {
  (void)pid;
  usize n = (g_max_cpus > 0) ? (usize)g_max_cpus : 1;
  for (usize i = 0; i < n; i++) {
    sb_addf(s, "processor\t: %lu\n", (unsigned long)i);
    sb_puts(s, "vendor_id\t: B1NIX\n");
    sb_puts(s, "model name\t: b1nix virtual CPU\n");
    {
      u32 khz = arch_cpu_khz();
      if (khz)
        sb_addf(s, "cpu MHz\t\t: %lu.%03lu\n", (unsigned long)(khz / 1000),
                (unsigned long)(khz % 1000));
    }
    sb_puts(s, "\n");
  }
  return 0;
}

static int r_gpuinfo(usize pid, struct sbuf *s) {
  (void)pid;
  struct pci_device_info pci;
  int found = 0;
  for (int idx = 0; idx < 4; idx++) {
    if (pci_find_class(0x03, 0x00, (u8)idx, &pci) == 0) {
      found = 1;
      sb_addf(s, "vendor\t: 0x%04x\n", pci.vendor_id);
      sb_addf(s, "device\t: 0x%04x\n", pci.device_id);
      if (pci.vendor_id == 0x1af4 && pci.device_id == 0x1050) {
        sb_puts(s, "model name\t: VirtIO GPU\n");
      } else if (pci.vendor_id == 0x15ad && pci.device_id == 0x0405) {
        sb_puts(s, "model name\t: VMware SVGA II\n");
      } else if (pci.vendor_id == 0x80ee && pci.device_id == 0xbeef) {
        sb_puts(s, "model name\t: VirtualBox Graphics Adapter\n");
      } else if (pci.vendor_id == 0x1234 && pci.device_id == 0x1111) {
        sb_puts(s, "model name\t: QEMU Standard VGA\n");
      } else {
        sb_puts(s, "model name\t: Generic Display Adapter\n");
      }
      u32 vram = pci_get_vram_size(pci.vendor_id, pci.device_id);
      if (vram > 0) {
        sb_addf(s, "vram\t: %lu MB\n", (unsigned long)(vram / (1024 * 1024)));
      }
      break;
    }
  }
  if (!found) {
    if (pci_find_class(0x03, 0x02, 0, &pci) == 0) {
      found = 1;
      sb_addf(s, "vendor\t: 0x%04x\n", pci.vendor_id);
      sb_addf(s, "device\t: 0x%04x\n", pci.device_id);
      sb_puts(s, "model name\t: 3D Graphics Accelerator\n");
      u32 vram = pci_get_vram_size(pci.vendor_id, pci.device_id);
      if (vram > 0) {
        sb_addf(s, "vram\t: %lu MB\n", (unsigned long)(vram / (1024 * 1024)));
      }
    }
  }
  if (!found) {
    sb_puts(s, "model name\t: Basic Framebuffer\n");
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

/* /proc/vmstat — virtual-memory event counters. BusyBox `vmstat` opens this
 * with xfopen_for_read() (which aborts if the file is missing) and reads the
 * paging/swap fields (pgpgin/pgpgout/pswpin/pswpout). b1nix has no swap-event
 * accounting yet, so the counters are reported as zero — enough for `vmstat`
 * to run and print its single report without dying. */
static int r_vmstat(usize pid, struct sbuf *s) {
  (void)pid;
  u64 total = pmm_total_usable_memory();
  u64 freeb = pmm_free_memory_estimate();
  unsigned long pages = (unsigned long)(total / 4096);
  unsigned long free_pages = (unsigned long)(freeb / 4096);
  sb_addf(s, "nr_free_pages %lu\n", free_pages);
  sb_addf(s, "nr_inactive_anon 0\n");
  sb_addf(s, "nr_active_anon 0\n");
  sb_addf(s, "nr_inactive_file 0\n");
  sb_addf(s, "nr_active_file 0\n");
  sb_addf(s, "nr_mapped %lu\n", pages - free_pages);
  sb_addf(s, "pgpgin 0\n");
  sb_addf(s, "pgpgout 0\n");
  sb_addf(s, "pswpin 0\n");
  sb_addf(s, "pswpout 0\n");
  sb_addf(s, "pgfault 0\n");
  sb_addf(s, "pgmajfault 0\n");
  return 0;
}

/* /proc/swaps — the Linux swap-area table. b1nix attaches at most one swap
 * device, at boot (kernel/mm/swap.c), so the table has one row or none. */
static int r_swaps(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
  u64 total = 0, used = 0;
  if (swap_active() && swap_stats(&total, &used) == 0)
    sb_addf(s, "/dev/swap0                              partition\t%lu\t%lu\t-2\n",
            (unsigned long)(total * 4), (unsigned long)(used * 4)); /* KiB */
  return 0;
}

/* /proc/modules — b1nix is a monolithic kernel with no loadable modules, so
 * the list is genuinely empty (Linux prints nothing in that case too). */
static int r_modules(usize pid, struct sbuf *s) {
  (void)pid;
  char *buf = kmalloc(4096);
  if (!buf)
    return -ENOMEM;
  int len = module_proc_render(buf, 4096);
  if (len > 0)
    sb_addf(s, "%s", buf);
  kfree(buf);
  return 0;
}

/* ── /proc/sys — the sysctl tree, read-only (b1nix has no procfs write path;
 * the writable knobs Linux exposes here have their own syscalls, e.g.
 * sethostname(2)). ── */
static int r_sys_hostname(usize pid, struct sbuf *s) {
  (void)pid;
  char h[65];
  kernel_hostname_get(h, sizeof(h));
  sb_addf(s, "%s\n", h);
  return 0;
}

static int r_sys_domainname(usize pid, struct sbuf *s) {
  (void)pid;
  char d[65];
  kernel_domainname_get(d, sizeof(d));
  sb_addf(s, "%s\n", d);
  return 0;
}

static int r_sys_ostype(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "B1NIX\n");
  return 0;
}

static int r_sys_osrelease(usize pid, struct sbuf *s) {
  (void)pid;
  sb_addf(s, "%s\n", B1NIX_VERSION_STR);
  return 0;
}

static int r_sys_pid_max(usize pid, struct sbuf *s) {
  (void)pid;
  sb_addf(s, "%lu\n", (unsigned long)scheduler_max_tasks());
  return 0;
}

static int r_sys_file_max(usize pid, struct sbuf *s) {
  (void)pid;
  /* The per-task descriptor ceiling is RLIMIT_NOFILE; the kernel imposes no
   * separate system-wide table, so report the hard limit. */
  struct rlimit rl;
  unsigned long v = 1024;
  if (scheduler_getrlimit(RLIMIT_NOFILE, &rl) == 0)
    v = (unsigned long)rl.rlim_max;
  sb_addf(s, "%lu\n", v);
  return 0;
}

/* ── M77 writable resource caps (runtime-tunable hard caps) ── */

static int r_sys_shmmax(usize pid, struct sbuf *s) {
  (void)pid;
  sb_addf(s, "%lu\n", (unsigned long)g_resource_caps.shmmax_bytes);
  return 0;
}

static int r_sys_tcp_max(usize pid, struct sbuf *s) {
  (void)pid;
  sb_addf(s, "%lu\n", (unsigned long)resource_caps_tcp_max());
  return 0;
}

static int r_sys_pipe_max(usize pid, struct sbuf *s) {
  (void)pid;
  sb_addf(s, "%lu\n", (unsigned long)resource_caps_pipe_max());
  return 0;
}

static int r_sys_coredump_max(usize pid, struct sbuf *s) {
  (void)pid;
  sb_addf(s, "%lu\n", (unsigned long)g_resource_caps.coredump_max_bytes);
  return 0;
}

/* Parse an unsigned decimal from a sysctl write; only trailing whitespace is
 * allowed after the digits. Returns 0 and fills *out, or -1. */
static int sysctl_parse_u64(const char *buf, usize len, u64 *out) {
  usize i = 0;
  while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' ||
                     buf[i] == '\n'))
    i++;
  if (i >= len || buf[i] < '0' || buf[i] > '9')
    return -1;
  u64 v = 0;
  while (i < len && buf[i] >= '0' && buf[i] <= '9') {
    u64 d = (u64)(buf[i] - '0');
    if (v > (0xFFFFFFFFFFFFFFFFULL - d) / 10)
      return -1; /* overflow */
    v = v * 10 + d;
    i++;
  }
  while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' ||
                     buf[i] == '\n'))
    i++;
  if (i != len)
    return -1;
  *out = v;
  return 0;
}

static int w_sys_shmmax(usize pid, const char *buf, usize len) {
  (void)pid;
  u64 v;
  if (sysctl_parse_u64(buf, len, &v) < 0 || resource_caps_set_shmmax(v) < 0)
    return -EINVAL;
  return (int)len;
}

/* M80: /proc/sys/kernel/yama/ptrace_scope — 0 lets any task trace another it
 * owns; 1 additionally requires the tracer to be an ancestor of the target or
 * the tracer the target declared with prctl(PR_SET_PTRACER), which is the mode
 * a crash-handler process is designed around. */
static int r_sys_ptrace_scope(usize pid, struct sbuf *s) {
  (void)pid;
  sb_addf(s, "%d\n", ptrace_scope_get());
  return 0;
}

static int w_sys_ptrace_scope(usize pid, const char *buf, usize len) {
  (void)pid;
  u64 v;
  if (sysctl_parse_u64(buf, len, &v) < 0 || ptrace_scope_set((int)v) < 0)
    return -EINVAL;
  return (int)len;
}

static int w_sys_tcp_max(usize pid, const char *buf, usize len) {
  (void)pid;
  u64 v;
  if (sysctl_parse_u64(buf, len, &v) < 0 || v > 0xFFFFFFFFULL ||
      resource_caps_set_tcp_max((u32)v) < 0)
    return -EINVAL;
  return (int)len;
}

static int w_sys_pipe_max(usize pid, const char *buf, usize len) {
  (void)pid;
  u64 v;
  if (sysctl_parse_u64(buf, len, &v) < 0 || v > 0xFFFFFFFFULL ||
      resource_caps_set_max_pipes((u32)v) < 0)
    return -EINVAL;
  return (int)len;
}

static int w_sys_coredump_max(usize pid, const char *buf, usize len) {
  (void)pid;
  u64 v;
  if (sysctl_parse_u64(buf, len, &v) < 0 ||
      resource_caps_set_coredump_max(v) < 0)
    return -EINVAL;
  return (int)len;
}

static int r_filesystems(usize pid, struct sbuf *s) {
  (void)pid;
  /* The live registry, not a fixed list: a filesystem that arrives with a
   * module (isofs.ko, ntfs.ko, btrfs.ko) has to show up here, and disappear
   * again when the module is removed. */
  struct vfs_fs_info fs[48];
  usize n = vfs_list_filesystems(fs, sizeof(fs) / sizeof(fs[0]));
  for (usize i = 0; i < n; i++)
    sb_addf(s, "%s\t%s\n", (fs[i].flags & VFS_FS_NODEV) ? "nodev" : "",
            fs[i].name);
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

/* /proc/self/mountinfo — Linux mountinfo format, needed by BusyBox `lsblk`
 * (which xopen()s it and would abort if it were missing). The maj:min in
 * field 3 is the synthetic 8:<blk-index> used by /sys/block + /proc/partitions
 * for a `/dev/<blk>` source, else 0:<mount-index>. Layout:
 *   id parent maj:min root mountpoint opts - fstype source superopts */
static int r_mountinfo(usize pid, struct sbuf *s) {
  (void)pid;
  struct b1nix_mount_entry ents[MAX_MOUNTS];
  isize n = vfs_mounts(ents, MAX_MOUNTS);
  for (isize i = 0; i < n; i++) {
    const char *src = ents[i].source[0] ? ents[i].source : "none";
    const char *tgt = ents[i].target[0] ? ents[i].target : "/";
    const char *fstype = ents[i].fstype[0] ? ents[i].fstype : "none";
    const char *opts = (ents[i].flags & B1NIX_MS_RDONLY) ? "ro" : "rw";
    int maj = 0, min = (int)i;
    const char *devname = src;
    if (strncmp(devname, "/dev/", 5) == 0)
      devname += 5;
    usize bn = blk_count();
    for (usize b = 0; b < bn; b++) {
      struct block_device *d = blk_at(b);
      if (d && d->name && strcmp(d->name, devname) == 0) {
        maj = 8;
        min = (int)b;
        break;
      }
    }
    sb_addf(s, "%ld 1 %d:%d / %s %s - %s %s %s\n", (long)(i + 1), maj, min,
            tgt, opts, fstype, src, opts);
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

static int r_kallsyms(usize pid, struct sbuf *s) {
  (void)pid;
  extern const unsigned char __kallsyms_start[];
  extern const unsigned char __kallsyms_end[];
  const unsigned char *p = __kallsyms_start;
  const unsigned char *end = __kallsyms_end;
  while (p + 8 < end) {
    u64 a;
    memcpy(&a, p, 8);
    p += 8;
    const char *name = (const char *)p;
    while (p < end && *p) p++;
    p++;
    sb_addf(s, "%016lx t %s\n", (unsigned long)a, name);
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
  /* Thread-group identity and size. A crash reporter reads Tgid to map a thread
   * back to its process and Threads to know how many /proc/<pid>/task entries
   * it must dump; gdb reads TracerPid to refuse to attach twice. */
  sb_addf(s, "Tgid:\t%lu\n", (unsigned long)task_tgid(t));
  sb_addf(s, "TracerPid:\t%lu\n", (unsigned long)ptrace_tracer_pid(t));
  /* Linux prints four ids per line — real, effective, saved and filesystem —
   * and a Groups line, and every /proc parser (Crashpad's included) reads all
   * of them. Three fields is not a shorter version of this format, it is an
   * unparseable one. */
  if (t->cred) {
    sb_addf(s, "Uid:\t%u\t%u\t%u\t%u\n", t->cred->uid, t->cred->euid,
            t->cred->suid, t->cred->fsuid);
    sb_addf(s, "Gid:\t%u\t%u\t%u\t%u\n", t->cred->gid, t->cred->egid,
            t->cred->sgid, t->cred->fsgid);
    /* Linux terminates every entry on the Groups line with a space, including
     * the last one; parsers rely on that trailing separator. */
    sb_addf(s, "Groups:\t%u \n", t->cred->gid);
  }
  sb_addf(s, "PGid:\t%lu\n", (unsigned long)t->process_group_id);
  sb_addf(s, "Sid:\t%lu\n", (unsigned long)t->session_id);
  {
    usize tgid = task_tgid(t);
    usize nthreads = 0;
    usize slots = scheduler_task_slots();
    for (usize i = 0; i < slots; i++) {
      struct task *o = scheduler_task_slot(i);
      if (o && o->id && task_tgid(o) == tgid)
        nthreads++;
    }
    sb_addf(s, "Threads:\t%lu\n", (unsigned long)(nthreads ? nthreads : 1));
  }
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

/* /proc/<pid>/environ — the NUL-separated environment the image was execve'd
 * with (the same array the auxv/stack builder copied in). */
static int r_pid_environ(usize pid, struct sbuf *s) {
  struct task *t = scheduler_task_by_pid(pid);
  if (!t || !t->user_image)
    return 0;
  struct user_loaded_image *img = (struct user_loaded_image *)t->user_image;
  if (!img->envp)
    return 0;
  for (int i = 0; img->envp[i]; i++) {
    sb_puts(s, img->envp[i]);
    sb_putc(s, '\0');
  }
  return 0;
}

/* /proc/<pid>/statm — page counts: size resident shared text lib data dt.
 * Resident is measured for real by walking the task's page tables, so the
 * number reflects what is actually mapped (not the VMA span). */
static int r_pid_statm(usize pid, struct sbuf *s) {
  struct task *t = scheduler_task_by_pid(pid);
  if (!t) {
    sb_puts(s, "0 0 0 0 0 0 0\n");
    return 0;
  }
  unsigned long size = 0, resident = 0, shared = 0, text = 0, data = 0;
  for (struct vm_area *v = t->vma_list; v; v = v->next) {
    unsigned long pages = (unsigned long)((v->end - v->start) / PAGE_SIZE);
    size += pages;
    if (v->flags & 0x1) /* MAP_SHARED */
      shared += pages;
    if (v->prot & 0x4) /* PROT_EXEC */
      text += pages;
    else if (v->prot & 0x2) /* PROT_WRITE */
      data += pages;
    if (t->pml4_phys) {
      for (u64 va = v->start; va < v->end; va += PAGE_SIZE)
        if (paging_user_frame(t->pml4_phys, va))
          resident++;
    }
  }
  sb_addf(s, "%lu %lu %lu %lu 0 %lu 0\n", size, resident, shared, text, data);
  return 0;
}

/* /proc/<pid>/limits — the task's own rlimits, in the Linux column layout. */
static int r_pid_limits(usize pid, struct sbuf *s) {
  static const struct {
    int res;
    const char *name;
    const char *unit;
  } tbl[] = {
      {0, "Max cpu time", "seconds"},    {1, "Max file size", "bytes"},
      {2, "Max data size", "bytes"},     {3, "Max stack size", "bytes"},
      {4, "Max core file size", "bytes"},{5, "Max resident set", "bytes"},
      {6, "Max processes", "processes"}, {7, "Max open files", "files"},
      {8, "Max locked memory", "bytes"}, {9, "Max address space", "bytes"},
  };
  struct task *t = scheduler_task_by_pid(pid);
  sb_puts(s, "Limit                     Soft Limit           Hard Limit"
             "           Units\n");
  for (usize i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
    struct rlimit rl = {0, 0};
    if (!t || scheduler_getrlimit_task(t, tbl[i].res, &rl) != 0)
      continue;
    char soft[24], hard[24];
    if (rl.rlim_cur == (u64)-1)
      snprintf(soft, sizeof(soft), "unlimited");
    else
      snprintf(soft, sizeof(soft), "%lu", (unsigned long)rl.rlim_cur);
    if (rl.rlim_max == (u64)-1)
      snprintf(hard, sizeof(hard), "unlimited");
    else
      snprintf(hard, sizeof(hard), "%lu", (unsigned long)rl.rlim_max);
    sb_addf(s, "%-25s %-20s %-20s %s\n", tbl[i].name, soft, hard,
            tbl[i].unit);
  }
  return 0;
}

/* One line of /proc/<pid>/maps, gathered from every source before printing so
 * the file comes out in ascending address order. */
struct procfs_map_ent {
  u64 start;
  u64 end;
  u64 offset;
  int prot;   /* PROT_READ/WRITE/EXEC bits */
  int shared;
  unsigned long ino;
  unsigned long dev;  /* st_dev of the backing filesystem (0 = anonymous) */
  const char *name; /* 0 = anonymous */
};

static void procfs_maps_add(struct procfs_map_ent *m, usize *n, usize max,
                            u64 start, u64 end, int prot, int shared,
                            u64 offset, unsigned long ino, unsigned long dev,
                            const char *name) {
  if (*n >= max || end <= start)
    return;
  m[*n].start = start;
  m[*n].end = end;
  m[*n].prot = prot;
  m[*n].shared = shared;
  m[*n].offset = offset;
  m[*n].ino = ino;
  m[*n].dev = dev;
  m[*n].name = name;
  (*n)++;
}

static int r_pid_maps(usize pid, struct sbuf *s) {
  struct task *t = scheduler_task_by_pid(pid);
  if (!t)
    return 0;
#define PROCFS_MAPS_MAX 192
  struct procfs_map_ent m[PROCFS_MAPS_MAX];
  usize n = 0;

  /* mmap'd regions (the only ones the VMA list tracks). */
  for (struct vm_area *v = t->vma_list; v && n < PROCFS_MAPS_MAX; v = v->next)
    procfs_maps_add(m, &n, PROCFS_MAPS_MAX, v->start, v->end, (int)v->prot,
                    (v->flags & 0x1) != 0, v->offset,
                    (v->node && v->node->inode) ? v->node->inode->ino : 0,
                    v->node ? vfs_node_dev(v->node) : 0,
                    (v->node && v->node->name[0]) ? v->node->name : 0);

  /* Name the mappings that belong to the loaded image. The executable's and
   * the interpreter's segments are mapped by the kernel loader, so they reach
   * the VMA list without a backing vfs node and would otherwise print as
   * anonymous memory — leaving a crash reporter with an address it cannot
   * attribute to any module. */
  struct user_loaded_image *img = (struct user_loaded_image *)t->user_image;
  if (img) {
    /* Real inode numbers for the file-backed mappings. A reader that sees
     * dev 0 AND inode 0 concludes the mapping is anonymous — no file to find
     * the start of — and never looks back for the ELF header at the module's
     * first segment. */
    unsigned long exe_ino = 0, interp_ino = 0, exe_dev = 0, interp_dev = 0;
    if (img->path) {
      struct vfs_node *nd = vfs_find_node(img->path);
      if (nd && !IS_ERR(nd)) {
        if (nd->inode) {
          exe_ino = (unsigned long)nd->inode->ino;
          exe_dev = (unsigned long)vfs_node_dev(nd);
        }
        vfs_node_put(nd);
      }
    }
    if (img->interp_path[0]) {
      struct vfs_node *nd = vfs_find_node(img->interp_path);
      if (nd && !IS_ERR(nd)) {
        if (nd->inode) {
          interp_ino = (unsigned long)nd->inode->ino;
          interp_dev = (unsigned long)vfs_node_dev(nd);
        }
        vfs_node_put(nd);
      }
    }
    for (usize i = 0; i < n; i++) {
      if (m[i].name)
        continue;
      for (usize k = 0; k < img->segment_count; k++) {
        const struct user_image_segment *seg = &img->segments[k];
        if (!seg->memsz)
          continue;
        u64 sstart = seg->vaddr & ~(u64)(PAGE_SIZE - 1);
        u64 send =
            (seg->vaddr + seg->memsz + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
        if (m[i].start >= sstart && m[i].start < send) {
          int is_interp = (img->interp_base && seg->vaddr >= img->interp_base &&
                           img->interp_path[0]);
          m[i].name = is_interp ? img->interp_path : img->path;
          m[i].ino = is_interp ? interp_ino : exe_ino;
          m[i].dev = is_interp ? interp_dev : exe_dev;
          /* The file offset matters as much as the name: a reader treats a
           * mapping at offset 0 as the start of a module and looks for an ELF
           * header there. Reporting 0 for every segment claims each one is a
           * separate module beginning, which is how a crash reporter ends up
           * reading a data segment as an ELF header. */
          m[i].offset = seg->file_offset + (m[i].start - sstart);
          break;
        }
      }
    }
  }

  /* Ascending order, and no duplicate starts: Crashpad (and pmap, and glibc's
   * backtrace) reject a maps file that is out of order or overlapping. */
  for (usize i = 1; i < n; i++) {
    struct procfs_map_ent key = m[i];
    usize j = i;
    while (j > 0 && m[j - 1].start > key.start) {
      m[j] = m[j - 1];
      j--;
    }
    m[j] = key;
  }

  u64 prev_end = 0;
  for (usize i = 0; i < n; i++) {
    u64 start = m[i].start;
    if (start < prev_end)
      start = prev_end; /* clip an overlap rather than emitting one */
    if (m[i].end <= start)
      continue;
    char perms[5];
    perms[0] = (m[i].prot & 0x1) ? 'r' : '-';
    perms[1] = (m[i].prot & 0x2) ? 'w' : '-';
    perms[2] = (m[i].prot & 0x4) ? 'x' : '-';
    perms[3] = m[i].shared ? 's' : 'p';
    perms[4] = '\0';
    /* "start-end perms offset major:minor inode path". The device is the
     * st_dev of the filesystem the file lives on — a real disk's number for a
     * block-backed mount, 00:xx for a RAM filesystem, 00:00 for anonymous
     * memory. The inode column always ends with a space, as Linux's padding
     * guarantees, even when there is no path. */
    sb_addf(s, "%lx-%lx %s %lx %02lx:%02lx %lu %s\n", (unsigned long)start,
            (unsigned long)m[i].end, perms, (unsigned long)m[i].offset,
            (m[i].dev >> 8) & 0xfful, m[i].dev & 0xfful, m[i].ino,
            m[i].name ? m[i].name : "");
    prev_end = m[i].end;
  }
  return 0;
}

/* ── /proc/<pid> directory builder ── */
/* ── /proc/<pid>/fd/ — one symlink per open file descriptor (for lsof) ──
 * Linux exposes each fd as a symlink whose readlink yields the open file's
 * path (or a "socket:[..]"/"pipe:[..]" synthetic name). BusyBox lsof reads the
 * dir and readlinks each entry. We snapshot the task's fd table under its
 * fd_lock (no allocation while locked), then materialise the symlink children
 * outside the lock. Children are created on demand and left in place — procfs
 * nodes are permanent, so a closed fd's stale symlink is harmless. */
#define PROCFS_FD_SNAP_MAX 64
struct procfs_fd_snap {
  int fd;
  char target[80];
};

static void procfs_fd_symlink(struct vfs_node *dir, int fd, const char *target) {
  char name[16];
  snprintf(name, sizeof(name), "%d", fd);
  if (find_child(dir, name))
    return; /* already materialised */
  struct vfs_node *n = vfs_create_node(VFS_SYMLINK);
  if (!n)
    return;
  usize nl = strlen(name);
  memcpy(n->name, name, nl);
  n->name[nl] = '\0';
  char *dup = strdup(target);
  n->inode->data = dup;
  n->inode->size = dup ? strlen(dup) : 0;
  n->inode->mode = 0777;
  n->inode->nlink = 1;
  n->parent = dir;
  n->refcount++;
  vfs_attach_child(dir, n);
}

/* Render the readlink target for one open fd. Called with the owner's fd_lock
 * held, so it must not sleep/allocate — pty_index_of() only reads a plain int,
 * which is safe. PTY targets use the pts INDEX (a real /dev/pts/<N> the devpts
 * lookup_cb can stat), NOT the fd number: musl's ttyname() readlinks this and
 * then stat()s the result, so it has to be a genuine path. */
static void procfs_fd_fill_target(struct vfs_handle *h, int fd, char *tg,
                                  usize sz) {
  switch (h->kind) {
  case VFS_HANDLE_PIPE_READ:
  case VFS_HANDLE_PIPE_WRITE:
    snprintf(tg, sz, "pipe:[%d]", fd);
    break;
  case VFS_HANDLE_SOCKET:
    snprintf(tg, sz, "socket:[%d]", fd);
    break;
  case VFS_HANDLE_PTY_MASTER:
  case VFS_HANDLE_PTY_SLAVE: {
    int idx = pty_index_of(h);
    if (idx >= 0)
      snprintf(tg, sz, "/dev/pts/%d", idx);
    else
      snprintf(tg, sz, "anon_inode:[pts]");
    break;
  }
  case VFS_HANDLE_NODE:
    if (h->node && h->node->name[0])
      snprintf(tg, sz, "/%s", h->node->name);
    else
      snprintf(tg, sz, "anon_inode:[unknown]");
    break;
  default:
    snprintf(tg, sz, "anon_inode:[unknown]");
    break;
  }
}

static isize procfs_fd_readdir(struct vfs_node *dir, usize offset,
                               struct dirent *buf, usize max_entries) {
  usize pid = pid_from_parent(dir); /* dir->parent is the /proc/<pid> dir */
  struct task *t = scheduler_task_by_pid(pid);
  if (t) {
    struct procfs_fd_snap snap[PROCFS_FD_SNAP_MAX];
    usize nsnap = 0;
    u64 flags;
    spin_lock_irqsave(&t->fd_lock, &flags);
    usize cap = t->fd_capacity;
    for (usize i = 0; i < cap && nsnap < PROCFS_FD_SNAP_MAX; i++) {
      struct vfs_handle *h = t->fd_table ? t->fd_table[i] : 0;
      if (!h || !h->used)
        continue;
      snap[nsnap].fd = (int)i;
      procfs_fd_fill_target(h, (int)i, snap[nsnap].target,
                            sizeof(snap[nsnap].target));
      nsnap++;
    }
    spin_unlock_irqrestore(&t->fd_lock, flags);
    for (usize i = 0; i < nsnap; i++)
      procfs_fd_symlink(dir, snap[i].fd, snap[i].target);
  }
  return vfs_readdir_children(dir, offset, buf, max_entries);
}

/* On-demand resolution of a single /proc/<pid>/fd/<N> — the resolver invokes
 * this when a direct lookup misses (readdir was never run). Without it,
 * readlink("/proc/self/fd/N") — the core of musl's ttyname() — fails. */
static int procfs_fd_lookup(struct vfs_node *dir, const char *name) {
  int fd = 0;
  for (const char *q = name; *q; q++) {
    if (*q < '0' || *q > '9')
      return -1;
    fd = fd * 10 + (*q - '0');
  }
  usize pid = pid_from_parent(dir);
  struct task *t = scheduler_task_by_pid(pid);
  if (!t)
    return -1;
  char target[80];
  int ok = 0;
  u64 flags;
  spin_lock_irqsave(&t->fd_lock, &flags);
  struct vfs_handle *h =
      (t->fd_table && (usize)fd < t->fd_capacity) ? t->fd_table[fd] : 0;
  if (h && h->used) {
    procfs_fd_fill_target(h, fd, target, sizeof(target));
    ok = 1;
  }
  spin_unlock_irqrestore(&t->fd_lock, flags);
  if (!ok)
    return -1;
  procfs_fd_symlink(dir, fd, target); /* idempotent (find_child guard) */
  return 0;
}

/* /proc/<pid>/exe — symlink to the task's executable path (task->name). Tools
 * resolve their own install directory via readlink("/proc/self/exe"); a bare
 * argv[0] is not enough for clang's .S two-step (cc1 preprocess + cc1as
 * assemble), whose assembler re-exec otherwise fails to locate clang. The
 * target is rendered per-caller via a read_cb: pid_from_parent() resolves the
 * parent dir name ("self" -> the calling task, or a numeric pid) so a single
 * static node serves both /proc/self/exe and /proc/<pid>/exe correctly. */
static isize procfs_exe_readlink(struct vfs_node *node, u64 offset, char *buf,
                                 usize size, int flags) {
  (void)offset;
  (void)flags;
  usize pid = pid_from_parent(node);
  struct task *t = scheduler_task_by_pid(pid);
  const char *path = user_task_exe_path(t);
  if (!path || !path[0])
    return -EINVAL;
  usize len = strlen(path);
  if (len > size)
    len = size;
  memcpy(buf, path, len);
  return (isize)len;
}

/* /proc/<pid>/cwd and /proc/<pid>/root — symlinks to the task's working
 * directory and its filesystem root. b1nix has no per-process root (no
 * chroot), so root always resolves to "/", which is the truth for every task. */
static isize procfs_cwd_readlink(struct vfs_node *node, u64 offset, char *buf,
                                 usize size, int flags) {
  (void)offset;
  (void)flags;
  usize pid = pid_from_parent(node);
  struct task *t = scheduler_task_by_pid(pid);
  const char *path = (t && t->cwd[0]) ? t->cwd : "/";
  usize len = strlen(path);
  if (len > size)
    len = size;
  memcpy(buf, path, len);
  return (isize)len;
}

static isize procfs_root_readlink(struct vfs_node *node, u64 offset, char *buf,
                                  usize size, int flags) {
  (void)node;
  (void)offset;
  (void)flags;
  if (size < 1)
    return 0;
  buf[0] = '/';
  return 1;
}

static void procfs_make_symlink(struct vfs_node *dir, const char *name,
                                isize (*readlink_cb)(struct vfs_node *, u64,
                                                     char *, usize, int)) {
  if (find_child(dir, name))
    return;
  struct vfs_node *n = vfs_create_node(VFS_SYMLINK);
  if (!n)
    return;
  usize nl = strlen(name);
  if (nl > 63)
    nl = 63;
  memcpy(n->name, name, nl);
  n->name[nl] = '\0';
  n->inode->read_cb = readlink_cb;
  n->inode->size = 256;
  n->inode->mode = 0777;
  n->inode->nlink = 1;
  n->parent = dir;
  n->refcount++;
  vfs_attach_child(dir, n);
}

static void procfs_make_exe_symlink(struct vfs_node *dir, usize pid) {
  (void)pid; /* resolved dynamically from dir name via pid_from_parent */
  if (find_child(dir, "exe"))
    return;
  struct vfs_node *n = vfs_create_node(VFS_SYMLINK);
  if (!n)
    return;
  memcpy(n->name, "exe", 4);
  n->inode->read_cb = procfs_exe_readlink;
  n->inode->size = 256;
  n->inode->mode = 0777;
  n->inode->nlink = 1;
  n->parent = dir;
  n->refcount++;
  vfs_attach_child(dir, n);
}

/* ── /proc/<pid>/auxv and /proc/<pid>/mem (M80) ──────────────────────────────
 * Both read the target's own address space through its page tables
 * (ptrace_copy_from_task), which is what makes them usable on a process other
 * than the caller — the case a crash reporter is built around. Access is gated
 * by the same ptrace_may_access() check that guards PTRACE_ATTACH: being able
 * to read another process's memory is exactly the privilege ptrace grants. */
static isize procfs_auxv_read(struct vfs_node *node, u64 offset, char *buf,
                              usize size, int flags) {
  (void)flags;
  usize pid = pid_from_parent(node);
  struct task *t = scheduler_task_by_pid(pid);
  if (!t)
    return -ESRCH;
  if (!ptrace_may_access(t))
    return -EACCES;
  struct user_loaded_image *img = (struct user_loaded_image *)t->user_image;
  if (!img || !img->auxv_vaddr || !img->auxv_size)
    return 0; /* kernel task, or an image built before auxv was recorded */
  u8 tmp[512];
  usize len = img->auxv_size;
  if (len > sizeof(tmp))
    len = sizeof(tmp);
  isize got = ptrace_copy_from_task(t, img->auxv_vaddr, tmp, len);
  if (got < 0)
    return got;
  return procfs_emit((const char *)tmp, (usize)got, offset, buf, size);
}

/* /proc/<pid>/mem: the file offset IS the virtual address, so a reader pread()s
 * at the address it wants. Reads stop at the first unmapped page (short count),
 * matching Linux. */
static isize procfs_mem_read(struct vfs_node *node, u64 offset, char *buf,
                             usize size, int flags) {
  (void)flags;
  usize pid = pid_from_parent(node);
  struct task *t = scheduler_task_by_pid(pid);
  if (!t)
    return -ESRCH;
  if (!ptrace_may_access(t))
    return -EACCES;
  if (size == 0)
    return 0;
  return ptrace_copy_from_task(t, offset, buf, size);
}

static isize procfs_mem_write(struct vfs_node *node, u64 offset,
                              const char *buf, usize size, int flags) {
  (void)flags;
  usize pid = pid_from_parent(node);
  struct task *t = scheduler_task_by_pid(pid);
  if (!t)
    return -ESRCH;
  if (!ptrace_may_access(t))
    return -EACCES;
  if (size == 0)
    return 0;
  return ptrace_copy_to_task(t, offset, buf, size);
}

/* ── /proc/<pid>/task/<tid> (M80) ───────────────────────────────────────────
 * One directory per thread of the process. b1nix models a thread as a task of
 * its own whose tgid is the thread-group leader's pid (task_tgid), so the
 * per-thread files are the ordinary per-pid renderers pointed at the tid. */
static void procfs_make_tiddir(struct vfs_node *taskdir, usize tid) {
  char name[16];
  snprintf(name, sizeof(name), "%lu", (unsigned long)tid);
  if (find_child(taskdir, name))
    return;
  struct vfs_node *d = procfs_mkchild(taskdir, name, VFS_DIRECTORY, 0, 0);
  if (!d)
    return;
  procfs_mkchild(d, "status", VFS_DEVICE, r_pid_status, tid);
  procfs_mkchild(d, "stat", VFS_DEVICE, r_pid_stat, tid);
  procfs_mkchild(d, "comm", VFS_DEVICE, r_pid_comm, tid);
  procfs_mkchild(d, "maps", VFS_DEVICE, r_pid_maps, tid);
  procfs_mkchild(d, "statm", VFS_DEVICE, r_pid_statm, tid);
}

static isize procfs_task_readdir(struct vfs_node *dir, usize offset,
                                 struct dirent *buf, usize max_entries) {
  usize pid = pid_from_parent(dir); /* dir->parent is the /proc/<pid> dir */
  struct task *leader = scheduler_task_by_pid(pid);
  if (leader) {
    usize tgid = task_tgid(leader);
    usize slots = scheduler_task_slots();
    for (usize i = 0; i < slots; i++) {
      struct task *t = scheduler_task_slot(i);
      if (t && t->id && task_tgid(t) == tgid)
        procfs_make_tiddir(dir, t->id);
    }
  }
  return vfs_readdir_children(dir, offset, buf, max_entries);
}

static int procfs_task_lookup(struct vfs_node *dir, const char *name) {
  usize tid = 0;
  for (const char *q = name; *q; q++) {
    if (*q < '0' || *q > '9')
      return -1;
    tid = tid * 10 + (usize)(*q - '0');
  }
  usize pid = pid_from_parent(dir);
  struct task *leader = scheduler_task_by_pid(pid);
  struct task *t = scheduler_task_by_pid(tid);
  if (!leader || !t || task_tgid(t) != task_tgid(leader))
    return -1;
  procfs_make_tiddir(dir, tid); /* idempotent (find_child guard) */
  return 0;
}

static struct vfs_node *procfs_make_piddir(struct vfs_node *parent,
                                           const char *name, usize pid) {
  struct vfs_node *d = procfs_mkchild(parent, name, VFS_DIRECTORY, 0, 0);
  if (!d)
    return 0;
  procfs_make_exe_symlink(d, pid);
  procfs_mkchild(d, "status", VFS_DEVICE, r_pid_status, pid);
  procfs_mkchild(d, "cmdline", VFS_DEVICE, r_pid_cmdline, pid);
  procfs_mkchild(d, "comm", VFS_DEVICE, r_pid_comm, pid);
  procfs_mkchild(d, "stat", VFS_DEVICE, r_pid_stat, pid);
  procfs_mkchild(d, "maps", VFS_DEVICE, r_pid_maps, pid);
  procfs_mkchild(d, "mountinfo", VFS_DEVICE, r_mountinfo, pid);
  procfs_mkchild(d, "mounts", VFS_DEVICE, r_mounts, pid);
  procfs_mkchild(d, "environ", VFS_DEVICE, r_pid_environ, pid);
  procfs_mkchild(d, "statm", VFS_DEVICE, r_pid_statm, pid);
  procfs_mkchild(d, "limits", VFS_DEVICE, r_pid_limits, pid);
  procfs_make_symlink(d, "cwd", procfs_cwd_readlink);
  procfs_make_symlink(d, "root", procfs_root_readlink);
  struct vfs_node *fddir = procfs_mkchild(d, "fd", VFS_DIRECTORY, 0, 0);
  if (fddir) {
    fddir->inode->readdir_cb = procfs_fd_readdir;
    fddir->inode->lookup_cb = procfs_fd_lookup;
  }
  /* M80: auxv/mem carry binary content, so they take a read_cb of their own
   * instead of the text-rendering procfs_read_cb. */
  struct vfs_node *auxv = procfs_mkchild(d, "auxv", VFS_DEVICE, 0, pid);
  if (auxv) {
    auxv->inode->read_cb = procfs_auxv_read;
    auxv->inode->mode = 0444;
  }
  struct vfs_node *mem = procfs_mkchild(d, "mem", VFS_DEVICE, 0, pid);
  if (mem) {
    mem->inode->read_cb = procfs_mem_read;
    mem->inode->write_cb = procfs_mem_write;
    mem->inode->mode = 0600; /* ptrace_may_access still gates every access */
  }
  struct vfs_node *taskdir = procfs_mkchild(d, "task", VFS_DIRECTORY, 0, 0);
  if (taskdir) {
    taskdir->inode->readdir_cb = procfs_task_readdir;
    taskdir->inode->lookup_cb = procfs_task_lookup;
  }
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

/* Resolve /proc/<pid> on a direct lookup. Without this, a pid directory exists
 * only after somebody has listed /proc — a crash reporter that is handed a pid
 * and opens /proc/<pid>/task straight away would get ENOENT. */
static int procfs_root_lookup(struct vfs_node *dir, const char *name) {
  usize pid = 0;
  for (const char *q = name; *q; q++) {
    if (*q < '0' || *q > '9')
      return -1;
    pid = pid * 10 + (usize)(*q - '0');
  }
  if (!pid || !scheduler_task_by_pid(pid))
    return -1;
  if (find_child(dir, name))
    return 0;
  return procfs_make_piddir(dir, name, 0 /* derive from name */) ? 0 : -1;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Mount
 * ────────────────────────────────────────────────────────────────────────── */
/* ──────────────────────────────────────────────────────────────────────────
 * /proc/net — socket, route and interface tables (netstat, route, netstat -i)
 * ────────────────────────────────────────────────────────────────────────── */

/* Linux /proc/net/{tcp,udp} print the local/remote IPv4 as 8 hex digits in
 * little-endian byte order (so 127.0.0.1 -> "0100007F") and the port as 4 hex
 * digits in host order. BusyBox netstat decodes both. */
static void sb_hexaddr4(struct sbuf *s, const u8 ip[4], u16 port) {
  sb_addf(s, "%02X%02X%02X%02X:%04X", ip[3], ip[2], ip[1], ip[0], port);
}

static void net_render_inet(struct sbuf *s, int family /*4 or 6*/, int udp) {
  struct net_sock_info info[64];
  usize n = udp ? udp_binding_snapshot(info, 64) : tcp_conn_snapshot(info, 64);
  usize sl = 0;
  for (usize i = 0; i < n; i++) {
    if (info[i].family != family)
      continue;
    sb_addf(s, "%4lu: ", (unsigned long)sl++);
    if (family == 6) {
      const u8 *l = info[i].local_ip, *r = info[i].remote_ip;
      sb_addf(s, "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X:%04X ",
              l[3], l[2], l[1], l[0], l[7], l[6], l[5], l[4], l[11], l[10], l[9],
              l[8], l[15], l[14], l[13], l[12], info[i].local_port);
      sb_addf(s, "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X:%04X ",
              r[3], r[2], r[1], r[0], r[7], r[6], r[5], r[4], r[11], r[10], r[9],
              r[8], r[15], r[14], r[13], r[12], info[i].remote_port);
    } else {
      sb_hexaddr4(s, info[i].local_ip, info[i].local_port);
      sb_puts(s, " ");
      sb_hexaddr4(s, info[i].remote_ip, info[i].remote_port);
      sb_puts(s, " ");
    }
    sb_addf(s, "%02X 00000000:00000000 00:00000000 00000000     0        0 0\n",
            info[i].state);
  }
}

static int r_net_tcp(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "  sl  local_address rem_address   st tx_queue rx_queue tr "
             "tm->when retrnsmt   uid  timeout inode\n");
  net_render_inet(s, 4, 0);
  return 0;
}

static int r_net_tcp6(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "  sl  local_address                         "
             "remote_address                        st tx_queue rx_queue tr "
             "tm->when retrnsmt   uid  timeout inode\n");
  net_render_inet(s, 6, 0);
  return 0;
}

static int r_net_udp(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "  sl  local_address rem_address   st tx_queue rx_queue tr "
             "tm->when retrnsmt   uid  timeout inode ref pointer drops\n");
  net_render_inet(s, 4, 1);
  return 0;
}

static int r_net_udp6(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "  sl  local_address                         "
             "remote_address                        st tx_queue rx_queue tr "
             "tm->when retrnsmt   uid  timeout inode\n");
  net_render_inet(s, 6, 1);
  return 0;
}

static int r_net_unix(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "Num       RefCount Protocol Flags    Type St Inode Path\n");
  return 0;
}

/* /proc/net/route: hex, tab-separated. Destination/Gateway/Mask are 8 hex
 * digits in little-endian byte order. BusyBox `route` parses this. We emit the
 * default route via the DHCP-learnt gateway. */
static int r_net_route(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask"
             "\t\tMTU\tWindow\tIRTT\n");
  struct ipv4_addr ip = net_get_ip();
  struct ipv4_addr gw = net_get_gateway();
  const char *ifn = "eth0";
  if ((ip.bytes[0] | ip.bytes[1] | ip.bytes[2] | ip.bytes[3]) != 0) {
    /* On-link subnet route, assumed /24 (the QEMU SLIRP default and what the
     * smoke uses): dest = ip & 0xFFFFFF00, no gateway, flags UP (0x0001). */
    sb_addf(s,
            "%s\t%02X%02X%02X00\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n",
            ifn, ip.bytes[2], ip.bytes[1], ip.bytes[0]);
  }
  if ((gw.bytes[0] | gw.bytes[1] | gw.bytes[2] | gw.bytes[3]) != 0) {
    /* default route: 0.0.0.0/0 -> gateway, flags UP|GATEWAY (0x0003) */
    sb_addf(s,
            "%s\t00000000\t%02X%02X%02X%02X\t0003\t0\t0\t0\t00000000\t0\t0\t0\n",
            ifn, gw.bytes[3], gw.bytes[2], gw.bytes[1], gw.bytes[0]);
  }
  return 0;
}

/* /proc/net/dev: per-interface byte/packet counters. b1nix has no per-iface
 * counters, so they are zero; the interface list itself is real (netstat -i,
 * and a fallback path for some tools). */
static int r_net_dev(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "Inter-|   Receive                                                "
             "|  Transmit\n");
  sb_puts(s, " face |bytes    packets errs drop fifo frame compressed multicast"
             "|bytes    packets errs drop fifo colls carrier compressed\n");
  sb_puts(s, "    lo:       0       0    0    0    0     0          0         0"
             "       0       0    0    0    0     0       0          0\n");
  if (netdev_active())
    sb_puts(s, "  eth0:       0       0    0    0    0     0          0       "
               "  0       0       0    0    0    0     0       0          0\n");
  return 0;
}

/* /proc/partitions: one line per block device. #blocks is in 1 KiB units (the
 * Linux convention). BusyBox blkid/lsblk enumerate from this. */
static int r_partitions(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "major minor  #blocks  name\n\n");
  usize n = blk_count();
  for (usize i = 0; i < n; i++) {
    struct block_device *d = blk_at(i);
    if (!d || !d->name)
      continue;
    u64 kblocks = ((u64)d->block_size * d->block_count) / 1024;
    sb_addf(s, "%4d %7lu %10lu %s\n", 8, (unsigned long)i,
            (unsigned long)kblocks, d->name);
  }
  return 0;
}

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
  root->inode->lookup_cb = procfs_root_lookup;
  procfs_root = root;

  procfs_mkchild(root, "meminfo", VFS_DEVICE, r_meminfo, 0);
  procfs_mkchild(root, "uptime", VFS_DEVICE, r_uptime, 0);
  procfs_mkchild(root, "loadavg", VFS_DEVICE, r_loadavg, 0);
  procfs_mkchild(root, "version", VFS_DEVICE, r_version, 0);
  procfs_mkchild(root, "cpuinfo", VFS_DEVICE, r_cpuinfo, 0);
  procfs_mkchild(root, "gpuinfo", VFS_DEVICE, r_gpuinfo, 0);
  procfs_mkchild(root, "stat", VFS_DEVICE, r_stat, 0);
  procfs_mkchild(root, "vmstat", VFS_DEVICE, r_vmstat, 0);
  procfs_mkchild(root, "filesystems", VFS_DEVICE, r_filesystems, 0);
  procfs_mkchild(root, "mounts", VFS_DEVICE, r_mounts, 0);
  procfs_mkchild(root, "cmdline", VFS_DEVICE, r_cmdline, 0);
  procfs_mkchild(root, "kallsyms", VFS_DEVICE, r_kallsyms, 0);
  procfs_mkchild(root, "partitions", VFS_DEVICE, r_partitions, 0);
  procfs_mkchild(root, "swaps", VFS_DEVICE, r_swaps, 0);
  procfs_mkchild(root, "modules", VFS_DEVICE, r_modules, 0);

  /* /proc/sys — the sysctl tree Linux tools read directly (busybox sysctl,
   * hostname(1), OpenRC's sysctl service). */
  struct vfs_node *sysd = procfs_mkchild(root, "sys", VFS_DIRECTORY, 0, 0);
  if (sysd) {
    struct vfs_node *kern = procfs_mkchild(sysd, "kernel", VFS_DIRECTORY, 0, 0);
    if (kern) {
      procfs_mkchild(kern, "hostname", VFS_DEVICE, r_sys_hostname, 0);
      procfs_mkchild(kern, "domainname", VFS_DEVICE, r_sys_domainname, 0);
      procfs_mkchild(kern, "ostype", VFS_DEVICE, r_sys_ostype, 0);
      procfs_mkchild(kern, "osrelease", VFS_DEVICE, r_sys_osrelease, 0);
      procfs_mkchild(kern, "version", VFS_DEVICE, r_version, 0);
      procfs_mkchild(kern, "pid_max", VFS_DEVICE, r_sys_pid_max, 0);
      /* M80: yama-style ptrace attach restriction. */
      struct vfs_node *yama =
          procfs_mkchild(kern, "yama", VFS_DIRECTORY, 0, 0);
      if (yama)
        procfs_mkchild_writable(yama, "ptrace_scope", r_sys_ptrace_scope,
                                w_sys_ptrace_scope);
      /* M77: writable global resource caps. */
      procfs_mkchild_writable(kern, "shmmax", r_sys_shmmax, w_sys_shmmax);
      procfs_mkchild_writable(kern, "tcp-max-conns", r_sys_tcp_max,
                              w_sys_tcp_max);
      procfs_mkchild_writable(kern, "pipe-max-count", r_sys_pipe_max,
                              w_sys_pipe_max);
      procfs_mkchild_writable(kern, "coredump-max-bytes", r_sys_coredump_max,
                              w_sys_coredump_max);
    }
    struct vfs_node *fsd = procfs_mkchild(sysd, "fs", VFS_DIRECTORY, 0, 0);
    if (fsd)
      procfs_mkchild(fsd, "file-max", VFS_DEVICE, r_sys_file_max, 0);
  }

  struct vfs_node *netd = procfs_mkchild(root, "net", VFS_DIRECTORY, 0, 0);
  if (netd) {
    procfs_mkchild(netd, "tcp", VFS_DEVICE, r_net_tcp, 0);
    procfs_mkchild(netd, "tcp6", VFS_DEVICE, r_net_tcp6, 0);
    procfs_mkchild(netd, "udp", VFS_DEVICE, r_net_udp, 0);
    procfs_mkchild(netd, "udp6", VFS_DEVICE, r_net_udp6, 0);
    procfs_mkchild(netd, "unix", VFS_DEVICE, r_net_unix, 0);
    procfs_mkchild(netd, "route", VFS_DEVICE, r_net_route, 0);
    procfs_mkchild(netd, "dev", VFS_DEVICE, r_net_dev, 0);
  }

  /* /proc/self — per-process view of the *calling* task (pid resolved at read
   * time via pid_from_parent → scheduler_get_pid). */
  procfs_make_piddir(root, "self", 0);
  return root;
}

static struct vfs_fs procfs_fs = {
    .name = "procfs",
    .mount = procfs_mount_cb,
    .flags = VFS_FS_NODEV,
};

/* Linux calls this filesystem "proc"; `mount -t proc proc /proc` from an init
 * script or busybox must work as well as b1nix's own "procfs" name. */
static struct vfs_fs procfs_linux_alias;

void procfs_init(void) {
  vfs_register_fs(&procfs_fs);
  procfs_linux_alias = procfs_fs;
  procfs_linux_alias.name = "proc";
  procfs_linux_alias.next = 0;
  vfs_register_fs(&procfs_linux_alias);
}
