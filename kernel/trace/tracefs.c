/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * /sys/kernel/tracing — the tree the tracing tools read.
 *
 * perf does not ask the kernel what it can trace; it reads this. `perf list`
 * walks `available_events`, and `perf record -e sched:sched_switch` reads
 * events/sched/sched_switch/id for the number it puts in
 * perf_event_attr.config, then writes `enable` to turn the site on. bpftrace
 * does the same. The layout is therefore not ours to choose: what is ours is
 * which events are in it, and that every one of them is real.
 *
 * Linux has this at /sys/kernel/tracing and, for older tools, at
 * /sys/kernel/debug/tracing. Both exist here, the second a symlink.
 */

#include <b1nix/tracepoint.h>

#include <b1nix/errno.h>
#include <b1nix/kprobe.h>
#include <b1nix/vfs.h>

#include <stdio.h>
#include <string.h>

/* A small ring of formatted hits, for `cat trace`. Not a substitute for the
 * perf ring buffer -- it is what a human reads when there is no tool. */
#define TRACE_RING_LINES 256
#define TRACE_LINE_MAX 96
static char g_ring[TRACE_RING_LINES][TRACE_LINE_MAX];
static u32 g_ring_head;   /* next line to write */
static u32 g_ring_count;  /* how many are valid */
static volatile int g_tracing_on = 1;

void tracefs_record(u16 id, u64 a, u64 b, u64 c) {
  struct b1nix_tracepoint *tp;
  u32 slot;

  if (!g_tracing_on)
    return;
  tp = tracepoint_by_id(id);
  if (!tp)
    return;
  slot = __atomic_fetch_add(&g_ring_head, 1, __ATOMIC_RELAXED) %
         TRACE_RING_LINES;
  snprintf(g_ring[slot], TRACE_LINE_MAX, "%s: %s: %s=0x%llx %s=0x%llx %s=0x%llx",
           tp->group, tp->name, tp->fields[0], (unsigned long long)a,
           tp->fields[1], (unsigned long long)b, tp->fields[2],
           (unsigned long long)c);
  if (g_ring_count < TRACE_RING_LINES)
    __atomic_add_fetch(&g_ring_count, 1, __ATOMIC_RELAXED);
}

/* ── the files ───────────────────────────────────────────────────────────── */

/* Every read here builds the whole answer and serves the caller's window of it,
 * which is what the pseudo-files elsewhere in this kernel do (see procfs). */
static isize tf_serve(u64 offset, char *out, usize size, const char *text) {
  usize len = strlen(text);

  if (offset >= len)
    return 0;
  if (size > len - offset)
    size = len - offset;
  memcpy(out, text + offset, size);
  return (isize)size;
}

static isize tf_read_available(struct vfs_node *node, u64 offset, char *out,
                               usize size, int flags) {
  char buf[2048];
  usize n = 0;

  (void)node;
  (void)flags;
  buf[0] = '\0';
  for (usize i = 0;; i++) {
    struct b1nix_tracepoint *tp = tracepoint_nth(i);

    if (!tp)
      break;
    n += (usize)snprintf(buf + n, sizeof(buf) - n, "%s:%s\n", tp->group,
                         tp->name);
    if (n >= sizeof(buf) - 64)
      break;
  }
  return tf_serve(offset, out, size, buf);
}

static isize tf_read_tracing_on(struct vfs_node *node, u64 offset, char *out,
                                usize size, int flags) {
  (void)node;
  (void)flags;
  return tf_serve(offset, out, size, g_tracing_on ? "1\n" : "0\n");
}

static isize tf_write_tracing_on(struct vfs_node *node, u64 offset,
                                 const char *in, usize size, int flags) {
  (void)node;
  (void)offset;
  (void)flags;
  if (size && (in[0] == '0'))
    g_tracing_on = 0;
  else if (size)
    g_tracing_on = 1;
  return (isize)size;
}

static isize tf_read_trace(struct vfs_node *node, u64 offset, char *out,
                           usize size, int flags) {
  /* The ring, oldest first, as one block of text. */
  char buf[TRACE_RING_LINES * 16];
  usize n = 0;
  u32 count = __atomic_load_n(&g_ring_count, __ATOMIC_RELAXED);
  u32 head = __atomic_load_n(&g_ring_head, __ATOMIC_RELAXED);

  (void)node;
  (void)flags;
  n += (usize)snprintf(buf + n, sizeof(buf) - n,
                       "# tracer: nop\n#\n# entries-in-buffer: %u\n",
                       (unsigned)count);
  for (u32 i = 0; i < count && n < sizeof(buf) - TRACE_LINE_MAX - 2; i++) {
    u32 slot = (head + TRACE_RING_LINES - count + i) % TRACE_RING_LINES;

    n += (usize)snprintf(buf + n, sizeof(buf) - n, "%s\n", g_ring[slot]);
  }
  return tf_serve(offset, out, size, buf);
}

/* events/<group>/<event>/id and /enable both hang off the tracepoint itself,
 * which the node carries in inode->data. */
static isize tf_read_id(struct vfs_node *node, u64 offset, char *out,
                        usize size, int flags) {
  struct b1nix_tracepoint *tp = node && node->inode
                                    ? (struct b1nix_tracepoint *)node->inode->data
                                    : 0;
  char buf[24];

  (void)flags;
  if (!tp)
    return -EIO;
  snprintf(buf, sizeof(buf), "%u\n", (unsigned)tp->id);
  return tf_serve(offset, out, size, buf);
}

static isize tf_read_enable(struct vfs_node *node, u64 offset, char *out,
                            usize size, int flags) {
  struct b1nix_tracepoint *tp = node && node->inode
                                    ? (struct b1nix_tracepoint *)node->inode->data
                                    : 0;
  (void)flags;
  if (!tp)
    return -EIO;
  return tf_serve(offset, out, size, tp->enabled ? "1\n" : "0\n");
}

static isize tf_write_enable(struct vfs_node *node, u64 offset, const char *in,
                             usize size, int flags) {
  struct b1nix_tracepoint *tp = node && node->inode
                                    ? (struct b1nix_tracepoint *)node->inode->data
                                    : 0;
  (void)offset;
  (void)flags;
  if (!tp)
    return -EIO;
  if (!size)
    return 0;
  {
    int rc = tracepoint_set_enabled(tp->id, in[0] != '0');

    if (rc < 0)
      return rc;
  }
  return (isize)size;
}

/* The `format` a tool reads to decode the payload. The three words this kernel
 * passes, named by the site. */
static isize tf_read_format(struct vfs_node *node, u64 offset, char *out,
                            usize size, int flags) {
  struct b1nix_tracepoint *tp = node && node->inode
                                    ? (struct b1nix_tracepoint *)node->inode->data
                                    : 0;
  char buf[512];

  (void)flags;
  if (!tp)
    return -EIO;
  snprintf(buf, sizeof(buf),
           "name: %s\nID: %u\nformat:\n"
           "\tfield:unsigned short common_type;\toffset:0;\tsize:2;\tsigned:0;\n"
           "\tfield:unsigned char common_flags;\toffset:2;\tsize:1;\tsigned:0;\n"
           "\tfield:unsigned char common_preempt_count;\toffset:3;\tsize:1;\tsigned:0;\n"
           "\tfield:int common_pid;\toffset:4;\tsize:4;\tsigned:1;\n"
           "\n"
           "\tfield:__u64 %s;\toffset:8;\tsize:8;\tsigned:0;\n"
           "\tfield:__u64 %s;\toffset:16;\tsize:8;\tsigned:0;\n"
           "\tfield:__u64 %s;\toffset:24;\tsize:8;\tsigned:0;\n"
           "\nprint fmt: \"%s=0x%%llx %s=0x%%llx %s=0x%%llx\", REC->%s, REC->%s, REC->%s\n",
           tp->name, (unsigned)tp->id, tp->fields[0], tp->fields[1],
           tp->fields[2], tp->fields[0], tp->fields[1], tp->fields[2],
           tp->fields[0], tp->fields[1], tp->fields[2]);
  return tf_serve(offset, out, size, buf);
}

/* kprobe_events: Linux's syntax, because that is what the tools write.
 *   p:name symbol      a probe at the symbol's entry
 *   r:name symbol      one at its return
 *   -:name             remove it
 */
static isize tf_read_kprobe_events(struct vfs_node *node, u64 offset, char *out,
                                   usize size, int flags) {
  char buf[1024];
  usize n = 0;

  (void)node;
  (void)flags;
  buf[0] = '\0';
  for (usize i = 0;; i++) {
    struct b1nix_tracepoint *tp = tracepoint_nth(i);

    if (!tp)
      break;
    if (tp->id < TP_KPROBE_BASE)
      continue;
    n += (usize)snprintf(buf + n, sizeof(buf) - n, "p:kprobes/%s %s\n",
                         tp->name, kprobe_symbol_of(tp->id));
    if (n >= sizeof(buf) - 96)
      break;
  }
  return tf_serve(offset, out, size, buf);
}

static isize tf_write_kprobe_events(struct vfs_node *node, u64 offset,
                                    const char *in, usize size, int flags) {
  char line[160];
  char name[48];
  char symbol[80];
  usize i = 0, j;
  int is_return = 0;

  (void)node;
  (void)offset;
  (void)flags;
  if (!size)
    return 0;
  if (size >= sizeof(line))
    size = sizeof(line) - 1;
  memcpy(line, in, size);
  line[size] = '\0';
  while (line[i] == ' ' || line[i] == '\t')
    i++;
  if (line[i] == '-') {
    /* -:name, or -:group/name */
    i++;
    if (line[i] == ':')
      i++;
    j = 0;
    while (line[i] && line[i] != '\n' && line[i] != ' ' && j < sizeof(name) - 1)
      name[j++] = line[i++];
    name[j] = '\0';
    {
      char *slash = strchr(name, '/');

      if (slash)
        memmove(name, slash + 1, strlen(slash + 1) + 1);
    }
    {
      int rc = tracepoint_kprobe_remove(name);

      return rc < 0 ? rc : (isize)size;
    }
  }
  if (line[i] == 'r')
    is_return = 1;
  else if (line[i] != 'p')
    return -EINVAL;
  i++;
  if (line[i] != ':')
    return -EINVAL;
  i++;
  j = 0;
  while (line[i] && line[i] != ' ' && line[i] != '\t' && line[i] != '\n' &&
         j < sizeof(name) - 1)
    name[j++] = line[i++];
  name[j] = '\0';
  {
    char *slash = strchr(name, '/');

    if (slash)
      memmove(name, slash + 1, strlen(slash + 1) + 1);
  }
  while (line[i] == ' ' || line[i] == '\t')
    i++;
  j = 0;
  while (line[i] && line[i] != ' ' && line[i] != '\t' && line[i] != '\n' &&
         j < sizeof(symbol) - 1)
    symbol[j++] = line[i++];
  symbol[j] = '\0';
  if (!name[0] || !symbol[0])
    return -EINVAL;
  {
    int rc = tracepoint_kprobe_add(name, symbol, is_return);

    if (rc < 0)
      return rc;
  }
  /* The event's files appear as soon as it exists, as they do on Linux. */
  tracefs_refresh_events();
  return (isize)size;
}

/* ── building the tree ───────────────────────────────────────────────────── */

static struct vfs_node *g_events_dir;

static struct vfs_node *tf_dir(const char *name, struct vfs_node *parent) {
  extern struct vfs_node *b1nix_debugfs_create_dir(const char *name,
                                                  struct vfs_node *parent);
  return b1nix_debugfs_create_dir(name, parent);
}

static struct vfs_node *tf_file(const char *name, u16 mode,
                                struct vfs_node *parent, void *data,
                                isize (*read_cb)(struct vfs_node *, u64, char *,
                                                 usize, int),
                                isize (*write_cb)(struct vfs_node *, u64,
                                                  const char *, usize, int)) {
  extern struct vfs_node *b1nix_debugfs_create_file(
      const char *name, u16 mode, struct vfs_node *parent, void *data,
      isize (*read_cb)(struct vfs_node *, u64, char *, usize, int));
  struct vfs_node *n = b1nix_debugfs_create_file(name, mode, parent, data,
                                                read_cb);

  if (n && n->inode)
    n->inode->write_cb = write_cb;
  return n;
}

/* One directory per event, created when the event exists. Called again when a
 * dynamic probe is added. */
void tracefs_refresh_events(void);

void tracefs_refresh_events(void) {
  if (!g_events_dir)
    return;
  for (usize i = 0;; i++) {
    struct b1nix_tracepoint *tp = tracepoint_nth(i);
    struct vfs_node *gdir, *edir;

    if (!tp)
      break;
    gdir = find_child(g_events_dir, tp->group);
    if (gdir)
      vfs_node_put(gdir); /* the tree holds it; we only asked */
    else
      gdir = tf_dir(tp->group, g_events_dir);
    if (!gdir)
      continue;
    {
      struct vfs_node *have = find_child(gdir, tp->name);

      if (have) {
        vfs_node_put(have);
        continue; /* already there */
      }
    }
    edir = tf_dir(tp->name, gdir);
    if (!edir)
      continue;
    tf_file("id", 0444, edir, tp, tf_read_id, 0);
    tf_file("enable", 0644, edir, tp, tf_read_enable, tf_write_enable);
    tf_file("format", 0444, edir, tp, tf_read_format, 0);
  }
}

void tracefs_init(void) {
  struct vfs_node *kernel_dir = vfs_find_node("/sys/kernel");
  struct vfs_node *root;

  if (!kernel_dir)
    return;
  root = tf_dir("tracing", kernel_dir);
  if (!root)
    return;
  tf_file("available_events", 0444, root, 0, tf_read_available, 0);
  tf_file("tracing_on", 0644, root, 0, tf_read_tracing_on, tf_write_tracing_on);
  tf_file("trace", 0444, root, 0, tf_read_trace, 0);
  tf_file("trace_pipe", 0444, root, 0, tf_read_trace, 0);
  tf_file("kprobe_events", 0644, root, 0, tf_read_kprobe_events,
          tf_write_kprobe_events);
  g_events_dir = tf_dir("events", root);
  tracefs_refresh_events();
  /* Older tools look under debugfs. One tree, two names. */
  vfs_symlink("/sys/kernel/tracing", "/sys/kernel/debug/tracing");
}
