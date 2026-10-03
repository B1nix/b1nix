/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The Linux kernel API KVM uses beyond what the shared lkpi headers provide
 * (M131). Force-included after kvm_prelude.h on KVM's sources only; each
 * function declared here is implemented in kernel/lkpi/kvm_*.c against
 * b1nix's own scheduler, memory manager and SMP code.
 */
#ifndef KVM_SHIM_LINUX_EXTRA_H
#define KVM_SHIM_LINUX_EXTRA_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/seqlock.h>
#include <linux/srcu.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/debugfs.h>
#include <linux/atomic.h>
#include <linux/msi.h>
#include <linux/wait.h>
#include <linux/sched/signal.h>
#include <b1nix/kvm_bridge.h>

/* KVM runs in the ioctls of a user task, which signals do reach: a VMM
 * kicks its vCPU threads out of KVM_RUN with one, and a vCPU halted in the
 * guest must wake for it. The shared lkpi answer ("never", right for the
 * kernel threads that run the imported drivers) is replaced for KVM's
 * sources by the task's real pending set. */
#define signal_pending(t)       ((void)(t), b1nix_kvm_signal_pending())
#define fatal_signal_pending(t) ((void)(t), b1nix_kvm_fatal_signal_pending())
#define signal_pending_state(state, t) \
	(((state) & TASK_INTERRUPTIBLE) ? signal_pending(t) : 0)

/* ── compiler and tracing leftovers ───────────────────────────────────── */
#ifndef __visible
#define __visible __attribute__((__externally_visible__))
#endif
#define CALLER_ADDR0 ((unsigned long)__builtin_return_address(0))
#define lockdep_assert_irqs_disabled() do { } while (0)
#define lockdep_assert_irqs_enabled()  do { } while (0)
#define lockdep_assert_preemption_disabled() do { } while (0)
#define trace_hardirqs_off_finish()    do { } while (0)
#define trace_hardirqs_on_prepare()    do { } while (0)

/* No per-task virtual CPU time accounting and no RCU extended quiescent
 * states: a guest's time is the task's time. */
static inline void vtime_account_guest_enter(void) { }
static inline void vtime_account_guest_exit(void) { }
static inline void rcu_virt_note_context_switch(void) { }

/* ── sequence counts ──────────────────────────────────────────────────── */
typedef seqcount_t seqcount_raw_spinlock_t;
#define seqcount_raw_spinlock_init(s, lock) seqcount_init(s)
#define raw_write_seqcount_begin(s) write_seqcount_begin(s)
#define raw_write_seqcount_end(s)   write_seqcount_end(s)
#define raw_read_seqcount_begin(s)  read_seqcount_begin(s)

/* ── mutexes ──────────────────────────────────────────────────────────── */
/* -EINTR when a fatal signal arrives while waiting; lkpi's mutex waits
 * interruptibly, which covers the fatal ones. */
#define mutex_lock_killable(m)               mutex_lock_interruptible(m)
#define mutex_lock_killable_nest_lock(m, n)  ({ (void)(n); mutex_lock_killable(m); })
#define mutex_trylock_nest_lock(m, n)        ({ (void)(n); mutex_trylock(m); })

/* ── SRCU ─────────────────────────────────────────────────────────────── */
void call_srcu(struct srcu_struct *ssp, struct rcu_head *head,
	       void (*func)(struct rcu_head *head));
void srcu_barrier(struct srcu_struct *ssp);
#define smp_mb__after_srcu_read_lock()   smp_mb()
#define smp_mb__after_srcu_read_unlock() smp_mb()

/* ── CPU masks and hotplug ────────────────────────────────────────────── */
typedef struct cpumask *cpumask_var_t;
bool alloc_cpumask_var(cpumask_var_t *mask, gfp_t flags);
bool zalloc_cpumask_var(cpumask_var_t *mask, gfp_t flags);
#define alloc_cpumask_var_node(m, f, n) ({ (void)(n); alloc_cpumask_var(m, f); })
#define zalloc_cpumask_var_node(m, f, n) ({ (void)(n); zalloc_cpumask_var(m, f); })
void free_cpumask_var(cpumask_var_t mask);
#define this_cpu_cpumask_var_ptr(x) this_cpu_read(x)
#define cpu_to_node(cpu) ((void)(cpu), 0)
bool cpu_online(unsigned int cpu);
#include <linux/cpu.h>
static inline void cpumask_clear(struct cpumask *m) { memset(m, 0, sizeof(*m)); }
static inline bool cpumask_empty(const struct cpumask *m)
{
	for (unsigned int i = 0; i < sizeof(m->bits) / sizeof(m->bits[0]); i++)
		if (m->bits[i])
			return false;
	return true;
}
static inline void __cpumask_set_cpu(unsigned int cpu, struct cpumask *m)
{
	m->bits[cpu / 64] |= 1UL << (cpu % 64);
}

#define cpuhp_remove_state_nocalls(s) cpuhp_remove_state(s)

/* ── cross-CPU calls ──────────────────────────────────────────────────── */
typedef void (*smp_call_func_t)(void *info);
int smp_call_function_single(int cpu, smp_call_func_t func, void *info, int wait);
void smp_call_function_many(const struct cpumask *mask, smp_call_func_t func,
			    void *info, bool wait);
void smp_send_reschedule(int cpu);

/* ── tasks ────────────────────────────────────────────────────────────── */
struct pid;
struct task_struct *get_pid_task(struct pid *pid, int type);
void put_task_struct(struct task_struct *t);
bool task_is_runnable(struct task_struct *t);
int yield_to(struct task_struct *p, bool preempt);
#define is_compat_task() 0
#define PIDTYPE_PID 0

/* ── preempt notifiers: KVM's vcpu_load/put on every context switch ───── */
struct preempt_notifier;
struct preempt_ops {
	void (*sched_in)(struct preempt_notifier *notifier, int cpu);
	void (*sched_out)(struct preempt_notifier *notifier, struct task_struct *next);
};
struct preempt_notifier {
	struct hlist_node link;
	struct preempt_ops *ops;
};
static inline void preempt_notifier_init(struct preempt_notifier *n,
					 struct preempt_ops *ops)
{
	INIT_HLIST_NODE(&n->link);
	n->ops = ops;
}
void preempt_notifier_register(struct preempt_notifier *n);
void preempt_notifier_unregister(struct preempt_notifier *n);
static inline void preempt_notifier_inc(void) { }
static inline void preempt_notifier_dec(void) { }

/* ── memory: the VM owner's address space ─────────────────────────────── */
#ifndef PAGE_OFFSET
#define PAGE_OFFSET ((unsigned long)DIRECT_MAP_BASE_LKPI)
#endif
unsigned long lkpi_direct_map_base(void);
#define DIRECT_MAP_BASE_LKPI lkpi_direct_map_base()
#define __va(pa) ((void *)((unsigned long)(pa) + PAGE_OFFSET))
#define untagged_addr(addr) (addr)
bool pfn_valid(unsigned long pfn);

#define FOLL_WRITE            0x01
#define FOLL_GET              0x04
#define FOLL_NOWAIT           0x20
#define FOLL_HWPOISON         0x100
#define FOLL_LONGTERM         0x400
#define FOLL_PIN              0x1000
#define FOLL_HONOR_NUMA_FAULT 0x2000
#define FAULT_FLAG_WRITE      0x01
#define FAULT_FLAG_REMOTE     0x80

long get_user_pages_unlocked(unsigned long start, unsigned long nr_pages,
			     struct page **pages, unsigned int gup_flags);
int get_user_page_fast_only(unsigned long addr, unsigned int gup_flags,
			    struct page **pagep);
int get_user_pages_fast_only(unsigned long start, int nr_pages,
			     unsigned int gup_flags, struct page **pages);
long pin_user_pages_unlocked(unsigned long start, unsigned long nr_pages,
			     struct page **pages, unsigned int gup_flags);
int pin_user_pages_fast(unsigned long start, int nr_pages,
			unsigned int gup_flags, struct page **pages);
void unpin_user_page(struct page *page);
int fixup_user_fault(struct mm_struct *mm, unsigned long address,
		     unsigned int fault_flags, bool *unlocked);
unsigned long vma_kernel_pagesize(struct vm_area_struct *vma);

/* A PFN mapping looked up under the owner's page-table lock, released by
 * follow_pfnmap_end. */
struct follow_pfnmap_args {
	struct vm_area_struct *vma;
	unsigned long address;
	spinlock_t *lock;
	pte_t *ptep;
	unsigned long pfn;
	pgprot_t pgprot;
	bool writable;
	bool special;
};
int follow_pfnmap_start(struct follow_pfnmap_args *args);
void follow_pfnmap_end(struct follow_pfnmap_args *args);

/* Page-table memory accounting (/proc/meminfo's SecondaryPageTables). */
#define NR_SECONDARY_PAGETABLE 0
static inline void mod_lruvec_page_state(struct page *page, int idx, int val)
{
	(void)page; (void)idx; (void)val;
}

void *vmemdup_array_user(const void __user *src, size_t n, size_t size);
void *__vcalloc(size_t n, size_t size, gfp_t flags);
#define vcalloc(n, size) __vcalloc(n, size, GFP_KERNEL)

/* ── MMU notifiers: KVM hears every change to the owner's page tables ─── */
struct mmu_notifier;
struct mmu_notifier_range {
	struct mm_struct *mm;
	unsigned long start;
	unsigned long end;
	unsigned int flags;
	int event;
	void *owner;
};
#define MMU_NOTIFIER_RANGE_BLOCKABLE (1 << 0)
static inline bool mmu_notifier_range_blockable(const struct mmu_notifier_range *r)
{
	return r->flags & MMU_NOTIFIER_RANGE_BLOCKABLE;
}
struct mmu_notifier_ops {
	void (*release)(struct mmu_notifier *sub, struct mm_struct *mm);
	int (*clear_flush_young)(struct mmu_notifier *sub, struct mm_struct *mm,
				 unsigned long start, unsigned long end);
	int (*clear_young)(struct mmu_notifier *sub, struct mm_struct *mm,
			   unsigned long start, unsigned long end);
	int (*test_young)(struct mmu_notifier *sub, struct mm_struct *mm,
			  unsigned long address);
	int (*invalidate_range_start)(struct mmu_notifier *sub,
				      const struct mmu_notifier_range *range);
	void (*invalidate_range_end)(struct mmu_notifier *sub,
				     const struct mmu_notifier_range *range);
	void (*arch_invalidate_secondary_tlbs)(struct mmu_notifier *sub,
					       struct mm_struct *mm,
					       unsigned long start, unsigned long end);
	struct mmu_notifier *(*alloc_notifier)(struct mm_struct *mm);
	void (*free_notifier)(struct mmu_notifier *sub);
};
struct mmu_notifier {
	struct hlist_node hlist;
	const struct mmu_notifier_ops *ops;
	struct mm_struct *mm;
	struct rcu_head rcu;
	unsigned int users;
};
int mmu_notifier_register(struct mmu_notifier *sub, struct mm_struct *mm);
void mmu_notifier_unregister(struct mmu_notifier *sub, struct mm_struct *mm);

/* ── file and debugfs odds ────────────────────────────────────────────── */
#define KVM_MINOR 232
struct file *anon_inode_getfile_fmode(const char *name,
				      const struct file_operations *fops,
				      void *priv, int flags, fmode_t f_mode);
struct kobj_uevent_env {
	char *argv[3];
	char *envp[64];
	int envp_idx;
	char buf[2048];
	int buflen;
};
static inline bool debugfs_initialized(void) { return false; }
static inline struct dentry *debugfs_lookup(const char *name, struct dentry *parent)
{
	(void)name; (void)parent;
	return NULL;
}
int simple_attr_open(struct inode *inode, struct file *file,
		     int (*get)(void *, u64 *), int (*set)(void *, u64),
		     const char *fmt);
int simple_attr_release(struct inode *inode, struct file *file);
ssize_t simple_attr_read(struct file *file, char __user *buf, size_t len, loff_t *ppos);
ssize_t simple_attr_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos);
#define __simple_attr_check_format(fmt, ...) do { } while (0)

/* ── bit and atomic odds ──────────────────────────────────────────────── */
static inline void set_bit_le(unsigned int nr, void *addr)
{
	set_bit(nr, (unsigned long *)addr);
}
static inline long atomic_long_fetch_andnot(long mask, atomic_long_t *v)
{
	return __atomic_fetch_and(&v->counter, ~mask, __ATOMIC_SEQ_CST);
}

/* ── more of the same ─────────────────────────────────────────────────── */
#ifndef noinstr
#define noinstr noinline
#endif
#define virt_mb()  smp_mb()
#define virt_rmb() smp_rmb()
#define virt_wmb() smp_wmb()
#define arch___test_and_set_bit(nr, addr)   __test_and_set_bit(nr, addr)
#define arch___test_and_clear_bit(nr, addr) __test_and_clear_bit(nr, addr)

/* Any kernel address to physical: the direct map by subtraction, the
 * kernel image and vmalloc space through the page tables. */
unsigned long lkpi_virt_to_phys(const volatile void *va);
#define __pa(va)          lkpi_virt_to_phys((const void *)(va))
#define __pa_symbol(va)   lkpi_virt_to_phys((const void *)(va))
#define __is_canonical_address(va, bits) \
	(((long)(va) << (64 - (bits))) >> (64 - (bits)) == (long)(va))

#ifndef pr_debug_ratelimited
#define pr_debug_ratelimited(fmt, ...) ((void)0)
#define pr_err_ratelimited(fmt, ...)   pr_err(fmt, ##__VA_ARGS__)
#define pr_warn_ratelimited(fmt, ...)  pr_warn(fmt, ##__VA_ARGS__)
#define pr_info_ratelimited(fmt, ...)  pr_info(fmt, ##__VA_ARGS__)
#endif

/* The TSC frequency b1nix calibrated at boot, in kHz. */
extern unsigned int tsc_khz;
extern unsigned int cpu_khz;

/* The zero page: KVM must not map it writable into a guest. */
unsigned long lkpi_zero_pfn(void);
#define my_zero_pfn(addr) ({ (void)(addr); lkpi_zero_pfn(); })
#define is_zero_pfn(pfn)  ((pfn) == lkpi_zero_pfn())

/* The owner's address space lock. */
#define mmap_read_lock(mm)     down_read(&(mm)->mmap_lock)
#define mmap_read_unlock(mm)   up_read(&(mm)->mmap_lock)
#define mmap_write_lock(mm)    down_write(&(mm)->mmap_lock)
#define mmap_write_unlock(mm)  up_write(&(mm)->mmap_lock)
#define mmap_read_trylock(mm)  down_read_trylock(&(mm)->mmap_lock)
/* Real counts: lkpi's own mmgrab/mmdrop are no-ops (nothing there keeps an
 * mm), and KVM holds its owner's mm for the life of a VM. */
#include <linux/sched/mm.h>
#define mmgrab kvm_mmgrab
#define mmdrop kvm_mmdrop
#define mmget_not_zero kvm_mmget_not_zero
#define mmput kvm_mmput
void kvm_mmgrab(struct mm_struct *mm);
void kvm_mmdrop(struct mm_struct *mm);
bool kvm_mmget_not_zero(struct mm_struct *mm);
void kvm_mmput(struct mm_struct *mm);


static inline pid_t task_tgid_nr(struct task_struct *t) { return t->tgid; }
static inline int srcu_read_lock_held(const struct srcu_struct *ssp) { (void)ssp; return 1; }

/* The CPU's family, model and stepping out of a CPUID(1) signature. */
static inline unsigned int x86_family(unsigned int sig)
{
	unsigned int x86 = (sig >> 8) & 0xf;

	if (x86 == 0xf)
		x86 += (sig >> 20) & 0xff;
	return x86;
}
static inline unsigned int x86_model(unsigned int sig)
{
	unsigned int fam = x86_family(sig), model = (sig >> 4) & 0xf;

	if (fam >= 0x6)
		model += ((sig >> 16) & 0xf) << 4;
	return model;
}
static inline unsigned int x86_stepping(unsigned int sig) { return sig & 0xf; }

/* Frequency changes of an unstable TSC are never announced here: b1nix has
 * no cpufreq transition notifiers. */
#include <linux/cpufreq.h>
struct cpufreq_freqs {
	struct cpufreq_policy *policy;
	unsigned int old;
	unsigned int new;
	u8 flags;
};
#define CPUFREQ_TRANSITION_NOTIFIER 0
#define CPUFREQ_PRECHANGE  0
#define CPUFREQ_POSTCHANGE 1
static inline int cpufreq_register_notifier(struct notifier_block *nb, unsigned int list)
{
	(void)nb; (void)list;
	return 0;
}
static inline int cpufreq_unregister_notifier(struct notifier_block *nb, unsigned int list)
{
	(void)nb; (void)list;
	return 0;
}


/* ── attributes, module parameters ────────────────────────────────────── */
#ifndef __ro_after_init
#define __ro_after_init
#endif
#define __MODULE_PARM_TYPE(name, _type)

/* ── lockdep and the list/RCU odds ────────────────────────────────────── */
#define spin_acquire(map, sub, trylock, ip) do { (void)(map); } while (0)
#define spin_release(map, ip)               do { (void)(map); } while (0)
typedef seqcount_t seqcount_spinlock_t;
#define seqcount_spinlock_init(s, lock) seqcount_init(s)

static inline void hlist_del(struct hlist_node *n) { hlist_del_init(n); }
#define hlist_del_rcu(n)            hlist_del_init(n)
#define hlist_del_init_rcu(n)       hlist_del_init(n)
#define hlist_add_head_rcu(n, h)    hlist_add_head(n, h)
#define list_for_each_entry_srcu(pos, head, member, cond) \
	list_for_each_entry(pos, head, member)
#define hlist_for_each_entry_srcu(pos, head, member, cond) \
	hlist_for_each_entry(pos, head, member)

/* ── CPUs ─────────────────────────────────────────────────────────────── */
const struct cpumask *get_cpu_mask(unsigned int cpu);
#define for_each_present_cpu(cpu) for_each_possible_cpu(cpu)
#define list_next_or_null_rcu(head, ptr, type, member) \
	({ struct list_head *__n = READ_ONCE((ptr)->next); \
	   __n != (head) ? list_entry(__n, type, member) : NULL; })
#define __alloc_pages_node(nid, gfp, order) ({ (void)(nid); alloc_pages(gfp, order); })
int param_get_uint(char *buffer, const struct kernel_param *kp);
int param_set_uint(const char *val, const struct kernel_param *kp);
int param_get_bool(char *buffer, const struct kernel_param *kp);
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#endif
#define FOLL_INTERRUPTIBLE 0x4000
#define CAP_SYS_BOOT 22
#define BUS_MCEERR_AR 4
#ifndef BITS_PER_LONG_LONG
#define BITS_PER_LONG_LONG 64
#endif
#define raw_spin_lock_nested(l, sub) do { (void)(sub); spin_lock(l); } while (0)
int vm_munmap(unsigned long start, size_t len);
int send_sig_mceerr(int code, void __user *addr, short lsb, struct task_struct *t);
void wbinvd_on_cpu(int cpu);
void wbinvd_on_cpus_mask(struct cpumask *cpus);
static inline void x86_clear_cpu_buffers(void) { }
static inline int set_memory_encrypted(unsigned long addr, int numpages) { (void)addr; (void)numpages; return 0; }
static inline int set_memory_decrypted(unsigned long addr, int numpages) { (void)addr; (void)numpages; return 0; }

static inline void cpumask_set_cpu(unsigned int cpu, struct cpumask *m)
{
	__atomic_fetch_or(&m->bits[cpu / 64], 1UL << (cpu % 64), __ATOMIC_SEQ_CST);
}
static inline bool cpu_smt_possible(void) { return sched_smt_active(); }
#define preemptible() (!irqs_disabled() && !in_atomic())

/* ── arithmetic and bits ──────────────────────────────────────────────── */
static inline s64 atomic64_fetch_and(s64 mask, atomic64_t *v)
{
	return __atomic_fetch_and(&v->counter, mask, __ATOMIC_SEQ_CST);
}
#define PAGE_ALIGN_DOWN(addr) ((addr) & PAGE_MASK)
#ifndef __GENMASK
#define __GENMASK(h, l) GENMASK(h, l)
#endif
#ifndef VM_BUG_ON
#define VM_BUG_ON(cond) BUG_ON(cond)
#endif
#ifndef __GENMASK_ULL
#define __GENMASK_ULL(h, l) GENMASK_ULL(h, l)
#endif
static inline u64 mul_u64_u64_shr(u64 a, u64 mul, unsigned int shift)
{
	return (u64)(((unsigned __int128)a * mul) >> shift);
}
static inline unsigned long find_last_bit(const unsigned long *addr, unsigned long size)
{
	for (unsigned long i = size; i-- > 0;)
		if ((addr[i / 64] >> (i % 64)) & 1)
			return i;
	return size;
}

/* ── user memory ──────────────────────────────────────────────────────── */
/* cmpxchg on a user address, as upstream's __try_cmpxchg_user: 0 when it
 * swapped, 1 when *old was stale (and is updated), -EFAULT when the address
 * is not mapped writable. The label is upstream's calling convention; it is
 * defined here, where upstream's asm goto defines it. */
int lkpi_cmpxchg_user(void __user *uaddr, void *old, u64 new, int size);
#define __try_cmpxchg_user(ptr, oldp, nval, label)                           \
	({                                                                    \
		int __r = lkpi_cmpxchg_user((void __user *)(ptr), (oldp),     \
					    (u64)(nval), sizeof(*(ptr)));      \
		if (0)                                                        \
			goto label;                                           \
	label:                                                                \
		__r;                                                          \
	})

/* ── the current task's user segments ─────────────────────────────────── */
void current_save_fsgs(void);
#define is_64bit_mm(mm) true
static inline bool sched_info_on(void) { return false; }

/* ── high-resolution timers ───────────────────────────────────────────── */
#define HRTIMER_MODE_ABS_HARD   HRTIMER_MODE_ABS
#define HRTIMER_MODE_ABS_PINNED HRTIMER_MODE_ABS
#define HRTIMER_MODE_REL_HARD   HRTIMER_MODE_REL
#define HRTIMER_MODE_ABS_PINNED_HARD HRTIMER_MODE_ABS
/* KVM's timers are guest clocks (the PIT, each LAPIC timer): all of them
 * fire from the timer interrupt, as upstream's hard hrtimers do. */
#define hrtimer_setup hrtimer_setup_hard
static inline void hrtimer_set_expires(struct hrtimer *t, ktime_t when) { t->node.expires = when; }
static inline void hrtimer_add_expires_ns(struct hrtimer *t, u64 ns)
{
	t->node.expires = ktime_add_ns(t->node.expires, ns);
}
static inline void hrtimer_start_expires(struct hrtimer *t, enum hrtimer_mode mode)
{
	hrtimer_start(t, t->node.expires, mode);
}
static inline ktime_t hrtimer_get_remaining(const struct hrtimer *t)
{
	return ktime_sub(t->node.expires, ktime_get());
}
static inline void hrtimer_restart(struct hrtimer *t)
{
	hrtimer_start(t, t->node.expires, HRTIMER_MODE_ABS);
}


/* ── guards ───────────────────────────────────────────────────────────── */
DEFINE_LOCK_GUARD_1(srcu, struct srcu_struct,
		    _T->idx = srcu_read_lock(_T->lock),
		    srcu_read_unlock(_T->lock, _T->idx),
		    int idx)
DEFINE_LOCK_GUARD_0(rcu, rcu_read_lock(), rcu_read_unlock())

/* ── CPUs, again ──────────────────────────────────────────────────────── */
#define for_each_cpu(cpu, mask) \
	for ((cpu) = 0; (cpu) < NR_CPUS; (cpu)++) \
		if (!(((mask)->bits[(cpu) / 64] >> ((cpu) % 64)) & 1)) {} else
static inline unsigned long find_nth_bit(const unsigned long *addr, unsigned long size,
					 unsigned long n)
{
	for (unsigned long i = 0; i < size; i++)
		if (((addr[i / 64] >> (i % 64)) & 1) && n-- == 0)
			return i;
	return size;
}
u32 cpu_physical_id(int cpu);
static inline bool cpu_mitigations_off(void) { return true; }
static inline bool gds_ucode_mitigated(void) { return false; }
unsigned long cpu_kernelmode_gs_base(int cpu);
#define migrate_disable() preempt_disable()
#define migrate_enable()  preempt_enable()
#define barrier_nospec()  __asm__ volatile("lfence" ::: "memory")
static inline bool object_is_on_stack(const void *obj) { (void)obj; return false; }
void __delay(unsigned long loops);

/* ── the TSC ──────────────────────────────────────────────────────────── */
/* Whether the TSC is unreliable across CPUs. b1nix keeps time with it and
 * believes it (see ktime), so it is stable here. */
static inline int check_tsc_unstable(void) { return 0; }
static inline void mark_tsc_unstable(char *reason) { (void)reason; }

/* ── arithmetic ───────────────────────────────────────────────────────── */
static inline s64 mul_s64_u64_shr(s64 a, u64 b, unsigned int shift)
{
	u64 ret = mul_u64_u64_shr(a < 0 ? -(u64)a : (u64)a, b, shift);

	return a < 0 ? -(s64)ret : (s64)ret;
}
static inline u32 __iter_div_u64_rem(u64 dividend, u32 divisor, u64 *remainder)
{
	u32 ret = 0;

	while (dividend >= divisor) {
		dividend -= divisor;
		ret++;
	}
	*remainder = dividend;
	return ret;
}
static inline u64 __canonical_address(u64 vaddr, u8 vaddr_bits)
{
	return ((s64)vaddr << (64 - vaddr_bits)) >> (64 - vaddr_bits);
}

/* ── atomics ──────────────────────────────────────────────────────────── */
#define atomic_cmpxchg_acquire(v, o, n) atomic_cmpxchg(v, o, n)
#define atomic_cmpxchg_release(v, o, n) atomic_cmpxchg(v, o, n)
static inline void atomic_long_set_release(atomic_long_t *v, long i)
{
	__atomic_store_n(&v->counter, i, __ATOMIC_RELEASE);
}
static inline bool atomic_long_try_cmpxchg_acquire(atomic_long_t *v, long *old, long new)
{
	return __atomic_compare_exchange_n(&v->counter, old, new, false,
					   __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE);
}
static inline void atomic64_andnot(s64 i, atomic64_t *v)
{
	__atomic_fetch_and(&v->counter, ~i, __ATOMIC_SEQ_CST);
}

/* ── page-table entry flags ───────────────────────────────────────────── */
static inline u64 pte_flags(pte_t pte) { return pte_val(pte) & ~0x000ffffffffff000ULL; }
static inline u16 pte_flags_pkey(u64 pte_flags) { return (pte_flags >> 59) & 0xf; }

/* ── the owner's memory, by another task ──────────────────────────────── */
long get_user_pages_remote(struct mm_struct *mm, unsigned long start,
			   unsigned long nr_pages, unsigned int gup_flags,
			   struct page **pages, int *locked);

/* ── MSI ──────────────────────────────────────────────────────────────── */
static inline u32 x86_msi_msg_get_destid(struct msi_msg *msg, bool extid)
{
	u32 dest = msg->arch_addr_lo.destid_0_7;

	if (extid)
		dest |= msg->arch_addr_lo.virt_destid_8_14 << 8;
	return dest;
}

/* ── L1TF flush bookkeeping (no mitigation, nothing to flush) ─────────── */
static inline u8 kvm_get_cpu_l1tf_flush_l1d(void) { return 0; }
static inline void kvm_clear_cpu_l1tf_flush_l1d(void) { }
static inline void kvm_set_cpu_l1tf_flush_l1d(void) { }

/* ── polling a file (irqfd hooks the eventfd's wait queue) ────────────── */
struct poll_table_struct;
typedef void (*poll_queue_proc)(struct file *, wait_queue_head_t *, struct poll_table_struct *);
typedef struct poll_table_struct {
	poll_queue_proc _qproc;
	__poll_t _key;
} poll_table;
static inline void init_poll_funcptr(poll_table *pt, poll_queue_proc qproc)
{
	pt->_qproc = qproc;
	pt->_key = ~(__poll_t)0;
}
#define key_to_poll(m) ((__poll_t)(uintptr_t)(void *)(m))
/* 0, or -EBUSY when an exclusive priority waiter is already there. */
int add_wait_queue_priority_exclusive(wait_queue_head_t *wq_head,
				      struct wait_queue_entry *wq_entry);


/* ── AMD SVM (M131) ───────────────────────────────────────────────────── */
/* No memory encryption on the host: the C-bit is never set in an address. */
#ifndef __sme_set
#define __sme_set(x) (x)
#define __sme_clr(x) (x)
#endif
static inline int numa_node_id(void) { return 0; }
#include <linux/cc_platform.h>
/* The host's memory-encryption mask: no SME here. */
#define sme_me_mask 0ULL
#define __no_kcsan
#ifndef FW_BUG
#define FW_BUG "[Firmware Bug]: "
#endif
/* Zen 1's divide-by-zero erratum: a DIV left in flight can leak its quotient
 * to the next context. Upstream clears it only on affected parts; a harmless
 * 0/1 everywhere does the same job. */
static inline void amd_clear_divider(void)
{
	unsigned long a = 0, d = 0;

	__asm__ volatile("div %2" : "+a"(a), "+d"(d) : "r"(1UL));
}
/* Only a Hyper-V host offers the enlightened flush. */
static inline int hyperv_flush_guest_mapping(u64 as) { (void)as; return -EOPNOTSUPP; }
/* Every translation this CPU holds. b1nix runs with CR4.PGE off, so a CR3
 * reload drops them all. */
static inline void __flush_tlb_all(void)
{
	unsigned long cr3;

	__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
	__asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
#define HWEIGHT32(w) __builtin_popcount((u32)(w))
unsigned long clear_user(void __user *to, unsigned long n);
/* AVIC's parameter ops: named by a definition, never called (see
 * <linux/moduleparam.h>). */
#define KERNEL_PARAM_OPS_FL_NOARG (1 << 0)
int param_set_bint(const char *val, const struct kernel_param *kp);
int param_get_bool(char *buffer, const struct kernel_param *kp);
DEFINE_LOCK_GUARD_1(raw_spinlock_irqsave, raw_spinlock_t,
		    raw_spin_lock_irqsave(_T->lock, _T->flags),
		    raw_spin_unlock_irqrestore(_T->lock, _T->flags),
		    unsigned long flags)
/* What AVIC hands the AMD IOMMU for a posted interrupt (asm/irq_remapping.h);
 * with no IOMMU interrupt remapping it only ever reaches the no-op stubs. */
struct amd_iommu_pi_data {
	u64 vapic_addr;
	u32 ga_tag;
	u32 vector;
	int cpu;
	bool ga_log_intr;
	bool is_guest_mode;
	void *ir_data;
};

/* ── eventfd: the process's own eventfds (<linux/eventfd.h>) ─────────── */
#include <linux/eventfd.h>


/* ── names b1nix or lkpi already give to something else ───────────────── */
/* b1nix's own vfs_poll and vma_lookup (the native VFS and VM) and lkpi's
 * placeholder find_vma/vm_mmap would be linked in their place; KVM gets real
 * versions under names of their own (kernel/lkpi/kvm_mm.c). */
/* lkpi's fd_install gives most files only lseek (DRM keeps descriptors of
 * its own), and its anon_inode_getfd is unwired; KVM's descriptors are owned
 * files: ioctl, poll, mmap and the last close all reach KVM. */
#define fd_install        kvm_fd_install
#define anon_inode_getfd  kvm_anon_inode_getfd
void kvm_fd_install(unsigned int fd, struct file *f);
int kvm_anon_inode_getfd(const char *name, const struct file_operations *fops,
			 void *priv, int flags);
#define vfs_poll   kvm_vfs_poll
#define vma_lookup kvm_vma_lookup
#define find_vma   kvm_find_vma
#define vm_mmap    kvm_vm_mmap
__poll_t kvm_vfs_poll(struct file *file, poll_table *pt);
struct vm_area_struct *kvm_vma_lookup(struct mm_struct *mm, unsigned long addr);
struct vm_area_struct *kvm_find_vma(struct mm_struct *mm, unsigned long addr);
unsigned long kvm_vm_mmap(struct file *file, unsigned long addr, unsigned long len,
			  unsigned long prot, unsigned long flag, unsigned long offset);


/* ── `current`, with the mm KVM expects on it ─────────────────────────── */
/* lkpi's task never carries an address space; KVM's needs the one of the
 * process it runs for (kernel/lkpi/kvm_mm.c). Only KVM's sources see this. */
struct task_struct *kvm_lkpi_current(void);
#undef current
#define current (kvm_lkpi_current())

/* Start-up (kernel/lkpi/kvm_sched.c), called by main.c. */
int kvm_lkpi_init(void);
void lkpi_x86_cpu_init(void);

#endif
