/* /dev/kmsg, /proc/kmsg and syslog(2) — M107.
 *
 * See b1nix/kmsg.h for the shape. The ring holds whole records; console output
 * is folded a line at a time so the transcript a user sees on the screen and
 * the records klogd reads are the same text, and a kernel caller can emit a
 * record with a real priority instead of an ASCII "<LEVEL>:" prefix.
 */

#include <b1nix/console.h>
#include <b1nix/kmsg.h>
#include <b1nix/kprintf.h>
#include <b1nix/ktime.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/resource_caps.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <string.h>

#define KMSG_RECORDS 512
#define KMSG_TEXT_MAX 240

/* Default priority for a console line with no explicit level: KERN_INFO. */
#define KMSG_DEFAULT_PRIO 6

struct kmsg_record {
  u64 seq;
  u64 usec;
  u8 prio;
  u16 len;
  char text[KMSG_TEXT_MAX];
};

static struct kmsg_record g_ring[KMSG_RECORDS];
static u64 g_next_seq = 1; /* sequence of the next record to be written */
static u64 g_first_seq = 1; /* oldest sequence still in the ring */
static spinlock_t kmsg_lock = SPINLOCK_INIT;

/* Console line assembly. Touched only from console_putc, which already
 * serialises through the console lock for a whole string; a stray interleaved
 * character can only split a line, never corrupt the ring. */
static char g_line[KMSG_TEXT_MAX];
static usize g_line_len;

/* Read cursors. Every open() of /dev/kmsg or /proc/kmsg gets its own (see
 * kmsg_open_cb): reading a record must not consume it for the other readers,
 * which is what a shared cursor did — two `dmesg -w`s each saw half the log.
 * The two globals below survive only as the cursors syslog(2)/klogctl uses,
 * which is a single stream by definition, and as the fallback for a descriptor
 * that could not get its own. */
static u64 g_dev_cursor = 1;
static u64 g_proc_cursor = 1;

static int g_inited;

static u64 kmsg_now_usec(void) {
  /* The one monotonic clock: the same reading the console stamps a line with
   * and the same one /proc/uptime reports. */
  return ktime_monotonic_ns() / 1000ull;
}

/* Priority of the record currently being folded out of console characters. The
 * console line assembler sets it when it opens a line; it falls back to
 * KMSG_DEFAULT_PRIO for anything written without a severity. */
static int g_line_prio = KMSG_DEFAULT_PRIO;

void kmsg_set_line_prio(int priority) {
  g_line_prio = priority & 7;
}

static void kmsg_store(int prio, const char *text, usize len) {
  if (len > KMSG_TEXT_MAX - 1)
    len = KMSG_TEXT_MAX - 1;
  u64 flags;
  spin_lock_irqsave(&kmsg_lock, &flags);
  struct kmsg_record *r = &g_ring[g_next_seq % KMSG_RECORDS];
  r->seq = g_next_seq;
  r->usec = kmsg_now_usec();
  r->prio = (u8)(prio & 7);
  r->len = (u16)len;
  memcpy(r->text, text, len);
  r->text[len] = '\0';
  g_next_seq++;
  if (g_next_seq - g_first_seq > KMSG_RECORDS)
    g_first_seq = g_next_seq - KMSG_RECORDS;
  if (g_dev_cursor < g_first_seq)
    g_dev_cursor = g_first_seq;
  if (g_proc_cursor < g_first_seq)
    g_proc_cursor = g_first_seq;
  spin_unlock_irqrestore(&kmsg_lock, flags);
  /* console_putc runs from the very first line of boot, long before there is
   * a scheduler to wake anything on. Records are captured from that first
   * character (so dmesg keeps the whole boot transcript) but the wakeup only
   * happens once kmsg_init has run, which is well after the scheduler is up. */
  if (g_inited) {
    scheduler_wake_all(&g_next_seq);
    scheduler_wake_all(vfs_poll_chan);
  }
}

void kmsg_emit(int priority, const char *text) {
  if (!text)
    return;
  kmsg_store(priority, text, strlen(text));
}

void kmsg_putc(char ch) {
  if (ch == '\r' || ch == '\0')
    return;
  if (ch == '\n') {
    if (g_line_len) {
      kmsg_store(g_line_prio, g_line, g_line_len);
      g_line_len = 0;
    }
    g_line_prio = KMSG_DEFAULT_PRIO;
    return;
  }
  if (g_line_len < KMSG_TEXT_MAX - 1) {
    g_line[g_line_len++] = ch;
    return;
  }
  /* An over-long line is closed where it overflows rather than truncated
   * away, so nothing is silently lost. */
  kmsg_store(g_line_prio, g_line, g_line_len);
  g_line_len = 0;
  g_line[g_line_len++] = ch;
}

/* Render one record into `out` in the Linux /dev/kmsg format. */
static usize kmsg_format(const struct kmsg_record *r, char *out, usize cap) {
  int n = snprintf(out, cap, "%u,%lu,%lu,-;%s\n", (unsigned)r->prio,
                   (unsigned long)r->seq, (unsigned long)r->usec, r->text);
  if (n < 0)
    return 0;
  return (usize)n < cap ? (usize)n : cap - 1;
}

/* Copy the next record at/after *cursor into a kernel buffer. Returns the
 * byte count, or 0 when the cursor has caught up with the writer. */
static usize kmsg_next(u64 *cursor, char *out, usize cap) {
  u64 flags;
  usize n = 0;
  spin_lock_irqsave(&kmsg_lock, &flags);
  if (*cursor < g_first_seq)
    *cursor = g_first_seq;
  if (*cursor < g_next_seq) {
    struct kmsg_record snap = g_ring[*cursor % KMSG_RECORDS];
    (*cursor)++;
    spin_unlock_irqrestore(&kmsg_lock, flags);
    return kmsg_format(&snap, out, cap);
  }
  spin_unlock_irqrestore(&kmsg_lock, flags);
  return n;
}

/* Snapshot the tail of the ring, then print it later.
 *
 * For a board whose only console is its own panel. When the stall watchdog
 * fires, what everyone wants is the handful of lines printed just before the
 * silence — and those have already scrolled off the top, with no serial log and
 * no shell to run `dmesg` from.
 *
 * Why this is two calls and not one: the watchdog prints a long task dump, and
 * that dump goes into this same ring. Reading the tail afterwards therefore
 * replays the dump's own output and nothing else — which is exactly what the
 * single-call version did. So the capture happens before the dump and the
 * printing after it, which is also the order that leaves the interesting lines
 * at the bottom of the screen, where they survive.
 */
#define KMSG_TAIL_LINES 16
#define KMSG_TAIL_TEXT 128
static char g_tail[KMSG_TAIL_LINES][KMSG_TAIL_TEXT];
static unsigned g_tail_count;

void kmsg_capture_tail(void) {
  u64 flags, cursor, next, first;

  spin_lock_irqsave(&kmsg_lock, &flags);
  next = g_next_seq;
  first = g_first_seq;
  spin_unlock_irqrestore(&kmsg_lock, flags);

  cursor = (next > KMSG_TAIL_LINES) ? next - KMSG_TAIL_LINES : first;
  if (cursor < first)
    cursor = first;

  g_tail_count = 0;
  while (cursor < next && g_tail_count < KMSG_TAIL_LINES) {
    spin_lock_irqsave(&kmsg_lock, &flags);
    if (cursor < g_first_seq)
      cursor = g_first_seq;
    if (cursor >= g_next_seq) {
      spin_unlock_irqrestore(&kmsg_lock, flags);
      break;
    }
    /* Text only — the "6,123,456,-;" prefix /dev/kmsg readers parse costs a
     * third of a 45-column line here and says nothing a person needs. */
    strncpy(g_tail[g_tail_count], g_ring[cursor % KMSG_RECORDS].text,
            KMSG_TAIL_TEXT - 1);
    g_tail[g_tail_count][KMSG_TAIL_TEXT - 1] = '\0';
    g_tail_count++;
    cursor++;
    spin_unlock_irqrestore(&kmsg_lock, flags);
  }
}

void kmsg_print_captured(void) {
  console_write("--- last log lines before the stall ---\n");
  for (unsigned i = 0; i < g_tail_count; i++) {
    /* Quoted, not re-emitted.
     *
     * These lines are a replay of what the ring already holds, and printing
     * them bare put text into the log that is indistinguishable from live
     * kernel output -- three watchdog dumps turned one "net: e1000
     * administratively down" into four, three of them with no timestamp,
     * because the stored line does not carry the console's stamp. Anything
     * reading the log then sees kernel records that appear to have been
     * emitted without one. The marker says whose words these are. */
    console_write("| ");
    console_write(g_tail[i]);
    console_write("\n");
  }
}

static int kmsg_pending(u64 cursor) {
  u64 flags;
  int p;
  spin_lock_irqsave(&kmsg_lock, &flags);
  p = cursor < g_next_seq;
  spin_unlock_irqrestore(&kmsg_lock, flags);
  return p;
}

/* Shared read implementation: one record per call, blocking unless the caller
 * asked not to. */
static isize kmsg_read_common(u64 *cursor, char *buf, usize size, int flags) {
  if (size == 0)
    return 0;
  char line[KMSG_TEXT_MAX + 64];
  for (;;) {
    usize n = kmsg_next(cursor, line, sizeof(line));
    if (n) {
      if (n > size)
        n = size;
      memcpy(buf, line, n);
      return (isize)n;
    }
    if (flags & B1NIX_O_NONBLOCK)
      return -EAGAIN;
    scheduler_wait_prepare(&g_next_seq);
    if (kmsg_pending(*cursor)) {
      scheduler_wait_cancel();
      continue;
    }
    if (scheduler_signal_pending()) {
      scheduler_wait_cancel();
      return -ERESTARTSYS;
    }
    scheduler_wait_commit();
  }
}

/* ── Per-descriptor cursors ──
 * Both /dev/kmsg and /proc/kmsg hand every open its own position in the ring.
 * A new reader starts at the oldest record still held, which is what Linux's
 * /dev/kmsg does and what makes `dmesg` show the boot transcript. */
static isize kmsg_handle_read(struct vfs_handle *h, char *buf, usize len) {
  u64 *cursor = (u64 *)h->private_data;
  return kmsg_read_common(cursor ? cursor : &g_dev_cursor, buf, len, h->flags);
}

static isize kmsg_dev_write(struct vfs_node *node, u64 offset, const char *buf,
                            usize size, int flags);

static isize kmsg_handle_write(struct vfs_handle *h, const char *buf,
                               usize len) {
  if (!h->node)
    return -EBADF;
  return kmsg_dev_write(h->node, 0, buf, len, h->flags);
}

static int kmsg_handle_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  u64 *cursor = (u64 *)h->private_data;
  pfd->revents = B1NIX_POLLOUT;
  if (kmsg_pending(cursor ? *cursor : g_dev_cursor))
    pfd->revents |= B1NIX_POLLIN;
  return 0;
}

/* SEEK_SET(0) rewinds to the oldest record, SEEK_END(2) skips to the newest —
 * the two positions Linux defines for this file. The offset argument is a
 * position selector, not a byte count. */
static isize kmsg_handle_lseek(struct vfs_handle *h, isize offset, int whence) {
  u64 *cursor = (u64 *)h->private_data;
  if (!cursor)
    return -ESPIPE;
  u64 flags;
  spin_lock_irqsave(&kmsg_lock, &flags);
  if (whence == 0 || whence == 3 /* SEEK_DATA: first record, as on Linux */)
    *cursor = g_first_seq;
  else if (whence == 2)
    *cursor = g_next_seq;
  else {
    spin_unlock_irqrestore(&kmsg_lock, flags);
    return -EINVAL;
  }
  spin_unlock_irqrestore(&kmsg_lock, flags);
  (void)offset;
  return 0;
}

static void kmsg_handle_release(struct vfs_handle *h) {
  if (h->private_data) {
    kfree(h->private_data);
    h->private_data = 0;
  }
  if (h->node) {
    vfs_node_put(h->node);
    h->node = 0;
  }
}

static const struct vfs_file_ops kmsg_file_ops = {
    .read = kmsg_handle_read,
    .write = kmsg_handle_write,
    .poll = kmsg_handle_poll,
    .lseek = kmsg_handle_lseek,
    .release = kmsg_handle_release,
};

/* /proc/kmsg is read-only in Linux; writing to it is not a thing klogd does. */
static const struct vfs_file_ops kmsg_proc_file_ops = {
    .read = kmsg_handle_read,
    .poll = kmsg_handle_poll,
    .lseek = kmsg_handle_lseek,
    .release = kmsg_handle_release,
};

static int kmsg_open_common(struct vfs_handle *h,
                            const struct vfs_file_ops *ops) {
  u64 *cursor = kmalloc(sizeof(u64));
  if (!cursor)
    return -ENOMEM;
  u64 flags;
  spin_lock_irqsave(&kmsg_lock, &flags);
  *cursor = g_first_seq;
  spin_unlock_irqrestore(&kmsg_lock, flags);
  h->private_data = cursor;
  h->ops = ops;
  return 0;
}

static int kmsg_dev_open(struct vfs_node *node, struct vfs_handle *h) {
  (void)node;
  return kmsg_open_common(h, &kmsg_file_ops);
}

int kmsg_proc_open(struct vfs_node *node, struct vfs_handle *h) {
  (void)node;
  return kmsg_open_common(h, &kmsg_proc_file_ops);
}

static isize kmsg_dev_read(struct vfs_node *node, u64 offset, char *buf,
                           usize size, int flags) {
  (void)node;
  (void)offset;
  return kmsg_read_common(&g_dev_cursor, buf, size, flags);
}

/* Writing to /dev/kmsg injects a userspace message, honouring a leading
 * "<N>" priority the way the kernel's own writer does. */
static isize kmsg_dev_write(struct vfs_node *node, u64 offset, const char *buf,
                            usize size, int flags) {
  (void)node;
  (void)offset;
  (void)flags;
  if (size == 0)
    return 0;
  int prio = KMSG_DEFAULT_PRIO;
  usize off = 0;
  if (size >= 3 && buf[0] == '<') {
    usize i = 1;
    int v = 0, digits = 0;
    while (i < size && buf[i] >= '0' && buf[i] <= '9' && digits < 3) {
      v = v * 10 + (buf[i] - '0');
      i++;
      digits++;
    }
    if (digits && i < size && buf[i] == '>') {
      prio = v & 7;
      off = i + 1;
    }
  }
  usize len = size - off;
  while (len && (buf[off + len - 1] == '\n' || buf[off + len - 1] == '\r'))
    len--;
  /* One path, one record. The message is handed to the console with the
   * severity it asked for, and the console's line assembler is what creates
   * the ring record — the same way a kernel kprintf() line becomes one. An
   * earlier version stored the record here AND mirrored the text through
   * klog, which now also produces a record: every write landed in dmesg
   * twice, once at the requested priority and once at the default. */
  char line[KMSG_TEXT_MAX];
  usize n = len < sizeof(line) - 5 ? len : sizeof(line) - 5;
  line[0] = '<';
  line[1] = (char)('0' + (prio & 7));
  line[2] = '>';
  memcpy(line + 3, buf + off, n);
  line[3 + n] = '\n';
  line[4 + n] = '\0';
  console_write(line);
  return (isize)size;
}

static int kmsg_dev_poll(struct vfs_node *node, struct b1nix_pollfd *pfd) {
  (void)node;
  pfd->revents = B1NIX_POLLOUT;
  if (kmsg_pending(g_dev_cursor))
    pfd->revents |= B1NIX_POLLIN;
  return 0;
}

isize kmsg_proc_read(struct vfs_node *node, u64 offset, char *buf, usize size,
                     int flags) {
  (void)node;
  (void)offset;
  return kmsg_read_common(&g_proc_cursor, buf, size, flags);
}

int kmsg_proc_poll(struct vfs_node *node, struct b1nix_pollfd *pfd) {
  (void)node;
  pfd->revents = 0;
  if (kmsg_pending(g_proc_cursor))
    pfd->revents |= B1NIX_POLLIN;
  return 0;
}

/* ── syslog(2) / klogctl ────────────────────────────────────────────────── */

/* Render the whole ring, newest-last, into `out`; returns bytes produced. */
static usize kmsg_render_all(char *out, usize cap, int with_prefix) {
  usize total = 0;
  u64 flags;
  spin_lock_irqsave(&kmsg_lock, &flags);
  u64 first = g_first_seq, last = g_next_seq;
  spin_unlock_irqrestore(&kmsg_lock, flags);
  for (u64 s = first; s < last && total + 1 < cap; s++) {
    struct kmsg_record snap;
    spin_lock_irqsave(&kmsg_lock, &flags);
    if (s < g_first_seq) {
      spin_unlock_irqrestore(&kmsg_lock, flags);
      continue;
    }
    snap = g_ring[s % KMSG_RECORDS];
    spin_unlock_irqrestore(&kmsg_lock, flags);
    int n;
    if (with_prefix)
      n = snprintf(out + total, cap - total, "<%u>%s\n", (unsigned)snap.prio,
                   snap.text);
    else
      n = snprintf(out + total, cap - total, "%s\n", snap.text);
    if (n <= 0)
      break;
    if ((usize)n >= cap - total) {
      total = cap - 1;
      break;
    }
    total += (usize)n;
  }
  return total;
}

isize kmsg_syslog(int type, char *ubuf, int len) {
  switch (type) {
  case SYSLOG_ACTION_CLOSE:
  case SYSLOG_ACTION_OPEN:
    return 0;
  /* `dmesg -n` and friends: these move the same console filter the
   * `loglevel=` boot parameter sets, so the two agree. Linux's level is a
   * "print messages below this" bound, hence the -1. */
  case SYSLOG_ACTION_CONSOLE_OFF:
    console_loglevel_set(CONSOLE_LOGLEVEL_QUIET - 1);
    return 0;
  case SYSLOG_ACTION_CONSOLE_ON:
    console_loglevel_set(CONSOLE_LOGLEVEL_DEFAULT);
    return 0;
  case SYSLOG_ACTION_CONSOLE_LEVEL:
    if (len < 1 || len > 8)
      return -EINVAL;
    console_loglevel_set(len - 1);
    return 0;
  case SYSLOG_ACTION_SIZE_BUFFER:
    /* The size a caller must allocate to hold everything READ_ALL can hand
     * back, which is klog's character ring. */
    return (isize)KLOG_BUF_SIZE;
  case SYSLOG_ACTION_SIZE_UNREAD: {
    u64 flags;
    isize n;
    spin_lock_irqsave(&kmsg_lock, &flags);
    n = (isize)(g_next_seq - (g_proc_cursor < g_first_seq ? g_first_seq
                                                          : g_proc_cursor));
    spin_unlock_irqrestore(&kmsg_lock, flags);
    /* Report a byte estimate, which is what a caller sizes a buffer with. */
    return n * 80;
  }
  case SYSLOG_ACTION_CLEAR: {
    u64 flags;
    spin_lock_irqsave(&kmsg_lock, &flags);
    g_first_seq = g_next_seq;
    g_dev_cursor = g_next_seq;
    g_proc_cursor = g_next_seq;
    spin_unlock_irqrestore(&kmsg_lock, flags);
    return 0;
  }
  case SYSLOG_ACTION_READ:
  case SYSLOG_ACTION_READ_ALL:
  case SYSLOG_ACTION_READ_CLEAR:
    break;
  default:
    return -EINVAL;
  }

  if (!ubuf || len <= 0)
    return -EINVAL;
  usize cap = (usize)len;
  if (cap > KLOG_BUF_SIZE)
    cap = KLOG_BUF_SIZE;
  char *tmp = kmalloc(cap + 1);
  if (!tmp)
    return -ENOMEM;

  usize n;
  if (type == SYSLOG_ACTION_READ) {
    /* READ drains: only what the caller has not seen yet, one shot. This is
     * the record stream, so klogd gets whole messages with priorities. */
    n = 0;
    for (;;) {
      char line[KMSG_TEXT_MAX + 64];
      usize k = kmsg_next(&g_proc_cursor, line, sizeof(line));
      if (!k || n + k >= cap)
        break;
      memcpy(tmp + n, line, k);
      n += k;
    }
  } else {
    /* READ_ALL/READ_CLEAR mean "everything still in the kernel ring", which
     * for dmesg is the whole boot transcript. The record ring holds the most
     * recent KMSG_RECORDS messages; klog's 64 KiB character ring holds the
     * whole boot. Serve the transcript, and fall back to the records only if
     * it is empty. */
    n = klog_read(tmp, cap);
    if (n == 0)
      n = kmsg_render_all(tmp, cap + 1, 1);
  }

  isize rc = (isize)n;
  if (n && syscall_copyout(ubuf, tmp, n) < 0)
    rc = -EFAULT;
  kfree(tmp);

  if (rc >= 0 && type == SYSLOG_ACTION_READ_CLEAR) {
    u64 flags;
    spin_lock_irqsave(&kmsg_lock, &flags);
    g_first_seq = g_next_seq;
    g_dev_cursor = g_next_seq;
    g_proc_cursor = g_next_seq;
    spin_unlock_irqrestore(&kmsg_lock, flags);
  }
  return rc;
}

/* ── Node registration ──────────────────────────────────────────────────── */

void kmsg_register_nodes(void) {
  struct vfs_node *n = vfs_add_node("/dev/kmsg", VFS_DEVICE, 0, 0, 0);
  if (!n || IS_ERR(n))
    return;
  n->inode->mode = 0644;
  n->inode->read_cb = kmsg_dev_read;
  n->inode->write_cb = kmsg_dev_write;
  n->inode->poll_cb = kmsg_dev_poll;
  /* Per-descriptor cursor; the node-level callbacks above stay as the fallback
   * for a handle that never went through open_cb. */
  n->inode->open_cb = kmsg_dev_open;
}

void kmsg_init(void) {
  if (g_inited)
    return;
  g_inited = 1;
  kmsg_register_nodes();
  kmsg_emit(6, "kmsg: structured kernel log ring online");
}
