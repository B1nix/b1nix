#ifndef TINYUNIX_USER_H
#define TINYUNIX_USER_H

#include <tinyunix/types.h>

typedef int (*user_program_entry)(int argc, const char **argv);

struct user_address_space {
	u64 pml4_frame;
	u64 stack_base;
	u64 stack_size;
};

struct user_program {
	const char *path;
	user_program_entry entry;
};

void userspace_init(void);
int user_spawn(const char *path, int argc, const char **argv);
void user_register_builtin_programs(void);
const struct user_program *user_find_program(const char *path);

#endif
