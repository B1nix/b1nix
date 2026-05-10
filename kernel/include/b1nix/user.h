#ifndef B1NIX_USER_H
#define B1NIX_USER_H

#include <b1nix/types.h>

typedef int (*user_program_entry)(int argc, const char **argv);

#define USER_MAX_ARGS 32
#define USER_MAX_ENVS 32
#define USER_MAX_IMAGE_SEGMENTS 8
#define USER_STACK_SIZE PAGE_SIZE
#define USER_STACK_TOP 0x0000800000000000ULL

enum user_image_kind {
	USER_IMAGE_BUILTIN = 1,
	USER_IMAGE_ELF64 = 2,
};

struct user_image_segment {
	u64 vaddr;
	u64 memsz;
	u64 filesz;
	u64 flags;
	void *data;
};

struct user_address_space {
	u64 pml4_frame;
	u64 stack_base;
	u64 stack_size;
	u64 stack_top;
	void *stack_image;
	usize stack_image_size;
};

struct user_program {
	const char *path;
	user_program_entry entry;
};

struct user_loaded_image {
	enum user_image_kind kind;
	const char *path;
	u64 entry;
	struct user_address_space address_space;
	struct user_image_segment segments[USER_MAX_IMAGE_SEGMENTS];
	usize segment_count;
	int argc;
	const char **argv;
	const char **envp;
};

void userspace_init(void);
int user_spawn(const char *path, int argc, const char **argv);
int user_execve_current(const char *path, const char **argv, const char **envp);
void user_register_builtin_programs(void);
const struct user_program *user_find_program(const char *path);
void user_image_free(struct user_loaded_image *image);

#endif
