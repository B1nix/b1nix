#ifndef B1NIX_DIRENT_H
#define B1NIX_DIRENT_H

#include <b1nix/types.h>

/* Directory entry returned by SYS_READDIR */
struct dirent {
	char  name[64];   /* File/directory name */
	u32   type;        /* 1 = file, 2 = device, 3 = directory */
	u32   is_dir;      /* 1 if directory */
	u32   is_exec;     /* 1 if executable */
	u64   size;        /* File size */
};

#endif
