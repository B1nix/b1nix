#include <string.h>
#include <stdio.h>
#include <b1nix/blk.h>
#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/posix.h>
#include <b1nix/syscall.h>
#include <b1nix/user.h>
#include <b1nix/video.h>

void user_register_program(const char *path, user_program_entry entry);

static void uwrite(const char *text) {
  syscall_dispatch(SYS_WRITE, 1, (u64)(usize)text, strlen(text), 0);
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

#define SH_HISTORY_MAX 16
static char sh_history[SH_HISTORY_MAX][256];
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
    u64 r = syscall_dispatch(SYS_WAITPID, sh_jobs[i].pid, (u64)(usize)&st,
                             1 /*WNOHANG*/, 0);
    if (r == sh_jobs[i].pid)
      sh_jobs[i].done = 1;
    if (sh_jobs[i].done)
      continue;
    char num[4] = {'[', '0' + (i + 1), ']', ' '};
    syscall_dispatch(SYS_WRITE, 0, (u64)(usize)num, 4, 0);
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
  syscall_dispatch(SYS_WAIT, sh_jobs[idx].pid, (u64)(usize)&st, 0, 0);
  sh_jobs[idx].done = 1;
  return st;
}

static int readline(char *buffer, usize max_len) {
  struct b1nix_termios old_t, new_t;
  syscall_dispatch(SYS_IOCTL, 0, B1NIX_TCGETS, (u64)(usize)&old_t, 0);
  new_t = old_t;
  new_t.c_lflag &= ~(B1NIX_ICANON | B1NIX_ECHO);
  syscall_dispatch(SYS_IOCTL, 0, B1NIX_TCSETS, (u64)(usize)&new_t, 0);

  usize len = 0;
  int hist_idx = sh_hist_count;

  while (1) {
    char c = (char)syscall_dispatch(SYS_READ_KBD, 0, 0, 0, 0);
    if (c == 4)
      return -1; /* Ctrl-D */
    if (c == 27) {
      char b1 = (char)syscall_dispatch(SYS_READ_KBD, 0, 0, 0, 0);
      if (b1 == '[') {
        char b2 = (char)syscall_dispatch(SYS_READ_KBD, 0, 0, 0, 0);
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

  syscall_dispatch(SYS_IOCTL, 0, B1NIX_TCSETS, (u64)(usize)&old_t, 0);
  return 0;
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
      while (in[i] && in[i] != ' ' && in[i] != '$' && in[i] != '/' && k < 31) {
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
    if (strcmp(args[i], "<") == 0 && i + 1 < argc) {
      redir->stdin_path = args[++i];
      continue;
    }
    if (strcmp(args[i], ">") == 0 && i + 1 < argc) {
      redir->stdout_path = args[++i];
      redir->stdout_append = 0;
      continue;
    }
    if (strcmp(args[i], ">>") == 0 && i + 1 < argc) {
      redir->stdout_path = args[++i];
      redir->stdout_append = 1;
      continue;
    }
    if (strcmp(args[i], "2>") == 0 && i + 1 < argc) {
      redir->stderr_path = args[++i];
      continue;
    }
    if (strcmp(args[i], "2>&1") == 0) {
      redir->stderr_to_stdout = 1;
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
  u64 fd = syscall_dispatch(SYS_CREATE, (u64)(usize)abs, (u64)(usize) "", 0, 0);
  if (fd == 0) {
    fd = syscall_dispatch(SYS_OPEN, (u64)(usize)abs, 0, 0, 0);
  } else {
    fd = syscall_dispatch(SYS_OPEN, (u64)(usize)abs, 0, 0, 0);
  }
  if (fd != (u64)-1 && append) {
    syscall_dispatch(SYS_LSEEK, fd, (u64)0, B1NIX_SEEK_END, 0);
  }
  return fd;
}

static int apply_redirs(const char *cwd, const struct shell_redir *redir,
                        int *opened, int max_opened) {
  int opened_count = 0;
  if (redir->stdin_path) {
    char abs[128];
    resolve_path(cwd, redir->stdin_path, abs);
    u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)abs, 0, 0, 0);
    if (fd == (u64)-1)
      return -1;
    syscall_dispatch(SYS_DUP2, fd, 0, 0, 0);
    if (opened_count < max_opened)
      opened[opened_count++] = (int)fd;
  }
  if (redir->stdout_path) {
    u64 fd = open_output(cwd, redir->stdout_path, redir->stdout_append);
    if (fd == (u64)-1)
      return -1;
    syscall_dispatch(SYS_DUP2, fd, 1, 0, 0);
    if (opened_count < max_opened)
      opened[opened_count++] = (int)fd;
  }
  if (redir->stderr_to_stdout) {
    syscall_dispatch(SYS_DUP2, 1, 2, 0, 0);
  } else if (redir->stderr_path) {
    u64 fd = open_output(cwd, redir->stderr_path, 0);
    if (fd == (u64)-1)
      return -1;
    syscall_dispatch(SYS_DUP2, fd, 2, 0, 0);
    if (opened_count < max_opened)
      opened[opened_count++] = (int)fd;
  }
  return opened_count;
}

static void save_stdio(int saved[3]) {
  saved[0] = 61;
  saved[1] = 62;
  saved[2] = 63;
  syscall_dispatch(SYS_DUP2, 0, saved[0], 0, 0);
  syscall_dispatch(SYS_DUP2, 1, saved[1], 0, 0);
  syscall_dispatch(SYS_DUP2, 2, saved[2], 0, 0);
}

static void restore_stdio(const int saved[3]) {
  syscall_dispatch(SYS_DUP2, saved[0], 0, 0, 0);
  syscall_dispatch(SYS_DUP2, saved[1], 1, 0, 0);
  syscall_dispatch(SYS_DUP2, saved[2], 2, 0, 0);
  syscall_dispatch(SYS_CLOSE, saved[0], 0, 0, 0);
  syscall_dispatch(SYS_CLOSE, saved[1], 0, 0, 0);
  syscall_dispatch(SYS_CLOSE, saved[2], 0, 0, 0);
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
    if (syscall_dispatch(SYS_STAT, (u64)(usize)out, (u64)(usize)&st, 0, 0) ==
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
  u64 pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)path, num_args,
                             (u64)(usize)args, 0);
  if (pid == (u64)-1 && strcmp(path, args[0]) != 0) {
    pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)args[0], num_args,
                           (u64)(usize)args, 0);
  }
  if (pid == (u64)-1 && args[0][0] != '/' && args[0][0] != '.') {
    char bin_path[128];
    usize len = strlen(args[0]);
    if (len < 120) {
      memcpy(bin_path, "/bin/", 5);
      memcpy(bin_path + 5, args[0], len + 1);
      pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)bin_path, num_args,
                             (u64)(usize)args, 0);
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
    syscall_dispatch(SYS_WAIT, pid, (u64)(usize)&status, 0, 0);
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
    int code = (num_args > 1) ? (int)(args[1][0] - '0') : sh_last_status;
    syscall_dispatch(SYS_EXIT, (u64)code, 0, 0, 0);
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
    if (syscall_dispatch(SYS_CHDIR, (u64)(usize)abs_path, 0, 0, 0) == 0) {
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
    char abs_path[128];
    const char *target = (num_args > 1 && args[1][0] != '-') ? args[1] : cwd;
    resolve_path(cwd, target, abs_path);
    syscall_dispatch(SYS_LIST, (u64)(usize)abs_path, 0, 0, 0);
    return 0;
  }
  if (strcmp(args[0], "cat") == 0 && num_args > 1 && args[1][0] != '-') {
    char abs_path[128];
    resolve_path(cwd, args[1], abs_path);
    char buffer[256];
    u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)abs_path, 0, 0, 0);
    if (fd == (u64)-1) {
      uwrite("sh: cat: file not found\n");
      return 1;
    } else {
      while (1) {
        u64 n = syscall_dispatch(SYS_READ, fd, (u64)(usize)buffer,
                                 sizeof(buffer) - 1, 0);
        if (n == 0 || n == (u64)-1)
          break;
        buffer[n] = '\0';
        uwrite(buffer);
      }
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0);
      return 0;
    }
  }
  if (strcmp(args[0], "clear") == 0) {
    syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0);
    return 0;
  }
  if (strcmp(args[0], "ps") == 0) {
    syscall_dispatch(SYS_PS, 0, 0, 0, 0);
    return 0;
  }
  if (strcmp(args[0], "mem") == 0) {
    syscall_dispatch(SYS_MEM, 0, 0, 0, 0);
    return 0;
  }
  if (strcmp(args[0], "reboot") == 0) {
    syscall_dispatch(SYS_REBOOT, 0, 0, 0, 0);
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
      syscall_dispatch(SYS_KILL, pid, (u64)sig, 0, 0);
    }
    return 0;
  }
  if (strcmp(args[0], "ip") == 0) {
    syscall_dispatch(SYS_NET_INFO, 0, 0, 0, 0);
    return 0;
  }
  if (strcmp(args[0], "selfhost") == 0) {
    struct b1nix_selfhost_status status;
    if (syscall_dispatch(SYS_SELFHOST_STATUS, (u64)(usize)&status, 0, 0, 0) ==
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
    char *args[16];
    int num_args = parse_cmd(p, args, 16);
    struct shell_redir redir;
    num_args = parse_redirs(args, num_args, &redir);

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
      syscall_dispatch(SYS_CLOSE, opened[i], 0, 0, 0);
    restore_stdio(saved);
    return status;
  }

  *pipe_pos = '\0';
  char *cmd1 = p;
  char *cmd2 = pipe_pos + 1;

  int pipefd[2];
  if (syscall_dispatch(SYS_PIPE, (u64)(usize)pipefd, 0, 0, 0) != 0)
    return 1;

  int saved[3];
  save_stdio(saved);

  /* First stage of pipe */
  syscall_dispatch(SYS_DUP2, (u64)pipefd[1], 1, 0, 0);
  syscall_dispatch(SYS_CLOSE, (u64)pipefd[1], 0, 0, 0);
  sh_execute_pipeline(cmd1, cwd);

  /* Second stage */
  restore_stdio(saved);
  save_stdio(saved);
  syscall_dispatch(SYS_DUP2, (u64)pipefd[0], 0, 0, 0);
  syscall_dispatch(SYS_CLOSE, (u64)pipefd[0], 0, 0, 0);
  int status = sh_execute_pipeline(cmd2, cwd);

  restore_stdio(saved);
  return status;
}

static void sh_execute_line(char *line, char *cwd) {
  char *comment = strchr(line, '#');
  if (comment)
    *comment = '\0';

  char *p = line;
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

    sh_last_status = sh_execute_pipeline(p, cwd);

    if (op == 0)
      break;
    if (op == 1 && sh_last_status != 0)
      break;
    if (op == 2 && sh_last_status == 0)
      break;

    p = end + (op == 3 ? 1 : 2);
  }
}

static void sh_run_script(const char *path, char *cwd) {
  u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)path, 0, 0, 0);
  if (fd == (u64)-1) {
    uwrite("sh: cannot open script\n");
    return;
  }
  char line[256];
  int i = 0;
  while (1) {
    char c;
    u64 n = syscall_dispatch(SYS_READ, fd, (u64)(usize)&c, 1, 0);
    if (n == 0 || n == (u64)-1)
      break;
    if (c == '\n') {
      line[i] = '\0';
      if (line[0] && strncmp(line, "#!", 2) != 0)
        sh_execute_line(line, cwd);
      i = 0;
    } else if (i < 255)
      line[i++] = c;
  }
  if (i > 0) {
    line[i] = '\0';
    sh_execute_line(line, cwd);
  }
  syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0);
}

static int sh_main(int argc, const char **argv) {
  char cwd[128] = "/";
  if (argc > 1) {
    sh_run_script(argv[1], cwd);
    return 0;
  }

  uwrite("Welcome to b1nix shell!\nType 'help' for a list of commands.\n\n");
  char raw_line[256];
  char line[512];
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

static int init_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;

  syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0);
  u64 native_pid =
      syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/native-smoke", 0, 0, 0);
  if (native_pid != (u64)-1) {
    int native_status = 0;
    syscall_dispatch(SYS_WAIT, native_pid, (u64)(usize)&native_status, 0, 0);
    if (native_status == 0) {
      uwrite("NATIVE-SMOKE: done\n");
    } else {
      uwrite("NATIVE-SMOKE: fail\n");
    }
  } else {
    uwrite("NATIVE-SMOKE: spawn-fail\n");
  }

  u64 smoke_pid =
      syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m22-smoke", 0, 0, 0);
  if (smoke_pid != (u64)-1) {
    int smoke_status = 0;
    syscall_dispatch(SYS_WAIT, smoke_pid, (u64)(usize)&smoke_status, 0, 0);
  }

  u64 stress_pid =
      syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m24-stress", 0, 0, 0);
  if (stress_pid != (u64)-1) {
    int stress_status = 0;
    syscall_dispatch(SYS_WAIT, stress_pid, (u64)(usize)&stress_status, 0, 0);
  }

  u64 shell_smoke_pid =
      syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/shell-smoke", 0, 0, 0);
  if (shell_smoke_pid != (u64)-1) {
    int status = 0;
    syscall_dispatch(SYS_WAIT, shell_smoke_pid, (u64)(usize)&status, 0, 0);
  }

  syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0);
  syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/sh", 0, 0, 0);

  while (1) {
    int status;
    syscall_dispatch(SYS_WAIT, 0, (u64)(usize)&status, 0, 0);
  }

  return 0;
}

extern int busybox_main(int argc, const char **argv);
extern int mc_main(int argc, const char **argv);
extern int editor_main(int argc, const char **argv);
extern int nmake_main(int argc, const char **argv);

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

  if (syscall_dispatch(SYS_STAT, (u64)(usize) "/tmp/m22dir/m22.link",
                       (u64)(usize)&st, 0, 0) != 0 ||
      syscall_dispatch(SYS_LSTAT, (u64)(usize) "/tmp/m22dir/m22.link",
                       (u64)(usize)&lst, 0, 0) != 0 ||
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
      syscall_dispatch(SYS_CREATE, (u64)(usize) "/tmp/m22-missing/file",
                       (u64)(usize) "bad", 0, 0);
  u64 mkdir_rc =
      syscall_dispatch(SYS_MKDIR, (u64)(usize) "/tmp/m22-missing/dir", 0, 0, 0);
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
  syscall_dispatch(SYS_CREATE, (u64)(usize) "/tmp/m22.txt",
                   (u64)(usize) "beta\nalpha\nalpha\n", 0, 0);

  int failures = 0;

  const char *pwd_argv[] = {"pwd", 0};
  failures += m22_run("pwd", "/bin/pwd", 1, pwd_argv);

  const char *mkdir_argv[] = {"mkdir", "/tmp/m22dir", 0};
  failures += m22_run("mkdir", "/bin/mkdir", 2, mkdir_argv);
  failures += m22_check_parent_enforcement();

  const char *ls_argv[] = {"ls", "/tmp", 0};
  failures += m22_run("ls", "/bin/ls", 2, ls_argv);

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

  const char *grep_argv[] = {"grep", "alpha", "/tmp/m22.txt", 0};
  failures += m22_run("grep", "/bin/grep", 3, grep_argv);

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

  uwrite(failures ? "M22-SMOKE: fail\n" : "M22-SMOKE: done\n");
  return failures ? 1 : 0;
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
    u64 pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/true", 1,
                               (u64)(usize)args, 0);
    if (pid == (u64)-1) {
      failures++;
      continue;
    }
    int status = 0;
    syscall_dispatch(SYS_WAIT, pid, (u64)(usize)&status, 0, 0);
    syscall_dispatch(SYS_YIELD, 0, 0, 0, 0);
    if (status != 0)
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
  if (syscall_dispatch(SYS_SELFHOST_STATUS, (u64)(usize)&status, 0, 0, 0) !=
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
  syscall_dispatch(SYS_UNAME, (u64)(usize)&uts, 0, 0, 0);

  char cwd[128];
  if ((isize)syscall_dispatch(SYS_GETCWD, (u64)(usize)cwd, sizeof(cwd), 0, 0) <
      0) {
    strcpy(cwd, "/");
  }

  u64 uptime = syscall_dispatch(SYS_TIME, 0, 0, 0, 0);
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
      (long)syscall_dispatch(SYS_MOUNTS, (u64)(usize)mounts, 8, 0, 0);
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

int shell_smoke_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  char cwd[128] = "/";
  sh_run_script("/etc/posix-smoke.sh", cwd);
  return 0;
}

void user_register_builtin_programs(void) {
  user_register_program("/bin/init", init_main);
  user_register_program("/bin/sh", sh_main);
  user_register_program("/bin/m22-smoke", m22_smoke_main);
  user_register_program("/bin/m24-stress", m24_stress_main);
  user_register_program("/bin/shell-smoke", shell_smoke_main);

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

  /* Also register the busybox dispatcher itself */
  user_register_program("/bin/busybox", busybox_main);

  /* M16 — TUI Applications */
  user_register_program("/bin/mc", mc_main); /* Mini Commander file manager */
  user_register_program("/bin/ne", editor_main);   /* Nano-like editor */
  user_register_program("/bin/nmake", nmake_main); /* Minimal make utility */
  user_register_program("/bin/selfhost",
                        selfhost_main); /* M17 toolchain status */
}
