#include <string.h>
#include <b1nix/posix.h>
#include <b1nix/syscall.h>
#include <b1nix/user.h>

void user_register_program(const char *path, user_program_entry entry);

static void uwrite(const char *text)
{
	syscall_dispatch(SYS_WRITE, (u64)(usize)text, strlen(text), 0, 0);
}

static void readline(char *buffer, usize max_len)
{
	usize len = 0;
	while (1) {
		char c = (char)syscall_dispatch(SYS_READ_KBD, 0, 0, 0, 0);
		if (c == '\n') {
			// Trim trailing spaces
			while (len > 0 && buffer[len - 1] == ' ') {
				len--;
			}
			buffer[len] = '\0';
			break;
		} else if (c == '\b') {
			if (len > 0) {
				len--;
			}
		} else if (c >= ' ' && c <= '~' && len < max_len - 1) {
			buffer[len++] = c;
		}
	}
}

static void resolve_path(const char *cwd, const char *rel, char *abs)
{
	if (rel[0] == '/') {
		// Already absolute
		usize i = 0;
		while (rel[i]) { abs[i] = rel[i]; i++; }
		abs[i] = '\0';
		return;
	}

	usize cwd_len = strlen(cwd);
	memcpy(abs, cwd, cwd_len);
	if (abs[cwd_len - 1] != '/') {
		abs[cwd_len++] = '/';
	}
	
	usize rel_len = strlen(rel);
	memcpy(abs + cwd_len, rel, rel_len + 1);
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
	resolve_path(cwd, name, out);
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

		int is_bg = 0;
		usize len = strlen(cmd);
		if (len > 0 && cmd[len - 1] == '&') {
			is_bg = 1;
			cmd[len - 1] = '\0';
			len--;
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
			uwrite("  clear      - Clear screen\n");
			uwrite("  ping <ip>  - Send ICMP echo\n");
			uwrite("  resolve    - Resolve DNS\n");
			uwrite("  ps         - List tasks\n");
			uwrite("  mem        - Show memory\n");
			uwrite("  reboot     - Reboot system\n");
			uwrite("  export     - Set env var\n");
			uwrite("  jobs       - List background jobs\n");
			uwrite("  ip         - Show network info\n");
			uwrite("  selfhost   - Show M17 toolchain status\n");
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
		} else if (strcmp(args[0], "reboot") == 0 && !pipe_pos) {
			syscall_dispatch(SYS_REBOOT, 0, 0, 0, 0);
		} else if (strcmp(args[0], "jobs") == 0 && !pipe_pos) {
			uwrite("Background jobs tracking not fully implemented, use 'ps'.\n");
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

void user_register_builtin_programs(void)
{
	user_register_program("/bin/init", init_main);
	user_register_program("/bin/sh", sh_main);

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
	user_register_program("/bin/sync", busybox_main);
	user_register_program("/bin/hexdump", busybox_main);

	/* M23 — Network utilities */
	user_register_program("/bin/ifconfig", busybox_main);
	user_register_program("/bin/ping", busybox_main);
	user_register_program("/bin/nc", busybox_main);
	user_register_program("/bin/wget", busybox_main);

	/* M24 — Diagnostics */
	user_register_program("/bin/dmesg", busybox_main);

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
