/* /dev/kmsg, /proc/kmsg and syslog(2) — M107.
 *
 * See b1nix/kmsg.h for the shape. The ring holds whole records; console output
 * is folded a line at a time so the transcript a user sees on the screen and
 * the records klogd reads are the same text, and a kernel caller can emit a
 * record with a real priority instead of an ASCII "<LEVEL>:" prefix.
 */

#include <b1nix/kmsg.h>
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

/* Read cursors. /dev/kmsg and /proc/kmsg are separate streams in Linux and are
 * kept separate here; each is a single shared cursor rather than a per-fd one,
 * which is exactly right for the one-reader case (klogd on /proc/kmsg,
 * syslogd on /dev/kmsg) and is documented as the limit it is. */
static u64 g_dev_cursor = 1;
static u64 g_proc_cursor = 1;

static int g_console_level = 7;
static int g_inited;

static u64 kmsg_now_usec(void) {
  /* The scheduler tick is 100 Hz; that is the real resolution of this clock
   * and the timestamps say so rather than pretending to microseconds. */
  return scheduler_get_uptime_ticks() * 10000ull;
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
      kmsg_store(KMSG_DEFAULT_PRIO, g_line, g_line_len);
      g_line_len = 0;
    }
    return;
  }
  if (g_line_len < KMSG_TEXT_MAX - 1) {
    g_line[g_line_len++] = ch;
    return;
  }
  /* An over-long line is closed where it overflows rather than truncated
   * away, so nothing is silently lost. */
  kmsg_store(KMSG_DEFAULT_PRIO, g_line, g_line_len);
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
  kmsg_store(prio, buf + off, len);
  /* Linux shows /dev/kmsg writes in dmesg too, and dmesg reads the klog
   * character transcript here — so mirror the text into it. */
  char line[KMSG_TEXT_MAX];
  usize n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
  memcpy(line, buf + off, n);
  line[n] = '\0';
  klog_info(line);
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
  case SYSLOG_ACTION_CONSOLE_OFF:
  case SYSLOG_ACTION_CONSOLE_ON:
    return 0;
  case SYSLOG_ACTION_CONSOLE_LEVEL:
    if (len < 1 || len > 8)
      return -EINVAL;
    g_console_level = len;
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
}

void kmsg_init(void) {
  if (g_inited)
    return;
  g_inited = 1;
  kmsg_register_nodes();
  kmsg_emit(6, "kmsg: structured kernel log ring online");
}
