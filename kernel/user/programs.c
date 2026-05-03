#include <string.h>
#include <tinyunix/syscall.h>
#include <tinyunix/user.h>

void user_register_program(const char *path, user_program_entry entry);

static void uwrite(const char *text)
{
	syscall_dispatch(SYS_WRITE, (u64)(usize)text, strlen(text), 0, 0);
}

static int sh_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	uwrite("sh: ls\n");
	syscall_dispatch(SYS_LIST, 0, 0, 0, 0);
	uwrite("sh: cat /etc/motd\n");
	char buffer[64];
	u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)"/etc/motd", 0, 0, 0);
	u64 count = syscall_dispatch(SYS_READ, fd, (u64)(usize)buffer, sizeof(buffer) - 1, 0);
	if (count != (u64)-1) {
		buffer[count] = '\0';
		uwrite(buffer);
	}
	syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0);
	uwrite("sh: cat /tmp/hello\n");
	fd = syscall_dispatch(SYS_OPEN, (u64)(usize)"/tmp/hello", 0, 0, 0);
	count = syscall_dispatch(SYS_READ, fd, (u64)(usize)buffer, sizeof(buffer) - 1, 0);
	if (count != (u64)-1) {
		buffer[count] = '\0';
		uwrite(buffer);
	}
	syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0);
	uwrite("sh: net demo\n");
	syscall_dispatch(SYS_NET_DEMO, 0, 0, 0, 0);
	uwrite("sh: echo userspace online\n");
	uwrite("userspace online\n");
	return 0;
}

static int init_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	uwrite("init: starting /bin/sh\n");
	syscall_dispatch(SYS_SPAWN, (u64)(usize)"/bin/sh", 0, 0, 0);
	syscall_dispatch(SYS_YIELD, 0, 0, 0, 0);
	uwrite("init: done\n");
	return 0;
}

void user_register_builtin_programs(void)
{
	user_register_program("/bin/init", init_main);
	user_register_program("/bin/sh", sh_main);
}
