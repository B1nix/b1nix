#include <stdio.h>
#include <stddef.h>

typedef unsigned long long u64;
typedef unsigned long usize;

enum task_state {
	READY,
	RUNNING,
	BLOCKED,
	SLEEPING,
	EXITED
};

struct cpu_context {
	u64 rsp;
	u64 rbp;
	u64 rbx;
	u64 r12;
	u64 r13;
	u64 r14;
	u64 r15;
};

struct task {
	usize id;
	const char *name;
	enum task_state state;
	struct cpu_context context;
	void *entry;
	void *arg;
	void *stack;
	u64 kernel_stack_ptr;
	u64 saved_user_rsp;
};

int main() {
    printf("kernel_stack_ptr: %lu\n", offsetof(struct task, kernel_stack_ptr));
    printf("saved_user_rsp: %lu\n", offsetof(struct task, saved_user_rsp));
    return 0;
}
