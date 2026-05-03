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

static int sh_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	uwrite("Welcome to b1nix shell!\nType 'help' for a list of commands.\n\n");

	char line[128];
	while (1) {
		uwrite("b1nix> ");
		readline(line, sizeof(line));

		char *cmd = line;
		// Trim leading spaces
		while (*cmd == ' ') {
			cmd++;
		}

		if (cmd[0] == '\0') {
			continue;
		}

		if (strcmp(cmd, "help") == 0) {
			uwrite("Built-in commands:\n");
			uwrite("  help       - Show this message\n");
			uwrite("  ls         - List files in root directory\n");
			uwrite("  cat <file> - Read and print a file's content\n");
			uwrite("  echo <txt> - Print text\n");
			uwrite("  clear      - Clear the screen\n");
			uwrite("  net        - Run network demo\n");
		} else if (strcmp(cmd, "ls") == 0) {
			syscall_dispatch(SYS_LIST, 0, 0, 0, 0);
		} else if (strcmp(cmd, "clear") == 0) {
			syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0);
		} else if (strcmp(cmd, "net") == 0) {
			syscall_dispatch(SYS_NET_DEMO, 0, 0, 0, 0);
		} else if (strncmp(cmd, "echo ", 5) == 0) {
			uwrite(cmd + 5);
			uwrite("\n");
		} else if (strncmp(cmd, "cat ", 4) == 0) {
			char *path = cmd + 4;
			// Trim leading spaces from path
			while (*path == ' ') path++;

			char buffer[256];
			u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)path, 0, 0, 0);
			if (fd == (u64)-1) {
				uwrite("cat: file not found: ");
				uwrite(path);
				uwrite("\n");
			} else {
				while (1) {
					u64 count = syscall_dispatch(SYS_READ, fd, (u64)(usize)buffer, sizeof(buffer) - 1, 0);
					if (count == 0 || count == (u64)-1) {
						break;
					}
					buffer[count] = '\0';
					uwrite(buffer);
				}
				syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0);
			}
		} else if (strncmp(cmd, "cd ", 3) == 0) {
			uwrite("cd: virtual file system does not support working directories yet\n");
		} else {
			// Try to spawn it as an external program
			u64 pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)cmd, 0, 0, 0);
			if (pid == (u64)-1) {
				// Syscall spawn prints its own error, but we add our generic one too
				// Note: user_spawn uses console_write which bypasses our uwrite (so it might print over UI)
				// But we'll just let it fail and print our error.
				uwrite("sh: command not found: ");
				uwrite(cmd);
				uwrite("\n");
			} else {
				// Yield to let the newly spawned process run
				syscall_dispatch(SYS_YIELD, 0, 0, 0, 0);
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
