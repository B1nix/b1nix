#include <tinyunix/console.h>
#include <tinyunix/initramfs.h>
#include <tinyunix/net.h>
#include <tinyunix/sched.h>
#include <tinyunix/syscall.h>
#include <tinyunix/user.h>
#include <tinyunix/vfs.h>

static u64 sys_write(const char *text, usize size)
{
	for (usize i = 0; i < size; i++) {
		console_putc(text[i]);
	}

	return size;
}

static u64 sys_list(void)
{
	const char *paths[64];
	usize count = vfs_list(paths, 64);

	for (usize i = 0; i < count; i++) {
		console_write(paths[i]);
		console_write("\n");
	}

	return count;
}

static u64 sys_read_file(const char *path)
{
	const struct initramfs_file *file = initramfs_find(path);

	if (file == 0) {
		return (u64)-1;
	}

	sys_write(file->data, file->size);
	return file->size;
}

u64 syscall_dispatch(u64 number, u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
	(void)arg3;

	switch (number) {
	case SYS_WRITE:
		return sys_write((const char *)(usize)arg0, (usize)arg1);
	case SYS_EXIT:
		scheduler_exit_current();
	case SYS_SPAWN:
		return (u64)user_spawn((const char *)(usize)arg0, (int)arg1, (const char **)(usize)arg2);
	case SYS_LIST:
		return sys_list();
	case SYS_READ_FILE:
		return sys_read_file((const char *)(usize)arg0);
	case SYS_YIELD:
		scheduler_yield();
		return 0;
	case SYS_OPEN:
		return (u64)vfs_open((const char *)(usize)arg0);
	case SYS_READ:
		return (u64)vfs_read((int)arg0, (char *)(usize)arg1, (usize)arg2);
	case SYS_CLOSE:
		vfs_close((int)arg0);
		return 0;
	case SYS_CREATE:
		return (u64)vfs_create((const char *)(usize)arg0, (const char *)(usize)arg1);
	case SYS_NET_DEMO:
		net_demo();
		return 0;
	default:
		console_write("syscall: unknown 0x");
		console_write_hex64(number);
		console_write("\n");
		return (u64)-1;
	}
}
