/* M39 — serial tty devices (/dev/ttyS0, /dev/ttyS1).
 *
 * Each UART line gets an independent tty: its own input ring, canonical line
 * assembly, termios, and foreground-pgrp/session state, so a getty/login/shell
 * session on a serial port is fully separate from the merged VGA+COM1 boot
 * console. Modeled on the pty slave side (kernel/dev/pty.c): opens are
 * intercepted in vfs_open_flags and return raw handles with custom file ops,
 * so the fds flow through the normal read/write/poll/ioctl/fork paths.
 *
 * Input is drained from the UART by serial_tty_tick() on the BSP timer tick
 * (the same path that polls the i8042), through the line discipline and into
 * a single-producer/single-consumer ring. While /dev/ttyS0 is open it OWNS
 * the COM1 receive side; the merged boot console only falls back to COM1
 * input when no one holds the device (see serial_tty_claimed()).
 *
 * Output still interleaves with the kernel log on COM1 — same property as a
 * Linux console=ttyS0 system running a getty on the same port.
 */
#include <b1nix/termios_abi.h>
#include <b1nix/vfs.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/sched.h>
#include <b1nix/posix.h>
#include <b1nix/serial.h>
#include <b1nix/serial_tty.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <string.h>

#define STTY_BUF 4096
#define STTY_LINE 1024

struct serial_tty {
  int com;          /* serial_port_* index backing this tty */
  char name[8];     /* device node name, e.g. "ttyS0" */
  int registered;   /* /dev node exists (port detected at boot) */
  volatile int open_count; /* live handles; >0 = tty owns its UART RX */

  /* Committed input (canonical lines / raw bytes). Single producer (BSP
   * timer ISR via serial_tty_tick or the test inject hook), single logical
   * consumer (the reading task, serialised by read_lock). The ring carries
   * head/tail only — no shared count — so producer and consumer never write
   * the same field. */
  u8 in[STTY_BUF];
  volatile usize in_head, in_tail;
  volatile int in_eof; /* VEOF on an empty line: next empty read returns 0 */

  /* Canonical line assembly — producer-side state only. */
  u8 line[STTY_LINE];
  usize line_len;

  spinlock_t read_lock; /* serialises concurrent readers' head updates */

  struct b1nix_termios termios;
  struct b1nix_winsize winsize;
  usize fg_pgrp;
  usize session_id;
};

static struct serial_tty sttys[SERIAL_NPORTS];

static usize rb_next(usize i) { return (i + 1) % STTY_BUF; }

static int rb_empty(struct serial_tty *t) {
  usize head = __atomic_load_n(&t->in_head, __ATOMIC_RELAXED);
  usize tail = __atomic_load_n(&t->in_tail, __ATOMIC_ACQUIRE);
  return head == tail;
}

/* Producer side: returns 0 (drops the byte) when the ring is full. */
static int rb_put(struct serial_tty *t, u8 c) {
  usize tail = __atomic_load_n(&t->in_tail, __ATOMIC_RELAXED);
  usize head = __atomic_load_n(&t->in_head, __ATOMIC_ACQUIRE);
  usize next = rb_next(tail);
  if (next == head)
    return 0;
  t->in[tail] = c;
  __atomic_store_n(&t->in_tail, next, __ATOMIC_RELEASE);
  return 1;
}

/* ── line discipline (producer context: BSP timer ISR or test inject) ── */

static void stty_echo(struct serial_tty *t, u8 c) {
  u64 flags;
  console_lock_acquire_irqsave(&flags);
  if ((t->termios.c_oflag & B1NIX_OPOST) &&
      (t->termios.c_oflag & B1NIX_ONLCR) && c == '\n')
    serial_port_putc(t->com, '\r');
  serial_port_putc(t->com, (char)c);
  console_lock_release_irqrestore(flags);
}

static void stty_commit_line(struct serial_tty *t) {
  for (usize i = 0; i < t->line_len; i++)
    rb_put(t, t->line[i]);
  if (t->line_len == 0)
    t->in_eof = 1; /* VEOF on an empty line == EOF for the reader */
  t->line_len = 0;
}

static void stty_input_char(struct serial_tty *t, u8 c) {
  struct b1nix_termios *tio = &t->termios;

  if ((tio->c_iflag & B1NIX_ICRNL) && c == '\r')
    c = '\n';

  if (tio->c_lflag & B1NIX_ISIG) {
    if (c == tio->c_cc[B1NIX_VINTR]) {
      if (t->fg_pgrp)
        scheduler_kill_process_group(t->fg_pgrp, SIGINT);
      return;
    }
    if (c == tio->c_cc[B1NIX_VQUIT]) {
      if (t->fg_pgrp)
        scheduler_kill_process_group(t->fg_pgrp, SIGQUIT);
      return;
    }
    if (c == tio->c_cc[B1NIX_VSUSP]) {
      if (t->fg_pgrp)
        scheduler_kill_process_group(t->fg_pgrp, SIGTSTP);
      return;
    }
  }

  if (tio->c_lflag & B1NIX_ICANON) {
    if (c == tio->c_cc[B1NIX_VERASE] || c == '\b' || c == 127) {
      if (t->line_len > 0) {
        t->line_len--;
        if (tio->c_lflag & B1NIX_ECHO) {
          stty_echo(t, '\b');
          stty_echo(t, ' ');
          stty_echo(t, '\b');
        }
      }
      return;
    }
    if (c == tio->c_cc[B1NIX_VEOF]) {
      stty_commit_line(t);
      return;
    }
    if (tio->c_lflag & B1NIX_ECHO)
      stty_echo(t, c);
    if (t->line_len < STTY_LINE)
      t->line[t->line_len++] = c;
    if (c == '\n')
      stty_commit_line(t);
  } else {
    if (tio->c_lflag & B1NIX_ECHO)
      stty_echo(t, c);
    rb_put(t, c);
  }
}

/* Defined with the rest of the line-configuration helpers further down. */
static void stty_cflag_from_hw(struct serial_tty *t);

static void stty_reset(struct serial_tty *t) {
  t->in_head = t->in_tail = 0;
  t->in_eof = 0;
  t->line_len = 0;
  t->fg_pgrp = 0;
  t->session_id = 0;
  memset(&t->termios, 0, sizeof(t->termios));
  t->termios.c_iflag = B1NIX_ICRNL;
  t->termios.c_oflag = B1NIX_OPOST | B1NIX_ONLCR;
  t->termios.c_lflag = B1NIX_ICANON | B1NIX_ECHO | B1NIX_ISIG;
  /* c_cflag starts out describing the line as the boot code left it, so a
   * get/modify/set round trip preserves the hardware settings instead of
   * asking for baud code 0. */
  stty_cflag_from_hw(t);
  t->termios.c_cc[B1NIX_VINTR] = 3;    /* ^C */
  t->termios.c_cc[B1NIX_VQUIT] = 28;   /* ^\ */
  t->termios.c_cc[B1NIX_VERASE] = 127; /* DEL */
  t->termios.c_cc[B1NIX_VEOF] = 4;     /* ^D */
  t->termios.c_cc[B1NIX_VSUSP] = 26;   /* ^Z */
  t->winsize.ws_row = 24;
  t->winsize.ws_col = 80;
}

/* ── file ops ── */

static isize stty_read(struct vfs_handle *h, char *buf, usize size) {
  struct serial_tty *t = (struct serial_tty *)h->private_data;
  if (!t)
    return -EINVAL;

  /* Job control: a background pgrp reading from its controlling tty gets
   * SIGTTIN (mirrors the boot-console rules in vfs.c tty_read). */
  if (current_task && t->fg_pgrp > 0 &&
      (t->session_id == 0 || current_task->session_id == t->session_id)) {
    if (current_task->process_group_id != t->fg_pgrp) {
      if ((current_task->blocked_signals & (1ULL << (SIGTTIN - 1))) ||
          current_task->sigactions[SIGTTIN - 1].sa_handler == SIG_IGN)
        return -EIO;
      scheduler_kill_process_group(current_task->process_group_id, SIGTTIN);
      return -ERESTARTSYS;
    }
  }

  for (;;) {
    u64 irqf;
    spin_lock_irqsave(&t->read_lock, &irqf);
    if (!rb_empty(t)) {
      usize n = 0;
      usize head = __atomic_load_n(&t->in_head, __ATOMIC_RELAXED);
      usize tail = __atomic_load_n(&t->in_tail, __ATOMIC_ACQUIRE);

      /* Against the LOCAL cursor, not rb_empty(t).
       *
       * rb_empty() compares t->in_head, which this loop does not touch until
       * it is over -- so the ring never looked empty, the loop ran to `size`,
       * and every read returned the whole buffer: the bytes that were there
       * followed by whatever the ring held past them, which is zeroes. A
       * canonical read of "m39ldisc\n" came back as 32 bytes, EOF came back as
       * 32 bytes, and a raw read of two characters came back as 32. */
      while (n < size && head != tail) {
        buf[n++] = (char)t->in[head];
        head = rb_next(head);
      }
      __atomic_store_n(&t->in_head, head, __ATOMIC_RELEASE);
      spin_unlock_irqrestore(&t->read_lock, irqf);
      return (isize)n;
    }
    if (t->in_eof) {
      t->in_eof = 0;
      spin_unlock_irqrestore(&t->read_lock, irqf);
      return 0;
    }
    spin_unlock_irqrestore(&t->read_lock, irqf);

    if (h->flags & B1NIX_O_NONBLOCK)
      return -EAGAIN;
    if (scheduler_signal_pending())
      return -ERESTARTSYS;
    scheduler_yield();
  }
}

static isize stty_write(struct vfs_handle *h, const char *buf, usize size) {
  struct serial_tty *t = (struct serial_tty *)h->private_data;
  if (!t)
    return -EINVAL;

  /* Job control: background writes raise SIGTTOU only when TOSTOP is set. */
  if (current_task && t->fg_pgrp > 0 &&
      current_task->session_id == t->session_id &&
      current_task->process_group_id != t->fg_pgrp &&
      (t->termios.c_lflag & B1NIX_TOSTOP)) {
    if (!(current_task->blocked_signals & (1ULL << (SIGTTOU - 1))) &&
        current_task->sigactions[SIGTTOU - 1].sa_handler != SIG_IGN) {
      if (current_task->parent_id == 0 || current_task->parent_id == 1)
        return -EIO;
      scheduler_kill_process_group(current_task->process_group_id, SIGTTOU);
      return -ERESTARTSYS;
    }
  }

  /* Hold the same lock kernel console_write() uses for the whole buffer:
   * serial_port_putc() is a raw, unlocked UART register write, so without
   * this a concurrent console_write() (or another tty's write()) could land
   * a byte in the middle of this buffer and vice versa — observed corrupting
   * test markers under SMP exec churn (busybox stdout vs. kernel exec log,
   * both landing on COM1). */
  u64 flags;
  console_lock_acquire_irqsave(&flags);
  for (usize i = 0; i < size; i++) {
    if ((t->termios.c_oflag & B1NIX_OPOST) &&
        (t->termios.c_oflag & B1NIX_ONLCR) && buf[i] == '\n')
      serial_port_putc(t->com, '\r');
    serial_port_putc(t->com, buf[i]);
  }
  console_lock_release_irqrestore(flags);
  return (isize)size;
}

static int stty_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct serial_tty *t = (struct serial_tty *)h->private_data;
  pfd->revents = B1NIX_POLLOUT; /* the UART TX path never blocks for long */
  if (t && (!rb_empty(t) || t->in_eof))
    pfd->revents |= B1NIX_POLLIN;
  return 0;
}

/* ── M109 line configuration ──────────────────────────────────────────────
 * c_cflag used to be a value the tty stored and nothing read, so tcsetattr on
 * a serial line changed the line discipline and left the wire at the 38400 8N1
 * the boot code hardcoded — while tcgetattr cheerfully reported whatever had
 * been written. These map c_cflag onto the UART's divisor latch and LCR, and
 * read the chip back afterwards, so the two agree. */
#define STTY_CBAUD  0x100fu
#define STTY_CSIZE  0x0030u
#define STTY_CSTOPB 0x0040u
#define STTY_PARENB 0x0100u
#define STTY_PARODD 0x0200u
#define STTY_CREAD  0x0080u
#define STTY_CLOCAL 0x0800u

/* Linux CBAUD codes, in the order the constants run. Index is the code for
 * 0x00..0x0f; the 0x100x block is handled separately. */
static const u32 stty_baud_table[16] = {
    0,   50,   75,   110,  134,  150,  200,   300,
    600, 1200, 1800, 2400, 4800, 9600, 19200, 38400,
};

static u32 stty_cbaud_to_rate(u32 cflag) {
  u32 code = cflag & STTY_CBAUD;
  if (code == 0x1001) return 57600;
  if (code == 0x1002) return 115200;
  if (code == 0x1003) return 230400;  /* beyond a 115200 clock: rejected below */
  if (code < 16) return stty_baud_table[code];
  return 0;
}

static u32 stty_rate_to_cbaud(u32 rate) {
  if (rate == 57600) return 0x1001;
  if (rate == 115200) return 0x1002;
  for (u32 i = 1; i < 16; i++)
    if (stty_baud_table[i] == rate)
      return i;
  return 0;
}

/* Rewrite c_cflag from what the UART registers actually hold, so a subsequent
 * TCGETS reports the line as it is rather than as it was asked to be. */
static void stty_cflag_from_hw(struct serial_tty *t) {
  u32 baud = 0;
  u8 bits = 8, parity = 0, stop = 1;
  if (serial_port_get_line(t->com, &baud, &bits, &parity, &stop) < 0)
    return;
  u32 cflag = t->termios.c_cflag & ~(STTY_CBAUD | STTY_CSIZE | STTY_CSTOPB |
                                     STTY_PARENB | STTY_PARODD);
  cflag |= stty_rate_to_cbaud(baud);
  cflag |= ((u32)(bits - 5) << 4) & STTY_CSIZE;
  if (stop == 2) cflag |= STTY_CSTOPB;
  if (parity == 1) cflag |= STTY_PARENB | STTY_PARODD;
  else if (parity == 2) cflag |= STTY_PARENB;
  t->termios.c_cflag = cflag | STTY_CREAD | STTY_CLOCAL;
}

/* Apply the hardware half of a termios. Returns 0, or -EINVAL for a line
 * setting this UART cannot produce. A c_cflag with no baud bits (B0) leaves
 * the line alone, which is what a caller that never looked at c_cflag means. */
static int stty_apply_cflag(struct serial_tty *t, u32 cflag) {
  u32 rate = stty_cbaud_to_rate(cflag);
  if (rate == 0)
    return 0;
  u8 bits = (u8)(5 + ((cflag & STTY_CSIZE) >> 4));
  u8 stop = (cflag & STTY_CSTOPB) ? 2 : 1;
  u8 parity = 0;
  if (cflag & STTY_PARENB)
    parity = (cflag & STTY_PARODD) ? 1 : 2;
  if (serial_port_set_line(t->com, rate, bits, parity, stop) < 0)
    return -EINVAL;
  return 0;
}

/* Linux TIOCM_* bits, and where each one lives in the 16550. */
#define TIOCM_LE   0x001
#define TIOCM_DTR  0x002
#define TIOCM_RTS  0x004
#define TIOCM_CTS  0x020
#define TIOCM_CAR  0x040
#define TIOCM_RNG  0x080
#define TIOCM_DSR  0x100

static int stty_modem_get(struct serial_tty *t) {
  u8 mcr = serial_port_get_mcr(t->com);
  u8 msr = serial_port_get_msr(t->com);
  int bits = 0;
  if (mcr & 0x01) bits |= TIOCM_DTR;
  if (mcr & 0x02) bits |= TIOCM_RTS;
  if (msr & 0x10) bits |= TIOCM_CTS;
  if (msr & 0x20) bits |= TIOCM_DSR;
  if (msr & 0x40) bits |= TIOCM_RNG;
  if (msr & 0x80) bits |= TIOCM_CAR;
  return bits;
}

static void stty_modem_set(struct serial_tty *t, int bits) {
  u8 mcr = (u8)(serial_port_get_mcr(t->com) & ~0x03);
  if (bits & TIOCM_DTR) mcr |= 0x01;
  if (bits & TIOCM_RTS) mcr |= 0x02;
  serial_port_set_mcr(t->com, mcr);
}

/* struct serial_struct, as setserial reads it. Only the fields setserial
 * prints are filled; the rest are zero, which is what "not configured" looks
 * like on Linux too. */
struct stty_serial_struct {
  int type;
  int line;
  unsigned int port;
  int irq;
  int flags;
  int xmit_fifo_size;
  int custom_divisor;
  int baud_base;
  unsigned short close_delay;
  char io_type;
  char reserved_char;
  int hub6;
  unsigned short closing_wait;
  unsigned short closing_wait2;
  unsigned char *iomem_base;
  unsigned short iomem_reg_shift;
  unsigned int port_high;
  unsigned long iomap_base;
};

#define STTY_PORT_16550A 3
#define STTY_TIOCMGET 0x5415
#define STTY_TIOCMBIS 0x5416
#define STTY_TIOCMBIC 0x5417
#define STTY_TIOCMSET 0x5418
#define STTY_TIOCGSERIAL 0x541E
#define STTY_TIOCSSERIAL 0x541F

static int stty_ioctl(struct vfs_handle *h, u64 request, void *arg) {
  struct serial_tty *t = (struct serial_tty *)h->private_data;
  if (!t)
    return -EINVAL;

  switch (request) {
  case B1NIX_TCGETS:
    return tty_termios_copyout(arg, &t->termios);
  /* TCGETS2 is the same question in the layout glibc 2.42 and later ask it in.
   * A serial line is where the extra c_ispeed/c_ospeed words actually mean
   * something, and they are filled from the rate the UART is running at. */
  case B1NIX_TCGETS2:
    return tty_termios2_copyout(arg, &t->termios);
  case B1NIX_TCSETS:
  case B1NIX_TCSETSW: /* TCSADRAIN — no output buffering, so same as TCSETS */
  case B1NIX_TCSETSF: /* TCSAFLUSH — no input queue to flush here either */
  case B1NIX_TCSETS2:
  case B1NIX_TCSETSW2:
  case B1NIX_TCSETSF2:
  {
    struct b1nix_termios want = t->termios;
    {
      int cin = tty_is_termios2_set(request)
                    ? tty_termios2_copyin(&want, arg)
                    : tty_termios_copyin(&want, arg);
      if (cin < 0)
        return cin;
    }
    struct b1nix_termios saved = t->termios;
    t->termios = want;
    /* Only c_cflag is the chip's business. A controller that cannot be
     * reprogrammed -- a PL011 has no divisor latch this kernel can reach, a
     * mini-UART's divisor follows a clock it cannot read -- keeps the rate it
     * has; the line discipline is software and applies regardless.
     *
     * Throwing the whole structure away on that refusal is what made ttyS0
     * permanently canonical on this arch: every tcsetattr, including the one
     * that only wanted to clear ICANON and ECHO, was undone because the UART
     * would not change its baud rate. A raw read then waited forever for a
     * newline that a raw writer was never going to send, and the smoke lane
     * sat there for 400 seconds.
     *
     * POSIX agrees: tcsetattr succeeds if it applied any of what was asked,
     * and the caller is expected to tcgetattr to see what stuck -- which is
     * exactly what stty_cflag_from_hw makes truthful below. */
    if (stty_apply_cflag(t, want.c_cflag) < 0)
      t->termios.c_cflag = saved.c_cflag;
    /* Report the line as the chip now holds it — a rate the divisor rounded
     * or a field this UART ignores must not come back as if it had stuck. */
    stty_cflag_from_hw(t);
    return 0;
  }
  case B1NIX_TIOCGWINSZ:
    if (!arg || syscall_copyout(arg, &t->winsize, sizeof(t->winsize)) < 0)
      return -EFAULT;
    return 0;
  case B1NIX_TIOCSWINSZ:
    if (!arg || syscall_copyin(&t->winsize, arg, sizeof(t->winsize)) < 0)
      return -EFAULT;
    if (t->fg_pgrp)
      scheduler_kill_process_group(t->fg_pgrp, SIGWINCH);
    return 0;
  case B1NIX_TIOCGPGRP: {
    /* The user buffer is a pid_t (32-bit) — copying sizeof(usize) would
     * read/write 4 bytes of adjacent user stack on x86_64. */
    int fg32 = (int)t->fg_pgrp;
    if (!arg || syscall_copyout(arg, &fg32, sizeof(fg32)) < 0)
      return -EFAULT;
    return 0;
  }
  case B1NIX_TIOCSPGRP: {
    int fg32;
    if (!arg || syscall_copyin(&fg32, arg, sizeof(fg32)) < 0)
      return -EFAULT;
    t->fg_pgrp = (usize)fg32;
    return 0;
  }
  case B1NIX_TIOCSCTTY:
    /* A session leader (getty after setsid) claims the tty. */
    if (current_task) {
      t->session_id = current_task->session_id;
      t->fg_pgrp = current_task->process_group_id;
      if (current_task->session_id == current_task->id) {
        scheduler_set_ctty(current_task, 2, 0); /* COM1 (index 0) */
      }
    }
    return 0;
  case B1NIX_TIOCNOTTY:
    return 0;
  case STTY_TIOCMGET: { /* modem lines, read from MCR + MSR */
    int bits = stty_modem_get(t);
    if (!arg || syscall_copyout(arg, &bits, sizeof(bits)) < 0)
      return -EFAULT;
    return 0;
  }
  case STTY_TIOCMSET:
  case STTY_TIOCMBIS:
  case STTY_TIOCMBIC: {
    int bits;
    if (!arg || syscall_copyin(&bits, arg, sizeof(bits)) < 0)
      return -EFAULT;
    int cur = stty_modem_get(t);
    if (request == STTY_TIOCMBIS)
      bits = cur | bits;
    else if (request == STTY_TIOCMBIC)
      bits = cur & ~bits;
    stty_modem_set(t, bits);
    return 0;
  }
  case STTY_TIOCGSERIAL: { /* setserial -g */
    struct stty_serial_struct ss;
    memset(&ss, 0, sizeof(ss));
    ss.type = STTY_PORT_16550A;
    ss.line = t->com;
    ss.port = serial_port_base(t->com);
    ss.irq = (t->com == 0) ? 4 : 3; /* the PC's fixed COM1/COM2 lines */
    ss.xmit_fifo_size = 16;
    ss.baud_base = 115200;
    {
      u32 baud = 0;
      serial_port_get_line(t->com, &baud, 0, 0, 0);
      ss.custom_divisor = baud ? (int)(115200u / baud) : 0;
    }
    ss.io_type = 0; /* SERIAL_IO_PORT */
    if (!arg || syscall_copyout(arg, &ss, sizeof(ss)) < 0)
      return -EFAULT;
    return 0;
  }
  case STTY_TIOCSSERIAL:
    /* The port address, IRQ and clock of a legacy COM line are fixed by the
     * platform, so there is nothing here for setserial to change. Accepting
     * the call and quietly keeping the old values would be a lie. */
    return -EPERM;
  case B1NIX_TIOCSTI: {
    /* Linux TIOCSTI: inject one byte into the input queue as if typed —
     * travels the full line discipline (canon/ISIG/VEOF) like UART input. */
    char c;
    if (!arg || syscall_copyin(&c, arg, 1) < 0)
      return -EFAULT;
    stty_input_char(t, (u8)c);
    scheduler_wake_all(vfs_poll_chan);
    return 0;
  }
  default:
    return -ENOTTY;
  }
}

static void stty_release(struct vfs_handle *h) {
  struct serial_tty *t = (struct serial_tty *)h->private_data;
  if (!t)
    return;
  if (t->open_count > 0)
    t->open_count--;
  /* Last handle gone (session fully exited): drop the claim so COM1 input
   * falls back to the boot console until the next getty respawn opens us. */
  if (t->open_count == 0)
    stty_reset(t);
  h->private_data = 0;
}

static const struct vfs_file_ops stty_ops = {
  .read = stty_read,
  .write = stty_write,
  .poll = stty_poll,
  .release = stty_release,
  .ioctl = stty_ioctl,
};

/* ── public API ── */

int serial_tty_present(int idx) {
  if (idx < 0 || idx >= SERIAL_NPORTS)
    return 0;
  return sttys[idx].registered;
}

int serial_tty_claimed(int idx) {
  if (idx < 0 || idx >= SERIAL_NPORTS)
    return 0;
  return sttys[idx].open_count > 0;
}

int serial_tty_open(int idx, int flags) {
  if (idx < 0 || idx >= SERIAL_NPORTS || !sttys[idx].registered)
    return -ENXIO;
  struct serial_tty *t = &sttys[idx];

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_SERIAL_TTY);
  if (!h)
    return -ENFILE;

  if (t->open_count == 0) {
    stty_reset(t);
    /* Sane default so ^C/job control work before an explicit TIOCSCTTY. */
    if (current_task) {
      t->fg_pgrp = current_task->process_group_id;
      t->session_id = current_task->session_id;
    }
  }
  t->open_count++;

  h->private_data = t;
  h->ops = &stty_ops;
  h->flags = flags;

  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    if (t->open_count > 0)
      t->open_count--;
    vfs_handle_release(h);
    return -EMFILE;
  }
  if (flags & B1NIX_O_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

/* Map a /dev path to a serial tty index; -1 if it is not one of ours. */
int serial_tty_path_index(const char *resolved_path) {
  for (int i = 0; i < SERIAL_NPORTS; i++) {
    if (!sttys[i].registered)
      continue;
    char full[16];
    strcpy(full, "/dev/");
    strcat(full, sttys[i].name);
    if (strcmp(resolved_path, full) == 0)
      return i;
  }
  return -1;
}

/* Drain pending UART RX through the line discipline. Runs on the BSP timer
 * tick (ISR context, single producer — same contract as the i8042 poll).
 * A tty only owns its UART receive side while open; otherwise COM1 bytes are
 * left for the merged boot console and COM2 bytes are left in the FIFO. */
void serial_tty_tick(void) {
  for (int i = 0; i < SERIAL_NPORTS; i++) {
    struct serial_tty *t = &sttys[i];
    if (!t->registered || t->open_count == 0)
      continue;
    char c;
    int got = 0;
    while ((c = serial_port_getc(t->com)) != 0) {
      stty_input_char(t, (u8)c);
      got = 1;
    }
    /* Readers blocked in select/poll sleep on vfs_poll_chan and are only
     * re-scanned on a wake (the m32b lost-wakeup contract) — without this,
     * a shell waiting in poll on the serial tty never sees its input.
     * scheduler_wake_all preserves the caller's IRQ state, so it is safe
     * from this ISR context (tcp_input wakes the same channel similarly). */
    if (got)
      scheduler_wake_all(vfs_poll_chan);
  }
}

/* Test hook (M39 self-test): feed bytes through the line discipline as if
 * they arrived from the UART, without real hardware input. */
void serial_tty_test_inject(int idx, const char *buf, usize n) {
  if (idx < 0 || idx >= SERIAL_NPORTS || !sttys[idx].registered)
    return;
  for (usize i = 0; i < n; i++)
    stty_input_char(&sttys[idx], (u8)buf[i]);
  scheduler_wake_all(vfs_poll_chan);
}

/* Expose per-tty state for the M39 self-test (termios/pgrp independence is
 * asserted against the boot console's globals). */
usize serial_tty_fg_pgrp(int idx) {
  if (idx < 0 || idx >= SERIAL_NPORTS)
    return 0;
  return sttys[idx].fg_pgrp;
}

void serial_tty_init(void) {
  memset(sttys, 0, sizeof(sttys));
  for (int i = 0; i < SERIAL_NPORTS; i++) {
    struct serial_tty *t = &sttys[i];
    t->com = i;
    t->name[0] = 't'; t->name[1] = 't'; t->name[2] = 'y'; t->name[3] = 'S';
    t->name[4] = (char)('0' + i);
    t->name[5] = '\0';
    stty_reset(t);
    if (serial_port_present(i))
      t->registered = 1;
  }
}

/* Create the /dev/ttySn nodes for detected ports. Called from vfs_init and
 * again from vfs_repopulate_after_root_mount (opens are intercepted by path
 * in vfs_open_flags; the nodes exist so stat()/ls resolve). */
void serial_tty_register_nodes(void) {
  for (int i = 0; i < SERIAL_NPORTS; i++) {
    if (!sttys[i].registered)
      continue;
    char path[16];
    strcpy(path, "/dev/");
    strcat(path, sttys[i].name);
    struct vfs_node *node = vfs_add_node(path, VFS_DEVICE, 0, 0, 0);
    if (node && !IS_ERR(node)) {
      node->inode->mode = 0620;
      node->inode->uid = 0;
      node->inode->gid = 5; /* group tty */
      vfs_node_put(node);
    }
  }
}
