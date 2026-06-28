#ifndef B1NIX_USER_H
#define B1NIX_USER_H

#include <b1nix/types.h>

typedef int (*user_program_entry)(int argc, const char **argv);

#define USER_MAX_ARGS 32
#define USER_MAX_ENVS 32
#define USER_MAX_IMAGE_SEGMENTS 32
#define USER_STACK_SIZE PAGE_SIZE
#ifdef __x86_64__
#define USER_SPACE_LIMIT 0x0000800000000000ULL
#define USER_STACK_TOP 0x0000800000000000ULL
#else
/* 32-bit layout: user [0, 2 GiB), kernel [2 GiB, 4 GiB). The kernel direct-maps
 * physical RAM at DIRECT_MAP_BASE (0x80000000) for up to DIRECT_MAP_MAX (1 GiB),
 * i.e. [0x80000000, 0xC0000000). The user stack MUST stay below 0x80000000 or
 * it aliases that direct map once RAM is large enough for the map to reach the
 * stack (>~1 GiB): the stack page at the old 0xBFFFF000 then resolves to the
 * supervisor direct-map page instead of the task's stack, so argv/locals read
 * back as NULL/garbage and userspace crashes (this is the native gcc driver
 * SIGSEGV at -m 1024+ and on the 16 GiB box). Keep the stack just under the
 * 2 GiB kernel split. USER_SPACE_LIMIT stays 0xC0000000 so the address-space
 * clone / lazy-fault range (PD index < 768) is unchanged. */
#define USER_SPACE_LIMIT 0xC0000000ULL
#define USER_STACK_TOP 0x80000000ULL
#endif

enum user_image_kind {
	USER_IMAGE_BUILTIN = 1,
	USER_IMAGE_ELF64 = 2,
	USER_IMAGE_ELF32 = 3,
};

/* M40 — binary personality. b1nix native binaries use the b1nix syscall ABI
 * (numbers in <b1nix/syscall.h>); Linux binaries use the Linux x86_64 syscall
 * ABI (same CPU calling convention, different numbers). The ELF loader detects
 * a Linux binary (via EI_OSABI==ELFOSABI_LINUX or a GNU .note.ABI-tag with
 * OS=Linux) and tags the image PERSONALITY_LINUX; syscall_dispatch_impl then
 * translates Linux syscall numbers to the native handlers for such tasks. */
enum user_personality {
	PERSONALITY_B1NIX = 0,
	PERSONALITY_LINUX = 1,
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
	int refcount;
	/* PT_TLS (thread-local storage) template for the main thread. tls_memsz==0
	 * means the binary has no TLS. tls_data holds a copy of the initialised
	 * portion (tdata, tls_filesz bytes); the rest up to tls_memsz is .tbss. */
	u64 tls_memsz;
	u64 tls_filesz;
	u64 tls_align;
	void *tls_data;
	/* M42: user VA of the kernel-owned signal-return trampoline page (RO+exec),
	 * mapped at exec. arch_build_signal_frame points the handler's return here
	 * instead of trusting a userspace-supplied sa_restorer. 0 = not mapped. */
	u64 sigreturn_trampoline;
	/* M40: binary personality (see enum user_personality). Set by the ELF64
	 * loader; PERSONALITY_LINUX activates Linux syscall-number translation. */
	enum user_personality personality;
};

void userspace_init(void);
int user_spawn(const char *path, int argc, const char **argv);
/* Like user_spawn but with an explicit environment (envp, NULL-terminated). */
int user_spawn_env(const char *path, int argc, const char **argv,
                   const char **envp);
int user_execve_current(const char *path, const char **argv, const char **envp);
void user_register_builtin_programs(void);
const struct user_program *user_find_program(const char *path);
struct task;
void user_address_space_cleanup(struct task *t);
/* Full executable path of a task (the loaded image's path), for /proc/<pid>/exe.
 * Falls back to task->name. */
const char *user_task_exe_path(struct task *t);
void user_image_free(struct user_loaded_image *image);

#endif
