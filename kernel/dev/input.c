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

    if (h->flags & B1NIX_O_NONBLOCK)
      return -EAGAIN;
    if (scheduler_signal_pending())
      return -ERESTARTSYS;
    scheduler_yield();
  }
}

static isize input_write(struct vfs_handle *h, const char *buf, usize len) {
  (void)h;
  (void)buf;
  (void)len;
  return -EINVAL;
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
  return fd;
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
  console_write("input: /dev/input/event0 (kbd) + event1 (mouse) ready\n");
}
