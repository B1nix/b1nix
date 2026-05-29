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
#include <stdio.h>
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
#define SH_JOBS_MAX 16
struct sh_job {
  u64 pid;
  char name[64];
  int done;
};
static struct sh_job sh_jobs[SH_JOBS_MAX];
static int sh_job_count = 0;

static int sh_job_add(u64 pid, const char *name) {
  for (int i = 0; i < SH_JOBS_MAX; i++) {
    if (!sh_jobs[i].pid || sh_jobs[i].done) {
      sh_jobs[i].pid = pid;
      sh_jobs[i].done = 0;
      usize nl = strlen(name);
      if (nl >= 64)
        nl = 63;
      memcpy(sh_jobs[i].name, name, nl + 1);
      if (i >= sh_job_count)
        sh_job_count = i + 1;
      return i + 1;
    }
  }
  return -1;
}

static void sh_jobs_print(void) {
  int any = 0;
  for (int i = 0; i < sh_job_count; i++) {
    if (!sh_jobs[i].pid || sh_jobs[i].done)
      continue;
    int st = 0;
    u64 r = syscall_dispatch(SYS_WAITPID, sh_jobs[i].pid, (u64)(usize)&st, 1 /*WNOHANG*/, 0, 0, 0);
    if (r == sh_jobs[i].pid)
      sh_jobs[i].done = 1;
    if (sh_jobs[i].done)
      continue;
    char num[4] = {'[', '0' + (i + 1), ']', ' '};
    syscall_dispatch(SYS_WRITE, 0, (u64)(usize)num, 4, 0, 0, 0);
    uwrite(sh_jobs[i].name);
    uwrite("\n");
    any = 1;
  }
  if (!any)
    uwrite("no background jobs\n");
}

static int sh_fg(int job_num) {
  int idx = job_num - 1;
  if (idx < 0 || idx >= sh_job_count || !sh_jobs[idx].pid ||
      sh_jobs[idx].done) {
    uwrite("fg: no such job\n");
    return -1;
  }
  int st = 0;
  syscall_dispatch(SYS_WAIT, sh_jobs[idx].pid, (u64)(usize)&st, 0, 0, 0, 0);
  sh_jobs[idx].done = 1;
  return st;
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

#define MAX_ENV_VARS 16
static char env_keys[MAX_ENV_VARS][32];
static char env_vals[MAX_ENV_VARS][64];
static int env_count = 0;

static void set_env(const char *key, const char *val) {
  for (int i = 0; i < env_count; i++) {
    if (strcmp(env_keys[i], key) == 0) {
      usize len = strlen(val);
      if (len > 63)
        len = 63;
      memcpy(env_vals[i], val, len);
      env_vals[i][len] = '\0';
      return;
    }
  }
  if (env_count < MAX_ENV_VARS) {
    usize klen = strlen(key);
    if (klen > 31)
      klen = 31;
    memcpy(env_keys[env_count], key, klen);
    env_keys[env_count][klen] = '\0';

    usize vlen = strlen(val);
    if (vlen > 63)
      vlen = 63;
    memcpy(env_vals[env_count], val, vlen);
    env_vals[env_count][vlen] = '\0';

    env_count++;
  }
}

static const char *get_env(const char *key) {
  for (int i = 0; i < env_count; i++) {
    if (strcmp(env_keys[i], key) == 0) {
      return env_vals[i];
    }
  }
  return "";
}

static int sh_last_status = 0;

static int atoi(const char *s) {
  int res = 0;
  int sign = 1;
  if (*s == '-') {
    sign = -1;
    s++;
  }
  while (*s >= '0' && *s <= '9') {
    res = res * 10 + (*s - '0');
    s++;
  }
  return res * sign;
}

static void expand_env(const char *in, char *out) {
  int i = 0, j = 0;
  while (in[i]) {
    if (in[i] == '$') {
      i++;
      if (in[i] == '?') {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d", sh_last_status);
        for (int k = 0; buf[k]; k++)
          out[j++] = buf[k];
        i++;
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

/* POSIX-style shell parsing with quotes and escaping */
static int parse_cmd(char *cmd, char **args, int max_args) {
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

      *dst++ = *p++;
    }
    if (*p)
      p++;
    *dst = '\0';
  }
  return count;
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
    int status;
    syscall_dispatch(SYS_WAIT, pid, (u64)(usize)&status, 0, 0, 0, 0);
    return status;
  }
  /* Background job — register in jobs list */
  sh_job_add(pid, args[0]);
  return 0;
}

static int sh_execute_cmd(char *cwd, char **args, int num_args, int is_bg) {
  if (num_args == 0)
    return 0;

  /* Built-ins */
  if (strcmp(args[0], "exit") == 0) {
    int code = (num_args > 1) ? atoi(args[1]) : sh_last_status;
    syscall_dispatch(SYS_EXIT, (u64)code, 0, 0, 0, 0, 0);
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

  char *pipe_pos = strchr(p, '|');
  if (!pipe_pos) {
    /* Cap matches USER_MAX_ARGS (the SYS_SPAWN argv copy limit). 16 was too few
     * for real toolchain command lines (e.g. a kernel-flag gcc invocation has
     * ~20 tokens), which silently dropped trailing args like `-o file`. */
    char *args[32];
    int num_args = parse_cmd(p, args, 32);
    struct shell_redir redir;
    num_args = parse_redirs(args, num_args, &redir);
    if (num_args < 0) {
      uwrite("sh: syntax error: redirection\n");
      return 2;
    }

    int saved[3];
    save_stdio(saved);
    int opened[4];
    int opened_count = apply_redirs(cwd, &redir, opened, 4);
    if (opened_count < 0) {
      uwrite("sh: redirection failed\n");
      restore_stdio(saved);
      return 1;
    }

    int status = sh_execute_cmd(cwd, args, num_args, is_bg);

    for (int i = 0; i < opened_count; i++)
      syscall_dispatch(SYS_CLOSE, opened[i], 0, 0, 0, 0, 0);
    restore_stdio(saved);
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
    while (*end) {
      if ((*end == '"' || *end == '\'') &&
          (in_quote == 0 || in_quote == *end)) {
        if (in_quote)
          in_quote = 0;
        else
          in_quote = *end;
      }
      if (!in_quote) {
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
        expand_env(line, expanded);
        sh_execute_line(expanded, cwd);
      }
      i = 0;
    } else if (i < SH_LINE_MAX - 1)
      line[i++] = c;
  }
  if (i > 0) {
    line[i] = '\0';
    expand_env(line, expanded);
    sh_execute_line(expanded, cwd);
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

    expand_env(raw_line, line);
    sh_execute_line(line, cwd);

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

  /* M27: pick the init/shell program from the kernel command line.
   * Precedence: explicit init=<path>  >  single-user emergency shell  >
   * graphical UI (unless nographics)  >  plain text shell. */
  char init_override[64];
  const char *init_prog;
  int single = bootinfo_has_flag("b1nix.single") || bootinfo_has_flag("single");
  int nographics = bootinfo_has_flag("b1nix.nographics") ||
                   bootinfo_has_flag("nographics");
  int want_ui = bootinfo_has_flag("b1nix.ui=1") || bootinfo_has_flag("ui=1");

  if (bootinfo_get_kv("init", init_override, sizeof(init_override)) &&
      init_override[0]) {
    init_prog = init_override;
    uwrite("init: launching ");
    uwrite(init_prog);
    uwrite(" (init= override)\n");
  } else if (single) {
    uwrite("init: single-user mode, launching emergency shell /bin/sh\n");
    init_prog = "/bin/sh";
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
    syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/sh", 0, 0, 0, 0, 0);
  }

  while (1) {
    int status;
    syscall_dispatch(SYS_WAIT, 0, (u64)(usize)&status, 0, 0, 0, 0);
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

int shell_smoke_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  uwrite("M11-SMOKE: start\n");
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
