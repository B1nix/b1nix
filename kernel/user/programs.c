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

static void uwrite(const char *text)
{
	syscall_dispatch(SYS_WRITE, (u64)(usize)text, strlen(text), 0, 0);
}

static void uwrite_dec_value(u64 value)
{
	char buf[24];
	snprintf(buf, sizeof(buf), "%d", (int)value);
	uwrite(buf);
}

static void uwrite_ipv4(struct ipv4_addr ip)
{
	char buf[24];
	snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
	         ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3]);
	uwrite(buf);
}

static void b1fetch_cpu_name(char *out, usize out_size)
{
	if (!out || out_size == 0) return;
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
		while (out[0] == ' ') memmove(out, out + 1, strlen(out));
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

static int sh_job_add(u64 pid, const char *name)
{
	for (int i = 0; i < SH_JOBS_MAX; i++) {
		if (!sh_jobs[i].pid || sh_jobs[i].done) {
			sh_jobs[i].pid  = pid;
			sh_jobs[i].done = 0;
			usize nl = strlen(name);
			if (nl >= 64) nl = 63;
			memcpy(sh_jobs[i].name, name, nl + 1);
			if (i >= sh_job_count) sh_job_count = i + 1;
			return i + 1; /* job number (1-based) */
		}
	}
	return -1;
}

static void sh_jobs_print(void)
{
	int any = 0;
	for (int i = 0; i < sh_job_count; i++) {
		if (!sh_jobs[i].pid || sh_jobs[i].done) continue;
		/* poll: non-blocking waitpid */
		int st = 0;
		u64 r = syscall_dispatch(SYS_WAITPID, sh_jobs[i].pid,
		                         (u64)(usize)&st, 1 /*WNOHANG*/, 0);
		if (r == sh_jobs[i].pid) sh_jobs[i].done = 1;
		if (sh_jobs[i].done) continue;
		char num[4] = {'[', '0' + (i + 1), ']', ' '};
		syscall_dispatch(SYS_WRITE, (u64)(usize)num, 4, 0, 0);
		uwrite(sh_jobs[i].name);
		uwrite("\n");
		any = 1;
	}
	if (!any) uwrite("no background jobs\n");
}

static int sh_fg(int job_num)
{
	int idx = job_num - 1;
	if (idx < 0 || idx >= sh_job_count || !sh_jobs[idx].pid || sh_jobs[idx].done) {
		uwrite("fg: no such job\n");
		return -1;
	}
	int st = 0;
	syscall_dispatch(SYS_WAIT, sh_jobs[idx].pid, (u64)(usize)&st, 0, 0);
	sh_jobs[idx].done = 1;
	return st;
}

static void readline(char *buffer, usize max_len)
{
	struct b1nix_termios old_t, new_t;
	syscall_dispatch(SYS_IOCTL, 0, B1NIX_TCGETS, (u64)(usize)&old_t, 0);
	new_t = old_t;
	new_t.c_lflag &= ~(B1NIX_ICANON | B1NIX_ECHO);
	syscall_dispatch(SYS_IOCTL, 0, B1NIX_TCSETS, (u64)(usize)&new_t, 0);

	usize len = 0;
	int hist_idx = sh_hist_count;

	while (1) {
		char c = (char)syscall_dispatch(SYS_READ_KBD, 0, 0, 0, 0);
		if (c == 27) {
			char b1 = (char)syscall_dispatch(SYS_READ_KBD, 0, 0, 0, 0);
			if (b1 == '[') {
				char b2 = (char)syscall_dispatch(SYS_READ_KBD, 0, 0, 0, 0);
				if (b2 == 'A') { // Up
					if (hist_idx > 0) {
						hist_idx--;
						for (usize i = 0; i < len; i++) uwrite("\b \b");
						len = strlen(sh_history[hist_idx]);
						if (len >= max_len) len = max_len - 1;
						memcpy(buffer, sh_history[hist_idx], len);
						buffer[len] = '\0';
						uwrite(buffer);
					}
				} else if (b2 == 'B') { // Down
					if (hist_idx < sh_hist_count) {
						for (usize i = 0; i < len; i++) uwrite("\b \b");
						hist_idx++;
						if (hist_idx < sh_hist_count) {
							len = strlen(sh_history[hist_idx]);
							if (len >= max_len) len = max_len - 1;
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
}

static void resolve_path(const char *cwd, const char *rel, char *abs)
{
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
		while (combined[i] == '/') i++;
		if (!combined[i]) break;

		char part[64];
		usize part_len = 0;
		while (combined[i] && combined[i] != '/' && part_len < sizeof(part) - 1) {
			part[part_len++] = combined[i++];
		}
		part[part_len] = '\0';
		while (combined[i] && combined[i] != '/') i++;

		if (part[0] == '\0' || strcmp(part, ".") == 0) continue;
		if (strcmp(part, "..") == 0) {
			if (len > 1) {
				if (abs[len - 1] == '/') len--;
				while (len > 1 && abs[len - 1] != '/') len--;
				abs[len] = '\0';
			}
			continue;
		}

		if (len > 1) abs[len++] = '/';
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
			if (len > 63) len = 63;
			memcpy(env_vals[i], val, len);
			env_vals[i][len] = '\0';
			return;
		}
	}
	if (env_count < MAX_ENV_VARS) {
		usize klen = strlen(key);
		if (klen > 31) klen = 31;
		memcpy(env_keys[env_count], key, klen);
		env_keys[env_count][klen] = '\0';
		
		usize vlen = strlen(val);
		if (vlen > 63) vlen = 63;
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

static void expand_env(const char *in, char *out) {
	int i = 0, j = 0;
	while (in[i]) {
		if (in[i] == '$') {
			i++;
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

static int parse_cmd(char *cmd, char **args, int max_args) {
	int argc = 0;
	int in_word = 0;
	for (int i = 0; cmd[i]; i++) {
		if (cmd[i] == ' ') {
			cmd[i] = '\0';
			in_word = 0;
		} else if (!in_word) {
			if (argc < max_args) {
				args[argc++] = &cmd[i];
			}
			in_word = 1;
		}
	}
	return argc;
}

struct shell_redir {
	const char *stdin_path;
	const char *stdout_path;
	const char *stderr_path;
	int stdout_append;
	int stderr_to_stdout;
};

static int parse_redirs(char **args, int argc, struct shell_redir *redir)
{
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

static u64 open_output(const char *cwd, const char *path, int append)
{
	char abs[128];
	resolve_path(cwd, path, abs);
	u64 fd = syscall_dispatch(SYS_CREATE, (u64)(usize)abs, (u64)(usize)"", 0, 0);
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

static int apply_redirs(const char *cwd, const struct shell_redir *redir, int *opened, int max_opened)
{
	int opened_count = 0;
	if (redir->stdin_path) {
		char abs[128];
		resolve_path(cwd, redir->stdin_path, abs);
		u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)abs, 0, 0, 0);
		if (fd == (u64)-1) return -1;
		syscall_dispatch(SYS_DUP2, fd, 0, 0, 0);
		if (opened_count < max_opened) opened[opened_count++] = (int)fd;
	}
	if (redir->stdout_path) {
		u64 fd = open_output(cwd, redir->stdout_path, redir->stdout_append);
		if (fd == (u64)-1) return -1;
		syscall_dispatch(SYS_DUP2, fd, 1, 0, 0);
		if (opened_count < max_opened) opened[opened_count++] = (int)fd;
	}
	if (redir->stderr_to_stdout) {
		syscall_dispatch(SYS_DUP2, 1, 2, 0, 0);
	} else if (redir->stderr_path) {
		u64 fd = open_output(cwd, redir->stderr_path, 0);
		if (fd == (u64)-1) return -1;
		syscall_dispatch(SYS_DUP2, fd, 2, 0, 0);
		if (opened_count < max_opened) opened[opened_count++] = (int)fd;
	}
	return opened_count;
}

static void save_stdio(int saved[3])
{
	saved[0] = 61;
	saved[1] = 62;
	saved[2] = 63;
	syscall_dispatch(SYS_DUP2, 0, saved[0], 0, 0);
	syscall_dispatch(SYS_DUP2, 1, saved[1], 0, 0);
	syscall_dispatch(SYS_DUP2, 2, saved[2], 0, 0);
}

static void restore_stdio(const int saved[3])
{
	syscall_dispatch(SYS_DUP2, saved[0], 0, 0, 0);
	syscall_dispatch(SYS_DUP2, saved[1], 1, 0, 0);
	syscall_dispatch(SYS_DUP2, saved[2], 2, 0, 0);
	syscall_dispatch(SYS_CLOSE, saved[0], 0, 0, 0);
	syscall_dispatch(SYS_CLOSE, saved[1], 0, 0, 0);
	syscall_dispatch(SYS_CLOSE, saved[2], 0, 0, 0);
}

static int lookup_path(const char *cwd, const char *name, char *out)
{
	if (name[0] == '/' || name[0] == '.') {
		resolve_path(cwd, name, out);
		return 0;
	}

	const char *path = get_env("PATH");
	if (!path || path[0] == '\0') path = "/bin";
	usize i = 0;
	while (path[i]) {
		char dir[128];
		usize d = 0;
		while (path[i] && path[i] != ':' && d < sizeof(dir) - 1) {
			dir[d++] = path[i++];
		}
		dir[d] = '\0';
		if (path[i] == ':') i++;
		resolve_path(dir, name, out);
		struct b1nix_stat st;
		if (syscall_dispatch(SYS_STAT, (u64)(usize)out, (u64)(usize)&st, 0, 0) == 0) {
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

static u64 spawn_path(const char *cwd, char **args, int num_args)
{
	char path[128];
	lookup_path(cwd, args[0], path);
	u64 pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)path, num_args, (u64)(usize)args, 0);
	if (pid == (u64)-1 && strcmp(path, args[0]) != 0) {
		pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)args[0], num_args, (u64)(usize)args, 0);
	}
	if (pid == (u64)-1 && args[0][0] != '/' && args[0][0] != '.') {
		char bin_path[128];
		usize len = strlen(args[0]);
		if (len < 120) {
			memcpy(bin_path, "/bin/", 5);
			memcpy(bin_path + 5, args[0], len + 1);
			pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)bin_path, num_args, (u64)(usize)args, 0);
		}
	}
	return pid;
}

static int run_external_command(const char *cwd, char **args, int num_args, int wait_for)
{
	u64 pid = spawn_path(cwd, args, num_args);
	if (pid == (u64)-1) return -1;
	if (wait_for) {
		int status;
		syscall_dispatch(SYS_WAIT, pid, (u64)(usize)&status, 0, 0);
		return status;
	}
	/* Background job — register in jobs list */
	sh_job_add(pid, args[0]);
	return 0;
}

static int sh_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	uwrite("Welcome to b1nix shell!\nType 'help' for a list of commands.\n\n");

	char cwd[128] = "/";
	char raw_line[256];
	char line[512];
	char abs_path[128];

	set_env("PATH", "/bin");

	while (1) {
		uwrite(cwd);
		uwrite("> ");
		readline(raw_line, sizeof(raw_line));
		
		expand_env(raw_line, line);

		char *cmd = line;
		while (*cmd == ' ') cmd++;
		if (cmd[0] == '\0') continue;

		usize raw_len = strlen(raw_line);
		if (raw_len > 0) {
			if (sh_hist_count < SH_HISTORY_MAX) {
				memcpy(sh_history[sh_hist_count++], raw_line, raw_len + 1);
			} else {
				for (int i = 1; i < SH_HISTORY_MAX; i++) {
					memcpy(sh_history[i - 1], sh_history[i], 256);
				}
				memcpy(sh_history[SH_HISTORY_MAX - 1], raw_line, raw_len + 1);
			}
		}

		int is_bg = 0;
		usize cmd_len = strlen(cmd);
		if (cmd_len > 0 && cmd[cmd_len - 1] == '&') {
			is_bg = 1;
			cmd[cmd_len - 1] = '\0';
			cmd_len--;
		}
		
		char *cmd1 = cmd;
		char *cmd2 = 0;
		char *pipe_pos = strchr(cmd, '|');
		if (pipe_pos) {
			*pipe_pos = '\0';
			cmd2 = pipe_pos + 1;
			while (*cmd2 == ' ') cmd2++;
		}

		char *args[16];
		int num_args = parse_cmd(cmd1, args, 16);
		if (num_args == 0) continue;
		struct shell_redir redir1;
		num_args = parse_redirs(args, num_args, &redir1);
		if (num_args == 0) continue;

		int pipefd[2] = {-1, -1};
		if (pipe_pos && syscall_dispatch(SYS_PIPE, (u64)(usize)pipefd, 0, 0, 0) != 0) {
			uwrite("sh: pipe failed\n");
			continue;
		}

		int saved_stdio[3];
		save_stdio(saved_stdio);
		int opened[4];
		u64 first_pid = (u64)-1;
		int opened_count = apply_redirs(cwd, &redir1, opened, 4);
		if (opened_count < 0) {
			uwrite("sh: redirection failed\n");
			restore_stdio(saved_stdio);
			continue;
		}
		if (pipe_pos) {
			syscall_dispatch(SYS_DUP2, (u64)pipefd[1], 1, 0, 0);
		}

		if (strcmp(args[0], "help") == 0 && !pipe_pos) {
			uwrite("Built-in commands:\n");
			uwrite("  help       - Show this message\n");
			uwrite("  ls [dir]   - List files\n");
			uwrite("  cd <dir>   - Change directory\n");
			uwrite("  cat <file> - Print file content\n");
			uwrite("  echo <txt> - Print text\n");
			uwrite("  pwd        - Print working directory\n");
			uwrite("  cp/mv/rm   - File operations\n");
			uwrite("  mkdir/rmdir- Directory operations\n");
			uwrite("  chmod/chown- Permission operations\n");
			uwrite("  clear      - Clear screen\n");
			uwrite("  ps         - List all tasks\n");
			uwrite("  jobs       - List background jobs\n");
			uwrite("  fg [n]     - Bring job n to foreground\n");
			uwrite("  kill [-s] <pid> - Send signal to task\n");
			uwrite("  mem/df/lsblk - System and block info\n");
			uwrite("  gpuinfo    - Video device info\n");
			uwrite("  b1fetch    - Tiny system summary\n");
			uwrite("  ifconfig   - Network info\n");
			uwrite("  ping <ip>  - Send ICMP echo\n");
			uwrite("  resolve    - Resolve DNS\n");
			uwrite("  reboot     - Reboot system\n");
			uwrite("  export     - Set env var (NAME=val)\n");
			uwrite("  selfhost   - Show M17 toolchain status\n");
			uwrite("  mc         - Mini Commander TUI\n");
			uwrite("  ne         - Nano-like editor\n");
		} else if (strcmp(args[0], "ip") == 0 && !pipe_pos) {
			syscall_dispatch(SYS_NET_INFO, 0, 0, 0, 0);
		} else if (strcmp(args[0], "selfhost") == 0 && !pipe_pos) {
			struct b1nix_selfhost_status status;
			if (syscall_dispatch(SYS_SELFHOST_STATUS, (u64)(usize)&status, 0, 0, 0) == 0) {
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
				uwrite("\nkernel self-build: ");
				uwrite(status.can_build_kernel_inside_b1nix ? "ready\n" : "toolchain manifest ready, full GCC port pending\n");
			}
		} else if (strcmp(args[0], "ls") == 0 && !pipe_pos) {
			const char *target = cwd;
			if (num_args > 1) target = args[1];
			resolve_path(cwd, target, abs_path);
			syscall_dispatch(SYS_LIST, (u64)(usize)abs_path, 0, 0, 0);
		} else if (strcmp(args[0], "clear") == 0 && !pipe_pos) {
			syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0);
		} else if (strcmp(args[0], "ping") == 0 && num_args > 1 && !pipe_pos) {
			syscall_dispatch(SYS_NET_PING, (u64)(usize)args[1], 0, 0, 0);
		} else if (strcmp(args[0], "resolve") == 0 && num_args > 1 && !pipe_pos) {
			syscall_dispatch(SYS_NET_DNS, (u64)(usize)args[1], 0, 0, 0);
		} else if (strcmp(args[0], "ps") == 0 && !pipe_pos) {
			syscall_dispatch(SYS_PS, 0, 0, 0, 0);
		} else if (strcmp(args[0], "mem") == 0 && !pipe_pos) {
			syscall_dispatch(SYS_MEM, 0, 0, 0, 0);
		} else if (strcmp(args[0], "gpuinfo") == 0 && !pipe_pos) {
			video_dump_info();
		} else if (strcmp(args[0], "reboot") == 0 && !pipe_pos) {
			syscall_dispatch(SYS_REBOOT, 0, 0, 0, 0);
		} else if (strcmp(args[0], "kill") == 0 && !pipe_pos) {
			int sig = 15; /* SIGTERM */
			int pid_idx = 1;
			if (num_args > 2 && args[1][0] == '-') {
				sig = (int)(args[1][1] - '0'); // Simple 1-digit sig for now
				if (args[1][1] >= '1' && args[1][1] <= '9' && args[1][2] >= '0' && args[1][2] <= '9') {
					sig = (args[1][1] - '0') * 10 + (args[1][2] - '0');
				}
				pid_idx = 2;
			}
			if (num_args > pid_idx) {
				u64 pid = (u64)(args[pid_idx][0] - '0');
				for (int i = 1; args[pid_idx][i]; i++) pid = pid * 10 + (args[pid_idx][i] - '0');
				syscall_dispatch(SYS_KILL, pid, (u64)sig, 0, 0);
			} else {
				uwrite("usage: kill [-sig] <pid>\n");
			}
		} else if (strcmp(args[0], "jobs") == 0 && !pipe_pos) {
			sh_jobs_print();
		} else if (strcmp(args[0], "fg") == 0 && !pipe_pos) {
			int jn = (num_args > 1) ? (int)(args[1][0] - '0') : sh_job_count;
			sh_fg(jn);
		} else if (strcmp(args[0], "export") == 0 && num_args > 1 && !pipe_pos) {
			char *eq = 0;
			for (int i = 0; args[1][i]; i++) {
				if (args[1][i] == '=') { eq = &args[1][i]; break; }
			}
			if (eq) {
				*eq = '\0';
				set_env(args[1], eq + 1);
			}
		} else if (strcmp(args[0], "echo") == 0 && !pipe_pos) {
			for (int i = 1; i < num_args; i++) {
				uwrite(args[i]);
				if (i < num_args - 1) uwrite(" ");
			}
			uwrite("\n");
		} else if (strcmp(args[0], "cd") == 0 && num_args > 1 && !pipe_pos) {
			resolve_path(cwd, args[1], abs_path);
			if (syscall_dispatch(SYS_CHDIR, (u64)(usize)abs_path, 0, 0, 0) == 0) {
				usize len = strlen(abs_path);
				memcpy(cwd, abs_path, len + 1);
			} else {
				uwrite("cd: not a directory: ");
				uwrite(abs_path);
				uwrite("\n");
			}
		} else if (strcmp(args[0], "cat") == 0 && num_args > 1 && !pipe_pos) {
			resolve_path(cwd, args[1], abs_path);
			char buffer[256];
			u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)abs_path, 0, 0, 0);
			if (fd == (u64)-1) {
				uwrite("cat: file not found: ");
				uwrite(abs_path);
				uwrite("\n");
			} else {
				while (1) {
					u64 count = syscall_dispatch(SYS_READ, fd, (u64)(usize)buffer, sizeof(buffer) - 1, 0);
					if (count == 0 || count == (u64)-1) break;
					buffer[count] = '\0';
					uwrite(buffer);
				}
				syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0);
			}
		} else {
			if (pipe_pos) {
				first_pid = spawn_path(cwd, args, num_args);
				if (first_pid == (u64)-1) {
					uwrite("sh: command not found: ");
					uwrite(args[0]);
					uwrite("\n");
				}
			} else if (run_external_command(cwd, args, num_args, !is_bg) < 0) {
				uwrite("sh: command not found: ");
				uwrite(args[0]);
				uwrite("\n");
			}
		}

		for (int i = 0; i < opened_count; i++) {
			syscall_dispatch(SYS_CLOSE, opened[i], 0, 0, 0);
		}

		if (pipe_pos && cmd2) {
			syscall_dispatch(SYS_CLOSE, (u64)pipefd[1], 0, 0, 0);
			restore_stdio(saved_stdio);
			save_stdio(saved_stdio);
			syscall_dispatch(SYS_DUP2, (u64)pipefd[0], 0, 0, 0);

			char *args2[16];
			int num_args2 = parse_cmd(cmd2, args2, 16);
			struct shell_redir redir2;
			num_args2 = parse_redirs(args2, num_args2, &redir2);
			opened_count = apply_redirs(cwd, &redir2, opened, 4);
			if (num_args2 > 0) {
				if (opened_count < 0 || run_external_command(cwd, args2, num_args2, !is_bg) < 0) {
					uwrite("sh: command not found: ");
					uwrite(args2[0]);
					uwrite("\n");
				}
			}
			for (int i = 0; i < opened_count; i++) {
				syscall_dispatch(SYS_CLOSE, opened[i], 0, 0, 0);
			}
			syscall_dispatch(SYS_CLOSE, (u64)pipefd[0], 0, 0, 0);
			if (!is_bg && first_pid != (u64)-1) {
				int status;
				syscall_dispatch(SYS_WAIT, first_pid, (u64)(usize)&status, 0, 0);
			}
		}

		restore_stdio(saved_stdio);
	}

	return 0;
}

static int init_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0);
	u64 smoke_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)"/bin/m22-smoke", 0, 0, 0);
	if (smoke_pid != (u64)-1) {
		int smoke_status = 0;
		syscall_dispatch(SYS_WAIT, smoke_pid, (u64)(usize)&smoke_status, 0, 0);
	}

	u64 stress_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)"/bin/m24-stress", 0, 0, 0);
	if (stress_pid != (u64)-1) {
		int stress_status = 0;
		syscall_dispatch(SYS_WAIT, stress_pid, (u64)(usize)&stress_status, 0, 0);
	}

	syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0);
	syscall_dispatch(SYS_SPAWN, (u64)(usize)"/bin/sh", 0, 0, 0);
	
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

static int m22_run(const char *label, const char *path, int argc, const char **argv)
{
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

static int m22_check_symlink_stat(void)
{
	struct b1nix_stat st;
	struct b1nix_stat lst;

	if (syscall_dispatch(SYS_STAT, (u64)(usize)"/tmp/m22dir/m22.link",
	                     (u64)(usize)&st, 0, 0) != 0 ||
	    syscall_dispatch(SYS_LSTAT, (u64)(usize)"/tmp/m22dir/m22.link",
	                     (u64)(usize)&lst, 0, 0) != 0 ||
	    (st.st_mode & B1NIX_S_IFLNK) == B1NIX_S_IFLNK ||
	    (lst.st_mode & B1NIX_S_IFLNK) != B1NIX_S_IFLNK) {
		uwrite("M22-SMOKE: fail lstat\n");
		return 1;
	}

	uwrite("M22-SMOKE: ok lstat\n");
	return 0;
}

static int m22_check_parent_enforcement(void)
{
	u64 create_rc = syscall_dispatch(SYS_CREATE, (u64)(usize)"/tmp/m22-missing/file",
	                                 (u64)(usize)"bad", 0, 0);
	u64 mkdir_rc = syscall_dispatch(SYS_MKDIR, (u64)(usize)"/tmp/m22-missing/dir",
	                                0, 0, 0);
	if ((isize)create_rc >= 0 || (isize)mkdir_rc >= 0) {
		uwrite("M22-SMOKE: fail parent-perms\n");
		return 1;
	}

	uwrite("M22-SMOKE: ok parent-perms\n");
	return 0;
}

static int m22_smoke_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	uwrite("M22-SMOKE: start\n");
	syscall_dispatch(SYS_CREATE, (u64)(usize)"/tmp/m22.txt",
	                 (u64)(usize)"beta\nalpha\nalpha\n", 0, 0);

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

	const char *ln_argv[] = {"ln", "-s", "/tmp/m22.txt", "/tmp/m22dir/m22.link", 0};
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

	const char *head_argv[] = {"head", "/tmp/m22.txt", 0};
	failures += m22_run("head", "/bin/head", 2, head_argv);

	const char *tail_argv[] = {"tail", "/tmp/m22.txt", 0};
	failures += m22_run("tail", "/bin/tail", 2, tail_argv);

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

static int m24_stress_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	uwrite("M24-STRESS: start\n");
	int failures = 0;
	const char *args[] = {"true", 0};

	/* Sequential spawn-wait across more iterations than MAX_TASKS to verify
	 * that waited children release their task slots and image state. */
	for (int i = 0; i < 24; i++) {
		u64 pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)"/bin/true", 1, (u64)(usize)args, 0);
		if (pid == (u64)-1) {
			failures++;
			continue;
		}
		int status = 0;
		syscall_dispatch(SYS_WAIT, pid, (u64)(usize)&status, 0, 0);
		syscall_dispatch(SYS_YIELD, 0, 0, 0, 0);
		if (status != 0) failures++;
	}

	if (failures) {
		uwrite("M24-STRESS: fail\n");
		return 1;
	}

	uwrite("M24-STRESS: done\n");
	return 0;
}

static int selfhost_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	struct b1nix_selfhost_status status;
	if (syscall_dispatch(SYS_SELFHOST_STATUS, (u64)(usize)&status, 0, 0, 0) != 0) {
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
	uwrite(status.can_build_kernel_inside_b1nix ? "ready\n" : "pending real GCC/binutils port\n");
	return status.can_build_kernel_inside_b1nix ? 0 : 2;
}

static int gpuinfo_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	video_dump_info();
	return 0;
}

static int b1fetch_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	struct b1nix_utsname uts;
	memset(&uts, 0, sizeof(uts));
	syscall_dispatch(SYS_UNAME, (u64)(usize)&uts, 0, 0, 0);

	char cwd[128];
	if ((isize)syscall_dispatch(SYS_GETCWD, (u64)(usize)cwd, sizeof(cwd), 0, 0) < 0) {
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
	if (video_adapter_count() != 1) uwrite("s");
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
	if (blk_count() != 1) uwrite("s");
	uwrite("\n");

	struct b1nix_mount_entry mounts[8];
	long mount_count = (long)syscall_dispatch(SYS_MOUNTS, (u64)(usize)mounts, 8, 0, 0);
	if (mount_count < 0) mount_count = 0;
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

void user_register_builtin_programs(void)
{
	user_register_program("/bin/init", init_main);
	user_register_program("/bin/sh", sh_main);
	user_register_program("/bin/m22-smoke", m22_smoke_main);
	user_register_program("/bin/m24-stress", m24_stress_main);

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

	/* System utilities */
	user_register_program("/bin/ps", busybox_main);
	user_register_program("/bin/kill", busybox_main);
	user_register_program("/bin/date", busybox_main);
	user_register_program("/bin/uname", busybox_main);

	/* Text utilities */
	user_register_program("/bin/cat", busybox_main);
	user_register_program("/bin/echo", busybox_main);
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
	user_register_program("/bin/yes", busybox_main);
	user_register_program("/bin/sleep", busybox_main);
	user_register_program("/bin/whoami", busybox_main);
	user_register_program("/bin/id", busybox_main);
	user_register_program("/bin/clear", busybox_main);

	/* Also register the busybox dispatcher itself */
	user_register_program("/bin/busybox", busybox_main);

	/* M16 — TUI Applications */
	user_register_program("/bin/mc", mc_main);       /* Mini Commander file manager */
	user_register_program("/bin/ne", editor_main);   /* Nano-like editor */
	user_register_program("/bin/nmake", nmake_main); /* Minimal make utility */
	user_register_program("/bin/selfhost", selfhost_main); /* M17 toolchain status */
}
