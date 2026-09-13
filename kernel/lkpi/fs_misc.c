/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: the services around a filesystem that are not the filesystem.
 *
 * Allocation scopes, identity, UUIDs, bitmaps, rate limiting, sysfs and
 * kobject odds and ends, the radix-tree spellings, mempools and the
 * error-sequence counters. Each is small; together they are what a filesystem
 * assumes the kernel has.
 *
 * On the b1nix side of the boundary — b1nix and lkpi headers only.
 */

#include <lkpi/env.h>
#include <lkpi/types.h>
#include <b1nix/klog.h>
#include <b1nix/sched.h>
#include <b1nix/ktime.h>
#include <b1nix/uidgid.h>
#include <b1nix/arch.h>
#include <string.h>

void *lkpi_kmalloc(usize size, u32 flags);
void lkpi_kfree(void *ptr);

/* ── allocation scope ───────────────────────────────────────────── */

/*
 * PF_MEMALLOC_NOFS and friends, per task.
 *
 * The scope says: for the rest of this region every allocation is implicitly
 * GFP_NOFS, even the ones made by code that does not know it is inside a
 * filesystem. That is the point — a filesystem holding a transaction calls
 * helpers that allocate, and none of them can be expected to clear __GFP_FS.
 *
 * The restore takes the PREVIOUS value rather than clearing, because these
 * nest: a restore that unconditionally cleared would re-enable reclaim
 * recursion in the middle of an outer scope.
 *
 * b1nix's reclaim does not call into a filesystem yet, so nothing reads the
 * flag. It is recorded rather than discarded because the day it does, this is
 * the state it reads — and by then the nesting has to already be right.
 */
#define LKPI_PF_MEMALLOC_NOFS 0x00040000u
#define LKPI_PF_MEMALLOC_NOIO 0x00080000u

static unsigned int *lkpi_task_flags(void)
{
	struct lkpi_task *t = lkpi_current();

	return t ? &t->flags : NULL;
}

unsigned int memalloc_nofs_save(void)
{
	unsigned int *flags = lkpi_task_flags();
	unsigned int old;

	if (!flags)
		return 0;
	old = *flags & LKPI_PF_MEMALLOC_NOFS;
	*flags |= LKPI_PF_MEMALLOC_NOFS;
	return old;
}

void memalloc_nofs_restore(unsigned int old)
{
	unsigned int *flags = lkpi_task_flags();

	if (!flags)
		return;
	*flags = (*flags & ~LKPI_PF_MEMALLOC_NOFS) | old;
}

unsigned int memalloc_noio_save(void)
{
	unsigned int *flags = lkpi_task_flags();
	unsigned int old;

	if (!flags)
		return 0;
	old = *flags & LKPI_PF_MEMALLOC_NOIO;
	*flags |= LKPI_PF_MEMALLOC_NOIO;
	return old;
}

void memalloc_noio_restore(unsigned int old)
{
	unsigned int *flags = lkpi_task_flags();

	if (!flags)
		return;
	*flags = (*flags & ~LKPI_PF_MEMALLOC_NOIO) | old;
}

unsigned int memalloc_nowait_save(void) { return 0; }
void memalloc_nowait_restore(unsigned int old) { (void)old; }

u32 current_gfp_context(u32 flags) { return flags; }

/* Park briefly after a failed allocation, before trying again. b1nix's
 * allocator does not fail transiently, so a caller reaching this has asked for
 * something too large — the yield keeps the retry loop from being a spin. */
void memalloc_retry_wait(u32 gfp_flags)
{
	(void)gfp_flags;
	scheduler_yield();
}

/* ── identity ───────────────────────────────────────────────────── */

/*
 * The credentials a filesystem stamps a new inode with.
 *
 * It must be the FS uid rather than the effective one: they differ exactly when
 * a process has called setfsuid, which exists so a server can act as a client
 * for filesystem access without becoming it for signals.
 */
struct lkpi_kuid { u32 val; };
struct lkpi_kgid { u32 val; };

struct lkpi_kuid current_fsuid_val(void)
{
	const struct cred *cred = scheduler_get_current_cred();
	struct lkpi_kuid uid = { .val = cred ? (u32)cred->fsuid : 0u };

	return uid;
}

struct lkpi_kgid current_fsgid_val(void)
{
	const struct cred *cred = scheduler_get_current_cred();
	struct lkpi_kgid gid = { .val = cred ? (u32)cred->fsgid : 0u };

	return gid;
}

/* The umask a new file's mode is masked with. */
unsigned short current_umask(void)
{
	struct task *t = current_task;

	return t ? t->umask : 0022;
}

/* One user namespace, so every filesystem sees the same one. */
struct user_namespace { int dummy; };
struct user_namespace init_user_ns;

struct user_namespace *current_user_ns(void) { return &init_user_ns; }

/* Is the caller in this group? Root is, by the same rule every permission
 * check here uses. */
int in_group_p(struct lkpi_kgid grp)
{
	const struct cred *cred = scheduler_get_current_cred();

	if (!cred)
		return 1;
	/* The effective group, then the supplementary list — the same order every
	 * permission check in b1nix uses, and the same one POSIX requires. */
	if (cred->egid == (u16)grp.val)
		return 1;
	for (int i = 0; i < cred->ngroups; i++)
		if (cred->groups[i] == (u16)grp.val)
			return 1;
	return 0;
}

/* ── UUIDs ──────────────────────────────────────────────────────── */

/*
 * A random UUID, version 4.
 *
 * btrfs writes one into every subvolume root it creates, and two filesystems
 * that produce the same one are indistinguishable to `btrfs filesystem show`.
 * The randomness therefore has to be real, not a counter — this draws from
 * b1nix's entropy pool, and the version and variant bits are set afterwards
 * because RFC 4122 fixes them.
 */
/* b1nix's entropy source: rdrand where the CPU has it, a seeded xorshift
 * otherwise. The same one SYS_GETRANDOM and the ASLR base use. */
u64 kernel_random_u64(void);

struct lkpi_guid { u8 b[16]; };
const struct lkpi_guid guid_null;
const struct lkpi_guid uuid_null;

static void lkpi_fill_random_uuid(unsigned char uuid[16])
{
	u64 a = kernel_random_u64();
	u64 b = kernel_random_u64();

	memcpy(uuid, &a, 8);
	memcpy(uuid + 8, &b, 8);
	uuid[6] = (u8)((uuid[6] & 0x0f) | 0x40);  /* version 4 */
	uuid[8] = (u8)((uuid[8] & 0x3f) | 0x80);  /* variant 1 */
}

void generate_random_uuid(unsigned char uuid[16])
{
	lkpi_fill_random_uuid(uuid);
}

void generate_random_guid(unsigned char guid[16])
{
	lkpi_fill_random_uuid(guid);
}

void guid_gen(struct lkpi_guid *u) { lkpi_fill_random_uuid(u->b); }
void uuid_gen(struct lkpi_guid *u) { lkpi_fill_random_uuid(u->b); }

/*
 * Copy out in the type's own byte order.
 *
 * A guid_t is little-endian and a uuid_t big-endian, and they are different
 * types for exactly this reason: btrfs stores both, and swapping them writes
 * the right sixteen bytes in the wrong order — which every other tool then
 * reads as a different filesystem.
 */
void export_guid(u8 *dst, const struct lkpi_guid *src)
{
	memcpy(dst, src->b, 16);
}

void export_uuid(u8 *dst, const struct lkpi_guid *src)
{
	memcpy(dst, src->b, 16);
}

void import_guid(struct lkpi_guid *dst, const u8 *src)
{
	memcpy(dst->b, src, 16);
}

void import_uuid(struct lkpi_guid *dst, const u8 *src)
{
	memcpy(dst->b, src, 16);
}

int uuid_is_null(const struct lkpi_guid *uuid)
{
	int i;

	for (i = 0; i < 16; i++)
		if (uuid->b[i])
			return 0;
	return 1;
}

/* ── bitmaps ────────────────────────────────────────────────────── */

/*
 * The next run of set bits at or after *rs, reported as a half-open range.
 *
 * A run rather than a bit, because that is what makes btrfs's subpage scan
 * proportional to the number of runs instead of the number of bits.
 */
void bitmap_next_set_region(unsigned long *bitmap, unsigned int *rs,
                            unsigned int *re, unsigned int end)
{
	unsigned int i = *rs;

	while (i < end && !((bitmap[i / 64] >> (i % 64)) & 1ul))
		i++;
	*rs = i;
	while (i < end && ((bitmap[i / 64] >> (i % 64)) & 1ul))
		i++;
	*re = i;
}

/*
 * Remove `cut` bits starting at `first`, shifting everything above down over
 * the gap. The bits shifted in at the top are zero.
 */
void bitmap_cut(const unsigned long *src, unsigned long *dst,
                unsigned int first, unsigned int cut, unsigned int nbits)
{
	unsigned int i, j = 0;

	for (i = 0; i < nbits; i++) {
		unsigned int from;

		if (i < first)
			from = i;
		else if (i + cut < nbits)
			from = i + cut;
		else
			from = (unsigned int)-1;

		if (from == (unsigned int)-1 ||
		    !((src[from / 64] >> (from % 64)) & 1ul))
			dst[i / 64] &= ~(1ul << (i % 64));
		else
			dst[i / 64] |= 1ul << (i % 64);
		j++;
	}
	(void)j;
}

/* Population count over a byte range. */
usize memweight(const void *ptr, usize bytes)
{
	const u8 *p = ptr;
	usize n = 0;

	while (bytes--)
		n += (usize)__builtin_popcount(*p++);
	return n;
}

/* Set some bits and clear others in one atomic step, reporting whether the
 * word changed. Two operations would let a reader see the half-updated value —
 * for an inode's flags, a file briefly neither immutable nor mutable. */
int set_mask_bits(unsigned long *ptr, unsigned long mask, unsigned long bits)
{
	unsigned long old, new;

	do {
		old = __atomic_load_n(ptr, __ATOMIC_RELAXED);
		new = (old & ~mask) | bits;
		if (new == old)
			return 0;
	} while (!__atomic_compare_exchange_n(ptr, &old, new, 0, __ATOMIC_ACQ_REL,
	                                      __ATOMIC_RELAXED));
	return 1;
}

/* ── rate limiting and reports ──────────────────────────────────── */

/*
 * The rate limiter's decision.
 *
 * A real decision, not always-true: a filesystem that has started finding
 * corruption prints per block, and an unlimited version turns a bad disk into a
 * console that never stops long enough to show the first message.
 */
struct lkpi_ratelimit_state {
	int interval;
	int burst;
	int printed;
	unsigned long begin;
};

int ___ratelimit(struct lkpi_ratelimit_state *rs, const char *func)
{
	unsigned long now = (unsigned long)(ktime_monotonic_ns() / 1000000ull);

	(void)func;
	if (!rs || rs->interval <= 0)
		return 1;
	if (!rs->begin || now - rs->begin > (unsigned long)rs->interval) {
		rs->begin = now;
		rs->printed = 0;
	}
	if (rs->printed < rs->burst) {
		rs->printed++;
		return 1;
	}
	return 0;
}

void dump_stack(void)
{
	/* Real: it is what a filesystem calls when it finds an inconsistency it
	 * is going to continue past, and a silent version throws away the only
	 * evidence of where that happened. */
	klog_warn("lkpi: filesystem requested a backtrace");
	arch_backtrace(0, (u64)__builtin_return_address(0));
}

struct page;

void dump_page(struct page *page, const char *reason)
{
	(void)page;
	klog_warn(reason ? reason : "lkpi: page dump requested");
}

/* ── error sequences ────────────────────────────────────────────── */

/*
 * A writeback error, encoded so each observer sees it exactly once.
 *
 * The low bits hold the errno and the rest a counter. A reader keeps its own
 * last-seen value and compares; `_check_and_advance` consumes, `_check` peeks.
 * Using the consuming form where a peek was meant makes the error vanish for
 * whoever should have received it.
 */
#define LKPI_ERRSEQ_ERR_MASK 0xffffu
#define LKPI_ERRSEQ_ONE      (1u << 16)

u32 errseq_set(u32 *eseq, int err)
{
	u32 old = __atomic_load_n(eseq, __ATOMIC_RELAXED);
	u32 new;

	if (!err)
		return old;
	new = (old + LKPI_ERRSEQ_ONE) | ((u32)(-err) & LKPI_ERRSEQ_ERR_MASK);
	__atomic_store_n(eseq, new, __ATOMIC_RELEASE);
	return new;
}

u32 errseq_sample(u32 *eseq)
{
	return __atomic_load_n(eseq, __ATOMIC_ACQUIRE);
}

int errseq_check(u32 *eseq, u32 since)
{
	u32 cur = __atomic_load_n(eseq, __ATOMIC_ACQUIRE);

	if (cur == since)
		return 0;
	return -(int)(cur & LKPI_ERRSEQ_ERR_MASK);
}

int errseq_check_and_advance(u32 *eseq, u32 *since)
{
	u32 cur = __atomic_load_n(eseq, __ATOMIC_ACQUIRE);
	int err;

	if (cur == *since)
		return 0;
	err = -(int)(cur & LKPI_ERRSEQ_ERR_MASK);
	*since = cur;
	return err;
}

/* ── mempools ───────────────────────────────────────────────────── */

/*
 * A pool with no reserve.
 *
 * Upstream a mempool holds pre-allocated elements so a path that must make
 * progress — writing a page out in order to free memory — never fails.
 * b1nix's allocator does not fail under pressure (it panics when the heap
 * cannot grow), so a reserve would protect against a failure mode that does not
 * exist here. The pool is the caller's own alloc/free callbacks, and the
 * guarantee it makes is the allocator's.
 */
struct lkpi_mempool {
	int min_nr;
	int curr_nr;
	void *(*alloc)(u32 gfp_mask, void *pool_data);
	void (*free)(void *element, void *pool_data);
	void *pool_data;
};

int mempool_init(struct lkpi_mempool *pool, int min_nr,
                 void *(*alloc_fn)(u32, void *),
                 void (*free_fn)(void *, void *), void *pool_data)
{
	pool->min_nr = min_nr;
	pool->curr_nr = 0;
	pool->alloc = alloc_fn;
	pool->free = free_fn;
	pool->pool_data = pool_data;
	return 0;
}

void mempool_exit(struct lkpi_mempool *pool) { (void)pool; }

struct lkpi_mempool *mempool_create(int min_nr, void *(*alloc_fn)(u32, void *),
                                    void (*free_fn)(void *, void *),
                                    void *pool_data)
{
	struct lkpi_mempool *pool = lkpi_kmalloc(sizeof(*pool), 0);

	if (pool)
		mempool_init(pool, min_nr, alloc_fn, free_fn, pool_data);
	return pool;
}

void mempool_destroy(struct lkpi_mempool *pool) { lkpi_kfree(pool); }

void *mempool_alloc(struct lkpi_mempool *pool, u32 gfp_mask)
{
	if (!pool || !pool->alloc)
		return NULL;
	return pool->alloc(gfp_mask, pool->pool_data);
}

void mempool_free(void *element, struct lkpi_mempool *pool)
{
	if (element && pool && pool->free)
		pool->free(element, pool->pool_data);
}

void *mempool_kmalloc(u32 gfp_mask, void *pool_data)
{
	return lkpi_kmalloc((usize)(unsigned long)pool_data, gfp_mask);
}

void mempool_kfree(void *element, void *pool_data)
{
	(void)pool_data;
	lkpi_kfree(element);
}

int mempool_init_kmalloc_pool(struct lkpi_mempool *pool, int min_nr, usize size)
{
	return mempool_init(pool, min_nr, mempool_kmalloc, mempool_kfree,
	                    (void *)(unsigned long)size);
}

struct lkpi_mempool *mempool_create_slab_pool(int min_nr, void *kc)
{
	/*
	 * A slab-backed pool. The cache's own alloc and free are reached through
	 * the pool's callbacks, which is what makes the elements come from that
	 * cache rather than from the general heap.
	 */
	extern void *kmem_cache_alloc(void *cache, u32 flags);
	extern void kmem_cache_free(void *cache, void *obj);

	struct lkpi_mempool *pool = lkpi_kmalloc(sizeof(*pool), 0);

	if (!pool)
		return NULL;
	pool->min_nr = min_nr;
	pool->curr_nr = 0;
	pool->pool_data = kc;
	pool->alloc = NULL;
	pool->free = NULL;
	/* The stock slab callbacks are installed by mempool_alloc_slab below;
	 * assigning them here keeps the pool self-contained. */
	extern void *mempool_alloc_slab(u32 gfp_mask, void *pool_data);
	extern void mempool_free_slab(void *element, void *pool_data);
	pool->alloc = mempool_alloc_slab;
	pool->free = mempool_free_slab;
	return pool;
}

void *mempool_alloc_slab(u32 gfp_mask, void *pool_data)
{
	extern void *kmem_cache_alloc(void *cache, u32 flags);

	return kmem_cache_alloc(pool_data, gfp_mask);
}

void mempool_free_slab(void *element, void *pool_data)
{
	extern void kmem_cache_free(void *cache, void *obj);

	kmem_cache_free(pool_data, element);
}

/* ── user copies ────────────────────────────────────────────────── */

/*
 * The iterator's copies, over b1nix's checked user access.
 *
 * b1nix's helpers return 0 or an error; Linux's return the number of bytes NOT
 * copied. The conversion is here rather than at each call site because the two
 * conventions are the kind that get inverted silently — a caller reading the
 * error as a byte count would report a successful copy as a short one.
 *
 * A failure reports the WHOLE length as uncopied. b1nix's copy does not say how
 * far it got, and claiming a partial copy that did not happen would leave the
 * destination holding bytes nobody wrote.
 */
int syscall_copyin(void *dst, const void *user_src, usize size);
int syscall_copyout(void *user_dst, const void *src, usize size);

usize copy_to_user_raw(void *dst, const void *src, usize len)
{
	return syscall_copyout(dst, src, len) == 0 ? 0 : len;
}

usize copy_from_user_raw(void *dst, const void *src, usize len)
{
	return syscall_copyin(dst, src, len) == 0 ? 0 : len;
}

/*
 * The no-fault forms.
 *
 * They must not page anything in: the caller holds a lock the fault handler
 * would need. b1nix's copy already refuses an address it cannot reach without
 * calling back into a filesystem, so the ordinary helper has the property this
 * one promises — and the name records why the caller wanted it.
 */
long copy_to_user_nofault(void *dst, const void *src, usize size)
{
	return syscall_copyout(dst, src, size) == 0 ? 0 : -14; /* -EFAULT */
}

long copy_from_user_nofault(void *dst, const void *src, usize size)
{
	return syscall_copyin(dst, src, size) == 0 ? 0 : -14;
}

/* ── kernel threads for the filesystems ─────────────────────────── */

/*
 * The adapter between Linux's kthread entry and b1nix's, plus the stop
 * protocol.
 *
 * Linux's entry returns int and takes the thread's own argument; b1nix's
 * returns void. The wrapper holds the pair, and lives here rather than on the
 * Linux side because creating the thread is a b1nix call.
 *
 * A thread also has to be STOPPABLE. btrfs's cleaner and transaction threads
 * are joined by close_ctree, which sets the stop flag, wakes the thread and
 * waits for it to leave its loop; with a kthread_should_stop() that always
 * answered false, the unmount never returned and the cleaner spun on the CPU
 * for as long as the machine was up.
 */
struct lkpi_kthread_arg {
	int (*fn)(void *);
	void *data;
	u64 id;                  /* b1nix task id, so the current thread finds it */
	/* The thread's own `current`, published by the thread itself: the handle
	 * a waker must mark, since it is what the thread's sleep checks. */
	struct lkpi_task *volatile task;
	volatile int should_stop;
	volatile int finished;
	int ret;
	struct lkpi_kthread_arg *next;
};

static struct lkpi_kthread_arg *lkpi_kthreads;
static spinlock_t lkpi_kthread_lock = SPINLOCK_INIT;

static struct lkpi_kthread_arg *kthread_find(u64 id)
{
	struct lkpi_kthread_arg *a;
	u64 flags;

	spin_lock_irqsave(&lkpi_kthread_lock, &flags);
	for (a = lkpi_kthreads; a; a = a->next)
		if (a->id == id)
			break;
	spin_unlock_irqrestore(&lkpi_kthread_lock, flags);
	return a;
}

static struct lkpi_kthread_arg *kthread_find_task(struct lkpi_task *t)
{
	struct lkpi_kthread_arg *a;
	u64 flags;

	spin_lock_irqsave(&lkpi_kthread_lock, &flags);
	for (a = lkpi_kthreads; a; a = a->next)
		if (a->task == t)
			break;
	spin_unlock_irqrestore(&lkpi_kthread_lock, flags);
	return a;
}

int kthread_create(const char *name, void (*entry)(void *arg), void *arg);

static void lkpi_fs_kthread_entry(void *arg)
{
	struct lkpi_kthread_arg *a = arg;

	a->task = lkpi_current();
	a->ret = a->fn(a->data);
	a->finished = 1;
	/* Whoever is in kthread_stop() is parked on the record. */
	scheduler_wake_all((void *)a);
	scheduler_exit_current(0);
}

/* Answers for the thread that asks — the flag belongs to the caller's own
 * record, found by its task id. */
int lkpi_kthread_should_stop(void)
{
	struct lkpi_kthread_arg *a = kthread_find(scheduler_current_task_id());

	return a ? a->should_stop : 0;
}

/*
 * Ask a thread to stop and wait until it has.
 *
 * The wait is the point: close_ctree frees the structures the thread is still
 * reading, so returning before it has left them is a use-after-free.
 */
int lkpi_kthread_stop(unsigned long handle)
{
	struct lkpi_kthread_arg *a = kthread_find_task((struct lkpi_task *)handle);
	struct lkpi_kthread_arg **pp;
	u64 flags;
	int ret;

	if (!a)
		return -22; /* -EINVAL; b1nix's errno header is not in scope here */
	a->should_stop = 1;
	lkpi_wake_task((struct lkpi_task *)handle);
	while (!a->finished) {
		lkpi_wake_task((struct lkpi_task *)handle);
		scheduler_yield();
	}
	ret = a->ret;

	spin_lock_irqsave(&lkpi_kthread_lock, &flags);
	for (pp = &lkpi_kthreads; *pp; pp = &(*pp)->next)
		if (*pp == a) {
			*pp = a->next;
			break;
		}
	spin_unlock_irqrestore(&lkpi_kthread_lock, flags);
	lkpi_kfree(a);
	return ret;
}

struct lkpi_task *lkpi_fs_kthread_run(int (*threadfn)(void *data), void *data,
                                      const char *name)
{
	struct lkpi_kthread_arg *a = lkpi_kmalloc(sizeof(*a), 0);
	u64 flags;
	int id;

	if (!a)
		return NULL;
	a->fn = threadfn;
	a->data = data;
	a->should_stop = 0;
	a->finished = 0;
	a->ret = 0;
	a->task = NULL;
	id = kthread_create(name ? name : "lkpi-fs", lkpi_fs_kthread_entry, a);
	if (id < 0) {
		lkpi_kfree(a);
		return NULL;
	}
	a->id = (u64)id;
	spin_lock_irqsave(&lkpi_kthread_lock, &flags);
	a->next = lkpi_kthreads;
	lkpi_kthreads = a;
	spin_unlock_irqrestore(&lkpi_kthread_lock, flags);
	/* The handle is the thread's real `current`, which only the thread can
	 * name; it publishes it first thing. A number standing in for a pointer
	 * here was dereferenced by every wake: on aarch64 that faulted, on x86_64
	 * it wrote into low physical memory and woke nobody. */
	while (!a->task)
		scheduler_yield();
	return a->task;
}

/* The kernel profiler's report, and a plain sleep — both b1nix services the
 * Linux-side files reach by name. */
void kprof_dump(void);
void lkpi_kprof_dump(void) { kprof_dump(); }
void lkpi_sleep_ticks(u64 ticks) { scheduler_sleep_ticks(ticks); }

/* The scheduler's own view, for the watchdog: the profile says where the CPU
 * is, this says what every thread is waiting for. */
void lkpi_task_dump(void) { scheduler_dump_tasks(); }

