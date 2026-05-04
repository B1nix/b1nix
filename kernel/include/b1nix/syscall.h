#ifndef B1NIX_SYSCALL_H
#define B1NIX_SYSCALL_H

#include <b1nix/types.h>

enum syscall_number {
	SYS_WRITE = 1,
	SYS_EXIT = 2,
	SYS_SPAWN = 3,
	SYS_LIST = 4,
	SYS_READ_FILE = 5,
	SYS_YIELD = 6,
	SYS_OPEN = 7,
	SYS_READ = 8,
	SYS_CLOSE = 9,
	SYS_CREATE = 10,
	SYS_NET_PING = 11,
	SYS_NET_DNS = 12,
	SYS_READ_KBD = 13,
	SYS_CLEAR = 14,
	SYS_PS = 15,
	SYS_MEM = 16,
	SYS_REBOOT = 17,
	SYS_SET_STDOUT = 18,
	SYS_NET_INFO = 19,
};

u64 syscall_dispatch(u64 number, u64 arg0, u64 arg1, u64 arg2, u64 arg3);

#endif
