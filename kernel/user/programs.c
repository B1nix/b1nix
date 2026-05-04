#include <string.h>
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
			uwrite("\n");
			// Trim trailing spaces
			while (len > 0 && buffer[len - 1] == ' ') {
				len--;
			}
			buffer[len] = '\0';
			break;
		} else if (c == '\b') {
			if (len > 0) {
				len--;
				uwrite("\b \b");
			}
		} else if (c >= ' ' && c <= '~' && len < max_len - 1) {
			buffer[len++] = c;
			char str[2] = {c, '\0'};
			uwrite(str);
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
		
		char *pipe_pos = 0;
		for (usize i = 0; i < len; i++) {
			if (cmd[i] == '|') {
				pipe_pos = &cmd[i];
				break;
			}
		}
		
		char *redir_pos = 0;
		if (!pipe_pos) {
			for (usize i = 0; i < len; i++) {
				if (cmd[i] == '>') {
					redir_pos = &cmd[i];
					break;
				}
			}
		}

		char *cmd1 = cmd;
		char *cmd2 = 0;
		char *redir_file = 0;

		if (pipe_pos) {
			*pipe_pos = '\0';
			cmd2 = pipe_pos + 1;
			while (*cmd2 == ' ') cmd2++;
		} else if (redir_pos) {
			*redir_pos = '\0';
			redir_file = redir_pos + 1;
			while (*redir_file == ' ') redir_file++;
		}

		// First command logic
		char *args[16];
		int num_args = parse_cmd(cmd1, args, 16);
		if (num_args == 0) continue;

		u64 redir_fd = (u64)-1;
		if (redir_file) {
			resolve_path(cwd, redir_file, abs_path);
			redir_fd = syscall_dispatch(SYS_CREATE, (u64)(usize)abs_path, (u64)(usize)"", 0, 0);
			if (redir_fd == (u64)-1) {
			    // Try opening if create fails or already exists
			    redir_fd = syscall_dispatch(SYS_OPEN, (u64)(usize)abs_path, 0, 0, 0);
			}
			if (redir_fd != (u64)-1) {
				syscall_dispatch(SYS_SET_STDOUT, redir_fd, 0, 0, 0);
			}
		} else if (pipe_pos) {
			syscall_dispatch(SYS_CREATE, (u64)(usize)"/tmp/pipe", (u64)(usize)"", 0, 0);
			redir_fd = syscall_dispatch(SYS_OPEN, (u64)(usize)"/tmp/pipe", 0, 0, 0);
			if (redir_fd != (u64)-1) {
				syscall_dispatch(SYS_SET_STDOUT, redir_fd, 0, 0, 0);
			}
		}

		if (strcmp(args[0], "help") == 0) {
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
		} else if (strcmp(args[0], "ip") == 0) {
			syscall_dispatch(SYS_NET_INFO, 0, 0, 0, 0);
		} else if (strcmp(args[0], "ls") == 0) {
			const char *target = cwd;
			if (num_args > 1) target = args[1];
			resolve_path(cwd, target, abs_path);
			syscall_dispatch(SYS_LIST, (u64)(usize)abs_path, 0, 0, 0);
		} else if (strcmp(args[0], "clear") == 0) {
			syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0);
		} else if (strcmp(args[0], "ping") == 0 && num_args > 1) {
			syscall_dispatch(SYS_NET_PING, (u64)(usize)args[1], 0, 0, 0);
		} else if (strcmp(args[0], "resolve") == 0 && num_args > 1) {
			syscall_dispatch(SYS_NET_DNS, (u64)(usize)args[1], 0, 0, 0);
		} else if (strcmp(args[0], "ps") == 0) {
			syscall_dispatch(SYS_PS, 0, 0, 0, 0);
		} else if (strcmp(args[0], "mem") == 0) {
			syscall_dispatch(SYS_MEM, 0, 0, 0, 0);
		} else if (strcmp(args[0], "reboot") == 0) {
			syscall_dispatch(SYS_REBOOT, 0, 0, 0, 0);
		} else if (strcmp(args[0], "jobs") == 0) {
			uwrite("Background jobs tracking not fully implemented, use 'ps'.\n");
		} else if (strcmp(args[0], "export") == 0 && num_args > 1) {
			char *eq = 0;
			for (int i = 0; args[1][i]; i++) {
				if (args[1][i] == '=') { eq = &args[1][i]; break; }
			}
			if (eq) {
				*eq = '\0';
				set_env(args[1], eq + 1);
			}
		} else if (strcmp(args[0], "echo") == 0) {
			for (int i = 1; i < num_args; i++) {
				uwrite(args[i]);
				if (i < num_args - 1) uwrite(" ");
			}
			uwrite("\n");
		} else if (strcmp(args[0], "cd") == 0 && num_args > 1) {
			resolve_path(cwd, args[1], abs_path);
			usize len = strlen(abs_path);
			memcpy(cwd, abs_path, len + 1);
		} else if (strcmp(args[0], "cat") == 0 && num_args > 1) {
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
			resolve_path(cwd, args[0], abs_path);
			u64 pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)abs_path, num_args, (u64)(usize)args, 0);
			if (pid == (u64)-1) {
				pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)args[0], num_args, (u64)(usize)args, 0);
				if (pid == (u64)-1) {
					char bin_path[128];
					resolve_path("/bin", args[0], bin_path);
					pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)bin_path, num_args, (u64)(usize)args, 0);
				}
			}
			
			if (pid == (u64)-1) {
				uwrite("sh: command not found: ");
				uwrite(args[0]);
				uwrite("\n");
			} else if (!is_bg && !pipe_pos) {
				// Wait for it. In a real OS we'd use waitpid. Here we just yield.
				for (int i=0; i<10; i++) {
					syscall_dispatch(SYS_YIELD, 0, 0, 0, 0);
				}
			}
		}

		if (redir_fd != (u64)-1) {
			syscall_dispatch(SYS_SET_STDOUT, (u64)-1, 0, 0, 0);
			syscall_dispatch(SYS_CLOSE, redir_fd, 0, 0, 0);
		}

		// Handle pipe second command
		if (pipe_pos && cmd2) {
			char *args2[16];
			int num_args2 = parse_cmd(cmd2, args2, 16);
			if (num_args2 > 0) {
				if (strcmp(args2[0], "cat") == 0 || strcmp(args2[0], "grep") == 0) {
					// Append /tmp/pipe as argument since we don't have stdin
					args2[num_args2++] = "/tmp/pipe";
				}
				
				if (strcmp(args2[0], "cat") == 0) {
					char buffer[256];
					u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)"/tmp/pipe", 0, 0, 0);
					if (fd != (u64)-1) {
						while (1) {
							u64 count = syscall_dispatch(SYS_READ, fd, (u64)(usize)buffer, sizeof(buffer) - 1, 0);
							if (count == 0 || count == (u64)-1) break;
							buffer[count] = '\0';
							uwrite(buffer);
						}
						syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0);
					}
				} else {
					// External piped command
					u64 pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)args2[0], num_args2, (u64)(usize)args2, 0);
					if (pid == (u64)-1) {
						char bin_path[128];
						resolve_path("/bin", args2[0], bin_path);
						pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)bin_path, num_args2, (u64)(usize)args2, 0);
					}
					if (!is_bg) {
						for (int i=0; i<10; i++) {
							syscall_dispatch(SYS_YIELD, 0, 0, 0, 0);
						}
					}
				}
			}
		}
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
		syscall_dispatch(SYS_YIELD, 0, 0, 0, 0);
	}
	
	return 0;
}

void user_register_builtin_programs(void)
{
	user_register_program("/bin/init", init_main);
	user_register_program("/bin/sh", sh_main);
}
