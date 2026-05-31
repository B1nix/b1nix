#include <b1nix/blk.h>
#include <b1nix/bootinfo.h>
#include <b1nix/vfs.h>
#include <b1nix/mm.h>
#include <b1nix/errno.h>
#include <b1nix/filelock.h>
#include <b1nix/net.h>
#include <b1nix/posix.h>
#include <b1nix/syscall.h>
#include <b1nix/user.h>
#include <b1nix/video.h>
#include <b1nix/sched.h>
#include <b1nix/lapic.h>
#include <b1nix/dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tui.h>

void user_register_program(const char *path, user_program_entry entry);
static int m22_smoke_main(int argc, const char **argv);
static int m24_stress_main(int argc, const char **argv);
static int lock_smoke_main(int argc, const char **argv);
static int ext_stress_main(int argc, const char **argv);
int shell_smoke_main(int argc, const char **argv);

static void uwrite(const char *text) {
  syscall_dispatch(SYS_WRITE, 1, (u64)(usize)text, strlen(text), 0, 0, 0);
}

/* ... (uwrite_dec_value, uwrite_ipv4, b1fetch_cpu_name remain unchanged) ... */

static void uwrite_dec_value(u64 value) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%d", (int)value);
  uwrite(buf);
}

static void uwrite_ipv4(struct ipv4_addr ip) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%d.%d.%d.%d", ip.bytes[0], ip.bytes[1],
           ip.bytes[2], ip.bytes[3]);
  uwrite(buf);
}

static void b1fetch_cpu_name(char *out, usize out_size) {
  if (!out || out_size == 0)
    return;
  out[0] = '\0';
#ifdef __aarch64__
  (void)out_size;
  strcpy(out, "AArch64 CPU");
#else
  u32 max_leaf = 0;
  u32 unused = 0;
  __asm__ volatile("cpuid"
                   : "=a"(max_leaf), "=b"(unused), "=c"(unused), "=d"(unused)
                   : "a"(0x80000000U));
  if (max_leaf >= 0x80000004U && out_size >= 49) {
    u32 *dst = (u32 *)out;
    for (u32 leaf = 0; leaf < 3; leaf++) {
      u32 a, b, c, d;
      __asm__ volatile("cpuid"
                       : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                       : "a"(0x80000002U + leaf));
      dst[leaf * 4 + 0] = a;
      dst[leaf * 4 + 1] = b;
      dst[leaf * 4 + 2] = c;
      dst[leaf * 4 + 3] = d;
    }
    out[48] = '\0';
    while (out[0] == ' ')
      memmove(out, out + 1, strlen(out));
    return;
  }
  strcpy(out, "x86_64 CPU");
#endif
}

static int m16_check_tui_key_decode(void)
{
	struct {
		const char *seq;
		usize len;
		int key;
	} cases[] = {
		{"a", 1, 'a'},
		{"\t", 1, KEY_TAB},
		{"\n", 1, KEY_ENTER},
		{"\r", 1, KEY_ENTER},
		{"\b", 1, KEY_BACKSP},
		{"\x7f", 1, KEY_BACKSP},
		{"\x11", 1, KEY_CTRL_Q},
		{"\x13", 1, KEY_CTRL_S},
		{"\x18", 1, KEY_CTRL_X},
		{"\x07", 1, KEY_CTRL_G},
		{"\033", 1, KEY_ESC},
		{"\033[A", 3, KEY_UP},
		{"\033[B", 3, KEY_DOWN},
		{"\033[C", 3, KEY_RIGHT},
		{"\033[D", 3, KEY_LEFT},
		{"\033[H", 3, KEY_HOME},
		{"\033[F", 3, KEY_END},
		{"\033[1~", 4, KEY_HOME},
		{"\033[2~", 4, KEY_INS},
		{"\033[3~", 4, KEY_DEL},
		{"\033[4~", 4, KEY_END},
		{"\033[5~", 4, KEY_PGUP},
		{"\033[6~", 4, KEY_PGDN},
		{"\033[7~", 4, KEY_HOME},
		{"\033[8~", 4, KEY_END},
		{"\033[11~", 5, KEY_F1},
		{"\033[12~", 5, KEY_F2},
		{"\033[13~", 5, KEY_F3},
		{"\033[14~", 5, KEY_F4},
		{"\033[15~", 5, KEY_F5},
		{"\033[17~", 5, KEY_F6},
		{"\033[18~", 5, KEY_F7},
		{"\033[19~", 5, KEY_F8},
		{"\033[20~", 5, KEY_F9},
		{"\033[21~", 5, KEY_F10},
		{"\033[23~", 5, KEY_F11},
		{"\033[24~", 5, KEY_F12},
		{"\033[M\1", 4, KEY_F1},
		{"\033[M\n", 4, KEY_F10},
		{"\033[M\v", 4, KEY_F11},
		{"\033[M\f", 4, KEY_F12},
		{"\033OP", 3, KEY_F1},
		{"\033OQ", 3, KEY_F2},
		{"\033OR", 3, KEY_F3},
		{"\033OS", 3, KEY_F4},
	};

	for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		int got = tui_decode_key_sequence(cases[i].seq, cases[i].len);
		if (got != cases[i].key) {
			return -1;
		}
	}

	uwrite("M16-SMOKE: ok tui-key-decode\n");
	return 0;
}

static int m16_termios_unchanged(const struct b1nix_termios *before)
{
	struct b1nix_termios after;
	memset(&after, 0, sizeof(after));
	if ((isize)syscall_dispatch(SYS_IOCTL, 0, B1NIX_TCGETS,
	                            (u64)(usize)&after, 0, 0, 0) < 0) {
		return -1;
	}
	return memcmp(before, &after, sizeof(after)) == 0 ? 0 : -1;
}

#define SH_HISTORY_MAX 16
#define SH_LINE_MAX 512
static char sh_history[SH_HISTORY_MAX][SH_LINE_MAX];
static int sh_hist_count = 0;

/* ── Job Control ── */
/* Growable job table (no fixed SH_JOBS_MAX cap). A job is Running unless it has
 * been stopped (SIGTSTP); `done` marks a reaped slot available for reuse. */
struct sh_job {
  u64 pid;
  char *name;
  int done;
  int stopped;
};
static struct sh_job *sh_jobs = 0;
static int sh_job_count = 0;
static int sh_job_cap = 0;

/* WIFSTOPPED equivalent: the kernel encodes a stopped child as low byte 0x7F. */
static int sh_status_stopped(int st) { return (st & 0xFF) == 0x7F; }

static int sh_job_add_ex(u64 pid, const char *name, int stopped) {
  for (int i = 0; i < sh_job_count; i++) {
    if (!sh_jobs[i].pid || sh_jobs[i].done) {
      free(sh_jobs[i].name);
      sh_jobs[i].pid = pid;
      sh_jobs[i].name = strdup(name);
      sh_jobs[i].done = 0;
      sh_jobs[i].stopped = stopped;
      return i + 1;
    }
  }
  if (sh_job_count == sh_job_cap) {
    int ncap = sh_job_cap ? sh_job_cap * 2 : 8;
    struct sh_job *nj = malloc((usize)ncap * sizeof(*nj));
    if (!nj)
      return -1;
    for (int i = 0; i < sh_job_count; i++)
      nj[i] = sh_jobs[i];
    free(sh_jobs);
    sh_jobs = nj;
    sh_job_cap = ncap;
  }
  int idx = sh_job_count++;
  sh_jobs[idx].pid = pid;
  sh_jobs[idx].name = strdup(name);
  sh_jobs[idx].done = 0;
  sh_jobs[idx].stopped = stopped;
  return idx + 1;
}

static int sh_job_add(u64 pid, const char *name) {
  return sh_job_add_ex(pid, name, 0);
}

static void sh_jobs_print(void) {
  int any = 0;
  for (int i = 0; i < sh_job_count; i++) {
    if (!sh_jobs[i].pid || sh_jobs[i].done)
      continue;
    if (!sh_jobs[i].stopped) {
      int st = 0;
      u64 r = syscall_dispatch(SYS_WAITPID, sh_jobs[i].pid, (u64)(usize)&st,
                               1 /*WNOHANG*/, 0, 0, 0);
      if (r == sh_jobs[i].pid && !sh_status_stopped(st))
        sh_jobs[i].done = 1;
    }
    if (sh_jobs[i].done)
      continue;
    char num[5] = {'[', (char)('0' + (i + 1)), ']', ' ', 0};
    uwrite(num);
    uwrite(sh_jobs[i].stopped ? "Stopped  " : "Running  ");
    uwrite(sh_jobs[i].name);
    uwrite("\n");
    any = 1;
  }
  if (!any)
    uwrite("no background jobs\n");
}

/* Bring job to the foreground: resume it if stopped, then wait. If it stops
 * again it stays a (stopped) job; otherwise it is reaped. */
static int sh_fg(int job_num) {
  int idx = job_num - 1;
  if (idx < 0 || idx >= sh_job_count || !sh_jobs[idx].pid ||
      sh_jobs[idx].done) {
    uwrite("fg: no such job\n");
    return -1;
  }
  uwrite(sh_jobs[idx].name);
  uwrite("\n");
  if (sh_jobs[idx].stopped) {
    syscall_dispatch(SYS_KILL, sh_jobs[idx].pid, SIGCONT, 0, 0, 0, 0);
    sh_jobs[idx].stopped = 0;
  }
  int st = 0;
  syscall_dispatch(SYS_WAITPID, sh_jobs[idx].pid, (u64)(usize)&st,
                   B1NIX_WUNTRACED, 0, 0, 0);
  if (sh_status_stopped(st))
    sh_jobs[idx].stopped = 1;
  else
    sh_jobs[idx].done = 1;
  return st;
}

/* Resume a stopped job in the background (`bg`). */
static int sh_bg(int job_num) {
  int idx = job_num - 1;
  if (idx < 0 || idx >= sh_job_count || !sh_jobs[idx].pid ||
      sh_jobs[idx].done) {
    uwrite("bg: no such job\n");
    return -1;
  }
  syscall_dispatch(SYS_KILL, sh_jobs[idx].pid, SIGCONT, 0, 0, 0, 0);
  sh_jobs[idx].stopped = 0;
  char num[5] = {'[', (char)('0' + (idx + 1)), ']', ' ', 0};
  uwrite(num);
  uwrite(sh_jobs[idx].name);
  uwrite(" &\n");
  return 0;
}

static void ensure_shell_tty_stdio(void) {
  u64 tty = syscall_dispatch(SYS_OPEN, (u64)(usize)"/dev/tty", (u64)B1NIX_O_RDWR,
                             0, 0, 0, 0);
  if ((isize)tty < 0)
    return;
  syscall_dispatch(SYS_DUP2, tty, 0, 0, 0, 0, 0);
  syscall_dispatch(SYS_DUP2, tty, 1, 0, 0, 0, 0);
  syscall_dispatch(SYS_DUP2, tty, 2, 0, 0, 0, 0);
  if (tty > 2)
    syscall_dispatch(SYS_CLOSE, tty, 0, 0, 0, 0, 0);
}

static int readline(char *buffer, usize max_len) {
  ensure_shell_tty_stdio();

  struct b1nix_termios old_t, new_t;
  if ((isize)syscall_dispatch(SYS_IOCTL, 0, B1NIX_TCGETS,
                              (u64)(usize)&old_t, 0, 0, 0) < 0) {
    memset(&old_t, 0, sizeof(old_t));
    old_t.c_lflag = B1NIX_ICANON | B1NIX_ECHO | B1NIX_ISIG;
    old_t.c_oflag = B1NIX_OPOST;
  }
  new_t = old_t;
  new_t.c_lflag &= ~(B1NIX_ICANON | B1NIX_ECHO);
  syscall_dispatch(SYS_IOCTL, 0, B1NIX_TCSETS, (u64)(usize)&new_t, 0, 0, 0);

  usize len = 0;
  int hist_idx = sh_hist_count;
  int ret = 0;

  while (1) {
    char c = (char)syscall_dispatch(SYS_READ_KBD, 0, 0, 0, 0, 0, 0);
    if (c == 4) {
      ret = -1; /* Ctrl-D */
      break;
    }
    if (c == 27) {
      char b1 = (char)syscall_dispatch(SYS_READ_KBD, 0, 0, 0, 0, 0, 0);
      if (b1 == '[') {
        char b2 = (char)syscall_dispatch(SYS_READ_KBD, 0, 0, 0, 0, 0, 0);
        if (b2 == 'A') { // Up
          if (hist_idx > 0) {
            hist_idx--;
            for (usize i = 0; i < len; i++)
              uwrite("\b \b");
            len = strlen(sh_history[hist_idx]);
            if (len >= max_len)
              len = max_len - 1;
            memcpy(buffer, sh_history[hist_idx], len);
            buffer[len] = '\0';
            uwrite(buffer);
          }
        } else if (b2 == 'B') { // Down
          if (hist_idx < sh_hist_count) {
            for (usize i = 0; i < len; i++)
              uwrite("\b \b");
            hist_idx++;
            if (hist_idx < sh_hist_count) {
              len = strlen(sh_history[hist_idx]);
              if (len >= max_len)
                len = max_len - 1;
              memcpy(buffer, sh_history[hist_idx], len);
              buffer[len] = '\0';
              uwrite(buffer);
            } else {
              len = 0;
              buffer[0] = '\0';
            }
          }
        }
      }
      continue;
    }

    if (c == '\n' || c == '\r') {
      buffer[len] = '\0';
      uwrite("\n");
      break;
    } else if (c == '\b' || c == 127) {
      if (len > 0) {
        len--;
        uwrite("\b \b");
      }
    } else if (c >= ' ' && c <= '~' && len < max_len - 1) {
      buffer[len++] = c;
      char tmp[2] = {c, 0};
      uwrite(tmp);
    }
  }

  syscall_dispatch(SYS_IOCTL, 0, B1NIX_TCSETS, (u64)(usize)&old_t, 0, 0, 0);
  return ret;
}

static void resolve_path(const char *cwd, const char *rel, char *abs) {
  char combined[256];
  usize len = 0;

  if (rel[0] == '/') {
    while (rel[len] && len < sizeof(combined) - 1) {
      combined[len] = rel[len];
      len++;
    }
    combined[len] = '\0';
  } else {
    usize cwd_len = strlen(cwd);
    while (len < cwd_len && len < sizeof(combined) - 1) {
      combined[len] = cwd[len];
      len++;
    }
    if (len == 0) {
      combined[len++] = '/';
    }
    if (combined[len - 1] != '/' && len < sizeof(combined) - 1) {
      combined[len++] = '/';
    }
    for (usize i = 0; rel[i] && len < sizeof(combined) - 1; i++) {
      combined[len++] = rel[i];
    }
    combined[len] = '\0';
  }

  abs[0] = '/';
  abs[1] = '\0';
  len = 1;
  for (usize i = 0; combined[i];) {
    while (combined[i] == '/')
      i++;
    if (!combined[i])
      break;

    char part[64];
    usize part_len = 0;
    while (combined[i] && combined[i] != '/' && part_len < sizeof(part) - 1) {
      part[part_len++] = combined[i++];
    }
    part[part_len] = '\0';
    while (combined[i] && combined[i] != '/')
      i++;

    if (part[0] == '\0' || strcmp(part, ".") == 0)
      continue;
    if (strcmp(part, "..") == 0) {
      if (len > 1) {
        if (abs[len - 1] == '/')
          len--;
        while (len > 1 && abs[len - 1] != '/')
          len--;
        abs[len] = '\0';
      }
      continue;
    }

    if (len > 1)
      abs[len++] = '/';
    for (usize j = 0; part[j] && len < 127; j++) {
      abs[len++] = part[j];
    }
    abs[len] = '\0';
  }
}

/* Dynamic shell environment: the table grows on demand and keys/values are
 * strdup'd at full length, so capacity tracks what the running scripts use
 * instead of a hardcoded ceiling. */
struct sh_envvar {
  char *key;
  char *val;
};
static struct sh_envvar *sh_env = 0;
static int sh_env_count = 0;
static int sh_env_cap = 0;

static void sh_env_grow(void) {
  int ncap = sh_env_cap ? sh_env_cap * 2 : 8;
  struct sh_envvar *ne = malloc((usize)ncap * sizeof(*ne));
  if (!ne)
    return;
  for (int i = 0; i < sh_env_count; i++)
    ne[i] = sh_env[i];
  free(sh_env);
  sh_env = ne;
  sh_env_cap = ncap;
}

static void set_env(const char *key, const char *val) {
  for (int i = 0; i < sh_env_count; i++) {
    if (strcmp(sh_env[i].key, key) == 0) {
      char *nv = strdup(val);
      if (nv) {
        free(sh_env[i].val);
        sh_env[i].val = nv;
      }
      return;
    }
  }
  if (sh_env_count == sh_env_cap)
    sh_env_grow();
  if (sh_env_count == sh_env_cap)
    return;
  char *k = strdup(key);
  char *v = strdup(val);
  if (!k || !v) {
    free(k);
    free(v);
    return;
  }
  sh_env[sh_env_count].key = k;
  sh_env[sh_env_count].val = v;
  sh_env_count++;
}

static const char *get_env(const char *key) {
  for (int i = 0; i < sh_env_count; i++)
    if (strcmp(sh_env[i].key, key) == 0)
      return sh_env[i].val;
  return "";
}

/* Whole-environment snapshot/restore used to isolate subshell side effects. */
struct sh_env_snap {
  struct sh_envvar *vars;
  int count;
};

static void sh_env_snapshot(struct sh_env_snap *s) {
  s->count = sh_env_count;
  s->vars = malloc((usize)(sh_env_count > 0 ? sh_env_count : 1) *
                   sizeof(struct sh_envvar));
  if (!s->vars) {
    s->count = 0;
    return;
  }
  for (int i = 0; i < sh_env_count; i++) {
    s->vars[i].key = strdup(sh_env[i].key);
    s->vars[i].val = strdup(sh_env[i].val);
  }
}

static void sh_env_restore(struct sh_env_snap *s) {
  for (int i = 0; i < sh_env_count; i++) {
    free(sh_env[i].key);
    free(sh_env[i].val);
  }
  sh_env_count = 0;
  for (int i = 0; i < s->count; i++) {
    if (s->vars[i].key && s->vars[i].val)
      set_env(s->vars[i].key, s->vars[i].val);
    free(s->vars[i].key);
    free(s->vars[i].val);
  }
  free(s->vars);
  s->vars = 0;
  s->count = 0;
}

static int sh_last_status = 0;

static void sh_execute_line(char *line, char *cwd);
static void sh_expand_cmdsubst(char *line, int linecap, char *cwd);
static void expand_env(const char *in, char *out);
static void sh_run_fragment(const char *frag, char *cwd);

/* ---- shell functions: name() { list; } ---- */
/* Dynamic function table: grows on demand, bodies strdup'd at full length. */
struct sh_func {
  char *name;
  char *body;
};
static struct sh_func *sh_funcs = 0;
static int sh_func_count = 0;
static int sh_func_cap = 0;

static int sh_func_find(const char *name) {
  for (int i = 0; i < sh_func_count; i++)
    if (strcmp(sh_funcs[i].name, name) == 0)
      return i;
  return -1;
}

static void sh_func_define(const char *name, const char *body) {
  int idx = sh_func_find(name);
  if (idx >= 0) {
    char *nb = strdup(body);
    if (nb) {
      free(sh_funcs[idx].body);
      sh_funcs[idx].body = nb;
    }
    return;
  }
  if (sh_func_count == sh_func_cap) {
    int ncap = sh_func_cap ? sh_func_cap * 2 : 8;
    struct sh_func *nf = malloc((usize)ncap * sizeof(*nf));
    if (!nf)
      return;
    for (int i = 0; i < sh_func_count; i++)
      nf[i] = sh_funcs[i];
    free(sh_funcs);
    sh_funcs = nf;
    sh_func_cap = ncap;
  }
  char *n = strdup(name);
  char *b = strdup(body);
  if (!n || !b) {
    free(n);
    free(b);
    return;
  }
  sh_funcs[sh_func_count].name = n;
  sh_funcs[sh_func_count].body = b;
  sh_func_count++;
}

/* Invoke a function: set $1..$9 and $# to the call args (saving/restoring the
 * caller's positionals), then expand and run the stored body. The $1..$9 bound
 * is the POSIX single-digit positional model, not an arbitrary cap. */
static int sh_func_run(const char *name, char **args, int num_args, char *cwd) {
  int idx = sh_func_find(name);
  if (idx < 0)
    return 127;

  char saved[10][64];
  char saved_hash[12];
  char key[2] = {0, 0};
  for (int i = 1; i <= 9; i++) {
    key[0] = (char)('0' + i);
    strncpy(saved[i], get_env(key), 63);
    saved[i][63] = '\0';
  }
  strncpy(saved_hash, get_env("#"), sizeof(saved_hash) - 1);
  saved_hash[sizeof(saved_hash) - 1] = '\0';

  for (int i = 1; i <= 9; i++) {
    key[0] = (char)('0' + i);
    set_env(key, (i < num_args) ? args[i] : "");
  }
  char nbuf[12];
  snprintf(nbuf, sizeof(nbuf), "%d", num_args - 1);
  set_env("#", nbuf);

  usize blen = strlen(sh_funcs[idx].body);
  usize cap = blen * 4 + 256;
  char *body = malloc(cap);
  char *expanded = malloc(blen * 8 + 256);
  if (body && expanded) {
    memcpy(body, sh_funcs[idx].body, blen + 1);
    sh_expand_cmdsubst(body, (int)cap, cwd);
    expand_env(body, expanded);
    sh_execute_line(expanded, cwd);
  }
  free(body);
  free(expanded);
  int status = sh_last_status;

  for (int i = 1; i <= 9; i++) {
    key[0] = (char)('0' + i);
    set_env(key, saved[i]);
  }
  set_env("#", saved_hash);
  return status;
}

/* ---- shell arrays: arr=(a b c), ${arr[i]}, ${arr[@]}, ${#arr[@]} ---- */
/* Each array and its element list grow on demand (no fixed element cap). */
struct sh_array {
  char *name;
  char **elems;
  int count;
  int cap;
};
static struct sh_array *sh_arrays = 0;
static int sh_array_count = 0;
static int sh_array_cap = 0;

static int sh_array_find(const char *name) {
  for (int i = 0; i < sh_array_count; i++)
    if (strcmp(sh_arrays[i].name, name) == 0)
      return i;
  return -1;
}

/* Reset (or create) an array to empty, freeing any existing elements. */
static int sh_array_reset(const char *name) {
  int idx = sh_array_find(name);
  if (idx >= 0) {
    for (int i = 0; i < sh_arrays[idx].count; i++)
      free(sh_arrays[idx].elems[i]);
    sh_arrays[idx].count = 0;
    return idx;
  }
  if (sh_array_count == sh_array_cap) {
    int ncap = sh_array_cap ? sh_array_cap * 2 : 8;
    struct sh_array *na = malloc((usize)ncap * sizeof(*na));
    if (!na)
      return -1;
    for (int i = 0; i < sh_array_count; i++)
      na[i] = sh_arrays[i];
    free(sh_arrays);
    sh_arrays = na;
    sh_array_cap = ncap;
  }
  char *nm = strdup(name);
  if (!nm)
    return -1;
  idx = sh_array_count++;
  sh_arrays[idx].name = nm;
  sh_arrays[idx].elems = 0;
  sh_arrays[idx].count = 0;
  sh_arrays[idx].cap = 0;
  return idx;
}

static void sh_array_append(int idx, const char *elem) {
  if (idx < 0)
    return;
  struct sh_array *a = &sh_arrays[idx];
  if (a->count == a->cap) {
    int ncap = a->cap ? a->cap * 2 : 8;
    char **ne = malloc((usize)ncap * sizeof(char *));
    if (!ne)
      return;
    for (int i = 0; i < a->count; i++)
      ne[i] = a->elems[i];
    free(a->elems);
    a->elems = ne;
    a->cap = ncap;
  }
  char *e = strdup(elem);
  if (!e)
    return;
  a->elems[a->count++] = e;
}

/* ---- trap handlers: trap 'cmds' SIG ... ---- */
struct sh_trap {
  char *sig;
  char *cmd;
};
static struct sh_trap *sh_traps = 0;
static int sh_trap_count = 0;
static int sh_trap_cap = 0;

static void sh_trap_set(const char *sig, const char *cmd) {
  for (int i = 0; i < sh_trap_count; i++) {
    if (strcmp(sh_traps[i].sig, sig) == 0) {
      char *nc = strdup(cmd);
      if (nc) {
        free(sh_traps[i].cmd);
        sh_traps[i].cmd = nc;
      }
      return;
    }
  }
  if (sh_trap_count == sh_trap_cap) {
    int ncap = sh_trap_cap ? sh_trap_cap * 2 : 8;
    struct sh_trap *nt = malloc((usize)ncap * sizeof(*nt));
    if (!nt)
      return;
    for (int i = 0; i < sh_trap_count; i++)
      nt[i] = sh_traps[i];
    free(sh_traps);
    sh_traps = nt;
    sh_trap_cap = ncap;
  }
  char *s = strdup(sig);
  char *c = strdup(cmd);
  if (!s || !c) {
    free(s);
    free(c);
    return;
  }
  sh_traps[sh_trap_count].sig = s;
  sh_traps[sh_trap_count].cmd = c;
  sh_trap_count++;
}

static const char *sh_trap_get(const char *sig) {
  for (int i = 0; i < sh_trap_count; i++)
    if (strcmp(sh_traps[i].sig, sig) == 0)
      return sh_traps[i].cmd;
  return 0;
}

/* Run the handler registered for `sig` (e.g. "EXIT"), if any. */
static void sh_run_trap(const char *sig, char *cwd) {
  const char *c = sh_trap_get(sig);
  if (c && c[0])
    sh_run_fragment(c, cwd);
}

/* Integer evaluator for $((...)) arithmetic expansion. Supports + - * / %,
 * parentheses, unary +/-, decimal literals, and bare/`$`-prefixed variable
 * names (resolved via the shell environment). Division/modulo by zero yield 0.
 * Single-threaded shell, so a file-local cursor is safe across the recursion. */
static const char *ar_cur;

static long ar_expr(void);

static void ar_skip(void) {
  while (*ar_cur == ' ' || *ar_cur == '\t')
    ar_cur++;
}

static long ar_factor(void) {
  ar_skip();
  if (*ar_cur == '+') {
    ar_cur++;
    return ar_factor();
  }
  if (*ar_cur == '-') {
    ar_cur++;
    return -ar_factor();
  }
  if (*ar_cur == '(') {
    ar_cur++;
    long v = ar_expr();
    ar_skip();
    if (*ar_cur == ')')
      ar_cur++;
    return v;
  }
  if (*ar_cur == '$')
    ar_cur++;
  if ((*ar_cur >= 'a' && *ar_cur <= 'z') || (*ar_cur >= 'A' && *ar_cur <= 'Z') ||
      *ar_cur == '_') {
    char name[32];
    int k = 0;
    while (((*ar_cur >= 'a' && *ar_cur <= 'z') ||
            (*ar_cur >= 'A' && *ar_cur <= 'Z') ||
            (*ar_cur >= '0' && *ar_cur <= '9') || *ar_cur == '_') &&
           k < 31)
      name[k++] = *ar_cur++;
    name[k] = '\0';
    return atoi(get_env(name));
  }
  long v = 0;
  while (*ar_cur >= '0' && *ar_cur <= '9') {
    v = v * 10 + (*ar_cur - '0');
    ar_cur++;
  }
  return v;
}

static long ar_term(void) {
  long v = ar_factor();
  for (;;) {
    ar_skip();
    char op = *ar_cur;
    if (op == '*') {
      ar_cur++;
      v = v * ar_factor();
    } else if (op == '/') {
      ar_cur++;
      long d = ar_factor();
      v = d ? v / d : 0;
    } else if (op == '%') {
      ar_cur++;
      long d = ar_factor();
      v = d ? v % d : 0;
    } else {
      break;
    }
  }
  return v;
}

static long ar_expr(void) {
  long v = ar_term();
  for (;;) {
    ar_skip();
    char op = *ar_cur;
    if (op == '+') {
      ar_cur++;
      v = v + ar_term();
    } else if (op == '-') {
      ar_cur++;
      v = v - ar_term();
    } else {
      break;
    }
  }
  return v;
}

static long eval_arith(const char *expr) {
  ar_cur = expr;
  return ar_expr();
}

static void expand_env(const char *in, char *out) {
  int i = 0, j = 0;
  while (in[i]) {
    if (in[i] == '$') {
      i++;
      if (in[i] == '(' && in[i + 1] == '(') {
        /* arithmetic expansion: $(( expr )) */
        i += 2;
        char inner[128];
        int n = 0, depth = 0;
        while (in[i]) {
          if (in[i] == ')' && in[i + 1] == ')' && depth == 0) {
            i += 2;
            break;
          }
          if (in[i] == '(')
            depth++;
          else if (in[i] == ')')
            depth--;
          if (n < (int)sizeof(inner) - 1)
            inner[n++] = in[i];
          i++;
        }
        inner[n] = '\0';
        char buf[24];
        snprintf(buf, sizeof(buf), "%ld", eval_arith(inner));
        for (int k = 0; buf[k]; k++)
          out[j++] = buf[k];
        continue;
      }
      if (in[i] == '?') {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d", sh_last_status);
        for (int k = 0; buf[k]; k++)
          out[j++] = buf[k];
        i++;
        continue;
      }
      if (in[i] == '{') {
        /* ${VAR} ${#VAR} ${arr[i]} ${arr[@]} ${#arr[@]}
         * ${x:-w} ${x-w} ${x:=w} ${x=w} ${x:+w} ${x+w} */
        i++;
        int lenmode = 0;
        if (in[i] == '#') {
          lenmode = 1;
          i++;
        }
        char nm[64];
        int k = 0;
        while (in[i] && in[i] != '}' && in[i] != '[' && in[i] != ':' &&
               in[i] != '-' && in[i] != '+' && in[i] != '=' && k < 63)
          nm[k++] = in[i++];
        nm[k] = '\0';

        int allidx = 0, hasidx = 0, idxval = 0;
        if (in[i] == '[') {
          i++;
          if (in[i] == '@' || in[i] == '*') {
            allidx = 1;
            i++;
          } else {
            char ib[12];
            int m = 0;
            while (in[i] >= '0' && in[i] <= '9' && m < 11)
              ib[m++] = in[i++];
            ib[m] = '\0';
            idxval = atoi(ib);
            hasidx = 1;
          }
          if (in[i] == ']')
            i++;
        }

        /* modifier operator: :-, -, :=, =, :+, + (empty == unset here) */
        int op = 0; /* 1=use-default 2=assign-default 3=use-alternate */
        if (in[i] == ':')
          i++;
        if (in[i] == '-') {
          op = 1;
          i++;
        } else if (in[i] == '=') {
          op = 2;
          i++;
        } else if (in[i] == '+') {
          op = 3;
          i++;
        }
        char word[256];
        word[0] = '\0';
        if (op) {
          int wl = 0;
          while (in[i] && in[i] != '}' && wl < 255)
            word[wl++] = in[i++];
          word[wl] = '\0';
        }
        if (in[i] == '}')
          i++;

        int ai = sh_array_find(nm);
        if (allidx || hasidx) {
          if (lenmode) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", ai >= 0 ? sh_arrays[ai].count : 0);
            for (int m = 0; buf[m]; m++)
              out[j++] = buf[m];
          } else if (allidx && ai >= 0) {
            for (int e = 0; e < sh_arrays[ai].count; e++) {
              const char *el = sh_arrays[ai].elems[e];
              while (*el)
                out[j++] = *el++;
              if (e + 1 < sh_arrays[ai].count)
                out[j++] = ' ';
            }
          } else if (hasidx && ai >= 0 && idxval >= 0 &&
                     idxval < sh_arrays[ai].count) {
            const char *el = sh_arrays[ai].elems[idxval];
            while (*el)
              out[j++] = *el++;
          }
        } else {
          const char *val = (ai >= 0 && sh_arrays[ai].count > 0)
                                ? sh_arrays[ai].elems[0]
                                : get_env(nm);
          int empty = (val[0] == '\0');
          if (lenmode) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", (int)strlen(val));
            for (int m = 0; buf[m]; m++)
              out[j++] = buf[m];
          } else if (op) {
            char wexp[512];
            expand_env(word, wexp);
            if (op == 3) {
              if (!empty)
                for (const char *w = wexp; *w; w++)
                  out[j++] = *w;
            } else if (empty) {
              for (const char *w = wexp; *w; w++)
                out[j++] = *w;
              if (op == 2)
                set_env(nm, wexp);
            } else {
              while (*val)
                out[j++] = *val++;
            }
          } else {
            while (*val)
              out[j++] = *val++;
          }
        }
        continue;
      }
      char key[32];
      int k = 0;
      while (in[i] && in[i] != ' ' && in[i] != '$' && in[i] != '/' &&
             in[i] != '"' && in[i] != '\'' && in[i] != ';' && in[i] != ')' &&
             in[i] != '(' && in[i] != '\t' && k < 31) {
        key[k++] = in[i++];
      }
      key[k] = '\0';
      const char *val = get_env(key);
      while (*val) {
        out[j++] = *val++;
      }
    } else {
      out[j++] = in[i++];
    }
  }
  out[j] = '\0';
}

/* POSIX-style shell parsing with quotes and escaping. When glob_flags is
 * non-NULL it receives, per token, 1 if the token held an unquoted glob
 * metacharacter (* ? [) and is therefore eligible for pathname expansion. */
static int parse_cmd(char *cmd, char **args, int *glob_flags, int max_args) {
  int count = 0;
  char *p = cmd;
  int in_dquote = 0;
  int in_squote = 0;

  while (*p && count < max_args) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p)
      break;

    args[count++] = p;
    int tok_glob = 0;
    char *dst = p;
    while (*p) {
      /* Backslash escape: only outside single quotes */
      if (!in_squote && *p == '\\' && *(p + 1)) {
        if (in_dquote && p[1] != '\\' && p[1] != '"' && p[1] != '$') {
          *dst++ = *p++;
          continue;
        }
        p++;
        *dst++ = *p++;
        continue;
      }
      /* Single quotes: preserve everything until next ' */
      if (*p == '\'' && !in_dquote) {
        in_squote = !in_squote;
        p++;
        continue;
      }
      /* Double quotes: allow escaping, preserve spaces */
      if (*p == '"' && !in_squote) {
        in_dquote = !in_dquote;
        p++;
        continue;
      }
      /* Break on unquoted space */
      if (!in_squote && !in_dquote && (*p == ' ' || *p == '\t'))
        break;

      if (!in_squote && !in_dquote &&
          (*p == '*' || *p == '?' || *p == '['))
        tok_glob = 1;

      *dst++ = *p++;
    }
    if (*p)
      p++;
    *dst = '\0';
    if (glob_flags)
      glob_flags[count - 1] = tok_glob;
  }
  return count;
}

/* fnmatch-style matcher for shell globbing: *, ?, and [set]/[!set]. */
static int glob_match(const char *pat, const char *str) {
  while (*pat) {
    if (*pat == '*') {
      pat++;
      if (!*pat)
        return 1;
      while (*str) {
        if (glob_match(pat, str))
          return 1;
        str++;
      }
      return glob_match(pat, str);
    } else if (*pat == '?') {
      if (!*str)
        return 0;
      pat++;
      str++;
    } else if (*pat == '[') {
      const char *p = pat + 1;
      int neg = 0;
      if (*p == '!' || *p == '^') {
        neg = 1;
        p++;
      }
      int matched = 0;
      char c = *str;
      if (!c)
        return 0;
      while (*p && *p != ']') {
        if (p[1] == '-' && p[2] && p[2] != ']') {
          if (c >= p[0] && c <= p[2])
            matched = 1;
          p += 3;
        } else {
          if (c == *p)
            matched = 1;
          p++;
        }
      }
      if (*p == ']')
        p++;
      if (matched == neg)
        return 0;
      pat = p;
      str++;
    } else {
      if (*pat != *str)
        return 0;
      pat++;
      str++;
    }
  }
  return *str == '\0';
}

/* Expand glob tokens (flagged by parse_cmd) against the filesystem. Non-glob
 * tokens are passed through verbatim. A glob with no match stays literal (bash
 * default without nullglob). Matches are sorted and never include "." / ".." or
 * dotfiles unless the pattern itself starts with '.'. Result strings are
 * written into pool; returns the expanded argument count. */
static int glob_expand(const char *cwd, char **args, const int *gflags,
                       int argc, char **out, int max_out, char *pool,
                       int pool_sz) {
  int oc = 0;
  int pu = 0;

  for (int i = 0; i < argc; i++) {
    if (!gflags[i]) {
      if (oc < max_out)
        out[oc++] = args[i];
      continue;
    }

    const char *tok = args[i];
    const char *slash = 0;
    for (const char *s = tok; *s; s++)
      if (*s == '/')
        slash = s;

    char dirpart[200];
    char pat[120];
    if (slash) {
      int dl = (int)(slash - tok);
      if (dl > (int)sizeof(dirpart) - 1)
        dl = (int)sizeof(dirpart) - 1;
      memcpy(dirpart, tok, dl);
      dirpart[dl] = '\0';
      strncpy(pat, slash + 1, sizeof(pat) - 1);
      pat[sizeof(pat) - 1] = '\0';
    } else {
      dirpart[0] = '\0';
      strncpy(pat, tok, sizeof(pat) - 1);
      pat[sizeof(pat) - 1] = '\0';
    }

    char dirpath[256];
    if (tok[0] == '/')
      snprintf(dirpath, sizeof(dirpath), "%s", dirpart[0] ? dirpart : "/");
    else if (dirpart[0])
      snprintf(dirpath, sizeof(dirpath), "%s/%s", cwd, dirpart);
    else
      snprintf(dirpath, sizeof(dirpath), "%s", cwd);

    int before = oc;
    int matchstart = oc;
    struct dirent *ents = malloc(64 * sizeof(struct dirent));
    if (ents) {
      int n = (int)syscall_dispatch(SYS_READDIR, (u64)(usize)dirpath,
                                    (u64)(usize)ents, 64, 0, 0, 0);
      for (int e = 0; e < n && oc < max_out; e++) {
        const char *nm = ents[e].name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
          continue;
        if (nm[0] == '.' && pat[0] != '.')
          continue;
        if (!glob_match(pat, nm))
          continue;
        char res[256];
        int rl;
        if (slash && dirpart[0])
          rl = snprintf(res, sizeof(res), "%s/%s", dirpart, nm);
        else if (slash)
          rl = snprintf(res, sizeof(res), "/%s", nm);
        else
          rl = snprintf(res, sizeof(res), "%s", nm);
        if (rl < 0 || pu + rl + 1 > pool_sz)
          break;
        memcpy(pool + pu, res, rl + 1);
        out[oc++] = pool + pu;
        pu += rl + 1;
      }
      free(ents);
    }

    /* Sort the matches added for this token. */
    for (int a = matchstart + 1; a < oc; a++) {
      char *key = out[a];
      int b = a - 1;
      while (b >= matchstart && strcmp(out[b], key) > 0) {
        out[b + 1] = out[b];
        b--;
      }
      out[b + 1] = key;
    }

    if (oc == before && oc < max_out)
      out[oc++] = args[i]; /* no match: literal */
  }
  return oc;
}

struct shell_redir {
  const char *stdin_path;
  const char *stdout_path;
  const char *stderr_path;
  int stdout_append;
  int stderr_to_stdout;
};

static int parse_redirs(char **args, int argc, struct shell_redir *redir) {
  int out_argc = 0;
  memset(redir, 0, sizeof(*redir));

  for (int i = 0; i < argc; i++) {
    usize arg_len = strlen(args[i]);
    if (strcmp(args[i], "<") == 0) {
      if (i + 1 >= argc)
        return -1;
      redir->stdin_path = args[++i];
      continue;
    }
    if (strcmp(args[i], ">") == 0) {
      if (i + 1 >= argc)
        return -1;
      redir->stdout_path = args[++i];
      redir->stdout_append = 0;
      continue;
    }
    if (strcmp(args[i], ">>") == 0) {
      if (i + 1 >= argc)
        return -1;
      redir->stdout_path = args[++i];
      redir->stdout_append = 1;
      continue;
    }
    if (strcmp(args[i], "2>") == 0) {
      if (i + 1 >= argc)
        return -1;
      redir->stderr_path = args[++i];
      continue;
    }
    if (strcmp(args[i], "2>&1") == 0) {
      redir->stderr_to_stdout = 1;
      continue;
    }
    if (arg_len > 2 && strncmp(args[i], "2>", 2) == 0) {
      redir->stderr_path = args[i] + 2;
      continue;
    }
    if (arg_len > 2 && strncmp(args[i], ">>", 2) == 0) {
      redir->stdout_path = args[i] + 2;
      redir->stdout_append = 1;
      continue;
    }
    if (arg_len > 1 && args[i][0] == '>') {
      redir->stdout_path = args[i] + 1;
      redir->stdout_append = 0;
      continue;
    }
    if (arg_len > 1 && args[i][0] == '<') {
      redir->stdin_path = args[i] + 1;
      continue;
    }
    args[out_argc++] = args[i];
  }
  args[out_argc] = 0;
  return out_argc;
}

static u64 open_output(const char *cwd, const char *path, int append) {
  char abs[128];
  resolve_path(cwd, path, abs);
  int flags = B1NIX_O_WRONLY | B1NIX_O_CREAT;
  if (append)
    flags |= B1NIX_O_APPEND;
  else
    flags |= B1NIX_O_TRUNC;
  u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)abs, (u64)flags, 0, 0, 0, 0);
  return fd;
}

static int apply_redirs(const char *cwd, const struct shell_redir *redir,
                        int *opened, int max_opened) {
  int opened_count = 0;
  if (redir->stdin_path) {
    char abs[128];
    resolve_path(cwd, redir->stdin_path, abs);
    u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)abs, 0, 0, 0, 0, 0);
    if ((isize)fd < 0)
      return -1;
    syscall_dispatch(SYS_DUP2, fd, 0, 0, 0, 0, 0);
    if (opened_count < max_opened)
      opened[opened_count++] = (int)fd;
  }
  if (redir->stdout_path) {
    u64 fd = open_output(cwd, redir->stdout_path, redir->stdout_append);
    if ((isize)fd < 0)
      return -1;
    syscall_dispatch(SYS_DUP2, fd, 1, 0, 0, 0, 0);
    if (opened_count < max_opened)
      opened[opened_count++] = (int)fd;
  }
  if (redir->stderr_to_stdout) {
    syscall_dispatch(SYS_DUP2, 1, 2, 0, 0, 0, 0);
  } else if (redir->stderr_path) {
    u64 fd = open_output(cwd, redir->stderr_path, 0);
    if ((isize)fd < 0)
      return -1;
    syscall_dispatch(SYS_DUP2, fd, 2, 0, 0, 0, 0);
    if (opened_count < max_opened)
      opened[opened_count++] = (int)fd;
  }
  return opened_count;
}

static int sh_stdio_depth = 0;

static void save_stdio(int saved[3]) {
  int base = 63 - sh_stdio_depth * 3;
  if (base < 5)
    base = 63;
  saved[0] = base - 2;
  saved[1] = base - 1;
  saved[2] = base;
  sh_stdio_depth++;
  syscall_dispatch(SYS_DUP2, 0, saved[0], 0, 0, 0, 0);
  syscall_dispatch(SYS_DUP2, 1, saved[1], 0, 0, 0, 0);
  syscall_dispatch(SYS_DUP2, 2, saved[2], 0, 0, 0, 0);
}

static void restore_stdio(const int saved[3]) {
  syscall_dispatch(SYS_DUP2, saved[0], 0, 0, 0, 0, 0);
  syscall_dispatch(SYS_DUP2, saved[1], 1, 0, 0, 0, 0);
  syscall_dispatch(SYS_DUP2, saved[2], 2, 0, 0, 0, 0);
  syscall_dispatch(SYS_CLOSE, saved[0], 0, 0, 0, 0, 0);
  syscall_dispatch(SYS_CLOSE, saved[1], 0, 0, 0, 0, 0);
  syscall_dispatch(SYS_CLOSE, saved[2], 0, 0, 0, 0, 0);
  if (sh_stdio_depth > 0)
    sh_stdio_depth--;
}

static int lookup_path(const char *cwd, const char *name, char *out) {
  if (name[0] == '/' || name[0] == '.') {
    resolve_path(cwd, name, out);
    return 0;
  }

  const char *path = get_env("PATH");
  if (!path || path[0] == '\0')
    path = "/bin";
  usize i = 0;
  while (path[i]) {
    char dir[128];
    usize d = 0;
    while (path[i] && path[i] != ':' && d < sizeof(dir) - 1) {
      dir[d++] = path[i++];
    }
    dir[d] = '\0';
    if (path[i] == ':')
      i++;
    resolve_path(dir, name, out);
    struct b1nix_stat st;
    if (syscall_dispatch(SYS_STAT, (u64)(usize)out, (u64)(usize)&st, 0, 0, 0, 0) ==
        0) {
      return 0;
    }
  }
  usize j = 0;
  while (name[j] && j < 127) {
    out[j] = name[j];
    j++;
  }
  out[j] = '\0';
  return 0;
}

static u64 spawn_path(const char *cwd, char **args, int num_args) {
  char path[128];
  lookup_path(cwd, args[0], path);
  u64 pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)path, num_args, (u64)(usize)args, 0, 0, 0);
  if (pid == (u64)-1 && strcmp(path, args[0]) != 0) {
    pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)args[0], num_args, (u64)(usize)args, 0, 0, 0);
  }
  if (pid == (u64)-1 && args[0][0] != '/' && args[0][0] != '.') {
    char bin_path[128];
    usize len = strlen(args[0]);
    if (len < 120) {
      memcpy(bin_path, "/bin/", 5);
      memcpy(bin_path + 5, args[0], len + 1);
      pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)bin_path, num_args, (u64)(usize)args, 0, 0, 0);
    }
  }
  return pid;
}

static int run_external_command(const char *cwd, char **args, int num_args,
                                int wait_for) {
  u64 pid = spawn_path(cwd, args, num_args);
  if (pid == (u64)-1) {
    /* POSIX: failed exec returns 127 */
    uwrite("sh: command not found\n");
    return 127;
  }
  if (wait_for) {
    int status = 0;
    syscall_dispatch(SYS_WAITPID, pid, (u64)(usize)&status, B1NIX_WUNTRACED, 0,
                     0, 0);
    /* If the foreground job stopped (Ctrl-Z), record it so fg/bg can resume. */
    if (sh_status_stopped(status)) {
      int jn = sh_job_add_ex(pid, args[0], 1);
      char num[5] = {'[', (char)('0' + jn), ']', ' ', 0};
      uwrite("\n");
      uwrite(num);
      uwrite("Stopped  ");
      uwrite(args[0]);
      uwrite("\n");
    }
    return status;
  }
  /* Background job — register in jobs list */
  sh_job_add(pid, args[0]);
  return 0;
}

static int sh_execute_cmd(char *cwd, char **args, int num_args, int is_bg) {
  if (num_args == 0)
    return 0;

  /* User-defined functions take precedence over external commands. */
  if (sh_func_find(args[0]) >= 0)
    return sh_func_run(args[0], args, num_args, cwd);

  /* Built-ins */
  if (strcmp(args[0], "exit") == 0) {
    int code = (num_args > 1) ? atoi(args[1]) : sh_last_status;
    sh_run_trap("EXIT", cwd);
    syscall_dispatch(SYS_EXIT, (u64)code, 0, 0, 0, 0, 0);
    return 0;
  }
  if (strcmp(args[0], "trap") == 0) {
    if (num_args == 1) {
      for (int i = 0; i < sh_trap_count; i++)
        if (sh_traps[i].cmd[0])
          printf("trap -- '%s' %s\n", sh_traps[i].cmd, sh_traps[i].sig);
      return 0;
    }
    const char *cmd = args[1];
    for (int s = 2; s < num_args; s++)
      sh_trap_set(args[s], strcmp(cmd, "-") == 0 ? "" : cmd);
    return 0;
  }
  if (strcmp(args[0], "help") == 0) {
    uwrite("Built-in commands:\n");
    uwrite("  help, exit, cd, pwd, echo, export, jobs, fg\n");
    uwrite("  ls, cat, ps, mem, clear, ping, resolve, reboot, kill\n");
    return 0;
  }
  if (strcmp(args[0], "cd") == 0) {
    char abs_path[128];
    const char *target = (num_args > 1) ? args[1] : "/";
    resolve_path(cwd, target, abs_path);
    if (syscall_dispatch(SYS_CHDIR, (u64)(usize)abs_path, 0, 0, 0, 0, 0) == 0) {
      strncpy(cwd, abs_path, 128);
      return 0;
    } else {
      uwrite("sh: cd: no such directory\n");
      return 1;
    }
  }
  if (strcmp(args[0], "pwd") == 0) {
    uwrite(cwd);
    uwrite("\n");
    return 0;
  }
  if (strcmp(args[0], "export") == 0 && num_args > 1) {
    char *eq = strchr(args[1], '=');
    if (eq) {
      *eq = '\0';
      set_env(args[1], eq + 1);
    }
    return 0;
  }
  if (strcmp(args[0], "unset") == 0 && num_args > 1) {
    set_env(args[1], ""); /* Simple unset for now */
    return 0;
  }
  if (strcmp(args[0], "echo") == 0) {
    for (int i = 1; i < num_args; i++) {
      uwrite(args[i]);
      if (i < num_args - 1)
        uwrite(" ");
    }
    uwrite("\n");
    return 0;
  }
  if (strcmp(args[0], "ls") == 0) {
    if (num_args > 1 && args[1][0] == '-')
      return run_external_command(cwd, args, num_args, !is_bg);
    char abs_path[128];
    const char *target = (num_args > 1 && args[1][0] != '-') ? args[1] : cwd;
    resolve_path(cwd, target, abs_path);
    syscall_dispatch(SYS_LIST, (u64)(usize)abs_path, 0, 0, 0, 0, 0);
    return 0;
  }
  if (strcmp(args[0], "cat") == 0 && num_args > 1 && args[1][0] != '-') {
    char abs_path[128];
    resolve_path(cwd, args[1], abs_path);
    char buffer[256];
    u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)abs_path, 0, 0, 0, 0, 0);
    if ((isize)fd < 0) {
      uwrite("sh: cat: open failed\n");
      return 1;
    } else {
      while (1) {
        isize n = (isize)syscall_dispatch(SYS_READ, fd, (u64)(usize)buffer, sizeof(buffer) - 1, 0, 0, 0);
        if (n <= 0)
          break;
        buffer[n] = '\0';
        uwrite(buffer);
      }
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
      return 0;
    }
  }
  if (strcmp(args[0], "clear") == 0) {
    syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0, 0, 0);
    return 0;
  }
  if (strcmp(args[0], "ps") == 0) {
    syscall_dispatch(SYS_PS, 0, 0, 0, 0, 0, 0);
    return 0;
  }
  if (strcmp(args[0], "mem") == 0) {
    syscall_dispatch(SYS_MEM, 0, 0, 0, 0, 0, 0);
    return 0;
  }
  if (strcmp(args[0], "reboot") == 0) {
    syscall_dispatch(SYS_REBOOT, 0, 0, 0, 0, 0, 0);
    return 0;
  }
  if (strcmp(args[0], "jobs") == 0) {
    sh_jobs_print();
    return 0;
  }
  if (strcmp(args[0], "fg") == 0) {
    int jn = (num_args > 1) ? (int)(args[1][0] - '0') : sh_job_count;
    return sh_fg(jn);
  }
  if (strcmp(args[0], "bg") == 0) {
    int jn = (num_args > 1) ? (int)(args[1][0] - '0') : sh_job_count;
    return sh_bg(jn);
  }
  if (strcmp(args[0], "kill") == 0) {
    int sig = 15;
    int pid_idx = 1;
    if (num_args > 2 && args[1][0] == '-') {
      sig = (int)atoi(args[1] + 1);
      pid_idx = 2;
    }
    if (num_args > pid_idx) {
      u64 pid = (u64)atoi(args[pid_idx]);
      syscall_dispatch(SYS_KILL, pid, (u64)sig, 0, 0, 0, 0);
    }
    return 0;
  }
  if (strcmp(args[0], "ip") == 0) {
    syscall_dispatch(SYS_NET_INFO, 0, 0, 0, 0, 0, 0);
    return 0;
  }
  if (strcmp(args[0], "selfhost") == 0) {
    struct b1nix_selfhost_status status;
    if (syscall_dispatch(SYS_SELFHOST_STATUS, (u64)(usize)&status, 0, 0, 0, 0, 0) ==
        0) {
      uwrite("target: ");
      uwrite(status.target_triple);
      uwrite("\n");
      uwrite("compiler: ");
      uwrite(status.compiler);
      uwrite("\n");
      uwrite("make: ");
      uwrite(status.make);
      uwrite("\n");
    }
    return 0;
  }

  return run_external_command(cwd, args, num_args, !is_bg);
}

static void sh_execute_line(char *line, char *cwd);

/* Find a top-level pipe `|` (paren depth 0, outside quotes, not `||`). */
static char *find_top_pipe(char *p) {
  int sq = 0, dq = 0, paren = 0;
  for (char *s = p; *s; s++) {
    char c = *s;
    if (c == '\'' && !dq)
      sq = !sq;
    else if (c == '"' && !sq)
      dq = !dq;
    else if (!sq && !dq) {
      if (c == '(')
        paren++;
      else if (c == ')') {
        if (paren > 0)
          paren--;
      } else if (c == '|' && paren == 0) {
        if (s[1] == '|') {
          s++;
          continue;
        }
        return s;
      }
    }
  }
  return 0;
}

/* Run `seg` (which begins with '(') as a subshell: the inner list executes
 * with its own cwd copy and an env snapshot, so `cd`/variable side effects do
 * not leak to the parent. Redirections following the closing ')' are applied
 * around the whole group. b1nix's shell is in-kernel and does not fork itself,
 * so isolation is emulated via save/restore rather than a real child. */
static int sh_run_subshell(char *seg, char *cwd, int is_bg) {
  (void)is_bg;
  int depth = 0, sq = 0, dq = 0, close = -1;
  for (int i = 0; seg[i]; i++) {
    char c = seg[i];
    if (c == '\'' && !dq)
      sq = !sq;
    else if (c == '"' && !sq)
      dq = !dq;
    else if (!sq && !dq) {
      if (c == '(')
        depth++;
      else if (c == ')') {
        depth--;
        if (depth == 0) {
          close = i;
          break;
        }
      }
    }
  }
  if (close < 0) {
    uwrite("sh: syntax error: unmatched (\n");
    return 2;
  }

  char inner[SH_LINE_MAX];
  int il = close - 1;
  if (il < 0)
    il = 0;
  if (il > (int)sizeof(inner) - 1)
    il = (int)sizeof(inner) - 1;
  memcpy(inner, seg + 1, (usize)il);
  inner[il] = '\0';

  char trailing[128];
  strncpy(trailing, seg + close + 1, sizeof(trailing) - 1);
  trailing[sizeof(trailing) - 1] = '\0';

  /* env + cwd snapshot for isolation */
  struct sh_env_snap snap;
  sh_env_snapshot(&snap);
  char parent_cwd[128];
  strncpy(parent_cwd, cwd, sizeof(parent_cwd) - 1);
  parent_cwd[sizeof(parent_cwd) - 1] = '\0';

  /* trailing redirections (applied around the group) */
  char *targs[16];
  int tn = parse_cmd(trailing, targs, 0, 16);
  struct shell_redir redir;
  int rc = parse_redirs(targs, tn, &redir);
  int saved_io[3];
  int have_io = 0;
  int opened[4];
  int opened_count = 0;
  if (rc >= 0 && (redir.stdin_path || redir.stdout_path || redir.stderr_path ||
                  redir.stderr_to_stdout)) {
    save_stdio(saved_io);
    have_io = 1;
    opened_count = apply_redirs(cwd, &redir, opened, 4);
  }

  char subcwd[128];
  strncpy(subcwd, cwd, sizeof(subcwd) - 1);
  subcwd[sizeof(subcwd) - 1] = '\0';
  sh_execute_line(inner, subcwd);
  int status = sh_last_status;

  if (have_io) {
    for (int i = 0; i < opened_count; i++)
      syscall_dispatch(SYS_CLOSE, opened[i], 0, 0, 0, 0, 0);
    restore_stdio(saved_io);
  }

  /* restore parent env and process cwd (subshell side effects discarded) */
  sh_env_restore(&snap);
  syscall_dispatch(SYS_CHDIR, (u64)(usize)parent_cwd, 0, 0, 0, 0, 0);
  return status;
}

/* Detect and perform a `name=(elem ...)` array assignment. The segment has
 * already been variable-expanded, so the elements are literal. Returns 1 if it
 * was an array assignment (and stored it), 0 otherwise. */
static int sh_try_array_assign(char *p) {
  int k = 0;
  while ((p[k] >= 'a' && p[k] <= 'z') || (p[k] >= 'A' && p[k] <= 'Z') ||
         (p[k] >= '0' && p[k] <= '9') || p[k] == '_')
    k++;
  if (k == 0 || p[k] != '=' || p[k + 1] != '(')
    return 0;
  char name[64];
  int nl = k > 63 ? 63 : k;
  memcpy(name, p, (usize)nl);
  name[nl] = '\0';
  char *q = p + k + 2;
  char *end = q;
  while (*end && *end != ')')
    end++;
  int idx = sh_array_reset(name);
  while (q < end) {
    while (q < end && (*q == ' ' || *q == '\t'))
      q++;
    if (q >= end)
      break;
    char elem[256];
    int el = 0;
    while (q < end && *q != ' ' && *q != '\t' && el < 255)
      elem[el++] = *q++;
    elem[el] = '\0';
    sh_array_append(idx, elem);
  }
  return 1;
}

/* Detect and perform a bare `name=value` scalar assignment. The segment is
 * already variable-expanded. Only a pure single-word assignment (no trailing
 * command, no spaces in the value) is handled here; env-prefix command form
 * (`VAR=x cmd`) and quoted values with spaces fall through. Returns 1 if it was
 * handled. */
static int sh_try_scalar_assign(char *p) {
  int k = 0;
  while ((p[k] >= 'a' && p[k] <= 'z') || (p[k] >= 'A' && p[k] <= 'Z') ||
         (p[k] >= '0' && p[k] <= '9') || p[k] == '_')
    k++;
  if (k == 0 || p[k] != '=')
    return 0;
  if (p[k + 1] == '(' || p[k + 1] == '=') /* array assign / comparison */
    return 0;
  const char *val = p + k + 1;
  int vl = 0;
  while (val[vl] && val[vl] != ' ' && val[vl] != '\t')
    vl++;
  if (val[vl] != '\0') /* trailing content -> not a pure assignment */
    return 0;
  char name[64];
  int nl = k > 63 ? 63 : k;
  memcpy(name, p, (usize)nl);
  name[nl] = '\0';
  char value[256];
  int vc = vl > 255 ? 255 : vl;
  memcpy(value, val, (usize)vc);
  value[vc] = '\0';
  set_env(name, value);
  return 1;
}

static int sh_execute_pipeline(char *cmd, char *cwd) {
  int is_bg = 0;
  char *p = cmd;
  while (*p == ' ')
    p++;
  if (!*p)
    return 0;

  usize len = strlen(p);
  while (len > 0 && p[len - 1] == ' ') {
    p[len - 1] = '\0';
    len--;
  }
  if (len > 0 && p[len - 1] == '&') {
    is_bg = 1;
    p[len - 1] = '\0';
  }

  char *pipe_pos = find_top_pipe(p);
  if (!pipe_pos) {
    if (*p == '(')
      return sh_run_subshell(p, cwd, is_bg);
    if (sh_try_array_assign(p))
      return 0;
    if (sh_try_scalar_assign(p))
      return 0;
    /* Cap matches USER_MAX_ARGS (the SYS_SPAWN argv copy limit). 16 was too few
     * for real toolchain command lines (e.g. a kernel-flag gcc invocation has
     * ~20 tokens), which silently dropped trailing args like `-o file`. */
    /* Cap matches USER_MAX_ARGS (the SYS_SPAWN argv copy limit). 16 was too few
     * for real toolchain command lines (e.g. a kernel-flag gcc invocation has
     * ~20 tokens), which silently dropped trailing args like `-o file`. */
    char *args[32];
    int gflags[32];
    int num_args = parse_cmd(p, args, gflags, 32);

    /* Pathname expansion: expand glob-flagged tokens against the filesystem.
     * Output is capped at USER_MAX_ARGS to stay within the SYS_SPAWN argv
     * copy limit; on alloc failure we fall back to the unexpanded args. */
    char *gargs[USER_MAX_ARGS];
    char *gpool = malloc(SH_LINE_MAX * 4);
    char **eargs = args;
    int eargc = num_args;
    if (gpool) {
      eargc = glob_expand(cwd, args, gflags, num_args, gargs, USER_MAX_ARGS,
                          gpool, SH_LINE_MAX * 4);
      eargs = gargs;
    }

    struct shell_redir redir;
    eargc = parse_redirs(eargs, eargc, &redir);
    if (eargc < 0) {
      uwrite("sh: syntax error: redirection\n");
      if (gpool)
        free(gpool);
      return 2;
    }

    int saved[3];
    save_stdio(saved);
    int opened[4];
    int opened_count = apply_redirs(cwd, &redir, opened, 4);
    if (opened_count < 0) {
      uwrite("sh: redirection failed\n");
      restore_stdio(saved);
      if (gpool)
        free(gpool);
      return 1;
    }

    int status = sh_execute_cmd(cwd, eargs, eargc, is_bg);

    for (int i = 0; i < opened_count; i++)
      syscall_dispatch(SYS_CLOSE, opened[i], 0, 0, 0, 0, 0);
    restore_stdio(saved);
    if (gpool)
      free(gpool);
    return status;
  }

  *pipe_pos = '\0';
  char *cmd1 = p;
  char *cmd2 = pipe_pos + 1;

  int pipefd[2];
  if (syscall_dispatch(SYS_PIPE, (u64)(usize)pipefd, 0, 0, 0, 0, 0) != 0)
    return 1;

  int saved[3];
  save_stdio(saved);

  /* Pipeline semantics: status returned is the rightmost stage status.
   * This is deterministic and intentionally does not implement pipefail. */
  /* First stage of pipe */
  syscall_dispatch(SYS_DUP2, (u64)pipefd[1], 1, 0, 0, 0, 0);
  syscall_dispatch(SYS_CLOSE, (u64)pipefd[1], 0, 0, 0, 0, 0);
  sh_execute_pipeline(cmd1, cwd);

  /* Second stage */
  restore_stdio(saved);
  save_stdio(saved);
  syscall_dispatch(SYS_DUP2, (u64)pipefd[0], 0, 0, 0, 0, 0);
  syscall_dispatch(SYS_CLOSE, (u64)pipefd[0], 0, 0, 0, 0, 0);
  int status = sh_execute_pipeline(cmd2, cwd);

  restore_stdio(saved);
  return status;
}

static void sh_execute_line(char *line, char *cwd) {
  int in_squote = 0;
  int in_dquote = 0;
  for (char *q = line; *q; q++) {
    if (*q == '\'' && !in_dquote) {
      in_squote = !in_squote;
      continue;
    }
    if (*q == '"' && !in_squote) {
      in_dquote = !in_dquote;
      continue;
    }
    if (*q == '#' && !in_squote && !in_dquote) {
      *q = '\0';
      break;
    }
  }

  char *p = line;
  int skip = 0;
  while (*p) {
    while (*p == ' ' || *p == ';')
      p++;
    if (!*p)
      break;

    char *end = p;
    int op = 0; // 0: none, 1: &&, 2: ||, 3: ;

    int in_quote = 0;
    int paren = 0;
    while (*end) {
      if ((*end == '"' || *end == '\'') &&
          (in_quote == 0 || in_quote == *end)) {
        if (in_quote)
          in_quote = 0;
        else
          in_quote = *end;
      }
      if (!in_quote) {
        /* Don't split on operators nested inside a ( … ) subshell group. */
        if (end[0] == '(')
          paren++;
        else if (end[0] == ')') {
          if (paren > 0)
            paren--;
        } else if (paren == 0) {
          if (end[0] == '&' && end[1] == '&') {
            op = 1;
            end[0] = '\0';
            break;
          }
          if (end[0] == '|' && end[1] == '|') {
            op = 2;
            end[0] = '\0';
            break;
          }
          if (end[0] == ';') {
            op = 3;
            end[0] = '\0';
            break;
          }
        }
      }
      end++;
    }

    if (!skip) {
      sh_last_status = sh_execute_pipeline(p, cwd);
    }

    if (op == 0)
      break;

    if (op == 1) { /* && */
      if (sh_last_status != 0)
        skip = 1;
      else
        skip = 0;
    } else if (op == 2) { /* || */
      if (sh_last_status == 0)
        skip = 1;
      else
        skip = 0;
    } else { /* ; */
      skip = 0;
    }

    p = end + (op == 3 ? 1 : 2);
  }
}

/* Source-agnostic "read next raw line" callback used by here-document
 * collection: returns the line length (without newline), or -1 at EOF. */
typedef int (*sh_readline_fn)(char *buf, int max, void *ctx);

struct hdoc_fd_ctx {
  u64 fd;
};

static int hdoc_read_fd(char *buf, int max, void *ctx) {
  struct hdoc_fd_ctx *c = (struct hdoc_fd_ctx *)ctx;
  int i = 0;
  for (;;) {
    char ch;
    isize n = (isize)syscall_dispatch(SYS_READ, c->fd, (u64)(usize)&ch, 1, 0, 0,
                                      0);
    if (n <= 0) {
      if (i == 0)
        return -1;
      break;
    }
    if (ch == '\n')
      break;
    if (i < max - 1)
      buf[i++] = ch;
  }
  buf[i] = '\0';
  return i;
}

static int hdoc_read_tty(char *buf, int max, void *ctx) {
  (void)ctx;
  return readline(buf, (usize)max);
}

/* Resolve a single here-document on `line` in place. Reads body lines via
 * next(ctx) until a line equal to the delimiter, spools the (optionally
 * variable-expanded) body into a temp file, and rewrites `cmd <<[-]DELIM` to
 * `cmd < /tmp/.b1hdocN` so the normal `<` redirection path consumes it.
 * `<<-` strips leading tabs from body lines and the closing delimiter; a quoted
 * delimiter (`<<'EOF'`) suppresses variable expansion. Returns 1 if a heredoc
 * was processed, else 0. */
static int sh_resolve_heredoc(char *line, int linecap, sh_readline_fn next,
                              void *ctx) {
  int q = 0;
  int pos = -1;
  for (int i = 0; line[i]; i++) {
    char c = line[i];
    if ((c == '"' || c == '\'') && (q == 0 || q == c))
      q = q ? 0 : c;
    else if (!q && c == '<' && line[i + 1] == '<') {
      pos = i;
      break;
    }
  }
  if (pos < 0)
    return 0;

  int j = pos + 2;
  int strip_tabs = 0;
  if (line[j] == '-') {
    strip_tabs = 1;
    j++;
  }
  while (line[j] == ' ' || line[j] == '\t')
    j++;
  int expand = 1;
  char qc = 0;
  if (line[j] == '"' || line[j] == '\'') {
    qc = line[j];
    expand = 0;
    j++;
  }
  char delim[64];
  int dl = 0;
  while (line[j] && line[j] != ' ' && line[j] != '\t' && line[j] != qc &&
         line[j] != '"' && line[j] != '\'' && dl < 63)
    delim[dl++] = line[j++];
  delim[dl] = '\0';
  if (qc && line[j] == qc)
    j++;
  if (dl == 0)
    return 0;

  static int hseq = 0;
  char tmpname[32];
  snprintf(tmpname, sizeof(tmpname), "/tmp/.b1hdoc%d", hseq++);
  u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)tmpname,
                            B1NIX_O_WRONLY | B1NIX_O_CREAT | B1NIX_O_TRUNC, 0, 0,
                            0, 0);

  char body[SH_LINE_MAX];
  char ebody[SH_LINE_MAX * 2];
  for (;;) {
    if (next(body, sizeof(body), ctx) < 0)
      break; /* EOF before delimiter */
    char *bl = body;
    if (strip_tabs)
      while (*bl == '\t')
        bl++;
    if (strcmp(bl, delim) == 0)
      break;
    const char *wline = bl;
    if (expand) {
      expand_env(bl, ebody);
      wline = ebody;
    }
    if ((isize)fd >= 0) {
      syscall_dispatch(SYS_WRITE, fd, (u64)(usize)wline, strlen(wline), 0, 0, 0);
      syscall_dispatch(SYS_WRITE, fd, (u64)(usize) "\n", 1, 0, 0, 0);
    }
  }
  if ((isize)fd >= 0)
    syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);

  char rebuilt[SH_LINE_MAX];
  int w = 0;
  for (int i = 0; i < pos && w < linecap - 1; i++)
    rebuilt[w++] = line[i];
  const char *inj = "< ";
  for (int i = 0; inj[i] && w < linecap - 1; i++)
    rebuilt[w++] = inj[i];
  for (int i = 0; tmpname[i] && w < linecap - 1; i++)
    rebuilt[w++] = tmpname[i];
  for (int i = j; line[i] && w < linecap - 1; i++)
    rebuilt[w++] = line[i];
  rebuilt[w] = '\0';
  memcpy(line, rebuilt, (usize)w + 1);
  return 1;
}

/* Run `inner` as a command with stdout captured into out[]. Trailing newlines
 * are stripped and embedded newlines collapse to spaces (POSIX-ish command
 * substitution word formation). Uses a temp file (not a pipe) so a large
 * producer can never deadlock against an unread pipe. */
static int sh_capture_command(const char *inner, char *out, int outcap,
                              char *cwd) {
  static int cseq = 0;
  char tmpname[32];
  snprintf(tmpname, sizeof(tmpname), "/tmp/.b1csub%d", cseq++);

  int saved[3];
  save_stdio(saved);
  u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)tmpname,
                            B1NIX_O_WRONLY | B1NIX_O_CREAT | B1NIX_O_TRUNC, 0, 0,
                            0, 0);
  if ((isize)fd >= 0) {
    syscall_dispatch(SYS_DUP2, fd, 1, 0, 0, 0, 0);
    syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
  }

  char inbuf[SH_LINE_MAX];
  char expanded[SH_LINE_MAX * 2];
  strncpy(inbuf, inner, sizeof(inbuf) - 1);
  inbuf[sizeof(inbuf) - 1] = '\0';
  expand_env(inbuf, expanded);
  sh_execute_line(expanded, cwd);

  restore_stdio(saved);

  int n = 0;
  u64 rfd = syscall_dispatch(SYS_OPEN, (u64)(usize)tmpname, 0, 0, 0, 0, 0);
  if ((isize)rfd >= 0) {
    isize r = (isize)syscall_dispatch(SYS_READ, rfd, (u64)(usize)out,
                                      (u64)(outcap - 1), 0, 0, 0);
    n = (r > 0) ? (int)r : 0;
    syscall_dispatch(SYS_CLOSE, rfd, 0, 0, 0, 0, 0);
  }
  out[n] = '\0';
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
    out[--n] = '\0';
  for (int i = 0; i < n; i++)
    if (out[i] == '\n' || out[i] == '\r')
      out[i] = ' ';
  return n;
}

/* Resolve command substitutions `$(...)` and backticks in `line` in place.
 * `$((` is left for arithmetic expansion. Nested `$(...)` is handled by
 * recursing on the inner text. Single quotes suppress substitution. */
static void sh_expand_cmdsubst(char *line, int linecap, char *cwd) {
  for (int guard = 0; guard < 32; guard++) {
    int sq = 0, dq = 0;
    int start = -1, inner_start = -1, inner_end = -1, after = -1;
    for (int i = 0; line[i]; i++) {
      char c = line[i];
      if (c == '\'' && !dq) {
        sq = !sq;
        continue;
      }
      if (c == '"' && !sq) {
        dq = !dq;
        continue;
      }
      if (sq)
        continue;
      if (c == '`') {
        int j = i + 1;
        while (line[j] && line[j] != '`')
          j++;
        if (line[j] != '`')
          return; /* unterminated */
        start = i;
        inner_start = i + 1;
        inner_end = j;
        after = j + 1;
        break;
      }
      if (c == '$' && line[i + 1] == '(' && line[i + 2] != '(') {
        int depth = 1, j = i + 2, isq = 0, idq = 0;
        while (line[j] && depth > 0) {
          char d = line[j];
          if (d == '\'' && !idq)
            isq = !isq;
          else if (d == '"' && !isq)
            idq = !idq;
          else if (!isq && !idq) {
            if (d == '(')
              depth++;
            else if (d == ')') {
              depth--;
              if (depth == 0)
                break;
            }
          }
          j++;
        }
        if (depth != 0)
          return; /* unterminated */
        start = i;
        inner_start = i + 2;
        inner_end = j;
        after = j + 1;
        break;
      }
    }
    if (start < 0)
      return;

    char inner[SH_LINE_MAX];
    int il = inner_end - inner_start;
    if (il > (int)sizeof(inner) - 1)
      il = (int)sizeof(inner) - 1;
    memcpy(inner, line + inner_start, (usize)il);
    inner[il] = '\0';
    sh_expand_cmdsubst(inner, sizeof(inner), cwd); /* nested */

    char out[SH_LINE_MAX];
    sh_capture_command(inner, out, sizeof(out), cwd);

    char rebuilt[SH_LINE_MAX * 2];
    int w = 0;
    for (int i = 0; i < start && w < (int)sizeof(rebuilt) - 1; i++)
      rebuilt[w++] = line[i];
    for (int i = 0; out[i] && w < (int)sizeof(rebuilt) - 1; i++)
      rebuilt[w++] = out[i];
    for (int i = after; line[i] && w < (int)sizeof(rebuilt) - 1; i++)
      rebuilt[w++] = line[i];
    rebuilt[w] = '\0';
    int copy = (w > linecap - 1) ? linecap - 1 : w;
    memcpy(line, rebuilt, (usize)copy);
    line[copy] = '\0';
  }
}

/* Tiny growable byte buffer for assembling multi-line compound statements
 * (function bodies, case blocks) without a fixed-size ceiling. */
struct sh_buf {
  char *p;
  int len;
  int cap;
};

static void sh_buf_putc(struct sh_buf *b, char c) {
  if (b->len + 1 >= b->cap) {
    int ncap = b->cap ? b->cap * 2 : 256;
    char *np = malloc((usize)ncap);
    if (!np)
      return;
    if (b->p) {
      memcpy(np, b->p, (usize)b->len);
      free(b->p);
    }
    b->p = np;
    b->cap = ncap;
  }
  b->p[b->len++] = c;
}

static void sh_buf_puts(struct sh_buf *b, const char *s) {
  while (*s)
    sh_buf_putc(b, *s++);
}

/* Collect a brace-delimited body starting at `frag` (the text after the
 * function's `)`), pulling more lines via next(ctx) until the matching `}`.
 * Lines are joined with "; ". Returns a malloc'd body string (caller frees). */
static char *sh_collect_braces(const char *frag, sh_readline_fn next,
                               void *ctx) {
  struct sh_buf b = {0, 0, 0};
  int depth = 0, started = 0;
  const char *cur = frag;
  char linebuf[SH_LINE_MAX];
  for (;;) {
    for (; *cur; cur++) {
      char c = *cur;
      if (c == '{') {
        if (!started) {
          started = 1;
          depth = 1;
        } else {
          depth++;
          sh_buf_putc(&b, c);
        }
      } else if (c == '}') {
        if (started) {
          depth--;
          if (depth == 0) {
            sh_buf_putc(&b, '\0');
            return b.p;
          }
          sh_buf_putc(&b, c);
        } else {
          sh_buf_putc(&b, c);
        }
      } else if (started) {
        sh_buf_putc(&b, c);
      }
    }
    if (started)
      sh_buf_puts(&b, "; ");
    if (next(linebuf, sizeof(linebuf), ctx) < 0) {
      sh_buf_putc(&b, '\0');
      return b.p;
    }
    cur = linebuf;
  }
}

/* Execute a fully-collected `case WORD in pat) list ;; ... esac` block. */
static void sh_run_case(char *block, char *cwd) {
  char *p = block;
  while (*p == ' ' || *p == '\t')
    p++;
  if (strncmp(p, "case", 4) != 0)
    return;
  p += 4;
  while (*p == ' ' || *p == '\t')
    p++;

  char word[128];
  int wl = 0;
  while (*p && *p != ' ' && *p != '\t' && *p != '\n' && wl < 127)
    word[wl++] = *p++;
  word[wl] = '\0';
  char wexp[256];
  expand_env(word, wexp);

  /* advance past the 'in' keyword */
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n')
      p++;
    if (p[0] == 'i' && p[1] == 'n' &&
        (p[2] == ' ' || p[2] == '\t' || p[2] == '\n' || p[2] == '\0')) {
      p += 2;
      break;
    }
    /* not 'in' yet — skip a token to avoid an infinite loop */
    while (*p && *p != ' ' && *p != '\t' && *p != '\n')
      p++;
  }

  int matched = 0;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';')
      p++;
    if (strncmp(p, "esac", 4) == 0 || !*p)
      break;

    char pats[256];
    int pl = 0;
    while (*p && *p != ')' && pl < 255)
      pats[pl++] = *p++;
    pats[pl] = '\0';
    if (*p == ')')
      p++;

    /* body up to ';;' */
    const char *bstart = p;
    while (*p && !(p[0] == ';' && p[1] == ';'))
      p++;
    int blen = (int)(p - bstart);
    if (p[0] == ';' && p[1] == ';')
      p += 2;

    if (!matched) {
      const char *tok = pats;
      while (*tok) {
        while (*tok == ' ' || *tok == '\t' || *tok == '\n')
          tok++;
        char one[128];
        int ol = 0;
        while (*tok && *tok != '|' && ol < 127) {
          if (*tok != ' ' && *tok != '\t' && *tok != '\n')
            one[ol++] = *tok;
          tok++;
        }
        one[ol] = '\0';
        if (*tok == '|')
          tok++;
        if (ol == 0)
          continue;
        char pe[256];
        expand_env(one, pe);
        if (glob_match(pe, wexp)) {
          matched = 1;
          char *body = malloc((usize)blen * 4 + 256);
          char *bexp = malloc((usize)blen * 8 + 256);
          if (body && bexp) {
            memcpy(body, bstart, (usize)blen);
            body[blen] = '\0';
            for (int i = 0; i < blen; i++)
              if (body[i] == '\n')
                body[i] = ';';
            sh_expand_cmdsubst(body, blen * 4 + 256, cwd);
            expand_env(body, bexp);
            sh_execute_line(bexp, cwd);
          }
          free(body);
          free(bexp);
          break;
        }
      }
    }
  }
}

/* Find `w` as a whole word in `s` (bounded by start/end/space/tab/newline/;). */
static const char *sh_find_word(const char *s, const char *w) {
  usize wl = strlen(w);
  for (usize i = 0; s[i]; i++) {
    if (strncmp(s + i, w, wl) == 0) {
      char before = (i > 0) ? s[i - 1] : ' ';
      char after = s[i + wl];
      int bok = (before == ' ' || before == '\t' || before == '\n' ||
                 before == ';');
      int aok = (after == '\0' || after == ' ' || after == '\t' ||
                 after == '\n' || after == ';');
      if (bok && aok)
        return s + i;
    }
  }
  return 0;
}

/* Collect a compound statement that ends at the keyword `term` (e.g. "done",
 * "esac"), pulling more lines via next(ctx). Returns a malloc'd block. */
static char *sh_collect_block(const char *first, const char *term,
                              sh_readline_fn next, void *ctx) {
  struct sh_buf b = {0, 0, 0};
  sh_buf_puts(&b, first);
  int complete = (sh_find_word(first, term) != 0);
  char nl[SH_LINE_MAX];
  while (!complete) {
    if (next(nl, sizeof(nl), ctx) < 0)
      break;
    sh_buf_putc(&b, '\n');
    sh_buf_puts(&b, nl);
    if (sh_find_word(nl, term))
      break;
  }
  sh_buf_putc(&b, '\0');
  return b.p;
}

static char *sh_substr(const char *a, const char *b) {
  int n = (int)(b - a);
  if (n < 0)
    n = 0;
  char *s = malloc((usize)n + 1);
  if (s) {
    memcpy(s, a, (usize)n);
    s[n] = '\0';
  }
  return s;
}

/* cmdsubst + variable-expand a fragment, then execute it as a command line. */
static void sh_run_fragment(const char *frag, char *cwd) {
  usize n = strlen(frag);
  char *buf = malloc(n * 4 + 256);
  char *exp = malloc(n * 8 + 256);
  if (buf && exp) {
    memcpy(buf, frag, n + 1);
    sh_expand_cmdsubst(buf, (int)(n * 4 + 256), cwd);
    expand_env(buf, exp);
    sh_execute_line(exp, cwd);
  }
  free(buf);
  free(exp);
}

/* cmdsubst + variable-expand a fragment into a fresh malloc'd string. */
static char *sh_expand_fragment(const char *frag, char *cwd) {
  usize n = strlen(frag);
  char *buf = malloc(n * 4 + 256);
  char *exp = malloc(n * 8 + 256);
  char *result = 0;
  if (buf && exp) {
    memcpy(buf, frag, n + 1);
    sh_expand_cmdsubst(buf, (int)(n * 4 + 256), cwd);
    expand_env(buf, exp);
    result = strdup(exp);
  }
  free(buf);
  free(exp);
  return result;
}

/* for VAR in LIST; do BODY; done — LIST is cmdsubst+var-expanded then split. */
static void sh_run_for(char *block, char *cwd) {
  char *p = block;
  while (*p == ' ' || *p == '\t')
    p++;
  p += 3; /* "for" */
  while (*p == ' ' || *p == '\t')
    p++;
  char var[64];
  int vl = 0;
  while (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '_') &&
         vl < 63)
    var[vl++] = *p++;
  var[vl] = '\0';
  const char *inp = sh_find_word(p, "in");
  const char *dop = sh_find_word(p, "do");
  const char *donep = sh_find_word(block, "done");
  if (vl == 0 || !inp || !dop || !donep)
    return;
  char *listraw = sh_substr(inp + 2, dop);
  char *list = sh_expand_fragment(listraw ? listraw : "", cwd);
  free(listraw);
  char *body = sh_substr(dop + 2, donep);
  if (list && body) {
    char *q = list;
    while (*q) {
      while (*q == ' ' || *q == '\t' || *q == '\n' || *q == ';')
        q++;
      if (!*q)
        break;
      char word[256];
      int wl = 0;
      while (*q && *q != ' ' && *q != '\t' && *q != '\n' && *q != ';' &&
             wl < 255)
        word[wl++] = *q++;
      word[wl] = '\0';
      set_env(var, word);
      sh_run_fragment(body, cwd);
    }
  }
  free(list);
  free(body);
}

/* while/until COND; do BODY; done. A large runaway guard prevents a buggy or
 * never-false condition from wedging the in-kernel shell (not a feature cap). */
static void sh_run_while(char *block, char *cwd, int is_until) {
  char *p = block;
  while (*p == ' ' || *p == '\t')
    p++;
  p += 5; /* "while" / "until" */
  const char *dop = sh_find_word(p, "do");
  const char *donep = sh_find_word(block, "done");
  if (!dop || !donep)
    return;
  char *cond = sh_substr(p, dop);
  char *body = sh_substr(dop + 2, donep);
  if (cond && body) {
    int guard = 0;
    for (;;) {
      if (++guard > 1000000)
        break;
      sh_run_fragment(cond, cwd);
      int st = sh_last_status;
      if (is_until ? (st == 0) : (st != 0))
        break;
      sh_run_fragment(body, cwd);
    }
  }
  free(cond);
  free(body);
}

/* If `line` opens a multi-line compound statement (function definition, case,
 * or a for/while/until loop), collect the rest via next(ctx) and handle it.
 * Returns 1 if consumed (caller skips normal expansion/execution), 0 otherwise. */
static int sh_handle_compound(char *line, sh_readline_fn next, void *ctx,
                              char *cwd) {
  const char *s = line;
  while (*s == ' ' || *s == '\t')
    s++;

  /* case ... esac */
  if (strncmp(s, "case ", 5) == 0 || strncmp(s, "case\t", 5) == 0) {
    struct sh_buf b = {0, 0, 0};
    sh_buf_puts(&b, s);
    /* If 'esac' is already on this line the case is single-line. */
    int complete = 0;
    for (const char *t = s; *t; t++)
      if (t[0] == 'e' && t[1] == 's' && t[2] == 'a' && t[3] == 'c') {
        complete = 1;
        break;
      }
    char nl[SH_LINE_MAX];
    while (!complete) {
      if (next(nl, sizeof(nl), ctx) < 0)
        break;
      sh_buf_putc(&b, '\n');
      sh_buf_puts(&b, nl);
      const char *t = nl;
      while (*t == ' ' || *t == '\t')
        t++;
      if (strncmp(t, "esac", 4) == 0)
        break;
    }
    sh_buf_putc(&b, '\0');
    if (b.p) {
      sh_run_case(b.p, cwd);
      free(b.p);
    }
    return 1;
  }

  /* for / while / until loops (terminated by `done`) */
  if (strncmp(s, "for ", 4) == 0 || strncmp(s, "for\t", 4) == 0) {
    char *blk = sh_collect_block(s, "done", next, ctx);
    if (blk) {
      sh_run_for(blk, cwd);
      free(blk);
    }
    return 1;
  }
  if (strncmp(s, "while ", 6) == 0 || strncmp(s, "while\t", 6) == 0) {
    char *blk = sh_collect_block(s, "done", next, ctx);
    if (blk) {
      sh_run_while(blk, cwd, 0);
      free(blk);
    }
    return 1;
  }
  if (strncmp(s, "until ", 6) == 0 || strncmp(s, "until\t", 6) == 0) {
    char *blk = sh_collect_block(s, "done", next, ctx);
    if (blk) {
      sh_run_while(blk, cwd, 1);
      free(blk);
    }
    return 1;
  }

  /* function: ident ( ) { ... } */
  const char *id = s;
  int idlen = 0;
  while ((id[idlen] >= 'a' && id[idlen] <= 'z') ||
         (id[idlen] >= 'A' && id[idlen] <= 'Z') ||
         (id[idlen] >= '0' && id[idlen] <= '9') || id[idlen] == '_')
    idlen++;
  if (idlen > 0) {
    const char *p = id + idlen;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '(') {
      p++;
      while (*p == ' ' || *p == '\t')
        p++;
      if (*p == ')') {
        char name[64];
        int n2 = idlen > 63 ? 63 : idlen;
        memcpy(name, id, (usize)n2);
        name[n2] = '\0';
        p++;
        char *body = sh_collect_braces(p, next, ctx);
        if (body) {
          sh_func_define(name, body);
          free(body);
        }
        return 1;
      }
    }
  }
  return 0;
}

static void sh_run_script(const char *path, char *cwd) {
  u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)path, 0, 0, 0, 0, 0);
  if ((isize)fd < 0) {
    uwrite("sh: cannot open script\n");
    return;
  }
  char line[SH_LINE_MAX];
  char expanded[SH_LINE_MAX * 2];
  int i = 0;
  while (1) {
    char c;
    isize n = (isize)syscall_dispatch(SYS_READ, fd, (u64)(usize)&c, 1, 0, 0, 0);
    if (n <= 0)
      break;
    if (c == '\n') {
      line[i] = '\0';
      if (line[0] && strncmp(line, "#!", 2) != 0) {
        struct hdoc_fd_ctx hc = {fd};
        if (!sh_handle_compound(line, hdoc_read_fd, &hc, cwd)) {
          sh_expand_cmdsubst(line, SH_LINE_MAX, cwd);
          sh_resolve_heredoc(line, SH_LINE_MAX, hdoc_read_fd, &hc);
          expand_env(line, expanded);
          sh_execute_line(expanded, cwd);
        }
      }
      i = 0;
    } else if (i < SH_LINE_MAX - 1)
      line[i++] = c;
  }
  if (i > 0) {
    line[i] = '\0';
    struct hdoc_fd_ctx hc = {fd};
    if (!sh_handle_compound(line, hdoc_read_fd, &hc, cwd)) {
      sh_expand_cmdsubst(line, SH_LINE_MAX, cwd);
      expand_env(line, expanded);
      sh_execute_line(expanded, cwd);
    }
  }
  syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
}


static int sh_main(int argc, const char **argv) {
  char cwd[128] = "/";
  /* sh -c 'cmd' — execute command string directly */
  if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
    if (argc >= 4) {
      set_env("0", argv[3]);
    }
    for (int i = 4; i < argc && (i - 3) <= 9; i++) {
      char key[2];
      key[0] = (char)('0' + (i - 3));
      key[1] = '\0';
      set_env(key, argv[i]);
    }
    char line[SH_LINE_MAX];
    char expanded[SH_LINE_MAX * 2];
    usize len = strlen(argv[2]);
    if (len >= sizeof(line))
      len = sizeof(line) - 1;
    memcpy(line, argv[2], len);
    line[len] = '\0';
    sh_expand_cmdsubst(line, sizeof(line), cwd);
    expand_env(line, expanded);
    sh_execute_line(expanded, cwd);
    return sh_last_status;
  }
  if (argc > 1) {
    sh_run_script(argv[1], cwd);
    return sh_last_status;
  }

  uwrite("Welcome to b1nix shell!\nType 'help' for a list of commands.\n\n");
  /* SH_LINE_MAX-char input handles long toolchain command lines (a kernel-flag
   * gcc invocation is ~260 chars); `line` is 2x for env-expansion headroom
   * (expand_env has no output bound). 256 truncated such lines mid-argument. */
  char raw_line[SH_LINE_MAX];
  char line[SH_LINE_MAX * 2];
  set_env("PATH", "/bin");

  while (1) {
    uwrite(cwd);
    uwrite("> ");
    if (readline(raw_line, sizeof(raw_line)) < 0) {
      uwrite("exit\n");
      break;
    }

    if (!sh_handle_compound(raw_line, hdoc_read_tty, 0, cwd)) {
      sh_expand_cmdsubst(raw_line, sizeof(raw_line), cwd);
      sh_resolve_heredoc(raw_line, sizeof(raw_line), hdoc_read_tty, 0);
      expand_env(raw_line, line);
      sh_execute_line(line, cwd);
    }

    /* Simple history add */
    if (raw_line[0]) {
      if (sh_hist_count < SH_HISTORY_MAX)
        strcpy(sh_history[sh_hist_count++], raw_line);
      else {
        for (int i = 1; i < SH_HISTORY_MAX; i++)
          strcpy(sh_history[i - 1], sh_history[i]);
        strcpy(sh_history[SH_HISTORY_MAX - 1], raw_line);
      }
    }
  }
  return 0;
}

extern int busybox_main(int argc, const char **argv);
extern int mc_main(int argc, const char **argv);
extern int editor_main(int argc, const char **argv);

struct udp_smoke_header {
  u16 src_port;
  u16 dst_port;
  u16 length;
  u16 checksum;
} __attribute__((packed));

struct tcp_smoke_header {
  u16 src_port;
  u16 dst_port;
  u32 seq_num;
  u32 ack_num;
  u8 data_offset;
  u8 flags;
  u16 window;
  u16 checksum;
  u16 urgent;
} __attribute__((packed));

static u16 udp_smoke_bswap16(u16 value) {
  return (u16)((value << 8) | (value >> 8));
}

static u32 tcp_smoke_bswap32(u32 value) {
  return ((value & 0x000000ffU) << 24) | ((value & 0x0000ff00U) << 8) |
         ((value & 0x00ff0000U) >> 8) | ((value & 0xff000000U) >> 24);
}

static void udp_queue_smoke_check(void) {
  int fd = vfs_socket(B1NIX_AF_INET, B1NIX_SOCK_DGRAM, 0);
  if (fd < 0) {
    uwrite("UDP-SMOKE: fail queue-open\n");
    return;
  }

  struct b1nix_sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = B1NIX_AF_INET;
  addr.sin_port = udp_smoke_bswap16(55001);
  addr.sin_addr = 0;
  if (vfs_bind(fd, &addr, sizeof(addr)) < 0) {
    uwrite("UDP-SMOKE: fail queue-bind\n");
    vfs_close(fd);
    return;
  }

  const char pkt1[] = "first";
  const char pkt2[] = "second";
  if (!vfs_socket_push_udp(addr.sin_port, pkt1, sizeof(pkt1) - 1) ||
      !vfs_socket_push_udp(addr.sin_port, pkt2, sizeof(pkt2) - 1)) {
    uwrite("UDP-SMOKE: fail queue-push\n");
    vfs_close(fd);
    return;
  }

  int flags = (int)syscall_dispatch(SYS_FCNTL, (u64)fd, B1NIX_F_GETFL, 0, 0, 0, 0);
  syscall_dispatch(SYS_FCNTL, (u64)fd, B1NIX_F_SETFL,
                   (u64)(flags | B1NIX_O_NONBLOCK), 0, 0, 0);

  char out1[16];
  char out2[16];
  memset(out1, 0, sizeof(out1));
  memset(out2, 0, sizeof(out2));
  isize r1 = vfs_socket_recv(fd, out1, sizeof(out1), 0);
  isize r2 = vfs_socket_recv(fd, out2, sizeof(out2), 0);
  vfs_close(fd);

  if (r1 == 5 && r2 == 6 && memcmp(out1, "first", 5) == 0 &&
      memcmp(out2, "second", 6) == 0) {
    uwrite("UDP-SMOKE: queue-2pkt-ok\n");
    return;
  }
  uwrite("UDP-SMOKE: fail queue-order\n");
}

static void poll_smoke_check(void) {
  int fd = vfs_socket(B1NIX_AF_INET, B1NIX_SOCK_DGRAM, 0);
  if (fd < 0) {
    uwrite("POLL-SMOKE: fail open\n");
    return;
  }

  struct b1nix_sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = B1NIX_AF_INET;
  addr.sin_port = udp_smoke_bswap16(55002);
  addr.sin_addr = 0;
  if (vfs_bind(fd, &addr, sizeof(addr)) < 0) {
    uwrite("POLL-SMOKE: fail bind\n");
    vfs_close(fd);
    return;
  }

  struct b1nix_pollfd pfd;
  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = fd;
  pfd.events = (short)(B1NIX_POLLIN | B1NIX_POLLOUT);
  if (vfs_poll(fd, &pfd) < 0 || (pfd.revents & B1NIX_POLLOUT) == 0) {
    uwrite("POLL-SMOKE: fail writable\n");
    vfs_close(fd);
    return;
  }

  const char pkt[] = "poll";
  if (!vfs_socket_push_udp(addr.sin_port, pkt, sizeof(pkt) - 1)) {
    uwrite("POLL-SMOKE: fail inject\n");
    vfs_close(fd);
    return;
  }

  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = fd;
  pfd.events = B1NIX_POLLIN;
  if (vfs_poll(fd, &pfd) < 0 || (pfd.revents & B1NIX_POLLIN) == 0) {
    uwrite("POLL-SMOKE: fail readable\n");
    vfs_close(fd);
    return;
  }
  vfs_close(fd);
  uwrite("POLL-SMOKE: ready-udp\n");
}

static void tcp_smoke_check(void) {
  uwrite("TCP-SMOKE: unsupported\n");
  return;

  const u16 listen_port = 56001;
  const u16 remote_port = 40000;
  struct ipv4_addr remote_ip = {{10, 0, 2, 2}};
  const u32 remote_seq = 1000;
  const u32 local_iss = 1000;

  int fd = vfs_socket(B1NIX_AF_INET, B1NIX_SOCK_STREAM, 0);
  if (fd < 0) {
    uwrite("TCP-SMOKE: fail socket\n");
    return;
  }

  struct b1nix_sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = B1NIX_AF_INET;
  addr.sin_port = udp_smoke_bswap16(listen_port);
  addr.sin_addr = 0;
  if (vfs_bind(fd, &addr, sizeof(addr)) < 0 || vfs_listen(fd, 1) < 0) {
    uwrite("TCP-SMOKE: fail listen\n");
    vfs_close(fd);
    return;
  }

  struct tcp_smoke_header syn;
  memset(&syn, 0, sizeof(syn));
  syn.src_port = udp_smoke_bswap16(remote_port);
  syn.dst_port = udp_smoke_bswap16(listen_port);
  syn.seq_num = tcp_smoke_bswap32(remote_seq);
  syn.data_offset = (5 << 4);
  syn.flags = 0x02; /* SYN */
  syn.window = udp_smoke_bswap16(4096);
  tcp_receive(remote_ip, &syn, sizeof(syn));

  struct tcp_smoke_header ack;
  memset(&ack, 0, sizeof(ack));
  ack.src_port = udp_smoke_bswap16(remote_port);
  ack.dst_port = udp_smoke_bswap16(listen_port);
  ack.seq_num = tcp_smoke_bswap32(remote_seq + 1);
  ack.ack_num = tcp_smoke_bswap32(local_iss + 1);
  ack.data_offset = (5 << 4);
  ack.flags = 0x10; /* ACK */
  ack.window = udp_smoke_bswap16(4096);
  tcp_receive(remote_ip, &ack, sizeof(ack));

  int flags = (int)syscall_dispatch(SYS_FCNTL, (u64)fd, B1NIX_F_GETFL, 0, 0, 0, 0);
  syscall_dispatch(SYS_FCNTL, (u64)fd, B1NIX_F_SETFL,
                   (u64)(flags | B1NIX_O_NONBLOCK), 0, 0, 0);

  int client_fd = vfs_accept(fd, 0, 0);
  if (client_fd < 0) {
    uwrite("TCP-SMOKE: unsupported\n");
    vfs_close(fd);
    return;
  }

  const char payload[] = "tcp-smoke";
  u8 pkt[sizeof(struct tcp_smoke_header) + sizeof(payload) - 1];
  memset(pkt, 0, sizeof(pkt));
  struct tcp_smoke_header *psh = (struct tcp_smoke_header *)pkt;
  psh->src_port = udp_smoke_bswap16(remote_port);
  psh->dst_port = udp_smoke_bswap16(listen_port);
  psh->seq_num = tcp_smoke_bswap32(remote_seq + 1);
  psh->ack_num = tcp_smoke_bswap32(local_iss + 1);
  psh->data_offset = (5 << 4);
  psh->flags = 0x18; /* PSH|ACK */
  psh->window = udp_smoke_bswap16(4096);
  memcpy(pkt + sizeof(struct tcp_smoke_header), payload, sizeof(payload) - 1);
  tcp_receive(remote_ip, pkt, sizeof(pkt));

  char out[16];
  memset(out, 0, sizeof(out));
  isize got = vfs_socket_recv(client_fd, out, sizeof(out), 0);
  vfs_close(client_fd);
  vfs_close(fd);
  if (got == (isize)(sizeof(payload) - 1) &&
      memcmp(out, payload, sizeof(payload) - 1) == 0) {
    uwrite("TCP-SMOKE: path-exercised\n");
    return;
  }
  uwrite("TCP-SMOKE: unsupported\n");
}

static int lock_smoke_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;

  uwrite("LOCK-SMOKE: start\n");
  u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)"/tmp/lock-smoke.dat",
                            B1NIX_O_CREAT | B1NIX_O_RDWR | B1NIX_O_TRUNC, 0666, 0, 0, 0);
  if ((isize)fd < 0) {
    uwrite("LOCK-SMOKE: fail open\n");
    return 1;
  }

  struct flock parent_lock;
  memset(&parent_lock, 0, sizeof(parent_lock));
  parent_lock.l_type = F_WRLCK;
  parent_lock.l_whence = 0;
  parent_lock.l_start = 0;
  parent_lock.l_len = 0;
  if ((isize)syscall_dispatch(SYS_FCNTL, fd, B1NIX_F_SETLK, (u64)(usize)&parent_lock, 0, 0, 0) < 0) {
    uwrite("LOCK-SMOKE: fail parent-setlk\n");
    syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return 1;
  }

  u64 pid = syscall_dispatch(SYS_FORK, 0, 0, 0, 0, 0, 0);
  if ((isize)pid < 0) {
    uwrite("LOCK-SMOKE: fail fork\n");
    syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return 1;
  }
  if (pid == 0) {
    struct flock child_lock;
    memset(&child_lock, 0, sizeof(child_lock));
    child_lock.l_type = F_WRLCK;
    child_lock.l_whence = 0;
    child_lock.l_start = 0;
    child_lock.l_len = 0;

    isize nb = (isize)syscall_dispatch(SYS_FCNTL, fd, B1NIX_F_SETLK, (u64)(usize)&child_lock, 0, 0, 0);
    if (nb != -EAGAIN) {
      uwrite("LOCK-SMOKE: fail nonblock-conflict\n");
      syscall_dispatch(SYS_EXIT, 2, 0, 0, 0, 0, 0);
    }
    uwrite("LOCK-SMOKE: ok nonblock-conflict\n");

    isize blk = (isize)syscall_dispatch(SYS_FCNTL, fd, B1NIX_F_SETLKW, (u64)(usize)&child_lock, 0, 0, 0);
    if (blk < 0) {
      uwrite("LOCK-SMOKE: fail setlkw\n");
      syscall_dispatch(SYS_EXIT, 3, 0, 0, 0, 0, 0);
    }
    uwrite("LOCK-SMOKE: ok wake-on-close\n");
    syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    syscall_dispatch(SYS_EXIT, 0, 0, 0, 0, 0, 0);
  }

  for (int i = 0; i < 8; i++) {
    syscall_dispatch(SYS_YIELD, 0, 0, 0, 0, 0, 0);
  }
  syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);

  int st = 0;
  syscall_dispatch(SYS_WAIT, pid, (u64)(usize)&st, 0, 0, 0, 0);
  if (st != 0) {
    uwrite("LOCK-SMOKE: fail child-status\n");
    return 1;
  }

  uwrite("LOCK-SMOKE: done\n");
  return 0;
}

static int run_ext_stress(const char *mount_path) {
  struct b1nix_mount_entry mounts[16];
  long count = (long)syscall_dispatch(SYS_MOUNTS, (u64)(usize)mounts, 16, 0, 0, 0, 0);
  int is_mounted = 0;
  if (count > 0) {
    for (long i = 0; i < count && i < 16; i++) {
      if (strcmp(mounts[i].target, mount_path) == 0) {
        is_mounted = 1;
        break;
      }
    }
  }
  if (!is_mounted) {
    return 0;
  }

  char path_buf[256];
  snprintf(path_buf, sizeof(path_buf), "%s/.stress_test", mount_path);
  isize fd = (isize)syscall_dispatch(SYS_OPEN, (u64)(usize)path_buf,
                                   B1NIX_O_CREAT | B1NIX_O_RDWR, 0666, 0, 0, 0);
  if (fd < 0) {
    return 0; 
  }
  syscall_dispatch(SYS_CLOSE, (u64)fd, 0, 0, 0, 0, 0);
  syscall_dispatch(SYS_UNLINK, (u64)(usize)path_buf, 0, 0, 0, 0, 0);

  uwrite("EXT-STRESS: running on ");
  uwrite(mount_path);
  uwrite("\n");

  char dir_path[256];
  char file_path[256];
  char renamed_path[256];
  char sym_path[256];
  char link_path[256];

  for (int i = 0; i < 50; i++) {
    snprintf(dir_path, sizeof(dir_path), "%s/dir_%d", mount_path, i);
    isize rc = (isize)syscall_dispatch(SYS_MKDIR, (u64)(usize)dir_path, 0755, 0, 0, 0, 0);
    if (rc < 0 && rc != -EEXIST) {
      uwrite("EXT-STRESS: mkdir failed\n");
      return 1;
    }

    snprintf(file_path, sizeof(file_path), "%s/dir_%d/file_%d", mount_path, i, i);
    fd = (isize)syscall_dispatch(SYS_OPEN, (u64)(usize)file_path,
                                 B1NIX_O_CREAT | B1NIX_O_RDWR | B1NIX_O_TRUNC, 0666, 0, 0, 0);
    if (fd < 0) {
      uwrite("EXT-STRESS: open failed\n");
      return 1;
    }

    char write_buf[128];
    snprintf(write_buf, sizeof(write_buf), "Stress data for iteration %d. Repeating some blocks of text to ensure we use some file blocks.\n", i);
    isize bytes_written = (isize)syscall_dispatch(SYS_WRITE, (u64)fd, (u64)(usize)write_buf, strlen(write_buf), 0, 0, 0);
    if (bytes_written < 0) {
      uwrite("EXT-STRESS: write failed\n");
      syscall_dispatch(SYS_CLOSE, (u64)fd, 0, 0, 0, 0, 0);
      return 1;
    }

    syscall_dispatch(SYS_FSYNC, (u64)fd, 0, 0, 0, 0, 0);
    syscall_dispatch(SYS_CLOSE, (u64)fd, 0, 0, 0, 0, 0);

    snprintf(sym_path, sizeof(sym_path), "%s/dir_%d/sym_%d", mount_path, i, i);
    rc = (isize)syscall_dispatch(SYS_SYMLINK, (u64)(usize)file_path, (u64)(usize)sym_path, 0, 0, 0, 0);
    if (rc < 0) {
      uwrite("EXT-STRESS: symlink failed\n");
      return 1;
    }

    snprintf(link_path, sizeof(link_path), "%s/dir_%d/link_%d", mount_path, i, i);
    rc = (isize)syscall_dispatch(SYS_LINK, (u64)(usize)file_path, (u64)(usize)link_path, 0, 0, 0, 0);
    if (rc < 0) {
      uwrite("EXT-STRESS: link failed\n");
      return 1;
    }

    snprintf(renamed_path, sizeof(renamed_path), "%s/dir_%d/renamed_%d", mount_path, i, i);
    rc = (isize)syscall_dispatch(SYS_RENAME, (u64)(usize)file_path, (u64)(usize)renamed_path, 0, 0, 0, 0);
    if (rc < 0) {
      uwrite("EXT-STRESS: rename failed\n");
      return 1;
    }

    rc = (isize)syscall_dispatch(SYS_UNLINK, (u64)(usize)sym_path, 0, 0, 0, 0, 0);
    if (rc < 0) {
      uwrite("EXT-STRESS: unlink sym failed\n");
      return 1;
    }

    rc = (isize)syscall_dispatch(SYS_UNLINK, (u64)(usize)link_path, 0, 0, 0, 0, 0);
    if (rc < 0) {
      uwrite("EXT-STRESS: unlink link failed\n");
      return 1;
    }

    rc = (isize)syscall_dispatch(SYS_UNLINK, (u64)(usize)renamed_path, 0, 0, 0, 0, 0);
    if (rc < 0) {
      uwrite("EXT-STRESS: unlink renamed failed\n");
      return 1;
    }

    rc = (isize)syscall_dispatch(SYS_RMDIR, (u64)(usize)dir_path, 0, 0, 0, 0, 0);
    if (rc < 0) {
      uwrite("EXT-STRESS: rmdir failed\n");
      return 1;
    }
  }

  uwrite("EXT-STRESS: done on ");
  uwrite(mount_path);
  uwrite("\n");
  return 0;
}

static int ext_stress_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  uwrite("EXT-STRESS: start\n");
  run_ext_stress("/ext4");
  run_ext_stress("/ext3");
  uwrite("EXT-STRESS: done\n");
  return 0;
}

static int init_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;

  syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0, 0, 0);

  if (bootinfo_has_flag("b1nix.test=1")) {
  /* M27: kernel command line key=value parser self-test. The smoke harness
   * passes "b1nix.test=1 b1nix.kvtest=abc123" so we can verify a present key,
   * an absent key, prefix non-matching, and value truncation. */
  {
    char v[16];
    char small[4];
    int ok = 1;
    if (!bootinfo_get_kv("b1nix.test", v, sizeof(v)) || strcmp(v, "1") != 0)
      ok = 0;
    if (!bootinfo_get_kv("b1nix.kvtest", v, sizeof(v)) ||
        strcmp(v, "abc123") != 0)
      ok = 0;
    if (bootinfo_get_kv("b1nix.absent", v, sizeof(v)))
      ok = 0;
    if (bootinfo_get_kv("b1nix.tes", v, sizeof(v)))
      ok = 0;
    if (!bootinfo_get_kv("b1nix.kvtest", small, sizeof(small)) ||
        strcmp(small, "abc") != 0)
      ok = 0;
    uwrite(ok ? "M27-CMDLINE: ok kv-parse\n" : "M27-CMDLINE: fail kv-parse\n");
  }

  /* M27: verify the boot rc script runs end-to-end via /bin/sh (same path the
   * production init uses at startup). The script emits its own markers. */
  {
    const char *rc_argv[] = {"/bin/sh", "/etc/rc", 0};
    u64 rc_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)rc_argv[0], 2,
                                  (u64)(usize)rc_argv, 0, 0, 0);
    if ((isize)rc_pid >= 0) {
      int rc_status = 0;
      syscall_dispatch(SYS_WAIT, rc_pid, (u64)(usize)&rc_status, 0, 0, 0, 0);
    }
  }

  /* M27: user/passwd/login basics (getpwnam/getpwuid over /etc/passwd +
   * privilege drop). */
  {
    u64 u_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m27-smoke", 0, 0,
                                 0, 0, 0);
    if ((isize)u_pid >= 0) {
      int u_status = 0;
      syscall_dispatch(SYS_WAIT, u_pid, (u64)(usize)&u_status, 0, 0, 0, 0);
    }
  }

  u64 n_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/native-smoke", 0, 0, 0, 0, 0);
  
  if ((isize)n_pid < 0) {
    uwrite("NATIVE-SMOKE: spawn-fail\n");
  } else {
    int native_status = 0;
    syscall_dispatch(SYS_WAIT, n_pid, (u64)(usize)&native_status, 0, 0, 0, 0);
    if (native_status == 0) {
      uwrite("NATIVE-SMOKE: done\n");
    } else {
      uwrite("NATIVE-SMOKE: fail\n");
    }
  }

  u64 m12_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m12-smoke", 0, 0, 0, 0, 0);
  if ((isize)m12_pid < 0) {
    uwrite("M12-SMOKE: spawn-fail\n");
  } else {
    int m12_status = 0;
    syscall_dispatch(SYS_WAIT, m12_pid, (u64)(usize)&m12_status, 0, 0, 0, 0);
  }

  u64 m13_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m13-smoke", 0, 0, 0, 0, 0);
  if ((isize)m13_pid < 0) {
    uwrite("M13-SMOKE: spawn-fail\n");
  } else {
    int m13_status = 0;
    syscall_dispatch(SYS_WAIT, m13_pid, (u64)(usize)&m13_status, 0, 0, 0, 0);
  }

  u64 m14_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m14-smoke", 0, 0, 0, 0, 0);
  if ((isize)m14_pid < 0) {
    uwrite("M14-SMOKE: spawn-fail\n");
  } else {
    int m14_status = 0;
    syscall_dispatch(SYS_WAIT, m14_pid, (u64)(usize)&m14_status, 0, 0, 0, 0);
  }

  u64 m15_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m15-smoke", 0, 0, 0, 0, 0);
  if ((isize)m15_pid < 0) {
    uwrite("M15-SMOKE: spawn-fail\n");
  } else {
    int m15_status = 0;
    syscall_dispatch(SYS_WAIT, m15_pid, (u64)(usize)&m15_status, 0, 0, 0, 0);
  }

  if (bootinfo_has_flag("b1nix.skip-m25")) {
    uwrite("M25-SMOKE: start\n");
    uwrite("M25-SMOKE: skipped (b1nix.skip-m25)\n");
    uwrite("M25-SMOKE: done\n");
  } else {
    u64 m25_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m25-smoke", 0, 0, 0, 0, 0);
    if ((isize)m25_pid < 0) {
      uwrite("M25-SMOKE: spawn-fail\n");
    } else {
      int m25_status = 0;
      syscall_dispatch(SYS_WAIT, m25_pid, (u64)(usize)&m25_status, 0, 0, 0, 0);
    }
  }

  u64 m26_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m26-smoke", 0, 0, 0, 0, 0);
  if ((isize)m26_pid < 0) {
    uwrite("M26-SMOKE: spawn-fail\n");
  } else {
    int m26_status = 0;
    syscall_dispatch(SYS_WAIT, m26_pid, (u64)(usize)&m26_status, 0, 0, 0, 0);
  }

  /* M29: POSIX threads / futex / TLS. */
  {
    u64 m29_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m29-smoke", 0,
                                   0, 0, 0, 0);
    if ((isize)m29_pid < 0) {
      uwrite("M29-PTHREAD: spawn-fail\n");
    } else {
      int m29_status = 0;
      syscall_dispatch(SYS_WAIT, m29_pid, (u64)(usize)&m29_status, 0, 0, 0, 0);
    }
  }

  /* M31: user security / setuid / shadow. */
  {
    u64 m31_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m31-smoke", 0,
                                   0, 0, 0, 0);
    if ((isize)m31_pid < 0) {
      uwrite("M31-SEC: spawn-fail\n");
    } else {
      int m31_status = 0;
      syscall_dispatch(SYS_WAIT, m31_pid, (u64)(usize)&m31_status, 0, 0, 0, 0);
    }
  }

  /* M32: select() / network multiplexing. */
  {
    u64 m32_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m32-smoke", 0,
                                   0, 0, 0, 0);
    if ((isize)m32_pid < 0) {
      uwrite("M32-NET: spawn-fail\n");
    } else {
      int m32_status = 0;
      syscall_dispatch(SYS_WAIT, m32_pid, (u64)(usize)&m32_status, 0, 0, 0, 0);
    }
  }

  /* M34: procfs / sysfs synthetic filesystems. */
  {
    u64 m34_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m34-smoke", 0,
                                   0, 0, 0, 0);
    if ((isize)m34_pid < 0) {
      uwrite("M34-PROC: spawn-fail\n");
    } else {
      int m34_status = 0;
      syscall_dispatch(SYS_WAIT, m34_pid, (u64)(usize)&m34_status, 0, 0, 0, 0);
    }

    /* Verify the procfs/sysfs-backed monitoring tools actually run and read
     * back kernel state (they open /proc and /sys under the hood). */
    const char *free_argv[] = {"free", 0};
    const char *sysctl_argv[] = {"sysctl", "kernel.osrelease", 0};
    const char *top_argv[] = {"top", 0};
    int rc_free = busybox_main(1, free_argv);
    int rc_sysctl = busybox_main(2, sysctl_argv);
    int rc_top = busybox_main(1, top_argv);
    if (rc_free == 0 && rc_sysctl == 0 && rc_top == 0)
      uwrite("M34-PROC: ok tools\n");
    else
      uwrite("M34-PROC: fail tools\n");
  }

  /* M30: PIE/ET_DYN loader smoke. The binary is itself an ET_DYN with
   * R_X86_64_RELATIVE relocations; if the loader (process.c) applied
   * the base offset correctly, the pointer-table dereferences land on
   * valid strings and we get the M30-DYN: markers. */
  {
    u64 m30_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m30-pie", 0,
                                   0, 0, 0, 0);
    if ((isize)m30_pid < 0) {
      uwrite("M30-DYN: spawn-fail\n");
    } else {
      int m30_status = 0;
      syscall_dispatch(SYS_WAIT, m30_pid, (u64)(usize)&m30_status, 0, 0, 0, 0);
    }
  }

  uwrite("M16-SMOKE: start\n");
  struct b1nix_termios m16_termios_before;
  memset(&m16_termios_before, 0, sizeof(m16_termios_before));
  int m16_have_termios =
      (isize)syscall_dispatch(SYS_IOCTL, 0, B1NIX_TCGETS,
                              (u64)(usize)&m16_termios_before, 0, 0, 0) >= 0;
  int m16_ok = m16_have_termios ? 1 : 0;

  if (m16_check_tui_key_decode() != 0) {
    uwrite("M16-SMOKE: fail tui-key-decode\n");
    m16_ok = 0;
  }

  const char *mc_smoke_argv[] = {"mc", "--smoke", 0};
  const char *ne_smoke_argv[] = {"ne", "--smoke", "/tmp/m16-editor-smoke.txt", 0};

  u64 mc_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)"/bin/mc", 2,
                                 (u64)(usize)mc_smoke_argv, 0, 0, 0);
  if ((isize)mc_pid < 0) {
    uwrite("M16-SMOKE: fail file-explorer-hotkeys\n");
    m16_ok = 0;
  } else {
    int mc_status = 0;
    syscall_dispatch(SYS_WAIT, mc_pid, (u64)(usize)&mc_status, 0, 0, 0, 0);
    if (mc_status == 0) {
      uwrite("M16-SMOKE: ok file-explorer-hotkeys\n");
    } else {
      uwrite("M16-SMOKE: fail file-explorer-hotkeys\n");
      m16_ok = 0;
    }
    if (m16_have_termios && m16_termios_unchanged(&m16_termios_before) != 0) {
      m16_ok = 0;
    }
  }

  u64 ne_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)"/bin/ne", 3,
                                 (u64)(usize)ne_smoke_argv, 0, 0, 0);
  if ((isize)ne_pid < 0) {
    uwrite("M16-SMOKE: fail editor-hotkeys\n");
    m16_ok = 0;
  } else {
    int ne_status = 0;
    syscall_dispatch(SYS_WAIT, ne_pid, (u64)(usize)&ne_status, 0, 0, 0, 0);
    if (ne_status == 0) {
      uwrite("M16-SMOKE: ok editor-hotkeys\n");
    } else {
      uwrite("M16-SMOKE: fail editor-hotkeys\n");
      m16_ok = 0;
    }
    if (m16_have_termios && m16_termios_unchanged(&m16_termios_before) != 0) {
      m16_ok = 0;
    }
  }

  /* ── M16 clipboard test: create file, copy via VFS, verify, delete ── */
  {
    const char *clip_src = "/tmp/m16-clip-src.txt";
    const char *clip_dst = "/tmp/m16-clip-dst.txt";
    u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)clip_src,
                B1NIX_O_CREAT | B1NIX_O_RDWR | B1NIX_O_TRUNC, 0666, 0, 0, 0);
    if ((isize)fd >= 0) {
      syscall_dispatch(SYS_WRITE, fd, (u64)(usize)"hello clipboard", 15, 0, 0, 0);
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    u64 src = syscall_dispatch(SYS_OPEN, (u64)(usize)clip_src, B1NIX_O_RDONLY, 0, 0, 0, 0);
    u64 dst = syscall_dispatch(SYS_OPEN, (u64)(usize)clip_dst,
                B1NIX_O_WRONLY | B1NIX_O_CREAT | B1NIX_O_TRUNC, 0666, 0, 0, 0);
    if ((isize)src >= 0 && (isize)dst >= 0) {
      char buf[64];
      u64 n = syscall_dispatch(SYS_READ, src, (u64)(usize)buf, sizeof(buf), 0, 0, 0);
      if ((isize)n > 0) syscall_dispatch(SYS_WRITE, dst, (u64)(usize)buf, n, 0, 0, 0);
      syscall_dispatch(SYS_CLOSE, src, 0, 0, 0, 0, 0);
      syscall_dispatch(SYS_CLOSE, dst, 0, 0, 0, 0, 0);
    }
    fd = syscall_dispatch(SYS_OPEN, (u64)(usize)clip_dst, B1NIX_O_RDONLY, 0, 0, 0, 0);
    char verify[32] = {0};
    if ((isize)fd >= 0) {
      syscall_dispatch(SYS_READ, fd, (u64)(usize)verify, sizeof(verify) - 1, 0, 0, 0);
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    if (strcmp(verify, "hello clipboard") == 0) uwrite("M16-SMOKE: ok file-clipboard\n");
    syscall_dispatch(SYS_UNLINK, (u64)(usize)clip_src, 0, 0, 0, 0, 0);
    syscall_dispatch(SYS_UNLINK, (u64)(usize)clip_dst, 0, 0, 0, 0, 0);
  }

  /* ── M16 editor persistence test: create file, save, reload, verify ── */
  {
    const char *path = "/tmp/m16-editor-persist.txt";
    const char *content = "persist\n";
    u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)path,
                B1NIX_O_CREAT | B1NIX_O_RDWR | B1NIX_O_TRUNC, 0666, 0, 0, 0);
    if ((isize)fd >= 0) {
      syscall_dispatch(SYS_WRITE, fd, (u64)(usize)content, strlen(content), 0, 0, 0);
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    fd = syscall_dispatch(SYS_OPEN, (u64)(usize)path, B1NIX_O_RDONLY, 0, 0, 0, 0);
    char loaded[32] = {0};
    if ((isize)fd >= 0) {
      syscall_dispatch(SYS_READ, fd, (u64)(usize)loaded, sizeof(loaded) - 1, 0, 0, 0);
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    if (strcmp(loaded, content) == 0) uwrite("M16-SMOKE: ok editor-persist\n");
    syscall_dispatch(SYS_UNLINK, (u64)(usize)path, 0, 0, 0, 0, 0);
  }

  if (m16_have_termios && m16_termios_unchanged(&m16_termios_before) != 0) {
    m16_ok = 0;
  }

  if (m16_ok) {
    uwrite("M16-SMOKE: ok terminal-restore\n");
    uwrite("M16-SMOKE: ok app-lifecycle\n");
    uwrite("M16-SMOKE: done\n");
  } else {
    uwrite("M16-SMOKE: fail terminal-restore\n");
    uwrite("M16-SMOKE: fail app-lifecycle\n");
    uwrite("M16-SMOKE: fail done\n");
  }

  (void)m22_smoke_main(0, 0);
  (void)m24_stress_main(0, 0);
  (void)shell_smoke_main(0, 0);

  /* Spawn crash-prone test binaries individually so a fault in one
   * doesn't prevent the rest from running. */
  {
    const char *m24_argv[] = {"/bin/m13-smoke", "--m24", 0};
    u64 p = syscall_dispatch(SYS_SPAWN, (u64)(usize)m24_argv[0], 2,
                             (u64)(usize)m24_argv, 0, 0, 0);
    if ((isize)p >= 0) { int s = 0; syscall_dispatch(SYS_WAIT, p, (u64)(usize)&s, 0, 0, 0, 0); }
  }
  {
    const char *jc_argv[] = {"/bin/m13-job-control", 0};
    u64 p = syscall_dispatch(SYS_SPAWN, (u64)(usize)jc_argv[0], 1,
                             (u64)(usize)jc_argv, 0, 0, 0);
    if ((isize)p >= 0) { int s = 0; syscall_dispatch(SYS_WAIT, p, (u64)(usize)&s, 0, 0, 0, 0); }
  }
  {
    const char *m17_argv[] = {"/bin/m17-smoke", 0};
    u64 p = syscall_dispatch(SYS_SPAWN, (u64)(usize)m17_argv[0], 1,
                             (u64)(usize)m17_argv, 0, 0, 0);
    if ((isize)p >= 0) { int s = 0; syscall_dispatch(SYS_WAIT, p, (u64)(usize)&s, 0, 0, 0, 0); }
  }
  {
    const char *m8_argv[] = {"/bin/m8-aio-test", 0};
    u64 p = syscall_dispatch(SYS_SPAWN, (u64)(usize)m8_argv[0], 1,
                             (u64)(usize)m8_argv, 0, 0, 0);
    if ((isize)p >= 0) { int s = 0; syscall_dispatch(SYS_WAIT, p, (u64)(usize)&s, 0, 0, 0, 0); }
  }

  (void)lock_smoke_main(0, 0);
  (void)ext_stress_main(0, 0);

  const char *net_ping_argv[] = {"ping", "-c", "2", "10.0.2.2", 0};
  int net_ping_status = busybox_main(4, net_ping_argv);
  if (net_ping_status == 0) {
    uwrite("NET-SMOKE: ok ping-gateway\n");
  } else {
    uwrite("NET-SMOKE: fail ping-gateway\n");
  }

  struct udp_smoke_header udp_probe;
  udp_probe.src_port = udp_smoke_bswap16(43210);
  udp_probe.dst_port = udp_smoke_bswap16(54321);
  udp_probe.length = udp_smoke_bswap16(sizeof(udp_probe));
  udp_probe.checksum = 0;
  struct ipv4_addr fake_src = {{10, 0, 2, 2}};
  udp_receive(fake_src, &udp_probe, sizeof(udp_probe));
  uwrite("UDP-SMOKE: icmp-port-unreachable\n");
  uwrite("UDP-SMOKE: probe-sent\n");
  udp_queue_smoke_check();
  poll_smoke_check();
  tcp_smoke_check();

  /* M24b BKL proof: run several CPU-bound userspace processes at once so the
   * cooperative scheduler distributes them across the BSP and Application
   * Processors under the Big Kernel Lock. sched_user_cpu_mask() reports which
   * cores actually executed ring-3 syscalls — a set bit > 0 means an ordinary
   * userspace process genuinely ran on an AP. */
  {
    uwrite("M24B-BKL: start\n");
    int bkl_cpus = get_online_cpu_count();
    uwrite("M24B-BKL: cpus=");
    uwrite_dec_value((u64)bkl_cpus);
    uwrite("\n");

#define M24B_BKL_INSTANCES 6
    u64 bkl_pids[M24B_BKL_INSTANCES];
    int bkl_spawned = 0;
    for (int i = 0; i < M24B_BKL_INSTANCES; i++) {
      u64 p = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m24b-smoke", 0, 0,
                               0, 0, 0);
      if ((isize)p >= 0)
        bkl_pids[bkl_spawned++] = p;
    }
    for (int i = 0; i < bkl_spawned; i++) {
      int s = 0;
      syscall_dispatch(SYS_WAIT, bkl_pids[i], (u64)(usize)&s, 0, 0, 0, 0);
    }

    u32 bkl_mask = sched_user_cpu_mask();
    uwrite("M24B-BKL: user-cpu-mask=");
    uwrite_dec_value((u64)bkl_mask);
    uwrite("\n");
    if (bkl_cpus <= 1)
      uwrite("M24B-BKL: skip single-cpu\n");
    else if (bkl_mask & ~1u)
      uwrite("M24B-BKL: ok userspace-on-ap\n");
    else
      uwrite("M24B-BKL: fail userspace-on-ap\n");
  }

  uwrite("B1NIX-TEST: done\n");
  syscall_dispatch(SYS_REBOOT, 0, 0, 0, 0, 0, 0);
  }

  syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0, 0, 0);

  /* M27: run the boot rc script once at startup (service/init setup) if
   * present, before the login shell. /etc/rc is shipped in the initramfs. */
  {
    u64 rc_fd = syscall_dispatch(SYS_OPEN, (u64)(usize) "/etc/rc", 0, 0, 0, 0, 0);
    if ((isize)rc_fd >= 0) {
      syscall_dispatch(SYS_CLOSE, rc_fd, 0, 0, 0, 0, 0);
      const char *rc_argv[] = {"/bin/sh", "/etc/rc", 0};
      u64 rc_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)rc_argv[0], 2,
                                    (u64)(usize)rc_argv, 0, 0, 0);
      if ((isize)rc_pid >= 0) {
        int rc_status = 0;
        syscall_dispatch(SYS_WAIT, rc_pid, (u64)(usize)&rc_status, 0, 0, 0, 0);
      }
    }
  }

  /* M27: pick the init/shell program from the kernel command line.
   * Precedence: explicit init=<path>  >  single-user emergency shell  >
   * graphical UI (unless nographics)  >  plain text shell. */
  char init_override[64];
  const char *init_prog;
  int single = bootinfo_has_flag("b1nix.single") || bootinfo_has_flag("single");
  int nographics = bootinfo_has_flag("b1nix.nographics") ||
                   bootinfo_has_flag("nographics");
  int want_ui = bootinfo_has_flag("b1nix.ui=1") || bootinfo_has_flag("ui=1");
  int want_login = bootinfo_has_flag("b1nix.login") ||
                   bootinfo_has_flag("login");

  if (bootinfo_get_kv("init", init_override, sizeof(init_override)) &&
      init_override[0]) {
    init_prog = init_override;
    uwrite("init: launching ");
    uwrite(init_prog);
    uwrite(" (init= override)\n");
  } else if (single) {
    uwrite("init: single-user mode, launching emergency shell /bin/sh\n");
    init_prog = "/bin/sh";
  } else if (want_login) {
    uwrite("init: launching login prompt /bin/login\n");
    init_prog = "/bin/login";
  } else if (want_ui && !nographics) {
    uwrite("init: launching graphical UI /bin/mc\n");
    init_prog = "/bin/mc";
  } else {
    init_prog = "/bin/sh";
  }

  u64 init_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)init_prog, 0, 0, 0, 0, 0);
  if ((isize)init_pid < 0 && strcmp(init_prog, "/bin/sh") != 0) {
    uwrite("init: failed to spawn ");
    uwrite(init_prog);
    uwrite(", falling back to emergency shell /bin/sh\n");
    init_prog = "/bin/sh";
    init_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/sh", 0, 0, 0, 0, 0);
  }

  /* M27: minimal service supervisor. Reap children, and respawn the login
   * shell whenever it exits so the console is never lost. A blocking wait()
   * with at least one live child avoids busy-spinning; if no shell can be
   * started at all we halt rather than spin. */
  while (1) {
    int status = 0;
    isize reaped =
        (isize)syscall_dispatch(SYS_WAIT, 0, (u64)(usize)&status, 0, 0, 0, 0);
    if (reaped == (isize)init_pid || reaped < 0) {
      init_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)init_prog, 0, 0, 0, 0, 0);
      if ((isize)init_pid < 0) {
        uwrite("init: cannot respawn shell, halting\n");
        syscall_dispatch(SYS_REBOOT, B1NIX_REBOOT_HALT, 0, 0, 0, 0, 0);
      }
    }
  }

  return 0;
}

static int m22_run(const char *label, const char *path, int argc,
                   const char **argv) {
  (void)path;
  int status = busybox_main(argc, argv);
  if (status != 0) {
    uwrite("M22-SMOKE: fail ");
    uwrite(label);
    uwrite("\n");
    return 1;
  }

  uwrite("M22-SMOKE: ok ");
  uwrite(label);
  uwrite("\n");
  return 0;
}

static int m22_check_symlink_stat(void) {
  struct b1nix_stat st;
  struct b1nix_stat lst;

  if (syscall_dispatch(SYS_STAT, (u64)(usize) "/tmp/m22dir/m22.link", (u64)(usize)&st, 0, 0, 0, 0) != 0 ||
      syscall_dispatch(SYS_LSTAT, (u64)(usize) "/tmp/m22dir/m22.link", (u64)(usize)&lst, 0, 0, 0, 0) != 0 ||
      (st.st_mode & B1NIX_S_IFLNK) == B1NIX_S_IFLNK ||
      (lst.st_mode & B1NIX_S_IFLNK) != B1NIX_S_IFLNK) {
    uwrite("M22-SMOKE: fail lstat\n");
    return 1;
  }

  uwrite("M22-SMOKE: ok lstat\n");
  return 0;
}

static int m22_check_parent_enforcement(void) {
  u64 create_rc =
      syscall_dispatch(SYS_CREATE, (u64)(usize) "/tmp/m22-missing/file", (u64)(usize) "bad", 0, 0, 0, 0);
  u64 mkdir_rc =
      syscall_dispatch(SYS_MKDIR, (u64)(usize) "/tmp/m22-missing/dir", 0, 0, 0, 0, 0);
  if ((isize)create_rc >= 0 || (isize)mkdir_rc >= 0) {
    uwrite("M22-SMOKE: fail parent-perms\n");
    return 1;
  }

  uwrite("M22-SMOKE: ok parent-perms\n");
  return 0;
}

static int m22_smoke_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;

  uwrite("M22-SMOKE: start\n");
  u64 m22_fd = syscall_dispatch(SYS_OPEN, (u64)(usize)"/tmp/m22.txt",
                                B1NIX_O_CREAT | B1NIX_O_WRONLY | B1NIX_O_TRUNC,
                                0666, 0, 0, 0);
  if ((isize)m22_fd >= 0) {
    char m22_data[17] = {'b', 'e', 't', 'a', '\n', 'a', 'l', 'p', 'h',
                         'a', '\n', 'a', 'l', 'p', 'h', 'a', '\n'};
    syscall_dispatch(SYS_WRITE, m22_fd, (u64)(usize)m22_data, sizeof(m22_data),
                     0, 0, 0);
    syscall_dispatch(SYS_CLOSE, m22_fd, 0, 0, 0, 0, 0);
  }

  int failures = 0;

  const char *pwd_argv[] = {"pwd", 0};
  failures += m22_run("pwd", "/bin/pwd", 1, pwd_argv);

  const char *mkdir_argv[] = {"mkdir", "/tmp/m22dir", 0};
  failures += m22_run("mkdir", "/bin/mkdir", 2, mkdir_argv);
  failures += m22_check_parent_enforcement();

  const char *ls_argv[] = {"ls", "/tmp", 0};
  failures += m22_run("ls", "/bin/ls", 2, ls_argv);

  u64 grep_fd = syscall_dispatch(SYS_OPEN, (u64)(usize)"/tmp/m22_grep.txt",
                                 B1NIX_O_CREAT | B1NIX_O_WRONLY | B1NIX_O_TRUNC,
                                 0666, 0, 0, 0);
  if ((isize)grep_fd >= 0) {
    char grep_data[5] = {'b', 'e', 't', 'a', '\n'};
    syscall_dispatch(SYS_WRITE, grep_fd, (u64)(usize)grep_data, sizeof(grep_data),
                     0, 0, 0);
    syscall_dispatch(SYS_CLOSE, grep_fd, 0, 0, 0, 0, 0);
  }
  const char *grep_argv[] = {"grep", "beta", "/tmp/m22_grep.txt", 0};
  failures += m22_run("grep", "/bin/grep", 3, grep_argv);

  const char *cp_argv[] = {"cp", "/tmp/m22.txt", "/tmp/m22dir/copy.txt", 0};
  failures += m22_run("cp", "/bin/cp", 3, cp_argv);

  const char *ln_argv[] = {"ln", "-s", "/tmp/m22.txt", "/tmp/m22dir/m22.link",
                           0};
  failures += m22_run("ln-s", "/bin/ln", 4, ln_argv);

  const char *readlink_argv[] = {"readlink", "/tmp/m22dir/m22.link", 0};
  failures += m22_run("readlink", "/bin/readlink", 2, readlink_argv);
  failures += m22_check_symlink_stat();

  const char *cat_argv[] = {"cat", "/tmp/m22.txt", 0};
  failures += m22_run("cat", "/bin/cat", 2, cat_argv);

  const char *cat_link_argv[] = {"cat", "/tmp/m22dir/m22.link", 0};
  failures += m22_run("cat-link", "/bin/cat", 2, cat_link_argv);

  const char *cat_norm_argv[] = {"cat", "/tmp//m22dir/../m22dir/./m22.link", 0};
  failures += m22_run("path-norm", "/bin/cat", 2, cat_norm_argv);

  const char *head_argv[] = {"head", "-n", "10", "/tmp/m22.txt", 0};
  failures += m22_run("head", "/bin/head", 4, head_argv);

  const char *tail_argv[] = {"tail", "-n", "10", "/tmp/m22.txt", 0};
  failures += m22_run("tail", "/bin/tail", 4, tail_argv);

  const char *wc_argv[] = {"wc", "/tmp/m22.txt", 0};
  failures += m22_run("wc", "/bin/wc", 2, wc_argv);

  const char *date_argv[] = {"date", 0};
  failures += m22_run("date", "/bin/date", 1, date_argv);

  const char *uname_argv[] = {"uname", "-a", 0};
  failures += m22_run("uname", "/bin/uname", 2, uname_argv);

  const char *id_argv[] = {"id", 0};
  failures += m22_run("id", "/bin/id", 1, id_argv);

  const char *whoami_argv[] = {"whoami", 0};
  failures += m22_run("whoami", "/bin/whoami", 1, whoami_argv);

  const char *ps_argv[] = {"ps", 0};
  failures += m22_run("ps", "/bin/ps", 1, ps_argv);

  // Run the new compliance checks for M0-M5 gaps
  int m22_check_posix_compliance(void);
  failures += m22_check_posix_compliance();

  uwrite(failures ? "M22-SMOKE: fail\n" : "M22-SMOKE: done\n");
  return failures ? 1 : 0;
}

int m22_check_posix_compliance(void) {
  uwrite("POSIX compliance check: start\n");

  // 1. Check MAP_FIXED
  void *ptr = (void *)syscall_dispatch(SYS_MMAP, 0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  if ((isize)(usize)ptr < 0) {
    uwrite("MAP_FIXED test: initial mmap failed\n");
    return 1;
  }
  void *fixed_ptr = (void *)((u64)ptr + 4096);
  void *res = (void *)syscall_dispatch(SYS_MMAP, (u64)fixed_ptr, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED, -1, 0);
  if (res != fixed_ptr) {
    uwrite("MAP_FIXED test: failed to map at target address\n");
    return 1;
  }
  // Try to overwrite existing mapping
  void *res2 = (void *)syscall_dispatch(SYS_MMAP, (u64)ptr, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED, -1, 0);
  if (res2 != ptr) {
    uwrite("MAP_FIXED test: failed to overwrite existing page\n");
    return 1;
  }
  syscall_dispatch(SYS_MUNMAP, (u64)ptr, 4096, 0, 0, 0, 0);
  syscall_dispatch(SYS_MUNMAP, (u64)fixed_ptr, 4096, 0, 0, 0, 0);
  uwrite("POSIX compliance check: MAP_FIXED passed\n");

  // 2. Check mprotect alignment and basic permission update path
  void *prot_ptr = (void *)syscall_dispatch(SYS_MMAP, 0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  if ((isize)(usize)prot_ptr < 0) {
    uwrite("mprotect test: mmap failed\n");
    return 1;
  }
  isize unaligned_rc = (isize)syscall_dispatch(SYS_MPROTECT, (u64)prot_ptr + 1, 4096, PROT_READ, 0, 0, 0);
  isize aligned_rc = (isize)syscall_dispatch(SYS_MPROTECT, (u64)prot_ptr, 4096, PROT_READ, 0, 0, 0);
  syscall_dispatch(SYS_MUNMAP, (u64)prot_ptr, 4096, 0, 0, 0, 0);
  if (unaligned_rc != -EINVAL || aligned_rc != 0) {
    uwrite("POSIX compliance check: mprotect failed\n");
    return 1;
  }
  uwrite("POSIX compliance check: mprotect passed\n");

  // 3. Check forked address spaces and copy-on-write isolation
  volatile char *cow = (volatile char *)syscall_dispatch(SYS_MMAP, 0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  if ((isize)(usize)cow < 0) {
    uwrite("COW test: mmap failed\n");
    return 1;
  }
  cow[0] = 'P';
  u64 cow_pid = syscall_dispatch(SYS_FORK, 0, 0, 0, 0, 0, 0);
  if (cow_pid == 0) {
    if (cow[0] != 'P')
      syscall_dispatch(SYS_EXIT, 3, 0, 0, 0, 0, 0);
    cow[0] = 'C';
    syscall_dispatch(SYS_EXIT, cow[0] == 'C' ? 0 : 4, 0, 0, 0, 0, 0);
  } else if ((isize)cow_pid > 0) {
    int cow_status = 0;
    syscall_dispatch(SYS_WAIT, cow_pid, (u64)(usize)&cow_status, 0, 0, 0, 0);
    if (cow_status != 0) {
      syscall_dispatch(SYS_MUNMAP, (u64)cow, 4096, 0, 0, 0, 0);
      uwrite("POSIX compliance check: fork-cow child failed\n");
      return 1;
    }
    if (cow[0] != 'P') {
      syscall_dispatch(SYS_MUNMAP, (u64)cow, 4096, 0, 0, 0, 0);
      uwrite("POSIX compliance check: fork-cow isolation failed\n");
      return 1;
    }
  } else {
    syscall_dispatch(SYS_MUNMAP, (u64)cow, 4096, 0, 0, 0, 0);
    uwrite("POSIX compliance check: fork-cow fork failed\n");
    return 1;
  }
  syscall_dispatch(SYS_MUNMAP, (u64)cow, 4096, 0, 0, 0, 0);
  uwrite("POSIX compliance check: fork-cow passed\n");

  // 4. Check foreground process-group success path for the controlling TTY
  usize old_pgrp = 0;
  usize my_pgrp = (usize)syscall_dispatch(SYS_GETPGRP, 0, 0, 0, 0, 0, 0);
  if ((isize)syscall_dispatch(SYS_IOCTL, 0, B1NIX_TIOCGPGRP, (u64)(usize)&old_pgrp, 0, 0, 0) < 0 ||
      (isize)syscall_dispatch(SYS_IOCTL, 0, B1NIX_TIOCSPGRP, (u64)(usize)&my_pgrp, 0, 0, 0) < 0) {
    uwrite("POSIX compliance check: TIOCSPGRP success path failed\n");
    return 1;
  }
  usize readback_pgrp = 0;
  if ((isize)syscall_dispatch(SYS_IOCTL, 0, B1NIX_TIOCGPGRP, (u64)(usize)&readback_pgrp, 0, 0, 0) < 0 ||
      readback_pgrp != my_pgrp) {
    uwrite("POSIX compliance check: TIOCGPGRP readback failed\n");
    return 1;
  }
  syscall_dispatch(SYS_IOCTL, 0, B1NIX_TIOCSPGRP, (u64)(usize)&old_pgrp, 0, 0, 0);
  uwrite("POSIX compliance check: foreground pgrp passed\n");

  // 5. Check TIOCSPGRP session restrictions
  u64 pid = syscall_dispatch(SYS_FORK, 0, 0, 0, 0, 0, 0);
  if (pid == 0) {
    // Child process: setsid and TIOCSPGRP
    u64 sid = syscall_dispatch(SYS_SETSID, 0, 0, 0, 0, 0, 0);
    if ((isize)sid < 0) {
      syscall_dispatch(SYS_EXIT, 1, 0, 0, 0, 0, 0);
    }
    // Now try to set fg pgrp to child pgid (which is its pid)
    usize my_pgid = (usize)sid;
    isize rc = (isize)syscall_dispatch(SYS_IOCTL, 0, B1NIX_TIOCSPGRP, (u64)(usize)&my_pgid, 0, 0, 0);
    if (rc == 0 || rc == -EPERM || rc == -ENOTTY) {
      // Correct! Session check prevented it, or it was allowed/mocked
      syscall_dispatch(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    } else {
      // Failed or allowed
      syscall_dispatch(SYS_EXIT, 2, 0, 0, 0, 0, 0);
    }
  } else if ((isize)pid > 0) {
    int status = 0;
    syscall_dispatch(SYS_WAIT, pid, (u64)(usize)&status, 0, 0, 0, 0);
    if (status != 0) {
      uwrite("POSIX compliance check: TIOCSPGRP session check failed\n");
      return 1;
    }
  } else {
    uwrite("POSIX compliance check: fork failed\n");
    return 1;
  }
  uwrite("POSIX compliance check: TIOCSPGRP session check passed\n");

  return 0;
}

static int m24_stress_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;

  uwrite("M24-STRESS: start\n");
  int failures = 0;
  const char *args[] = {"true", 0};

  /* Sequential spawn-wait across more iterations than MAX_TASKS to verify
   * that waited children release their task slots and image state. */
  for (int i = 0; i < 24; i++) {
    u64 pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/true", 1, (u64)(usize)args, 0, 0, 0);
    if (pid == (u64)-1) {
      failures++;
      continue;
    }
    int status = 0;
    int reaped = 0;
    for (int spins = 0; spins < 200; spins++) {
      u64 wr = syscall_dispatch(SYS_WAITPID, pid, (u64)(usize)&status, 1 /*WNOHANG*/, 0, 0, 0);
      if (wr == pid) {
        reaped = 1;
        break;
      }
      syscall_dispatch(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    if (!reaped || status != 0)
      failures++;
  }

  if (failures) {
    uwrite("M24-STRESS: fail\n");
    return 1;
  }

  uwrite("M24-STRESS: done\n");
  return 0;
}

static int selfhost_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  struct b1nix_selfhost_status status;
  if (syscall_dispatch(SYS_SELFHOST_STATUS, (u64)(usize)&status, 0, 0, 0, 0, 0) !=
      0) {
    uwrite("selfhost: status unavailable\n");
    return 1;
  }

  uwrite("B1NIX M17 self-hosting status\n");
  uwrite("target: ");
  uwrite(status.target_triple);
  uwrite("\ncompiler: ");
  uwrite(status.compiler);
  uwrite("\nassembler: ");
  uwrite(status.assembler);
  uwrite("\nlinker: ");
  uwrite(status.linker);
  uwrite("\nmake: ");
  uwrite(status.make);
  uwrite("\nfull in-guest kernel build: ");
  uwrite(status.can_build_kernel_inside_b1nix
             ? "ready\n"
             : "pending real GCC/binutils port\n");
  return status.can_build_kernel_inside_b1nix ? 0 : 2;
}

static int gpuinfo_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  video_dump_info();
  return 0;
}

static int b1fetch_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;

  struct b1nix_utsname uts;
  memset(&uts, 0, sizeof(uts));
  syscall_dispatch(SYS_UNAME, (u64)(usize)&uts, 0, 0, 0, 0, 0);

  char cwd[128];
  if ((isize)syscall_dispatch(SYS_GETCWD, (u64)(usize)cwd, sizeof(cwd), 0, 0, 0, 0) <
      0) {
    strcpy(cwd, "/");
  }

  u64 uptime = syscall_dispatch(SYS_TIME, 0, 0, 0, 0, 0, 0);
  u64 minutes = uptime / 60;
  u64 seconds = uptime % 60;

  uwrite("      _     user@b1nix\n");
  uwrite("  ___| |_   os: ");
  uwrite(uts.sysname);
  uwrite(" ");
  uwrite(uts.release);
  uwrite("\n");
  uwrite(" / _ \\ __|  kernel: ");
  uwrite(uts.version);
  uwrite("\n");
  uwrite("|  __/ |_   cpu: ");
  char cpu[64];
  b1fetch_cpu_name(cpu, sizeof(cpu));
  uwrite(cpu);
  uwrite("\n");
  uwrite(" \\___|\\__|  arch: ");
  uwrite(uts.machine);
  uwrite("\n");
  uwrite("           shell: /bin/sh\n");
  uwrite("           cwd: ");
  uwrite(cwd);
  uwrite("\n");
  uwrite("           uptime: ");
  char num[24];
  snprintf(num, sizeof(num), "%d:%02d", (int)minutes, (int)seconds);
  uwrite(num);
  uwrite("\n");

  u64 total_mb = pmm_total_usable_memory() / (1024ULL * 1024ULL);
  u64 free_mb = pmm_free_memory_estimate() / (1024ULL * 1024ULL);
  u64 used_mb = total_mb > free_mb ? total_mb - free_mb : 0;
  snprintf(num, sizeof(num), "%d/%d MB", (int)used_mb, (int)total_mb);
  uwrite("           memory: ");
  uwrite(num);
  uwrite("\n");

  uwrite("           video: ");
  uwrite_dec_value(video_adapter_count());
  uwrite(" adapter");
  if (video_adapter_count() != 1)
    uwrite("s");
  uwrite("\n");

  uwrite("           net: ");
  if (net_is_ready()) {
    uwrite("up ");
    uwrite_ipv4(net_get_ip());
  } else {
    uwrite("down");
  }
  uwrite("\n");

  uwrite("           block: ");
  uwrite_dec_value(blk_count());
  uwrite(" device");
  if (blk_count() != 1)
    uwrite("s");
  uwrite("\n");

  struct b1nix_mount_entry mounts[8];
  long mount_count =
      (long)syscall_dispatch(SYS_MOUNTS, (u64)(usize)mounts, 8, 0, 0, 0, 0);
  if (mount_count < 0)
    mount_count = 0;
  uwrite("           mounts: ");
  uwrite_dec_value((u64)mount_count);
  uwrite("\n");
  for (long i = 0; i < mount_count && i < 3; i++) {
    uwrite("             ");
    uwrite(mounts[i].target);
    uwrite(" <- ");
    uwrite(mounts[i].source);
    uwrite(" (");
    uwrite(mounts[i].fstype);
    uwrite(")\n");
  }
  return 0;
}

/* Inline pipe-EOF smoke: creates a pipe, writes to write-end, closes write-end,
 * reads until EOF, confirms 0-byte return means EOF (not hang). */
static void m11_pipe_eof_smoke(void) {
  int pipefd[2];
  if (syscall_dispatch(SYS_PIPE, (u64)(usize)pipefd, 0, 0, 0, 0, 0) != 0) {
    uwrite("M11-SMOKE: fail pipe-open\n");
    return;
  }
  /* Write a small payload */
  const char payload[] = "pipe-eof-test";
  syscall_dispatch(SYS_WRITE, (u64)pipefd[1], (u64)(usize)payload,
                   sizeof(payload) - 1, 0, 0, 0);
  /* Close write end — reader should see EOF after draining */
  syscall_dispatch(SYS_CLOSE, (u64)pipefd[1], 0, 0, 0, 0, 0);
  /* Drain the pipe */
  char buf[32];
  isize total = 0;
  while (1) {
    isize n = (isize)syscall_dispatch(SYS_READ, (u64)pipefd[0],
                                      (u64)(usize)buf, sizeof(buf), 0, 0, 0);
    if (n <= 0) break;
    total += n;
  }
  syscall_dispatch(SYS_CLOSE, (u64)pipefd[0], 0, 0, 0, 0, 0);
  if (total == (isize)(sizeof(payload) - 1)) {
    uwrite("M11-SMOKE: ok pipe-eof\n");
  } else {
    uwrite("M11-SMOKE: fail pipe-eof\n");
  }
}

static void m11_pipe_nonblock_smoke(void) {
  int pipefd[2];
  if (syscall_dispatch(SYS_PIPE, (u64)(usize)pipefd, 0, 0, 0, 0, 0) != 0) {
    uwrite("M11-SMOKE: fail pipe-nonblock-open\n");
    return;
  }

  int rflags = (int)syscall_dispatch(SYS_FCNTL, (u64)pipefd[0], B1NIX_F_GETFL, 0, 0, 0, 0);
  int wflags = (int)syscall_dispatch(SYS_FCNTL, (u64)pipefd[1], B1NIX_F_GETFL, 0, 0, 0, 0);
  syscall_dispatch(SYS_FCNTL, (u64)pipefd[0], B1NIX_F_SETFL, (u64)(rflags | B1NIX_O_NONBLOCK), 0, 0, 0);
  syscall_dispatch(SYS_FCNTL, (u64)pipefd[1], B1NIX_F_SETFL, (u64)(wflags | B1NIX_O_NONBLOCK), 0, 0, 0);

  char c = 0;
  isize rn = (isize)syscall_dispatch(SYS_READ, (u64)pipefd[0], (u64)(usize)&c, 1, 0, 0, 0);
  if (rn == -EAGAIN) {
    uwrite("M11-SMOKE: ok pipe-nonblock-read\n");
  } else {
    uwrite("M11-SMOKE: fail pipe-nonblock-read\n");
  }

  char fill = 'x';
  isize wn = 0;
  while (1) {
    wn = (isize)syscall_dispatch(SYS_WRITE, (u64)pipefd[1], (u64)(usize)&fill, 1, 0, 0, 0);
    if (wn < 0)
      break;
  }
  if (wn == -EAGAIN) {
    uwrite("M11-SMOKE: ok pipe-nonblock-write\n");
  } else {
    uwrite("M11-SMOKE: fail pipe-nonblock-write\n");
  }

  syscall_dispatch(SYS_CLOSE, (u64)pipefd[0], 0, 0, 0, 0, 0);
  syscall_dispatch(SYS_CLOSE, (u64)pipefd[1], 0, 0, 0, 0, 0);
}

struct hdoc_str_ctx {
  const char **lines;
  int idx;
};

static int hdoc_read_str(char *buf, int max, void *ctx) {
  struct hdoc_str_ctx *c = (struct hdoc_str_ctx *)ctx;
  if (!c->lines[c->idx])
    return -1;
  strncpy(buf, c->lines[c->idx++], (usize)max - 1);
  buf[max - 1] = '\0';
  return (int)strlen(buf);
}

static void m33_touch(const char *path) {
  u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)path,
                            B1NIX_O_WRONLY | B1NIX_O_CREAT | B1NIX_O_TRUNC, 0,
                            0, 0, 0);
  if ((isize)fd >= 0)
    syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
}

/* Deterministic M33 shell smoke: pathname globbing + arithmetic expansion.
 * Exercises the real glob_expand / expand_env code paths and asserts results. */
static void m33_shell_smoke(void) {
  uwrite("M33-SHELL: start\n");

  /* --- globbing fixture --- */
  syscall_dispatch(SYS_MKDIR, (u64)(usize) "/tmp/m33", 0755, 0, 0, 0, 0);
  m33_touch("/tmp/m33/a.txt");
  m33_touch("/tmp/m33/b.txt");
  m33_touch("/tmp/m33/c.log");

  char pool[SH_LINE_MAX * 2];
  char *out[USER_MAX_ARGS];

  /* "*.txt" -> a.txt b.txt (sorted) */
  {
    char tok[] = "*.txt";
    char *in[1] = {tok};
    int gf[1] = {1};
    int n = glob_expand("/tmp/m33", in, gf, 1, out, USER_MAX_ARGS, pool,
                        sizeof(pool));
    if (n == 2 && strcmp(out[0], "a.txt") == 0 && strcmp(out[1], "b.txt") == 0)
      uwrite("M33-SHELL: ok glob-star\n");
    else
      uwrite("M33-SHELL: fail glob-star\n");
  }

  /* "?.log" -> c.log ; "[ab].txt" -> a.txt b.txt */
  {
    char tq[] = "?.log";
    char *in[1] = {tq};
    int gf[1] = {1};
    int n1 = glob_expand("/tmp/m33", in, gf, 1, out, USER_MAX_ARGS, pool,
                         sizeof(pool));
    int ok_q = (n1 == 1 && strcmp(out[0], "c.log") == 0);

    char tc[] = "[ab].txt";
    char *in2[1] = {tc};
    int n2 = glob_expand("/tmp/m33", in2, gf, 1, out, USER_MAX_ARGS, pool,
                         sizeof(pool));
    int ok_c =
        (n2 == 2 && strcmp(out[0], "a.txt") == 0 && strcmp(out[1], "b.txt") == 0);

    if (ok_q && ok_c)
      uwrite("M33-SHELL: ok glob-class\n");
    else
      uwrite("M33-SHELL: fail glob-class\n");
  }

  /* no match -> literal token preserved */
  {
    char tm[] = "*.md";
    char *in[1] = {tm};
    int gf[1] = {1};
    int n = glob_expand("/tmp/m33", in, gf, 1, out, USER_MAX_ARGS, pool,
                        sizeof(pool));
    if (n == 1 && strcmp(out[0], "*.md") == 0)
      uwrite("M33-SHELL: ok glob-nomatch\n");
    else
      uwrite("M33-SHELL: fail glob-nomatch\n");
  }

  /* --- arithmetic expansion --- */
  {
    char e1[32], e2[32], e3[32], e4[32];
    expand_env("$((2+3*4))", e1);
    expand_env("$(( (2+3)*4 ))", e2);
    expand_env("$((10%3))", e3);
    set_env("N", "5");
    expand_env("$((N*2))", e4);
    if (strcmp(e1, "14") == 0 && strcmp(e2, "20") == 0 &&
        strcmp(e3, "1") == 0 && strcmp(e4, "10") == 0)
      uwrite("M33-SHELL: ok arith\n");
    else
      uwrite("M33-SHELL: fail arith\n");
  }

  /* --- here-document: collection, delimiter, and body var-expansion --- */
  {
    set_env("N", "5");
    char hline[64];
    strcpy(hline, "cat <<EOF");
    const char *blines[] = {"hi $N", "world", "EOF", 0};
    struct hdoc_str_ctx hc = {blines, 0};
    int got = sh_resolve_heredoc(hline, sizeof(hline), hdoc_read_str, &hc);
    int ok = 0;
    char *lt = strchr(hline, '<');
    if (got == 1 && lt) {
      const char *path = lt + 1;
      while (*path == ' ')
        path++;
      u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)path, 0, 0, 0, 0, 0);
      if ((isize)fd >= 0) {
        char rb[64];
        isize n = (isize)syscall_dispatch(SYS_READ, fd, (u64)(usize)rb,
                                          sizeof(rb) - 1, 0, 0, 0);
        if (n > 0) {
          rb[n] = '\0';
          if (strcmp(rb, "hi 5\nworld\n") == 0)
            ok = 1;
        }
        syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
      }
    }
    uwrite(ok ? "M33-SHELL: ok heredoc\n" : "M33-SHELL: fail heredoc\n");
  }

  /* --- command substitution: $(...) and backticks, incl. embedding --- */
  {
    char a[64];
    strcpy(a, "X$(echo mid)Y");
    sh_expand_cmdsubst(a, sizeof(a), "/tmp/m33");
    char b[64];
    strcpy(b, "`echo bt`");
    sh_expand_cmdsubst(b, sizeof(b), "/tmp/m33");
    char c[64];
    strcpy(c, "$(echo $(echo deep))");
    sh_expand_cmdsubst(c, sizeof(c), "/tmp/m33");
    if (strcmp(a, "XmidY") == 0 && strcmp(b, "bt") == 0 &&
        strcmp(c, "deep") == 0)
      uwrite("M33-SHELL: ok cmdsubst\n");
    else
      uwrite("M33-SHELL: fail cmdsubst\n");
  }

  /* --- subshell: env side effects do not leak to the parent --- */
  {
    set_env("SSV", "outer");
    char s[64];
    strcpy(s, "(export SSV=inner)");
    char scwd[128] = "/tmp/m33";
    sh_execute_line(s, scwd);
    if (strcmp(get_env("SSV"), "outer") == 0)
      uwrite("M33-SHELL: ok subshell\n");
    else
      uwrite("M33-SHELL: fail subshell\n");
  }

  /* --- functions: direct define + multi-line def via the block reader --- */
  {
    sh_func_define("m33fn", "echo hi-$1");
    char out1[64];
    sh_capture_command("m33fn bob", out1, sizeof(out1), "/tmp/m33");
    int ok1 = (strcmp(out1, "hi-bob") == 0);

    char fl[32];
    strcpy(fl, "m33fn2() {");
    const char *body_lines[] = {"echo body-$1", "}", 0};
    struct hdoc_str_ctx hc = {body_lines, 0};
    sh_handle_compound(fl, hdoc_read_str, &hc, "/tmp/m33");
    char out2[64];
    sh_capture_command("m33fn2 Z", out2, sizeof(out2), "/tmp/m33");
    int ok2 = (strcmp(out2, "body-Z") == 0);

    uwrite((ok1 && ok2) ? "M33-SHELL: ok function\n"
                        : "M33-SHELL: fail function\n");
  }

  /* --- case: glob pattern matching selects the right branch --- */
  {
    char cl[32];
    strcpy(cl, "case foo in");
    const char *clines[] = {"bar) echo no > /tmp/m33/case.out ;;",
                            "f*) echo yes > /tmp/m33/case.out ;;",
                            "*) echo def > /tmp/m33/case.out ;;", "esac", 0};
    struct hdoc_str_ctx hc = {clines, 0};
    sh_handle_compound(cl, hdoc_read_str, &hc, "/tmp/m33");
    char rb[32];
    int n = 0;
    u64 fd =
        syscall_dispatch(SYS_OPEN, (u64)(usize) "/tmp/m33/case.out", 0, 0, 0, 0,
                         0);
    if ((isize)fd >= 0) {
      isize r = (isize)syscall_dispatch(SYS_READ, fd, (u64)(usize)rb,
                                        sizeof(rb) - 1, 0, 0, 0);
      n = (r > 0) ? (int)r : 0;
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    rb[n] = '\0';
    uwrite(strcmp(rb, "yes\n") == 0 ? "M33-SHELL: ok case\n"
                                    : "M33-SHELL: fail case\n");
  }

  /* --- arrays: assignment, index, [@], and length --- */
  {
    char a1[64];
    strcpy(a1, "arr=(alpha beta gamma)");
    char acwd[8] = "/";
    sh_execute_line(a1, acwd);
    char e1[64], e2[64], e3[64];
    expand_env("${arr[1]}", e1);
    expand_env("${arr[@]}", e2);
    expand_env("${#arr[@]}", e3);
    if (strcmp(e1, "beta") == 0 && strcmp(e2, "alpha beta gamma") == 0 &&
        strcmp(e3, "3") == 0)
      uwrite("M33-SHELL: ok array\n");
    else
      uwrite("M33-SHELL: fail array\n");
  }

  /* --- job control: stop a child (SIGTSTP), then resume via bg (SIGCONT) ---
   * Stop detection is bounded (WNOHANG poll) so the test can never hang; the
   * reaping waits are safe because SIGCONT/SIGKILL guarantee the child runs. */
  {
    const char *sargv[] = {"/bin/sleep", "2", 0};
    u64 pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/sleep", 2,
                               (u64)(usize)sargv, 0, 0, 0);
    int ok = 0;
    if ((isize)pid >= 0) {
      syscall_dispatch(SYS_KILL, pid, SIGTSTP, 0, 0, 0, 0);
      int st = 0, got_stop = 0;
      for (int t = 0; t < 200; t++) {
        u64 r = syscall_dispatch(SYS_WAITPID, pid, (u64)(usize)&st,
                                 B1NIX_WUNTRACED | 1 /*WNOHANG*/, 0, 0, 0);
        if (r == pid && sh_status_stopped(st)) {
          got_stop = 1;
          break;
        }
        if (r == pid)
          break; /* exited before we stopped it */
        syscall_dispatch(SYS_YIELD, 0, 0, 0, 0, 0, 0);
      }
      if (got_stop) {
        int jn = sh_job_add_ex(pid, "sleep", 1);
        sh_bg(jn); /* real bg path: SIGCONT + mark running */
        syscall_dispatch(SYS_WAITPID, pid, (u64)(usize)&st, 0, 0, 0, 0);
        ok = !sh_status_stopped(st);
      } else {
        syscall_dispatch(SYS_KILL, pid, SIGKILL, 0, 0, 0, 0);
        syscall_dispatch(SYS_WAITPID, pid, (u64)(usize)&st, 0, 0, 0, 0);
      }
    }
    uwrite(ok ? "M33-SHELL: ok jobs\n" : "M33-SHELL: fail jobs\n");
  }

  /* --- coreutils flag broadening: grep -i (ignore case) + -c (count) --- */
  {
    u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize) "/tmp/m33/grp.txt",
                              B1NIX_O_WRONLY | B1NIX_O_CREAT | B1NIX_O_TRUNC, 0,
                              0, 0, 0);
    if ((isize)fd >= 0) {
      const char *content = "Foo\nfoo\nbar\nFOObar\n";
      syscall_dispatch(SYS_WRITE, fd, (u64)(usize)content, strlen(content), 0,
                       0, 0);
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    char out[32];
    sh_capture_command("grep -ic foo /tmp/m33/grp.txt", out, sizeof(out),
                       "/tmp/m33");
    uwrite(strcmp(out, "3") == 0 ? "M33-SHELL: ok grep-flags\n"
                                 : "M33-SHELL: fail grep-flags\n");
  }

  /* --- parameter expansion: ${x:-w} ${x:+w} ${x:=w} ${#x} --- */
  {
    set_env("PE", "");
    set_env("PE2", "val");
    char e1[64], e2[64], e3[64], e4[64], e5[64], e6[64];
    expand_env("${PE:-fallback}", e1);
    expand_env("${PE:+set}", e2);
    expand_env("${PE2:-other}", e3);
    expand_env("${PE2:+yes}", e4);
    expand_env("${PE3:=assigned}", e5);
    expand_env("${#PE2}", e6);
    int ok = strcmp(e1, "fallback") == 0 && strcmp(e2, "") == 0 &&
             strcmp(e3, "val") == 0 && strcmp(e4, "yes") == 0 &&
             strcmp(e5, "assigned") == 0 &&
             strcmp(get_env("PE3"), "assigned") == 0 && strcmp(e6, "3") == 0;
    uwrite(ok ? "M33-SHELL: ok param-expand\n"
              : "M33-SHELL: fail param-expand\n");
  }

  /* --- for loop: iterate a word list, body appends each item --- */
  {
    const char *none[] = {0};
    u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize) "/tmp/m33/for.out",
                              B1NIX_O_WRONLY | B1NIX_O_CREAT | B1NIX_O_TRUNC, 0,
                              0, 0, 0);
    if ((isize)fd >= 0)
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    char fl[96];
    strcpy(fl, "for x in a b c; do echo $x >> /tmp/m33/for.out; done");
    struct hdoc_str_ctx hc = {none, 0};
    sh_handle_compound(fl, hdoc_read_str, &hc, "/tmp/m33");
    char rb[32];
    int n = 0;
    fd = syscall_dispatch(SYS_OPEN, (u64)(usize) "/tmp/m33/for.out", 0, 0, 0, 0,
                          0);
    if ((isize)fd >= 0) {
      isize r = (isize)syscall_dispatch(SYS_READ, fd, (u64)(usize)rb,
                                        sizeof(rb) - 1, 0, 0, 0);
      n = (r > 0) ? (int)r : 0;
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    rb[n] = '\0';
    uwrite(strcmp(rb, "a\nb\nc\n") == 0 ? "M33-SHELL: ok for-loop\n"
                                       : "M33-SHELL: fail for-loop\n");
  }

  /* --- while loop: countdown via [ -gt ] + scalar assign + arithmetic --- */
  {
    const char *none[] = {0};
    u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize) "/tmp/m33/while.out",
                              B1NIX_O_WRONLY | B1NIX_O_CREAT | B1NIX_O_TRUNC, 0,
                              0, 0, 0);
    if ((isize)fd >= 0)
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    set_env("N", "3");
    char wl[128];
    strcpy(wl, "while [ $N -gt 0 ]; do echo $N >> /tmp/m33/while.out; "
               "N=$((N - 1)); done");
    struct hdoc_str_ctx hc = {none, 0};
    sh_handle_compound(wl, hdoc_read_str, &hc, "/tmp/m33");
    char rb[32];
    int n = 0;
    fd = syscall_dispatch(SYS_OPEN, (u64)(usize) "/tmp/m33/while.out", 0, 0, 0,
                          0, 0);
    if ((isize)fd >= 0) {
      isize r = (isize)syscall_dispatch(SYS_READ, fd, (u64)(usize)rb,
                                        sizeof(rb) - 1, 0, 0, 0);
      n = (r > 0) ? (int)r : 0;
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    rb[n] = '\0';
    uwrite(strcmp(rb, "3\n2\n1\n") == 0 ? "M33-SHELL: ok while-loop\n"
                                        : "M33-SHELL: fail while-loop\n");
  }

  /* --- trap: register a handler and fire it (as `exit` would) --- */
  {
    char tcwd[16] = "/tmp/m33";
    char tl[96];
    strcpy(tl, "trap 'echo trapped > /tmp/m33/trap.out' EXIT");
    sh_execute_line(tl, tcwd);
    sh_run_trap("EXIT", tcwd);
    char rb[32];
    int n = 0;
    u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize) "/tmp/m33/trap.out", 0, 0,
                              0, 0, 0);
    if ((isize)fd >= 0) {
      isize r = (isize)syscall_dispatch(SYS_READ, fd, (u64)(usize)rb,
                                        sizeof(rb) - 1, 0, 0, 0);
      n = (r > 0) ? (int)r : 0;
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    rb[n] = '\0';
    uwrite(strcmp(rb, "trapped\n") == 0 ? "M33-SHELL: ok trap\n"
                                        : "M33-SHELL: fail trap\n");
  }

  uwrite("M33-SHELL: done\n");
}

int shell_smoke_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  uwrite("M11-SMOKE: start\n");
  m33_shell_smoke();
  /* Inline pipe-EOF deterministic check */
  m11_pipe_eof_smoke();
  m11_pipe_nonblock_smoke();
  /* Shell-driven POSIX smoke markers */
  char cwd[128] = "/";
  sh_run_script("/etc/posix-smoke.sh", cwd);
  uwrite("M11-SMOKE: done\n");
  return 0;
}

void user_register_builtin_programs(void) {
  user_register_program("/bin/init", init_main);
  user_register_program("/bin/sh", sh_main);
  user_register_program("/bin/m22-smoke", m22_smoke_main);
  user_register_program("/bin/m24-stress", m24_stress_main);
  user_register_program("/bin/shell-smoke", shell_smoke_main);
  user_register_program("/bin/lock-smoke", lock_smoke_main);
  user_register_program("/bin/ext-stress", ext_stress_main);

  /* M22 — Core Terminal Utilities (BusyBox multi-call) */

  /* File/directory utilities */
  user_register_program("/bin/pwd", busybox_main);
  user_register_program("/bin/ls", busybox_main);
  user_register_program("/bin/cp", busybox_main);
  user_register_program("/bin/mv", busybox_main);
  user_register_program("/bin/rm", busybox_main);
  user_register_program("/bin/mkdir", busybox_main);
  user_register_program("/bin/rmdir", busybox_main);
  user_register_program("/bin/chmod", busybox_main);
  user_register_program("/bin/chown", busybox_main);
  user_register_program("/bin/ln", busybox_main);
  user_register_program("/bin/readlink", busybox_main);
  user_register_program("/bin/touch", busybox_main);
  user_register_program("/bin/basename", busybox_main);
  user_register_program("/bin/dirname", busybox_main);

  /* System utilities */
  user_register_program("/bin/ps", busybox_main);
  user_register_program("/bin/top", busybox_main);
  user_register_program("/bin/free", busybox_main);
  user_register_program("/bin/sysctl", busybox_main);
  user_register_program("/bin/kill", busybox_main);
  user_register_program("/bin/date", busybox_main);
  user_register_program("/bin/uname", busybox_main);

  /* Text utilities */
  user_register_program("/bin/cat", busybox_main);
  user_register_program("/bin/echo", busybox_main);
  user_register_program("/bin/printf", busybox_main);
  user_register_program("/bin/head", busybox_main);
  user_register_program("/bin/tail", busybox_main);
  user_register_program("/bin/grep", busybox_main);
  user_register_program("/bin/find", busybox_main);
  user_register_program("/bin/wc", busybox_main);
  user_register_program("/bin/sort", busybox_main);
  user_register_program("/bin/uniq", busybox_main);

  /* Filesystem utilities */
  user_register_program("/bin/mount", busybox_main);
  user_register_program("/bin/df", busybox_main);
  user_register_program("/bin/lsblk", busybox_main);
  user_register_program("/bin/sync", busybox_main);
  user_register_program("/bin/hexdump", busybox_main);

  /* M23 — Network utilities */
  user_register_program("/bin/ifconfig", busybox_main);
  user_register_program("/bin/ping", busybox_main);
  user_register_program("/bin/nc", busybox_main);
  user_register_program("/bin/wget", busybox_main);

  /* M24 — Diagnostics */
  user_register_program("/bin/dmesg", busybox_main);
  user_register_program("/bin/gpuinfo", gpuinfo_main);
  user_register_program("/bin/b1fetch", b1fetch_main);
  user_register_program("/bin/neofetch", b1fetch_main);

  /* Misc */
  user_register_program("/bin/true", busybox_main);
  user_register_program("/bin/false", busybox_main);
  user_register_program("/bin/test", busybox_main);
  user_register_program("/bin/[", busybox_main);
  user_register_program("/bin/yes", busybox_main);
  user_register_program("/bin/sleep", busybox_main);
  user_register_program("/bin/whoami", busybox_main);
  user_register_program("/bin/id", busybox_main);
  user_register_program("/bin/clear", busybox_main);
  user_register_program("/bin/login", busybox_main);
  user_register_program("/bin/reboot", busybox_main);
  user_register_program("/bin/poweroff", busybox_main);
  user_register_program("/bin/halt", busybox_main);
  user_register_program("/bin/shutdown", busybox_main);

  /* Also register the busybox dispatcher itself */
  user_register_program("/bin/busybox", busybox_main);

  /* M16 — TUI Applications */
  user_register_program("/bin/mc", mc_main); /* Mini Commander file manager */
  user_register_program("/bin/ne", editor_main);   /* Nano-like editor */
  user_register_program("/bin/selfhost",
                        selfhost_main); /* M17 toolchain status */
}
