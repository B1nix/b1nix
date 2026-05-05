#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/initramfs.h>
#include <b1nix/net.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <b1nix/mm.h>
#include <b1nix/io.h>
#include <b1nix/mqueue.h>
#include <b1nix/shm.h>
#include <b1nix/uidgid.h>
#include <b1nix/dirent.h>
#include <b1nix/posix.h>
#include <string.h>

int syscall_copyin(void *dst, const void *user_src, usize size)
{
	if (size == 0) return 0;
	if (!dst || !user_src) return -1;
	memcpy(dst, user_src, size);
	return 0;
}

int syscall_copyout(void *user_dst, const void *src, usize size)
{
	if (size == 0) return 0;
	if (!user_dst || !src) return -1;
	memcpy(user_dst, src, size);
	return 0;
}

int syscall_copyinstr(char *dst, usize dst_size, const char *user_src)
{
	if (!dst || dst_size == 0 || !user_src) return -1;

	for (usize i = 0; i < dst_size; i++) {
		char c;
		if (syscall_copyin(&c, user_src + i, 1) != 0) return -1;
		dst[i] = c;
		if (c == '\0') return 0;
	}

	dst[dst_size - 1] = '\0';
	return -1;
}

static u64 sys_write(const char *text, usize size, int fd, int has_fd)
{
	if (!text && size != 0) return (u64)-1;
	if (!has_fd) {
		fd = scheduler_get_stdout();
		if (fd == -1 && scheduler_fd_get(1) >= 0) {
			fd = 1;
		}
	}
	if (fd != -1 || has_fd) {
		return (u64)vfs_write(fd, text, size);
	}

	for (usize i = 0; i < size; i++) {
		console_putc(text[i]);
	}

	return size;
}

static u64 sys_list(const char *dir_path)
{
	const char *paths[64];
	usize count = vfs_list(dir_path, paths, 64);

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

	sys_write(file->data, file->size, -1, 0);
	return file->size;
}

#ifndef __aarch64__
static u64 sys_read_kbd(void)
{
	char c = 0;
	if (vfs_read(0, &c, 1) == 1) return (u64)c;
	return 0;
}
#endif

static u64 sys_readdir(const char *dir_path, struct dirent *buf, usize max_entries)
{
	const char *names[128];
	usize count = vfs_list(dir_path, names, 128);
	if (count > max_entries) count = max_entries;

	for (usize i = 0; i < count; i++) {
		usize len = strlen(names[i]);
		if (len > 63) len = 63;
		memcpy(buf[i].name, names[i], len);
		buf[i].name[len] = '\0';

		/* Try to get more info by resolving the path */
		char full_path[256];
		usize dirlen = strlen(dir_path);
		memcpy(full_path, dir_path, dirlen);
		if (dirlen > 0 && full_path[dirlen - 1] != '/') {
			full_path[dirlen++] = '/';
		}
		memcpy(full_path + dirlen, names[i], len + 1);

		struct vfs_node *node = vfs_find_node(full_path);
		if (node) {
			buf[i].type   = (u32)node->type;
			buf[i].is_dir = (node->type == VFS_DIRECTORY) ? 1 : 0;
			buf[i].is_exec = (node->mode & 0111) ? 1 : 0;
			buf[i].size   = node->size;
		} else {
			buf[i].type   = 0;
			buf[i].is_dir = 0;
			buf[i].is_exec = 0;
			buf[i].size   = 0;
		}
	}

	return count;
}

static u64 sys_clear(void)
{
	console_clear();
	return 0;
}

static void copy_cstr(char *dst, usize dst_size, const char *src)
{
	if (dst_size == 0) return;
	usize len = strlen(src);
	if (len >= dst_size) len = dst_size - 1;
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static void normalize_path(const char *path, char *out, usize out_size)
{
	if (!path || out_size == 0) return;
	if (path[0] == '/') {
		copy_cstr(out, out_size, path);
		return;
	}

	const char *cwd = scheduler_get_cwd();
	usize cwd_len = strlen(cwd);
	if (cwd_len >= out_size) cwd_len = out_size - 1;
	memcpy(out, cwd, cwd_len);
	if (cwd_len == 0 || out[cwd_len - 1] != '/') {
		if (cwd_len + 1 < out_size) out[cwd_len++] = '/';
	}
	usize path_len = strlen(path);
	usize remain = out_size - cwd_len - 1;
	if (path_len > remain) path_len = remain;
	memcpy(out + cwd_len, path, path_len);
	out[cwd_len + path_len] = '\0';
}

static u64 sys_execve(const char *path, const char **argv, const char **envp)
{
	char kernel_path[VFS_MAX_PATH];
	if (syscall_copyinstr(kernel_path, sizeof(kernel_path), path) != 0) {
		return (u64)-1;
	}
	return (u64)user_execve_current(kernel_path, argv, envp);
}

static u64 sys_ioctl(int fd, u64 request, void *arg)
{
	return (u64)vfs_ioctl(fd, request, arg);
}

static u64 sys_selfhost_status(struct b1nix_selfhost_status *status)
{
	if (!status) return (u64)-1;
	memset(status, 0, sizeof(*status));
	status->abi_version = 17;
	status->target_ready = 1;
	status->binutils_ready = 1;
	status->make_ready = 1;
	status->can_build_kernel_inside_b1nix = 0;
	copy_cstr(status->target_triple, sizeof(status->target_triple), "x86_64-b1nix");
	copy_cstr(status->compiler, sizeof(status->compiler), "gcc-port-manifest");
	copy_cstr(status->assembler, sizeof(status->assembler), "b1nix-as-abi");
	copy_cstr(status->linker, sizeof(status->linker), "b1nix-ld-abi");
	copy_cstr(status->make, sizeof(status->make), "nmake");
	return 0;
}

u64 syscall_dispatch(u64 number, u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
	switch (number) {
	case SYS_WRITE:
		return sys_write((const char *)(usize)arg0, (usize)arg1, (int)arg2, arg3 != 0);
	case SYS_EXIT:
		scheduler_exit_current((int)arg0);
	case SYS_SPAWN:
		return (u64)user_spawn((const char *)(usize)arg0, (int)arg1, (const char **)(usize)arg2);
	case SYS_LIST:
		return sys_list((const char *)(usize)arg0);
	case SYS_READ_FILE:
		return sys_read_file((const char *)(usize)arg0);
	case SYS_YIELD:
		scheduler_yield();
		return 0;
	case SYS_OPEN:
		return (u64)vfs_open_flags((const char *)(usize)arg0, (int)arg1);
	case SYS_READ:
		return (u64)vfs_read((int)arg0, (char *)(usize)arg1, (usize)arg2);
	case SYS_CLOSE:
		vfs_close((int)arg0);
		return 0;
	case SYS_CREATE:
		return (u64)vfs_create((const char *)(usize)arg0, (const char *)(usize)arg1);
#ifndef __aarch64__
	case SYS_NET_PING: {
		struct ipv4_addr dest;
		const char *ip_str = (const char *)(usize)arg0;
		usize i = 0, j = 0;
		u8 val = 0;
		while (ip_str[i] && j < 4) {
			if (ip_str[i] == '.') {
				dest.bytes[j++] = val;
				val = 0;
			} else {
				val = val * 10 + (ip_str[i] - '0');
			}
			i++;
		}
		if (j < 4) dest.bytes[j] = val;

		u8 echo[64];
		memset(echo, 0, 64);
		echo[0] = 8; // Type: Echo Request
		echo[1] = 0; // Code
		echo[2] = 0; // Checksum
		echo[3] = 0;
		echo[4] = 0; // ID

		for (int i = 0; i < 4; i++) {
			echo[2] = 0;
			echo[3] = 0;
			echo[5] = i + 1; // Seq
			
			u32 sum = 0;
			for (usize j = 0; j < 64; j += 2) {
				sum += ((u16)echo[j] << 8) | echo[j + 1];
			}
			while ((sum >> 16) != 0) {
				sum = (sum & 0xffff) + (sum >> 16);
			}
			u16 csum = ~sum;
			echo[2] = (u8)(csum >> 8);
			echo[3] = (u8)(csum & 0xff);

			ipv4_send(dest, 1 /* ICMP */, echo, sizeof(echo));
			console_write("ping: sent request seq=");
			console_write_dec(i + 1);
			console_write("\n");
			scheduler_sleep_ticks(100); // Wait approx 1 second
		}
		
		return 0;
	}
	case SYS_NET_DNS:
		dns_resolve((const char *)(usize)arg0);
		return 0;
	case SYS_READ_KBD:
		return sys_read_kbd();
#else
	case SYS_NET_PING:
	case SYS_NET_DNS:
	case SYS_READ_KBD:
		return (u64)-1;
#endif
	case SYS_CLEAR:
		return sys_clear();
	case SYS_PS:
		scheduler_dump_tasks();
		return 0;
	case SYS_MEM:
		console_write("Total usable memory: ");
		console_write_dec(pmm_total_usable_memory() / 1024);
		console_write(" KB\n");
		console_write("Free memory approx:  ");
		console_write_dec(pmm_free_memory_estimate() / 1024);
		console_write(" KB\n");
		return 0;
	case SYS_REBOOT:
#ifdef __aarch64__
		arch_halt();
#else
		outb(0x64, 0xFE);
		while (1) {
			__asm__ volatile("hlt");
		}
#endif
		return 0;
	case SYS_SET_STDOUT:
		scheduler_set_stdout((int)arg0);
		return 0;
#ifndef __aarch64__
	case SYS_NET_INFO: {
		struct mac_addr mac = net_get_mac();
		struct ipv4_addr ip = net_get_ip();
		struct ipv4_addr gw = net_get_gateway();

		console_write("MAC Address: ");
		for (int i = 0; i < 6; i++) {
			if (mac.bytes[i] < 0x10) console_write("0");
			console_write_hex32(mac.bytes[i]);
			if (i < 5) console_write(":");
		}
		console_write("\nIP Address:  ");
		for (int i = 0; i < 4; i++) {
			console_write_dec(ip.bytes[i]);
			if (i < 3) console_write(".");
		}
		console_write("\nGateway:     ");
		for (int i = 0; i < 4; i++) {
			console_write_dec(gw.bytes[i]);
			if (i < 3) console_write(".");
		}
		console_write("\n");
		return 0;
	}
#else
	case SYS_NET_INFO:
		console_write("Network not available on AArch64 yet.\n");
		return 0;
#endif
	case SYS_EXEC: {
		const char *path = (const char *)(usize)arg0;
		int argc = (int)arg1;
		const char **argv = (const char **)(usize)arg2;

		const char *empty_env[] = {0};
		const char **envp = empty_env;
		(void)argc;
		return (u64)user_execve_current(path, argv, envp);
	}
	case SYS_WAIT:
		return (u64)scheduler_wait((usize)arg0, (int *)(usize)arg1);
	case SYS_MMAP:
		return (u64)(usize)kmalloc((usize)arg0);
	case SYS_SLEEP:
		scheduler_sleep_ticks(arg0);
		return 0;
	case SYS_KILL:
		return (u64)scheduler_kill((usize)arg0, (int)arg1);
	case SYS_SIGNAL: {
		/* arg0 = sig, arg1 = handler, arg2 = flags
		 * Returns old handler, or SIG_ERR on error */
		int sig = (int)arg0;
		if (sig < 1 || sig >= NSIG) return (u64)SIG_ERR;
		if (sig == SIGKILL || sig == SIGSTOP) return (u64)SIG_ERR;
		struct sigaction act, old;
		memset(&act, 0, sizeof(act));
		memset(&old, 0, sizeof(old));
		act.sa_handler = (sighandler_t)arg1;
		act.sa_flags = arg2;
		if (scheduler_sigaction(sig, &act, &old) < 0) return (u64)SIG_ERR;
		return (u64)old.sa_handler;
	}
	case SYS_GETPID:
		return scheduler_get_pid();
	case SYS_MQ_OPEN: {
		/* arg0 = name (char*), arg1 = flags (unused for now) */
		const char *name = (const char *)(usize)arg0;
		struct mqueue *mq = mqueue_create(name);
		return (mq) ? (u64)(usize)mq : (u64)-1;
	}
	case SYS_MQ_SEND: {
		/* arg0 = mqueue ptr, arg1 = data ptr, arg2 = data len */
		struct mqueue *mq = (struct mqueue *)(usize)arg0;
		const void *data = (const void *)(usize)arg1;
		u32 len = (u32)arg2;
		return (u64)mqueue_send(mq, data, len);
	}
	case SYS_MQ_RECEIVE: {
		/* arg0 = mqueue ptr, arg1 = buffer ptr, arg2 = len ptr */
		struct mqueue *mq = (struct mqueue *)(usize)arg0;
		void *buffer = (void *)(usize)arg1;
		u32 *len = (u32 *)(usize)arg2;
		return (u64)mqueue_receive(mq, buffer, len);
	}
	case SYS_MQ_CLOSE: {
		struct mqueue *mq = (struct mqueue *)(usize)arg0;
		mqueue_close(mq);
		return 0;
	}
	case SYS_MQ_UNLINK: {
		const char *name = (const char *)(usize)arg0;
		return (u64)mqueue_unlink(name);
	}
	case SYS_SHMGET:
		return (u64)shmget((u32)arg0, (usize)arg1, (int)arg2);
	case SYS_SHMAT:
		return (u64)(usize)shmat((int)arg0, (const void *)(usize)arg1, (int)arg2);
	case SYS_SHMDT:
		return (u64)shmdt((const void *)(usize)arg0);
	case SYS_SHMCTL:
		return (u64)shmctl((int)arg0, (int)arg1, (struct shmid_ds *)(usize)arg2);
	case SYS_CHMOD:
		return (u64)vfs_chmod((const char *)(usize)arg0, (u16)arg1);
	case SYS_CHOWN:
		return (u64)vfs_chown((const char *)(usize)arg0, (u16)arg1, (u16)arg2);
	case SYS_GETUID: {
		struct cred *c = scheduler_get_current_cred();
		return c ? c->uid : 0;
	}
	case SYS_GETEUID: {
		struct cred *c = scheduler_get_current_cred();
		return c ? c->euid : 0;
	}
	case SYS_GETGID: {
		struct cred *c = scheduler_get_current_cred();
		return c ? c->gid : 0;
	}
	case SYS_GETEGID: {
		struct cred *c = scheduler_get_current_cred();
		return c ? c->egid : 0;
	}
	case SYS_SETUID: {
		struct cred *c = scheduler_get_current_cred();
		if (!c) return -1;
		return (u64)cred_set_uid(c, (u16)arg0);
	}
	case SYS_SETGID: {
		struct cred *c = scheduler_get_current_cred();
		if (!c) return -1;
		return (u64)cred_set_gid(c, (u16)arg0);
	}
	case SYS_READDIR:
		return sys_readdir((const char *)(usize)arg0, (struct dirent *)(usize)arg1, (usize)arg2);
	case SYS_FORK:
		return (u64)scheduler_fork_current();
	case SYS_EXECVE:
		return sys_execve((const char *)(usize)arg0, (const char **)(usize)arg1, (const char **)(usize)arg2);
	case SYS_WAITPID:
		return (u64)scheduler_waitpid((usize)arg0, (int *)(usize)arg1, (int)arg2);
	case SYS_STAT: {
		char path[VFS_MAX_PATH];
		normalize_path((const char *)(usize)arg0, path, sizeof(path));
		return (u64)vfs_stat(path, (struct b1nix_stat *)(usize)arg1);
	}
	case SYS_LSEEK:
		return (u64)vfs_lseek((int)arg0, (isize)arg1, (int)arg2);
	case SYS_UNLINK: {
		char path[VFS_MAX_PATH];
		normalize_path((const char *)(usize)arg0, path, sizeof(path));
		return (u64)vfs_unlink(path);
	}
	case SYS_MKDIR: {
		char path[VFS_MAX_PATH];
		(void)arg1;
		normalize_path((const char *)(usize)arg0, path, sizeof(path));
		return (u64)vfs_mkdir(path);
	}
	case SYS_CHDIR: {
		char path[VFS_MAX_PATH];
		normalize_path((const char *)(usize)arg0, path, sizeof(path));
		struct vfs_node *node = vfs_find_node(path);
		if (!node || node->type != VFS_DIRECTORY) return (u64)-1;
		return (u64)scheduler_set_cwd(path);
	}
	case SYS_GETDENTS:
		return (u64)vfs_getdents((int)arg0, (struct dirent *)(usize)arg1, (usize)arg2);
	case SYS_PIPE:
		return (u64)vfs_pipe((int *)(usize)arg0);
	case SYS_DUP2:
		return (u64)vfs_dup2((int)arg0, (int)arg1);
	case SYS_FCNTL:
		return (u64)vfs_fcntl((int)arg0, (int)arg1, arg2);
	case SYS_MUNMAP:
		kfree((void *)(usize)arg0);
		return 0;
	case SYS_BRK:
		return scheduler_brk_set(arg0);
	case SYS_SOCKET:
		return (u64)vfs_socket((int)arg0, (int)arg1, (int)arg2);
	case SYS_BIND:
		return (u64)vfs_bind((int)arg0, (const void *)(usize)arg1, (usize)arg2);
	case SYS_CONNECT:
		return (u64)vfs_connect((int)arg0, (const void *)(usize)arg1, (usize)arg2);
	case SYS_SEND:
		return (u64)vfs_socket_send((int)arg0, (const void *)(usize)arg1, (usize)arg2, (int)arg3);
	case SYS_RECV:
		return (u64)vfs_socket_recv((int)arg0, (void *)(usize)arg1, (usize)arg2, (int)arg3);
	case SYS_IOCTL:
		return sys_ioctl((int)arg0, arg1, (void *)(usize)arg2);
	case SYS_TERMIOS_GET:
		return sys_ioctl((int)arg0, B1NIX_TCGETS, (void *)(usize)arg1);
	case SYS_TERMIOS_SET:
		return sys_ioctl((int)arg0, B1NIX_TCSETS, (void *)(usize)arg1);
	case SYS_SELFHOST_STATUS:
		return sys_selfhost_status((struct b1nix_selfhost_status *)(usize)arg0);
	case SYS_RENAME: {
		char old_path[VFS_MAX_PATH];
		char new_path[VFS_MAX_PATH];
		normalize_path((const char *)(usize)arg0, old_path, sizeof(old_path));
		normalize_path((const char *)(usize)arg1, new_path, sizeof(new_path));
		return (u64)vfs_rename(old_path, new_path);
	}
	case SYS_RMDIR: {
		char path[VFS_MAX_PATH];
		normalize_path((const char *)(usize)arg0, path, sizeof(path));
		return (u64)vfs_rmdir(path);
	}
	case SYS_FSTAT:
		return (u64)vfs_fstat((int)arg0, (struct b1nix_stat *)(usize)arg1);
	case SYS_FSYNC:
		return (u64)vfs_fsync((int)arg0);
	case SYS_MOUNT:
		return (u64)vfs_mount((const char *)(usize)arg0, (const char *)(usize)arg1, (const char *)(usize)arg2, arg3);
	case SYS_UMOUNT:
		return (u64)vfs_umount((const char *)(usize)arg0);
	case SYS_SYNC:
		return (u64)vfs_sync();
	default:
		console_write("syscall: unknown 0x");
		console_write_hex64(number);
		console_write("\n");
		return (u64)-1;
	}
}
