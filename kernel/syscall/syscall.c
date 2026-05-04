#include <b1nix/console.h>
#include <b1nix/initramfs.h>
#include <b1nix/net.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <b1nix/mm.h>
#include <b1nix/io.h>
#include <string.h>

static u64 sys_write(const char *text, usize size)
{
	int fd = scheduler_get_stdout();
	if (fd != -1) {
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

	sys_write(file->data, file->size);
	return file->size;
}

extern char ps2_kbd_getc(void);

static u64 sys_read_kbd(void)
{
	char c = 0;
	while (c == 0) {
		c = ps2_kbd_getc();
		if (c == 0) {
			scheduler_yield();
		}
	}
	return (u64)c;
}

static u64 sys_clear(void)
{
	console_clear();
	return 0;
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
		return sys_list((const char *)(usize)arg0);
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
	case SYS_NET_PING: {
		struct ipv4_addr dest;
		const char *ip_str = (const char *)(usize)arg0;
		// A real OS would parse the string. For our demo, we just parse a simple string or assume it's an IPv4 string
		// Since we don't have a generic inet_pton yet, we will just send an ICMP Echo to the given IP
		// For simplicity, we assume arg0 is actually a pointer to a struct ipv4_addr, OR we parse it.
		// Wait, the shell will pass a string!
		// We'll write a quick parser.
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
		outb(0x64, 0xFE);
		while (1) {
			__asm__ volatile("hlt");
		}
		return 0;
	case SYS_SET_STDOUT:
		scheduler_set_stdout((int)arg0);
		return 0;
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
	default:
		console_write("syscall: unknown 0x");
		console_write_hex64(number);
		console_write("\n");
		return (u64)-1;
	}
}
