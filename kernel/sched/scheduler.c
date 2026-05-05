#include <string.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/uidgid.h>

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
#ifdef __aarch64__
	u64 x19;
	u64 x20;
	u64 x21;
	u64 x22;
	u64 x23;
	u64 x24;
	u64 x25;
	u64 x26;
	u64 x27;
	u64 x28;
	u64 fp;
	u64 lr;
	u64 sp;
#else
	u64 rsp;
	u64 rbp;
	u64 rbx;
	u64 r12;
	u64 r13;
	u64 r14;
	u64 r15;
#endif
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
	int priority;
	int exit_code;
	usize parent_id;

	/* Signal handling */
	u64 pending_signals;       /* bitmask of pending signals */
	u64 blocked_signals;       /* bitmask of blocked signals */
	struct sigaction sigactions[NSIG];  /* signal actions */

	/* Credentials */
	struct cred *cred;
};

extern void arch_context_switch(struct cpu_context *old_context, struct cpu_context *new_context);

static struct task tasks[MAX_TASKS];
static struct task *current_task;
static usize next_task_id = 1;
static volatile u64 scheduler_ticks;
static int scheduler_started;

static void interrupts_disable(void)
{
#ifdef __aarch64__
	__asm__ volatile("msr daifset, #2" : : : "memory");
#else
	__asm__ volatile("cli" : : : "memory");
#endif
}

static void interrupts_enable(void)
{
#ifdef __aarch64__
	__asm__ volatile("msr daifclr, #2" : : : "memory");
#else
	__asm__ volatile("sti" : : : "memory");
#endif
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
	int max_priority = -1;
	struct task *best_task = 0;

	// Find the highest priority among ready tasks
	for (usize offset = 1; offset <= MAX_TASKS; offset++) {
		usize index = (start + offset) % MAX_TASKS;

		if (tasks[index].state == TASK_READY) {
			if (tasks[index].priority > max_priority) {
				max_priority = tasks[index].priority;
				best_task = &tasks[index];
			}
		}
	}

	return best_task;
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
	scheduler_exit_current(0);
}

void scheduler_init(void)
{
	memset(tasks, 0, sizeof(tasks));

	struct task *boot = &tasks[0];
	boot->id = next_task_id++;
	boot->name = "boot";
	boot->state = TASK_RUNNING;
	boot->stdout_fd = -1;
	boot->priority = 1;
	boot->parent_id = 0;
	current_task = boot;
	scheduler_started = 1;

	console_write("sched: initialized\n");
}

static void task_init_cred(struct task *task)
{
	if (task->id == 1) {
		/* Boot task gets root credentials */
		task->cred = cred_create_default();
	} else {
		/* Inherit credentials from parent */
		struct task *parent = 0;
		for (usize i = 0; i < MAX_TASKS; i++) {
			if (tasks[i].id == task->parent_id) {
				parent = &tasks[i];
				break;
			}
		}
		task->cred = cred_dup(parent ? parent->cred : 0);
	}
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
#ifdef __aarch64__
	task->context.fp = 0;
	task->context.lr = initial_rsp; // Use lr for entry point on AArch64 trampoline
	task->context.sp = initial_rsp;
	task->context.x19 = 0;
	task->context.x20 = 0;
	task->context.x21 = 0;
	task->context.x22 = 0;
	task->context.x23 = 0;
	task->context.x24 = 0;
	task->context.x25 = 0;
	task->context.x26 = 0;
	task->context.x27 = 0;
	task->context.x28 = 0;
#else
	task->context.rsp = initial_rsp;
	task->context.rbp = 0;
	task->context.rbx = 0;
	task->context.r12 = 0;
	task->context.r13 = 0;
	task->context.r14 = 0;
	task->context.r15 = 0;
#endif
	task->stdout_fd = current_task ? current_task->stdout_fd : -1;
	task->priority = 1;
	task->parent_id = current_task ? current_task->id : 0;
	task->exit_code = 0;
	task->pending_signals = 0;
	task->blocked_signals = 0;
	memset(task->sigactions, 0, sizeof(task->sigactions));

	task_init_cred(task);

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

	/* Deliver pending signals for current task */
	if (current_task) {
		scheduler_deliver_pending_signals();
	}

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

void scheduler_exit_current(int exit_code)
{
	interrupts_disable();

	if (current_task == 0) {
		panic("scheduler_exit_current without current task");
	}

	console_write("sched: task exited ");
	console_write(current_task->name);
	console_write("\n");

	/* Free credentials */
	if (current_task->cred) {
		cred_free(current_task->cred);
		current_task->cred = 0;
	}

	current_task->exit_code = exit_code;
	current_task->state = TASK_DEAD;
	
	// Wake up parent if it is blocked waiting for us
	for (usize i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].id == current_task->parent_id && tasks[i].state == TASK_BLOCKED) {
			tasks[i].state = TASK_READY;
		}
	}

	scheduler_yield();
	panic("dead task resumed");
}

int scheduler_wait(usize pid, int *status)
{
	if (current_task == 0) return -1;
	
	while (1) {
		interrupts_disable();
		int has_children = 0;
		for (usize i = 0; i < MAX_TASKS; i++) {
			if (tasks[i].state != TASK_UNUSED && tasks[i].parent_id == current_task->id) {
				if (pid == 0 || tasks[i].id == pid) {
					has_children = 1;
					if (tasks[i].state == TASK_DEAD) {
						int code = tasks[i].exit_code;
						int child_id = tasks[i].id;
						tasks[i].state = TASK_UNUSED;
						interrupts_enable();
						if (status) *status = code;
						return child_id;
					}
				}
			}
		}
		
		if (!has_children) {
			interrupts_enable();
			return -1;
		}
		
		current_task->state = TASK_BLOCKED;
		scheduler_yield();
		interrupts_enable();
	}
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

/* ── Signal Delivery ── */

int scheduler_kill(usize task_id, int sig)
{
	if (sig < 1 || sig >= NSIG) return -1;

	interrupts_disable();
	for (usize i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].id == task_id && tasks[i].state != TASK_UNUSED) {
			/* SIGKILL and SIGSTOP cannot be blocked/ignored */
			tasks[i].pending_signals |= (1ULL << sig);

			/* Wake blocked task so it can handle signal */
			if (tasks[i].state == TASK_BLOCKED) {
				tasks[i].state = TASK_READY;
			}
			interrupts_enable();
			return 0;
		}
	}
	interrupts_enable();
	return -1;
}

int scheduler_sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
	if (sig < 1 || sig >= NSIG) return -1;
	/* SIGKILL and SIGSTOP cannot be caught/ignored */
	if (sig == SIGKILL || sig == SIGSTOP) return -1;
	if (!current_task) return -1;

	interrupts_disable();
	if (old) {
		*old = current_task->sigactions[sig];
	}
	if (act) {
		current_task->sigactions[sig] = *act;
		/* Remove SA_NODEFER: block the signal by default */
		if (!(act->sa_flags & SA_NODEFER)) {
			current_task->blocked_signals &= ~(1ULL << sig);
		}
	}
	interrupts_enable();
	return 0;
}

sighandler_t scheduler_get_sighandler(int sig)
{
	if (!current_task || sig < 1 || sig >= NSIG) return SIG_DFL;
	return current_task->sigactions[sig].sa_handler;
}

usize scheduler_get_pid(void)
{
	if (!current_task) return 0;
	return current_task->id;
}

struct cred *scheduler_get_current_cred(void)
{
	if (!current_task) return 0;
	return current_task->cred;
}

/* Called before returning to userspace — delivers pending signals */
void scheduler_deliver_pending_signals(void)
{
	if (!current_task) return;

	u64 pending = current_task->pending_signals;
	u64 blocked = current_task->blocked_signals;
	u64 deliverable = pending & ~blocked;

	if (deliverable == 0) return;

	/* Find highest-priority signal (lowest number = highest priority) */
	for (int sig = 1; sig < NSIG; sig++) {
		if (!(deliverable & (1ULL << sig))) continue;

		sighandler_t handler = current_task->sigactions[sig].sa_handler;

		if (sig == SIGKILL) {
			/* SIGKILL — terminate immediately */
			interrupts_disable();
			current_task->exit_code = 128 + SIGKILL;
			current_task->state = TASK_DEAD;
			scheduler_yield();
			/* unreachable */
		}

		if (handler == SIG_IGN) {
			/* Ignored — just clear */
			current_task->pending_signals &= ~(1ULL << sig);
			continue;
		}

		if (handler == SIG_DFL) {
			/* Default actions */
			switch (sig) {
			case SIGINT:
			case SIGTERM:
			case SIGQUIT:
			case SIGPIPE:
			case SIGSEGV:
			case SIGBUS:
			case SIGFPE:
			case SIGILL:
			case SIGABRT:
			case SIGSYS:
			case SIGTRAP:
			case SIGXCPU:
			case SIGXFSZ:
			case SIGVTALRM:
			case SIGPROF:
				/* Terminate */
				interrupts_disable();
				current_task->exit_code = 128 + sig;
				current_task->state = TASK_DEAD;
				scheduler_yield();
				/* unreachable */
			case SIGCONT:
				current_task->pending_signals &= ~(1ULL << sig);
				current_task->state = TASK_READY;
				continue;
			case SIGSTOP:
			case SIGTSTP:
			case SIGTTIN:
			case SIGTTOU:
				current_task->state = TASK_BLOCKED;
				scheduler_yield();
				continue;
			case SIGCHLD:
			case SIGURG:
			case SIGWINCH:
			default:
				/* Ignore by default */
				current_task->pending_signals &= ~(1ULL << sig);
				continue;
			}
		}

		/* Custom handler — for now, just clear pending and log */
		/* In a full implementation we'd set up a signal frame on user stack */
		current_task->pending_signals &= ~(1ULL << sig);

		/* If SA_RESETHAND, reset to SIG_DFL after delivery */
		if (current_task->sigactions[sig].sa_flags & SA_RESETHAND) {
			current_task->sigactions[sig].sa_handler = SIG_DFL;
		}
	}
}
