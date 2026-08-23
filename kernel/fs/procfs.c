#include <b1nix/console.h>
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
#include <b1nix/cgroup.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/kmsg.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/module.h>
#include <b1nix/namespace.h>
#include <b1nix/ptrace.h>
#include <b1nix/resource_caps.h>
#include <b1nix/sched.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/vnet.h>
#include <b1nix/blk.h>
#include <b1nix/pci.h>
#include <b1nix/version.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The scheduler tick, as programmed — /proc reports times in these units and a
 * literal here would misreport every one of them the moment the rate changed. */
#define PROCFS_HZ (sched_tick_hz())

/* ── tiny string builder over a fixed buffer ── */
struct sbuf {
  char *p;
  usize cap;
  usize len;
  int owned; /* p was allocated by sb_grow and must be freed */
};

static void sb_init(struct sbuf *s, char *buf, usize cap) {
  s->p = buf;
  s->cap = cap;
  s->len = 0;
  s->owned = 0;
  if (cap)
    buf[0] = '\0';
}

/* Release anything sb_grow allocated. The caller still owns the buffer it
 * passed to sb_init. */
static void sb_free(struct sbuf *s) {
  if (s->owned)
    kfree(s->p);
  s->p = 0;
  s->cap = 0;
  s->len = 0;
  s->owned = 0;
}

/* Grow to fit `need` more bytes.
 *
 * The builder used to stop at a fixed 8 KiB, which is a hundred-odd lines —
 * enough for a shell, not for a process that maps its binary and a hundred
 * libraries. /proc/<pid>/maps was cut off mid-file with no indication, and a
 * reader that parses it saw a memory map that simply ended. */
static void sb_grow(struct sbuf *s, usize need) {
  if (s->len + need + 1 <= s->cap)
    return;
  usize ncap = s->cap ? s->cap : 8192;
  while (ncap < s->len + need + 1) {
    if (ncap > (usize)1 << 28) /* refuse to grow without bound */
      return;
    ncap *= 2;
  }
  char *n = kmalloc(ncap);
  if (!n)
    return; /* keep what we have; the content truncates rather than faults */
  memcpy(n, s->p, s->len);
  n[s->len] = '\0';
  if (s->owned)
    kfree(s->p);
  s->p = n;
  s->cap = ncap;
  s->owned = 1;
}

static void sb_puts(struct sbuf *s, const char *str) {
  usize n = 0;
  while (str[n])
    n++;
  sb_grow(s, n);
  while (*str && s->len + 1 < s->cap)
    s->p[s->len++] = *str++;
  s->p[s->len] = '\0';
}

/* Append one raw byte, including '\0' — /proc/<pid>/environ and cmdline are
 * NUL-separated, which sb_puts (string-terminated) cannot express. */
static void sb_putc(struct sbuf *s, char c) {
  sb_grow(s, 1);
  if (s->len + 1 >= s->cap)
    return;
  s->p[s->len++] = c;
  s->p[s->len] = '\0';
}

/* Render content built into `buf` (len bytes) into the caller's [offset,size). */
__attribute__((format(printf, 2, 3))) static void sb_addf(struct sbuf *s,
                                                          const char *fmt,
                                                          ...) {
  va_list ap;
  va_start(ap, fmt);
  char scratch[256];
  int n = vsnprintf(scratch, sizeof(scratch), fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  /* A result longer than the scratch used to be truncated in silence — that is
   * how /proc/modules lost the tail of its last line (an address cut to
   * "0xffff") once the module list grew past 256 bytes. Retry through a buffer
   * of the size vsnprintf just told us it needs. */
  if (n >= (int)sizeof(scratch)) {
    char *big = kmalloc((usize)n + 1);
    if (!big)
      return;
    va_start(ap, fmt);
    int n2 = vsnprintf(big, (usize)n + 1, fmt, ap);
    va_end(ap);
    if (n2 >= 0)
      sb_puts(s, big);
    kfree(big);
    return;
  }
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
  /* The snapshot a sequential read continues from.
   *
   * Content here is generated on demand, and it used to be regenerated on
   * every read(): a file longer than one read returned its first part from
   * one moment and its next part from another, so a reader assembling the
   * whole thing got lines that had moved, repeated or been cut in half. That
   * is precisely what a crash reporter re-reads /proc/<pid>/maps to detect,
   * and why it kept reporting that its retries never agreed.
   *
   * The first read of a file (offset 0) takes the snapshot; the reads that
   * continue it are served from that copy. Keyed by reader, so two tasks
   * walking the same node do not consume each other's. */
  char *snap;
  usize snap_len;
  usize snap_owner; /* tid of the reader the snapshot belongs to */
};

#define PROCFS_BUF 8192

/* Guards the per-node read snapshots. Held only for pointer swaps and one
 * memcpy into a kernel buffer — no allocation, no sleeping. */
static spinlock_t procfs_snap_lock = SPINLOCK_INIT;

static struct procfs_node *pn_of(struct vfs_node *node) {
  return (struct procfs_node *)node->inode->data;
}

/* Derive the pid for a per-process file from its parent directory's name. */
static usize pid_from_parent(struct vfs_node *node) {
  if (!node->parent)
    return 0;
  const char *name = node->parent->name;
  /* M109: /proc/<pid>/ns/<kind> sits one level deeper than the rest of the
   * per-process files, and "ns" carries no pid of its own — the name that does
   * is the directory above it. */
  if (strcmp(name, "ns") == 0 && node->parent->parent)
    return pid_from_parent(node->parent);
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
  usize me = current_task ? (usize)current_task->id : 0;

  /* Continuing a read this reader already started: serve the snapshot it
   * began with, so the file it assembles is one consistent state. */
  if (offset > 0) {
    u64 sflags;
    usize got = 0;
    /* Copy out of the snapshot into our own buffer while holding the lock,
     * and hand it to the caller after dropping it: the caller's buffer can
     * fault, and faulting with interrupts disabled inside a spinlock wedges
     * the CPU (it did — a lockup and a thousand fault reports). */
    spin_lock_irqsave(&procfs_snap_lock, &sflags);
    if (pn->snap && pn->snap_owner == me && offset < (u64)pn->snap_len) {
      usize avail = pn->snap_len - (usize)offset;
      got = avail < size ? avail : size;
      if (got > PROCFS_BUF)
        got = PROCFS_BUF;
      memcpy(tmp, pn->snap + offset, got);
    }
    spin_unlock_irqrestore(&procfs_snap_lock, sflags);
    if (got) {
      memcpy(buffer, tmp, got);
      kfree(tmp);
      return (isize)got;
    }
  }

  struct sbuf s;
  sb_init(&s, tmp, PROCFS_BUF);
  usize pid = pn->pid ? pn->pid : pid_from_parent(node);
  int rc = pn->render(pid, &s);
  isize res;
  if (rc < 0) {
    res = rc;
  } else {
    res = procfs_emit(s.p, s.len, offset, buffer, size);

    /* Publish this as the snapshot the rest of the read continues from. */
    if (offset == 0 && s.len) {
      char *copy = kmalloc(s.len);
      if (copy) {
        memcpy(copy, s.p, s.len);
        u64 sflags;
        char *old;
        spin_lock_irqsave(&procfs_snap_lock, &sflags);
        old = pn->snap;
        pn->snap = copy;
        pn->snap_len = s.len;
        pn->snap_owner = me;
        spin_unlock_irqrestore(&procfs_snap_lock, sflags);
        kfree(old); /* freed outside the lock */
      }
    }
  }
  sb_free(&s);
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
  /* A /proc file is served through a device-style read callback, but to
   * anyone who stats it it is a REGULAR FILE — that is what it is on Linux,
   * and programs check. systemd's read_virtual_file() fstat()s the descriptor
   * and returns EBADF for anything that is not S_ISREG, so /proc/cmdline
   * reported as a character device made PID 1 fail before it had started:
   * "Failed to fix up PID 1 environment: Bad file descriptor". */
  if (type != VFS_DIRECTORY && type != VFS_SYMLINK)
    n->inode->flags |= VFS_NODE_PSEUDO_REG;
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

/* Same, for a per-process file: the pid the renderer and writer are called
 * with is the process the file belongs to, not 0. */
static struct vfs_node *procfs_mkchild_writable_pid(struct vfs_node *parent,
                                                    const char *name,
                                                    procfs_render render,
                                                    procfs_writer write,
                                                    usize pid) {
  struct vfs_node *n = procfs_mkchild(parent, name, VFS_DEVICE, render, pid);
  if (!n)
    return 0;
  struct procfs_node *pn = pn_of(n);
  if (pn)
    pn->write = write;
  n->inode->mode = 0644;
  n->inode->write_cb = procfs_write_cb;
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
  u64 hz = PROCFS_HZ;
  u64 sec = ticks / hz;
  /* Hundredths, derived from the tick rate rather than assuming it.
   *
   * This printed `ticks % 100` and called the result centiseconds, which is
   * true at one rate and at no other: at a kilohertz tick the remainder counts
   * milliseconds, and printed without padding it also lost them — five
   * milliseconds past the second came out as ".5", i.e. half a second. Linux
   * prints two digits here and so does this. */
  u64 cs = (ticks % hz) * 100u / hz;

  sb_addf(s, "%lu.%lu%lu %lu.%lu%lu\n", (unsigned long)sec,
          (unsigned long)(cs / 10), (unsigned long)(cs % 10),
          (unsigned long)sec, (unsigned long)(cs / 10),
          (unsigned long)(cs % 10));
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

/* /proc/modules — rendered from the live module list (M95). Empty when nothing
 * is loaded, exactly as Linux prints it. */
static int r_modules(usize pid, struct sbuf *s) {
  (void)pid;
  char *buf = kmalloc(4096);
  if (!buf)
    return -ENOMEM;
  int len = module_proc_render(buf, 4096);
  if (len > 0)
    sb_puts(s, buf); /* verbatim: the text is already formatted */
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

/* /proc/sys/kernel/random/boot_id — 128 random bits, formatted as a UUID, the
 * same for every reader for as long as the machine is up.
 *
 * It is not decoration. systemd-journald stamps every journal file header with
 * it (sd_id128_get_boot), and a missing file is ENOENT there — which journald
 * reports as "Failed to open runtime journal: No such file or directory" and
 * exits 1 on, forever, taking every unit that logs with it. `uuid` is the same
 * shape with a fresh value on each read, which is what Linux gives.
 *
 * Generated once, lazily, from the kernel's own entropy source. */
static void proc_uuid_str(char out[37], const u64 v[2]) {
  static const char hex[] = "0123456789abcdef";
  u8 b[16];
  for (int i = 0; i < 8; i++) {
    b[i] = (u8)(v[0] >> (8 * i));
    b[8 + i] = (u8)(v[1] >> (8 * i));
  }
  /* RFC 4122 version 4, variant 1 — what Linux writes and what
   * sd_id128_from_string() accepts. */
  b[6] = (u8)((b[6] & 0x0f) | 0x40);
  b[8] = (u8)((b[8] & 0x3f) | 0x80);
  int o = 0;
  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      out[o++] = '-';
    out[o++] = hex[(b[i] >> 4) & 0xf];
    out[o++] = hex[b[i] & 0xf];
  }
  out[o] = '\0';
}

static int r_sys_boot_id(usize pid, struct sbuf *s) {
  (void)pid;
  static u64 boot_id[2];
  static int have;
  if (!have) {
    boot_id[0] = kernel_random_u64();
    boot_id[1] = kernel_random_u64();
    have = 1;
  }
  char u[37];
  proc_uuid_str(u, boot_id);
  sb_addf(s, "%s\n", u);
  return 0;
}

static int r_sys_random_uuid(usize pid, struct sbuf *s) {
  (void)pid;
  u64 v[2] = {kernel_random_u64(), kernel_random_u64()};
  char u[37];
  proc_uuid_str(u, v);
  sb_addf(s, "%s\n", u);
  return 0;
}

/* The pool is a CSPRNG that never blocks here, so it is always full. */
static int r_sys_entropy_avail(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "256\n");
  return 0;
}

/* The highest capability this kernel knows. libcap and systemd read it to size
 * their bounding-set loops; without it they fall back to guessing. */
static int r_sys_cap_last_cap(usize pid, struct sbuf *s) {
  (void)pid;
  sb_addf(s, "%d\n", CAP_LAST_CAP);
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

/* /proc/sys/kernel/threads-max — the system-wide ceiling on tasks.
 *
 * systemd reads it together with pid_max to turn DefaultTasksMax=15% into a
 * number before it starts a unit (procfs_tasks_get_limit). A missing file is
 * ENOENT there, and the manager reports it as "Failed to run 'start' task: No
 * such file or directory" without ever forking -- so the unit never runs and
 * the message names no file. b1nix's ceiling is its task table. */
static int r_sys_threads_max(usize pid, struct sbuf *s) {
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

/* The mount table is sized from RAM now, so a buffer for it belongs on the heap
 * — MAX_MOUNTS entries is ~2 MiB, and this used to be a stack array. */
static int r_mounts(usize pid, struct sbuf *s) {
  (void)pid;
  usize cap = vfs_mount_capacity();
  struct b1nix_mount_entry *ents = kmalloc(cap * sizeof(*ents));

  if (!ents)
    return 0;
  isize n = vfs_mounts(ents, cap);
  for (isize i = 0; i < n; i++) {
    const char *src = ents[i].source[0] ? ents[i].source : "none";
    const char *tgt = ents[i].target[0] ? ents[i].target : "/";
    const char *fstype = ents[i].fstype[0] ? ents[i].fstype : "none";
    const char *opts = (ents[i].flags & B1NIX_MS_RDONLY) ? "ro" : "rw";
    /* device mountpoint fstype options dump pass */
    sb_addf(s, "%s %s %s %s 0 0\n", src, tgt, fstype, opts);
  }
  kfree(ents);
  return 0;
}

/* /proc/self/mountinfo — Linux mountinfo format, needed by BusyBox `lsblk`
 * (which xopen()s it and would abort if it were missing). The maj:min in
 * field 3 is the synthetic 8:<blk-index> used by /sys/block + /proc/partitions
 * for a `/dev/<blk>` source, else 0:<mount-index>. Layout:
 *   id parent maj:min root mountpoint opts - fstype source superopts */
static int r_mountinfo(usize pid, struct sbuf *s) {
  (void)pid;
  usize cap = vfs_mount_capacity();
  struct b1nix_mount_entry *ents = kmalloc(cap * sizeof(*ents));

  if (!ents)
    return 0;
  isize n = vfs_mounts(ents, cap);
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
  kfree(ents);
  return 0;
}


/* /proc/b1nix-prof — reading it prints the syscall and page-fault profiles to
 * the console and starts a fresh interval.
 *
 * The profiles were only printed by the periodic task dump, and that dump is
 * far too expensive to leave running while measuring: it writes thousands of
 * lines through the console lock and changes the very timings it is supposed
 * to report (a browser start-up that takes 95 s took 337 s with it on). A
 * reader that asks for the numbers when it wants them costs nothing in
 * between, so a script can take one sample per run and compare runs. */
static int r_b1nix_prof(usize pid, struct sbuf *s) {
  (void)pid;
  extern void syscall_prof_dump(void);
  extern void pf_prof_dump(void);
  extern void syscall_prof_reset(void);
  extern void kprof_dump(void);
  syscall_prof_dump();
  pf_prof_dump();
  kprof_dump();
  {
    extern void vfs_inode_wait_stats(u64 *, u64 *, u64 *, const void **);
    extern void vfs_inode_wait_reset(void);
    extern void ksym_print(u64 addr);
    u64 cyc = 0, n = 0, worst = 0;
    const void *site = 0;
    vfs_inode_wait_stats(&cyc, &n, &worst, &site);
    console_write("inode-wait: waits=");
    console_write_dec(n);
    console_write(" Mcycles=");
    console_write_dec(cyc / 1000000);
    console_write(" worst_Mcycles=");
    console_write_dec(worst / 1000000);
    console_write(" holder=0x");
    console_write_hex64((u64)(usize)site);
    ksym_print((u64)(usize)site);
    console_write("\n");
    vfs_inode_wait_reset();
  }
  syscall_prof_reset();
  sb_puts(s, "profile written to console\n");
  return 0;
}

/* /proc/b1nix-tasks — reading it prints the scheduler's task dump to the
 * console. The same dump b1nix.task-watch prints on a ten-second timer, but
 * asked for at a chosen moment instead: a harness that has just watched a
 * daemon stop answering can take one sample right then and see which syscall
 * every task is in, rather than paying for sixty dumps of a healthy machine to
 * catch the one that is not. */
static int r_b1nix_tasks(usize pid, struct sbuf *s) {
  extern void scheduler_dump_tasks(void);

  (void)pid;
  scheduler_dump_tasks();
  sb_puts(s, "task dump written to console\n");
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
  /* A process that renamed itself is listed under the name it chose. PR_SET_NAME
   * recorded one and nothing ever read it back, so systemd -- exec'd by the
   * kernel as /sbin/init and renamed to "systemd" straight away -- was still
   * reported as "init" by /proc/1/comm, which is the file every "which init is
   * this" check reads. */
  const char *chosen = t ? scheduler_comm_override(t->id) : 0;
  const char *name = chosen ? chosen : ((t && t->name) ? t->name : "?");
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
      /* Living threads only — same reason as /proc/<pid>/task: a count that
       * still includes threads which have exited never drops to one, and a
       * sandbox waits for exactly that before it will initialise. */
      if (o && o->id && task_tgid(o) == tgid && o->state != TASK_DEAD &&
          o->state != TASK_REAPING && o->state != TASK_UNUSED)
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
  if (!t)
    return 0;

  /* The recorded argument vector, NUL separators and all — that is the format
   * every reader of this file expects, and the separators are what let it be
   * split back into arguments. Falls back to the executable path for a task
   * that has none (a kernel thread, or one that never exec'd). */
  usize len = 0;
  const char *argv = task_cmdline(t, &len);
  if (argv && len) {
    for (usize i = 0; i < len; i++)
      sb_putc(s, argv[i]);
    return 0;
  }

  if (t->name)
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

/* /proc/<pid>/cgroup — the v2 line, "0::<path>".
 *
 * systemd asks this of itself before it does anything else: cg_pid_get_path()
 * reads it to learn which cgroup PID 1 is in, and an absent file is ESRCH,
 * which it reports as "Cannot determine cgroup we are running in" and freezes.
 * There is exactly one hierarchy here, so there is exactly one line. */
static int r_pid_cgroup(usize pid, struct sbuf *s) {
  char path[128];
  if (cgroup_path_of(pid, path, sizeof(path)) < 0)
    return -ESRCH;
  sb_addf(s, "0::%s\n", path);
  return 0;
}

/*
 * /proc/<pid>/oom_score_adj — the OOM-killer bias, recorded per process.
 *
 * systemd sets it on nearly every service it starts, and treats the write
 * failing as fatal to the spawn: every unit in this image died at
 * "Failed at step OOM_ADJUST" because the file was not there, which took
 * dbus, journald and systemd-udevd down with it.
 *
 * It is a stored tunable here and it is reported honestly as one: this kernel
 * has no OOM killer to bias — kmalloc panics rather than choosing a victim —
 * so the value is what userspace set and nothing acts on it yet. Recording it
 * in a side table keeps struct task the size it is.
 */
#define PROC_OOM_SLOTS 128
#define PROC_OOM_MIN (-1000)
#define PROC_OOM_MAX 1000

static struct {
  usize pid;
  int adj;
  u8 used;
} g_oom_adj[PROC_OOM_SLOTS];
static spinlock_t g_oom_lock;

static int proc_oom_adj_get(usize pid) {
  u64 flags;
  int v = 0;
  spin_lock_irqsave(&g_oom_lock, &flags);
  for (usize i = 0; i < PROC_OOM_SLOTS; i++) {
    if (g_oom_adj[i].used && g_oom_adj[i].pid == pid) {
      v = g_oom_adj[i].adj;
      break;
    }
  }
  spin_unlock_irqrestore(&g_oom_lock, flags);
  return v;
}

static void proc_oom_adj_set(usize pid, int adj) {
  u64 flags;
  usize free_slot = PROC_OOM_SLOTS;
  spin_lock_irqsave(&g_oom_lock, &flags);
  for (usize i = 0; i < PROC_OOM_SLOTS; i++) {
    if (g_oom_adj[i].used && g_oom_adj[i].pid == pid) {
      g_oom_adj[i].adj = adj;
      spin_unlock_irqrestore(&g_oom_lock, flags);
      return;
    }
    if (!g_oom_adj[i].used && free_slot == PROC_OOM_SLOTS)
      free_slot = i;
  }
  /* The default is 0, so a process that cannot get a slot reads back the
   * default rather than another process's value. */
  if (adj != 0 && free_slot < PROC_OOM_SLOTS) {
    g_oom_adj[free_slot].pid = pid;
    g_oom_adj[free_slot].adj = adj;
    g_oom_adj[free_slot].used = 1;
  }
  spin_unlock_irqrestore(&g_oom_lock, flags);
}

static int r_pid_oom_score_adj(usize pid, struct sbuf *s) {
  sb_addf(s, "%d\n", proc_oom_adj_get(pid));
  return 0;
}

static int w_pid_oom_score_adj(usize pid, const char *buf, usize len) {
  int sign = 1, v = 0;
  usize i = 0;
  while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
    i++;
  if (i < len && (buf[i] == '-' || buf[i] == '+')) {
    sign = buf[i] == '-' ? -1 : 1;
    i++;
  }
  if (i >= len || buf[i] < '0' || buf[i] > '9')
    return -EINVAL;
  for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++)
    v = v * 10 + (buf[i] - '0');
  v *= sign;
  if (v < PROC_OOM_MIN || v > PROC_OOM_MAX)
    return -EINVAL;
  proc_oom_adj_set(pid, v);
  return (int)len;
}

/* /proc/<pid>/oom_score — Linux derives it from the process's memory footprint
 * and the bias above. With no OOM killer there is no victim ranking to report,
 * so the bias is all this can honestly say. */
static int r_pid_oom_score(usize pid, struct sbuf *s) {
  int adj = proc_oom_adj_get(pid);
  int score = adj < 0 ? 0 : adj;
  sb_addf(s, "%d\n", score);
  return 0;
}

/* /proc/cgroups — the controllers this kernel has. Hierarchy 0 is what Linux
 * reports for a controller that is only available on the unified hierarchy. */
static int r_cgroups(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "#subsys_name\thierarchy\tnum_cgroups\tenabled\n");
  const char *c = cgroup_available_controllers();
  while (c && *c) {
    while (*c == ' ')
      c++;
    const char *start = c;
    while (*c && *c != ' ' && *c != '\n')
      c++;
    if (c > start) {
      char name[32];
      usize n = (usize)(c - start);
      if (n > sizeof(name) - 1)
        n = sizeof(name) - 1;
      memcpy(name, start, n);
      name[n] = '\0';
      sb_addf(s, "%s\t0\t1\t1\n", name);
    }
    if (*c == '\n')
      break;
  }
  return 0;
}

static int r_pid_stat(usize pid, struct sbuf *s) {
  struct task *t = scheduler_task_by_pid(pid);
  if (!t) {
    /* A task that exited between being listed and being read still owes the
     * reader a well-formed line. Four fields is not a shorter version of this
     * format: a reader that asks for field 23 of a 4-field line by index is
     * the crash that brought the browser down — Chromium walks every thread of
     * a process, and a thread that dies mid-walk landed here. Zombie state,
     * everything else zero, all 52 fields. */
    sb_addf(s,
            "%lu (gone) Z 0 0 0 0 -1 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 "
            "0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
            (unsigned long)pid);
    return 0;
  }
  u64 vsz = t->user_brk > t->heap_start ? t->user_brk - t->heap_start : 0;
  char comm[PROC_COMM_LEN];
  proc_comm(t, comm);
  /* Thread count for field 20 (num_threads) — a debugger reads it to know how
   * many /proc/<pid>/task entries to expect. */
  unsigned long nthreads = 0;
  {
    usize tgid = task_tgid(t);
    usize slots = scheduler_task_slots();
    for (usize i = 0; i < slots; i++) {
      struct task *o = scheduler_task_slot(i);
      /* Living threads only — same reason as /proc/<pid>/task: a count that
       * still includes threads which have exited never drops to one, and a
       * sandbox waits for exactly that before it will initialise. */
      if (o && o->id && task_tgid(o) == tgid && o->state != TASK_DEAD &&
          o->state != TASK_REAPING && o->state != TASK_UNUSED)
        nthreads++;
    }
    if (!nthreads)
      nthreads = 1;
  }
  /* Fields, in Linux order: pid comm state ppid pgrp session tty_nr tpgid flags
   * minflt cminflt majflt cmajflt utime stime cutime cstime priority nice
   * num_threads itrealvalue starttime vsize rss. M86 fills 14-17 (the CPU
   * times, in USER_HZ ticks — they were hard-coded zeros, so every `top`/`ps`
   * in the tree reported 0 % for every process), 20 and 22. */
  /* All 52 fields, because a reader counts them.
   *
   * This used to stop at 24. Chromium's /proc parser (ParseProcStats, then
   * ReadProcStatsAndGetFieldAsSizeT) reads a field by index and traps on a
   * line that is short — an illegal instruction, caught by its own crash
   * handler, re-entered on return, four hundred times in one run. A short
   * line is not a smaller version of this format; it is a different format.
   *
   * Fields b1nix does not track are zero, which is what Linux itself reports
   * for several of them, and the ones it does know (the CPU times, the thread
   * count, the memory bounds, the CPU last run on) carry real values. */
  sb_addf(s,
          /*  1- 4 */ "%lu (%s) %s %lu "
          /*  5- 9 */ "%lu %lu 0 -1 0 "
          /* 10-13 */ "0 0 0 0 "
          /* 14-17 */ "%lu %lu %lu %lu "
          /* 18-20 */ "%d 0 %lu "
          /* 21-24 */ "0 %lu %lu %lu "
          /* 25-27 */ "%lu %lu %lu "
          /* 28-31 */ "%lu 0 0 0 "
          /* 32-35 */ "0 0 0 0 "
          /* 36-38 */ "0 0 17 "
          /* 39-41 */ "%d 0 0 "
          /* 42-44 */ "0 0 0 "
          /* 45-48 */ "%lu %lu %lu 0 "
          /* 49-52 */ "0 0 0 0\n",
          (unsigned long)t->id, comm,
          scheduler_state_name((int)t->state), (unsigned long)t->parent_id,
          (unsigned long)t->process_group_id, (unsigned long)t->session_id,
          (unsigned long)task_utime(t), (unsigned long)task_stime(t),
          (unsigned long)task_cutime(t), (unsigned long)task_cstime(t),
          t->priority, nthreads, (unsigned long)task_start_ticks(t),
          (unsigned long)vsz, (unsigned long)(vsz / 4096),
          (unsigned long)~0UL,                    /* 25 rsslim: unlimited */
          (unsigned long)t->heap_start,           /* 26 startcode */
          (unsigned long)t->user_brk,             /* 27 endcode */
          0UL,                                    /* 28 startstack: unknown */
          0,                                      /* 39 processor: unknown */
          (unsigned long)t->heap_start,           /* 45 start_data */
          (unsigned long)t->user_brk,             /* 46 end_data */
          (unsigned long)t->user_brk);            /* 47 start_brk */
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
    if (t->pml4_phys)
      resident += paging_user_resident(t->pml4_phys, v->start, v->end);
  }
  /* The walk above already measured the resident set, so let the peak tracker
   * see it too (M86) — a `ps`/`top` poll then also refreshes ru_maxrss. */
  task_rss_sample(t, 0);
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
  /* Sized from the task, not from a guess.
   *
   * This was a 192-entry array on the kernel stack, which is two bugs at once:
   * ~9 KB of stack for a routine anyone can call, and a hard ceiling on how
   * much of an address space /proc/<pid>/maps will admit to. A browser maps
   * its binary, a hundred shared objects and an allocator's arenas — many
   * hundreds of regions — so the file it read back described someone else's
   * process. Crashpad, which re-reads maps until two passes agree, never got
   * two agreeing passes out of a truncated one and gave up with "retry count
   * exceeded"; anything else parsing it simply believed the short answer. */
  usize cap = 0;
  for (struct vm_area *v = t->vma_list; v; v = v->next)
    cap++;
  if (!cap)
    return 0;
  /* Room for the regions the loop below appends beyond the VMA list. */
  cap += 8;
  struct procfs_map_ent *m = kmalloc(cap * sizeof(*m));
  if (!m)
    return 0;
  usize n = 0;

  /* mmap'd regions (the only ones the VMA list tracks). */
  for (struct vm_area *v = t->vma_list; v && n < cap; v = v->next)
    procfs_maps_add(m, &n, cap, v->start, v->end, (int)v->prot,
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

  /* M107: name the two anonymous regions every reader asks about by name.
   * pmap prints them as [heap]/[stack] and `lsof` uses them to tell a mapping
   * of a file from the process's own memory; without the labels both showed
   * up as unattributed anonymous ranges. */
  if (img) {
    for (usize i = 0; i < n; i++) {
      if (m[i].name)
        continue;
      if (t->heap_start && t->user_brk > t->heap_start &&
          m[i].start >= (t->heap_start & ~(u64)(PAGE_SIZE - 1)) &&
          m[i].start < t->user_brk)
        m[i].name = "[heap]";
      else if (img->address_space.stack_top &&
               m[i].end <= img->address_space.stack_top &&
               m[i].end > img->address_space.stack_top - USER_STACK_MAX_SIZE)
        m[i].name = "[stack]";
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
  kfree(m);
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
struct procfs_fd_snap {
  int fd;
  char target[80];
  /* M107: a file-backed fd's full path cannot be rendered under fd_lock —
   * vfs_get_node_path() walks the mount table, which yields. The node is
   * referenced under the lock and resolved after it is dropped. */
  struct vfs_node *node;
};

/* The symlink target buffer, allocated once per descriptor number and then
 * rewritten in place. */
#define PROCFS_FD_TARGET_MAX 256

/* The magic-link half of /proc/<pid>/fd/<n>: what the descriptor holds right
 * now, resolved from the owner's live fd table rather than by re-walking the
 * path string this symlink reads back as.
 *
 * That distinction is the whole point of the file. systemd opens every mount
 * destination O_PATH|O_NOFOLLOW and then calls mount(2) on /proc/self/fd/<n>,
 * so that nothing can substitute a symlink for the destination in between --
 * a re-walk of the name gives that guarantee away, and gives the wrong answer
 * whenever the file's name has changed or it has none.
 *
 * A descriptor that holds no VFS node (a pipe, a socket, an eventfd) returns
 * NULL: the resolver then falls back to the "pipe:[7]"-style string, which is
 * exactly what it did before, so nothing that worked stops working. */
static struct vfs_node *procfs_fd_magic_link(struct vfs_node *link) {
  if (!link || !link->parent)
    return ERR_PTR(-ENOENT);
  int fd = 0;
  for (const char *q = link->name; *q; q++) {
    if (*q < '0' || *q > '9')
      return ERR_PTR(-ENOENT);
    fd = fd * 10 + (*q - '0');
  }
  usize pid = pid_from_parent(link->parent);
  struct task *t = scheduler_task_by_pid(pid);
  if (!t)
    return ERR_PTR(-ENOENT);
  struct vfs_node *out = 0;
  int open_fd = 0;
  u64 flags;
  spin_lock_irqsave(&t->fd_lock, &flags);
  struct vfs_handle *h =
      (t->fd_table && (usize)fd < t->fd_capacity) ? t->fd_table[fd] : 0;
  if (h && h->used) {
    open_fd = 1;
    if (h->kind == VFS_HANDLE_NODE && h->node)
      out = vfs_node_get(h->node);
  }
  spin_unlock_irqrestore(&t->fd_lock, flags);
  if (!open_fd)
    return ERR_PTR(-ENOENT); /* closed: Linux answers ENOENT too */
  return out;                /* NULL: no node behind it, use the string */
}

static void procfs_fd_symlink(struct vfs_node *dir, int fd, const char *target) {
  char name[16];
  snprintf(name, sizeof(name), "%d", fd);
  struct vfs_node *existing = find_child(dir, name);
  if (existing) {
    /*
     * Refresh it. A descriptor NUMBER is reused the moment the descriptor is
     * closed, so a symlink materialised once and never updated reports the
     * first file that fd ever held, for the life of the process. systemd
     * mounts every API filesystem by opening the mount point and passing
     * /proc/self/fd/<n> as the target — always the same small fd number — so a
     * frozen link put devtmpfs, tmpfs and cgroup2 all on top of whatever fd 4
     * was first, and PID 1 died reporting that the unified cgroup hierarchy
     * was not mounted.
     *
     * The buffer is rewritten in place rather than replaced: a reader may hold
     * the old pointer, and freeing it under them is a use-after-free. Writing
     * the terminator first means a concurrent readlink sees either the old
     * path or a prefix of the new one, never two paths run together.
     */
    existing->inode->magic_link_cb = procfs_fd_magic_link;
    char *buf = (char *)existing->inode->data;
    if (buf) {
      usize tl = strlen(target);
      if (tl > PROCFS_FD_TARGET_MAX - 1)
        tl = PROCFS_FD_TARGET_MAX - 1;
      buf[0] = '\0';
      memcpy(buf, target, tl);
      buf[tl] = '\0';
      existing->inode->size = tl;
    }
    return;
  }
  struct vfs_node *n = vfs_create_node(VFS_SYMLINK);
  if (!n)
    return;
  usize nl = strlen(name);
  memcpy(n->name, name, nl);
  n->name[nl] = '\0';
  char *dup = (char *)kmalloc(PROCFS_FD_TARGET_MAX);
  if (dup) {
    usize tl = strlen(target);
    if (tl > PROCFS_FD_TARGET_MAX - 1)
      tl = PROCFS_FD_TARGET_MAX - 1;
    memcpy(dup, target, tl);
    dup[tl] = '\0';
  }
  n->inode->data = dup;
  n->inode->size = dup ? strlen(dup) : 0;
  n->inode->mode = 0777;
  n->inode->nlink = 1;
  n->inode->magic_link_cb = procfs_fd_magic_link;
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
                                  usize sz, struct vfs_node **out_node) {
  if (out_node)
    *out_node = 0;
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
    /* The name the descriptor was opened under, which is the answer this file
     * is asked for. Deriving it from the node walks the parent chain and gives
     * the node's FIRST name, so a file opened through a bind mount was
     * reported at the other place -- and mount(2) on this link then recorded
     * the mount somewhere the caller was not looking. It also costs nothing
     * here: the string is already on the handle, so the path walk (which
     * yields, and so cannot run under fd_lock) is not needed at all. */
    if (h->open_path && h->open_path[0]) {
      snprintf(tg, sz, "%s", h->open_path);
    } else {
      /* The caller resolves the full path after dropping fd_lock (see
       * procfs_fd_resolve); the basename is the fallback when it cannot. */
      if (h->node && h->node->name[0])
        snprintf(tg, sz, "/%s", h->node->name);
      else
        snprintf(tg, sz, "anon_inode:[unknown]");
      if (out_node)
        *out_node = h->node ? vfs_node_get(h->node) : 0;
    }
    break;
  default:
    snprintf(tg, sz, "anon_inode:[unknown]");
    break;
  }
}

/* Turn a referenced node into the absolute path lsof/fuser/pmap expect, then
 * drop the reference. Must run with fd_lock released. */
static void procfs_fd_resolve(struct procfs_fd_snap *snap) {
  if (!snap->node)
    return;
  char path[80];
  if (vfs_get_node_path(snap->node, path, sizeof(path)) == 0 && path[0])
    snprintf(snap->target, sizeof(snap->target), "%s", path);
  vfs_node_put(snap->node);
  snap->node = 0;
}

static isize procfs_fd_readdir(struct vfs_node *dir, usize offset,
                               struct dirent *buf, usize max_entries) {
  usize pid = pid_from_parent(dir); /* dir->parent is the /proc/<pid> dir */
  struct task *t = scheduler_task_by_pid(pid);
  /* Only at the start of a read, for the same reason as task/ above: a listing
   * that changes between chunks has no stable end for the reader to reach. */
  if (t && offset == 0) {
    /* Sized from the task's fd table rather than capped at a constant.
     *
     * The snapshot used to be 64 entries on the kernel stack while the limit
     * on open descriptors is 1024, so a process past its 64th fd had the rest
     * of /proc/<pid>/fd silently omitted — and the readers that matter here
     * enumerate this directory to decide which descriptors to close before
     * exec. Allocation cannot happen under fd_lock, so the buffer is taken
     * first and the capacity re-checked once the lock is held. */
    usize want = t->fd_capacity; /* unlocked: a size hint, verified below */
    if (want < 8)
      want = 8;
    struct procfs_fd_snap *snap = kmalloc(want * sizeof(*snap));
    if (!snap)
      return vfs_readdir_children(dir, offset, buf, max_entries);
    usize nsnap = 0;
    u64 flags;
    spin_lock_irqsave(&t->fd_lock, &flags);
    usize cap = t->fd_capacity;
    if (cap > want)
      cap = want; /* the table grew after sizing; the next readdir catches up */
    for (usize i = 0; i < cap && nsnap < want; i++) {
      struct vfs_handle *h = t->fd_table ? t->fd_table[i] : 0;
      if (!h || !h->used)
        continue;
      snap[nsnap].fd = (int)i;
      procfs_fd_fill_target(h, (int)i, snap[nsnap].target,
                            sizeof(snap[nsnap].target), &snap[nsnap].node);
      nsnap++;
    }
    spin_unlock_irqrestore(&t->fd_lock, flags);
    /* Take out what is no longer open before putting in what is.
     *
     * This directory was only ever added to: an entry created for a descriptor
     * stayed after the descriptor was closed, and /proc/self is a single node
     * shared by every process, so entries also accumulated across processes
     * that had nothing to do with each other. A reader therefore saw
     * descriptors it did not hold — its own closed ones, and other processes'.
     *
     * That is not a cosmetic error. A program that launches helpers decides
     * which descriptors to close by enumerating exactly this directory, so a
     * listing with strangers in it makes it close numbers that mean something
     * else in this process. Chromium's zygote does this and then failed to
     * answer its parent, which is the shape that led here.
     *
     * Rebuilt from the calling task's table on every fresh read, which also
     * settles the shared-node problem: whatever the previous reader left is
     * removed before this reader's own descriptors are added. */
    {
      struct vfs_node *child = dir->first_child;
      while (child) {
        struct vfs_node *next = child->next_sibling;
        int numeric = child->name[0] != '\0';
        int fd = 0;

        for (const char *q = child->name; *q; q++) {
          if (*q < '0' || *q > '9') {
            numeric = 0;
            break;
          }
          fd = fd * 10 + (*q - '0');
        }

        if (numeric) {
          int still_open = 0;
          for (usize i = 0; i < nsnap; i++) {
            if (snap[i].fd == fd) {
              still_open = 1;
              break;
            }
          }
          if (!still_open) {
            vfs_detach_child(dir, child);
            vfs_node_put(child);
          }
        }
        child = next;
      }
    }

    for (usize i = 0; i < nsnap; i++) {
      procfs_fd_resolve(&snap[i]);
      procfs_fd_symlink(dir, snap[i].fd, snap[i].target);
    }
    kfree(snap);
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
  /* A single lookup has room for a whole path, unlike the readdir snapshot
   * (which holds one of these per open descriptor). The path is what mount(2)
   * records when it is handed this link, so truncating it here would move the
   * mount somewhere else. */
  char target[PROCFS_FD_TARGET_MAX];
  struct vfs_node *pathnode = 0;
  target[0] = '\0';
  int ok = 0;
  u64 flags;
  spin_lock_irqsave(&t->fd_lock, &flags);
  struct vfs_handle *h =
      (t->fd_table && (usize)fd < t->fd_capacity) ? t->fd_table[fd] : 0;
  if (h && h->used) {
    procfs_fd_fill_target(h, fd, target, sizeof(target), &pathnode);
    ok = 1;
  }
  spin_unlock_irqrestore(&t->fd_lock, flags);
  if (!ok)
    return -1;
  if (pathnode) {
    char path[PROCFS_FD_TARGET_MAX];
    if (vfs_get_node_path(pathnode, path, sizeof(path)) == 0 && path[0])
      snprintf(target, sizeof(target), "%s", path);
    vfs_node_put(pathnode);
  }
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

/* /proc/self — a SYMLINK to the caller's own pid directory, which is what
 * Linux has and what every user of this path assumes.
 *
 * It used to be a pid directory in its own right, shared by every process on
 * the system. That is fine for a file whose contents are rendered per caller,
 * and wrong for anything with state: /proc/<pid>/fd/<n> materialises one child
 * node per descriptor NUMBER and refreshes its target on lookup, so under
 * /proc/self those children were shared too. Two processes with different files
 * on the same descriptor number then had one node between them, and whichever
 * looked it up last decided what the other one saw. systemd performs every
 * mount by opening the destination and passing /proc/self/fd/<n> as the target
 * (mount_nofollow, so a symlink cannot be swapped in underneath it), always on
 * a small descriptor number — so a child setting up its unit root was handed
 * PID 1's last mount point instead of its own file, and the bind was refused
 * as a directory-onto-file mismatch. As a symlink there is nothing to share:
 * the path resolves into the caller's own /proc/<pid> tree.
 *
 * Rendered rather than stored, because the answer differs per reader. */
static isize procfs_self_readlink(struct vfs_node *node, u64 offset, char *buf,
                                  usize size, int flags) {
  (void)node;
  (void)offset;
  (void)flags;
  char num[24];
  usize len;

  /* Absolute, where Linux writes a bare pid. A relative target has to be
   * resolved against the directory the link sits in, and this resolver starts
   * from the root instead -- so "36" looked for /36, found nothing, and handed
   * back the root directory rather than an error. Naming the whole path costs
   * a reader nothing and removes the question. */
  snprintf(num, sizeof(num), "/proc/%lu", (unsigned long)scheduler_get_pid());
  len = strlen(num);
  if (len > size)
    len = size;
  memcpy(buf, num, len);
  return (isize)len;
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
  /* Rebuild the listing only when a read starts.
   *
   * This directory is read in chunks: the caller asks from offset 0, then from
   * where it left off. Re-synchronising on every one of those calls means the
   * entries move underneath the reader — threads appear, exit and are removed
   * between chunks — and a reader that never sees a stable end keeps asking.
   * That is what a browser hangs on here: its sandbox counts the entries in
   * /proc/self/task to decide whether it is single-threaded, and it counted
   * forever. Snapshot at the start, then serve the walk from it. */
  if (leader && offset == 0) {
    usize tgid = task_tgid(leader);
    usize slots = scheduler_task_slots();

    /* Retire the threads that have exited.
     *
     * procfs nodes are created once and left in place, which is harmless for
     * a directory whose contents only grow. This one shrinks: a thread that
     * exits must leave /proc/<pid>/task, because the count of entries here is
     * a process's answer to "am I single-threaded?" — and a sandbox that must
     * not be entered with other threads running asks exactly that before it
     * starts. Keeping the corpses made every process look permanently
     * multi-threaded, and the check fired in a child that had in fact reduced
     * itself to one thread. */
    struct vfs_node *child = dir->first_child;
    while (child) {
      struct vfs_node *next = child->next_sibling;
      usize tid = 0;
      int numeric = child->name[0] != '\0';

      for (const char *q = child->name; *q; q++) {
        if (*q < '0' || *q > '9') {
          numeric = 0;
          break;
        }
        tid = tid * 10 + (usize)(*q - '0');
      }
      if (numeric) {
        struct task *t = scheduler_task_by_pid(tid);
        /* Gone, or dead and merely not reaped yet — both must leave the
         * listing, or a caller counting threads never sees the count drop. */
        if (!t || !t->id || task_tgid(t) != tgid || t->state == TASK_DEAD ||
            t->state == TASK_REAPING || t->state == TASK_UNUSED) {
          vfs_detach_child(dir, child);
          vfs_node_put(child);
        }
      }
      child = next;
    }

    for (usize i = 0; i < slots; i++) {
      struct task *t = scheduler_task_slot(i);

      /* Living threads only.
       *
       * A thread that has exited keeps its slot until the reaper gets to it,
       * and listing those made the directory report threads that are gone.
       * That is not cosmetic: a sandbox stops every thread of a process and
       * then waits for /proc/self/task to show exactly one entry before it
       * will initialise. The corpses never left the listing, the count never
       * reached one, and the wait never ended — which is precisely where the
       * browser's GPU process was parked
       * (sandbox::ThreadHelpers::IsSingleThreaded, reached from
       * SandboxLinux::InitializeSandbox). */
      if (!t || !t->id || task_tgid(t) != tgid)
        continue;
      if (t->state == TASK_DEAD || t->state == TASK_REAPING ||
          t->state == TASK_UNUSED)
        continue;
      procfs_make_tiddir(dir, t->id);
    }
  }
  return vfs_readdir_children(dir, offset, buf, max_entries);
}

/* The link count of /proc/<pid>/task: "." plus ".." plus one per live thread.
 *
 * Chromium's sandbox does not count the entries here — it stats the directory
 * and reads st_nlink, refusing to go on with fewer than three
 * (sandbox/linux/services/thread_helpers.cc). A synthetic directory that
 * reports a stored 2 therefore claims to hold no threads at all, and the check
 * fires: the zygote takes an int3 the moment it is forked, and the browser it
 * left behind waits for a process that is already gone. Computed at stat time
 * because it changes with every clone and every thread exit. */
static void procfs_task_getattr(struct vfs_node *node) {
  usize pid = pid_from_parent(node);
  struct task *leader = scheduler_task_by_pid(pid);

  if (!leader) return;

  usize tgid = task_tgid(leader);
  usize slots = scheduler_task_slots();
  usize live = 0;

  for (usize i = 0; i < slots; i++) {
    struct task *t = scheduler_task_slot(i);

    if (!t || !t->id || task_tgid(t) != tgid)
      continue;
    /* Same rule the listing uses: a thread that has exited must not be
     * counted, whether or not anyone has reaped it yet. */
    if (t->state == TASK_DEAD || t->state == TASK_REAPING ||
        t->state == TASK_UNUSED)
      continue;
    live++;
  }
  if (live == 0) live = 1; /* the caller doing the stat is itself alive */
  node->inode->nlink = (int)(2 + live);
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

/* ── /proc/<pid>/ns/{uts,mnt,pid,net} (M109) ────────────────────────────────
 * The handles nsenter(1) opens and hands to setns(2). Linux makes these magic
 * symlinks whose text is "<kind>:[<inode>]"; b1nix makes them plain files with
 * the same text, so open() works without a target that has to exist, and
 * `readlink`-style comparison of two tasks' handles still tells you whether
 * they share a namespace. Kinds b1nix has only one of (pid, net) are here too
 * and always name the initial namespace — that is the truth, and nsenter needs
 * the file to exist to say so. */
static int r_pid_ns(usize pid, struct sbuf *s, int kind) {
  u32 id = namespace_id_of(pid, kind);
  /* Same shape as Linux's nsfs inode numbers, one range per kind. */
  u32 ino = 4026531835u + (u32)kind * 64u + id;
  sb_addf(s, "%s:[%u]\n", namespace_kind_name(kind), ino);
  return 0;
}

/* Pin the namespace the handle names at open(), the way Linux's nsfs does.
 * Resolving it again when setns(2) runs would follow the TASK, so a caller
 * could never hold a handle on the namespace it is about to leave — and
 * "join a namespace, then come back" is the whole of what nsenter does. */
static int procfs_ns_open_cb(struct vfs_node *node, struct vfs_handle *h) {
  struct procfs_node *pn = pn_of(node);
  usize pid = (pn && pn->pid) ? pn->pid : pid_from_parent(node);
  int kind = namespace_kind_from_name(node->name);
  if (kind < 0)
    return 0;
  h->ns_pin = VFS_NS_PIN_MAKE(kind, namespace_id_of(pid, kind));
  return 0;
}

static int r_pid_ns_uts(usize pid, struct sbuf *s) {
  return r_pid_ns(pid, s, NS_UTS);
}
static int r_pid_ns_mnt(usize pid, struct sbuf *s) {
  return r_pid_ns(pid, s, NS_MNT);
}
static int r_pid_ns_pid(usize pid, struct sbuf *s) {
  return r_pid_ns(pid, s, NS_PID);
}
static int r_pid_ns_net(usize pid, struct sbuf *s) {
  return r_pid_ns(pid, s, NS_NET);
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
  procfs_mkchild(d, "cgroup", VFS_DEVICE, r_pid_cgroup, pid);
  procfs_mkchild_writable_pid(d, "oom_score_adj", r_pid_oom_score_adj,
                              w_pid_oom_score_adj, pid);
  procfs_mkchild(d, "oom_score", VFS_DEVICE, r_pid_oom_score, pid);
  procfs_make_symlink(d, "cwd", procfs_cwd_readlink);
  procfs_make_symlink(d, "root", procfs_root_readlink);
  struct vfs_node *nsdir = procfs_mkchild(d, "ns", VFS_DIRECTORY, 0, pid);
  if (nsdir) {
    static const procfs_render ns_render[NS_KIND_COUNT] = {
        r_pid_ns_uts, r_pid_ns_mnt, r_pid_ns_pid, r_pid_ns_net};
    for (int k = 0; k < NS_KIND_COUNT; k++) {
      struct vfs_node *n = procfs_mkchild(nsdir, namespace_kind_name(k),
                                          VFS_DEVICE, ns_render[k], pid);
      if (n)
        n->inode->open_cb = procfs_ns_open_cb;
    }
  }
  struct vfs_node *fddir = procfs_mkchild(d, "fd", VFS_DIRECTORY, 0, 0);
  if (fddir) {
    fddir->inode->readdir_cb = procfs_fd_readdir;
    fddir->inode->lookup_cb = procfs_fd_lookup;
    /* An fd number is reused, so the symlink under it has to be re-rendered on
     * every lookup rather than materialised once. */
    fddir->inode->lookup_refresh = 1;
    /* procfs_fd_readdir ends in vfs_readdir_children, so the VFS must not
     * append the in-memory children a second time. */
    fddir->inode->readdir_lists_children = 1;
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
    taskdir->inode->getattr_cb = procfs_task_getattr;
    taskdir->inode->readdir_lists_children = 1;
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
 * digits in little-endian byte order. BusyBox `route` parses this. M84: the
 * rows are the real FIB (kernel/net/route.c), not a synthesised /24 + default
 * pair, so a manually added or non-/24 route shows up here. */
/* /proc/net/vlan/config — the file Linux's 802.1Q layer publishes, in the same
 * shape: a two-line header, then one line per VLAN device naming its VID and
 * the interface it is stacked on. */
static int r_net_vlan_config(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "VLAN Dev name\t | VLAN ID\nName-Type: VLAN_NAME_TYPE_RAW_PLUS_VID"
             "_NO_PAD\n");
  struct vlan_info v[8];
  usize n = vlan_snapshot(v, 8);
  for (usize i = 0; i < n; i++)
    sb_addf(s, "%s       | %u  | %s\n", v[i].name, (unsigned)v[i].vid,
            v[i].lower);
  return 0;
}

/* /proc/net/bridge — Linux keeps this in sysfs and in binary; here it is one
 * text table: a "port" row per member interface, then one row per learned
 * address with the port it was learned on and its age in seconds. */
static int r_net_bridge(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "bridge\tport\t\tmac\t\t\tage\n");
  /* 32 rows, like the route table's snapshot: this runs on a kernel stack. */
  struct bridge_fdb_info e[32];
  usize n = bridge_snapshot(e, 32);
  for (usize i = 0; i < n; i++) {
    if (e[i].is_port_row) {
      sb_addf(s, "%s\t%s\t\tport\t\t\t-\n", e[i].bridge, e[i].port);
      continue;
    }
    sb_addf(s, "%s\t%s\t\t%02x:%02x:%02x:%02x:%02x:%02x\t%u\n", e[i].bridge,
            e[i].port, e[i].mac[0], e[i].mac[1], e[i].mac[2], e[i].mac[3],
            e[i].mac[4], e[i].mac[5], (unsigned)(e[i].age_ticks / 100));
  }
  return 0;
}

/* /proc/net/bonding — one row per slave, with the active one marked. */
static int r_net_bonding(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "bond\tslave\t\tstate\n");
  struct bond_info b[8];
  usize n = bond_snapshot(b, 8);
  for (usize i = 0; i < n; i++)
    sb_addf(s, "%s\t%s\t\t%s\n", b[i].name,
            b[i].slave[0] ? b[i].slave : "-",
            b[i].slave[0] ? (b[i].active ? "active" : "backup") : "empty");
  return 0;
}

static int r_net_route(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask"
             "\t\tMTU\tWindow\tIRTT\n");
  struct route_info rt[32];
  usize n = route_snapshot(rt, 32);
  for (usize i = 0; i < n; i++) {
    /* Host-order u32 -> the little-endian hex Linux prints (least significant
     * byte first, i.e. the first octet of the dotted quad last). */
    sb_addf(s, "%s\t%02X%02X%02X%02X\t%02X%02X%02X%02X\t%04X\t0\t0\t%u"
               "\t%02X%02X%02X%02X\t0\t0\t0\n",
            rt[i].iface,
            (unsigned)(rt[i].dst & 0xFF), (unsigned)((rt[i].dst >> 8) & 0xFF),
            (unsigned)((rt[i].dst >> 16) & 0xFF), (unsigned)((rt[i].dst >> 24) & 0xFF),
            (unsigned)(rt[i].gateway & 0xFF), (unsigned)((rt[i].gateway >> 8) & 0xFF),
            (unsigned)((rt[i].gateway >> 16) & 0xFF),
            (unsigned)((rt[i].gateway >> 24) & 0xFF),
            (unsigned)rt[i].flags, (unsigned)rt[i].metric,
            (unsigned)(rt[i].mask & 0xFF), (unsigned)((rt[i].mask >> 8) & 0xFF),
            (unsigned)((rt[i].mask >> 16) & 0xFF),
            (unsigned)((rt[i].mask >> 24) & 0xFF));
  }
  return 0;
}

/* /proc/net/rt_tables — every route in every policy table, plus the write
 * interface that manages them (see kernel/net/route.c for the grammar). This
 * is b1nix's equivalent of `ip route add ... table N`: there is no rtnetlink,
 * so the control plane is text. */
static int r_net_rt_tables(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "# table\tprefix\tgateway\tdev\tmetric\tflags\n");
  struct route_info rt[64];
  usize n = route_snapshot(rt, 64);
  for (usize i = 0; i < n; i++) {
    int plen = 0;
    for (int b = 31; b >= 0; b--) {
      if (rt[i].mask & (1u << b))
        plen++;
    }
    sb_addf(s, "%u\t%u.%u.%u.%u/%d\t%u.%u.%u.%u\t%s\t%u\t%04X\n",
            (unsigned)rt[i].table, (unsigned)((rt[i].dst >> 24) & 0xFF),
            (unsigned)((rt[i].dst >> 16) & 0xFF),
            (unsigned)((rt[i].dst >> 8) & 0xFF), (unsigned)(rt[i].dst & 0xFF),
            plen, (unsigned)((rt[i].gateway >> 24) & 0xFF),
            (unsigned)((rt[i].gateway >> 16) & 0xFF),
            (unsigned)((rt[i].gateway >> 8) & 0xFF),
            (unsigned)(rt[i].gateway & 0xFF), rt[i].iface,
            (unsigned)rt[i].metric, (unsigned)rt[i].flags);
  }

  struct route6_info r6[64];
  usize n6 = route6_snapshot(r6, 64);
  for (usize i = 0; i < n6; i++) {
    sb_addf(s, "%u\t", (unsigned)r6[i].table);
    for (int b = 0; b < 16; b++)
      sb_addf(s, "%02x", (unsigned)r6[i].dst[b]);
    sb_addf(s, "/%u\t", (unsigned)r6[i].plen);
    for (int b = 0; b < 16; b++)
      sb_addf(s, "%02x", (unsigned)r6[i].gateway[b]);
    sb_addf(s, "\t%s\t%u\t%04X\n", r6[i].iface, (unsigned)r6[i].metric,
            (unsigned)r6[i].flags);
  }
  return 0;
}

static int w_net_rt_tables(usize pid, const char *buf, usize len) {
  (void)pid;
  if (route_control_write(buf, len) != 0)
    return -EINVAL;
  return (int)len;
}

/* /proc/net/rt_rules — the policy rules themselves (`ip rule` equivalent). */
static int r_net_rt_rules(usize pid, struct sbuf *s) {
  (void)pid;
  sb_puts(s, "# prio\tfamily\tfrom\tiif\ttable\n");
  struct route_rule_info ru[16];
  usize n = route_rule_snapshot(ru, 16);
  /* Ascending priority, the order the lookup consults them in. */
  for (usize pass = 0; pass < n; pass++) {
    usize best = n;
    for (usize i = 0; i < n; i++) {
      if (ru[i].prio == 0xFFFFFFFFu)
        continue;
      if (best == n || ru[i].prio < ru[best].prio)
        best = i;
    }
    if (best == n)
      break;
    char ifname[8] = "any";
    if (ru[best].iif)
      netdev_ifname(ru[best].iif, ifname, sizeof(ifname));
    if (ru[best].family == 6) {
      sb_addf(s, "%u\tinet6\t", (unsigned)ru[best].prio);
      if (ru[best].src6_plen) {
        for (int b = 0; b < 16; b++)
          sb_addf(s, "%02x", (unsigned)ru[best].src6[b]);
        sb_addf(s, "/%u", (unsigned)ru[best].src6_plen);
      } else {
        sb_puts(s, "all");
      }
      sb_addf(s, "\t%s\t%u\n", ifname, (unsigned)ru[best].table);
    } else {
      int plen = 0;
      for (int b = 31; b >= 0; b--) {
        if (ru[best].src_mask & (1u << b))
          plen++;
      }
      sb_addf(s, "%u\tinet\t", (unsigned)ru[best].prio);
      if (ru[best].src_mask)
        sb_addf(s, "%u.%u.%u.%u/%d", (unsigned)((ru[best].src >> 24) & 0xFF),
                (unsigned)((ru[best].src >> 16) & 0xFF),
                (unsigned)((ru[best].src >> 8) & 0xFF),
                (unsigned)(ru[best].src & 0xFF), plen);
      else
        sb_puts(s, "all");
      sb_addf(s, "\t%s\t%u\n", ifname, (unsigned)ru[best].table);
    }
    ru[best].prio = 0xFFFFFFFFu; /* consumed */
  }
  return 0;
}

static int w_net_rt_rules(usize pid, const char *buf, usize len) {
  (void)pid;
  if (route_rule_control_write(buf, len) != 0)
    return -EINVAL;
  return (int)len;
}

/* /proc/net/ipv6_route: Linux format — 32 hex digits of destination, prefix
 * length, 32 hex digits of source, source prefix length, 32 hex digits of the
 * next hop, metric, refcnt, use, flags, interface name. M84: rendered from the
 * real IPv6 FIB. */
static void sb_put_in6_hex(struct sbuf *s, const u8 *a) {
  for (int i = 0; i < 16; i++)
    sb_addf(s, "%02x", (unsigned)a[i]);
}

static int r_net_ipv6_route(usize pid, struct sbuf *s) {
  (void)pid;
  struct route6_info rt[32];
  usize n = route6_snapshot(rt, 32);
  u8 zero[16];
  memset(zero, 0, sizeof(zero));
  for (usize i = 0; i < n; i++) {
    sb_put_in6_hex(s, rt[i].dst);
    sb_addf(s, " %02x ", (unsigned)rt[i].plen);
    sb_put_in6_hex(s, zero);
    sb_puts(s, " 00 ");
    sb_put_in6_hex(s, rt[i].gateway);
    sb_addf(s, " %08x %08x %08x %08x %8s\n", (unsigned)rt[i].metric, 0u, 0u,
            (unsigned)rt[i].flags, rt[i].iface);
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
  /* Walk the registry rather than printing a hardcoded eth0: a bridge, a VLAN
   * or a veth is as much an interface as the NIC is, and netdev_by_index
   * answers only for the caller's network namespace, so this listing is
   * namespaced for free. */
  for (int idx = 1; idx <= (int)netdev_slot_count(); idx++) {
    struct netdev *nd = netdev_by_index(idx);
    if (!nd)
      continue;
    char name[16];
    netdev_ifname(idx, name, sizeof(name));
    sb_addf(s, "%6s:       0       0    0    0    0     0          0         0"
               "       0       0    0    0    0     0       0          0\n",
            name);
  }
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
  root->inode->readdir_lists_children = 1;
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
  procfs_mkchild(root, "cgroups", VFS_DEVICE, r_cgroups, 0);
  procfs_mkchild(root, "mounts", VFS_DEVICE, r_mounts, 0);
  procfs_mkchild(root, "cmdline", VFS_DEVICE, r_cmdline, 0);
  procfs_mkchild(root, "b1nix-prof", VFS_DEVICE, r_b1nix_prof, 0);
  procfs_mkchild(root, "b1nix-tasks", VFS_DEVICE, r_b1nix_tasks, 0);
  /* M107: /proc/kmsg — the same record stream as /dev/kmsg. klogd reads this
   * one and expects it to block until a message arrives. */
  {
    struct vfs_node *km = procfs_mkchild(root, "kmsg", VFS_DEVICE, 0, 0);
    if (km) {
      km->inode->read_cb = kmsg_proc_read;
      km->inode->poll_cb = kmsg_proc_poll;
      km->inode->open_cb = kmsg_proc_open;
      km->inode->mode = 0400;
    }
  }
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
      procfs_mkchild(kern, "cap_last_cap", VFS_DEVICE, r_sys_cap_last_cap, 0);
      procfs_mkchild(kern, "threads-max", VFS_DEVICE, r_sys_threads_max, 0);
      struct vfs_node *rnd =
          procfs_mkchild(kern, "random", VFS_DIRECTORY, 0, 0);
      if (rnd) {
        procfs_mkchild(rnd, "boot_id", VFS_DEVICE, r_sys_boot_id, 0);
        procfs_mkchild(rnd, "uuid", VFS_DEVICE, r_sys_random_uuid, 0);
        procfs_mkchild(rnd, "entropy_avail", VFS_DEVICE, r_sys_entropy_avail,
                       0);
      }
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
    procfs_mkchild(netd, "ipv6_route", VFS_DEVICE, r_net_ipv6_route, 0);
    procfs_mkchild_writable(netd, "rt_tables", r_net_rt_tables,
                            w_net_rt_tables);
    procfs_mkchild_writable(netd, "rt_rules", r_net_rt_rules, w_net_rt_rules);
    procfs_mkchild(netd, "dev", VFS_DEVICE, r_net_dev, 0);
    procfs_mkchild(netd, "bridge", VFS_DEVICE, r_net_bridge, 0);
    procfs_mkchild(netd, "bonding", VFS_DEVICE, r_net_bonding, 0);
    struct vfs_node *vland = procfs_mkchild(netd, "vlan", VFS_DIRECTORY, 0, 0);
    if (vland)
      procfs_mkchild(vland, "config", VFS_DEVICE, r_net_vlan_config, 0);
  }

  /* /proc/self — per-process view of the *calling* task (pid resolved at read
   * time via pid_from_parent → scheduler_get_pid). */
  procfs_make_symlink(root, "self", procfs_self_readlink);
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
