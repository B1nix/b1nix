/* M47 — evdev-style input event devices (/dev/input/event0, event1).
 *
 * event0 carries keyboard EV_KEY events (raw PS/2 set-1 scancodes), event1
 * carries mouse EV_REL/EV_ABS/BTN events. Producers are the PS/2 IRQ
 * handlers (and the timer-tick i8042 poll on real hardware), so every queue
 * is guarded by an irqsave spinlock.
 *
 * Like the serial ttys, opens are intercepted in vfs_open_flags and return
 * raw handles with custom file ops. Each open handle gets its own event
 * queue (evdev semantics: concurrent readers each see the full stream);
 * pushes fan out to all open clients and drop the oldest event on overflow.
 * Blocking reads use the stty yield-loop pattern (signal-interruptible);
 * blocking poll() is woken by input_event_push, like every other device that
 * feeds poll(). */
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/input.h>
#include <b1nix/ktime.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/vfs.h>
#include <b1nix/bootinfo.h>
#include <b1nix/syscall.h>
#include <b1nix/sysfs_attr.h>
#include <b1nix/uevent.h>
#include <b1nix/user.h>
#include <stdio.h>
#include <string.h>

#define INPUT_MAX_CLIENTS 4
#define INPUT_QUEUE_EVENTS 128

struct input_client {
  int used;
  int dev;
  struct b1nix_input_event ring[INPUT_QUEUE_EVENTS];
  u32 head; /* consumer index */
  u32 tail; /* producer index */
  u32 dropped;
};

struct input_device {
  const char *name;
  int registered;
  struct input_client *clients[INPUT_MAX_CLIENTS];
};

static struct input_device devs[INPUT_NDEVS] = {
  [INPUT_DEV_KBD] = {.name = "event0"},
  [INPUT_DEV_MOUSE] = {.name = "event1"},
  [INPUT_DEV_TOUCH] = {.name = "event2"},
};
static spinlock_t input_lock;

/* ── The Linux evdev view of these devices ──────────────────────────────
 *
 * libinput, and udev's input_id builtin before it, see a device through
 * three things: the sysfs capabilities/ files (what udev reads to say
 * ID_INPUT_MOUSE or ID_INPUT_KEYBOARD), the EVIOCG* ioctls (what libinput
 * asks the open descriptor), and 24-byte `struct input_event` records with a
 * timeval in front. b1nix's own readers get the 16-byte native record; a task
 * running under the Linux personality gets the Linux one. Nothing in the
 * event source changes.
 *
 * The absolute pointer (virtio-tablet) is described the way QEMU's usb-tablet
 * is on Linux: ABS_X/ABS_Y plus BTN_LEFT/RIGHT/MIDDLE and no BTN_TOUCH, which
 * udev classifies as ID_INPUT_MOUSE and libinput drives as a pointer with
 * absolute coordinates. The PS/2 mouse also publishes its cursor position as
 * ABS events for the kernel's own consumers; those are not part of a
 * relative mouse's contract and are left out of the Linux stream. */
#define EVDEV_BITS(n) (((n) + 63) / 64)
#define EV_MAX_BITS 0x20
#define KEY_MAX_BITS 0x300
#define REL_MAX_BITS 0x10
#define ABS_MAX_BITS 0x40
#define MSC_MAX_BITS 0x08
#define LED_MAX_BITS 0x10
#define SW_MAX_BITS 0x11
#define FF_MAX_BITS 0x80
#define PROP_MAX_BITS 0x20

#define LX_EV_SYN 0x00
#define LX_EV_KEY 0x01
#define LX_EV_REL 0x02
#define LX_EV_ABS 0x03
#define LX_EV_MSC 0x04
#define LX_EV_LED 0x11
#define LX_EV_REP 0x14
#define LX_REL_WHEEL 0x08
#define LX_MSC_SCAN 0x04
#define LX_BUS_I8042 0x11
#define LX_BUS_VIRTUAL 0x06

struct evdev_view {
  const char *name;
  u16 bustype, vendor, product, version;
  u64 ev[EVDEV_BITS(EV_MAX_BITS)];
  u64 key[EVDEV_BITS(KEY_MAX_BITS)];
  u64 rel[EVDEV_BITS(REL_MAX_BITS)];
  u64 abs[EVDEV_BITS(ABS_MAX_BITS)];
  u64 msc[EVDEV_BITS(MSC_MAX_BITS)];
  u64 led[EVDEV_BITS(LED_MAX_BITS)];
  i32 abs_max; /* ABS_X/ABS_Y range, when EV_ABS is offered */
};
static struct evdev_view g_view[INPUT_NDEVS];

static void view_set(u64 *bits, unsigned n) { bits[n / 64] |= 1ull << (n % 64); }
static int view_test(const u64 *bits, unsigned n) { return (bits[n / 64] >> (n % 64)) & 1; }

static void evdev_view_init(void) {
  struct evdev_view *k = &g_view[INPUT_DEV_KBD];
  k->name = "b1nix PS/2 Keyboard";
  k->bustype = LX_BUS_I8042; k->vendor = 1; k->product = 1; k->version = 0xab41;
  view_set(k->ev, LX_EV_SYN); view_set(k->ev, LX_EV_KEY); view_set(k->ev, LX_EV_MSC);
  view_set(k->ev, LX_EV_LED); view_set(k->ev, LX_EV_REP);
  for (unsigned c = 1; c <= 0xff; c++) view_set(k->key, c); /* the AT set */
  view_set(k->msc, LX_MSC_SCAN);
  for (unsigned l = 0; l < 3; l++) view_set(k->led, l);

  struct evdev_view *m = &g_view[INPUT_DEV_MOUSE];
  m->name = "b1nix PS/2 Mouse";
  m->bustype = LX_BUS_I8042; m->vendor = 2; m->product = 1; m->version = 0;
  view_set(m->ev, LX_EV_SYN); view_set(m->ev, LX_EV_KEY); view_set(m->ev, LX_EV_REL);
  view_set(m->key, B1NIX_BTN_LEFT); view_set(m->key, B1NIX_BTN_RIGHT); view_set(m->key, B1NIX_BTN_MIDDLE);
  view_set(m->rel, B1NIX_REL_X); view_set(m->rel, B1NIX_REL_Y); view_set(m->rel, LX_REL_WHEEL);

  struct evdev_view *t = &g_view[INPUT_DEV_TOUCH];
  t->name = "b1nix Absolute Pointer";
  t->bustype = LX_BUS_VIRTUAL; t->vendor = 0x627; t->product = 0x10; t->version = 1;
  view_set(t->ev, LX_EV_SYN); view_set(t->ev, LX_EV_KEY); view_set(t->ev, LX_EV_ABS);
  view_set(t->key, B1NIX_BTN_LEFT); view_set(t->key, B1NIX_BTN_RIGHT); view_set(t->key, B1NIX_BTN_MIDDLE);
  view_set(t->abs, B1NIX_ABS_X); view_set(t->abs, B1NIX_ABS_Y);
  t->abs_max = 32767;
}

/* A user program gets the 24-byte Linux record, and only the events the
 * device's capabilities announce; a kernel reader gets the b1nix record. */
static int linux_reader(void) {
  struct task *t = current_task;
  return t && t->user_image;
}

static int view_offers(int dev, u16 type, u16 code) {
  const struct evdev_view *v = &g_view[dev];
  if (type >= EV_MAX_BITS || !view_test(v->ev, type))
    return 0;
  switch (type) {
  case LX_EV_KEY: return code < KEY_MAX_BITS && view_test(v->key, code);
  case LX_EV_REL: return code < REL_MAX_BITS && view_test(v->rel, code);
  case LX_EV_ABS: return code < ABS_MAX_BITS && view_test(v->abs, code);
  default: return 1;
  }
}

struct linux_input_event {
  i64 tv_sec;
  i64 tv_usec;
  u16 type;
  u16 code;
  i32 value;
};

static u32 ring_next(u32 v) { return (v + 1) % INPUT_QUEUE_EVENTS; }

/* ── producer side ── */

/* Events that arrived, and how many found a reader.
 *
 * "The desktop is frozen" and "the desktop never heard the mouse" look
 * identical from in front of the monitor, and the difference decides where to
 * look next. QEMU's input-linux objects only forward the host's devices while
 * the grab is on, so a run with no events at all is a grab that was never
 * taken, not a stall. */
static u64 input_stat_pushed, input_stat_delivered, input_stat_dropped;

void input_event_counts(u64 *pushed, u64 *delivered, u64 *dropped) {
  if (pushed)
    *pushed = __atomic_load_n(&input_stat_pushed, __ATOMIC_RELAXED);
  if (delivered)
    *delivered = __atomic_load_n(&input_stat_delivered, __ATOMIC_RELAXED);
  if (dropped)
    *dropped = __atomic_load_n(&input_stat_dropped, __ATOMIC_RELAXED);
}

/* A frame at 120 Hz -- twice the panel's rate, so no frame can be built from a
 * stale position, and still eight times fewer wakes than a gaming mouse's
 * report rate produces. */
#define INPUT_MOTION_WAKE_MS 8
static u64 last_motion_wake_ms;
/* Devices with a report whose wake the spacing held back. Their queues are no
 * longer empty, so no later report would wake the reader either; the next
 * report or the timer tick pays it. */
static volatile u32 wake_owed;

static void input_wake_readers(int dev) {
  scheduler_wake_all(vfs_poll_chan);
  /* A blocking read() parks on the device, not on the poll channel. */
  scheduler_wake_all(&devs[dev]);
}

void input_tick(void) {
  u32 owed = __atomic_load_n(&wake_owed, __ATOMIC_RELAXED);

  if (!owed ||
      ktime_monotonic_ns() / 1000000ull - last_motion_wake_ms < INPUT_MOTION_WAKE_MS)
    return;
  owed = __atomic_exchange_n(&wake_owed, 0u, __ATOMIC_ACQ_REL);
  for (int dev = 0; dev < INPUT_NDEVS; dev++)
    if (owed & (1u << dev))
      input_wake_readers(dev);
}

void input_event_push(int dev, u16 type, u16 code, i32 value) {
  if (dev < 0 || dev >= INPUT_NDEVS || !devs[dev].registered)
    return;

  __atomic_fetch_add(&input_stat_pushed, 1u, __ATOMIC_RELAXED);

  struct b1nix_input_event ev;
  ev.time_ticks = scheduler_get_uptime_ticks();
  ev.type = type;
  ev.code = code;
  ev.value = value;

  /* A key or a button is a decision and must reach its reader now. Pointer
   * motion is not: the screen cannot show more than one position per frame,
   * and a client redraws once per frame however many times it was told the
   * pointer moved. So motion wakes are spaced, and everything else is not.
 */
  /* Per device, per report: whether it carries a key or a button, and whether
   * it found a client's queue empty. The wake decision is made at the SYN that
   * ends the report, when the report's own events already fill the queue, so
   * emptiness has to be remembered from its first event -- tested at the SYN
   * it was never true, and pointer motion woke nobody. */
  static int report_has_key[INPUT_NDEVS];
  static int report_found_empty[INPUT_NDEVS];
  int urgent;

  if (type == B1NIX_EV_KEY)
    report_has_key[dev] = 1;
  urgent = (type == B1NIX_EV_SYN) ? report_has_key[dev] : 0;
  if (type == B1NIX_EV_SYN)
    report_has_key[dev] = 0;
  u64 now_ms = ktime_monotonic_ns() / 1000000ull;
  u64 flags;
  int was_empty = 0;

  spin_lock_irqsave(&input_lock, &flags);
  for (int i = 0; i < INPUT_MAX_CLIENTS; i++) {
    struct input_client *c = devs[dev].clients[i];
    if (!c)
      continue;
    if (c->head == c->tail)
      was_empty = 1; /* this client has nothing pending: a wake will be needed */
    if (ring_next(c->tail) == c->head) { /* full: drop oldest */
      c->head = ring_next(c->head);
      c->dropped++;
      input_stat_dropped++;
    }
    c->ring[c->tail] = ev;
    c->tail = ring_next(c->tail);
    input_stat_delivered++;
  }
  spin_unlock_irqrestore(&input_lock, flags);

  /* Wake whoever is polling, once per finished report.
   *
   * Every other device that feeds poll() wakes on new data -- pipes, the DRM
   * event queue, the vts, kmsg, inotify -- and input did not: the comment at
   * the top of this file described a reader as being woken by "the periodic
   * vfs_poll_chan tick wake" instead. A compositor blocked in poll() on
   * /dev/input/eventN therefore slept until something unrelated happened to
   * wake that channel. Measured on the panel: 1713 events arrived in a second
   * of which 1351 were dropped for a full ring, with the machine 97% idle and
   * the screen not updating for three seconds.
   *
   * The wake belongs on EV_SYN and not on every event: a mouse in motion
   * sends an axis event per axis and then the SYN that ends the report, and a
   * reader cannot act on half a report.
   *
   * It also belongs only on the edge where a client's queue stops being empty.
   * b1nix parks every poller on one channel, so a wake is a rescan for all of
   * them; a client that already has unread events has a reader that is awake
   * or about to be, and waking again buys nothing. Without the edge, synthetic
   * motion at 1800 events a second cost 560 kernel ticks a second -- 0.3 ms of
   * kernel per event -- and held the desktop at 25 frames. Readiness is still
   * level-triggered: a poll that runs while the queue is non-empty returns
   * immediately, so no reader can be left asleep on data it has not seen.
   *
   * Outside the lock: scheduler_wake_all takes its own, and this one is held
   * by interrupt handlers. */
  if (was_empty)
    report_found_empty[dev] = 1;
  if (type != B1NIX_EV_SYN)
    return;
  was_empty = report_found_empty[dev];
  report_found_empty[dev] = 0;
  int owed = (__atomic_load_n(&wake_owed, __ATOMIC_RELAXED) >> dev) & 1;
  if (was_empty || urgent || owed) {
    if (!urgent && now_ms - last_motion_wake_ms < INPUT_MOTION_WAKE_MS) {
      __atomic_fetch_or(&wake_owed, 1u << dev, __ATOMIC_ACQ_REL);
      return;
    }
    __atomic_fetch_and(&wake_owed, ~(1u << dev), __ATOMIC_ACQ_REL);
    if (!urgent)
      last_motion_wake_ms = now_ms;
    input_wake_readers(dev);
  }
}

void input_event_sync(int dev) {
  input_event_push(dev, B1NIX_EV_SYN, 0, 0);
}

void input_kbd_scancode(u8 scancode, int extended) {
  u16 code = (u16)((extended ? 0xE000u : 0u) | (scancode & 0x7Fu));
  input_event_push(INPUT_DEV_KBD, B1NIX_EV_KEY, code,
                   (scancode & 0x80u) ? 0 : 1);
  input_event_sync(INPUT_DEV_KBD);
}

/* Bumped by every successful open(). Consumers that must react once per open
 * (the M47 burst injector) watch this instead of the client count: a reader
 * that closes and immediately reopens never presents an observable zero-client
 * window, so has_clients() edge detection misses the second open. */
static u32 dev_open_seq[INPUT_NDEVS];

static u32 input_dev_open_seq(int dev) {
  if (dev < 0 || dev >= INPUT_NDEVS)
    return 0;
  return __atomic_load_n(&dev_open_seq[dev], __ATOMIC_ACQUIRE);
}

/* The event index (0=kbd,1=mouse,2=touch) behind an input handle, or -1.
 * fstat(2) on the fd needs it to report the char-device number 13:64+idx;
 * libinput fstats every device it opens and treats a failure as "not a
 * device", so a mouse fd that could not be fstat'd was invisible to it. */
int input_handle_index(struct vfs_handle *h) {
  struct input_client *c = h ? (struct input_client *)h->private_data : 0;
  return c ? c->dev : -1;
}

int input_dev_has_clients(int dev) {
  if (dev < 0 || dev >= INPUT_NDEVS)
    return 0;
  for (int i = 0; i < INPUT_MAX_CLIENTS; i++) {
    if (devs[dev].clients[i])
      return 1;
  }
  return 0;
}

/* ── file ops ── */

static isize input_read(struct vfs_handle *h, char *buf, usize size) {
  struct input_client *c = (struct input_client *)h->private_data;
  if (!c) return -EINVAL;
  int lx = linux_reader();
  usize rec = lx ? sizeof(struct linux_input_event) : sizeof(struct b1nix_input_event);
  if (size < rec)
    return -EINVAL;
  for (;;) {
    usize n = 0;
    u64 irqf;
    spin_lock_irqsave(&input_lock, &irqf);
    while (c->head != c->tail && n + rec <= size) {
      const struct b1nix_input_event *e = &c->ring[c->head];
      c->head = ring_next(c->head);
      if (!lx) {
        memcpy(buf + n, e, sizeof(*e));
        n += rec;
        continue;
      }
      if (!view_offers(c->dev, e->type, e->code))
        continue; /* not part of this device's Linux contract */
      struct linux_input_event le;
      u32 hz = sched_tick_hz() ? sched_tick_hz() : 100;
      le.tv_sec = (i64)(e->time_ticks / hz);
      le.tv_usec = (i64)((e->time_ticks % hz) * (1000000ull / hz));
      le.type = e->type;
      le.code = e->code;
      le.value = e->value;
      memcpy(buf + n, &le, sizeof(le));
      n += rec;
    }
    spin_unlock_irqrestore(&input_lock, irqf);
    if (n > 0)
      return (isize)n;
    if (h->flags & B1NIX_O_NONBLOCK)
      return -EAGAIN;
    if (scheduler_signal_pending())
      return -EINTR;
    scheduler_wait_prepare(&devs[c->dev]);
    if (c->head != c->tail) {
      scheduler_wait_cancel();
      continue;
    }
    scheduler_wait_commit();
  }
}

/* Event injection, same shape as Linux evdev: writing whole
 * struct b1nix_input_event records to /dev/input/eventN feeds them into that
 * device's stream, so every client reading it (a compositor, a browser's fbtk
 * loop) sees them as ordinary input. The node is 0600, so only root can do
 * this. This is what lets a userspace test drive an interactive app — the
 * kernel-side injector threads only ever produced one hard-coded burst. */
static isize input_write(struct vfs_handle *h, const char *buf, usize len) {
  struct input_client *c = (struct input_client *)h->private_data;
  if (!c)
    return -EINVAL;
  /* The record size matches the reader: a user program writes the 24-byte
   * struct input_event, a kernel writer the 16-byte b1nix record. Only
   * type/code/value are used; the kernel stamps the time. Reading one size
   * and writing another (input_read converts, input_write did not) meant a
   * Linux program's event injection -- what a uinput-style writer or a test
   * does -- was rejected with EINVAL for a length that was never a multiple
   * of the native size. */
  int lx = linux_reader();
  usize rec = lx ? sizeof(struct linux_input_event) : sizeof(struct b1nix_input_event);
  if (len == 0 || (len % rec) != 0)
    return -EINVAL;
  usize count = len / rec;
  for (usize i = 0; i < count; i++) {
    u16 type, code; i32 value;
    if (lx) {
      struct linux_input_event le;
      memcpy(&le, buf + i * rec, sizeof(le));
      type = le.type; code = le.code; value = le.value;
    } else {
      struct b1nix_input_event ev;
      memcpy(&ev, buf + i * rec, sizeof(ev));
      type = ev.type; code = ev.code; value = ev.value;
    }
    input_event_push(c->dev, type, code, value);
  }
  return (isize)len;
}

static int input_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct input_client *c = (struct input_client *)h->private_data;
  pfd->revents = 0;
  if (!c)
    return 0;
  u64 flags;
  spin_lock_irqsave(&input_lock, &flags);
  if (c->head != c->tail)
    pfd->revents |= B1NIX_POLLIN;
  spin_unlock_irqrestore(&input_lock, flags);
  return 0;
}

static void input_release(struct vfs_handle *h) {
  struct input_client *c = (struct input_client *)h->private_data;
  if (!c)
    return;
  u64 flags;
  spin_lock_irqsave(&input_lock, &flags);
  if (c->dev >= 0 && c->dev < INPUT_NDEVS) {
    for (int i = 0; i < INPUT_MAX_CLIENTS; i++) {
      if (devs[c->dev].clients[i] == c)
        devs[c->dev].clients[i] = 0;
    }
  }
  spin_unlock_irqrestore(&input_lock, flags);
  kfree(c);
  h->private_data = 0;
}

/* The EVIOCG* ioctls libinput and libevdev ask of an event device. The
 * request encodes (dir, 'E', nr, size); the size is the caller's buffer and
 * the answer is truncated to it, as Linux does. Writes that carry no meaning
 * here -- grab, revoke, clock, key repeat -- succeed. */
#define EVIOC_TYPE(r) (((r) >> 8) & 0xff)
#define EVIOC_NR(r) ((r) & 0xff)
#define EVIOC_SIZE(r) (((r) >> 16) & 0x3fff)
#define EVIOC_DIR(r) (((r) >> 30) & 3)
struct linux_input_id { u16 bustype, vendor, product, version; };
struct linux_input_absinfo { i32 value, minimum, maximum, fuzz, flat, resolution; };

static int evdev_put(void *arg, const void *src, usize have, usize want) {
  usize n = have < want ? have : want;
  if (n && syscall_copyout(arg, src, n) < 0)
    return -EFAULT;
  return (int)n;
}

static int input_ioctl_impl(struct vfs_handle *h, u64 request, void *arg);
static int input_ioctl(struct vfs_handle *h, u64 request, void *arg) {
  int rc = input_ioctl_impl(h, request, arg);
  if (bootinfo_has_flag("b1nix.trace-evioc")) {
    static unsigned n;
    if (n < 200) {
      n++;
      char line[96];
      snprintf(line, sizeof(line),
               "evioc: dir=%u type=%c nr=0x%02x size=%u rc=%d by %s\n",
               (unsigned)EVIOC_DIR(request), (char)EVIOC_TYPE(request),
               (unsigned)EVIOC_NR(request), (unsigned)EVIOC_SIZE(request), rc,
               current_task && current_task->name ? current_task->name : "?");
      console_write(line);
    }
  }
  return rc;
}
static int input_ioctl_impl(struct vfs_handle *h, u64 request, void *arg) {
  struct input_client *c = (struct input_client *)h->private_data;
  if (!c || EVIOC_TYPE(request) != 'E')
    return -ENOTTY;
  const struct evdev_view *v = &g_view[c->dev];
  unsigned nr = EVIOC_NR(request);
  usize len = EVIOC_SIZE(request);
  if (nr >= 0x20 && nr < 0x40) { /* EVIOCGBIT(ev, len) */
    unsigned ev = nr - 0x20;
    const u64 *bits = 0; usize bytes = 0;
    switch (ev) {
    case 0: bits = v->ev; bytes = sizeof(v->ev); break;
    case LX_EV_KEY: bits = v->key; bytes = sizeof(v->key); break;
    case LX_EV_REL: bits = v->rel; bytes = sizeof(v->rel); break;
    case LX_EV_ABS: bits = v->abs; bytes = sizeof(v->abs); break;
    case LX_EV_MSC: bits = v->msc; bytes = sizeof(v->msc); break;
    case LX_EV_LED: bits = v->led; bytes = sizeof(v->led); break;
    default: break;
    }
    if (!bits) {
      static const u64 none[EVDEV_BITS(FF_MAX_BITS)];
      bits = none; bytes = sizeof(none);
    }
    return evdev_put(arg, bits, bytes, len);
  }
  if (nr >= 0x40 && nr < 0x80) { /* EVIOCGABS(axis) */
    unsigned axis = nr - 0x40;
    struct linux_input_absinfo ai = {0, 0, 0, 0, 0, 0};
    if (axis < ABS_MAX_BITS && view_test(v->abs, axis))
      ai.maximum = v->abs_max;
    return evdev_put(arg, &ai, sizeof(ai), len) < 0 ? -EFAULT : 0;
  }
  if (nr >= 0xc0) /* EVIOCSABS */
    return 0;
  switch (nr) {
  case 0x01: { u32 ver = 0x010001; return evdev_put(arg, &ver, sizeof(ver), len) < 0 ? -EFAULT : 0; }
  case 0x02: { struct linux_input_id id = {v->bustype, v->vendor, v->product, v->version};
               return evdev_put(arg, &id, sizeof(id), len) < 0 ? -EFAULT : 0; }
  case 0x03: { u32 rep[2] = {250, 33}; return evdev_put(arg, rep, sizeof(rep), len) < 0 ? -EFAULT : 0; }
  case 0x06: { /* EVIOCGNAME */
    usize l = strlen(v->name) + 1;
    return evdev_put(arg, v->name, l, len);
  }
  case 0x07: case 0x08: /* PHYS, UNIQ: none */
    return -ENOENT;
  case 0x09: { static const u64 props[EVDEV_BITS(PROP_MAX_BITS)]; return evdev_put(arg, props, sizeof(props), len); }
  case 0x0a: /* EVIOCGMTSLOTS */
    return -EINVAL;
  case 0x18: case 0x19: case 0x1a: case 0x1b: { /* current KEY/LED/SND/SW state: nothing held */
    static const u64 zero[EVDEV_BITS(KEY_MAX_BITS)]; return evdev_put(arg, zero, sizeof(zero), len);
  }
  case 0x90: case 0x91: case 0xa0: case 0x93: /* GRAB, REVOKE, SCLOCKID, SMASK */
    return 0;
  case 0x92: /* EVIOCGMASK */
    return -EINVAL;
  default:
    return -ENOTTY;
  }
}

static const struct vfs_file_ops input_ops = {
  .read = input_read,
  .ioctl = input_ioctl,
  .write = input_write,
  .poll = input_poll,
  .release = input_release,
};

/* ── open path ── */

int input_path_index(const char *resolved_path) {
  if (strncmp(resolved_path, "/dev/input/event", 16) != 0)
    return -1;
  const char *num = resolved_path + 16;
  if (*num < '0' || *num > '9')
    return -1;
  int idx = 0;
  for (const char *q = num; *q; q++) {
    if (*q < '0' || *q > '9')
      return -1;
    idx = idx * 10 + (*q - '0');
  }
  return idx < INPUT_NDEVS ? idx : -1;
}

int input_dev_open(int idx, int flags) {
  if (idx < 0 || idx >= INPUT_NDEVS || !devs[idx].registered)
    return -ENXIO;


  struct input_client *c = (struct input_client *)kzalloc(sizeof(*c));
  if (!c)
    return -ENOMEM;
  c->used = 1;
  c->dev = idx;

  u64 irqf;
  spin_lock_irqsave(&input_lock, &irqf);
  int slot = -1;
  for (int i = 0; i < INPUT_MAX_CLIENTS; i++) {
    if (!devs[idx].clients[i]) {
      slot = i;
      break;
    }
  }
  if (slot >= 0)
    devs[idx].clients[slot] = c;
  spin_unlock_irqrestore(&input_lock, irqf);
  if (slot < 0) {
    kfree(c);
    return -EBUSY;
  }

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_INPUT);
  if (!h) {
    spin_lock_irqsave(&input_lock, &irqf);
    devs[idx].clients[slot] = 0;
    spin_unlock_irqrestore(&input_lock, irqf);
    kfree(c);
    return -ENFILE;
  }
  h->private_data = c;
  h->ops = &input_ops;
  h->flags = flags;

  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h); /* release() detaches the client + frees it */
    return -EMFILE;
  }
  if (flags & B1NIX_O_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  __atomic_add_fetch(&dev_open_seq[idx], 1, __ATOMIC_RELEASE);
  /* Tell the M47 injector an open happened, instead of leaving it to notice on
   * its next poll. */
  scheduler_wake_all(&dev_open_seq[0]);
  return fd;
}

/* ── M47 diagnostic: synthetic window-drag injector ───────────────────────
 * Headless reproduction of the interactive "drag a window" path (the smoke
 * suite otherwise never drags). Enabled with b1nix.gfxtest=1 on a runlevel-5
 * boot: once displayd has /dev/input/event1 open, repeatedly grab a window by
 * its title bar and drag it, so a drag-triggered server/client crash shows up
 * as repeated app reloads in the serial log. Not built into normal boots. */
static void inj_move(int dx, int dy) {
  if (dx)
    input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_REL, B1NIX_REL_X, dx);
  if (dy)
    input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_REL, B1NIX_REL_Y, dy);
  input_event_sync(INPUT_DEV_MOUSE);
}

static void inj_btn(int down) {
  input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_KEY, B1NIX_BTN_LEFT, down);
  input_event_sync(INPUT_DEV_MOUSE);
}

/* Park the cursor at a known spot: a huge negative delta clamps to (0,0) in
 * displayd, then a positive delta lands on the target. */
static void inj_goto(int x, int y) {
  inj_move(-4000, -4000);
  scheduler_sleep_ticks(2);
  inj_move(x, y);
  scheduler_sleep_ticks(2);
}

static void inj_drag(int x, int y, int dx, int dy, int steps) {
  inj_goto(x, y);
  inj_btn(1);
  scheduler_sleep_ticks(SCHED_MS_TO_TICKS(20));
  for (int i = 0; i < steps; i++) {
    inj_move(dx, dy);
    scheduler_sleep_ticks(SCHED_MS_TO_TICKS(20));
  }
  inj_btn(0);
  scheduler_sleep_ticks(SCHED_MS_TO_TICKS(50));
}

static void gfxtest_thread(void *arg) {
  (void)arg;
  while (!input_dev_has_clients(INPUT_DEV_MOUSE))
    scheduler_sleep_ticks(SCHED_MS_TO_TICKS(100));
  scheduler_sleep_ticks(SCHED_MS_TO_TICKS(2000)); /* let the desktop apps map their windows */
  console_write("gfxtest: drag injector active\n");
  for (;;) {
    /* gpaint title (placement 0 ≈ (48,92), title row ~84). */
    inj_drag(120, 84, 24, 6, 12);
    /* terminal title (placement 2 ≈ x208,y430, title row ~422). */
    inj_drag(400, 422, 20, -8, 12);
    /* gclock title (placement 1, top-right). */
    inj_drag(860, 46, -18, 10, 12);
  }
}

/* ── M47 smoke: mouse event burst ─────────────────────────────────────────
 * /bin/m47_smoke opens /dev/input/event1 and waits for a known burst
 * (REL_X=+7, REL_Y=-3, BTN_LEFT press, SYN) to prove the event stream, the
 * blocking read path and the SYN framing all work. Nothing else in a headless
 * run moves the mouse, so the kernel produces it in test mode once a reader
 * has the device open. (Lived in the old kernel/user/programs.c dispatcher;
 * re-homed here when the built-in programs moved to userspace.) */
static void m47_inject_thread(void *arg) {
  (void)arg;
  u32 served = input_dev_open_seq(INPUT_DEV_MOUSE);
  for (;;) {
    /* One burst per open(), keyed on the open counter rather than on the
     * client count: m47_smoke opens event1 twice back to back (input-open then
     * input-event) and the close/reopen gap is shorter than this poll period,
     * so a has_clients() edge would be missed and the second open would wait
     * forever. */
    u32 seq = input_dev_open_seq(INPUT_DEV_MOUSE);
    if (seq == served || !input_dev_has_clients(INPUT_DEV_MOUSE)) {
      /* Wait for an open rather than polling for one.
       *
       * This polled every 2 ticks for the whole run, and measured, that made a
       * test-support thread the machine's heartbeat: it ended 1,237 idle
       * stretches and accounted for 3,004 of the aarch64 sys lane's 3,438 idle
       * ticks -- 87% of all the time the machine spent idle, to watch a counter
       * that changes a handful of times in a run.
       *
       * The open path now wakes this thread, so the seq comparison above still
       * catches an open/close/open faster than any poll period (which is why it
       * counts opens rather than watching for a client edge). The timeout is a
       * safety net, not the mechanism. */
      scheduler_block_on_timeout(&dev_open_seq[0], 100);
      continue;
    }
    served = seq;
    input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_REL, B1NIX_REL_X, 7);
    input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_REL, B1NIX_REL_Y, -3);
    input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_KEY, B1NIX_BTN_LEFT, 1);
    input_event_sync(INPUT_DEV_MOUSE);
    /* Then a report of motion alone, after the reader has drained the first
     * and gone back to sleep. A button makes a report urgent and wakes the
     * reader however the queue looked; motion wakes only on the queue edge,
     * and for as long as that edge was tested at the SYN it never fired.
     * 300 ms: the reader drains the first report and blocks within
     * milliseconds even under TCG; a full second was idle lane time, paid
     * twice because m47_smoke opens the device twice. */
    scheduler_sleep_ticks(SCHED_MS_TO_TICKS(300));
    if (input_dev_open_seq(INPUT_DEV_MOUSE) != served)
      continue; /* a newer open is owed its own burst first */
    input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_REL, B1NIX_REL_X, 5);
    input_event_sync(INPUT_DEV_MOUSE);
  }
}

void input_m47_inject_start(void) {
  (void)kthread_create("m47-input-inject", m47_inject_thread, 0);
}

void input_gfxtest_start(void) {
  (void)kthread_create("gfxtest-drag", gfxtest_thread, 0);
  console_write("gfxtest: drag injector scheduled\n");
}

/* ── init ── */

/* ── sysfs: /sys/class/input, the way udev expects it ─────────────────────
 *
 * /sys/devices/virtual/input/inputN/         name, uevent, id/, capabilities/
 * /sys/devices/virtual/input/inputN/eventN/  dev, uevent
 * /sys/class/input/{inputN,eventN}           links to the two
 * /sys/dev/char/13:64+N                      link to eventN
 *
 * udev's input_id builtin reads capabilities/{ev,key,rel,abs} and writes
 * ID_INPUT_MOUSE / ID_INPUT_KEYBOARD into /run/udev/data, which is where
 * libinput (through elogind's seat) finds out what to open. `udevadm
 * trigger` writes "add" to the uevent files; the store posts the event. */
struct input_sysfs_text {
  char text[512];
  char devpath[96];
  char devname[24];
  int minor; /* -1: the inputN device, no node */
};
static isize input_sysfs_show(void *ctx, char *buf, usize cap) {
  const char *t = ctx ? ((struct input_sysfs_text *)ctx)->text : "";
  usize n = strlen(t);
  if (n > cap) n = cap;
  memcpy(buf, t, n);
  return (isize)n;
}
static isize input_sysfs_uevent_store(void *ctx, const char *buf, usize len) {
  struct input_sysfs_text *u = (struct input_sysfs_text *)ctx;
  char action[16];
  usize n = 0;
  if (!u || !buf) return -EINVAL;
  while (n < len && n < sizeof(action) - 1 && buf[n] != ' ' && buf[n] != '\n' && buf[n] != '\0')
    n++;
  memcpy(action, buf, n);
  action[n] = '\0';
  if (strcmp(action, "add") && strcmp(action, "change") && strcmp(action, "remove"))
    return -EINVAL;
  uevent_post(action, u->devpath, "input", 0, u->minor >= 0 ? u->devname : 0,
              u->minor >= 0 ? 13 : 0, u->minor >= 0 ? u->minor : 0);
  return (isize)len;
}
static void input_sysfs_free(void *ctx) { kfree(ctx); }

/* Hex words, most significant first, no leading zero words: the format of
 * every capabilities/ file and of the EV=/KEY= lines in uevent. */
static usize bits_hex(char *out, usize cap, const u64 *bits, usize words) {
  usize pos = 0;
  usize first = words;
  for (usize w = words; w-- > 0;)
    if (bits[w]) { first = w; break; }
  if (first == words)
    return (usize)snprintf(out, cap, "0");
  for (usize w = first + 1; w-- > 0;) {
    pos += (usize)snprintf(out + pos, cap > pos ? cap - pos : 0, "%s%lx", w == first ? "" : " ",
                           (unsigned long)bits[w]);
  }
  return pos;
}

static int input_sysfs_text_attr(struct sysfs_dir *d, const char *name, const char *text) {
  struct input_sysfs_text *t = kzalloc(sizeof(*t));
  if (!t) return -ENOMEM;
  snprintf(t->text, sizeof(t->text), "%s", text);
  t->minor = -1;
  if (sysfs_reg_attr(d, name, 0444, input_sysfs_show, 0, t, input_sysfs_free) != 0) {
    kfree(t);
    return -1;
  }
  return 0;
}

static void input_sysfs_publish(int i) {
  const struct evdev_view *v = &g_view[i];
  char inputN[16], eventN[16], node[16], tmp[256];
  snprintf(inputN, sizeof(inputN), "input%d", i);
  snprintf(eventN, sizeof(eventN), "event%d", i);
  snprintf(node, sizeof(node), "13:%d", 64 + i);
  struct sysfs_dir *virt = sysfs_reg_dir(sysfs_reg_dir(sysfs_reg_dir(0, "devices"), "virtual"), "input");
  struct sysfs_dir *dev = virt ? sysfs_reg_dir(virt, inputN) : 0;
  if (!dev)
    return;
  snprintf(tmp, sizeof(tmp), "%s\n", v->name);
  input_sysfs_text_attr(dev, "name", tmp);
  input_sysfs_text_attr(dev, "phys", "\n");
  input_sysfs_text_attr(dev, "uniq", "\n");
  input_sysfs_text_attr(dev, "properties", "0\n");
  struct sysfs_dir *id = sysfs_reg_dir(dev, "id");
  if (id) {
    snprintf(tmp, sizeof(tmp), "%04x\n", v->bustype); input_sysfs_text_attr(id, "bustype", tmp);
    snprintf(tmp, sizeof(tmp), "%04x\n", v->vendor);  input_sysfs_text_attr(id, "vendor", tmp);
    snprintf(tmp, sizeof(tmp), "%04x\n", v->product); input_sysfs_text_attr(id, "product", tmp);
    snprintf(tmp, sizeof(tmp), "%04x\n", v->version); input_sysfs_text_attr(id, "version", tmp);
  }
  char ev[64], key[256], rel[32], abs[64], msc[32], led[32];
  bits_hex(ev, sizeof(ev), v->ev, EVDEV_BITS(EV_MAX_BITS));
  bits_hex(key, sizeof(key), v->key, EVDEV_BITS(KEY_MAX_BITS));
  bits_hex(rel, sizeof(rel), v->rel, EVDEV_BITS(REL_MAX_BITS));
  bits_hex(abs, sizeof(abs), v->abs, EVDEV_BITS(ABS_MAX_BITS));
  bits_hex(msc, sizeof(msc), v->msc, EVDEV_BITS(MSC_MAX_BITS));
  bits_hex(led, sizeof(led), v->led, EVDEV_BITS(LED_MAX_BITS));
  struct sysfs_dir *caps = sysfs_reg_dir(dev, "capabilities");
  if (caps) {
    snprintf(tmp, sizeof(tmp), "%s\n", ev);  input_sysfs_text_attr(caps, "ev", tmp);
    snprintf(tmp, sizeof(tmp), "%s\n", key); input_sysfs_text_attr(caps, "key", tmp);
    snprintf(tmp, sizeof(tmp), "%s\n", rel); input_sysfs_text_attr(caps, "rel", tmp);
    snprintf(tmp, sizeof(tmp), "%s\n", abs); input_sysfs_text_attr(caps, "abs", tmp);
    snprintf(tmp, sizeof(tmp), "%s\n", msc); input_sysfs_text_attr(caps, "msc", tmp);
    snprintf(tmp, sizeof(tmp), "%s\n", led); input_sysfs_text_attr(caps, "led", tmp);
    input_sysfs_text_attr(caps, "sw", "0\n");
    input_sysfs_text_attr(caps, "ff", "0\n");
    input_sysfs_text_attr(caps, "snd", "0\n");
  }
  /* inputN/uevent: what a hotplug event for the device itself carries. */
  struct input_sysfs_text *u = kzalloc(sizeof(*u));
  if (u) {
    snprintf(u->devpath, sizeof(u->devpath), "/devices/virtual/input/%s", inputN);
    u->minor = -1;
    snprintf(u->text, sizeof(u->text),
             "PRODUCT=%x/%x/%x/%x\nNAME=\"%s\"\nPROP=0\nEV=%s\nKEY=%s\nREL=%s\nABS=%s\nMSC=%s\nLED=%s\n",
             v->bustype, v->vendor, v->product, v->version, v->name, ev, key, rel, abs, msc, led);
    if (sysfs_reg_attr(dev, "uevent", 0644, input_sysfs_show, input_sysfs_uevent_store, u, input_sysfs_free) != 0)
      kfree(u);
  }
  (void)sysfs_reg_link(dev, "subsystem", "../../../../class/input");
  /* eventN: the character device. */
  struct sysfs_dir *evd = sysfs_reg_dir(dev, eventN);
  if (evd) {
    snprintf(tmp, sizeof(tmp), "%s\n", node);
    input_sysfs_text_attr(evd, "dev", tmp);
    struct input_sysfs_text *ue = kzalloc(sizeof(*ue));
    if (ue) {
      snprintf(ue->devpath, sizeof(ue->devpath), "/devices/virtual/input/%s/%s", inputN, eventN);
      snprintf(ue->devname, sizeof(ue->devname), "input/%s", eventN);
      ue->minor = 64 + i;
      snprintf(ue->text, sizeof(ue->text), "MAJOR=13\nMINOR=%d\nDEVNAME=input/%s\n", 64 + i, eventN);
      if (sysfs_reg_attr(evd, "uevent", 0644, input_sysfs_show, input_sysfs_uevent_store, ue, input_sysfs_free) != 0)
        kfree(ue);
    }
    (void)sysfs_reg_link(evd, "subsystem", "../../../../../class/input");
    (void)sysfs_reg_link(evd, "device", "..");
  }
  struct sysfs_dir *cls = sysfs_reg_dir(sysfs_reg_dir(0, "class"), "input");
  char target[96];
  if (cls) {
    snprintf(target, sizeof(target), "../../devices/virtual/input/%s", inputN);
    (void)sysfs_reg_link(cls, inputN, target);
    snprintf(target, sizeof(target), "../../devices/virtual/input/%s/%s", inputN, eventN);
    (void)sysfs_reg_link(cls, eventN, target);
  }
  struct sysfs_dir *chr = sysfs_reg_dir(sysfs_reg_dir(0, "dev"), "char");
  if (chr) {
    snprintf(target, sizeof(target), "../../devices/virtual/input/%s/%s", inputN, eventN);
    (void)sysfs_reg_link(chr, node, target);
  }
}

void input_init(void) {
  evdev_view_init();
  struct vfs_node *dir = vfs_add_node("/dev/input", VFS_DIRECTORY, 0, 0, 0);
  if (!IS_ERR(dir) && dir)
    vfs_node_put(dir);

  for (int i = 0; i < INPUT_NDEVS; i++) {
    char path[32];
    strcpy(path, "/dev/input/");
    strcpy(path + 11, devs[i].name);
    struct vfs_node *node = vfs_add_node(path, VFS_DEVICE, 0, 0, 0);
    if (IS_ERR(node) || !node) {
      console_write("input: failed to register /dev/input node\n");
      continue;
    }
    /* Input events are sensitive (keystrokes): root-only access. */
    node->inode->mode = 0600;
    /* char 13:64+N, the numbers Linux gives event devices: elogind's
     * TakeDevice and udev's /run/udev/data/c13:N key both go by them. */
    node->inode->rdev = ((u64)13 << 8) | (u64)(64 + i);
    devs[i].registered = 1;
    vfs_node_put(node);
    input_sysfs_publish(i);
  }
  console_write("input: /dev/input/event0 (kbd) + event1 (mouse) + event2 "
                "(touch) ready\n");
}
