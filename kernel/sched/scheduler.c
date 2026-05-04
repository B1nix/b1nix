#include <string.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>

#define MAX_TASKS 16
#define KERNEL_STACK_SIZE (16 * 1024)

enum task_state {
	TASK_UNUSED = 0,
	TASK_RUNNING,
	TASK_READY,
	TASK_BLOCKED,
	TASK_SLEEPING,
	TASK_DEAD,
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
	kernel_thread_entry entry;
	void *arg;
	void *stack;
	u64 wake_tick;
	int stdout_fd;
};

extern void arch_context_switch(struct cpu_context *old_context, struct cpu_context *new_context);

static struct task tasks[MAX_TASKS];
static struct task *current_task;
static usize next_task_id = 1;
static volatile u64 scheduler_ticks;
static int scheduler_started;

static void interrupts_disable(void)
{
	__asm__ volatile("cli" : : : "memory");
}

static void interrupts_enable(void)
{
	__asm__ volatile("sti" : : : "memory");
}

static u64 align_down_u64(u64 value, u64 alignment)
{
	return value & ~(alignment - 1);
}

static struct task *find_unused_task(void)
{
	for (usize i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].state == TASK_UNUSED) {
			return &tasks[i];
		}
	}

	return 0;
}

static usize task_index(const struct task *task)
{
	return (usize)(task - tasks);
}

static struct task *pick_next_task(void)
{
	if (current_task == 0) {
		return 0;
	}

	usize start = task_index(current_task);

	for (usize offset = 1; offset <= MAX_TASKS; offset++) {
		usize index = (start + offset) % MAX_TASKS;

		if (tasks[index].state == TASK_READY) {
			return &tasks[index];
		}
	}

	return 0;
}

static void wake_sleepers(void)
{
	for (usize i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].state == TASK_SLEEPING && tasks[i].wake_tick <= scheduler_ticks) {
			tasks[i].state = TASK_READY;
			tasks[i].wake_tick = 0;
		}
	}
}

static void kernel_thread_trampoline(void)
{
	interrupts_enable();

	if (current_task == 0 || current_task->entry == 0) {
		panic("scheduler entered invalid task");
	}

	current_task->entry(current_task->arg);
	scheduler_exit_current();
}

void scheduler_init(void)
{
	memset(tasks, 0, sizeof(tasks));

	struct task *boot = &tasks[0];
	boot->id = next_task_id++;
	boot->name = "boot";
	boot->state = TASK_RUNNING;
	boot->stdout_fd = -1;
	current_task = boot;
	scheduler_started = 1;

	console_write("sched: initialized\n");
}

int kthread_create(const char *name, kernel_thread_entry entry, void *arg)
{
	struct task *task = find_unused_task();

	if (task == 0) {
		return -1;
	}

	void *stack = kmalloc(KERNEL_STACK_SIZE);
	u64 stack_top = align_down_u64((u64)(usize)stack + KERNEL_STACK_SIZE, 16);
	u64 initial_rsp = stack_top - 16;
	*(u64 *)(usize)initial_rsp = (u64)(usize)kernel_thread_trampoline;

	task->id = next_task_id++;
	task->name = name;
	task->state = TASK_READY;
	task->entry = entry;
	task->arg = arg;
	task->stack = stack;
	task->wake_tick = 0;
	task->context.rsp = initial_rsp;
	task->context.rbp = 0;
	task->context.rbx = 0;
	task->context.r12 = 0;
	task->context.r13 = 0;
	task->context.r14 = 0;
	task->context.r15 = 0;
	task->stdout_fd = current_task ? current_task->stdout_fd : -1;

	console_write("sched: created task ");
	console_write(name);
	console_write(" id 0x");
	console_write_hex64(task->id);
	console_write("\n");

	return 0;
}

void scheduler_yield(void)
{
	interrupts_disable();
	wake_sleepers();

	struct task *old_task = current_task;
	struct task *new_task = pick_next_task();

	if (new_task == 0) {
		if (old_task != 0 && old_task->state == TASK_DEAD) {
			panic("dead task has nowhere to yield");
		}

		interrupts_enable();
		return;
	}

	if (old_task->state == TASK_RUNNING) {
		old_task->state = TASK_READY;
	}

	new_task->state = TASK_RUNNING;
	current_task = new_task;

	arch_context_switch(&old_task->context, &new_task->context);
	interrupts_enable();
}

void scheduler_block_current(void)
{
	interrupts_disable();

	if (current_task == 0 || current_task->state != TASK_RUNNING) {
		panic("scheduler_block_current without running task");
	}

	current_task->state = TASK_BLOCKED;
	scheduler_yield();
	interrupts_enable();
}

void scheduler_wake_task(usize task_id)
{
	interrupts_disable();

	for (usize i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].id == task_id && tasks[i].state == TASK_BLOCKED) {
			tasks[i].state = TASK_READY;
			break;
		}
	}

	interrupts_enable();
}

void scheduler_sleep_ticks(u64 ticks)
{
	interrupts_disable();

	if (current_task == 0 || current_task->state != TASK_RUNNING) {
		panic("scheduler_sleep_ticks without running task");
	}

	current_task->wake_tick = scheduler_ticks + ticks;
	current_task->state = TASK_SLEEPING;
	scheduler_yield();
	interrupts_enable();
}

void scheduler_on_timer_tick(void)
{
	if (!scheduler_started || current_task == 0) {
		return;
	}

	scheduler_ticks++;
	wake_sleepers();

	if (current_task->state == TASK_RUNNING) {
		scheduler_yield();
	}
}

void scheduler_exit_current(void)
{
	interrupts_disable();

	if (current_task == 0) {
		panic("scheduler_exit_current without current task");
	}

	console_write("sched: task exited ");
	console_write(current_task->name);
	console_write("\n");

	current_task->state = TASK_DEAD;
	scheduler_yield();
	panic("dead task resumed");
}

usize scheduler_task_count(void)
{
	usize count = 0;

	for (usize i = 0; i < MAX_TASKS; i++) {
			if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_DEAD) {
				count++;
			}
	}

	return count;
}

void scheduler_dump_tasks(void)
{
	console_write("ID\tSTATE\tNAME\n");
	for (usize i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].state != TASK_UNUSED) {
			console_write_hex64(tasks[i].id);
			console_write("\t");
			
			const char *state_str = "UNKNOWN";
			switch (tasks[i].state) {
			case TASK_RUNNING: state_str = "RUNNING"; break;
			case TASK_READY: state_str = "READY"; break;
			case TASK_BLOCKED: state_str = "BLOCKED"; break;
			case TASK_SLEEPING: state_str = "SLEEPING"; break;
			case TASK_DEAD: state_str = "DEAD"; break;
			default: break;
			}
			
			console_write(state_str);
			console_write("\t");
			console_write(tasks[i].name);
			console_write("\n");
		}
	}
}

void scheduler_set_stdout(int fd)
{
	interrupts_disable();
	if (current_task != 0) {
		current_task->stdout_fd = fd;
	}
	interrupts_enable();
}

int scheduler_get_stdout(void)
{
	int fd = -1;
	interrupts_disable();
	if (current_task != 0) {
		fd = current_task->stdout_fd;
	}
	interrupts_enable();
	return fd;
}
