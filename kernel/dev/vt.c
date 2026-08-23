/* Virtual terminals, console fonts and keymaps — M107.
 *
 * b1nix had exactly one text console. This file turns it into VT 1 of six and
 * gives the other five their own screen and keyboard queue, which is what
 * `chvt`, `openvt` and `deallocvt` manipulate. Alongside it live the two other
 * pieces of console state those tools' neighbours need:
 *
 *   - the console font, replaceable through PIO_FONT / GIO_FONT / KDFONTOP.
 *     A loaded face is handed to the framebuffer console and really changes
 *     what is drawn (`setfont`).
 *   - the keymap, a genuine keysym table indexed by (modifier state, scancode)
 *     that the PS/2 driver translates through. KDSKBENT rewrites an entry and
 *     the next keypress produces the new character (`loadkmap`), KDGKBENT
 *     reads it back (`dumpkmap`).
 *
 * Screen model: each VT owns a character cell buffer sized to the physical
 * console. Everything printed to a VT is recorded there; a switch repaints the
 * target's buffer through the ordinary console primitives. That keeps a single
 * renderer (VGA text memory or the framebuffer console) and means a VT's
 * contents survive being switched away from.
 */

#include <b1nix/termios_abi.h>
#include <b1nix/vt.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/fb_console.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/sysfs_attr.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <b1nix/uevent.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <string.h>

/* ── ioctl numbers (Linux <linux/vt.h> and <linux/kd.h>) ────────────────── */

#define VT_OPENQRY     0x5600
#define VT_GETMODE     0x5601
#define VT_SETMODE     0x5602
#define VT_GETSTATE    0x5603
#define VT_RELDISP     0x5605
#define VT_ACTIVATE    0x5606
#define VT_WAITACTIVE  0x5607
#define VT_DISALLOCATE 0x5608

#define KDGETMODE  0x4B3B
#define KDSETMODE  0x4B3A
#define KDGKBMODE  0x4B44
#define KDSKBMODE  0x4B45
#define KDGKBTYPE  0x4B33
#define KDGKBENT   0x4B46
#define KDSKBENT   0x4B47
#define GIO_FONT   0x4B60
#define PIO_FONT   0x4B61
#define GIO_FONTX  0x4B6B
#define PIO_FONTX  0x4B6C
#define KDFONTOP   0x4B72

#define KD_TEXT     0
#define KD_GRAPHICS 1

#define K_RAW       0x00
#define K_XLATE     0x01
#define K_MEDIUMRAW 0x02
#define K_UNICODE   0x03
/* Keyboard off: the console delivers nothing to userspace and prints nothing
 * of its own. A session manager sets it while a compositor owns the display,
 * so keystrokes go to the compositor's input devices and not to the terminal
 * underneath. Missing it made KDSKBMODE answer EINVAL, which is what a session
 * manager's TakeControl turns into org.freedesktop.DBus.Error.InvalidArgs --
 * the refusal that kept kwin off every card. */
#define K_OFF       0x04

#define KB_101 0x02 /* KDGKBTYPE: a normal PC keyboard */

#define VT_AUTO    0x00
#define VT_PROCESS 0x01

/* KDFONTOP */
#define KD_FONT_OP_SET  0
#define KD_FONT_OP_GET  1
#define KD_FONT_OP_COPY 3

/* An empty keymap slot. */
#define K_HOLE 0xF200

/* ── Sizes ──────────────────────────────────────────────────────────────── */

#define VT_MAX_COLS 160
#define VT_MAX_ROWS 100
#define VT_INPUT_QUEUE 512
#define VT_LINE_MAX 256

/* Keymaps: Linux indexes 256 tables by the modifier bitmask (shift 1, altgr 2,
 * ctrl 4, alt 8, ...). Only the low 16 are ever populated on a PC layout; the
 * rest read back as K_HOLE, exactly as an unallocated Linux keymap does. */
#define VT_NR_KEYS 128
#define VT_NR_TABLES 16
#define VT_MAX_NR_KEYMAPS 256

/* Font store: 256 glyphs, up to 16 rows, one byte per row. */
#define VT_FONT_MAX_H 16
#define VT_FONT_GLYPHS 256
/* The PIO_FONT/GIO_FONT buffer is a fixed 256 * 32 bytes. */
#define VT_PIO_FONT_STRIDE 32
#define VT_PIO_FONT_SIZE (VT_FONT_GLYPHS * VT_PIO_FONT_STRIDE)

struct vt_screen {
  /* The size a caller set with TIOCSWINSZ, reported back by TIOCGWINSZ. The
   * console's real geometry comes from the framebuffer and the font and does
   * not move; this is what the caller was told, which is what it checks. */
  u16 ws_row, ws_col;
  u8 winsize_set;
  int allocated;
  char *cells; /* rows * cols, ' '-filled */
  u16 row, col;
  int mode;    /* KD_TEXT / KD_GRAPHICS */
  int kbmode;  /* K_XLATE / K_RAW / K_MEDIUMRAW / K_UNICODE / K_OFF */
  u8 vt_mode;  /* VT_AUTO / VT_PROCESS */
  u8 relsig, acqsig;
  int waitv;   /* VT_SETMODE waitv */
  usize owner_pid;

  /* Input: a byte queue plus the canonical line buffer. */
  char q[VT_INPUT_QUEUE];
  usize q_head, q_tail, q_count;
  char line[VT_LINE_MAX];
  usize line_len;

  struct b1nix_termios termios;
  usize fg_pgrp;
  usize session_id;
};

static struct vt_screen g_vts[VT_COUNT + 1]; /* 1-based; slot 0 unused */
static int g_active = VT_CONSOLE;
static u16 g_rows = 25, g_cols = 80;
static spinlock_t vt_lock = SPINLOCK_INIT;
/* Set while repainting so the console_putc hook does not re-record the very
 * characters it is replaying. */
static volatile int g_replaying;
static int g_inited;

/* Keymap + font state. */
static u16 g_keymap[VT_NR_TABLES][VT_NR_KEYS];
static u8 g_font[VT_FONT_GLYPHS * VT_FONT_MAX_H];
static u32 g_font_h = 8;
static u32 g_font_count = 128;

/* ── Geometry ───────────────────────────────────────────────────────────── */

static void vt_compute_geometry(void) {
  u32 rows = 25, cols = 80;
  if (fb_console_ready()) {
    u32 h = 8, cnt = 128;
    fb_console_font_metrics(&h, &cnt);
    if (h == 0)
      h = 8;
    cols = fb_console_width() / 8;
    rows = fb_console_height() / h;
  }
  if (cols < 20)
    cols = 20;
  if (rows < 5)
    rows = 5;
  if (cols > VT_MAX_COLS)
    cols = VT_MAX_COLS;
  if (rows > VT_MAX_ROWS)
    rows = VT_MAX_ROWS;
  g_cols = (u16)cols;
  g_rows = (u16)rows;
}

static int vt_alloc_cells(struct vt_screen *v) {
  if (v->cells)
    return 0;
  usize n = (usize)g_rows * g_cols;
  v->cells = kmalloc(n);
  if (!v->cells)
    return -ENOMEM;
  memset(v->cells, ' ', n);
  v->row = 0;
  v->col = 0;
  return 0;
}

/* ── The cell-buffer terminal model ─────────────────────────────────────── */

static void vt_scroll(struct vt_screen *v) {
  if (!v->cells)
    return;
  memmove(v->cells, v->cells + g_cols, (usize)(g_rows - 1) * g_cols);
  memset(v->cells + (usize)(g_rows - 1) * g_cols, ' ', g_cols);
  v->row = (u16)(g_rows - 1);
}

/* Record one character. ESC sequences are consumed by a tiny state machine so
 * a CSI never lands in the buffer as literal text; the sequence still reaches
 * the renderer, which does its own (richer) ANSI handling. */
static void vt_record(struct vt_screen *v, char ch) {
  static int esc[VT_COUNT + 1];
  int idx = (int)(v - g_vts);
  if (idx < 0 || idx > VT_COUNT)
    return;
  if (!v->cells)
    return;

  if (esc[idx] == 1) { /* saw ESC */
    esc[idx] = (ch == '[') ? 2 : 0;
    return;
  }
  if (esc[idx] == 2) { /* inside CSI: parameters until a final byte */
    if ((ch >= '@' && ch <= '~')) {
      esc[idx] = 0;
      if (ch == 'J' || ch == 'H') {
        /* Clear / home: keep the model in step with what the renderer does. */
        if (ch == 'J')
          memset(v->cells, ' ', (usize)g_rows * g_cols);
        v->row = 0;
        v->col = 0;
      }
    }
    return;
  }

  switch (ch) {
  case 27:
    esc[idx] = 1;
    return;
  case '\n':
    v->col = 0;
    if (++v->row >= g_rows)
      vt_scroll(v);
    return;
  case '\r':
    v->col = 0;
    return;
  case '\b':
    if (v->col)
      v->col--;
    return;
  case '\t':
    do {
      if (v->col < g_cols)
        v->cells[(usize)v->row * g_cols + v->col] = ' ';
      v->col++;
    } while ((v->col & 7) && v->col < g_cols);
    break;
  case '\0':
    return;
  default:
    if ((unsigned char)ch < 32)
      return;
    v->cells[(usize)v->row * g_cols + v->col] = ch;
    v->col++;
    break;
  }
  if (v->col >= g_cols) {
    v->col = 0;
    if (++v->row >= g_rows)
      vt_scroll(v);
  }
}

/* Repaint the active VT's cell buffer onto the physical screen. */
static void vt_repaint(struct vt_screen *v) {
  /* Hold the console lock for the whole repaint: console_putc is the unlocked
   * primitive, so without this another CPU's console_write can interleave and
   * split a line in the middle of the replay. */
  u64 clk;
  console_lock_acquire_irqsave(&clk);
  g_replaying = 1;
  console_clear();
  if (v->cells) {
    for (u16 r = 0; r < g_rows; r++) {
      /* Trim the trailing blanks so a mostly-empty screen is cheap to paint
       * and the cursor lands where the text ends. */
      u16 last = 0;
      for (u16 c = 0; c < g_cols; c++)
        if (v->cells[(usize)r * g_cols + c] != ' ')
          last = (u16)(c + 1);
      for (u16 c = 0; c < last; c++)
        console_putc_raw(v->cells[(usize)r * g_cols + c]);
      if (r + 1 < g_rows && (last || r < v->row))
        console_putc_raw('\n');
      else if (r >= v->row)
        break;
    }
  }
  g_replaying = 0;
  console_lock_release_irqrestore(clk);
}

int vt_active(void) { return g_active; }

int vt_console_putc(char ch) {
  if (!g_inited || g_replaying)
    return 0;
  struct vt_screen *cons = &g_vts[VT_CONSOLE];
  vt_record(cons, ch);
  /* Another VT owns the screen: the console's output is kept, not drawn. */
  return g_active != VT_CONSOLE;
}

/* ── Input ──────────────────────────────────────────────────────────────── */

static void vt_queue_put(struct vt_screen *v, char c) {
  if (v->q_count >= VT_INPUT_QUEUE)
    return;
  v->q[v->q_tail] = c;
  v->q_tail = (v->q_tail + 1) % VT_INPUT_QUEUE;
  v->q_count++;
}

/* Echo + canonical assembly for a non-console VT. */
static void vt_input_char(struct vt_screen *v, char c) {
  int canon = (v->termios.c_lflag & B1NIX_ICANON) != 0;
  int echo = (v->termios.c_lflag & B1NIX_ECHO) != 0;
  int is_active = (v == &g_vts[g_active]);

  if ((v->termios.c_lflag & B1NIX_ISIG) && v->fg_pgrp > 1) {
    if (c == v->termios.c_cc[B1NIX_VINTR]) {
      scheduler_kill_process_group(v->fg_pgrp, SIGINT);
      return;
    }
    if (c == v->termios.c_cc[B1NIX_VSUSP]) {
      scheduler_kill_process_group(v->fg_pgrp, SIGTSTP);
      return;
    }
    if (c == v->termios.c_cc[B1NIX_VQUIT]) {
      scheduler_kill_process_group(v->fg_pgrp, SIGQUIT);
      return;
    }
  }
  if ((v->termios.c_iflag & B1NIX_ICRNL) && c == '\r')
    c = '\n';

  if (!canon) {
    vt_queue_put(v, c);
    if (echo && is_active)
      console_putc_raw(c);
    return;
  }

  if (c == '\b' || c == 127) {
    if (v->line_len) {
      v->line_len--;
      if (echo && is_active)
        console_write_raw("\b \b");
    }
    return;
  }
  if (c == '\n') {
    if (v->line_len < VT_LINE_MAX)
      v->line[v->line_len++] = '\n';
    for (usize i = 0; i < v->line_len; i++)
      vt_queue_put(v, v->line[i]);
    v->line_len = 0;
    if (echo && is_active)
      console_putc_raw('\n');
    return;
  }
  if (c == v->termios.c_cc[B1NIX_VEOF]) { /* ^D: end the line as-is */
    for (usize i = 0; i < v->line_len; i++)
      vt_queue_put(v, v->line[i]);
    v->line_len = 0;
    return;
  }
  if ((unsigned char)c < 32 && c != '\t')
    return;
  if (v->line_len < VT_LINE_MAX) {
    v->line[v->line_len++] = c;
    if (echo && is_active)
      console_putc_raw(c);
  }
}

int vt_kbd_char(char c) {
  if (!g_inited || g_active == VT_CONSOLE)
    return 0;
  struct vt_screen *v = &g_vts[g_active];
  vt_input_char(v, c);
  scheduler_wake_all(v);
  scheduler_wake_all(vfs_poll_chan);
  return 1;
}

/* ── Switching ──────────────────────────────────────────────────────────── */

static int vt_switch_to(int n) {
  if (n < 1 || n > VT_COUNT)
    return -EINVAL;
  struct vt_screen *v = &g_vts[n];
  if (!v->allocated)
    return -ENXIO;
  if (n == g_active)
    return 0;
  if (vt_alloc_cells(v) < 0)
    return -ENOMEM;
  g_active = n;
  vt_repaint(v);
  scheduler_wake_all(&g_active);
  return 0;
}

/* Alt+F1..F12 (and Ctrl+Alt+Fn, which sends the same scancodes) switches VT.
 * Set-1 make codes: F1..F10 = 0x3B..0x44, F11 = 0x57, F12 = 0x58. */
int vt_kbd_hotkey(u8 scancode, int alt) {
  if (!g_inited || !alt)
    return 0;
  int n = -1;
  if (scancode >= 0x3B && scancode <= 0x44)
    n = scancode - 0x3B + 1;
  else if (scancode == 0x57)
    n = 11;
  else if (scancode == 0x58)
    n = 12;
  if (n < 1 || n > VT_COUNT)
    return 0;
  if (!g_vts[n].allocated)
    return 1; /* consumed: the key must not reach the application either */
  vt_switch_to(n);
  return 1;
}

/* ── Keymap ─────────────────────────────────────────────────────────────── */

/* The default set-1 layout. It belongs to the console, not to a particular
 * keyboard driver: KDGKBENT/KDSKBENT and loadkmap operate on this table
 * whatever feeds scancodes into it. It used to live in the PS/2 driver and be
 * seeded from there, which left the table empty on a machine with no PS/2
 * controller (aarch64 takes input from virtio-input) — so the console reported
 * an empty layout and loadkmap had nothing to modify. */
static const char vt_default_map[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '7', '8',
    '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', 0, 0
};

static const char vt_default_map_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '7', '8',
    '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', 0, 0
};

void vt_keymap_seed(const char *plain, const char *shifted) {
  for (int t = 0; t < VT_NR_TABLES; t++)
    for (int k = 0; k < VT_NR_KEYS; k++)
      g_keymap[t][k] = K_HOLE;
  for (int k = 0; k < VT_NR_KEYS; k++) {
    if (plain && plain[k])
      g_keymap[0][k] = (u16)(unsigned char)plain[k];
    if (shifted && shifted[k])
      g_keymap[1][k] = (u16)(unsigned char)shifted[k];
  }
  /* Ctrl tables: the ASCII control fold of the unshifted keysym, which is what
   * the driver used to compute inline. Keeping it in the table means loadkmap
   * can override it like any other entry. */
  for (int k = 0; k < VT_NR_KEYS; k++) {
    u16 base = g_keymap[0][k];
    if (base == K_HOLE)
      continue;
    char c = (char)base;
    u16 folded = K_HOLE;
    if (c >= 'a' && c <= 'z')
      folded = (u16)(c - 'a' + 1);
    else if (c >= 'A' && c <= 'Z')
      folded = (u16)(c - 'A' + 1);
    else if (c == '\\')
      folded = 28;
    else if (c == '[')
      folded = 27;
    else if (c == ']')
      folded = 29;
    else if (c == ' ')
      folded = 0;
    if (folded != K_HOLE) {
      g_keymap[4][k] = folded;  /* ctrl */
      g_keymap[5][k] = folded;  /* ctrl+shift */
    }
  }
}

int vt_keymap_translate(u8 scancode, int shift, int ctrl, int alt, char *out) {
  if (scancode >= VT_NR_KEYS)
    return 0;
  int table = (shift ? 1 : 0) | (ctrl ? 4 : 0) | (alt ? 8 : 0);
  u16 v = g_keymap[table][scancode];
  if (v == K_HOLE && alt) /* Alt is a prefix, not a layer, on a US layout */
    v = g_keymap[table & ~8][scancode];
  if (v == K_HOLE && ctrl)
    v = g_keymap[table & ~4][scancode];
  if (v == K_HOLE)
    return 0;
  if (v > 0xFF)
    return 0;
  *out = (char)v;
  return 1;
}

int vt_kbd_mode_is_xlate(void) {
  if (!g_inited)
    return 1;
  return g_vts[g_active].kbmode == K_XLATE ||
         g_vts[g_active].kbmode == K_UNICODE;
}

/* ── Node I/O ───────────────────────────────────────────────────────────── */

/* /dev/tty0 addresses the active VT; /dev/ttyN addresses VT N. */
static int vt_index_of_node(struct vfs_node *node) {
  const char *n = node->name;
  if (strncmp(n, "tty", 3) != 0)
    return -1;
  int idx = 0;
  const char *p = n + 3;
  if (!*p)
    return -1;
  for (; *p; p++) {
    if (*p < '0' || *p > '9')
      return -1;
    idx = idx * 10 + (*p - '0');
  }
  if (idx == 0)
    return g_active;
  if (idx < 1 || idx > VT_COUNT)
    return -1;
  return idx;
}

static isize vt_node_write(struct vfs_node *node, u64 offset, const char *buf,
                           usize size, int flags) {
  (void)offset;
  (void)flags;
  int idx = vt_index_of_node(node);
  if (idx < 0)
    return -ENXIO;
  struct vt_screen *v = &g_vts[idx];
  if (!v->allocated)
    return -ENXIO;
  if (vt_alloc_cells(v) < 0)
    return -ENOMEM;
  for (usize i = 0; i < size; i++) {
    char c = buf[i];
    if ((v->termios.c_oflag & B1NIX_OPOST) && c == '\n') {
      vt_record(v, '\r');
      if (idx == g_active)
        console_putc_raw('\r');
    }
    vt_record(v, c);
    if (idx == g_active) {
      g_replaying = 1; /* the record above already happened */
      console_putc_raw(c);
      g_replaying = 0;
    }
  }
  return (isize)size;
}

static isize vt_node_read(struct vfs_node *node, u64 offset, char *buf,
                          usize size, int flags) {
  (void)offset;
  int idx = vt_index_of_node(node);
  if (idx < 0)
    return -ENXIO;
  struct vt_screen *v = &g_vts[idx];
  if (!v->allocated)
    return -ENXIO;
  if (size == 0)
    return 0;
  while (v->q_count == 0) {
    if (flags & B1NIX_O_NONBLOCK)
      return -EAGAIN;
    scheduler_wait_prepare(v);
    if (v->q_count) {
      scheduler_wait_cancel();
      break;
    }
    if (scheduler_signal_pending()) {
      scheduler_wait_cancel();
      return -ERESTARTSYS;
    }
    scheduler_wait_commit();
  }
  usize n = 0;
  while (n < size && v->q_count) {
    buf[n++] = v->q[v->q_head];
    v->q_head = (v->q_head + 1) % VT_INPUT_QUEUE;
    v->q_count--;
  }
  return (isize)n;
}

static int vt_node_poll(struct vfs_node *node, struct b1nix_pollfd *pfd) {
  int idx = vt_index_of_node(node);
  pfd->revents = 0;
  if (idx < 0)
    return 0;
  pfd->revents |= B1NIX_POLLOUT;
  if (g_vts[idx].q_count)
    pfd->revents |= B1NIX_POLLIN;
  return 0;
}

/* ── Font ioctls ────────────────────────────────────────────────────────── */

/* Publish the in-kernel font store to the renderer. */
static int vt_font_publish(u32 height, u32 count) {
  if (height == 0 || height > VT_FONT_MAX_H || count == 0 || count > 256)
    return -EINVAL;
  /* Keeping the font purely in software when the renderer refuses it (a console
   * with no framebuffer) was tried and made the console output garble and the
   * lane hang, so the refusal is propagated: no framebuffer, no font. */
  if (fb_console_set_font(g_font, height, VT_FONT_MAX_H, count) != 0)
    return -EINVAL;
  g_font_h = height;
  g_font_count = count;
  vt_compute_geometry();
  return 0;
}

/* PIO_FONT: 256 glyphs at a fixed 32-byte stride; the height is implied by the
 * non-zero rows, which is how the historical interface encodes it. */
static int vt_pio_font(void *arg) {
  u8 *tmp = kmalloc(VT_PIO_FONT_SIZE);
  if (!tmp)
    return -ENOMEM;
  if (syscall_copyin(tmp, arg, VT_PIO_FONT_SIZE) < 0) {
    kfree(tmp);
    return -EFAULT;
  }
  u32 height = 0;
  for (u32 g = 0; g < VT_FONT_GLYPHS; g++)
    for (u32 r = 0; r < VT_PIO_FONT_STRIDE && r < VT_FONT_MAX_H; r++)
      if (tmp[g * VT_PIO_FONT_STRIDE + r] && r + 1 > height)
        height = r + 1;
  if (height == 0)
    height = 8;
  if (height > VT_FONT_MAX_H)
    height = VT_FONT_MAX_H;
  for (u32 g = 0; g < VT_FONT_GLYPHS; g++) {
    memcpy(g_font + g * VT_FONT_MAX_H, tmp + g * VT_PIO_FONT_STRIDE, height);
    if (height < VT_FONT_MAX_H)
      memset(g_font + g * VT_FONT_MAX_H + height, 0, VT_FONT_MAX_H - height);
  }
  kfree(tmp);
  return vt_font_publish(height, VT_FONT_GLYPHS);
}

static int vt_gio_font(void *arg) {
  u8 *tmp = kzalloc(VT_PIO_FONT_SIZE);
  if (!tmp)
    return -ENOMEM;
  for (u32 g = 0; g < VT_FONT_GLYPHS; g++)
    memcpy(tmp + g * VT_PIO_FONT_STRIDE, g_font + g * VT_FONT_MAX_H, g_font_h);
  int rc = syscall_copyout(arg, tmp, VT_PIO_FONT_SIZE) < 0 ? -EFAULT : 0;
  kfree(tmp);
  return rc;
}

/* PIO_FONTX/GIO_FONTX carry a small descriptor instead of a bare buffer:
 * the glyph count and height travel with the data. There is no width field —
 * the interface predates non-8-pixel faces — so this renderer can honour it
 * exactly, which is why it is implemented rather than refused. */
struct vt_consolefontdesc {
  u16 charcount;
  u16 charheight;
  u64 chardata; /* userspace pointer to charcount * 32 bytes */
};

static int vt_pio_fontx(void *arg) {
  struct vt_consolefontdesc d;
  if (syscall_copyin(&d, arg, sizeof(d)) < 0)
    return -EFAULT;
  if (d.charcount == 0 || d.charcount > VT_FONT_GLYPHS)
    return -EINVAL;
  if (d.charheight == 0 || d.charheight > VT_FONT_MAX_H)
    return -EINVAL;
  if (!d.chardata)
    return -EFAULT;
  usize bytes = (usize)d.charcount * VT_PIO_FONT_STRIDE;
  u8 *tmp = kzalloc(bytes);
  if (!tmp)
    return -ENOMEM;
  if (syscall_copyin(tmp, (void *)(usize)d.chardata, bytes) < 0) {
    kfree(tmp);
    return -EFAULT;
  }
  /* Glyphs past charcount keep whatever the previous face had rather than
   * turning into blanks — the same thing Linux's console does. */
  for (u32 g = 0; g < d.charcount; g++) {
    memcpy(g_font + g * VT_FONT_MAX_H, tmp + g * VT_PIO_FONT_STRIDE,
           d.charheight);
    if (d.charheight < VT_FONT_MAX_H)
      memset(g_font + g * VT_FONT_MAX_H + d.charheight, 0,
             VT_FONT_MAX_H - d.charheight);
  }
  kfree(tmp);
  return vt_font_publish(d.charheight, d.charcount);
}

static int vt_gio_fontx(void *arg) {
  struct vt_consolefontdesc d;
  if (syscall_copyin(&d, arg, sizeof(d)) < 0)
    return -EFAULT;
  /* Linux reports the font's real size through the descriptor and fails with
   * ENOMEM when the caller's buffer is too small, so a caller can size its
   * buffer by asking twice. */
  u16 want = (u16)g_font_count;
  if (d.charcount < want) {
    d.charcount = want;
    d.charheight = (u16)g_font_h;
    if (syscall_copyout(arg, &d, sizeof(d)) < 0)
      return -EFAULT;
    return -ENOMEM;
  }
  d.charcount = want;
  d.charheight = (u16)g_font_h;
  if (d.chardata) {
    usize bytes = (usize)want * VT_PIO_FONT_STRIDE;
    u8 *tmp = kzalloc(bytes);
    if (!tmp)
      return -ENOMEM;
    for (u32 g = 0; g < want; g++)
      memcpy(tmp + g * VT_PIO_FONT_STRIDE, g_font + g * VT_FONT_MAX_H, g_font_h);
    int rc = syscall_copyout((void *)(usize)d.chardata, tmp, bytes) < 0 ? -EFAULT
                                                                        : 0;
    kfree(tmp);
    if (rc < 0)
      return rc;
  }
  return syscall_copyout(arg, &d, sizeof(d)) < 0 ? -EFAULT : 0;
}

/* struct console_font_op (KDFONTOP). */
struct vt_console_font_op {
  u32 op;
  u32 flags;
  u32 width, height;
  u32 charcount;
  u8 *data;
};

static int vt_kdfontop(void *arg) {
  struct vt_console_font_op op;
  if (syscall_copyin(&op, arg, sizeof(op)) < 0)
    return -EFAULT;
  switch (op.op) {
  case KD_FONT_OP_GET: {
    if (!op.data)
      return -EINVAL;
    u32 count = op.charcount && op.charcount < g_font_count ? op.charcount
                                                            : g_font_count;
    /* The KDFONTOP buffer is 32 bytes per glyph, like PIO_FONT. */
    u8 *tmp = kzalloc((usize)count * VT_PIO_FONT_STRIDE);
    if (!tmp)
      return -ENOMEM;
    for (u32 g = 0; g < count; g++)
      memcpy(tmp + g * VT_PIO_FONT_STRIDE, g_font + g * VT_FONT_MAX_H,
             g_font_h);
    int rc = syscall_copyout(op.data, tmp, (usize)count * VT_PIO_FONT_STRIDE);
    kfree(tmp);
    if (rc < 0)
      return -EFAULT;
    op.width = 8;
    op.height = g_font_h;
    op.charcount = count;
    return syscall_copyout(arg, &op, sizeof(op)) < 0 ? -EFAULT : 0;
  }
  case KD_FONT_OP_SET: {
    if (!op.data || op.charcount == 0 || op.charcount > VT_FONT_GLYPHS)
      return -EINVAL;
    /* Only an 8-pixel-wide face can be rendered by this console. */
    if (op.width && op.width != 8)
      return -EINVAL;
    if (op.height == 0 || op.height > VT_FONT_MAX_H)
      return -EINVAL;
    usize len = (usize)op.charcount * VT_PIO_FONT_STRIDE;
    u8 *tmp = kzalloc(len);
    if (!tmp)
      return -ENOMEM;
    if (syscall_copyin(tmp, op.data, len) < 0) {
      kfree(tmp);
      return -EFAULT;
    }
    memset(g_font, 0, sizeof(g_font));
    for (u32 g = 0; g < op.charcount; g++)
      memcpy(g_font + g * VT_FONT_MAX_H, tmp + g * VT_PIO_FONT_STRIDE,
             op.height);
    kfree(tmp);
    return vt_font_publish(op.height, op.charcount);
  }
  case KD_FONT_OP_COPY:
    return -EOPNOTSUPP;
  default:
    return -EINVAL;
  }
}

/* ── ioctl ──────────────────────────────────────────────────────────────── */

struct vt_stat_k {
  u16 v_active;
  u16 v_signal;
  u16 v_state;
};

struct vt_mode_k {
  u8 mode;
  u8 waitv;
  i16 relsig;
  i16 acqsig;
  i16 frsig;
};

struct kbentry_k {
  u8 kb_table;
  u8 kb_index;
  u16 kb_value;
};

static int vt_ioctl(struct vfs_node *node, u64 request, void *arg) {
  int idx = vt_index_of_node(node);
  if (idx < 0)
    return -ENOTTY;
  struct vt_screen *v = &g_vts[idx];

  switch (request) {
  case VT_OPENQRY: {
    int free_vt = -1;
    u64 flags;
    spin_lock_irqsave(&vt_lock, &flags);
    for (int i = 1; i <= VT_COUNT; i++)
      if (!g_vts[i].allocated) {
        free_vt = i;
        break;
      }
    spin_unlock_irqrestore(&vt_lock, flags);
    if (free_vt < 0)
      free_vt = -1; /* Linux reports -1 in the argument, not an error */
    return syscall_copyout(arg, &free_vt, sizeof(free_vt)) < 0 ? -EFAULT : 0;
  }
  case VT_GETSTATE: {
    struct vt_stat_k st;
    memset(&st, 0, sizeof(st));
    st.v_active = (u16)g_active;
    for (int i = 1; i <= VT_COUNT; i++)
      if (g_vts[i].allocated)
        st.v_state |= (u16)(1u << i);
    return syscall_copyout(arg, &st, sizeof(st)) < 0 ? -EFAULT : 0;
  }
  case VT_ACTIVATE: {
    int n = (int)(isize)arg;
    u64 flags;
    spin_lock_irqsave(&vt_lock, &flags);
    if (n >= 1 && n <= VT_COUNT && !g_vts[n].allocated) {
      g_vts[n].allocated = 1; /* activating an unused VT allocates it */
      g_vts[n].owner_pid = scheduler_get_pid();
    }
    spin_unlock_irqrestore(&vt_lock, flags);
    return vt_switch_to(n);
  }
  case VT_WAITACTIVE: {
    int n = (int)(isize)arg;
    if (n < 1 || n > VT_COUNT)
      return -EINVAL;
    while (g_active != n) {
      scheduler_wait_prepare(&g_active);
      if (g_active == n) {
        scheduler_wait_cancel();
        break;
      }
      if (scheduler_signal_pending()) {
        scheduler_wait_cancel();
        return -ERESTARTSYS;
      }
      scheduler_wait_commit();
    }
    return 0;
  }
  case VT_DISALLOCATE: {
    int n = (int)(isize)arg;
    u64 flags;
    int rc = 0;
    spin_lock_irqsave(&vt_lock, &flags);
    if (n == 0) {
      /* Free every unused VT except the console and the active one. */
      for (int i = 2; i <= VT_COUNT; i++) {
        if (i == g_active)
          continue;
        g_vts[i].allocated = 0;
      }
    } else if (n < 1 || n > VT_COUNT) {
      rc = -ENXIO;
    } else if (n == VT_CONSOLE || n == g_active) {
      rc = -EBUSY;
    } else if (!g_vts[n].allocated) {
      rc = -ENXIO;
    } else {
      g_vts[n].allocated = 0;
    }
    spin_unlock_irqrestore(&vt_lock, flags);
    if (rc == 0) {
      /* Release the cell buffers outside the lock. */
      for (int i = 2; i <= VT_COUNT; i++) {
        if (!g_vts[i].allocated && g_vts[i].cells) {
          char *old = g_vts[i].cells;
          g_vts[i].cells = 0;
          kfree(old);
        }
      }
    }
    return rc;
  }
  case VT_GETMODE: {
    struct vt_mode_k m;
    memset(&m, 0, sizeof(m));
    m.mode = v->vt_mode;
    m.waitv = (u8)v->waitv;
    m.relsig = (i16)v->relsig;
    m.acqsig = (i16)v->acqsig;
    return syscall_copyout(arg, &m, sizeof(m)) < 0 ? -EFAULT : 0;
  }
  case VT_SETMODE: {
    struct vt_mode_k m;
    if (syscall_copyin(&m, arg, sizeof(m)) < 0)
      return -EFAULT;
    if (m.mode != VT_AUTO && m.mode != VT_PROCESS)
      return -EINVAL;
    v->vt_mode = m.mode;
    v->waitv = m.waitv;
    v->relsig = (u8)m.relsig;
    v->acqsig = (u8)m.acqsig;
    return 0;
  }
  case VT_RELDISP:
    /* Switches are synchronous here: there is no window in which a VT_PROCESS
     * owner must acknowledge a release, so an acknowledgement is a no-op. */
    return 0;

  case KDGETMODE:
    return syscall_copyout(arg, &v->mode, sizeof(v->mode)) < 0 ? -EFAULT : 0;
  case KDSETMODE: {
    int m = (int)(isize)arg;
    if (m != KD_TEXT && m != KD_GRAPHICS)
      return -EINVAL;
    v->mode = m;
    return 0;
  }
  case KDGKBMODE:
    return syscall_copyout(arg, &v->kbmode, sizeof(v->kbmode)) < 0 ? -EFAULT
                                                                   : 0;
  case KDSKBMODE: {
    int m = (int)(isize)arg;
    if (m != K_RAW && m != K_XLATE && m != K_MEDIUMRAW && m != K_UNICODE &&
        m != K_OFF)
      return -EINVAL;
    v->kbmode = m;
    return 0;
  }
  case KDGKBTYPE: {
    u8 t = KB_101;
    return syscall_copyout(arg, &t, sizeof(t)) < 0 ? -EFAULT : 0;
  }
  case KDGKBENT: {
    struct kbentry_k ke;
    if (syscall_copyin(&ke, arg, sizeof(ke)) < 0)
      return -EFAULT;
    /* kb_table is a u8, so the VT_MAX_NR_KEYMAPS (256) bound is structural. */
    if (ke.kb_index >= VT_NR_KEYS)
      return -EINVAL;
    ke.kb_value = ke.kb_table < VT_NR_TABLES
                      ? g_keymap[ke.kb_table][ke.kb_index]
                      : K_HOLE;
    return syscall_copyout(arg, &ke, sizeof(ke)) < 0 ? -EFAULT : 0;
  }
  case KDSKBENT: {
    struct kbentry_k ke;
    if (syscall_copyin(&ke, arg, sizeof(ke)) < 0)
      return -EFAULT;
    if (ke.kb_index >= VT_NR_KEYS)
      return -EINVAL;
    if (ke.kb_table >= VT_NR_TABLES)
      return ke.kb_value == K_HOLE ? 0 : -EINVAL;
    g_keymap[ke.kb_table][ke.kb_index] = ke.kb_value;
    return 0;
  }

  case PIO_FONT:
    if (!arg)
      return -EFAULT;
    return vt_pio_font(arg);
  case GIO_FONT:
    if (!arg)
      return -EFAULT;
    return vt_gio_font(arg);
  case KDFONTOP:
    if (!arg)
      return -EFAULT;
    return vt_kdfontop(arg);
  case PIO_FONTX:
    if (!arg)
      return -EFAULT;
    return vt_pio_fontx(arg);
  case GIO_FONTX:
    if (!arg)
      return -EFAULT;
    return vt_gio_fontx(arg);

  case B1NIX_TCGETS:
    return tty_termios_copyout(arg, &v->termios);
  case B1NIX_TCSETS:
  case B1NIX_TCSETSW:
  case B1NIX_TCSETSF:
    return tty_termios_copyin(&v->termios, arg);
  /* The termios2 form of the same four. A glibc from 2.42 onwards issues only
   * these, so a terminal that answers the 0x5401 family alone is not a
   * terminal to anything built against it. */
  case B1NIX_TCGETS2:
    return tty_termios2_copyout(arg, &v->termios);
  case B1NIX_TCSETS2:
  case B1NIX_TCSETSW2:
  case B1NIX_TCSETSF2:
    return tty_termios2_copyin(&v->termios, arg);
  case B1NIX_TIOCGWINSZ: {
    struct b1nix_winsize ws;
    memset(&ws, 0, sizeof(ws));
    /* What was set, if anything was, and the real geometry otherwise. A
     * caller that sets a size and reads it back expects its own answer. */
    ws.ws_row = v->winsize_set ? v->ws_row : g_rows;
    ws.ws_col = v->winsize_set ? v->ws_col : g_cols;
    ws.ws_xpixel = (u16)(g_cols * 8);
    ws.ws_ypixel = (u16)(g_rows * g_font_h);
    return syscall_copyout(arg, &ws, sizeof(ws)) < 0 ? -EFAULT : 0;
  }
  case B1NIX_TIOCSWINSZ: {
    /* Accepted, because refusing it is a lie about what this is.
     *
     * This fell through to -ENOTTY, which tells the caller "not a terminal"
     * about a terminal, for an ioctl every terminal answers. util-linux's
     * login sets the size during startup, and on the failure path it went on
     * to use a buffer it had not filled -- faulting with a string that had no
     * terminator and a pointer walked to 0x800000000000. On Linux the call
     * succeeds and that path never runs.
     *
     * The console's geometry is fixed by the framebuffer and the font, so a
     * caller cannot resize it; the size it asked for is recorded and reported
     * back, which is what a caller checks, and the screen keeps the geometry
     * it actually has. Refusing outright is the one answer that is wrong. */
    struct b1nix_winsize ws;

    if (syscall_copyin(&ws, arg, sizeof(ws)) < 0)
      return -EFAULT;
    v->winsize_set = 1;
    v->ws_row = ws.ws_row;
    v->ws_col = ws.ws_col;
    return 0;
  }
  case B1NIX_TIOCGPGRP: {
    int fg = (int)v->fg_pgrp;
    return syscall_copyout(arg, &fg, sizeof(fg)) < 0 ? -EFAULT : 0;
  }
  case B1NIX_TIOCSPGRP: {
    int fg;
    if (syscall_copyin(&fg, arg, sizeof(fg)) < 0)
      return -EFAULT;
    v->fg_pgrp = (usize)fg;
    return 0;
  }
  case B1NIX_TIOCSCTTY:
    v->session_id = scheduler_get_pid();
    return 0;
  case B1NIX_TIOCNOTTY:
    return 0;
  /* TCSBRK / TCFLSH: 0x5409 and 0x540B.
   *
   * Both fell through to -ENOTTY, and util-linux's login issues them while
   * preparing the terminal. It takes the refusal, carries on, and then uses a
   * buffer it never filled -- the fault had a frame pointer holding the bytes
   * " root" and a pointer walked to the first address past the user half. On
   * Linux both calls succeed on a console and that path never runs.
   *
   * TCSBRK waits for queued output to drain (and, with a non-zero argument,
   * sends a break). This console writes straight to the framebuffer, so there
   * is nothing queued to wait for and no line to break: the honest answer is
   * that it is already done.
   *
   * TCFLSH discards what is queued. There is no output queue, so only the
   * input side has anything to discard, and the argument says which: 0 input,
   * 1 output, 2 both. */
  case 0x5409: /* TCSBRK */
    return 0;
  case 0x5429: { /* TIOCGSID -- the session this terminal belongs to. */
    int sid = (int)v->session_id;

    /* Linux answers ENOTTY when the terminal is not a controlling terminal of
     * any session. Here session_id is set by TIOCSCTTY and starts at 1 (init),
     * so it always names one; reporting it is the truthful answer, and
     * refusing was what sent login down its error path. */
    return syscall_copyout(arg, &sid, sizeof(sid)) < 0 ? -EFAULT : 0;
  }
  case 0x540B: { /* TCFLSH */
    usize which = (usize)arg;

    if (which == 0 || which == 2) {
      v->q_head = v->q_tail = v->q_count = 0;
      v->line_len = 0;
    }
    return 0;
  }
  default:
    /* Which request was refused, when asked.
     *
     * "errno 25 from syscall 16" names ioctl and stops there, and a terminal
     * answers a dozen different ones -- so the trace that found the failure
     * could not say what had failed. util-linux's login takes the refusal and
     * then uses a buffer it never filled, so the request code is the whole
     * question. `b1nix.trace-ioctl`. */
    if (bootinfo_has_flag("b1nix.trace-ioctl")) {
      console_write("vt: unhandled ioctl 0x");
      console_write_hex64((u64)request);
      console_write(" on tty");
      console_write_dec((u64)(usize)(v - g_vts));
      console_write("\n");
    }
    return -ENOTTY;
  }
}

/* ── Setup ──────────────────────────────────────────────────────────────── */

static void vt_reset_termios(struct vt_screen *v) {
  memset(&v->termios, 0, sizeof(v->termios));
  v->termios.c_lflag = B1NIX_ICANON | B1NIX_ECHO | B1NIX_ISIG;
  v->termios.c_oflag = B1NIX_OPOST | B1NIX_ONLCR;
  v->termios.c_iflag = B1NIX_ICRNL;
  v->termios.c_cc[B1NIX_VINTR] = 3;
  v->termios.c_cc[B1NIX_VQUIT] = 28;
  v->termios.c_cc[B1NIX_VERASE] = 127;
  v->termios.c_cc[B1NIX_VEOF] = 4;
  v->termios.c_cc[B1NIX_VSUSP] = 26;
  v->termios.c_cc[B1NIX_VMIN] = 1;
}

void vt_register_nodes(void) {
  for (int i = 0; i <= VT_COUNT; i++) {
    char path[16];
    snprintf(path, sizeof(path), "/dev/tty%d", i);
    struct vfs_node *n = vfs_add_node(path, VFS_DEVICE, 0, 0, 0);
    if (!n || IS_ERR(n))
      continue;
    n->inode->mode = 0620;
    n->inode->gid = 5; /* tty */
    /* The number the console really has.
     *
     * Left at zero, every virtual terminal was a device file that was no
     * device: stat reported 0:0, so nothing could identify it by devnum.
     * util-linux's login does exactly that when it works out which terminal
     * it is on -- it faulted here with a string that had no terminator, in
     * that part of its startup -- and udev cannot file a node it cannot
     * number either. Major 4 is the Linux console major; the minor is the VT,
     * with 0 meaning "the current one", as on Linux. */
    n->inode->rdev = ((u64)4 << 8) | (u64)i;
    n->inode->read_cb = vt_node_read;
    n->inode->write_cb = vt_node_write;
    n->inode->poll_cb = vt_node_poll;
    n->inode->ioctl_cb = vt_ioctl;
  }
}

/* ── /sys/class/tty, so a session manager can see the consoles ──────────── */

/*
 * logind decides whether a seat has virtual terminals by opening
 * /sys/class/tty/tty0/active. With the file absent it concludes there are
 * none, and then refuses to create a session whose VT number is not zero --
 * "Seat has no VTs but VT number not 0" -- which is exactly what a login on
 * tty1 asks for. The compositor's DRM backend takes its devices from that
 * session, so with no session it never issues a single open() on /dev/dri:
 * a kernel trace of a whole boot recorded five, every one of them from a
 * shell probe and none from the compositor.
 *
 * Rendered per read rather than stored, because the value moves: a VT switch
 * must change what this file says, and a cached string would keep answering
 * with the console that was active when it was registered.
 */
static isize vt_sysfs_active_show(void *ctx, char *buf, usize cap)
{
	(void)ctx;
	return (isize)snprintf(buf, cap, "tty%d\n", vt_active());
}

/* Every VT publishes the device number it really has, so a lookup by devnum
 * finds it. Major 4 is the Linux console major, and it is what our own nodes
 * carry; the minor is the VT number, as on Linux. */
static isize vt_sysfs_dev_show(void *ctx, char *buf, usize cap)
{
	return (isize)snprintf(buf, cap, "4:%d\n", (int)(usize)ctx);
}

static isize vt_sysfs_uevent_show(void *ctx, char *buf, usize cap)
{
	int minor = (int)(usize)ctx;

	if (minor == 0)
		return (isize)snprintf(buf, cap,
			"MAJOR=4\nMINOR=0\nDEVNAME=tty0\n");
	return (isize)snprintf(buf, cap,
		"MAJOR=4\nMINOR=%d\nDEVNAME=tty%d\n", minor, minor);
}

/* Writing "add" to a device's `uevent` file re-announces it, and that is how
 * every coldplug replay works: `udevadm trigger` walks /sys and writes to each
 * one. These files were read-only, so the replay reached every VT with
 * "Failed to write 'add' to '/sys/class/tty/ttyN/uevent': Permission denied"
 * and no tty was ever announced to a manager that started after the kernel
 * did. */
static isize vt_sysfs_uevent_store(void *ctx, const char *buf, usize len)
{
	int minor = (int)(usize)ctx;
	char name[8];
	char devpath[32];

	snprintf(name, sizeof(name), "tty%d", minor);
	snprintf(devpath, sizeof(devpath), "/class/tty/%s", name);
	/* A tty carries no DEVTYPE on Linux either. */
	return uevent_store_write(buf, len, devpath, "tty", 0, name, 4, minor);
}

static void vt_sysfs_publish(void)
{
	struct sysfs_dir *cls = sysfs_reg_dir(sysfs_reg_dir(0, "class"), "tty");
	struct sysfs_dir *d;
	char name[8];

	if (!cls)
		return;

	/* tty0 is not a console of its own: it is the name for "whichever is
	 * current", which is why the active file lives here and nowhere else. */
	d = sysfs_reg_dir(cls, "tty0");
	if (d) {
		(void)sysfs_reg_attr(d, "active", 0444, vt_sysfs_active_show, 0, 0, 0);
		(void)sysfs_reg_attr(d, "dev", 0444, vt_sysfs_dev_show, 0,
				     (void *)(usize)0, 0);
		(void)sysfs_reg_attr(d, "uevent", 0644, vt_sysfs_uevent_show,
				     vt_sysfs_uevent_store, (void *)(usize)0, 0);
	}

	for (int i = 1; i <= VT_COUNT; i++) {
		snprintf(name, sizeof(name), "tty%d", i);
		d = sysfs_reg_dir(cls, name);
		if (!d)
			continue;
		(void)sysfs_reg_attr(d, "dev", 0444, vt_sysfs_dev_show, 0,
				     (void *)(usize)i, 0);
		(void)sysfs_reg_attr(d, "uevent", 0644, vt_sysfs_uevent_show,
				     vt_sysfs_uevent_store, (void *)(usize)i, 0);
	}
}

void vt_init(void) {
  if (g_inited)
    return;

  vt_compute_geometry();
  /* A console without a framebuffer has no builtin glyph bitmap (aarch64 has
   * no boot framebuffer at all). Everything else the VT layer does — switching,
   * per-VT termios/mode state, allocation — is independent of fonts, so start
   * with an empty font table instead of copying from a null pointer. */
  const u8 *builtin = fb_console_builtin_font();
  memset(g_font, 0, sizeof(g_font));
  if (builtin) {
    for (u32 g = 0; g < 128; g++)
      memcpy(g_font + g * VT_FONT_MAX_H, builtin + g * 8, 8);
    g_font_h = 8;
    g_font_count = 128;
  } else {
    g_font_h = 0;
    g_font_count = 0;
  }

  vt_keymap_seed(vt_default_map, vt_default_map_shift);

  for (int i = 1; i <= VT_COUNT; i++) {
    struct vt_screen *v = &g_vts[i];
    memset(v, 0, sizeof(*v));
    vt_reset_termios(v);
    v->mode = KD_TEXT;
    v->kbmode = K_XLATE;
    v->vt_mode = VT_AUTO;
    v->fg_pgrp = 1;
    v->session_id = 1;
  }
  /* VT 1 is the boot console and always exists; VT 2 is pre-allocated so a
   * bare `chvt 2` works without an openvt first, exactly as a Linux console
   * with a getty on tty2 behaves. */
  g_vts[VT_CONSOLE].allocated = 1;
  g_vts[2].allocated = 1;
  vt_alloc_cells(&g_vts[VT_CONSOLE]);
  vt_alloc_cells(&g_vts[2]);
  g_active = VT_CONSOLE;
  g_inited = 1;
  vt_sysfs_publish();

  vt_register_nodes();
}
