// SPDX-License-Identifier: GPL-2.0-only
/*
 * The scheduler-facing half of what Linux's KVM expects (M131), and its
 * start-up.
 *
 * Preempt notifiers: a vCPU thread registers one while it holds its vCPU
 * loaded (vcpu_load), and it is told when it leaves a CPU and when it comes
 * back, so the VMCS follows it. b1nix calls the hooks from its context switch
 * (kvm_hooks.h); the notifiers are kept per b1nix task.
 *
 * User-return notifiers: KVM leaves some of a guest's MSRs (the SYSCALL
 * ones, TSC_AUX) in the CPU after a VM exit and restores the host's only when
 * the CPU next returns to user mode. b1nix calls the hook, with interrupts
 * off, on every path to ring 3 of a CPU that asked for it.
 *
 * And the task-level odds KVM calls: the signal mask for KVM_SET_SIGNAL_MASK,
 * waiting and waking, its worker thread, SRCU callbacks.
 */
#include <linux/eventfd.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/signal.h>
#include <linux/rcuwait.h>
#include <linux/srcu.h>
#include <linux/anon_inodes.h>
#include <linux/sched/vhost_task.h>
#include <linux/user-return-notifier.h>
#include <linux/fs.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <b1nix/kvm_bridge.h>
#include <lkpi/env.h>

/* ── preempt notifiers ────────────────────────────────────────────────── */

#define PN_BUCKETS 64

struct pn_task {
	struct hlist_node node;
	void *task;                  /* the b1nix task */
	struct hlist_head notifiers;
};

static struct hlist_head pn_table[PN_BUCKETS];
static DEFINE_SPINLOCK(pn_lock);

static unsigned int pn_hash(void *task)
{
	return (unsigned int)(((unsigned long)task >> 6) % PN_BUCKETS);
}

static struct pn_task *pn_find(void *task)
{
	struct pn_task *p;

	hlist_for_each_entry(p, &pn_table[pn_hash(task)], node)
		if (p->task == task)
			return p;
	return NULL;
}

void preempt_notifier_register(struct preempt_notifier *n)
{
	void *task = b1nix_kvm_current_task();
	struct pn_task *p, *fresh = kzalloc(sizeof(*fresh), GFP_KERNEL);
	unsigned long flags;

	spin_lock_irqsave(&pn_lock, flags);
	p = pn_find(task);
	if (!p && fresh) {
		p = fresh;
		fresh = NULL;
		p->task = task;
		INIT_HLIST_HEAD(&p->notifiers);
		hlist_add_head(&p->node, &pn_table[pn_hash(task)]);
	}
	if (p)
		hlist_add_head(&n->link, &p->notifiers);
	spin_unlock_irqrestore(&pn_lock, flags);
	kfree(fresh);
}

void preempt_notifier_unregister(struct preempt_notifier *n)
{
	void *task = b1nix_kvm_current_task();
	struct pn_task *p, *gone = NULL;
	unsigned long flags;

	spin_lock_irqsave(&pn_lock, flags);
	hlist_del_init(&n->link);
	p = pn_find(task);
	if (p && hlist_empty(&p->notifiers)) {
		hlist_del_init(&p->node);
		gone = p;
	}
	spin_unlock_irqrestore(&pn_lock, flags);
	kfree(gone);
}

/* Both run inside b1nix's context switch, interrupts off. The task's list
 * is looked up under the lock and walked without it: only the task itself
 * changes its own list, and it is not running while it is being switched. */
static struct pn_task *pn_lookup(void *task)
{
	struct pn_task *p;
	unsigned long flags;

	spin_lock_irqsave(&pn_lock, flags);
	p = pn_find(task);
	spin_unlock_irqrestore(&pn_lock, flags);
	return p;
}

static void kvm_sched_out_hook(void *prev, void *next)
{
	struct preempt_notifier *n;
	struct pn_task *p = pn_lookup(prev);

	(void)next;
	if (!p)
		return;
	lkpi_irq_enable_suppress(1);
	hlist_for_each_entry(n, &p->notifiers, link)
		n->ops->sched_out(n, NULL);
	lkpi_irq_enable_suppress(0);
}

static void kvm_sched_in_hook(void *task, int cpu)
{
	struct preempt_notifier *n;
	struct pn_task *p = pn_lookup(task);

	if (!p)
		return;
	lkpi_irq_enable_suppress(1);
	hlist_for_each_entry(n, &p->notifiers, link)
		n->ops->sched_in(n, cpu);
	lkpi_irq_enable_suppress(0);
}

/* ── user-return notifiers ────────────────────────────────────────────── */

static struct hlist_head urn_list[NR_CPUS];

/* Called with preemption off, on the CPU whose MSRs need restoring. */
void user_return_notifier_register(struct user_return_notifier *urn)
{
	int cpu = b1nix_kvm_this_cpu();

	if (hlist_unhashed(&urn->link))
		hlist_add_head(&urn->link, &urn_list[cpu]);
	b1nix_kvm_arm_user_return();
}

void user_return_notifier_unregister(struct user_return_notifier *urn)
{
	hlist_del_init(&urn->link);
}

static void kvm_return_to_user_hook(void)
{
	struct user_return_notifier *urn;
	struct hlist_node *tmp;

	hlist_for_each_entry_safe(urn, tmp, &urn_list[b1nix_kvm_this_cpu()], link)
		urn->on_user_return(urn);
}

/* ── the task, as KVM asks about it ───────────────────────────────────── */

int sigprocmask(int how, sigset_t *set, sigset_t *oldset)
{
	u64 cur = b1nix_kvm_sigmask_get();

	if (oldset)
		oldset->sig[0] = cur;
	if (!set)
		return 0;
	switch (how) {
	case SIG_BLOCK:   cur |= set->sig[0]; break;
	case SIG_UNBLOCK: cur &= ~set->sig[0]; break;
	case SIG_SETMASK: cur = set->sig[0]; break;
	default: return -EINVAL;
	}
	b1nix_kvm_sigmask_set(cur);
	return 0;
}

/* Machine-check signals for poisoned guest memory: b1nix does no memory
 * error recovery, so nothing ever asks for one. */
int send_sig_mceerr(int code, void __user *addr, short lsb, struct task_struct *t)
{
	(void)code; (void)addr; (void)lsb; (void)t;
	return -ENOSYS;
}

bool single_task_running(void)
{
	return !b1nix_kvm_other_work_pending();
}

bool task_is_runnable(struct task_struct *t)
{
	(void)t;   /* KVM asks only about current */
	return b1nix_kvm_current_runnable() != 0;
}

/* Directed yield to another vCPU's thread: b1nix's scheduler has no way to
 * hand the CPU to a chosen task, so the spinning vCPU just yields. */
int yield_to(struct task_struct *p, bool preempt)
{
	(void)p; (void)preempt;
	return 0;
}

struct task_struct *get_pid_task(struct pid *pid, int type)
{
	(void)pid; (void)type;
	return NULL;
}

void put_task_struct(struct task_struct *t)
{
	(void)t;
}

int rcuwait_wake_up(struct rcuwait *w)
{
	struct task_struct *t = READ_ONCE(w->task);

	if (!t)
		return 0;
	return wake_up_process(t);
}

/* ── the worker KVM starts per VM (vhost_task) ────────────────────────── */

struct vhost_task {
	bool (*fn)(void *data);
	void (*handle_sigkill)(void *data);
	void *data;
	struct task_struct *task;
	struct completion exited;
	volatile int stop;
	volatile int woken;
};

static int vhost_task_main(void *arg)
{
	struct vhost_task *v = arg;

	for (;;) {
		bool more;

		if (READ_ONCE(v->stop))
			break;
		WRITE_ONCE(v->woken, 0);
		more = v->fn(v->data);
		if (!more) {
			set_current_state(TASK_INTERRUPTIBLE);
			if (!READ_ONCE(v->woken) && !READ_ONCE(v->stop))
				schedule();
			__set_current_state(TASK_RUNNING);
		}
	}
	complete(&v->exited);
	return 0;
}

struct vhost_task *vhost_task_create(bool (*fn)(void *),
				     void (*handle_sigkill)(void *), void *arg,
				     const char *name)
{
	struct vhost_task *v = kzalloc(sizeof(*v), GFP_KERNEL);

	if (!v)
		return ERR_PTR(-ENOMEM);
	v->fn = fn;
	v->handle_sigkill = handle_sigkill;
	v->data = arg;
	init_completion(&v->exited);
	v->task = kthread_create(vhost_task_main, v, "%s", name);
	if (IS_ERR(v->task)) {
		long err = PTR_ERR(v->task);

		kfree(v);
		return ERR_PTR(err);
	}
	return v;
}

void vhost_task_start(struct vhost_task *v)
{
	wake_up_process(v->task);
}

void vhost_task_wake(struct vhost_task *v)
{
	WRITE_ONCE(v->woken, 1);
	wake_up_process(v->task);
}

void vhost_task_stop(struct vhost_task *v)
{
	WRITE_ONCE(v->stop, 1);
	wake_up_process(v->task);
	wait_for_completion(&v->exited);
	kfree(v);
}

bool vhost_task_should_stop(struct vhost_task *v)
{
	return READ_ONCE(v->stop) != 0;
}

/* ── SRCU callbacks: run once a grace period has passed ───────────────── */

void call_srcu(struct srcu_struct *ssp, struct rcu_head *head,
	       void (*func)(struct rcu_head *head))
{
	synchronize_srcu(ssp);
	func(head);
}

void srcu_barrier(struct srcu_struct *ssp)
{
	(void)ssp;   /* call_srcu completes before it returns */
}

/* ── files ────────────────────────────────────────────────────────────── */

struct file *anon_inode_getfile_fmode(const char *name,
				      const struct file_operations *fops,
				      void *priv, int flags, fmode_t f_mode)
{
	struct file *f = anon_inode_getfile(name, fops, priv, flags);

	if (!IS_ERR(f))
		f->f_mode |= f_mode;
	return f;
}

void lkpi_fd_install_owned_file(unsigned int fd, struct file *f);

void kvm_fd_install(unsigned int fd, struct file *f)
{
	lkpi_fd_install_owned_file(fd, f);
}

int kvm_anon_inode_getfd(const char *name, const struct file_operations *fops,
			 void *priv, int flags)
{
	struct file *f;
	int fd = get_unused_fd_flags(flags);

	if (fd < 0)
		return fd;
	f = anon_inode_getfile(name, fops, priv, flags);
	if (IS_ERR(f)) {
		put_unused_fd(fd);
		return PTR_ERR(f);
	}
	kvm_fd_install(fd, f);
	return fd;
}

__poll_t kvm_vfs_poll(struct file *file, poll_table *pt)
{
	/* A process eventfd (KVM_IRQFD): the poll table's callback is how the
	 * irqfd hangs itself on the eventfd's wait queue. */
	if (lkpi_file_is_eventfd(file)) {
		struct eventfd_ctx *ctx = file->private_data;

		if (pt && pt->_qproc)
			pt->_qproc(file, &ctx->wqh, pt);
		return (lkpi_eventfd_readable(ctx->handle) ? EPOLLIN : 0) |
		       (lkpi_eventfd_writable(ctx->handle) ? EPOLLOUT : 0);
	}
	if (!file->f_op || !file->f_op->poll)
		return EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM;
	return file->f_op->poll(file, pt);
}

/* One priority waiter per queue, ahead of the rest: the irqfd, which is the
 * only thing that may consume an eventfd KVM injects from. */
int add_wait_queue_priority_exclusive(wait_queue_head_t *wq_head,
				      struct wait_queue_entry *wq_entry)
{
	struct list_head *first;
	int ret = 0;

	lkpi_spin_lock(&wq_head->lock);
	first = wq_head->head.next;
	if (first != &wq_head->head &&
	    (container_of(first, struct wait_queue_entry, entry)->flags & WQ_FLAG_PRIORITY)) {
		ret = -EBUSY;
	} else {
		wq_entry->flags |= WQ_FLAG_EXCLUSIVE | WQ_FLAG_PRIORITY;
		list_add(&wq_entry->entry, &wq_head->head);
	}
	lkpi_spin_unlock(&wq_head->lock);
	return ret;
}

/* ── start-up ─────────────────────────────────────────────────────────── */

void kvm_mm_hooks_fill(struct b1nix_kvm_hooks *h);
void kvm_cpuhp_all_down(void);
void kvm_cpuhp_all_up(void);
/* The architecture's half (kvm_x86_cpu.c, kvm_arm64_init.c): what KVM reads
 * of the CPU before anything else, and the arch module init itself. */
int kvm_arch_lkpi_early(void);
int kvm_arch_lkpi_start(void);

static struct b1nix_kvm_hooks kvm_hooks;

/* Bring KVM up: per-CPU storage, the CPU description it reads, the kernel
 * hooks, then the architecture's KVM in upstream's module order. */
int kvm_lkpi_init(void)
{
	int r;

	r = kvm_percpu_init();
	if (r)
		return r;
	r = kvm_arch_lkpi_early();
	if (r)
		return r;
	kvm_hooks.sched_out = kvm_sched_out_hook;
	kvm_hooks.sched_in = kvm_sched_in_hook;
	kvm_hooks.return_to_user = kvm_return_to_user_hook;
	kvm_mm_hooks_fill(&kvm_hooks);
	kvm_hooks.cpus_down = kvm_cpuhp_all_down;
	kvm_hooks.cpus_up = kvm_cpuhp_all_up;
	b1nix_kvm_set_hooks(&kvm_hooks);
	return kvm_arch_lkpi_start();
}
