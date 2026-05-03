#include <string.h>
#include <b1nix/console.h>
#include <b1nix/initramfs.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/user.h>

#define MAX_PROGRAMS 16
#define USER_STACK_SIZE PAGE_SIZE

struct process_start {
	const struct user_program *program;
	int argc;
	const char **argv;
	struct user_address_space address_space;
};

static struct user_program programs[MAX_PROGRAMS];
static usize program_count;

static struct user_address_space user_address_space_create(void)
{
	struct user_address_space address_space;

	address_space.pml4_frame = pmm_alloc_frame();
	address_space.stack_base = pmm_alloc_frame();
	address_space.stack_size = USER_STACK_SIZE;

	return address_space;
}

static void user_process_thread(void *arg)
{
	struct process_start *start = arg;

	console_write("user: enter ");
	console_write(start->program->path);
	console_write(" pml4 0x");
	console_write_hex64(start->address_space.pml4_frame);
	console_write("\n");

	int code = start->program->entry(start->argc, start->argv);
	syscall_dispatch(SYS_EXIT, (u64)code, 0, 0, 0);
}

void userspace_init(void)
{
	program_count = 0;
	user_register_builtin_programs();
	console_write("userspace: builtin programs 0x");
	console_write_hex64(program_count);
	console_write("\n");
}

int user_spawn(const char *path, int argc, const char **argv)
{
	const struct initramfs_file *file = initramfs_find(path);

	if (file == 0 || (file->flags & INITRAMFS_EXECUTABLE) == 0) {
		console_write("user: executable not found ");
		console_write(path);
		console_write("\n");
		return -1;
	}

	const struct user_program *program = user_find_program(path);

	if (program == 0) {
		console_write("user: no loader for ");
		console_write(path);
		console_write("\n");
		return -1;
	}

	struct process_start *start = kzalloc(sizeof(*start));
	start->program = program;
	start->argc = argc;
	start->argv = argv;
	start->address_space = user_address_space_create();

	return kthread_create(path, user_process_thread, start);
}

void user_register_program(const char *path, user_program_entry entry)
{
	if (program_count >= MAX_PROGRAMS) {
		panic("too many builtin user programs");
	}

	programs[program_count].path = path;
	programs[program_count].entry = entry;
	program_count++;
}

const struct user_program *user_find_program(const char *path)
{
	for (usize i = 0; i < program_count; i++) {
		if (strcmp(programs[i].path, path) == 0) {
			return &programs[i];
		}
	}

	return 0;
}
