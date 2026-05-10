#include <string.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/uidgid.h>
#include <b1nix/user.h>

#define MAX_TASKS 16
#define KERNEL_STACK_SIZE (16 * 1024)
#define TASK_ENV_MAX 16
#define TASK_ENV_VALUE_MAX 64

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
	u64 kernel_stack_ptr;
	u64 saved_user_rsp;
	u64 wake_tick;
	void *wait_chan;
	int stdout_fd;
	int fd_table[SCHED_MAX_FDS];
	int fd_flags[SCHED_MAX_FDS];
	int priority;
	int exit_code;
	usize parent_id;
	char cwd[64];
	u64 user_brk;
	u16 umask;
	usize process_group_id;
	usize session_id;
	char env[TASK_ENV_MAX][TASK_ENV_VALUE_MAX];

	/* Signal handling */
	u64 pending_signals;       /* bitmask of pending signals */
	u64 blocked_signals;       /* bitmask of blocked signals */
	struct sigaction sigactions[NSIG];  /* signal actions */

	/* Credentials */
	struct cred *cred;

	/* Userspace Image */
	void *user_image;
};

extern void arch_context_switch(struct cpu_context *old_context, struct cpu_context *new_context);
extern void vfs_handle_retain(int handle);
extern void vfs_handle_release(int handle);
extern char x86_syscall_stack_top[];

static struct task tasks[MAX_TASKS];
struct task *current_task;
static usize next_task_id = 1;
static volatile u64 scheduler_ticks;
static int scheduler_started;
static void task_init_cred(struct task *task);

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
	for (usize i = 0; i < SCHED_MAX_FDS; i++) {
		boot->fd_table[i] = -1;
		boot->fd_flags[i] = 0;
	}
	boot->priority = 1;
	boot->parent_id = 0;
	boot->cwd[0] = '/';
	boot->cwd[1] = '\0';
	boot->user_brk = 0;
	boot->umask = 022;
	boot->process_group_id = boot->id;
	boot->session_id = boot->id;
	boot->kernel_stack_ptr = (u64)(usize)x86_syscall_stack_top;
	task_init_cred(boot);
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
	task->kernel_stack_ptr = stack_top;
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
	for (usize i = 0; i < SCHED_MAX_FDS; i++) {
		if (current_task) {
			task->fd_table[i] = current_task->fd_table[i];
			task->fd_flags[i] = current_task->fd_flags[i];
			if (task->fd_table[i] >= 0) {
				vfs_handle_retain(task->fd_table[i]);
			}
		} else {
			task->fd_table[i] = -1;
			task->fd_flags[i] = 0;
		}
	}
	task->priority = 1;
	task->parent_id = current_task ? current_task->id : 0;
	if (current_task) {
		memcpy(task->cwd, current_task->cwd, sizeof(task->cwd));
		task->cwd[sizeof(task->cwd) - 1] = '\0';
		task->user_brk = current_task->user_brk;
		task->umask = current_task->umask;
		task->process_group_id = current_task->process_group_id;
		task->session_id = current_task->session_id;
		memcpy(task->env, current_task->env, sizeof(task->env));
	} else {
		task->cwd[0] = '/';
		task->cwd[1] = '\0';
		task->user_brk = 0;
		task->umask = 022;
		task->process_group_id = task->id;
		task->session_id = task->id;
		memset(task->env, 0, sizeof(task->env));
	}
	task->exit_code = 0;
	task->pending_signals = 0;
	task->blocked_signals = 0;
	memset(task->sigactions, 0, sizeof(task->sigactions));

	task_init_cred(task);

	return (int)task->id;
}

int scheduler_fork_current(void)
{
	if (!current_task || current_task->entry == 0) {
		return -1;
	}

	return kthread_create(current_task->name, current_task->entry, current_task->arg);
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
			tasks[i].wait_chan = 0;
			break;
		}
	}

	interrupts_enable();
}

void scheduler_block_on(void *chan)
{
	interrupts_disable();

	if (current_task == 0 || current_task->state != TASK_RUNNING) {
		panic("scheduler_block_on without running task");
	}

	current_task->wait_chan = chan;
	current_task->state = TASK_BLOCKED;
	scheduler_yield();
	interrupts_enable();
}

void scheduler_wake_all(void *chan)
{
	interrupts_disable();

	for (usize i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_chan == chan) {
			tasks[i].state = TASK_READY;
			tasks[i].wait_chan = 0;
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

u64 scheduler_get_uptime_ticks(void)
{
	return scheduler_ticks;
}

void scheduler_exit_current(int exit_code)
{
	interrupts_disable();

	if (current_task == 0) {
		panic("scheduler_exit_current without current task");
	}

	/* Free credentials */
	if (current_task->cred) {
		cred_free(current_task->cred);
		current_task->cred = 0;
	}

	for (usize i = 0; i < SCHED_MAX_FDS; i++) {
		if (current_task->fd_table[i] >= 0) {
			vfs_handle_release(current_task->fd_table[i]);
			current_task->fd_table[i] = -1;
			current_task->fd_flags[i] = 0;
		}
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
	return scheduler_waitpid(pid, status, 0);
}

int scheduler_waitpid(usize pid, int *status, int options)
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
						if (tasks[i].user_image) {
							user_image_free(tasks[i].user_image);
							tasks[i].user_image = 0;
						}
						kfree(tasks[i].stack);
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

		if (options & B1NIX_WNOHANG) {
			interrupts_enable();
			return 0;
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

void scheduler_fd_table_init_current(void)
{
	if (!current_task) return;
	for (usize i = 0; i < SCHED_MAX_FDS; i++) {
		current_task->fd_table[i] = -1;
		current_task->fd_flags[i] = 0;
	}
}

int scheduler_fd_alloc(int handle)
{
	if (!current_task || handle < 0) return -1;
	for (usize i = 0; i < SCHED_MAX_FDS; i++) {
		if (current_task->fd_table[i] < 0) {
			current_task->fd_table[i] = handle;
			current_task->fd_flags[i] = 0;
			return (int)i;
		}
	}
	return -1;
}

int scheduler_fd_get(int fd)
{
	if (!current_task || fd < 0 || (usize)fd >= SCHED_MAX_FDS) return -1;
	return current_task->fd_table[fd];
}

int scheduler_fd_set(int fd, int handle)
{
	if (!current_task || fd < 0 || (usize)fd >= SCHED_MAX_FDS) return -1;
	current_task->fd_table[fd] = handle;
	current_task->fd_flags[fd] = 0;
	return fd;
}

int scheduler_fd_close(int fd)
{
	if (!current_task || fd < 0 || (usize)fd >= SCHED_MAX_FDS) return -1;
	current_task->fd_table[fd] = -1;
	current_task->fd_flags[fd] = 0;
	return 0;
}

int scheduler_fd_flags_get(int fd)
{
	if (!current_task || fd < 0 || (usize)fd >= SCHED_MAX_FDS) return -1;
	if (current_task->fd_table[fd] < 0) return -1;
	return current_task->fd_flags[fd];
}

int scheduler_fd_flags_set(int fd, int flags)
{
	if (!current_task || fd < 0 || (usize)fd >= SCHED_MAX_FDS) return -1;
	if (current_task->fd_table[fd] < 0) return -1;
	current_task->fd_flags[fd] = flags;
	return 0;
}

void scheduler_fd_close_on_exec(void)
{
	if (!current_task) return;
	for (usize i = 0; i < SCHED_MAX_FDS; i++) {
		if ((current_task->fd_flags[i] & B1NIX_FD_CLOEXEC) != 0) {
			current_task->fd_table[i] = -1;
			current_task->fd_flags[i] = 0;
		}
	}
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

void scheduler_set_user_image(void *image)
{
	if (current_task) {
		current_task->user_image = image;
	}
}

const char *scheduler_get_cwd(void)
{
	if (!current_task) return "/";
	return current_task->cwd;
}

int scheduler_set_cwd(const char *path)
{
	if (!current_task || !path || path[0] == '\0') return -1;
	usize len = strlen(path);
	if (len >= sizeof(current_task->cwd)) return -1;
	memcpy(current_task->cwd, path, len + 1);
	return 0;
}

u64 scheduler_brk_get(void)
{
	if (!current_task) return 0;
	return current_task->user_brk;
}

u64 scheduler_brk_set(u64 new_brk)
{
	if (!current_task) return 0;
	if (current_task->user_brk == 0) {
		current_task->user_brk = (u64)(usize)kmalloc(PAGE_SIZE);
	}
	if (new_brk != 0) {
		current_task->user_brk = new_brk;
	}
	return current_task->user_brk;
}

/* ── Priority ── */

int scheduler_set_priority(usize pid, int priority)
{
	if (priority < -20 || priority > 19) return -1;
	interrupts_disable();
	for (usize i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].state != TASK_UNUSED && tasks[i].id == pid) {
			tasks[i].priority = 10 - priority; /* nice → internal (higher = better) */
			interrupts_enable();
			return 0;
		}
	}
	interrupts_enable();
	return -1;
}

int scheduler_get_priority(usize pid)
{
	interrupts_disable();
	for (usize i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].state != TASK_UNUSED && tasks[i].id == pid) {
			int p = 10 - tasks[i].priority; /* internal → nice */
			interrupts_enable();
			return p;
		}
	}
	interrupts_enable();
	return -1;
}

/* ── Session / Process Group ── */

usize scheduler_setsid(void)
{
	if (!current_task) return (usize)-1;
	interrupts_disable();
	/* A process cannot be a process group leader to call setsid */
	if (current_task->process_group_id == current_task->id) {
		interrupts_enable();
		return (usize)-1;
	}
	current_task->session_id = current_task->id;
	current_task->process_group_id = current_task->id;
	interrupts_enable();
	return current_task->session_id;
}

usize scheduler_getpgrp(void)
{
	if (!current_task) return 0;
	return current_task->process_group_id;
}

int scheduler_setpgrp(usize pid, usize pgrp)
{
	if (pid == 0) pid = current_task ? current_task->id : 0;
	if (pgrp == 0) pgrp = pid;
	interrupts_disable();
	for (usize i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].state != TASK_UNUSED && tasks[i].id == pid) {
			tasks[i].process_group_id = pgrp;
			interrupts_enable();
			return 0;
		}
	}
	interrupts_enable();
	return -1;
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
