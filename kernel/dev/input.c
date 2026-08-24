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
 * blocking poll() is woken by the periodic vfs_poll_chan tick wake. */
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/input.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/vfs.h>
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

static u32 ring_next(u32 v) { return (v + 1) % INPUT_QUEUE_EVENTS; }

/* ── producer side ── */

void input_event_push(int dev, u16 type, u16 code, i32 value) {
  if (dev < 0 || dev >= INPUT_NDEVS || !devs[dev].registered)
    return;

  struct b1nix_input_event ev;
  ev.time_ticks = scheduler_get_uptime_ticks();
  ev.type = type;
  ev.code = code;
  ev.value = value;

  u64 flags;
  spin_lock_irqsave(&input_lock, &flags);
  for (int i = 0; i < INPUT_MAX_CLIENTS; i++) {
    struct input_client *c = devs[dev].clients[i];
    if (!c)
      continue;
    if (ring_next(c->tail) == c->head) { /* full: drop oldest */
      c->head = ring_next(c->head);
      c->dropped++;
    }
    c->ring[c->tail] = ev;
    c->tail = ring_next(c->tail);
  }
  spin_unlock_irqrestore(&input_lock, flags);
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
  if (!c)
    return -EINVAL;
  if (size < sizeof(struct b1nix_input_event))
    return -EINVAL;

  for (;;) {
    u64 flags;
    spin_lock_irqsave(&input_lock, &flags);
    if (c->head != c->tail) {
      usize n = 0;
      while (c->head != c->tail &&
             n + sizeof(struct b1nix_input_event) <= size) {
        memcpy(buf + n, &c->ring[c->head], sizeof(struct b1nix_input_event));
        c->head = ring_next(c->head);
        n += sizeof(struct b1nix_input_event);
      }
      spin_unlock_irqrestore(&input_lock, flags);
      return (isize)n;
    }
    spin_unlock_irqrestore(&input_lock, flags);

    /* musl's b1nix compatibility headers may retain its O_NONBLOCK bit
     * (0x8000) on raw device opens; accept it alongside the native VFS bit. */
    if (h->flags & (B1NIX_O_NONBLOCK | 0x8000))
      return -EAGAIN;
    if (scheduler_signal_pending())
      return -ERESTARTSYS;
    scheduler_yield();
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
  if (len == 0 || (len % sizeof(struct b1nix_input_event)) != 0)
    return -EINVAL;
  usize count = len / sizeof(struct b1nix_input_event);
  for (usize i = 0; i < count; i++) {
    struct b1nix_input_event ev;
    memcpy(&ev, buf + i * sizeof(ev), sizeof(ev));
    /* Timestamps are the kernel's to assign (input_event_push stamps them),
     * so only type/code/value are taken from the caller. */
    input_event_push(c->dev, ev.type, ev.code, ev.value);
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

static const struct vfs_file_ops input_ops = {
  .read = input_read,
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
      scheduler_sleep_ticks(2);
      continue;
    }
    served = seq;
    input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_REL, B1NIX_REL_X, 7);
    input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_REL, B1NIX_REL_Y, -3);
    input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_KEY, B1NIX_BTN_LEFT, 1);
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

void input_init(void) {
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
    devs[i].registered = 1;
    vfs_node_put(node);
  }
  console_write("input: /dev/input/event0 (kbd) + event1 (mouse) + event2 "
                "(touch) ready\n");
}
