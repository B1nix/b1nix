// SPDX-License-Identifier: GPL-2.0-only
/*
 * A b1nix process's address space, as Linux's KVM uses it (M131).
 *
 * KVM keeps the address space of the process that created a VM (kvm->mm),
 * turns the host addresses of its memslots into frames, maps a few private
 * slots of its own into that process, and listens -- through MMU notifiers --
 * for every change to the process's page tables, so its EPT never points at a
 * frame the host has taken back.
 *
 * Here the mm_struct stands for a b1nix address space, identified by its PML4;
 * one per space, shared by the process's threads, created when KVM first asks
 * for current->mm. Guest RAM is reached the way Linux reaches memory without
 * a struct page (VM_PFNMAP): the vma KVM looks up says so, and
 * follow_pfnmap_start reads the frame out of the page tables. Nothing holds a
 * reference on those frames; what keeps KVM from using one after the host
 * freed it is the notifier, which b1nix's TLB code raises before the frames of
 * a changed mapping are released (see kvm_hooks.h).
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/interval_tree_generic.h>
#include <linux/interval_tree.h>
#include <asm/pgtable.h>
#include <asm/memtype.h>
#include <asm/e820/api.h>
#include <b1nix/kvm_bridge.h>

/* ── address spaces ───────────────────────────────────────────────────── */

static LIST_HEAD(kvm_mm_list);
static DEFINE_SPINLOCK(kvm_mm_lock);

static void mm_free(struct mm_struct *mm)
{
	kfree(mm);
}

void mmgrab(struct mm_struct *mm)
{
	atomic_inc(&mm->mm_count);
}

void mmdrop(struct mm_struct *mm)
{
	if (atomic_dec_and_test(&mm->mm_count))
		mm_free(mm);
}

bool mmget_not_zero(struct mm_struct *mm)
{
	int users = atomic_read(&mm->mm_users);

	while (users > 0) {
		if (atomic_cmpxchg(&mm->mm_users, users, users + 1) == users)
			return true;
		users = atomic_read(&mm->mm_users);
	}
	return false;
}

void mmput(struct mm_struct *mm)
{
	atomic_dec(&mm->mm_users);
}

/* The mm of the address space the caller runs in, created on first use. The
 * registry holds one reference (mm_count) and the address space's life
 * (mm_users) until the space is torn down. */
static struct mm_struct *mm_for_pml4(u64 pml4)
{
	struct mm_struct *mm, *fresh = NULL;
	unsigned long flags;

	if (!pml4)
		return NULL;
again:
	spin_lock_irqsave(&kvm_mm_lock, flags);
	list_for_each_entry(mm, &kvm_mm_list, node) {
		if (mm->pml4_phys == pml4) {
			spin_unlock_irqrestore(&kvm_mm_lock, flags);
			kfree(fresh);
			return mm;
		}
	}
	if (fresh) {
		list_add(&fresh->node, &kvm_mm_list);
		spin_unlock_irqrestore(&kvm_mm_lock, flags);
		return fresh;
	}
	spin_unlock_irqrestore(&kvm_mm_lock, flags);
	fresh = kzalloc(sizeof(*fresh), GFP_KERNEL);
	if (!fresh)
		return NULL;
	fresh->pml4_phys = pml4;
	init_rwsem(&fresh->mmap_lock);
	atomic_set(&fresh->mm_users, 1);
	atomic_set(&fresh->mm_count, 1);
	INIT_HLIST_HEAD(&fresh->notifiers);
	spin_lock_init(&fresh->notifier_lock);
	goto again;
}

/* `current` in KVM's sources (see kvm_linux_extra.h): lkpi's task, with the
 * mm of the space it runs in. The cached pointer is checked against the live
 * PML4, so a task that exec'd gets its new space's mm. */
struct task_struct *kvm_lkpi_current(void)
{
	struct task_struct *t = lkpi_current();
	u64 pml4 = b1nix_kvm_current_pml4();

	if (!t->mm || t->mm->pml4_phys != pml4)
		t->mm = mm_for_pml4(pml4);
	return t;
}

/* ── MMU notifiers ────────────────────────────────────────────────────── */

int mmu_notifier_register(struct mmu_notifier *sub, struct mm_struct *mm)
{
	unsigned long flags;

	sub->mm = mm;
	mmgrab(mm);
	spin_lock_irqsave(&mm->notifier_lock, flags);
	hlist_add_head(&sub->hlist, &mm->notifiers);
	spin_unlock_irqrestore(&mm->notifier_lock, flags);
	return 0;
}

/* Callbacks run without the list lock (they take KVM's own locks and may
 * send IPIs); an unregistering caller waits for the ones in flight. */
static atomic_t kvm_mm_callbacks = ATOMIC_INIT(0);

void mmu_notifier_unregister(struct mmu_notifier *sub, struct mm_struct *mm)
{
	unsigned long flags;

	spin_lock_irqsave(&mm->notifier_lock, flags);
	hlist_del_init(&sub->hlist);
	spin_unlock_irqrestore(&mm->notifier_lock, flags);
	while (atomic_read(&kvm_mm_callbacks))
		cpu_relax();
	mmdrop(mm);
}

#define KVM_MM_MAX_SUBS 16

/* Snapshot of an mm's subscribers, taken under its lock. */
static int mm_subs(struct mm_struct *mm, struct mmu_notifier **subs)
{
	struct mmu_notifier *sub;
	unsigned long flags;
	int n = 0;

	spin_lock_irqsave(&mm->notifier_lock, flags);
	hlist_for_each_entry(sub, &mm->notifiers, hlist)
		if (n < KVM_MM_MAX_SUBS)
			subs[n++] = sub;
	spin_unlock_irqrestore(&mm->notifier_lock, flags);
	return n;
}

static void mm_invalidate(struct mm_struct *mm, u64 start, u64 end, int blockable)
{
	struct mmu_notifier *subs[KVM_MM_MAX_SUBS];
	struct mmu_notifier_range range = {
		.mm = mm, .start = start, .end = end,
		.flags = blockable ? MMU_NOTIFIER_RANGE_BLOCKABLE : 0,
	};
	int n = mm_subs(mm, subs);

	for (int i = 0; i < n; i++)
		if (subs[i]->ops->invalidate_range_start)
			subs[i]->ops->invalidate_range_start(subs[i], &range);
	for (int i = 0; i < n; i++)
		if (subs[i]->ops->invalidate_range_end)
			subs[i]->ops->invalidate_range_end(subs[i], &range);
}

/* From b1nix's TLB code: [start, end) of address space pml4 (0: all of
 * them) changed, and its old frames are about to be released. */
static void kvm_mm_invalidate_hook(u64 pml4, u64 start, u64 end, int blockable)
{
	struct mm_struct *targets[8];
	struct mm_struct *mm;
	unsigned long flags;
	int n = 0;

	if (list_empty(&kvm_mm_list))
		return;
	atomic_inc(&kvm_mm_callbacks);
	spin_lock_irqsave(&kvm_mm_lock, flags);
	list_for_each_entry(mm, &kvm_mm_list, node)
		if ((!pml4 || mm->pml4_phys == pml4) && !hlist_empty(&mm->notifiers) &&
		    n < (int)ARRAY_SIZE(targets)) {
			mmgrab(mm);
			targets[n++] = mm;
		}
	spin_unlock_irqrestore(&kvm_mm_lock, flags);
	for (int i = 0; i < n; i++) {
		mm_invalidate(targets[i], start, end, blockable);
		mmdrop(targets[i]);
	}
	atomic_dec(&kvm_mm_callbacks);
}

/* From process teardown: the space is going away. Every subscriber is told
 * (release), and the mm leaves the registry so a new process that gets the
 * same PML4 frame gets a new mm. */
static void kvm_mm_release_hook(u64 pml4)
{
	struct mmu_notifier *subs[KVM_MM_MAX_SUBS];
	struct mm_struct *mm, *found = NULL;
	unsigned long flags;
	int n;

	if (list_empty(&kvm_mm_list))
		return;
	spin_lock_irqsave(&kvm_mm_lock, flags);
	list_for_each_entry(mm, &kvm_mm_list, node)
		if (mm->pml4_phys == pml4) {
			found = mm;
			list_del_init(&mm->node);
			break;
		}
	spin_unlock_irqrestore(&kvm_mm_lock, flags);
	if (!found)
		return;
	atomic_inc(&kvm_mm_callbacks);
	n = mm_subs(found, subs);
	for (int i = 0; i < n; i++)
		if (subs[i]->ops->release)
			subs[i]->ops->release(subs[i], found);
	atomic_dec(&kvm_mm_callbacks);
	found->pml4_phys = 0;
	atomic_set(&found->mm_users, 0);
	mmdrop(found);
}

void kvm_mm_hooks_fill(struct b1nix_kvm_hooks *h)
{
	h->mm_invalidate = kvm_mm_invalidate_hook;
	h->mm_release = kvm_mm_release_hook;
}

/* ── page tables ──────────────────────────────────────────────────────── */

/* KVM sizes its own mappings after the host's (host_pfn_mapping_level): a
 * guest range the process has mapped with a 2 MiB page (THP) goes into EPT or
 * NPT as one. With four-level paging an mm's top table is its PML4, which is
 * what the walk expects. Under LA57 b1nix keeps the process's PML4 below a
 * PML5 of its own, which the walk would have to start from; there it gets an
 * empty table and maps everything at 4 KiB -- slower, never wrong. The walk
 * runs with interrupts off, so a table freed after a shootdown cannot go away
 * under it. */
static pgd_t kvm_empty_pgd[512] __attribute__((aligned(4096)));

pgd_t *lkpi_mm_pgd(struct mm_struct *mm)
{
	if (!mm || !mm->pml4_phys || b1nix_kvm_la57())
		return kvm_empty_pgd;
	return (pgd_t *)(uintptr_t)(mm->pml4_phys + lkpi_direct_map_base());
}

int lkpi_paging_la57(void) { return b1nix_kvm_la57(); }
unsigned long lkpi_direct_map_base(void) { return (unsigned long)b1nix_kvm_direct_map_base(); }
unsigned long lkpi_virt_to_phys(const volatile void *va)
{
	return (unsigned long)b1nix_kvm_virt_to_phys((const void *)va);
}
unsigned long lkpi_zero_pfn(void) { return (unsigned long)(b1nix_kvm_zero_page_phys() >> PAGE_SHIFT); }

/* No frame of guest memory has a struct page here (see above); KVM's own
 * allocations never reach it through pfn_valid. */
bool pfn_valid(unsigned long pfn)
{
	(void)pfn;
	return false;
}

bool pat_pfn_immune_to_uc_mtrr(unsigned long pfn)
{
	u64 pa = (u64)pfn << PAGE_SHIFT;

	return b1nix_kvm_range_is_ram(pa, pa + PAGE_SIZE);
}

bool e820__mapped_raw_any(u64 start, u64 end, enum e820_type type)
{
	if (type != E820_TYPE_RAM)
		return false;
	return b1nix_kvm_range_is_ram(start, end);
}

/* ── the mapping around an address ────────────────────────────────────── */

/* One scratch vma per lkpi task slot: KVM uses the pointer only while it
 * holds mmap_lock, from the thread that looked it up. */
struct vm_area_struct *kvm_vma_lookup(struct mm_struct *mm, unsigned long addr)
{
	struct task_struct *t = lkpi_current();
	struct vm_area_struct *vma;
	u64 start, end;
	u32 prot;

	if (!mm || mm->pml4_phys != b1nix_kvm_current_pml4())
		return NULL;
	if (b1nix_kvm_vma(addr, &start, &end, &prot))
		return NULL;
	if (!t->kvm_vma) {
		t->kvm_vma = kzalloc(sizeof(struct vm_area_struct), GFP_KERNEL);
		if (!t->kvm_vma)
			return NULL;
	}
	vma = t->kvm_vma;
	memset(vma, 0, sizeof(*vma));
	vma->vm_start = start;
	vma->vm_end = end;
	vma->vm_mm = mm;
	vma->vm_flags = VM_PFNMAP | VM_IO |
			((prot & 1) ? VM_READ : 0) | ((prot & 2) ? VM_WRITE : 0) |
			((prot & 4) ? VM_EXEC : 0) | VM_SHARED;
	return vma;
}

struct vm_area_struct *kvm_find_vma(struct mm_struct *mm, unsigned long addr)
{
	return kvm_vma_lookup(mm, addr);
}

unsigned long vma_kernel_pagesize(struct vm_area_struct *vma)
{
	(void)vma;
	return PAGE_SIZE;
}

/* ── frames behind user addresses ─────────────────────────────────────── */

/* None of guest memory has a struct page, so the page-returning lookups find
 * nothing and KVM falls through to follow_pfnmap_start. */
int get_user_page_fast_only(unsigned long addr, unsigned int gup_flags,
			    struct page **pagep)
{
	(void)addr; (void)gup_flags; (void)pagep;
	return 0;
}

int get_user_pages_fast_only(unsigned long start, int nr_pages,
			     unsigned int gup_flags, struct page **pages)
{
	(void)start; (void)nr_pages; (void)gup_flags; (void)pages;
	return 0;
}

long get_user_pages_unlocked(unsigned long start, unsigned long nr_pages,
			     struct page **pages, unsigned int gup_flags)
{
	(void)start; (void)nr_pages; (void)pages; (void)gup_flags;
	return -EFAULT;
}

long get_user_pages_remote(struct mm_struct *mm, unsigned long start,
			   unsigned long nr_pages, unsigned int gup_flags,
			   struct page **pages, int *locked)
{
	(void)mm; (void)start; (void)nr_pages; (void)gup_flags; (void)pages; (void)locked;
	return -EFAULT;
}

long pin_user_pages_unlocked(unsigned long start, unsigned long nr_pages,
			     struct page **pages, unsigned int gup_flags)
{
	(void)start; (void)nr_pages; (void)pages; (void)gup_flags;
	return -EFAULT;
}

int pin_user_pages_fast(unsigned long start, int nr_pages,
			unsigned int gup_flags, struct page **pages)
{
	(void)start; (void)nr_pages; (void)gup_flags; (void)pages;
	return -EFAULT;
}

void unpin_user_page(struct page *page)
{
	(void)page;
}

int fixup_user_fault(struct mm_struct *mm, unsigned long address,
		     unsigned int fault_flags, bool *unlocked)
{
	if (unlocked)
		*unlocked = false;
	if (!mm || mm->pml4_phys != b1nix_kvm_current_pml4())
		return -EFAULT;
	return b1nix_kvm_fault_in(address, !!(fault_flags & FAULT_FLAG_WRITE));
}

/* The frame at args->address, and whether it may be written. A present page
 * that is read-only in a writable mapping is shared copy-on-write (or the
 * zero page); it is broken here, because a guest mapping of it would be one
 * the guest could never write through. */
int follow_pfnmap_start(struct follow_pfnmap_args *args)
{
	struct vm_area_struct *vma = args->vma;
	u64 pml4 = vma && vma->vm_mm ? vma->vm_mm->pml4_phys : b1nix_kvm_current_pml4();
	u64 pa;
	int writable;

	if (b1nix_kvm_user_lookup(pml4, args->address, &pa, &writable))
		return -EINVAL;
	if (!writable && vma && (vma->vm_flags & VM_WRITE) &&
	    pml4 == b1nix_kvm_current_pml4()) {
		if (b1nix_kvm_fault_in(args->address, 1) ||
		    b1nix_kvm_user_lookup(pml4, args->address, &pa, &writable))
			return -EINVAL;
	}
	args->pfn = (unsigned long)(pa >> PAGE_SHIFT);
	args->writable = writable != 0;
	args->special = true;
	args->lock = NULL;
	args->ptep = NULL;
	args->pgprot = PAGE_KERNEL;
	return 0;
}

void follow_pfnmap_end(struct follow_pfnmap_args *args)
{
	(void)args;
}

/* ── mappings KVM makes in the owner (its private memslots) ───────────── */

unsigned long kvm_vm_mmap(struct file *file, unsigned long addr, unsigned long len,
			  unsigned long prot, unsigned long flag, unsigned long offset)
{
	if (file || offset)
		return (unsigned long)-EINVAL;
	return (unsigned long)b1nix_kvm_mmap_anon(addr, len, (int)prot, (int)flag);
}

int vm_munmap(unsigned long start, size_t len)
{
	return b1nix_kvm_munmap(start, len);
}

/* ── user memory, small pieces ────────────────────────────────────────── */

int lkpi_cmpxchg_user(void __user *uaddr, void *old, u64 new, int size)
{
	u64 va = (u64)(unsigned long)uaddr, pa;
	int writable, ok = 0;
	void *p;

	if (size != 1 && size != 2 && size != 4 && size != 8)
		return -EFAULT;
	if (b1nix_kvm_fault_in(va, 1) ||
	    b1nix_kvm_user_lookup(b1nix_kvm_current_pml4(), va, &pa, &writable) || !writable)
		return -EFAULT;
	if ((va & (PAGE_SIZE - 1)) + size > PAGE_SIZE)
		return -EFAULT;
	p = (void *)(unsigned long)(pa + b1nix_kvm_direct_map_base());
	switch (size) {
	case 1: ok = __atomic_compare_exchange_n((u8 *)p, (u8 *)old, (u8)new, false,
						 __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); break;
	case 2: ok = __atomic_compare_exchange_n((u16 *)p, (u16 *)old, (u16)new, false,
						 __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); break;
	case 4: ok = __atomic_compare_exchange_n((u32 *)p, (u32 *)old, (u32)new, false,
						 __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); break;
	case 8: ok = __atomic_compare_exchange_n((u64 *)p, (u64 *)old, (u64)new, false,
						 __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); break;
	}
	return ok ? 0 : 1;
}

void *vmemdup_array_user(const void __user *src, size_t n, size_t size)
{
	size_t bytes;
	void *p;

	if (__builtin_mul_overflow(n, size, &bytes))
		return ERR_PTR(-EOVERFLOW);
	p = kvmalloc(bytes, GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);
	if (copy_from_user(p, src, bytes)) {
		kvfree(p);
		return ERR_PTR(-EFAULT);
	}
	return p;
}

void *__vcalloc(size_t n, size_t size, gfp_t flags)
{
	size_t bytes;

	if (__builtin_mul_overflow(n, size, &bytes))
		return NULL;
	return kvzalloc(bytes, flags);
}

/* ── Linux's interval tree, under its own names (see linux/interval_tree.h) */

#define ITREE_START(node) ((node)->start)
#define ITREE_LAST(node)  ((node)->last)
INTERVAL_TREE_DEFINE(struct interval_tree_node, rb, unsigned long, __subtree_last,
		     ITREE_START, ITREE_LAST, , linux_interval_tree)

/* Zero n bytes of user memory; the count not cleared, as upstream. */
unsigned long clear_user(void __user *to, unsigned long n)
{
	static const char zeros[256];

	while (n) {
		unsigned long c = n < sizeof(zeros) ? n : sizeof(zeros);

		if (lkpi_copy_to_user(to, zeros, c))
			return n;
		to = (char __user *)to + c;
		n -= c;
	}
	return 0;
}
