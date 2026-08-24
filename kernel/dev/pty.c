/* M32b — pseudo-terminal (pty) substrate.
 *
 * A pty is a bidirectional character pipe with a terminal line discipline on
 * the slave side. Opening /dev/ptmx allocates a master; the matching slave is
 * /dev/pts/<N>. The shell/login program runs on the slave (its stdin/stdout);
 * a terminal server (sshd) drives the master.
 *
 *   master write  -> input line discipline (ICRNL, ISIG, ICANON, ECHO) -> slave read
 *   slave  write  -> output processing (OPOST/ONLCR)                    -> master read
 *
 * Handles are raw (allocated via alloc_raw_handle, like sockets) with custom
 * vfs_file_ops, so master/slave fds flow through the normal read/write/poll/
 * ioctl/close paths and inherit across fork/exec.
 */
#include <b1nix/termios_abi.h>
#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/uidgid.h>
#include <b1nix/posix.h>
#include <b1nix/syscall.h>
#include <b1nix/klog.h>
#include <string.h>
#include <stdlib.h>

/* How many pseudo-terminals can exist at once — a capacity derived at init,
 * with the slots themselves allocated only as they are opened.
 *
 * Sixteen was a hard wall reached by ordinary use, not by a stress test: a
 * tmux session with a dozen panes, or a build fanning out shells, exhausts it
 * and every further openpty() returns ENFILE. Linux's equivalent (pty.max)
 * defaults to 4096.
 *
 * A `struct pty` is ~17 KiB — two 8 KiB rings plus a line buffer — so 4096 of
 * them statically would be 71 MiB of BSS on a machine that may never open a
 * second terminal. Hence the two-level shape: a small pointer table sized once
 * from RAM, and a slot kmalloc'd on first use of that index and kept for reuse.
 * Idle capacity therefore costs one pointer, not 17 KiB.
 *
 * FLOOR   16 slots — the previous fixed count, so a 256 MiB guest is unchanged
 *         in both capacity and behaviour.
 * CEILING 4096 slots (32 KiB of pointers, and 71 MiB only if every one is
 *         actually opened) — Linux's default, and past it the pts namespace is
 *         not the thing under strain.
 * `b1nix.pty-max=N` states the capacity directly. */
#define PTY_SLOTS_MIN 16u
#define PTY_SLOTS_MAX 4096u
#define PTY_BUF   8192
#define PTY_LINE  1024

struct pty {
  /* Guards everything below it in this structure.
   *
   * The rings were mutated with no lock at all: a terminal's write to the
   * slave and its own read of the master run on different CPUs at the same
   * time, and both update `out_count` with a read-modify-write. A lost update
   * there makes the count disagree with the ring — too high, and a reader
   * hands out bytes that were never written; wrapped below zero (it is
   * unsigned), and the "is there room" test never says no again, so head and
   * tail walk past each other and the line discipline reads its own garbage.
   * That is what a compositor with a terminal on it hit on six cores and never
   * on one.
   *
   * Taken with interrupts masked: the line discipline is reached from a
   * write(2) on any CPU, and the wakeups it drives run from the scheduler.
   * Nothing that can sleep — signal delivery, wakeups, copies to and from user
   * memory — happens while it is held. */
  spinlock_t lock;
  int used;
  int index;
  int master_open;
  int slave_open;

  /* slave -> master (output the slave wrote, post OPOST). master reads this. */
  u8 out[PTY_BUF];
  usize out_head, out_tail, out_count;

  /* master -> slave committed bytes (canonical lines / raw). slave reads this. */
  u8 in[PTY_BUF];
  usize in_head, in_tail, in_count;
  int in_eof; /* a VEOF on an empty line: next slave read returns 0 once. */

  /* canonical line assembly (not yet committed to `in`). */
  u8 line[PTY_LINE];
  usize line_len;

  struct b1nix_termios termios;
  struct b1nix_winsize winsize;
  usize fg_pgrp;     /* foreground process group for job control + signals */
  usize session_id;  /* session that claimed this pty via TIOCSCTTY */
  u16 uid;
  u16 gid;
};

static struct pty **ptys;
static u32 pty_slots = PTY_SLOTS_MIN;
/* Guards the free-slot scan and the lazy allocation behind it. The scan used to
 * run unlocked, so two concurrent openpty() calls could pick the same index and
 * one would hand back a master already owned by the other. */
static spinlock_t pty_lock = SPINLOCK_INIT;

/* The pty at `idx`, or NULL if the index is out of range or never allocated. */
static struct pty *pty_slot(int idx) {
  if (idx < 0 || (u32)idx >= pty_slots || !ptys)
    return 0;
  return ptys[idx];
}

/* ── ring-buffer helpers ── */
static int rb_putc(u8 *buf, usize *tail, usize *count, u8 c) {
  if (*count >= PTY_BUF)
    return 0;
  buf[*tail] = c;
  *tail = (*tail + 1) % PTY_BUF;
  (*count)++;
  return 1;
}

static int rb_getc(u8 *buf, usize *head, usize *count, u8 *c) {
  if (*count == 0)
    return 0;
  *c = buf[*head];
  *head = (*head + 1) % PTY_BUF;
  (*count)--;
  return 1;
}

/* ── line discipline ── */

/* Push one output byte to the master read buffer, applying OPOST/ONLCR. */
static void pty_output(struct pty *p, u8 c) {
  if ((p->termios.c_oflag & B1NIX_OPOST) &&
      (p->termios.c_oflag & B1NIX_ONLCR) && c == '\n')
    rb_putc(p->out, &p->out_tail, &p->out_count, '\r');
  rb_putc(p->out, &p->out_tail, &p->out_count, c);
}

static void pty_commit_line(struct pty *p) {
  for (usize i = 0; i < p->line_len; i++)
    rb_putc(p->in, &p->in_tail, &p->in_count, p->line[i]);
  if (p->line_len == 0)
    p->in_eof = 1; /* VEOF on empty line == EOF for the reader */
  p->line_len = 0;
}

/* Job-control signals for this pty's foreground group.
 *
 * pgrp 1 is the boot/kernel group that every directly kernel-spawned task
 * (netd, displayd, and historically /bin/init) starts in. Signalling it from a
 * pty would take out unrelated system tasks — a session ending on the pty
 * (master closed => SIGHUP) once killed /bin/init itself, silently, ending the
 * whole test run mid-suite. /bin/init now runs in its own session, and this
 * guard keeps any other boot-group task out of a pty's blast radius. */
static void pty_signal_fg(struct pty *p, int sig) {
  if (p->fg_pgrp <= 1)
    return;
  scheduler_kill_process_group(p->fg_pgrp, sig);
}

/* Return a slot to its unused state, ready to be handed out again.
 *
 * Called only when neither side is open, so nothing is inside the lock and
 * zeroing it leaves it in exactly the state SPINLOCK_INIT names. */
static void pty_wipe(struct pty *p) {
  memset(p, 0, sizeof(*p));
}

/* Process one byte arriving from the master (the "keyboard" side).
 *
 * Returns the signal the byte asks for, or 0. It is not delivered here: this
 * runs under p->lock, and killing a process group takes scheduler locks and
 * walks the task list. The caller sends it once the lock is dropped. */
static int pty_input_char(struct pty *p, u8 c) {
  struct b1nix_termios *t = &p->termios;

  if ((t->c_iflag & B1NIX_ICRNL) && c == '\r')
    c = '\n';

  if (t->c_lflag & B1NIX_ISIG) {
    if (c == t->c_cc[B1NIX_VINTR])
      return SIGINT;
    if (c == t->c_cc[B1NIX_VQUIT])
      return SIGQUIT;
    if (c == t->c_cc[B1NIX_VSUSP])
      return SIGTSTP;
  }

  if (t->c_lflag & B1NIX_ICANON) {
    if (c == t->c_cc[B1NIX_VERASE] || c == '\b' || c == 127) {
      if (p->line_len > 0) {
        p->line_len--;
        if (t->c_lflag & B1NIX_ECHO) {
          pty_output(p, '\b');
          pty_output(p, ' ');
          pty_output(p, '\b');
        }
      }
      return 0;
    }
    if (c == t->c_cc[B1NIX_VEOF]) {
      pty_commit_line(p);
      return 0;
    }
    if (t->c_lflag & B1NIX_ECHO)
      pty_output(p, c);
    if (p->line_len < PTY_LINE)
      p->line[p->line_len++] = c;
    if (c == '\n')
      pty_commit_line(p);
  } else {
    if (t->c_lflag & B1NIX_ECHO)
      pty_output(p, c);
    rb_putc(p->in, &p->in_tail, &p->in_count, c);
  }
  return 0;
}

/* ── master file ops ── */
static isize pty_master_read(struct vfs_handle *h, char *buf, usize size) {
  struct pty *p = (struct pty *)h->private_data;
  u64 f;
  for (;;) {
    spin_lock_irqsave(&p->lock, &f);
    if (p->out_count > 0) {
      usize n = 0;
      u8 c;
      while (n < size && rb_getc(p->out, &p->out_head, &p->out_count, &c))
        buf[n++] = (char)c;
      spin_unlock_irqrestore(&p->lock, f);
      scheduler_wake_all(&p->out_tail); /* writers waiting for out space */
      return (isize)n;
    }
    int eof = !p->slave_open;
    spin_unlock_irqrestore(&p->lock, f);
    if (eof)
      return 0; /* slave gone: EOF */
    if (h->flags & B1NIX_O_NONBLOCK)
      return -EAGAIN;
    scheduler_block_on(&p->out_count);
  }
}

static isize pty_master_write(struct vfs_handle *h, const char *buf, usize size) {
  struct pty *p = (struct pty *)h->private_data;
  u64 f;
  /* The signals the line discipline asks for, collected under the lock and
   * sent after it: killing a process group walks the task list and takes the
   * scheduler's own locks, which is not something to do from inside this one.
   * Four slots is more than a write can usefully produce — the same signal
   * twice in one buffer is one signal to the group either way. */
  int sigs[4];
  int nsig = 0;
  spin_lock_irqsave(&p->lock, &f);
  for (usize i = 0; i < size; i++) {
    int sig = pty_input_char(p, (u8)buf[i]);
    if (sig) {
      int seen = 0;
      for (int k = 0; k < nsig; k++)
        if (sigs[k] == sig)
          seen = 1;
      if (!seen && nsig < (int)(sizeof(sigs) / sizeof(sigs[0])))
        sigs[nsig++] = sig;
    }
  }
  spin_unlock_irqrestore(&p->lock, f);
  for (int k = 0; k < nsig; k++)
    pty_signal_fg(p, sigs[k]);
  scheduler_wake_all(&p->in_count);  /* wake slave readers */
  scheduler_wake_all(&p->out_count); /* echo produced master-readable output */
  scheduler_wake_all(vfs_poll_chan);
  return (isize)size;
}

static int pty_master_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct pty *p = (struct pty *)h->private_data;
  u64 f;
  pfd->revents = 0;
  spin_lock_irqsave(&p->lock, &f);
  if (p->out_count > 0 || !p->slave_open)
    pfd->revents |= B1NIX_POLLIN;
  if (p->in_count < PTY_BUF)
    pfd->revents |= B1NIX_POLLOUT;
  spin_unlock_irqrestore(&p->lock, f);
  return 0;
}

/* Teardown (and the controlling-terminal SIGHUP hangup) runs ONLY from
 * ->release at refcount 0, never from a per-fd ->close — so a master shared
 * across dup2/fork hangs up only when its LAST fd is closed, not the first. */
static void pty_master_release(struct vfs_handle *h) {
  struct pty *p = (struct pty *)h->private_data;
  u64 f;
  if (!p)
    return;
  spin_lock_irqsave(&p->lock, &f);
  p->master_open = 0;
  int last = !p->slave_open;
  spin_unlock_irqrestore(&p->lock, f);
  /* Hangup: signal the foreground group, then wake blocked slave readers so
   * they observe EOF. */
  pty_signal_fg(p, SIGHUP);
  scheduler_wake_all(&p->in_count);
  scheduler_wake_all(vfs_poll_chan);
  if (last)
    pty_wipe(p);
  h->private_data = 0;
}

/* ── slave file ops ── */
static isize pty_slave_read(struct vfs_handle *h, char *buf, usize size) {
  struct pty *p = (struct pty *)h->private_data;
  u64 f;
  for (;;) {
    spin_lock_irqsave(&p->lock, &f);
    if (p->in_count > 0) {
      usize n = 0;
      u8 c;
      while (n < size && rb_getc(p->in, &p->in_head, &p->in_count, &c))
        buf[n++] = (char)c;
      spin_unlock_irqrestore(&p->lock, f);
      return (isize)n;
    }
    if (p->in_eof) {
      p->in_eof = 0;
      spin_unlock_irqrestore(&p->lock, f);
      return 0;
    }
    int hup = !p->master_open;
    spin_unlock_irqrestore(&p->lock, f);
    if (hup)
      return 0; /* hangup: EOF */
    if (h->flags & B1NIX_O_NONBLOCK)
      return -EAGAIN;
    scheduler_block_on(&p->in_count);
  }
}

static isize pty_slave_write(struct vfs_handle *h, const char *buf, usize size) {
  struct pty *p = (struct pty *)h->private_data;
  u64 f;
  usize i = 0;
  while (i < size) {
    spin_lock_irqsave(&p->lock, &f);
    /* Each logical byte may expand to 2 (ONLCR); write while there is room for
     * both, and give the lock back before waiting for more. */
    while (i < size && PTY_BUF - p->out_count >= 2)
      pty_output(p, (u8)buf[i++]);
    int hup = !p->master_open;
    spin_unlock_irqrestore(&p->lock, f);
    if (i == size)
      break;
    if (hup)
      return i ? (isize)i : -EPIPE;
    if (h->flags & B1NIX_O_NONBLOCK)
      return i ? (isize)i : -EAGAIN;
    scheduler_wake_all(&p->out_count); /* let the master drain */
    scheduler_wake_all(vfs_poll_chan);
    scheduler_block_on(&p->out_tail);
  }
  scheduler_wake_all(&p->out_count);
  scheduler_wake_all(vfs_poll_chan);
  return (isize)size;
}

static int pty_slave_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct pty *p = (struct pty *)h->private_data;
  u64 f;
  pfd->revents = 0;
  spin_lock_irqsave(&p->lock, &f);
  if (p->in_count > 0 || p->in_eof || !p->master_open)
    pfd->revents |= B1NIX_POLLIN;
  if (PTY_BUF - p->out_count >= 2)
    pfd->revents |= B1NIX_POLLOUT;
  spin_unlock_irqrestore(&p->lock, f);
  return 0;
}

/* Teardown runs ONLY from ->release (at refcount 0), never from a per-fd
 * ->close: a slave fd shared across dup2/fork (e.g. login_tty dups the slave to
 * 0/1/2 then closes the original) must not be torn down while other fds still
 * reference it. A premature teardown nulls private_data and every later ioctl/
 * read on the shared handle fails (tcgetattr -> EINVAL). Same fix as sockets. */
static void pty_slave_release(struct vfs_handle *h) {
  struct pty *p = (struct pty *)h->private_data;
  u64 f;
  if (!p)
    return;
  spin_lock_irqsave(&p->lock, &f);
  p->slave_open = 0;
  int last = !p->master_open;
  spin_unlock_irqrestore(&p->lock, f);
  scheduler_wake_all(&p->out_count); /* master readers observe EOF */
  scheduler_wake_all(vfs_poll_chan);
  if (last)
    pty_wipe(p);
  h->private_data = 0;
}

/* ── shared ioctl (master + slave) ── */
static int pty_ioctl(struct vfs_handle *h, u64 request, void *arg) {
  struct pty *p = (struct pty *)h->private_data;
  if (!p)
    return -EINVAL;

  /* Every case below copies through a local first. The lock must not be held
   * across a copy to or from user memory: that memory may be paged out, and
   * faulting it in with a spinlock held (interrupts masked) is how a CPU stops
   * for good. */
  u64 f;

  switch (request) {
  case B1NIX_TCGETS: {
    struct b1nix_termios snapshot;
    spin_lock_irqsave(&p->lock, &f);
    snapshot = p->termios;
    spin_unlock_irqrestore(&p->lock, f);
    return tty_termios_copyout(arg, &snapshot);
  }
  /* TCSADRAIN and TCSAFLUSH: there is no output queue to drain and no input
   * queue that outlives the call, so all three are the same operation. They
   * used to be folded into TCSETS by the Linux-ABI ioctl wrapper; the wrapper
   * is gone, and a pty that answered only TCSETS made dbclient fail with
   * "Failed to set raw TTY mode". */
  case B1NIX_TCSETSW:
  case B1NIX_TCSETSF:
  case B1NIX_TCSETS: {
    struct b1nix_termios want;
    int rc = tty_termios_copyin(&want, arg);
    if (rc < 0)
      return rc;
    spin_lock_irqsave(&p->lock, &f);
    p->termios = want;
    spin_unlock_irqrestore(&p->lock, f);
    return 0;
  }
  case B1NIX_TIOCGWINSZ: {
    struct b1nix_winsize ws;
    spin_lock_irqsave(&p->lock, &f);
    ws = p->winsize;
    spin_unlock_irqrestore(&p->lock, f);
    if (!arg || syscall_copyout(arg, &ws, sizeof(ws)) < 0)
      return -EFAULT;
    return 0;
  }
  case B1NIX_TIOCSWINSZ: {
    struct b1nix_winsize ws;
    if (!arg || syscall_copyin(&ws, arg, sizeof(ws)) < 0)
      return -EFAULT;
    spin_lock_irqsave(&p->lock, &f);
    p->winsize = ws;
    spin_unlock_irqrestore(&p->lock, f);
    /* A window-size change notifies the foreground group. */
    pty_signal_fg(p, SIGWINCH);
    return 0;
  }
  case B1NIX_TIOCGPTN: {
    u32 n = (u32)p->index;
    if (!arg || syscall_copyout(arg, &n, sizeof(n)) < 0)
      return -EFAULT;
    return 0;
  }
  case B1NIX_TIOCSPTLCK:
    /* unlockpt: b1nix slaves are never locked; accept and ignore the value. */
    return 0;
  case B1NIX_TIOCGPGRP: {
    /* The user buffer is a pid_t (32-bit) — copying sizeof(usize) would
     * read/write 4 bytes of adjacent user stack on x86_64. */
    int fg32 = (int)p->fg_pgrp;
    if (!arg || syscall_copyout(arg, &fg32, sizeof(fg32)) < 0)
      return -EFAULT;
    return 0;
  }
  case B1NIX_TIOCSPGRP: {
    int fg32;
    if (!arg || syscall_copyin(&fg32, arg, sizeof(fg32)) < 0)
      return -EFAULT;
    spin_lock_irqsave(&p->lock, &f);
    p->fg_pgrp = (usize)fg32;
    spin_unlock_irqrestore(&p->lock, f);
    return 0;
  }
  case B1NIX_TIOCSCTTY:
    /* The caller (typically a session leader after setsid) claims this pty as
     * its controlling terminal. */
    if (current_task) {
      p->session_id = current_task->session_id;
      p->fg_pgrp = current_task->process_group_id;
      if (current_task->session_id == current_task->id) {
        scheduler_set_ctty(current_task, 3, (int)p->index);
      }
    }
    return 0;
  default:
    return -ENOTTY;
  }
}

static const struct vfs_file_ops pty_master_ops = {
  .read = pty_master_read,
  .write = pty_master_write,
  .poll = pty_master_poll,
  .release = pty_master_release,
  .ioctl = pty_ioctl,
};

static const struct vfs_file_ops pty_slave_ops = {
  .read = pty_slave_read,
  .write = pty_slave_write,
  .poll = pty_slave_poll,
  .release = pty_slave_release,
  .ioctl = pty_ioctl,
};

static void pty_reset_termios(struct pty *p) {
  memset(&p->termios, 0, sizeof(p->termios));
  p->termios.c_iflag = B1NIX_ICRNL;
  p->termios.c_oflag = B1NIX_OPOST | B1NIX_ONLCR;
  p->termios.c_lflag = B1NIX_ICANON | B1NIX_ECHO | B1NIX_ISIG;
  p->termios.c_cc[B1NIX_VINTR] = 3;    /* ^C */
  p->termios.c_cc[B1NIX_VQUIT] = 28;   /* ^\ */
  p->termios.c_cc[B1NIX_VERASE] = 127; /* DEL */
  p->termios.c_cc[B1NIX_VEOF] = 4;     /* ^D */
  p->termios.c_cc[B1NIX_VSUSP] = 26;   /* ^Z */
  p->winsize.ws_row = 24;
  p->winsize.ws_col = 80;
}

void pty_init(void) {
  u64 ram_mb = pmm_total_usable_memory() / (1024ULL * 1024ULL);
  /* One slot per 4 MiB of RAM: a machine with room for the sessions gets the
   * capacity for them, and a small one keeps the floor. */
  u32 want = bootinfo_get_u32("b1nix.pty-max",
                              (u32)(ram_mb > 0x100000ULL ? 0x100000ULL : ram_mb) / 4u);

  if (want < PTY_SLOTS_MIN)
    want = PTY_SLOTS_MIN;
  if (want > PTY_SLOTS_MAX)
    want = PTY_SLOTS_MAX;
  pty_slots = want;

  ptys = kzalloc((usize)pty_slots * sizeof(*ptys));
  if (!ptys) {
    pty_slots = PTY_SLOTS_MIN;
    ptys = kzalloc((usize)pty_slots * sizeof(*ptys));
  }
}

int pty_open_master(int flags) {
  u64 lock_flags;
  int idx = -1;

  /* Claim a slot under the lock, allocating one if this index has never been
   * used. kmalloc does not block, so it is safe here; the alternative — drop
   * the lock to allocate and retake it — reopens the double-claim race the
   * lock exists to close. */
  spin_lock_irqsave(&pty_lock, &lock_flags);
  for (u32 i = 0; i < pty_slots && ptys; i++) {
    if (ptys[i] && ptys[i]->used)
      continue;
    if (!ptys[i]) {
      ptys[i] = kzalloc(sizeof(struct pty));
      if (!ptys[i])
        break; /* out of memory — report it as a full table */
    } else {
      /* Reused slot: wipe the previous session's state. This has to happen
       * before the lock is dropped — clearing the struct outside it would
       * momentarily zero `used` again and let another CPU claim the same
       * slot. */
      memset(ptys[i], 0, sizeof(struct pty));
    }
    idx = (int)i;
    ptys[i]->used = 1; /* claimed before the lock is dropped */
    ptys[i]->index = idx;
    break;
  }
  spin_unlock_irqrestore(&pty_lock, lock_flags);

  if (idx < 0)
    return -ENFILE;

  struct pty *p = ptys[idx];
  p->master_open = 1;
  p->slave_open = 0;
  pty_reset_termios(p);

  const struct cred *cred = scheduler_get_current_cred();
  p->uid = cred ? cred->euid : ROOT_UID;
  p->gid = cred ? cred->egid : ROOT_GID;

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_PTY_MASTER);
  if (!h) {
    p->used = 0;
    return -ENFILE;
  }
  h->private_data = p;
  h->ops = &pty_master_ops;
  h->flags = flags;

  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    p->used = 0;
    vfs_handle_release(h);
    return -EMFILE;
  }
  if (flags & B1NIX_O_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

int pty_open_slave(int index, int flags) {
  struct pty *p = pty_slot(index);

  if (!p)
    return -ENXIO;
  if (!p->used || !p->master_open)
    return -ENXIO;

  const struct cred *cred = scheduler_get_current_cred();
  int access_mask = 0;
  if (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))
    access_mask |= W_OK;
  if ((flags & 3) == B1NIX_O_RDONLY || (flags & B1NIX_O_RDWR))
    access_mask |= R_OK;

  if (cred && !cred_can_access(cred, p->uid, p->gid, 0620, access_mask))
    return -EACCES;

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_PTY_SLAVE);
  if (!h)
    return -ENFILE;
  h->private_data = p;
  h->ops = &pty_slave_ops;
  h->flags = flags;

  u64 f;
  spin_lock_irqsave(&p->lock, &f);
  p->slave_open = 1;
  /* If no session owns this pty yet, the opener becomes its controlling
   * process group (sane default so ^C/job-control work without an explicit
   * TIOCSCTTY). */
  if (p->fg_pgrp == 0 && current_task) {
    p->fg_pgrp = current_task->process_group_id;
    p->session_id = current_task->session_id;
  }
  spin_unlock_irqrestore(&p->lock, f);

  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    spin_lock_irqsave(&p->lock, &f);
    p->slave_open = 0;
    spin_unlock_irqrestore(&p->lock, f);
    vfs_handle_release(h);
    return -EMFILE;
  }
  if (flags & B1NIX_O_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

usize pty_fg_pgrp(int idx) {
  struct pty *p = pty_slot(idx);

  return p ? p->fg_pgrp : 0;
}

/* Pts index behind an open pty master/slave handle (for /proc/<pid>/fd names),
 * or -1 if `h` isn't a pty. */
int pty_index_of(struct vfs_handle *h) {
  if (!h ||
      (h->kind != VFS_HANDLE_PTY_SLAVE && h->kind != VFS_HANDLE_PTY_MASTER))
    return -1;
  struct pty *p = (struct pty *)h->private_data;
  return p ? p->index : -1;
}

/* Is pty slot `idx` currently allocated? Used by the /dev/pts lookup_cb to
 * materialise a stat()-able /dev/pts/<idx> node only for live slaves. */
int pty_allocated(int idx) {
  struct pty *p = pty_slot(idx);

  return p ? p->used : 0;
}
