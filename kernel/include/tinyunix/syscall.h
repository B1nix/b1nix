#ifndef TINYUNIX_SYSCALL_H
#define TINYUNIX_SYSCALL_H

#include <tinyunix/types.h>

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
	SYS_NET_DEMO = 11,
};

u64 syscall_dispatch(u64 number, u64 arg0, u64 arg1, u64 arg2, u64 arg3);

#endif
