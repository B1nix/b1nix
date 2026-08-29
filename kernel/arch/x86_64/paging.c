#include <b1nix/console.h>
#include <b1nix/bootinfo.h>

#include <b1nix/user.h>
#include <b1nix/lockdep.h>
#include <b1nix/sched.h>
#include <b1nix/mm.h>
#include <b1nix/rwlock.h>
#include <b1nix/vfs.h>
#include <b1nix/page_cache.h>
#include <b1nix/panic.h>
#include <b1nix/klog.h>
#include <stdio.h>
#include <string.h>

/* F2 (M28 #7 prep): rwlock serialising page-table mutations across CPUs.
 * Today every kernel-mode entry holds the BKL so this is decorative; the
 * lock becomes load-bearing once the BKL teardown lands, at which point
 * two CPUs that both call vmm_map_page or vmm_unmap_page on the same
 * address space could race ensure_child_table / pt[idx] = entry and leak
 * an intermediate PT frame. Held as the writer for every map/unmap; the
 * page-fault read path stays lock-free for now (still BKL-protected).
 *
 * DAG position: HEAP (700) -> VMM (750) -> PMM (800). kheap_grow holds
 * heap_lock and calls vmm_map_page; the map path then allocates a PT
 * frame via pmm_alloc_frame. Reverse order is the kernel's normal
 * "kmalloc under vmm" pattern but never happens in practice — call sites
 * that need both lock VMM-first. */
static rwlock_t vmm_lock = RWLOCK_INIT;

static inline void vmm_write_acquire(u64 *flags) {
  rw_write_lock_irqsave(&vmm_lock, flags);
  LOCKDEP_ACQUIRE(LOCKDEP_LVL_VMM);
}
static inline void vmm_write_release(u64 flags) {
  LOCKDEP_RELEASE(LOCKDEP_LVL_VMM);
  rw_write_unlock_irqrestore(&vmm_lock, flags);
}

static inline void vmm_read_acquire(u64 *flags) {
  rw_read_lock_irqsave(&vmm_lock, flags);
  LOCKDEP_ACQUIRE(LOCKDEP_LVL_VMM);
}
static inline void vmm_read_release(u64 flags) {
  LOCKDEP_RELEASE(LOCKDEP_LVL_VMM);
  rw_read_unlock_irqrestore(&vmm_lock, flags);
}

#define PAGE_ENTRY_ADDRESS_MASK 0x000ffffffffff000ULL
#define PAGE_TABLE_INDEX_MASK 0x1ffULL
#define HUGE_PAGE_FLAG (1ULL << 7)
/* DIRECT_MAP_SIZE is shared with the pmm via <b1nix/mm.h> (the pmm clamps
 * usable RAM to it so no frame is ever allocated outside the direct map). */
#define MMIO_MAP_BASE 0xffffa00000000000ULL
#define MMIO_MAP_SIZE (512ULL * 1024ULL * 1024ULL)

static u64 *kernel_pml4_virt;
static u64 kernel_pml4_phys;
int direct_map_ready;
static u64 mmio_next = MMIO_MAP_BASE;


static u64 *get_current_pml4(void) {
  if (current_task && current_task->pml4_phys) {
    return (u64 *)(usize)(current_task->pml4_phys + DIRECT_MAP_BASE);
  }
  return kernel_pml4_virt;
}

static inline int is_canonical(u64 addr) {
  return ((isize)addr >> 47) == 0 || ((isize)addr >> 47) == -1;
}

static u64 read_cr3(void) {
  u64 value;

  __asm__ volatile("movq %%cr3, %0" : "=r"(value));
  return value;
}

static void write_cr3(u64 value) {
  __asm__ volatile("movq %0, %%cr3" : : "r"(value) : "memory");
}

/* Address-space epoch.
 *
 * Bumped every time an address space is created or destroyed. A CPU records
 * the epoch it saw when it loaded CR3; a later switch skips the CR3 write only
 * when both the PML4 frame AND the epoch still match what it recorded.
 *
 * Comparing the PML4 frame address alone — the obvious form of this
 * optimisation — is wrong: the frame that held one process's PML4 is freed and
 * handed to the next process for its own PML4. The address then matches while
 * the table behind it is a different address space entirely, and the skipped
 * write leaves the TLB full of the dead process's translations. That panicked
 * with a page-table entry out of range. The epoch closes exactly that hole: an
 * address space created or destroyed anywhere, on any CPU, makes every
 * recorded value stale, and the next switch writes CR3.
 *
 * Read with acquire AFTER the target PML4 has been read, so a CPU that obtains
 * a newly created space's PML4 cannot also observe the pre-creation epoch. */
static volatile u64 g_addrspace_epoch = 1;

static inline void addrspace_epoch_bump(void) {
  __atomic_add_fetch(&g_addrspace_epoch, 1, __ATOMIC_SEQ_CST);
}

/* A live translation is being taken away or pointed somewhere else.
 *
 * Writing CR3 on every context switch used to flush the whole TLB, and that
 * flush silently stood in for cross-CPU invalidation everywhere the kernel
 * edits page tables: unmap, swap-out, the copy-on-write flip, a protection
 * downgrade. invlpg only reaches the CPU running it, so once the switch stops
 * flushing, another core keeps using the old translation until something makes
 * it reload. Bumping the epoch is that something — every CPU's recorded load
 * becomes stale and its next switch writes CR3 again.
 *
 * Only replacements and removals need this. x86 does not cache non-present
 * entries, so filling a lazy or absent page in — the overwhelmingly common
 * page-fault case — needs no announcement, which is what keeps the skip
 * worthwhile. */
static inline int irqs_are_enabled(void) {
  u64 rflags;

  __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
  return (rflags & (1ull << 9)) != 0;
}

static inline void addrspace_note_replaced(u64 old_entry) {
  if (old_entry & VMM_PRESENT)
    addrspace_epoch_bump();
}

/* Drop every cached translation for the address space this CPU already runs
 * on. The fork paths need it: they edit the live page tables in place (the
 * parent's writable user pages become COW) and the stale writable entries have
 * to go. Those call sites used to get the flush by "switching" to the address
 * space that was already loaded, which the skip below now elides. */
void paging_reload_cr3(void) {
  u64 cr3;

  __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
  __asm__ volatile("movq %0, %%cr3" : : "r"(cr3) : "memory");
}

/* b1nix.no-cr3-skip: never elide the CR3 write on a context switch.
 *
 * The skip below is what makes the epoch scheme necessary, and the epoch only
 * takes effect at a switch. This flag turns the whole mechanism off so a run
 * can be compared against one with it on -- if a stale-translation symptom
 * survives with every switch flushing the TLB, the cause is a missing
 * cross-CPU shootdown somewhere and not the skip. Read once: this is the
 * hottest path in the scheduler. */
static int cr3_skip_disabled = -1;

void paging_switch_address_space(u64 pml4_phys) {
  u64 target_phys = pml4_phys ? pml4_phys : kernel_pml4_phys;

  if (cr3_skip_disabled < 0)
    cr3_skip_disabled = bootinfo_has_flag("b1nix.no-cr3-skip") ? 1 : 0;

  /* The per-CPU record has to be read, tested and written on the CPU whose CR3
   * this is. The scheduler calls in with interrupts already masked, but the
   * ELF loader and vmm_init do not, and a preemption between get_percpu() and
   * the store would file this CPU's CR3 under another CPU's record. Masking
   * locally costs a few cycles against a whole TLB. */
  u64 rflags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) : : "memory");

  struct percpu *pc = get_percpu();

  if (pc) {
    u64 epoch = __atomic_load_n(&g_addrspace_epoch, __ATOMIC_ACQUIRE);

    if (!cr3_skip_disabled && pc->loaded_pml4_phys == target_phys &&
        pc->loaded_addrspace_epoch == epoch) {
      /* Same address space, and none has been created or destroyed since it
       * was loaded: CR3 already holds it and every translation cached under it
       * is still this space's. Writing it again would throw the whole TLB away
       * for nothing — which is what a switch between two threads of one
       * process used to do, every time. */
      __asm__ volatile("pushq %0; popfq" : : "r"(rflags) : "memory", "cc");
      return;
    }

    __asm__ volatile("mov %0, %%cr3" : : "r"(target_phys) : "memory");
    pc->loaded_pml4_phys = target_phys;
    pc->loaded_addrspace_epoch = epoch;
  } else {
    /* Before per-CPU data exists (vmm_init on the BSP) there is nowhere to
     * record the load, so never skip. */
    __asm__ volatile("mov %0, %%cr3" : : "r"(target_phys) : "memory");
  }

  __asm__ volatile("pushq %0; popfq" : : "r"(rflags) : "memory", "cc");
}

static void invalidate_page(u64 virtual_address) {
  __asm__ volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

static usize pml4_index(u64 virtual_address) {
  return (virtual_address >> 39) & PAGE_TABLE_INDEX_MASK;
}

static usize pdpt_index(u64 virtual_address) {
  return (virtual_address >> 30) & PAGE_TABLE_INDEX_MASK;
}

static usize pd_index(u64 virtual_address) {
  return (virtual_address >> 21) & PAGE_TABLE_INDEX_MASK;
}

static usize pt_index(u64 virtual_address) {
  return (virtual_address >> 12) & PAGE_TABLE_INDEX_MASK;
}

static u64 *table_from_entry(u64 entry) {
  u64 phys = entry & PAGE_ENTRY_ADDRESS_MASK;
  if (direct_map_ready && phys < DIRECT_MAP_SIZE) {
    return (u64 *)(usize)(phys + DIRECT_MAP_BASE);
  }
  if (direct_map_ready) {
    /* Once the direct map exists, every real table is inside it. An entry
     * pointing outside is corruption, and the raw physical address returned
     * below is usually non-canonical — so the caller's very next load is a #GP
     * with no indication of where it came from. Say what the entry was
     * instead; the walk cannot continue either way. */
    console_write("paging: page-table entry out of range: 0x");
    console_write_hex64(entry);
    console_write("\n");
    panic("paging: corrupted page-table entry");
  }
  /* Before the direct map, tables live in the low identity window. */
  return (u64 *)(usize)phys;
}

static u64 table_to_phys(u64 *table) {
  u64 phys = (u64)(usize)table;
  if (phys >= DIRECT_MAP_BASE)
    phys -= DIRECT_MAP_BASE;
  return phys;
}

static u64 *alloc_page_table(void) {
  u64 frame = pmm_alloc_frame();
  if (frame == 0) {
    panic("vmm: OOM during page table allocation");
  }

  /* Page tables are dereferenced via the direct map once it is ready
   * (frame + DIRECT_MAP_BASE), so any frame below DIRECT_MAP_SIZE is
   * reachable. Before the direct map exists, only the low 4GB identity
   * window set up by the bootstrap (boot.S) is addressable. */
  u64 reachable_limit = direct_map_ready ? DIRECT_MAP_SIZE : 0x100000000ULL;
  if (frame >= reachable_limit) {
    panic("vmm: page table frame beyond reachable map");
  }

  u64 *table = (u64 *)(usize)frame;
  if (direct_map_ready && frame < DIRECT_MAP_SIZE) {
    table = (u64 *)(usize)(frame + DIRECT_MAP_BASE);
  }

  /* Claim the frame as a page table for as long as it is one, so the pmm can
   * refuse to hand it to a second owner (see pmm_note_page_table). */
  pmm_note_page_table(frame, 1);

  memset(table, 0, PAGE_SIZE);
  return table;
}

/* Turn a 2 MiB entry into a page table the caller already allocated. Splitting
 * inside a lock cannot allocate: alloc_page_table can reclaim, and reclaim
 * writes dirty pages back, which blocks with interrupts off — the CPU then
 * never answers a TLB shootdown and the initiator panics on the timeout. The
 * fault path allocates first and commits here. */
static void split_huge_page_into(u64 *pd, usize index, u64 *pt) {
  u64 entry = pd[index];
  u64 base = entry & PAGE_ENTRY_ADDRESS_MASK;
  u64 flags = (entry & ~PAGE_ENTRY_ADDRESS_MASK) & ~HUGE_PAGE_FLAG;
  u64 leaf_flags = flags & ~VMM_USER; /* see split_huge_page */

  for (usize i = 0; i < 512; i++)
    pt[i] = (base + i * PAGE_SIZE) | leaf_flags;
  pd[index] = table_to_phys(pt) | flags;
  /* The 2 MiB entry other CPUs may have cached no longer describes this
   * range on its own — and the leaves lose the user bit. */
  addrspace_note_replaced(entry);
}

static u64 *split_huge_page(u64 *pd, usize index) {
  u64 entry = pd[index];
  u64 base = entry & PAGE_ENTRY_ADDRESS_MASK;
  u64 flags = entry & ~PAGE_ENTRY_ADDRESS_MASK;
  flags &= ~HUGE_PAGE_FLAG;

  /* The 2MB huge page entry in pd may have had VMM_USER set before split.
   * Strip VMM_USER when creating the 512 4KB leaf PTEs so the 511 identity-mapped
   * neighbor PTEs remain supervisor-only. The caller (vmm_map_page_locked /
   * vmm_set_lazy) will explicitly add VMM_USER to the single leaf PTE it maps.
   * This prevents free_table() during process teardown from treating identity-mapped
   * physical memory frames as user allocations and wrongly freeing them to PMM. */
  u64 leaf_flags = flags & ~VMM_USER;

  u64 *pt = alloc_page_table();
  for (usize i = 0; i < 512; i++) {
    pt[i] = (base + i * PAGE_SIZE) | leaf_flags;
  }

  pd[index] = table_to_phys(pt) | flags;
  addrspace_note_replaced(entry); /* see split_huge_page_into */
  return pt;
}

/* A child table we can actually dereference, or NULL.
 *
 * table_from_entry() hands back the raw physical address when the entry points
 * outside the direct map, and dereferencing that from the higher half is a #GP
 * — not a page fault — so one corrupted entry crashes inside whichever
 * subsystem happened to walk it. (It did: a kernel PD holding file data made
 * vmm_virt_to_phys #GP inside the virtio-blk submit path, which reads as a
 * block-layer bug and is not one.) Read-only walkers use this and report a
 * failed translation instead of taking the fault. */
static u64 *reachable_table(u64 entry) {
  u64 phys = entry & PAGE_ENTRY_ADDRESS_MASK;
  if (direct_map_ready && phys >= DIRECT_MAP_SIZE) {
    return 0;
  }
  return table_from_entry(entry);
}

static u64 *ensure_child_table(u64 *parent, usize index) {
  /* Physical address zero is never a page table — it is the bottom of memory,
   * where the BIOS data area and early kernel structures live. A parent that
   * lands exactly on the start of the direct map means the caller followed an
   * entry whose address bits were zero, and continuing reads those bytes as
   * entries (the "AUX A/D" i915 strings a walk has reported more than once).
   * Refuse it here, one level before the damage, and name the caller. */
  if (direct_map_ready && (u64)parent == DIRECT_MAP_BASE) {
    console_write("ensure_child_table: parent is physical 0 (parent=0x");
    console_write_hex64((u64)(usize)parent);
    console_write(" task pml4=0x");
    console_write_hex64(current_task ? current_task->pml4_phys : 0);
    console_write(" kernel pml4 virt=0x");
    console_write_hex64((u64)(usize)kernel_pml4_virt);
    console_write("), called from 0x");
    console_write_hex64((u64)(usize)__builtin_return_address(0));
    ksym_print((u64)(usize)__builtin_return_address(0));
    if (current_task) {
      console_write("\n  task ");
      console_write_dec(current_task->id);
      console_write(" ");
      console_write(current_task->name);
    }
    console_write("\n");
    panic("paging: walked into physical address 0 as a page table");
  }
  if (!parent || (direct_map_ready && (u64)parent < DIRECT_MAP_BASE) || !is_canonical((u64)parent)) {
    console_write("ensure_child_table: INVALID parent: 0x");
    console_write_hex64((u64)parent);
    console_write(" index: ");
    console_write_hex64(index);
    console_write(" caller: 0x");
    console_write_hex64((u64)__builtin_return_address(0));
    console_write("\n");
    if (current_task) {
      console_write("current_task: ");
      console_write(current_task->name);
      console_write(" (id: ");
      console_write_dec(current_task->id);
      console_write(")\n");
    } else {
      console_write("current_task is NULL\n");
    }
    panic("ensure_child_table: invalid parent pointer");
  }
  u64 *result;
  if ((parent[index] & VMM_PRESENT) == 0) {
    u64 *child = alloc_page_table();
    u64 want = table_to_phys(child) | VMM_PRESENT | VMM_WRITABLE;
    /* A present entry addressing frame 0 is the bug we keep arriving at from
     * the far end — a later walk follows it into the bottom of memory and reads
     * BIOS bytes as page-table entries. alloc_page_table never returns frame 0,
     * so if this is ever about to be published, say so here rather than three
     * subsystems away. */
    if ((want & PAGE_ENTRY_ADDRESS_MASK) == 0) {
      console_write("ensure_child_table: about to publish a null table at index ");
      console_write_dec(index);
      console_write(" (child=0x");
      console_write_hex64((u64)(usize)child);
      console_write(") from 0x");
      console_write_hex64((u64)(usize)__builtin_return_address(0));
      ksym_print((u64)(usize)__builtin_return_address(0));
      console_write("\n");
      panic("paging: page-table entry would address frame 0");
    }
    /* Whatever is in the slot now — which is NOT necessarily zero. The branch
     * we are in only established that PRESENT is clear, and a slot can hold a
     * non-present value with meaning: VMM_LAZY marks a reserved page, and the
     * USER bit is set on intermediate levels independently. Comparing against a
     * hard-coded 0 made the exchange fail on exactly those slots, and the loser
     * path then read that non-present value as a table pointer — address zero,
     * i.e. the bottom of physical memory — while also handing its own frame
     * back, which is where "page table freed while still mapped" and the double
     * frees came from. */
    u64 expected = __atomic_load_n(&parent[index], __ATOMIC_ACQUIRE);

    /*
     * Publish the new table only if the slot is still empty, and take the
     * winner's table if it is not.
     *
     * A plain store loses races that a multithreaded process reaches easily:
     * two threads calling mmap at once both find the entry absent, both
     * allocate, and the second store overwrites the first — leaving one table
     * unreachable while threads already walk through the other. Worse, the
     * `|= VMM_USER` updates around the callers are read-modify-write, so a
     * store that lands between another thread's read and write is simply
     * discarded, and the surviving entry can name neither table. A later walk
     * follows it into an unrelated physical page and reads those bytes as
     * page-table entries — which is exactly what a walk reported when it found
     * entries made of ASCII and a parent table at physical address 0.
     *
     * The compare-exchange keeps this correct without holding the VMM write
     * lock across the allocation: taking that lock here re-introduced the
     * huge-page split under the lock, which has its own history of shootdown
     * timeouts.
     */
    if (__atomic_compare_exchange_n(&parent[index], &expected, want, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
      result = child;
    } else {
      /* Someone else won. Their entry is the live one; give ours back — and
       * drop its page-table claim first, or the allocator refuses a frame that
       * still says it is a live table. */
      u64 loser = table_to_phys(child);
      pmm_note_page_table(loser, 0);
      pmm_free_frame(loser);
      /* The exchange failed because the slot changed under us. It is only a
       * table if the value that beat us says PRESENT; a non-present value means
       * another writer put something else there (a lazy marker, a flag bit), and
       * the slot still needs a table — so start over rather than treat that
       * value as a pointer. */
      if (expected & VMM_PRESENT)
        result = (expected & HUGE_PAGE_FLAG) ? split_huge_page(parent, index)
                                             : table_from_entry(expected);
      else
        result = ensure_child_table(parent, index);
    }
  } else if ((parent[index] & HUGE_PAGE_FLAG) != 0) {
    result = split_huge_page(parent, index);
  } else {
    /*
     * A present entry whose address is outside the direct map is not a table at
     * all — the parent's page has been reused for something else and we are
     * reading that other owner's bytes. Which owner, and when it took the page,
     * is the whole question, and only the parent's own frame can answer it: name
     * it and print its allocation history before the walk gives up.
     */
    extern void pmm_report_page_table_history_pub(u64 frame);
    u64 child_phys = parent[index] & PAGE_ENTRY_ADDRESS_MASK;
    if (direct_map_ready && child_phys >= DIRECT_MAP_SIZE) {
      u64 parent_phys = (u64)(usize)parent - DIRECT_MAP_BASE;

      console_write("ensure_child_table: called from 0x");
      console_write_hex64((u64)(usize)__builtin_return_address(0));
      ksym_print((u64)(usize)__builtin_return_address(0));
      console_write("\n  parent table 0x");
      console_write_hex64(parent_phys);
      console_write(" index ");
      console_write_dec(index);
      console_write(" holds a non-table entry 0x");
      console_write_hex64(parent[index]);
      console_write("\n  history of the parent frame:\n");
      pmm_report_page_table_history_pub(parent_phys);
      if (current_task) {
        console_write("  task: ");
        console_write(current_task->name);
        console_write(" id ");
        console_write_dec(current_task->id);
        console_write("\n");
      }
    }
    result = table_from_entry(parent[index]);
  }

  if (direct_map_ready && ((u64)result < DIRECT_MAP_BASE || !is_canonical((u64)result))) {
    console_write("ensure_child_table: returning INVALID pointer: 0x");
    console_write_hex64((u64)result);
    console_write(" parent: 0x");
    console_write_hex64((u64)parent);
    console_write(" index: ");
    console_write_hex64(index);
    console_write(" entry: 0x");
    console_write_hex64(parent[index]);
    console_write("\n");
  }
  return result;
}

void vmm_map_page_in_table(u64 *pml4, u64 virtual_address, u64 physical_address, u64 flags) {
  u64 *pdpt = ensure_child_table(pml4, pml4_index(virtual_address));
  u64 *pd = ensure_child_table(pdpt, pdpt_index(virtual_address));

  /* Every level above a user leaf has to permit user access, or the CPU
   * refuses the access at the first supervisor entry it meets and reports a
   * protection violation on a page that looks perfectly mapped. The
   * current-space mapper does this; this one, which maps into another
   * process's tables, did not. */
  if ((flags & VMM_USER) != 0) {
    __atomic_or_fetch(&pml4[pml4_index(virtual_address)], VMM_USER, __ATOMIC_SEQ_CST);
    __atomic_or_fetch(&pdpt[pdpt_index(virtual_address)], VMM_USER, __ATOMIC_SEQ_CST);
  }

  u64 *pt;
  if ((pd[pd_index(virtual_address)] & HUGE_PAGE_FLAG) != 0) {
    pt = split_huge_page(pd, pd_index(virtual_address));
  } else {
    pt = ensure_child_table(pd, pd_index(virtual_address));
  }
  if ((flags & VMM_USER) != 0)
    __atomic_or_fetch(&pd[pd_index(virtual_address)], VMM_USER, __ATOMIC_SEQ_CST);

  addrspace_note_replaced(pt[pt_index(virtual_address)]);
  pt[pt_index(virtual_address)] =
      (physical_address & PAGE_ENTRY_ADDRESS_MASK) | flags | VMM_PRESENT;
  invalidate_page(virtual_address);
  /* Every user page install, when the run asks for them. This is the only
   * edit that leaves no mapping call behind it, so a range that holds pages
   * with no mmap to account for is either explained here or by nothing. */
  {
    extern int vma_trace_faults_enabled(void);
    extern void vma_trace_record(const char *what, u64 start, u64 end);

    if (flags & VMM_USER) {
      if (vma_trace_faults_enabled())
        vma_trace_record("map-page", virtual_address, virtual_address + PAGE_SIZE);
    }
  }
}

void vmm_init(void) {
  u64 phys_pml4 = pmm_alloc_frame();
  u64 *pml4 = (u64 *)(usize)phys_pml4;
  memset(pml4, 0, PAGE_SIZE);

  /* Map higher half (direct map) AND identity map using huge pages (2MB) */
  for (u64 physical = 0; physical < DIRECT_MAP_SIZE; physical += 0x200000ULL) {
    u64 virtual_high = DIRECT_MAP_BASE + physical;

    /* Higher-half mapping */
    u64 *pdpt_h = ensure_child_table(pml4, pml4_index(virtual_high));
    u64 *pd_h = ensure_child_table(pdpt_h, pdpt_index(virtual_high));
    pd_h[pd_index(virtual_high)] = physical | VMM_PRESENT | VMM_WRITABLE | (1ULL << 7);

    /* Identity mapping (for transition and physical access) */
    u64 *pdpt_i = ensure_child_table(pml4, pml4_index(physical));
    u64 *pd_i = ensure_child_table(pdpt_i, pdpt_index(physical));
    pd_i[pd_index(physical)] = physical | VMM_PRESENT | VMM_WRITABLE | (1ULL << 7);
  }

  /* Higher-half kernel window: map KERNEL_VMA (0xFFFFFFFF80000000) -> phys 0..
   * so the kernel — which is LINKED at KERNEL_VMA — stays mapped after we switch
   * CR3 to this pml4 below. Mirrors boot.S (pdpt[510] -> pd covering phys
   * 0-1GiB). 1 GiB is ample for the kernel image; this window is also what keeps
   * the kernel's data out of the low VA range userspace maps into. */
  for (u64 physical = 0; physical < 0x40000000ULL; physical += 0x200000ULL) {
    u64 virt = KERNEL_VMA + physical;
    u64 *pdpt_k = ensure_child_table(pml4, pml4_index(virt));
    u64 *pd_k = ensure_child_table(pdpt_k, pdpt_index(virt));
    pd_k[pd_index(virt)] = physical | VMM_PRESENT | VMM_WRITABLE | (1ULL << 7);
  }

  /* Pre-allocate page tables for the kernel heap in the higher half */
  u64 *pdpt_kheap = ensure_child_table(pml4, pml4_index(KHEAP_START));
  ensure_child_table(pdpt_kheap, pdpt_index(KHEAP_START));

  console_write("vmm: direct map 0x");
  console_write_hex64(DIRECT_MAP_BASE);
  console_write("-0x");
  console_write_hex64(DIRECT_MAP_BASE + DIRECT_MAP_SIZE);
  console_write("\n");

  /* Switch to new page table */
  paging_switch_address_space(phys_pml4);

  /* Now that direct map is ready, we can use virtual addresses for the PML4 */
  direct_map_ready = 1;
  kernel_pml4_virt = (u64 *)(usize)(phys_pml4 + DIRECT_MAP_BASE);
  kernel_pml4_phys = phys_pml4;

  extern void pmm_switch_to_direct_map(void);
  pmm_switch_to_direct_map();

  /* Arm the boot stack's guard now that the kernel window is ours. See the
   * comment on boot_stack_guard in boot.S: without this an overflow of the boot
   * stack is silent, because everything under it is ordinary writable .bss.
   * The guard spans several pages so no single stack frame can step over it.
   *
   * Counted and reported, not assumed. A guard that quietly failed to install
   * is indistinguishable from one that was never needed, and this whole bug was
   * a thing that failed without saying so. */
  extern u8 boot_stack_guard[];
  extern u8 boot_stack_guard_end[];
  usize guard_want = 0, guard_got = 0;
  for (u64 p = (u64)(usize)boot_stack_guard; p < (u64)(usize)boot_stack_guard_end;
       p += PAGE_SIZE) {
    guard_want++;
    guard_got += (usize)paging_install_guard_page(p);
  }
  console_write("vmm: boot-stack guard ");
  console_write_dec(guard_got);
  console_write("/");
  console_write_dec(guard_want);
  console_write(guard_got == guard_want ? " pages unmapped\n"
                                        : " pages unmapped — GUARD INCOMPLETE\n");

  /* And the syscall entry stack, which sits immediately above the boot stack
   * and so overflows INTO it. Same tripwire, reported the same way. */
  extern u8 x86_syscall_stack_guard[];
  extern u8 x86_syscall_stack_guard_end[];
  usize sc_want = 0, sc_got = 0;
  for (u64 p = (u64)(usize)x86_syscall_stack_guard;
       p < (u64)(usize)x86_syscall_stack_guard_end; p += PAGE_SIZE) {
    sc_want++;
    sc_got += (usize)paging_install_guard_page(p);
  }
  console_write("vmm: syscall-stack guard ");
  console_write_dec(sc_got);
  console_write("/");
  console_write_dec(sc_want);
  console_write(sc_got == sc_want ? " pages unmapped\n"
                                  : " pages unmapped — GUARD INCOMPLETE\n");
}

/* Unmap exactly one page from the kernel window, leaving its neighbours alone.
 *
 * vmm_unmap_page() cannot be used for this: the kernel window is mapped with
 * 2 MiB huge pages, and its huge-page branch clears the whole PDE — which would
 * unmap two megabytes of kernel image rather than one guard page. So split the
 * huge page first and then clear the single leaf.
 *
 * Only the kernel window alias is removed. The same physical page is still
 * reachable through the identity and direct maps, which is fine: this is a
 * tripwire for a stack that grows down through the kernel window, not a
 * guarantee that the frame is unreachable.
 *
 * Returns 1 when the page is now unmapped, 0 when it could not be — the caller
 * reports the tally rather than trusting it. */
int paging_install_guard_page(u64 virtual_address) {
  if ((virtual_address & (PAGE_SIZE - 1)) != 0)
    panic("paging_install_guard_page requires a page-aligned address");

  u64 *pml4 = get_current_pml4();
  u64 pml4e = pml4[pml4_index(virtual_address)];
  if ((pml4e & VMM_PRESENT) == 0)
    return 1; /* already not mapped here */
  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(virtual_address)];
  if ((pdpte & VMM_PRESENT) == 0)
    return 1;
  if ((pdpte & HUGE_PAGE_FLAG) != 0)
    return 0; /* a 1 GiB leaf is not something to split for a tripwire */
  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(virtual_address)];
  if ((pde & VMM_PRESENT) == 0)
    return 1;

  u64 *pt = (pde & HUGE_PAGE_FLAG) ? split_huge_page(pd, pd_index(virtual_address))
                                   : table_from_entry(pde);
  if (!pt)
    return 0;
  u64 old = pt[pt_index(virtual_address)];
  pt[pt_index(virtual_address)] = 0;
  addrspace_note_replaced(old);
  invalidate_page(virtual_address);
  return 1;
}

/* Build the page-table path for virtual_address and install the leaf PTE.
 * Caller MUST already hold vmm_lock (write). No allocation here blocks
 * (alloc_page_table uses the non-blocking pmm fast path), so this is safe in an
 * IRQs-off critical section. Split out of vmm_map_page so the page-fault
 * handler — which also holds vmm_lock at commit time — can install without
 * re-entering the non-recursive rwlock. */
static void vmm_map_page_locked(u64 virtual_address, u64 physical_address,
                                u64 flags) {
  u64 *pml4 = get_current_pml4();
  u64 *pdpt = ensure_child_table(pml4, pml4_index(virtual_address));
  u64 *pd = ensure_child_table(pdpt, pdpt_index(virtual_address));
  if ((flags & VMM_USER) != 0) {
    __atomic_or_fetch(&pml4[pml4_index(virtual_address)], VMM_USER, __ATOMIC_SEQ_CST);
    __atomic_or_fetch(&pdpt[pdpt_index(virtual_address)], VMM_USER, __ATOMIC_SEQ_CST);
  }

  u64 *pt;
  if ((pd[pd_index(virtual_address)] & HUGE_PAGE_FLAG) != 0) {
    pt = split_huge_page(pd, pd_index(virtual_address));
  } else {
    pt = ensure_child_table(pd, pd_index(virtual_address));
  }
  if ((flags & VMM_USER) != 0) {
    __atomic_or_fetch(&pd[pd_index(virtual_address)], VMM_USER, __ATOMIC_SEQ_CST);
  }
  addrspace_note_replaced(pt[pt_index(virtual_address)]);
  pt[pt_index(virtual_address)] =
      (physical_address & PAGE_ENTRY_ADDRESS_MASK) | flags | VMM_PRESENT;
  invalidate_page(virtual_address);
}

void vmm_map_page(u64 virtual_address, u64 physical_address, u64 flags) {
  if ((virtual_address & (PAGE_SIZE - 1)) != 0 ||
      (physical_address & (PAGE_SIZE - 1)) != 0) {
    panic("vmm_map_page requires page-aligned addresses");
  }

  if (kernel_pml4_virt == 0) {
    panic("vmm_map_page called before vmm_init");
  }

  u64 _vmflags;
  vmm_write_acquire(&_vmflags);
  vmm_map_page_locked(virtual_address, physical_address, flags);
  vmm_write_release(_vmflags);

  if ((flags & VMM_USER) && (flags & VMM_PRESENT)) {
    extern void eviction_register_page(struct task *task, u64 vaddr, u64 frame);
    eviction_register_page(current_task, virtual_address, physical_address);
  }
}

/* Map a run of frames under ONE lock acquisition.
 *
 * vmm_map_page takes the VMM write lock per page. A large kernel allocation —
 * the buffer behind a browser's shared-memory segment, say — is tens of
 * thousands of pages, so it was tens of thousands of irqsave round trips
 * against CPUs that are handling faults meanwhile. The mapping work per page
 * is unchanged; only the locking leaves the loop. */
void vmm_map_range(u64 base, const u64 *frames, usize n, u64 flags) {
  u64 _vmflags;

  if (!frames || n == 0)
    return;
  vmm_write_acquire(&_vmflags);
  for (usize i = 0; i < n; i++)
    vmm_map_page_locked(base + i * PAGE_SIZE, frames[i], flags);
  vmm_write_release(_vmflags);
}

void *vmm_map_mmio(u64 physical_address, usize size, u64 flags) {
  if (size == 0) {
    return 0;
  }

  u64 phys_base = physical_address & ~(PAGE_SIZE - 1);
  u64 phys_offset = physical_address - phys_base;
  u64 total_size = (u64)size + phys_offset;
  u64 map_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  u64 virt_base = (mmio_next + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  if (virt_base < MMIO_MAP_BASE ||
      virt_base + map_size > MMIO_MAP_BASE + MMIO_MAP_SIZE) {
    panic("vmm_map_mmio exhausted mmio virtual range");
  }

  for (u64 off = 0; off < map_size; off += PAGE_SIZE) {
    vmm_map_page(virt_base + off, phys_base + off, flags | VMM_PRESENT);
  }

  mmio_next = virt_base + map_size;
  return (void *)(usize)(virt_base + phys_offset);
}

/* Clear one leaf entry.
 *
 * `defer` decides who releases the frame. With NULL the frame goes back to the
 * allocator here, which is only safe when no other CPU can still be holding
 * the translation — a mapping being torn down in a space nobody runs in, or a
 * page that was never published. Otherwise pass a slot: the frame is reported
 * instead of freed, and the caller releases it AFTER its shootdown. A frame
 * handed back while another core's TLB still names it is a write into whatever
 * the allocator gives that frame to next. */
static void unmap_page_from_pml4(u64 *pml4, u64 virtual_address, u64 *defer) {
  if ((virtual_address & (PAGE_SIZE - 1)) != 0) {
    panic("vmm_unmap_page requires page-aligned address");
  }

  u64 pml4e = pml4[pml4_index(virtual_address)];
  if ((pml4e & VMM_PRESENT) == 0) {
    return;
  }

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(virtual_address)];
  if ((pdpte & VMM_PRESENT) == 0) {
    return;
  }

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(virtual_address)];
  if ((pde & VMM_PRESENT) == 0) {
    return;
  }

  if ((pde & HUGE_PAGE_FLAG) != 0) {
    pd[pd_index(virtual_address)] = 0;
    addrspace_note_replaced(pde);
    invalidate_page(virtual_address);
    return;
  }

  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(virtual_address)];
  if (pte & VMM_PRESENT) {
    u64 frame = pte & PAGE_ENTRY_ADDRESS_MASK;
    if (frame) {
      if (pte & VMM_USER) {
        if (defer)
          *defer = frame;
        else
          pmm_free_frame(frame);
        extern void eviction_unregister_page(u64 frame);
        eviction_unregister_page(frame);
      }
    }
  } else if (pte & VMM_SWAPPED) {
    /* The page is swapped out — release its swap slot (the slot index is encoded
     * in the PTE address field). Done here, incrementally per unmapped page, so
     * address-space teardown (the VMA-unmap loop) and munmap of a swapped range
     * both reclaim slots without any separate page-table walk. */
    extern void swap_free_slot_index(u32 slot);
    swap_free_slot_index((u32)((pte & PAGE_ENTRY_ADDRESS_MASK) >> 12));
  }
  pt[pt_index(virtual_address)] = 0;
  addrspace_note_replaced(pte);
  invalidate_page(virtual_address);
}

void vmm_unmap_page(u64 virtual_address) {
  u64 _vmflags;
  u64 defer = 0;

  vmm_write_acquire(&_vmflags);
  unmap_page_from_pml4(get_current_pml4(), virtual_address, &defer);
  vmm_write_release(_vmflags);
  /* M28 #5: every other CPU that has cached this translation needs to drop
   * it before we return; otherwise a write through their stale TLB entry hits
   * the (potentially freed-and-reused) physical frame. tlb_shootdown_page is
   * a no-op when g_max_cpus <= 1 so single-CPU boots pay nothing. Issued
   * with vmm_lock released so a target CPU that just took vmm_lock as a
   * reader doesn't block the shootdown ACK. */
  extern void tlb_shootdown_page(u64);
  tlb_shootdown_page(virtual_address);
  /* After the flush, never before. */
  if (defer)
    pmm_free_frame(defer);
}

void vmm_unmap_page_nosync(u64 virtual_address) {
  u64 _vmflags;
  vmm_write_acquire(&_vmflags);
  unmap_page_from_pml4(get_current_pml4(), virtual_address, 0);
  vmm_write_release(_vmflags);
}

void paging_unmap_page_from_space(u64 pml4_phys, u64 virtual_address) {
  u64 _vmflags;
  vmm_write_acquire(&_vmflags);
  u64 *pml4 = pml4_phys ? (u64 *)(usize)(pml4_phys + DIRECT_MAP_BASE)
                         : kernel_pml4_virt;
  unmap_page_from_pml4(pml4, virtual_address, 0);
  vmm_write_release(_vmflags);
}

/* Unmap a whole run from another address space under ONE lock acquisition.
 *
 * Teardown walks every page of every mapping, and taking the VMM write lock per
 * page turned a browser's exit into hundreds of thousands of lock round trips
 * against CPUs that are still faulting — minutes of wall clock, most of it
 * spent handing the lock back and forth. The work per page is the same; only
 * the locking moves out of the loop. The caller still issues one shootdown for
 * the whole teardown, as before. */
void paging_unmap_range_from_space(u64 pml4_phys, u64 base, usize npages) {
  u64 _vmflags;
  u64 *pml4 = pml4_phys ? (u64 *)(usize)(pml4_phys + DIRECT_MAP_BASE)
                        : kernel_pml4_virt;
  u64 end = base + (u64)npages * PAGE_SIZE;

  /* Skip the holes instead of walking them.
   *
   * A mapping is not the same thing as memory: a browser reserves address
   * space in terabytes and touches a fraction of it, and one of its processes
   * asked to release 302 million pages of which almost none were present.
   * Visiting each of those in turn is a walk down four levels per page for
   * nothing. When a level is absent, everything below it is absent too, so the
   * scan jumps the whole 512 GiB, 1 GiB or 2 MiB that entry covers. */
  vmm_write_acquire(&_vmflags);
  for (u64 va = base; va < end;) {
    u64 pml4e = pml4[pml4_index(va)];

    if (!(pml4e & VMM_PRESENT)) {
      va = (va + (1ULL << 39)) & ~((1ULL << 39) - 1);
      continue;
    }

    u64 *pdpt = table_from_entry(pml4e);
    u64 pdpte = pdpt[pdpt_index(va)];

    if (!(pdpte & VMM_PRESENT)) {
      va = (va + (1ULL << 30)) & ~((1ULL << 30) - 1);
      continue;
    }
    if (pdpte & HUGE_PAGE_FLAG) {
      unmap_page_from_pml4(pml4, va, 0);
      va = (va + (1ULL << 30)) & ~((1ULL << 30) - 1);
      continue;
    }

    u64 *pd = table_from_entry(pdpte);
    u64 pde = pd[pd_index(va)];

    if (!(pde & VMM_PRESENT)) {
      va = (va + (1ULL << 21)) & ~((1ULL << 21) - 1);
      continue;
    }
    if (pde & HUGE_PAGE_FLAG) {
      unmap_page_from_pml4(pml4, va, 0);
      va = (va + (1ULL << 21)) & ~((1ULL << 21) - 1);
      continue;
    }

    /* A present page table: clear the entries this range covers inside it,
     * then move to the next table. */
    u64 *pt = table_from_entry(pde);
    u64 tab_end = (va + (1ULL << 21)) & ~((1ULL << 21) - 1);

    if (tab_end > end)
      tab_end = end;
    for (; va < tab_end; va += PAGE_SIZE) {
      if (pt[pt_index(va)])
        unmap_page_from_pml4(pml4, va, 0);
    }
  }
  vmm_write_release(_vmflags);
}

/* Change one page's protection in ANOTHER address space (the current one
 * included). paging_mprotect_page only ever touches the loaded space; the
 * futex watchpoint needs to name the space explicitly. */
void paging_mprotect_page_in_space(u64 pml4_phys, u64 vaddr, u64 flags) {
  u64 _vmflags;
  u64 *pml4 = pml4_phys ? (u64 *)(usize)(pml4_phys + DIRECT_MAP_BASE)
                        : kernel_pml4_virt;

  vmm_write_acquire(&_vmflags);
  u64 pml4e = pml4[pml4_index(vaddr)];

  if (pml4e & VMM_PRESENT) {
    u64 *pdpt = table_from_entry(pml4e);
    u64 pdpte = pdpt[pdpt_index(vaddr)];

    if ((pdpte & VMM_PRESENT) && !(pdpte & HUGE_PAGE_FLAG)) {
      u64 *pd = table_from_entry(pdpte);
      u64 pde = pd[pd_index(vaddr)];

      if ((pde & VMM_PRESENT) && !(pde & HUGE_PAGE_FLAG)) {
        u64 *pt = table_from_entry(pde);
        u64 pte = pt[pt_index(vaddr)];

        if (pte & VMM_PRESENT) {
          u64 keep = pte & (PAGE_ENTRY_ADDRESS_MASK | VMM_PRESENT | VMM_COW);

          pt[pt_index(vaddr)] = keep | (flags & ~PAGE_ENTRY_ADDRESS_MASK);
          invalidate_page(vaddr);
        }
      }
    }
  }
  vmm_write_release(_vmflags);
}

void paging_map_page(u64 virtual_address, u64 physical_address, u64 flags) {
  vmm_map_page(virtual_address, physical_address, flags);
}

void paging_unmap_page(u64 virtual_address) { vmm_unmap_page(virtual_address); }

/* Change protection over a whole range in one walk.
 *
 * Doing it a page at a time meant a four-level descent per page, an epoch bump
 * per page — each of which makes every other CPU reload CR3 at its next switch
 * — and an invlpg per page. Chromium's allocator protects and unprotects large
 * spans constantly: measured at 181 ms per mprotect call. This walks the tables
 * once, skips whole 512 GiB / 1 GiB / 2 MiB spans that have nothing mapped in
 * them, and announces the change once at the end. */
void paging_mprotect_range(u64 start, u64 end, u64 flags) {
  extern void tlb_shootdown_all(void);
  u64 _vmflags;
  u64 *pml4 = get_current_pml4();
  int touched = 0;

  /* The lock is taken per page table, not for the whole range.
   *
   * Holding the write lock across a multi-gigabyte mprotect blocks every page
   * fault on every other CPU for as long as it takes — four browser threads
   * were seen queued behind one such call. A 2 MiB table is a short enough
   * critical section, and the walk still costs one descent per table. */
  for (u64 va = start; va < end;) {
    vmm_write_acquire(&_vmflags);
    u64 pml4e = pml4[pml4_index(va)];

    if (!(pml4e & VMM_PRESENT)) {
      vmm_write_release(_vmflags);
      va = (va + (1ULL << 39)) & ~((1ULL << 39) - 1);
      continue;
    }

    u64 *pdpt = table_from_entry(pml4e);
    u64 pdpte = pdpt[pdpt_index(va)];

    if (!(pdpte & VMM_PRESENT) || (pdpte & HUGE_PAGE_FLAG)) {
      vmm_write_release(_vmflags);
      va = (va + (1ULL << 30)) & ~((1ULL << 30) - 1);
      continue;
    }

    u64 *pd = table_from_entry(pdpte);
    u64 pde = pd[pd_index(va)];

    if (!(pde & VMM_PRESENT) || (pde & HUGE_PAGE_FLAG)) {
      vmm_write_release(_vmflags);
      va = (va + (1ULL << 21)) & ~((1ULL << 21) - 1);
      continue;
    }

    u64 *pt = table_from_entry(pde);
    u64 tab_end = (va + (1ULL << 21)) & ~((1ULL << 21) - 1);

    if (tab_end > end)
      tab_end = end;
    for (; va < tab_end; va += PAGE_SIZE) {
      u64 pte = pt[pt_index(va)];

      if (!(pte & VMM_PRESENT)) {
        /* A lazy or swapped leaf carries the protection the fault handler will
         * use when it materialises the page, so the new flags have to be
         * written there too — skipping it left the page to come back with the
         * protection mprotect had just replaced. */
        if (pte & (VMM_LAZY | VMM_SWAPPED)) {
          pt[pt_index(va)] =
              (pte & (PAGE_ENTRY_ADDRESS_MASK | VMM_LAZY | VMM_SWAPPED)) | flags;
          invalidate_page(va);
          touched = 1;
        }
        continue;
      }

      u64 nf = flags;

      /* The shared zero page never becomes directly writable, and neither does
       * a copy-on-write page: the first store makes the private copy. Same two
       * rules paging_mprotect_page applies. */
      if ((nf & VMM_WRITABLE) &&
          (pte & PAGE_ENTRY_ADDRESS_MASK) == pmm_zero_page()) {
        nf &= ~VMM_WRITABLE;
        nf |= VMM_COW;
      }
      if ((pte & VMM_COW) && (nf & VMM_WRITABLE)) {
        nf &= ~VMM_WRITABLE;
        nf |= VMM_COW;
      }

      u64 entry = (pte & PAGE_ENTRY_ADDRESS_MASK) | nf | VMM_PRESENT;

      if (entry == pte)
        continue;
      pt[pt_index(va)] = entry;
      invalidate_page(va);
      touched = 1;
    }
    vmm_write_release(_vmflags);
  }

  if (touched) {
    /* One announcement for the range, not one per page. */
    addrspace_note_replaced(VMM_PRESENT);
    if (irqs_are_enabled())
      tlb_shootdown_all();
  }
}

void paging_mprotect_page(u64 virtual_address, u64 flags) {
  if ((virtual_address & (PAGE_SIZE - 1)) != 0) {
    panic("paging_mprotect_page requires page-aligned address");
  }

  u64 *pml4 = get_current_pml4();
  /* The top-level frame must still be claimed as a page table. When it is not,
   * this address space has already been released and its frames re-issued —
   * the entries below are whatever the new owner wrote. Reading on yields a
   * corrupt entry with nothing to attribute it to; stop while the task that
   * owns the walk is still named. */
  {
    u64 pml4_phys = table_to_phys(pml4);

    /* Zero means the kernel's own space (the convention the unmap helpers use),
     * and the kernel PML4 is never claimed through the page-table allocator.
     * Neither is a released space, and treating them as one turned this guard
     * into a false panic on the first such call. */
    if (pml4_phys && pml4_phys != kernel_pml4_phys &&
        !pmm_frame_is_page_table(pml4_phys)) {
      console_write("paging: mprotect on a released address space, pml4 0x");
      console_write_hex64(pml4_phys);
      console_write(" va 0x");
      console_write_hex64(virtual_address);
      console_write("\n");
      panic("paging: address space used after free");
    }
  }
  /* Every level is checked before it is followed. A present entry addressing
   * something outside the direct map is not a table, and following it reads an
   * unrelated page as one — the walk then panics on a value ("AUX A/D" and
   * friends) that says nothing about who wrote it. Report the level, the table
   * that held the entry, and that frame's history instead. */
  extern void pmm_report_page_table_history_pub(u64 frame);
#define MPROTECT_CHECK_ENTRY(entry, level, parent_ptr)                          \
  do {                                                                          \
    u64 _child = (entry) & PAGE_ENTRY_ADDRESS_MASK;                             \
    if (direct_map_ready && _child >= DIRECT_MAP_SIZE) {                        \
      u64 _pp = (u64)(usize)(parent_ptr) - DIRECT_MAP_BASE;                     \
      console_write("paging_mprotect_page: level ");                            \
      console_write_dec(level);                                                 \
      console_write(" of table 0x");                                            \
      console_write_hex64(_pp);                                                 \
      console_write(" holds a non-table entry 0x");                             \
      console_write_hex64(entry);                                               \
      console_write(" for va 0x");                                              \
      console_write_hex64(virtual_address);                                     \
      console_write("\n  history of that table's frame:\n");                   \
      pmm_report_page_table_history_pub(_pp);                                   \
      if (current_task) {                                                       \
        console_write("  task ");                                               \
        console_write_dec(current_task->id);                                    \
        console_write(" ");                                                     \
        console_write(current_task->name);                                      \
        console_write("\n");                                                    \
      }                                                                         \
      return;                                                                   \
    }                                                                           \
  } while (0)

  u64 pml4e = pml4[pml4_index(virtual_address)];
  if ((pml4e & VMM_PRESENT) == 0)
    return;
  MPROTECT_CHECK_ENTRY(pml4e, 4, pml4);

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(virtual_address)];
  if ((pdpte & VMM_PRESENT) == 0)
    return;
  MPROTECT_CHECK_ENTRY(pdpte, 3, pdpt);

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(virtual_address)];
  if ((pde & VMM_PRESENT) == 0)
    return;
  if ((pde & HUGE_PAGE_FLAG) == 0)
    MPROTECT_CHECK_ENTRY(pde, 2, pd);

  if ((pde & HUGE_PAGE_FLAG) != 0)
    return;

  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(virtual_address)];

  if (pte & VMM_PRESENT) {
    u64 nf = flags;
    /* The shared zero page must never become directly writable through
     * mprotect — even after a PROT_READ downgrade dropped its COW marker — or
     * a store would corrupt the one frame mapped into every address space.
     * Re-arm COW so the first write materialises a private page. */
    if ((nf & VMM_WRITABLE) &&
        (pte & PAGE_ENTRY_ADDRESS_MASK) == pmm_zero_page()) {
      nf &= ~VMM_WRITABLE;
      nf |= VMM_COW;
    }
    /* Never let mprotect(PROT_WRITE) flip a COW page (fork-shared anon, or a
     * MAP_PRIVATE view of a page-cache frame) to directly-writable — that
     * would let stores corrupt the shared frame. Keep the COW marker and
     * leave the page read-only; the next store faults into the COW copy
     * path, which duplicates the frame and grants write access. */
    if ((pte & VMM_COW) && (nf & VMM_WRITABLE)) {
      nf &= ~VMM_WRITABLE;
      nf |= VMM_COW; /* write still faults into the COW copy path. A downgrade
                        to read-only (e.g. relro) intentionally drops the COW
                        marker: a later write is then a clean protection fault
                        (VMA prot tracking is too coarse to consult here). */
    }
    pt[pt_index(virtual_address)] =
        (pte & PAGE_ENTRY_ADDRESS_MASK) | nf | VMM_PRESENT;
    addrspace_note_replaced(pte); /* a protection change is a downgrade */
  } else if (pte & (VMM_LAZY | VMM_SWAPPED)) {
    // For non-present pages, update the saved flags
    pt[pt_index(virtual_address)] =
        (pte & (PAGE_ENTRY_ADDRESS_MASK | VMM_LAZY | VMM_SWAPPED)) | flags;
  }

  invalidate_page(virtual_address);
}

/* Diagnostic: return the raw leaf PTE for a user VA in the current address
 * space, plus the covering PDE and whether that PDE is a 2 MiB huge page.
 * Returns 0 if any level is absent. Read-only, no allocation. */
u64 paging_debug_leaf_pte(u64 va, u64 *out_pde, int *out_huge) {
  if (out_pde) *out_pde = 0;
  if (out_huge) *out_huge = 0;
  u64 *pml4 = get_current_pml4();
  if (!pml4) return 0;
  u64 pml4e = pml4[pml4_index(va)];
  if ((pml4e & VMM_PRESENT) == 0) return 0;
  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(va)];
  if ((pdpte & VMM_PRESENT) == 0) return 0;
  if (pdpte & HUGE_PAGE_FLAG) { if (out_huge) *out_huge = 1; if (out_pde) *out_pde = pdpte; return pdpte; }
  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(va)];
  if (out_pde) *out_pde = pde;
  if ((pde & VMM_PRESENT) == 0) return 0;
  if (pde & HUGE_PAGE_FLAG) { if (out_huge) *out_huge = 1; return pde; }
  u64 *pt = table_from_entry(pde);
  return pt[pt_index(va)];
}

void vmm_remap_page(u64 virtual_address, u64 physical_address, u64 flags) {
  vmm_unmap_page(virtual_address);
  vmm_map_page(virtual_address, physical_address, flags);
}

u64 vmm_direct_map_base(void) { return DIRECT_MAP_BASE; }

u64 vmm_virt_to_phys(void *ptr) {
  u64 virtual_address = (u64)(usize)ptr;
  if (virtual_address >= DIRECT_MAP_BASE && virtual_address < DIRECT_MAP_BASE + DIRECT_MAP_SIZE) {
    return virtual_address - DIRECT_MAP_BASE;
  }
  u64 *pml4 = get_current_pml4();
  if (!pml4) return 0;

  u64 pml4e = pml4[pml4_index(virtual_address)];
  if ((pml4e & VMM_PRESENT) == 0) return 0;

  u64 *pdpt = reachable_table(pml4e);
  if (!pdpt) return 0;
  u64 pdpte = pdpt[pdpt_index(virtual_address)];
  if ((pdpte & VMM_PRESENT) == 0) return 0;
  if (pdpte & HUGE_PAGE_FLAG) {
    return (pdpte & PAGE_ENTRY_ADDRESS_MASK) + (virtual_address & 0x3FFFFFFF);
  }

  u64 *pd = reachable_table(pdpte);
  if (!pd) return 0;
  u64 pde = pd[pd_index(virtual_address)];
  if ((pde & VMM_PRESENT) == 0) return 0;
  if (pde & HUGE_PAGE_FLAG) {
    return (pde & PAGE_ENTRY_ADDRESS_MASK) + (virtual_address & 0x1FFFFF);
  }

  u64 *pt = reachable_table(pde);
  if (!pt) return 0;
  u64 pte = pt[pt_index(virtual_address)];
  if ((pte & VMM_PRESENT) == 0) return 0;

  return (pte & PAGE_ENTRY_ADDRESS_MASK) + (virtual_address & 0xFFF);
}

// Mark a page as lazy (will allocate on first access)
/* Mark a page lazy AND give the marker the protection the fault handler should
 * use when it materialises the page. vmm_set_lazy followed by
 * paging_mprotect_page did the same thing in two four-level walks. */
static void vmm_set_lazy_common(u64 virtual_address, u64 leaf_extra);

void vmm_set_lazy_flags(u64 virtual_address, u64 flags) {
  /* The protection goes into the marker itself: the fault handler reads it
   * back when it materialises the page, so writing it here saves the second
   * four-level walk paging_mprotect_page would have done. */
  vmm_set_lazy_common(virtual_address,
                      flags & ~(PAGE_ENTRY_ADDRESS_MASK | VMM_PRESENT));
}

void vmm_set_lazy(u64 virtual_address) { vmm_set_lazy_common(virtual_address, 0); }

static void vmm_set_lazy_common(u64 virtual_address, u64 leaf_extra) {
  if ((virtual_address & (PAGE_SIZE - 1)) != 0)
    return;

  /* Tolerate a branch that moves under this walk instead of trusting it.
   *
   * Four levels are walked here while holding pointers into them, and a second
   * thread building the same branch publishes its own table and frees the
   * loser — so a pointer taken a moment ago can name a freed frame, and the
   * level below reads as zeroes. Following that is the "walked into physical
   * address 0" panic.
   *
   * Holding the page-table write lock across the whole walk fixes the race and
   * introduces a worse one: the section runs with interrupts off, and the work
   * inside it waits for other CPUs to acknowledge a TLB shootdown that they
   * cannot answer while blocked on this very lock (observed directly — "tlb:
   * STUCK cpu 2", then a lockup). So re-read each level instead, and give up
   * quietly if one is gone: a lazy marker is an optimisation, and the page it
   * would have marked is still faulted in correctly by the ordinary path. */
  u64 *pml4 = get_current_pml4();
  /* The root can be missing too. A task whose address space is being built or
   * replaced has pml4_phys briefly zero, and get_current_pml4 then hands back
   * the base of the direct map — physical address zero wearing the shape of a
   * table. Walking into it panics one level down. */
  if (!pml4 || (u64)(usize)pml4 == DIRECT_MAP_BASE)
    return;
  (void)ensure_child_table(pml4, pml4_index(virtual_address));
  __atomic_or_fetch(&pml4[pml4_index(virtual_address)], VMM_USER, __ATOMIC_SEQ_CST);
  /* Re-read rather than reuse what ensure_child_table just returned: the entry
   * is the authority, and it may name a different table by now. */
  u64 pml4e = __atomic_load_n(&pml4[pml4_index(virtual_address)], __ATOMIC_ACQUIRE);
  if (!(pml4e & VMM_PRESENT) || (pml4e & PAGE_ENTRY_ADDRESS_MASK) == 0)
    return;
  u64 *pdpt = table_from_entry(pml4e);

  (void)ensure_child_table(pdpt, pdpt_index(virtual_address));
  __atomic_or_fetch(&pdpt[pdpt_index(virtual_address)], VMM_USER, __ATOMIC_SEQ_CST);
  u64 pdpte = __atomic_load_n(&pdpt[pdpt_index(virtual_address)], __ATOMIC_ACQUIRE);
  if (!(pdpte & VMM_PRESENT) || (pdpte & PAGE_ENTRY_ADDRESS_MASK) == 0 ||
      (pdpte & HUGE_PAGE_FLAG))
    return;
  u64 *pd = table_from_entry(pdpte);

  /* The low 4 GiB is identity-mapped with 2 MiB SUPERVISOR huge pages in every
   * address space (cloned per-space). The userspace load base 0x2000000 lives
   * inside that region, so a lazy mapping there must SPLIT the huge page into a
   * 4 KiB page table first — exactly as vmm_map_page_in_table does for the eager
   * path. Without this, ensure_child_table below would treat the huge page's
   * physical base as a page-table pointer and scribble VMM_LAZY into arbitrary
   * physical memory (the deterministic clang-entry corruption behind the
   * demand-paged-loader SIGILL). */
  if ((pd[pd_index(virtual_address)] & HUGE_PAGE_FLAG) != 0) {
    (void)split_huge_page(pd, pd_index(virtual_address));
  } else {
    (void)ensure_child_table(pd, pd_index(virtual_address));
  }
  __atomic_or_fetch(&pd[pd_index(virtual_address)], VMM_USER, __ATOMIC_SEQ_CST);

  u64 pde = __atomic_load_n(&pd[pd_index(virtual_address)], __ATOMIC_ACQUIRE);
  if (!(pde & VMM_PRESENT) || (pde & PAGE_ENTRY_ADDRESS_MASK) == 0 ||
      (pde & HUGE_PAGE_FLAG))
    return;
  u64 *pt = table_from_entry(pde);

  /* Only mark an entry that is empty, or that is the kernel's own identity
   * mapping of this very address.
   *
   * "Empty" alone was wrong for every load base inside the low 4 GiB. The split
   * above does not leave holes: it fills all 512 leaves with the identity
   * mapping the 2 MiB entry used to describe, so the leaf is present, not zero,
   * and the marker was never installed. The caller then set the user bit on
   * that leaf — handing the process a writable mapping of the kernel's own
   * physical memory. A static binary loaded at 0x200000 was given the frames
   * the kernel image occupies: its output arrived as garbage because the loader
   * had overwritten kernel .text, and its exit handed those frames back to the
   * allocator ("pmm: double free detected at 0x0000000000200000").
   *
   * The extra value accepted is deliberately exact — present, supervisor, and
   * mapping this address to itself, which is what the identity window is and
   * what nothing else looks like. A real mapping of this page carries the user
   * bit and is still never overwritten. */
  u64 expect = __atomic_load_n(&pt[pt_index(virtual_address)], __ATOMIC_ACQUIRE);
  int identity_leaf = virtual_address < DIRECT_MAP_SIZE &&
                      (expect & VMM_PRESENT) && !(expect & VMM_USER) &&
                      (expect & PAGE_ENTRY_ADDRESS_MASK) ==
                          (virtual_address & PAGE_ENTRY_ADDRESS_MASK);

  if (expect == 0 || identity_leaf) {
    if (__atomic_compare_exchange_n(&pt[pt_index(virtual_address)], &expect,
                                    VMM_LAZY | leaf_extra, 0, __ATOMIC_RELEASE,
                                    __ATOMIC_RELAXED) &&
        identity_leaf)
      addrspace_note_replaced(expect);
  }
  invalidate_page(virtual_address);
}

static u64 *pf_leaf_pte_ptr(u64 va);

/* The same, but for another task's address space — the caller supplies the
 * PML4 rather than borrowing the running one. Used to tell code from data on a
 * stopped thread's stack: a return address lives on an executable page, and a
 * stray pointer to a structure does not. */
u64 paging_user_pte(u64 pml4_phys, u64 vaddr) {
  if (!pml4_phys)
    return 0;

  u64 *pml4 = (u64 *)(usize)(pml4_phys + DIRECT_MAP_BASE);
  u64 pml4e = pml4[pml4_index(vaddr)];

  if ((pml4e & VMM_PRESENT) == 0)
    return 0;

  u64 *pdpt = reachable_table(pml4e);
  if (!pdpt)
    return 0;
  u64 pdpte = pdpt[pdpt_index(vaddr)];
  if ((pdpte & VMM_PRESENT) == 0 || (pdpte & HUGE_PAGE_FLAG))
    return 0;

  u64 *pd = reachable_table(pdpte);
  if (!pd)
    return 0;
  u64 pde = pd[pd_index(vaddr)];
  if ((pde & VMM_PRESENT) == 0 || (pde & HUGE_PAGE_FLAG))
    return 0;

  u64 *pt = reachable_table(pde);
  if (!pt)
    return 0;
  return pt[pt_index(vaddr)];
}

/* The live leaf entry for an address, or 0 when no page table describes it.
 * Read-only, takes the read lock, and is meant for reporting a fault — the
 * entry is what separates "no mapping" from "mapped but not writable". */
u64 vmm_query_leaf_pte(u64 vaddr) {
  u64 flags;
  u64 value = 0;

  vmm_read_acquire(&flags);
  {
    u64 *slot = pf_leaf_pte_ptr(vaddr);

    if (slot)
      value = *slot;
  }
  vmm_read_release(flags);
  return value;
}

// Handle page faults for demand paging and swap
/* Walk the CURRENT address space to the leaf PT entry for va, following only
 * already-present, non-huge intermediate tables (no allocation — safe to call
 * under vmm_lock with IRQs off). Returns a pointer to the live pt[idx] slot, or
 * NULL if any level is absent/huge (i.e. there is no leaf PTE to fault on).
 * Caller must hold vmm_lock so the tables can't be torn down mid-walk. */
static u64 *pf_leaf_pte_ptr(u64 va) {
  u64 *pml4 = get_current_pml4();
  u64 pml4e = pml4[pml4_index(va)];
  if ((pml4e & VMM_PRESENT) == 0) return 0;
  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(va)];
  if ((pdpte & VMM_PRESENT) == 0 || (pdpte & HUGE_PAGE_FLAG)) return 0;
  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(va)];
  if ((pde & VMM_PRESENT) == 0 || (pde & HUGE_PAGE_FLAG)) return 0;
  u64 *pt = table_from_entry(pde);
  return &pt[pt_index(va)];
}

/* Unmap a run of pages under ONE acquisition of the page-table lock.
 *
 * Freeing a quarter-gigabyte image a page at a time takes this lock sixty-five
 * thousand times, and it is the lock every page fault on every CPU needs; the
 * teardown of a browser that has run for ten minutes took minutes of its own.
 * The frame each entry held is reported so the caller can release the frames
 * after its flush — never before, or another core's stale translation would
 * reach a reallocated frame. */
/* Clear a run of leaf entries and REPORT the frames instead of freeing them.
 *
 * The freeing is the caller's, and it must not happen until every other CPU has
 * dropped its cached translation: a frame handed back to the allocator while a
 * stale TLB entry still names it is a write into somebody else's page. The
 * unmap path below frees inline, which is why it needs a shootdown every few
 * dozen pages; a caller that tears down a large mapping can instead clear a
 * long run, shoot down once, and only then return the frames. On a four-CPU
 * guest that is the difference between one broadcast IPI per sixty-four pages
 * and one per range — measured at ~1.8 s for a single large munmap, which was
 * the largest remaining cost in the browser's start-up.
 *
 * Swap slots and eviction bookkeeping are released here, since neither is
 * reachable through a stale TLB entry. Huge leaves fall back to the inline
 * path: they are kernel mappings, whose frames this never frees anyway. */
usize vmm_unmap_range_collect(u64 base, usize npages, u64 *frames_out) {
  u64 _vmflags;
  usize nframes = 0;
  int any_present = 0;
  u64 end = base + (u64)npages * PAGE_SIZE;
  u64 *pml4 = get_current_pml4();

  vmm_write_acquire(&_vmflags);
  for (u64 va = base; va < end;) {
    /* One walk per table, not per page, and no walk at all through the holes.
     * Chromium unmaps out of an address space it reserved in terabytes: the
     * pages are mostly absent, and descending four levels for each of them was
     * 30 ms a call. */
    u64 pml4e = pml4[pml4_index(va)];

    if (!(pml4e & VMM_PRESENT)) {
      va = (va + (1ULL << 39)) & ~((1ULL << 39) - 1);
      continue;
    }

    u64 *pdpt = table_from_entry(pml4e);
    u64 pdpte = pdpt[pdpt_index(va)];

    if (!(pdpte & VMM_PRESENT)) {
      va = (va + (1ULL << 30)) & ~((1ULL << 30) - 1);
      continue;
    }
    if (pdpte & HUGE_PAGE_FLAG) {
      unmap_page_from_pml4(pml4, va, 0);
      va = (va + (1ULL << 30)) & ~((1ULL << 30) - 1);
      continue;
    }

    u64 *pd = table_from_entry(pdpte);
    u64 pde = pd[pd_index(va)];

    if (!(pde & VMM_PRESENT)) {
      va = (va + (1ULL << 21)) & ~((1ULL << 21) - 1);
      continue;
    }
    if (pde & HUGE_PAGE_FLAG) {
      unmap_page_from_pml4(pml4, va, 0);
      va = (va + (1ULL << 21)) & ~((1ULL << 21) - 1);
      continue;
    }

    u64 *pt = table_from_entry(pde);
    u64 tab_end = (va + (1ULL << 21)) & ~((1ULL << 21) - 1);

    if (tab_end > end)
      tab_end = end;
    for (; va < tab_end; va += PAGE_SIZE) {
      u64 e = pt[pt_index(va)];

      if (!e)
        continue;
      if (e & VMM_PRESENT) {
        u64 frame = e & PAGE_ENTRY_ADDRESS_MASK;

        if (frame && (e & VMM_USER)) {
          extern void eviction_unregister_page(u64 frame);

          eviction_unregister_page(frame);
          frames_out[nframes++] = frame;
        }
      } else if (e & VMM_SWAPPED) {
        extern void swap_free_slot_index(u32 slot);

        swap_free_slot_index((u32)((e & PAGE_ENTRY_ADDRESS_MASK) >> 12));
      }
      pt[pt_index(va)] = 0;
      any_present = 1;
      invalidate_page(va);
    }
  }
  /* One announcement for the whole range. The per-page bump was a global
   * atomic per page and a CR3 reload on every other CPU at its next switch —
   * the same cost that made mprotect expensive. */
  if (any_present)
    addrspace_note_replaced(VMM_PRESENT);
  vmm_write_release(_vmflags);
  return nframes;
}

usize vmm_unmap_range_nosync(u64 base, usize npages, u64 *frames_out) {
  u64 _vmflags;

  vmm_write_acquire(&_vmflags);
  for (usize i = 0; i < npages; i++) {
    u64 va = base + i * PAGE_SIZE;
    u64 *pte = pf_leaf_pte_ptr(va);

    frames_out[i] = (pte && (*pte & VMM_PRESENT))
                        ? (*pte & 0x000ffffffffff000ULL)
                        : 0;
    unmap_page_from_pml4(get_current_pml4(), va, 0);
  }
  vmm_write_release(_vmflags);
  return npages;
}


/* A ring of the last address-space edits, for attributing leftovers.
 *
 * When a freshly allocated destination turns out to hold pages, the only
 * question that matters is which earlier mapping left them — and by then every
 * trace of that mapping is gone. Recording each mmap/munmap/move with its range
 * and the thread that made it costs four stores on paths that already walk page
 * tables, and turns "somebody left pages here" into a named sequence of calls.
 *
 * Deliberately lock-free: the index is bumped atomically and a torn entry would
 * at worst misreport one line of a diagnostic. A lock here would sit on the
 * munmap path of every process for the sake of a message almost never printed.
 */
#define VMA_TRACE_N 8192
struct vma_trace_ent {
  u64 start, end;
  u64 seq;
  u64 space;  /* the PML4 frame: which address space these addresses are in */
  u32 pid;
  u32 task;   /* which thread of it: the task slot, not the shared pid */
  const char *what;
};
static struct vma_trace_ent g_vma_trace[VMA_TRACE_N];
static u64 g_vma_trace_seq;

/* Page installs are recorded too, when asked for: they are the only edits that
 * happen with no mapping call behind them, so a leftover with no munmap to
 * blame is either theirs or nobody's. Off by default — a fault is frequent
 * enough that four stores per fault is a real cost, and the flag is read once. */
static int vma_trace_faults = -1;

int vma_trace_faults_enabled(void) {
  /* Do not cache a "no" answer taken before the command line was parsed: the
   * first user page is mapped early enough that an eager read of the flag
   * settles on 0 for the whole boot, and the tracing then never happens no
   * matter what the run asked for. Cache only the positive. */
  if (vma_trace_faults < 0) {
    const struct boot_info *bi = bootinfo_get();

    if (!bi || !bi->command_line[0])
      return 0;
    vma_trace_faults = bootinfo_has_flag("b1nix.vma-trace") ? 1 : 0;
    /* Say so once. A probe that silently never armed looks exactly like a
     * probe that armed and found nothing, and the two have opposite meanings. */
    console_write(vma_trace_faults ? "vma-trace: armed\n"
                                   : "vma-trace: not requested\n");
  }
  return vma_trace_faults;
}

void vma_trace_record(const char *what, u64 start, u64 end) {
  u64 seq = __atomic_add_fetch(&g_vma_trace_seq, 1, __ATOMIC_RELAXED);
  struct vma_trace_ent *e = &g_vma_trace[seq % VMA_TRACE_N];

  e->start = start;
  e->end = end;
  e->space = current_task ? current_task->pml4_phys : 0;
  e->pid = current_task ? (u32)current_task->id : 0;
  e->task = (u32)(((u64)(usize)current_task >> 6) & 0xffff);
  e->what = what;
  __atomic_store_n(&e->seq, seq, __ATOMIC_RELEASE);
}

/* The newest recorded edit covering one address, however far back it is.
 *
 * The range listing is capped, and the entry that explains a leftover page can
 * be older than the cap — this answers "what was the last mapping call that
 * covered THIS page" with no window at all. */
void vma_trace_dump_addr(u64 va) {
  u64 space = current_task ? current_task->pml4_phys : 0;
  u64 now = __atomic_load_n(&g_vma_trace_seq, __ATOMIC_ACQUIRE);
  u64 first = now > VMA_TRACE_N ? now - VMA_TRACE_N : 1;

  for (u64 i = now; i >= first; i--) {
    struct vma_trace_ent *e = &g_vma_trace[i % VMA_TRACE_N];

    if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != i || e->space != space)
      continue;
    if (va < e->start || va >= e->end)
      continue;
    console_write("  last edit covering 0x");
    console_write_hex64(va);
    console_write(": #");
    console_write_dec(e->seq);
    console_write(" tid ");
    console_write_dec(e->pid);
    console_write(" ");
    console_write(e->what);
    console_write(" 0x");
    console_write_hex64(e->start);
    console_write("-0x");
    console_write_hex64(e->end);
    console_write("\n");
    return;
  }
  console_write("  no recorded edit ever covered 0x");
  console_write_hex64(va);
  console_write("\n");
}

/* Every recorded edit that touched this range, oldest first. */
void vma_trace_dump(u64 start, u64 end) {
  /* Only this address space's edits. The ring is global, and an address in one
   * process says nothing about the same address in another — mixing them in
   * makes an unrelated process's mapping look like an explanation. */
  u64 space = current_task ? current_task->pml4_phys : 0;
  u64 now = __atomic_load_n(&g_vma_trace_seq, __ATOMIC_ACQUIRE);
  u64 first = now > VMA_TRACE_N ? now - VMA_TRACE_N : 1;
  unsigned shown = 0;

  /* Newest first: the edits that explain a leftover are the ones just before
   * it, and an oldest-first listing spends its whole budget on history. */
  console_write("  edits touching this range (newest first):\n");
  for (u64 i = now; i >= first && shown < 16; i--) {
    struct vma_trace_ent *e = &g_vma_trace[i % VMA_TRACE_N];

    if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != i)
      continue;
    if (e->start >= end || e->end <= start)
      continue;
    if (e->space != space)
      continue;
    shown++;
    console_write("    #");
    console_write_dec(e->seq);
    console_write(" pid ");
    console_write_dec(e->pid);
    console_write(" thread ");
    console_write_dec(e->task);
    console_write(" ");
    console_write(e->what);
    console_write(" 0x");
    console_write_hex64(e->start);
    console_write("-0x");
    console_write_hex64(e->end);
    console_write("\n");
  }
  if (!shown)
    console_write("    (none recorded)\n");

  /* And the last few edits of any range, so an empty list above can be read as
   * "nothing touched this range" rather than "nothing is being recorded". */
  console_write("  most recent edits of this space, any range:\n");
  shown = 0;
  for (u64 i = now; i >= first && shown < 6; i--) {
    struct vma_trace_ent *e = &g_vma_trace[i % VMA_TRACE_N];

    if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != i || e->space != space)
      continue;
    shown++;
    console_write("    #");
    console_write_dec(e->seq);
    console_write(" tid ");
    console_write_dec(e->pid);
    console_write(" ");
    console_write(e->what);
    console_write(" 0x");
    console_write_hex64(e->start);
    console_write("\n");
  }
}


/* How many times a move has reported a dirty destination. The scan that
 * produces the report is itself expensive, so this bounds both. */
static unsigned move_leftovers_reported;


/* Move a range's leaf entries to another address, without touching the pages.
 *
 * mremap used to grow a mapping by allocating a second one and copying every
 * byte through a kernel bounce buffer — faulting each page in on the way, and
 * holding the address-space mutex for all of it. The pages themselves never
 * need to move: only the entries that name them do, and that includes the lazy
 * and swapped leaves, which have no page behind them at all and would have been
 * materialised by a copy.
 *
 * An anonymous mmap installs no leaf at all, so the destination page tables do
 * not exist yet. They are built one page ahead of the move, outside the lock —
 * vmm_set_lazy allocates, and allocation can reclaim, which must never happen
 * with the VMM write lock held. The move itself is then a pair of stores.
 */
void paging_move_range(u64 old_start, u64 new_start, u64 len) {
  extern void tlb_shootdown_all(void);
  u64 flags;

  /* Recorded after the destination has been inspected, not before: an entry
   * for the move itself would otherwise be the newest edit covering the very
   * page whose history is being asked for, and answer every question with
   * itself. */

  /* A block at a time: the tables for a whole block are built first, outside
   * the lock, then one critical section moves its entries. Taking the write
   * lock per page turned a large move into hundreds of thousands of lock
   * round trips; holding it for the whole range would stall every fault on
   * every CPU for as long as the move takes. The caller owns both ranges
   * throughout, so no other thread can map into them meanwhile. */
  /* One page table covers this much address space, and it is also the block
   * size: a span this size shares one destination table. */
  const u64 block = 512 * PAGE_SIZE;

  /* The destination must be empty before anything is moved into it.
   *
   * It is supposed to be — it is a mapping that was created moments ago — and
   * yet it arrives holding present, read-only, user entries naming real
   * frames: pages left behind by whatever occupied this address before. The
   * move then overwrites them, the frames they named are never released by
   * their owner, and the unmap that follows frees a frame the surviving entry
   * still points at. That is the "pmm: double free ... from
   * unmap_page_from_pml4" that killed the shell running the browser.
   *
   * Clearing them here is right whoever left them: no caller of this function
   * intends to inherit another mapping's pages. Collected, flushed, then
   * freed — never freed before the flush. */
  {
    enum { MOVE_CLEAR_BATCH = 64 };
    u64 frames[MOVE_CLEAR_BATCH];
    u64 stop = new_start + len;
    usize cleared = 0;

    /* Read the destination before touching it.
     *
     * Only while there is a report left to make: the scan walks every page of
     * the destination, and mremap is called on every large realloc — paying a
     * full walk per call forever, to print nothing, is not a diagnostic, it is
     * a tax.
     *
     * What the leftovers ARE decides who left them, and the clear is what
     * destroys that evidence: a run of copy-on-write zero pages is the fault
     * handler's read-ahead, which installs up to sixteen neighbours per fault,
     * while ordinary private frames are pages something actually wrote to. */
    u64 scan_first_va = 0, scan_first_pte = 0;
    usize scan_present = 0, scan_cow = 0, scan_zero = 0, scan_lazy = 0;

    for (u64 v = new_start; move_leftovers_reported < 8 && v < stop;
         v += PAGE_SIZE) {
      u64 *pte = pf_leaf_pte_ptr(v);

      if (!pte || !*pte)
        continue;
      if (!scan_first_va) {
        scan_first_va = v;
        scan_first_pte = *pte;
      }
      if (*pte & VMM_PRESENT) {
        scan_present++;
        if (*pte & VMM_COW)
          scan_cow++;
        if ((*pte & PAGE_ENTRY_ADDRESS_MASK) == pmm_zero_page())
          scan_zero++;
      } else if (*pte & VMM_LAZY) {
        scan_lazy++;
      }
    }

    for (u64 v = new_start; v < stop;) {
      usize n = (usize)((stop - v) / PAGE_SIZE);

      if (n > MOVE_CLEAR_BATCH)
        n = MOVE_CLEAR_BATCH;
      usize nf = vmm_unmap_range_collect(v, n, frames);
      if (nf) {
        tlb_shootdown_all();
        for (usize k = 0; k < nf; k++)
          pmm_free_frame(frames[k]);
        cleared += nf;
      }
      v += (u64)n * PAGE_SIZE;
    }
    if (cleared) {
      if (move_leftovers_reported < 8) {
        move_leftovers_reported++;
        console_write("mremap: leftovers present=");
        console_write_dec(scan_present);
        console_write(" cow=");
        console_write_dec(scan_cow);
        console_write(" zero-page=");
        console_write_dec(scan_zero);
        console_write(" lazy=");
        console_write_dec(scan_lazy);
        console_write(" first 0x");
        console_write_hex64(scan_first_va);
        console_write(" pte=0x");
        console_write_hex64(scan_first_pte);
        console_write("\n");
        vma_trace_dump_addr(scan_first_va);
        vma_trace_dump(new_start, new_start + len);
        console_write("mremap: destination 0x");
        console_write_hex64(new_start);
        console_write(" arrived holding ");
        console_write_dec(cleared);
        console_write(" page(s) left by a previous mapping");
        /* Whose pages they are decides what this is.
         *
         * The destination came from the allocator moments ago, so nothing of
         * this process should cover it. If a live mapping does — one other
         * than the fresh one being moved into — then the allocator handed out
         * an address that was already taken, and the pages just freed belonged
         * to a mapping that is still in use. That is a different fault from
         * leftovers of a mapping that is already gone, and only one of the two
         * corrupts a running program. */
        {
          struct task *t = current_task;
          struct vm_area *owner = 0;

          if (t) {
            for (struct vm_area *v = t->vma_list; v; v = v->next) {
              if (v->start < new_start + len && v->end > new_start &&
                  v->start != new_start) {
                owner = v;
                break;
              }
            }
          }
          if (owner) {
            console_write("; a LIVE mapping 0x");
            console_write_hex64(owner->start);
            console_write("-0x");
            console_write_hex64(owner->end);
            console_write(" still covers it");
          } else {
            console_write("; no live mapping covers it");
          }
        }
        console_write("\n");
      }
    }
  }

  vma_trace_record("move-src", old_start, old_start + len);
  vma_trace_record("move-dst", new_start, new_start + len);

  for (u64 base = 0; base < len; base += block) {
    u64 span = len - base;

    if (span > block)
      span = block;

    /* One call per destination page table, not per page: every page of a
     * 2 MiB-aligned span shares the same table, so building it once is enough
     * and the other 511 walks are wasted work. */
    for (u64 off = base; off < base + span;) {
      u64 to = new_start + off;
      u64 next = (to + block) & ~(block - 1);

      vmm_set_lazy(to);
      off += next - to;
    }

    vmm_write_acquire(&flags);
    for (u64 off = base; off < base + span; off += PAGE_SIZE) {
      u64 *src = pf_leaf_pte_ptr(old_start + off);
      u64 *dst = pf_leaf_pte_ptr(new_start + off);

      if (src && dst && *src) {
        u64 moved = *src;

        *dst = moved;
        *src = 0;
        addrspace_note_replaced(moved);
        invalidate_page(old_start + off);
        invalidate_page(new_start + off);
      }
    }
    vmm_write_release(flags);
  }
  /* One flush for the whole move rather than two IPI round trips per page —
   * a large range has hundreds of thousands of pages, and per-page shootdowns
   * alone cost more than the copy this replaces. A small move is the opposite
   * case: broadcasting a full flush for two pages throws away every other
   * CPU's TLB, so name the pages instead. */
  if (len > 64 * PAGE_SIZE) {
    tlb_shootdown_all();
  } else {
    extern void tlb_shootdown_page(u64);

    for (u64 off = 0; off < len; off += PAGE_SIZE) {
      tlb_shootdown_page(old_start + off);
      tlb_shootdown_page(new_start + off);
    }
  }
}

/* SMP page-fault handler (M28 #7 — make the fault path self-locking so the
 * exception handler can drop the BKL). The page-table mutators (vmm_map_page,
 * vmm_unmap_page, paging_clone_address_space's CoW write-back) all serialise on
 * vmm_lock; the fault handler is the one mutator that historically did not, so
 * a CLONE_VM sibling faulting on one CPU could RMW the same leaf PTE that fork
 * was CoW-marking on another. vmm_lock is IRQs-off and non-recursive and the
 * file/swap paths block (blk_*_cached → scheduler_yield), so we cannot simply
 * hold it across the handler. Instead use the classic prepare-then-commit
 * shape: allocate the frame and do the blocking I/O OUTSIDE the lock, then take
 * vmm_lock, RE-READ the live leaf PTE, and only install if the precondition
 * (LAZY / SWAPPED / COW with the same old frame) still holds — otherwise
 * another CPU already serviced this address, so we discard our spare frame and
 * report success. */
/* What the fault handler saw, for the report that runs after it.
 *
 * The exception report walks the tables itself, but it runs after the handler
 * has returned and after other CPUs have had their turn — it printed a leaf
 * that permitted the very access that had just been refused, which describes
 * the state at print time and not at fault time. These record the decision as
 * it was made. Per-CPU: two faults on two CPUs must not overwrite each other's
 * evidence. */
static u64 fault_leaf_seen[MAX_CPUS];
static u64 fault_leaf_val[MAX_CPUS];
static const char *fault_reason[MAX_CPUS];

void paging_last_fault_leaf(int *seen, u64 *val, const char **why) {
  int cpu = (int)percpu_read(cpu_id);
  if (cpu < 0 || cpu >= MAX_CPUS) {
    *seen = 0;
    *val = 0;
    *why = "(no cpu)";
    return;
  }
  *seen = (int)fault_leaf_seen[cpu];
  *val = fault_leaf_val[cpu];
  *why = fault_reason[cpu] ? fault_reason[cpu] : "(unset)";
}

static void fault_note(int seen, u64 val, const char *why) {
  int cpu = (int)percpu_read(cpu_id);
  if (cpu < 0 || cpu >= MAX_CPUS)
    return;
  fault_leaf_seen[cpu] = (u64)seen;
  fault_leaf_val[cpu] = val;
  fault_reason[cpu] = why;
}

/* A cross-CPU shootdown waits for every other CPU to acknowledge, so it can
 * only be issued with interrupts enabled: a fault taken from kernel code that
 * already held an irqsave lock cannot wait for CPUs that may in turn be waiting
 * for that lock. Those faults are rare (a copy to user memory under a lock) and
 * the mapping they resolve is this CPU's own, so the local invalidate stands
 * and the count below says how often it happened. */
static u64 g_cow_shootdown_skipped;
static u64 g_cow_shootdowns;

/* How many copy-on-write resolutions published their new frame to the other
 * CPUs, and how many could not because the fault arrived with interrupts off.
 * A skip leaves a sibling thread reading the pre-copy page until something
 * else flushes it, so the count is the size of the hole. */
void cow_shootdown_stats(u64 *done, u64 *skipped) {
  if (done)
    *done = g_cow_shootdowns;
  if (skipped)
    *skipped = g_cow_shootdown_skipped;
}

static void cow_publish_page(u64 va) {
  extern void tlb_shootdown_page(u64 vaddr);

  if (!irqs_are_enabled()) {
    g_cow_shootdown_skipped++;
    return;
  }
  g_cow_shootdowns++;
  tlb_shootdown_page(va);
}

int vmm_handle_page_fault(u64 fault_addr, u64 error_code) {

  u64 page_aligned = fault_addr & ~(PAGE_SIZE - 1);

  fault_note(0, 0, "(entered)");

  if (!is_canonical(fault_addr)) {
    panic("Non-canonical address fault!");
  }

  extern void eviction_evict_page(void);
  extern void eviction_register_page(struct task *task, u64 vaddr, u64 frame);
  extern void eviction_unregister_page(u64 frame);

  /* A user mapping that landed on the kernel's identity window.
   *
   * The low 4 GiB is identity-mapped with 2 MiB SUPERVISOR huge pages in every
   * address space. A lazily-backed user page inside that range therefore does
   * not fault as absent — the huge page underneath is present, so the CPU
   * reports a protection violation (present + user) and the demand-paging path
   * below, which only runs when the page is absent, never sees it. mmap is
   * free to place an anonymous region there, and chromium's allocator does:
   * every thread touching one died with a write fault whose PTE read as
   * instruction bytes, because there was no page table under the huge entry at
   * all.
   *
   * Split the huge page and mark this one leaf lazy, so the fault repeats as
   * the absent-page fault it should have been. Only for an address a VMA of
   * this task actually covers — outside one, a supervisor page must keep
   * refusing user access. */
  if (error_code & PF_PRESENT) {
    /* Cheap test first, under the read lock. An ordinary copy-on-write fault
     * arrives here too, and it must not pay for a page-table allocation. */
    int on_huge = 0;
    int leaf_seen = 0;
    u64 leaf_val = 0;
    u64 probe_flags;

    /* One walk answers both questions a protection fault raises: whether it
     * landed on the identity window's supervisor huge page, and what the live
     * leaf says. Each of those used to take the lock and walk the tables again,
     * on every such fault. */
    vmm_read_acquire(&probe_flags);
    {
      u64 *pml4 = get_current_pml4();
      u64 pml4e = pml4[pml4_index(page_aligned)];

      if (pml4e & VMM_PRESENT) {
        u64 pdpte = table_from_entry(pml4e)[pdpt_index(page_aligned)];

        if ((pdpte & VMM_PRESENT) && !(pdpte & HUGE_PAGE_FLAG)) {
          u64 pde = table_from_entry(pdpte)[pd_index(page_aligned)];

          on_huge = (pde & VMM_PRESENT) && (pde & HUGE_PAGE_FLAG) &&
                    !(pde & VMM_USER);
          if ((pde & VMM_PRESENT) && !(pde & HUGE_PAGE_FLAG)) {
            leaf_seen = 1;
            leaf_val = table_from_entry(pde)[pt_index(page_aligned)];
          }
        }
      }
    }
    vmm_read_release(probe_flags);

    int covered = 0;
    int found_vma = 0;
    u32 found_prot = 0;

    if (!(error_code & PF_USER) || !current_task)
      on_huge = 0; /* the remedy below is for user mappings only */
    for (struct vm_area *v = on_huge ? current_task->vma_list : 0;
         v && v->start <= page_aligned; v = v->next) {
      if (page_aligned < v->end) {
        found_vma = 1;
        found_prot = v->prot;
        covered = v->prot != PROT_NONE;
        break;
      }
    }
    /* Say why a fault over the identity window was refused. "No VMA" is a wild
     * access and belongs to SIGSEGV; a VMA with no access is the same; but a
     * VMA that grants access and still lands here means the split above did not
     * happen, and that is a kernel bug rather than a program's. */
    if (on_huge && !covered) {
      static unsigned refused;

      if (refused < 8) {
        refused++;
        console_write("pf: identity-window fault refused, va 0x");
        console_write_hex64(page_aligned);
        console_write(found_vma ? " vma prot " : " no vma");
        if (found_vma)
          console_write_hex64(found_prot);
        console_write("\n");
      }
    }
    if (covered) {
      u64 huge_flags;
      /* Allocated before the lock: see split_huge_page_into. Freed again below
       * when the split turns out to be unnecessary. */
      u64 *spare = alloc_page_table();

      vmm_write_acquire(&huge_flags);
      u64 *pml4 = get_current_pml4();
      u64 pml4e = pml4[pml4_index(page_aligned)];
      if (pml4e & VMM_PRESENT) {
        u64 *pdpt = table_from_entry(pml4e);
        u64 pdpte = pdpt[pdpt_index(page_aligned)];
        if ((pdpte & VMM_PRESENT) && !(pdpte & HUGE_PAGE_FLAG)) {
          u64 *pd = table_from_entry(pdpte);
          usize pdi = pd_index(page_aligned);
          if (spare && (pd[pdi] & VMM_PRESENT) && (pd[pdi] & HUGE_PAGE_FLAG) &&
              !(pd[pdi] & VMM_USER)) {
            split_huge_page_into(pd, pdi, spare);
            spare = 0;

            __atomic_or_fetch(&pml4[pml4_index(page_aligned)], VMM_USER, __ATOMIC_SEQ_CST);
            __atomic_or_fetch(&pdpt[pdpt_index(page_aligned)], VMM_USER, __ATOMIC_SEQ_CST);
            __atomic_or_fetch(&pd[pdi], VMM_USER, __ATOMIC_SEQ_CST);
            {
              u64 *pt = table_from_entry(pd[pdi]);

              addrspace_note_replaced(pt[pt_index(page_aligned)]);
              pt[pt_index(page_aligned)] = VMM_LAZY;
            }
            invalidate_page(page_aligned);
            vmm_write_release(huge_flags);
            return 0;
          }
        }
      }
      vmm_write_release(huge_flags);
      if (spare) {
        u64 frame = table_to_phys(spare);

        pmm_note_page_table(frame, 0);
        pmm_free_frame(frame);
      }
    }

    /* A fault the tables disagree with is a stale TLB entry, not an access
     * error. The CPU reports PF_PRESENT from what it had cached; when the live
     * leaf says otherwise, the entry was changed by another CPU (or by this one
     * before an invalidation landed) and this CPU has not seen it yet. Flush
     * the one page and let the instruction run again: if the mapping really is
     * gone, the next fault arrives with PF_PRESENT clear and takes the ordinary
     * path. Observed under chromium as a write fault at error 0x7 whose PTE
     * read 0x0000000000000000. */
    int stale;
    /* Either the entry is gone, or it already permits exactly what faulted.
     * The second case is the same disagreement seen from the other side: a
     * write fault on a page whose live entry is present and writable, because
     * another CPU granted the write (a copy-on-write break, an mprotect) and
     * this CPU is still acting on the entry it had cached. A genuine
     * copy-on-write fault does not match — that entry is present and read-only
     * — so it still goes down the path that breaks the sharing. */
    stale = !leaf_seen || ((leaf_val & VMM_PRESENT) == 0) ||
            (((error_code & PF_WRITE) != 0) && (leaf_val & VMM_WRITABLE) != 0) ||
            (((error_code & PF_WRITE) == 0) && (leaf_val & VMM_PRESENT) != 0);

    fault_note(leaf_seen, leaf_val,
               stale ? "protection fault, leaf disagrees (stale)"
                     : "protection fault, leaf agrees with refusal");

    /* Retry the same address a bounded number of times. A flush that does not
     * take would otherwise loop here forever with nothing on the console;
     * after a few attempts the fault goes down the ordinary path, which either
     * services it or reports it. */
    /* Retry, and keep retrying: a fault the leaf permits is not an access
     * error, whatever the CPU thought it had cached.
     *
     * The budget that used to bound this lived in file-scope statics shared by
     * every core, so one CPU's attempts were charged to another CPU's address.
     * It ran out on unrelated faults and the fault was then handed to the
     * ordinary path, which delivered SIGSEGV for a write to a page whose own
     * entry said writable — a fork/exec workload lost a thread to it about
     * every fourth run on six cores. Per-CPU accounting is what makes the
     * count mean anything. It stays bounded so a genuine fault still reaches
     * the signal that reports it, and every sixty-fourth attempt on the same
     * address reloads CR3, which drops every non-global translation this CPU
     * holds. */
    struct percpu *pf_pcpu = get_percpu();
    static u64 spurious_va_cpu[MAX_CPUS];
    static unsigned spurious_repeat_cpu[MAX_CPUS];
    unsigned pf_cpu = pf_pcpu ? (unsigned)pf_pcpu->cpu_id : 0u;
    static u64 spurious_count;

    if (pf_cpu >= MAX_CPUS)
      pf_cpu = 0;
    if (stale) {
      if (page_aligned == spurious_va_cpu[pf_cpu]) {
        unsigned n = ++spurious_repeat_cpu[pf_cpu];

        if ((n % 64u) == 0u) {
          u64 cr3;

          __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
          __asm__ volatile("movq %0, %%cr3" : : "r"(cr3) : "memory");
        }
        if (n > 256u)
          stale = 0; /* not a translation of ours: let the ordinary path decide */
      } else {
        spurious_va_cpu[pf_cpu] = page_aligned;
        spurious_repeat_cpu[pf_cpu] = 1;
      }
    }
    if (stale) {
      u64 n = ++spurious_count;

      __asm__ volatile("invlpg (%0)" : : "r"(page_aligned) : "memory");
      if (n <= 8)
        klog_warn("page fault on an entry this CPU no longer has: retrying");
      return 0;
    }
  }

  /* A non-present leaf can be an explicit lazy/swap entry. Do not let the
   * heap growth fast path overwrite that metadata with a zero page.
   *
   * A protection fault has already read this leaf above and does not reach
   * here; only an absent-page fault does, so this walk happens once per fault
   * rather than beside another one. */
  int has_deferred_leaf = 0;
  u64 deferred_flags;
  vmm_read_acquire(&deferred_flags);
  u64 *deferred_leaf = pf_leaf_pte_ptr(page_aligned);
  if (deferred_leaf && (*deferred_leaf & (VMM_LAZY | VMM_SWAPPED)))
    has_deferred_leaf = 1;
  vmm_read_release(deferred_flags);

  // Lazy Allocation for User Heap/Mmap region (anonymous, no leaf PTE yet).
  if (!(error_code & PF_PRESENT) && !has_deferred_leaf &&
      fault_addr >= 0x40000000 &&
      fault_addr < 0x00007FFFFFFFFFFF) {
    /* M88: enforce PROT_NONE. A pure reservation (mmap PROT_NONE) records a VMA
     * but installs no leaf PTE, so without this a wild user access would fall
     * into the zero-fill below and silently succeed. If the faulting address
     * lands in a no-access VMA, refuse to service it — the caller delivers
     * SIGSEGV. The VMA list is sorted by start, so the walk early-exits past the
     * address; this runs only on the anonymous not-present path (PROT_NONE
     * regions never have present/lazy leaves), not on heap-growth faults that
     * have no covering VMA. mprotect splits the VMA and updates ->prot, so a
     * region later made accessible (e.g. V8 committing part of a reservation)
     * has prot != PROT_NONE here and falls through to the normal zero-fill. */
    struct vm_area *anon_vma = 0;

    if ((error_code & PF_USER) && current_task) {
      anon_vma = vma_lookup(current_task, page_aligned);
      if (anon_vma && anon_vma->prot == PROT_NONE) {
        fault_note(0, 0, "address is inside a PROT_NONE reservation");
        return -1; /* no access -> SIGSEGV */
      }
    }
    /* Zero-page dedup: a fresh anonymous heap page has no content, so point
     * the mapping at the single shared read-only zero page instead of
     * allocating a frame. The first store faults into the COW path and
     * materialises a private frame (served from the pre-zeroed pool — no
     * memset). Capacity win: brk/mmap regions stay unbacked until written.
     *
     * Not when the access that faulted is itself a store.
     *
     * The dedup pays off for pages that are mapped and then read, or never
     * touched at all. For a page whose first access is a write — which is
     * every buffer an allocator hands out and immediately fills — it costs two
     * faults instead of one: the first installs the shared page read-only, the
     * store faults again, and the copy path allocates the frame that could
     * have been installed straight away. Measured on a heap-churn run: 2.3 s
     * of work took 5 s. A write fault gets its own frame here. */
    /* Protection for the page about to be installed. An anonymous mapping that
     * did not ask for PROT_EXEC gets the NX bit, so a heap or stack page cannot
     * be jumped into; without a VMA to consult (a few legacy kernel-side
     * mappings) the old permissive behaviour stands rather than guessing. */
    u64 anon_user_flags =
        anon_vma ? vmm_user_flags_from_prot((int)anon_vma->prot) : VMM_USER;
    u64 anon_exec_bits = anon_user_flags & VMM_NO_EXECUTE;

    u64 zero_pg = (error_code & PF_WRITE) ? 0 : pmm_zero_page();
    if (zero_pg) {
      u64 cflags;
      vmm_write_acquire(&cflags);
      u64 *slot = pf_leaf_pte_ptr(page_aligned);
      if (slot && (*slot & VMM_PRESENT)) {
        vmm_write_release(cflags);
        return 0; /* already serviced concurrently */
      }
      vmm_map_page_locked(page_aligned, zero_pg,
                          VMM_PRESENT | VMM_COW | VMM_USER | anon_exec_bits);
      /* Point the neighbours at the same shared zero page while the tables are
       * open. Anonymous memory is touched in runs — a heap grows, a thread
       * stack is written down — and each of those pages otherwise costs a
       * fault that ends in exactly this store. Nothing is allocated and no
       * content is invented: the frame is the one read-only zero page every
       * fresh anonymous page already gets, and the first write to any of them
       * still takes the copy-on-write path. Bounded by the page table and by
       * the mapping. */
      if (anon_vma) {
        const u64 table_span = 512 * PAGE_SIZE;
        u64 stop = (page_aligned + table_span) & ~(table_span - 1);

        if (stop > anon_vma->end)
          stop = anon_vma->end;
        if (stop > page_aligned + 16 * PAGE_SIZE)
          stop = page_aligned + 16 * PAGE_SIZE;

        for (u64 va = page_aligned + PAGE_SIZE; va < stop; va += PAGE_SIZE) {
          u64 *nslot = pf_leaf_pte_ptr(va);

          if (!nslot || (*nslot & VMM_PRESENT) ||
              (*nslot & (VMM_LAZY | VMM_SWAPPED)))
            break;
          vmm_map_page_locked(va, zero_pg,
                              VMM_PRESENT | VMM_COW | VMM_USER | anon_exec_bits);
        }
      }
      vmm_write_release(cflags);
      /* Not registered in the eviction ring: one shared frame, read-only, and
       * swap must never evict a frame mapped in many address spaces. */
      if (vma_trace_faults_enabled())
        vma_trace_record(anon_vma ? "pf-zero(vma)" : "pf-zero(NO VMA)",
                         page_aligned, page_aligned + PAGE_SIZE);
      return 0;
    }
    // Prepare a zeroed frame OUTSIDE the lock (alloc may run reclaim/swap).
    u64 frame = pmm_alloc_frame();
    if (!frame) {
      eviction_evict_page();
      frame = pmm_alloc_frame();
      if (!frame) {
        /* True OOM while growing a userspace mapping. Do NOT panic the whole
         * kernel for one greedy process — return failure so the exception
         * handler kills the faulting task with SIGSEGV (Linux-style OOM: the
         * offending process dies, the system survives). Matches the VMM_LAZY
         * path below. Kernel-internal OOM (page tables, klarge, heap growth)
         * still panics — there is no faulting userspace task to blame. */
        console_write("pf: OOM growing userspace mapping for pid ");
        console_write_dec(current_task ? current_task->id : 0);
        console_write(" — killing task\n");
        return -1;
      }
    }
    memset((void *)((u64)frame + DIRECT_MAP_BASE), 0, PAGE_SIZE);

    // Commit under vmm_lock. Re-check that the leaf is still absent — another
    // CPU faulting the same anonymous address may have installed it while we
    // were allocating; if so, discard our spare and let the retry succeed.
    u64 cflags;
    vmm_write_acquire(&cflags);
    u64 *slot = pf_leaf_pte_ptr(page_aligned);
    if (slot && (*slot & VMM_PRESENT)) {
      vmm_write_release(cflags);
      pmm_free_frame(frame);
      return 0; // already serviced concurrently
    }
    vmm_map_page_locked(page_aligned, frame,
                        VMM_PRESENT | VMM_WRITABLE | VMM_USER | anon_exec_bits);
    vmm_write_release(cflags);
    eviction_register_page(current_task, page_aligned, frame);

    return 0;
  }

  /* Classify against the live leaf PTE. Read it under the lock so we get a
   * coherent snapshot, then drop the lock for any blocking work and re-validate
   * on commit. */
  u64 rflags;
  vmm_read_acquire(&rflags);
  u64 *leaf0 = pf_leaf_pte_ptr(page_aligned);
  if (!leaf0) {
    vmm_read_release(rflags);
    /* No leaf means the address sits under a huge page (the identity window
     * uses 2 MiB supervisor pages for the low 4 GiB) or under nothing at all,
     * and those are opposite problems: the first is ours to split and hand to
     * userspace, the second is a genuine wild access. Whether a VMA covers it
     * is what separates them, and the report never said. */
    if ((error_code & PF_USER) && current_task) {
      struct vm_area *cover = vma_lookup(current_task, page_aligned);

      fault_note(0, 0, cover ? "no leaf PTE, but a VMA covers this address"
                             : "no leaf PTE and no VMA covers this address");
    } else {
      fault_note(0, 0, "no leaf PTE for this address");
    }
    return -1;
  }
  u64 pte = *leaf0;
  vmm_read_release(rflags);

  // Case 1: Lazy page (VMM_LAZY leaf, not present) — anonymous or file-backed.
  if (!(pte & VMM_PRESENT) && (pte & VMM_LAZY)) {
    /* Zero-page dedup: classify the faulting address up front. If it is
     * anonymous (no covering file-backed VMA), map the single shared zero
     * page read-only (+COW when the mapping is writable) instead of allocating
     * a frame — the first store materialises a private page via the COW path.
     * File-backed faults skip this and run the normal allocate+fill below. */
    int anon_page = 1;
    struct vm_area *va = current_task->vma_list;
    while (va) {
      if (page_aligned >= va->start && page_aligned < va->end) {
        if (va->node && va->node->inode) {
          struct vfs_inode *in = va->node->inode;
          if (in->type == VFS_FILE || in->read_cb || in->data)
            anon_page = 0;
        }
        break;
      }
      va = va->next;
    }
    /* Not for a store. See the note on the same decision in the no-leaf path
     * above: a page whose first access is a write pays two faults for the
     * shared zero page and then allocates the frame anyway, and an allocator
     * filling the block it has just been handed does exactly that for every
     * page of it. Fall through to the allocate-and-map path below, which
     * serves the store in one fault. */
    if (anon_page && !(error_code & PF_WRITE) && pmm_zero_page()) {
      u64 cflags;
      vmm_write_acquire(&cflags);
      u64 *slot = pf_leaf_pte_ptr(page_aligned);
      if (!slot || (*slot & VMM_PRESENT) || !(*slot & VMM_LAZY)) {
        /* Another CPU serviced it (or it was torn down). */
        vmm_write_release(cflags);
        return (slot && (*slot & VMM_PRESENT)) ? 0 : -1;
      }
      u64 zflags = VMM_PRESENT;
      if (*slot & VMM_WRITABLE) zflags |= VMM_COW; /* first store COWs */
      if (*slot & VMM_USER) zflags |= VMM_USER;
      /* By the mapping, not by who faulted — see the note on the same test in
       * the file-backed path below. A zero page brought in by a syscall
       * writing through a lazy user mapping must still belong to userspace.
       * The lazy marker's own user bit is inherited just above and is the
       * usual answer; the VMA is the fallback for a marker that lost it. */
      if ((error_code & PF_USER) ||
          (current_task && vma_lookup(current_task, page_aligned)))
        zflags |= VMM_USER;
      if (*slot & VMM_NO_EXECUTE) zflags |= VMM_NO_EXECUTE;
      *slot = pmm_zero_page() | zflags;
      if (vma_trace_faults_enabled())
        vma_trace_record("pf-lazy-zero", page_aligned, page_aligned + PAGE_SIZE);
      invalidate_page(page_aligned);
      vmm_write_release(cflags);
      /* Shared frame: never registered in the eviction ring (swap must not
       * evict a page mapped read-only in many address spaces). */
      return 0;
    }
    u64 frame = pmm_alloc_frame();
    if (!frame) {
      eviction_evict_page();
      frame = pmm_alloc_frame();
      if (!frame) {
        console_write("pf: OOM during lazy allocation, swap failed\n");
        return -1;
      }
    }
    void *new_frame_virt = (void *)((u64)frame + DIRECT_MAP_BASE);
    memset(new_frame_virt, 0, PAGE_SIZE);

    // File-backed fill happens OUTSIDE vmm_lock (read_cb / blk_*_cached block).
    int shared_cache_frame = 0;
    int vma_shared = 0;
    struct vm_area *vma = vma_lookup(current_task, page_aligned);
    while (vma) {
      if (page_aligned >= vma->start && page_aligned < vma->end) {
        /* MAP_SHARED file pages must stay shared across fork: the page-cache
         * frame is the single backing store for every mapper (e.g. an M48
         * memfd shared between a client and displayd). Without VMM_SHARED the
         * fork CoW path (clone_table) would copy the page on the next write,
         * silently de-sharing the mapping. */
        vma_shared = (vma->flags & MAP_SHARED) != 0;
        if (vma->node && vma->node->inode) {
          u64 file_offset = vma->offset + (page_aligned - vma->start);
          u64 file_page = file_offset & ~(PAGE_SIZE - 1);

          if (vma->node->inode->type == VFS_FILE) {
            /* M72: a writable MAP_SHARED file page is potentially-dirty the
             * moment it is mapped — stores through the PTE won't fault again, so
             * we cannot observe the write later. Mark the page-cache entry dirty
             * now so reactive reclaim writes it back instead of dropping it as
             * "clean", which used to lose mmap stores that raced ahead of msync.
             * Read-only or MAP_PRIVATE mappings are untouched. */
            int mark_dirty = vma_shared && (vma->prot & PROT_WRITE);
            struct page_cache_entry *page = page_cache_get_page(vma->node->inode, file_page);

            /* Miss: ask for the neighbourhood, not just this page.
             *
             * Each miss costs a disk round trip — 2.6 ms measured — and a
             * demand-paged browser takes tens of thousands of them. One cluster
             * read fills a whole window of cache entries, so the faults that
             * follow in this neighbourhood find their page resident, and the
             * fault-around below maps them without faulting at all.
             *
             * The size is asked for, not stated: page_cache_cluster_pages()
             * derives it from RAM (16 pages on a small guest, up to 64 on a
             * large one) so this path is not pinned to a constant that a bigger
             * machine has long outgrown. A read-only mapping only: a private
             * writable page must not be filled from a shared cache entry it
             * would then copy. */
            if (!page && !mark_dirty && vma->node->inode->read_cb) {
              page_cache_read_cluster(vma->node->inode, file_page,
                                      page_cache_cluster_pages());
              page = page_cache_get_page(vma->node->inode, file_page);
            }
            if (page) {
              pmm_free_frame(frame); // drop the freshly allocated frame
              frame = page->frame;
              pmm_ref_frame(frame);  // VMA references it
              if (mark_dirty)
                page_cache_mark_dirty(page);
              page_cache_put_page(page);
              shared_cache_frame = 1;
            } else if (vma->node->inode->read_cb) {
              isize res = vma->node->inode->read_cb(vma->node, file_page, (char *)new_frame_virt, PAGE_SIZE, 0);
              if (res >= 0) {
                if (page_cache_add_page(vma->node->inode, file_page, frame) == 0) {
                  pmm_ref_frame(frame); // cache ref + VMA ref
                  /* The frame now belongs to the page cache as well, so it is
                   * shared exactly like the hit path above — every later mapper
                   * of this file page, and every read()/pread(), is served from
                   * it. Say so, or the COW downgrade below does not fire and a
                   * writable MAP_PRIVATE mapping gets the cached frame mapped
                   * WRITABLE: the process's private stores then land straight
                   * in the page cache, silently rewriting the file's contents
                   * for everyone else.
                   *
                   * This is what corrupted libpam.so.2: ELF pads its RX segment
                   * out to the same file page its RW segment starts on, so
                   * ld.so's relocation stores through the writable mapping
                   * overwrote the cached copy of the page that also holds the
                   * library's `.plt` tail. Later mappers executed the resulting
                   * garbage and died on a #UD, and even a plain read() of the
                   * file returned the runtime pointers instead of its code. */
                  shared_cache_frame = 1;
                  if (mark_dirty) {
                    struct page_cache_entry *pe =
                        page_cache_get_page(vma->node->inode, file_page);
                    if (pe) {
                      page_cache_mark_dirty(pe);
                      page_cache_put_page(pe);
                    }
                  }
                }
              } else {
                pmm_free_frame(frame);
                return -1;
              }
            } else if (vma->node->inode->data) {
              /* A file that lives in inode->data and has no read callback:
               * initramfs images, and every in-memory file created through the
               * VFS — which is what a POSIX shared-memory object is, since
               * shm_open() is open() under /dev/shm.
               *
               * Populating the frame is not enough. The frame must go into the
               * page cache, because that is the only thing that makes two
               * processes mapping the same file page share one frame. Without
               * it each mapper faulted in a private copy and every write was
               * invisible to the other side: descriptors passed, buffers were
               * accepted, frame callbacks fired, and the screen stayed blank.
               * memfd escaped this only because it happens to install a read
               * callback, so the branch above claimed it — the presence of a
               * callback silently decided whether a mapping was shared. */
              if (file_page < vma->node->inode->size) {
                usize copy_size = vma->node->inode->size - file_page;
                if (copy_size > PAGE_SIZE)
                  copy_size = PAGE_SIZE;
                memcpy(new_frame_virt,
                       (const char *)vma->node->inode->data + file_page,
                       copy_size);
              }
              if (page_cache_add_page(vma->node->inode, file_page, frame) == 0) {
                pmm_ref_frame(frame); /* cache ref + VMA ref */
                shared_cache_frame = 1;
                if (mark_dirty) {
                  struct page_cache_entry *pe =
                      page_cache_get_page(vma->node->inode, file_page);
                  if (pe) {
                    page_cache_mark_dirty(pe);
                    page_cache_put_page(pe);
                  }
                }
              }
            }
          } else if (vma->node->inode->read_cb) {
            isize res = vma->node->inode->read_cb(vma->node, file_offset, (char *)new_frame_virt, PAGE_SIZE, 0);
            if (res < 0) {
              pmm_free_frame(frame);
              return -1;
            }
          }
        }
        break;
      }
      vma = vma->next;
    }

    // Commit: re-read the leaf; only install if it is still the same LAZY PTE.
    u64 cflags;
    vmm_write_acquire(&cflags);
    u64 *slot = pf_leaf_pte_ptr(page_aligned);
    if (!slot || (*slot & VMM_PRESENT) || !(*slot & VMM_LAZY)) {
      // Another CPU serviced it (or it was torn down) — discard our frame.
      vmm_write_release(cflags);
      if (shared_cache_frame) pmm_unref_frame(frame); else pmm_free_frame(frame);
      return (slot && (*slot & VMM_PRESENT)) ? 0 : -1;
    }
    /* Honour the protection saved in the lazy PTE (paging_mprotect_page stored
     * VMM_WRITABLE/USER at mmap/loader time) instead of forcing writable. A
     * read-only file-backed mapping — e.g. a demand-paged executable's RO text,
     * which shares one refcounted page-cache frame across every mapper — must
     * fault in read-only so a stray write can't corrupt the shared cache page. */
    u64 flags = VMM_PRESENT;
    if (*slot & VMM_WRITABLE) flags |= VMM_WRITABLE;
    if (*slot & VMM_USER) flags |= VMM_USER;
    /* The lazy marker also carries the execute permission the mapping was
     * created with. Rebuilding the entry from a whitelist that omitted it made
     * every lazily-faulted page executable no matter what the caller asked
     * for — which is most of userspace, mmap being lazy for anonymous and
     * file-backed alike. */
    if (*slot & VMM_NO_EXECUTE) flags |= VMM_NO_EXECUTE;
    /* A fault raised in ring 3 must produce a page ring 3 can use.
     *
     * The user bit was recovered from the previous entry alone, so an entry
     * that had lost it — a lazy marker installed without one, a slot cleared
     * and refilled — materialised a SUPERVISOR page under a user mapping. The
     * process then took a protection fault on its own code with the entry
     * reading present-and-supervisor, which is a SIGSEGV at the instruction
     * pointer itself and looks nothing like a missing page. */
    /* The mapping decides this, not who faulted.
     *
     * Gating on PF_USER alone meant a page first touched by the KERNEL — a
     * copyout into a user buffer, a read(2) filling it, any syscall writing
     * through a lazy mapping — was materialised without the user bit and
     * stayed that way. The process then took a protection fault reading its
     * own memory, with the entry present-and-supervisor: observed as a SIGSEGV
     * inside PartitionRoot::FreeInUnknownRoot.
     *
     * The fix for that was first written as "any address below the user/kernel
     * split", which is a different claim and a wrong one. Low addresses also
     * hold mappings the kernel made and owns, and the user bit is what
     * address-space teardown uses to decide a frame is the process's to free —
     * so marking them user handed the allocator frames that were never the
     * process's. It showed up as nine double frees of the 2 MiB frame when a
     * static binary loaded there exited.
     *
     * We are here because a VMA covers this address: the page belongs to this
     * process's address space by construction, whoever faulted it in. */
    if ((error_code & PF_USER) || vma)
      flags |= VMM_USER;
    if (vma_shared) flags |= VMM_SHARED;
    /* A writable MAP_PRIVATE file page must NOT map the shared page-cache
     * frame writable: the first store (e.g. ld.so applying relocations to a
     * library's data segment) would be written INTO the cache and served to
     * every later mapper of the file — the next process's ld.so then reads
     * pre-relocated garbage (observed: libOSMesa's DYNAMIC page carrying
     * another process's relocated pointers → decode_dyn found no DT_HASH →
     * find_sym crash). Map it read-only + COW so the first write copies the
     * page out of the cache, exactly like a forked private page. */
    if (shared_cache_frame && !vma_shared && (flags & VMM_WRITABLE)) {
      flags &= ~VMM_WRITABLE;
      flags |= VMM_COW;
    }
    *slot = frame | flags;
    if (vma_trace_faults_enabled())
      vma_trace_record("pf-file/anon", page_aligned, page_aligned + PAGE_SIZE);
    invalidate_page(page_aligned);
    vmm_write_release(cflags);
    /* Shared page-cache frames are owned by the cache and mapped in several
     * address spaces — leave them out of the per-task swap set (same as SysV
     * shm, which never registers either).
     *
     * That is what this comment has always said and not what the condition
     * did: `!vma_shared` excludes a MAP_SHARED mapping, and a MAP_PRIVATE
     * read-only mapping of a shared library is not one — its frame comes
     * straight out of the page cache and is mapped by every process that
     * loaded the library. Registered, it could be chosen by the evictor, which
     * writes it to swap, marks THIS task's entry swapped and hands the frame
     * back for immediate reuse without ever asking who else holds it: the
     * cache's entry and every other mapper's page table still point at it.
     * The next program to execute from that page runs whatever was written
     * over it. */
    if (!vma_shared && !shared_cache_frame)
      eviction_register_page(current_task, page_aligned, frame);
    /* Map the neighbours that are already cached, while we are here.
     *
     * A 210 MB executable demand-paged one page per fault is fifty thousand
     * faults, each walking the tables and taking the page-cache lock, and the
     * read-ahead has usually put the next pages in the cache already — they
     * cost a lookup and a store here, versus a full fault apiece later. Only
     * cache hits are mapped: a miss would mean disk I/O, which belongs on the
     * fault that actually needs the page, not on this one.
     *
     * Bounded to the rest of this page table so no new table is allocated, and
     * to the mapping's own extent. */
    if (vma && vma->node && vma->node->inode && !(error_code & PF_WRITE)) {
      const u64 table_span = 512 * PAGE_SIZE;
      u64 stop = (page_aligned + table_span) & ~(table_span - 1);

      if (stop > vma->end)
        stop = vma->end;
      if (stop > page_aligned + 16 * PAGE_SIZE)
        stop = page_aligned + 16 * PAGE_SIZE;

      for (u64 va = page_aligned + PAGE_SIZE; va < stop; va += PAGE_SIZE) {
        u64 off = vma->offset + (va - vma->start);
        struct page_cache_entry *near =
            page_cache_get_page(vma->node->inode, off & ~(PAGE_SIZE - 1));

        if (!near)
          break; /* not cached: leave it to its own fault */

        u64 nflags;
        vmm_write_acquire(&nflags);
        u64 *nslot = pf_leaf_pte_ptr(va);
        int installed = 0;

        if (nslot && !(*nslot & VMM_PRESENT) && !(*nslot & VMM_SWAPPED)) {
          /* A neighbour is ALWAYS a page-cache frame -- it came out of the
           * cache two lines up -- but `flags` was decided for the faulting
           * page, whose frame need not have been. The COW downgrade above
           * fires on `shared_cache_frame`, and that is 0 whenever the faulting
           * page could not be added to the cache: a duplicate insert lost to
           * another CPU, or an allocation refused under pressure. `flags` then
           * still carries VMM_WRITABLE, and copying it here maps a shared
           * cache page writable into a MAP_PRIVATE mapping.
           *
           * That is the libpam.so.2 corruption again, arriving by a different
           * road: ld.so's relocation stores land in the page cache instead of
           * in a private copy, every later mapper of the library gets the
           * relocated bytes, and the one that executes them dies on a #UD.
           *
           * Found while chasing exactly such a #UD, which this did NOT fix --
           * the bytes at that faulting instruction turned out to be correct.
           * It is a real hole regardless: a private mapping must never be able
           * to write into the cache.
           *
           * The neighbour's own protection is what decides this, and the
           * neighbour's frame is shared by construction. */
          u64 f = flags;

          if (!vma_shared && (f & VMM_WRITABLE)) {
            f &= ~VMM_WRITABLE;
            f |= VMM_COW;
          }
          pmm_ref_frame(near->frame);
          *nslot = near->frame | f;
          invalidate_page(va);
          installed = 1;
        }
        vmm_write_release(nflags);
        /* A writable shared mapping can be stored through without faulting
         * again, so the entry is potentially dirty from the moment it is
         * mapped -- the same reason the faulting page above is marked. Outside
         * the page-table lock: this takes the cache's own lock. */
        if (installed && vma_shared && (flags & VMM_WRITABLE))
          page_cache_mark_dirty(near);
        page_cache_put_page(near);
        if (!installed)
          break;
      }
    }
    return 0;
  }

  // Case 2: Swapped page (VMM_SWAPPED leaf, not present). The swap slot index is
  // encoded in the PTE's address field — read it directly, no reverse-map scan.
  if (!(pte & VMM_PRESENT) && (pte & VMM_SWAPPED)) {
    // swap_in allocates a frame and does blocking disk I/O — outside the lock.
    u64 new_frame = 0;
    u32 swslot = (u32)((pte & PAGE_ENTRY_ADDRESS_MASK) >> 12);
    if (swap_in(swslot, &new_frame) < 0) {
      console_write("pf: swap in failed for 0x");
      console_write_hex64(page_aligned);
      console_write("\n");
      return -1;
    }

    u64 cflags;
    vmm_write_acquire(&cflags);
    u64 *slot = pf_leaf_pte_ptr(page_aligned);
    if (!slot || (*slot & VMM_PRESENT) || !(*slot & VMM_SWAPPED)) {
      // Raced with another fault/unmap — our frame is now orphaned.
      vmm_write_release(cflags);
      pmm_free_frame(new_frame);
      return (slot && (*slot & VMM_PRESENT)) ? 0 : -1;
    }
    u64 flags = VMM_PRESENT | VMM_WRITABLE;
    if (*slot & VMM_USER) flags |= VMM_USER;
    if (*slot & VMM_NO_EXECUTE) flags |= VMM_NO_EXECUTE;
    *slot = new_frame | flags;
    if (vma_trace_faults_enabled())
      vma_trace_record("pf-cow-a", page_aligned, page_aligned + PAGE_SIZE);
    invalidate_page(page_aligned);
    vmm_write_release(cflags);
    eviction_register_page(current_task, page_aligned, new_frame);
    return 0;
  }

  // Case 3: Copy-on-Write (write to a cloned private page).
  if ((error_code & PF_WRITE) && (pte & VMM_PRESENT) && (pte & VMM_COW)) {
    // Pre-allocate a copy frame outside the lock (we may not need it if we are
    // the last sharer, but allocating speculatively keeps the commit section
    // non-blocking). pmm_alloc_frame fast path doesn't block.
    u64 new_frame = pmm_alloc_frame();
    if (!new_frame) {
      eviction_evict_page();
      new_frame = pmm_alloc_frame();
      if (!new_frame) return -1;
    }

    u64 cflags;
    vmm_write_acquire(&cflags);
    u64 *slot = pf_leaf_pte_ptr(page_aligned);
    if (!slot || !(*slot & VMM_PRESENT) || !(*slot & VMM_COW)) {
      // Another CPU already resolved the COW for this page.
      vmm_write_release(cflags);
      pmm_free_frame(new_frame);
      return 0;
    }
    u64 cur = *slot;
    u64 old_frame = cur & PAGE_ENTRY_ADDRESS_MASK;
    u64 new_flags = (cur & ~PAGE_ENTRY_ADDRESS_MASK);
    new_flags &= ~VMM_COW;
    new_flags |= VMM_PRESENT | VMM_WRITABLE;

    /* A COW that changes the frame must be published to every CPU, not just
     * this one.
     *
     * Threads share an address space: while this CPU resolves the fault, a
     * sibling on another CPU may still have the old, read-only translation in
     * its TLB, and x86 keeps serving reads from it. That sibling then reads the
     * pre-copy contents of the page indefinitely — two threads disagreeing
     * about the same word. It cost the browser about four minutes of every
     * start-up: a mutex on the main thread's stack, freshly COWed after a
     * zygote fork, read as still-contended on one CPU and as free on the other,
     * so the unlocker saw no waiter to wake and the waiter slept until a signal
     * happened to release it. invalidate_page below only flushes the CPU
     * taking the fault; the shootdown goes out after the lock is dropped, the
     * same order vmm_unmap_page uses. */
    if (old_frame == pmm_zero_page()) {
      /* First write to the shared zero page (zero-page dedup): the spare frame
       * is already zero-filled, so there is nothing to copy, and the zero page
       * must never be unreferenced or eviction-unregistered (it is shared and
       * has a permanent reservation). */
      addrspace_note_replaced(*slot);
      *slot = new_frame | new_flags;
      if (vma_trace_faults_enabled())
        vma_trace_record("pf-cow-b", page_aligned, page_aligned + PAGE_SIZE);
      invalidate_page(page_aligned);
      vmm_write_release(cflags);
      cow_publish_page(page_aligned);
      eviction_register_page(current_task, page_aligned, new_frame);
      return 0;
    }

    if (pmm_get_refcount(old_frame) == 1) {
      // Sole owner now — just flip to writable in place, no copy needed.
      addrspace_note_replaced(*slot);
      *slot = old_frame | new_flags;
      if (vma_trace_faults_enabled())
        vma_trace_record("pf-cow-c", page_aligned, page_aligned + PAGE_SIZE);
      invalidate_page(page_aligned);
      vmm_write_release(cflags);
      pmm_free_frame(new_frame); // didn't need the spare
      return 0;
    }

    // Shared: copy into the spare frame and point this mapping at it. The copy
    // touches only the two physical frames via the direct map (not the fault
    // va), so it is safe under the lock.
    memcpy((void *)(usize)(new_frame + DIRECT_MAP_BASE),
           (void *)(usize)(old_frame + DIRECT_MAP_BASE), PAGE_SIZE);
    addrspace_note_replaced(*slot);
    *slot = new_frame | new_flags;
    pmm_unref_frame(old_frame);
    invalidate_page(page_aligned);
    vmm_write_release(cflags);
    cow_publish_page(page_aligned);

    eviction_unregister_page(old_frame);
    eviction_register_page(current_task, page_aligned, new_frame);
    return 0;
  }

  /* Nothing above claimed this fault — usually a wild access, but not always.
   *
   * Threads share an address space, and a sibling on another CPU may resolve
   * the very page this fault was taken on (a copy-on-write break, an mprotect)
   * between the moment the CPU raised the fault and the moment this handler
   * read the tables. Every case above then declines it: the leaf no longer
   * says copy-on-write, is no longer lazy and is no longer swapped, because it
   * is simply present and writable. Falling off the end of this function kills
   * the thread for writing to memory it is entitled to write to — which is how
   * a compositor's thread came to die at error 0x7 on a page whose entry, in
   * the very report that announced its death, read present, writable and user.
   *
   * If the live entry permits what faulted, the honest answer is to run the
   * instruction again — always, not a few times.
   *
   * The count that used to bound this lived in file-scope statics shared by
   * every core, so one CPU's retries were charged to another CPU's address:
   * with several threads faulting at once the allowance was spent on faults
   * that had nothing to do with each other, and the next thread to be forgiven
   * nothing died. That is the SIGSEGV above, and it killed a fork/exec
   * workload about one run in four on six cores. Per-CPU, the count means
   * something. It stays bounded, because a fault that really is the program's
   * own must still reach the signal that reports it — a test that provokes one
   * and waits for SIGSEGV would otherwise wait forever — but the bound is now
   * per-CPU and generous, and every eighth attempt on the same address reloads
   * CR3, which drops every non-global translation this CPU holds. */
  {
    struct percpu *rs_pcpu = get_percpu();
    static u64 resolved_va_cpu[MAX_CPUS];
    static unsigned resolved_repeat_cpu[MAX_CPUS];
    unsigned rs_cpu = rs_pcpu ? (unsigned)rs_pcpu->cpu_id : 0u;

    if (rs_cpu >= MAX_CPUS)
      rs_cpu = 0;
    u64 *resolved_va = &resolved_va_cpu[rs_cpu];
    unsigned *resolved_repeat = &resolved_repeat_cpu[rs_cpu];
    u64 rf;
    u64 live = 0;

    vmm_read_acquire(&rf);
    {
      u64 *now = pf_leaf_pte_ptr(page_aligned);

      if (now)
        live = *now;
    }
    vmm_read_release(rf);

    if ((live & VMM_PRESENT) &&
        (!(error_code & PF_WRITE) || (live & VMM_WRITABLE)) &&
        (!(error_code & PF_USER) || (live & VMM_USER))) {
      if (page_aligned != *resolved_va) {
        *resolved_va = page_aligned;
        *resolved_repeat = 0;
      }
      if ((++*resolved_repeat % 8u) == 0u) {
        u64 cr3;

        __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("movq %0, %%cr3" : : "r"(cr3) : "memory");
      }
      if (*resolved_repeat <= 64) {
        if (*resolved_repeat <= 8)
          fault_note(1, live, "another CPU resolved this entry; retrying");
        invalidate_page(page_aligned);
        return 0;
      }
    }
  }

  return -1; // Unhandled
}

u64 paging_create_address_space(void) {
  u64 *pml4 = alloc_page_table();
  u64 pml4_phys = table_to_phys(pml4);

  u64 _vmflags;
  vmm_read_acquire(&_vmflags);

  // Clone kernel-half entries (256-511)
  for (usize i = 256; i < 512; i++) {
    pml4[i] = kernel_pml4_virt[i];
  }

  // Instead of sharing PML4 entry 0 directly, we allocate a private PDPT for PML4 entry 0
  // and clone the kernel's identity mapping (0-4 GB) into it.
  u64 *kernel_pml4 = kernel_pml4_virt;
  if (kernel_pml4[0] & VMM_PRESENT) {
    u64 *kernel_pdpt = table_from_entry(kernel_pml4[0]);
    u64 *dst_pdpt = alloc_page_table();
    pml4[0] = table_to_phys(dst_pdpt) | (kernel_pml4[0] & ~PAGE_ENTRY_ADDRESS_MASK);

    // Clone the first 4 entries of the kernel's PDPT (which cover 0-4 GB)
    for (usize j = 0; j < 4; j++) {
      if (kernel_pdpt[j] & VMM_PRESENT) {
        if (kernel_pdpt[j] & HUGE_PAGE_FLAG) {
          dst_pdpt[j] = kernel_pdpt[j];
        } else {
          // Allocate a private PD for this gigabyte
          u64 *kernel_pd = table_from_entry(kernel_pdpt[j]);
          u64 *dst_pd = alloc_page_table();
          dst_pdpt[j] = table_to_phys(dst_pd) | (kernel_pdpt[j] & ~PAGE_ENTRY_ADDRESS_MASK);

          // Copy all 512 entries of this PD (these are the 2MB huge pages)
          for (usize k = 0; k < 512; k++) {
            dst_pd[k] = kernel_pd[k];
          }
        }
      }
    }
  }

  vmm_read_release(_vmflags);

  /* A brand-new address space now exists, possibly on a frame that a dead one
   * used to occupy. Invalidate every CPU's "already loaded" record. */
  addrspace_epoch_bump();
  return pml4_phys;
}

/* Drain any in-flight cross-CPU TLB shootdown (defined in arch/x86/tlb.c).
 * clone_table / free_table / swap_in_recursive walk the whole user page-table
 * tree doing real work — not spinning — while the caller holds an IRQs-off
 * section (paging_clone_address_space takes vmm_write_lock via its _irqsave
 * variant; the exit reaper frees with interrupts disabled). With IRQs masked
 * and no spin loop, such a CPU never ACKs a shootdown IPI, so an initiator on
 * another core hits `tlb: shootdown stalled, pending=1` and panics — exactly
 * the smp>=2 in-guest-build failure. A poll once per visited table keeps the
 * ACK latency far under the runaway guard; it is a single atomic load on the
 * common (nothing-pending) path, so uniprocessor and uncontended SMP pay
 * nothing. */
void tlb_shootdown_poll(void);

static void clone_table(u64 *src_table, u64 *dst_table, int level) {
  tlb_shootdown_poll();
  for (usize i = 0; i < 512; i++) {
    if (!(src_table[i] & VMM_PRESENT)) {
      dst_table[i] = src_table[i]; // Copy lazy/swapped entries as is
      continue;
    }

    u64 entry = src_table[i];

    // If it's a huge page (PS bit set), it's a leaf entry (1GB at level 1, or 2MB at level 2)
    if (entry & HUGE_PAGE_FLAG) {
      dst_table[i] = entry;
      continue;
    }

    if (level < 3) {
      // PML4, PDPT, or PD -> recurse
      u64 *src_child = table_from_entry(entry);
      u64 *dst_child = alloc_page_table();
      dst_table[i] = table_to_phys(dst_child) | (entry & ~PAGE_ENTRY_ADDRESS_MASK);
      clone_table(src_child, dst_child, level + 1);
    } else {
      // PT -> copy frame and handle CoW
      u64 frame = entry & PAGE_ENTRY_ADDRESS_MASK;

      if (entry & VMM_USER) {
        if ((entry & VMM_WRITABLE) && !(entry & VMM_SHARED)) {
          // Mark both as Read-Only for CoW
          entry &= ~VMM_WRITABLE;
          entry |= VMM_COW;
          src_table[i] = entry;
        }

        dst_table[i] = entry;
        if (frame && frame != pmm_zero_page()) {
          /* The shared zero page keeps its permanent reservation refcount; a
           * fork of an untouched (still shared) anonymous page adds no
           * reference, and pmm_free_frame ignores it on teardown anyway. */
          pmm_ref_frame(frame);
        }
      } else {
        dst_table[i] = entry;
      }
    }
  }
}

u64 paging_clone_address_space(u64 src_pml4_phys) {
  u64 real_src_phys = src_pml4_phys ? src_pml4_phys : kernel_pml4_phys;
  u64 *src_pml4 = (u64 *)(usize)(real_src_phys + DIRECT_MAP_BASE);
  u64 *dst_pml4 = alloc_page_table();
  u64 dst_pml4_phys = table_to_phys(dst_pml4);

  /* M28 #7 T4: serialize page-table reads against concurrent vmm_map_page
   * /vmm_unmap_page writes. Without BKL the syscall path lets two CPUs run
   * mm syscalls in parallel; a fork that walks its src pml4 while another
   * core mutates the same pml4 (mmap, page-fault CoW handler, eviction)
   * sees torn flag/address words and produces a corrupt child mapping that
   * faults the moment the child enters ring 3. The clone_table CoW pass
   * also writes back to src PT entries, so we need the write side. */
  u64 _vmflags;
  vmm_write_acquire(&_vmflags);

  // Clone user-half entries (0-255)
  for (usize i = 0; i < 256; i++) {
    if (src_pml4[i] & VMM_PRESENT) {
      u64 *src_pdpt = table_from_entry(src_pml4[i]);
      u64 *dst_pdpt = alloc_page_table();
      dst_pml4[i] = table_to_phys(dst_pdpt) | (src_pml4[i] & ~PAGE_ENTRY_ADDRESS_MASK);
      clone_table(src_pdpt, dst_pdpt, 1);
    }
  }

  // Copy kernel-half entries (256-511)
  for (usize i = 256; i < 512; i++) {
    dst_pml4[i] = kernel_pml4_virt[i];
  }

  if (real_src_phys == read_cr3()) {
    write_cr3(real_src_phys);
  }

  vmm_write_release(_vmflags);

  /* Two reasons to announce here. The child is a new address space on a frame
   * that may have belonged to a dead one; and this clone just flipped the
   * parent's writable user pages to COW in place, so any OTHER CPU running a
   * sibling thread of the parent has to drop its stale writable translations
   * too. The CR3 reload above only covers this one.
   *
   * The epoch bump alone is not enough for the second reason. It is collected
   * at the next context switch, and a sibling thread busy in user code does not
   * switch: until it happens to, it keeps writing through a stale WRITABLE
   * entry to the pre-fork frame while this address space has already moved on.
   * Two threads then disagree about the same word — which is how the browser's
   * main thread came to sleep on a mutex that the rest of the process saw as
   * free, with nobody left to wake it. A fork is rare enough to pay for a real
   * shootdown; the epoch stays as the record for CPUs that are not running
   * this space at all. */
  addrspace_epoch_bump();
  {
    extern void tlb_shootdown_all(void);

    if (irqs_are_enabled())
      tlb_shootdown_all();
  }

  return dst_pml4_phys;
}


/* Mark vaddr's leaf PTE as swapped, storing the swap SLOT index in the PTE's
 * (now-unused) address field. The page is non-present, so the address bits are
 * free to carry the slot; the #PF handler reads it back to drive swap_in with no
 * reverse-map table. */
void paging_mark_swapped(u64 pml4_phys, u64 vaddr, u64 slot) {
  u64 *pml4 = (u64 *)(usize)(pml4_phys ? (pml4_phys + DIRECT_MAP_BASE) : (u64)(usize)kernel_pml4_virt);
  u64 pml4e = pml4[pml4_index(vaddr)];
  if (!(pml4e & VMM_PRESENT)) return;

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(vaddr)];
  if (!(pdpte & VMM_PRESENT)) return;
  if (pdpte & HUGE_PAGE_FLAG) return;

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(vaddr)];
  if (!(pde & VMM_PRESENT)) return;
  if (pde & HUGE_PAGE_FLAG) return;

  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(vaddr)];

  if (pte & VMM_PRESENT) {
    u64 flags = pte & ~PAGE_ENTRY_ADDRESS_MASK;
    flags &= ~VMM_PRESENT;
    flags |= VMM_SWAPPED;
    pt[pt_index(vaddr)] = ((slot << 12) & PAGE_ENTRY_ADDRESS_MASK) | flags;
    addrspace_note_replaced(pte);
    invalidate_page(vaddr);
  }
}

/* Swap slots are now freed incrementally as their pages are unmapped
 * (unmap_page_from_pml4) and as a teardown backstop in free_table — there is no
 * separate per-exit page-table walk. A standalone full-tree walk on every exit
 * (the first cut of this) regressed the heavy multi-threaded smoke instances;
 * folding the slot-free into the existing unmap/teardown paths is both correct
 * and free. Kept as a no-op so swap_free_all_slots has a stable callee. */
void paging_free_swap_slots(u64 pml4_phys) {
  (void)pml4_phys;
}

int paging_test_and_clear_accessed(u64 pml4_phys, u64 vaddr) {
  u64 *pml4 = (u64 *)(usize)(pml4_phys ? (pml4_phys + DIRECT_MAP_BASE) : (u64)(usize)kernel_pml4_virt);
  u64 pml4e = pml4[pml4_index(vaddr)];
  if (!(pml4e & VMM_PRESENT)) return 0;

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(vaddr)];
  if (!(pdpte & VMM_PRESENT)) return 0;
  if (pdpte & HUGE_PAGE_FLAG) return 0;

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(vaddr)];
  if (!(pde & VMM_PRESENT)) return 0;
  if (pde & HUGE_PAGE_FLAG) return 0;

  u64 *pt = table_from_entry(pde);
  if (!(pt[pt_index(vaddr)] & VMM_PRESENT)) return 0;

  /* Atomic clear: the MMU sets the Accessed/Dirty bits in this PTE from other
   * CPUs concurrently. A plain read-modify-write would clobber a hardware A/D
   * update landing between the read and the store, losing it. `lock and` (via
   * __atomic_fetch_and) clears our bit while preserving every other bit the
   * hardware races to set, and returns the prior value so we can test it. */
  u64 old = __atomic_fetch_and((u64 *)&pt[pt_index(vaddr)], ~VMM_ACCESSED,
                               __ATOMIC_SEQ_CST);
  if (old & VMM_ACCESSED) {
    /* Every CPU has to reload before it will set the bit again. */
    addrspace_note_replaced(old);
    invalidate_page(vaddr);
    return 1;
  }
  return 0;
}

/* M72: test-and-clear the hardware dirty bit of a 4 KiB user page. Used by
 * msync to discover which MAP_SHARED pages userspace actually wrote (the CPU
 * sets PTE.D on the store) so only those are written back to the file. Returns
 * 1 if the page was dirty (and clears the bit), 0 otherwise. */
int paging_test_and_clear_dirty(u64 pml4_phys, u64 vaddr) {
  u64 *pml4 = (u64 *)(usize)(pml4_phys ? (pml4_phys + DIRECT_MAP_BASE) : (u64)(usize)kernel_pml4_virt);
  u64 pml4e = pml4[pml4_index(vaddr)];
  if (!(pml4e & VMM_PRESENT)) return 0;

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(vaddr)];
  if (!(pdpte & VMM_PRESENT)) return 0;
  if (pdpte & HUGE_PAGE_FLAG) return 0;

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(vaddr)];
  if (!(pde & VMM_PRESENT)) return 0;
  if (pde & HUGE_PAGE_FLAG) return 0;

  u64 *pt = table_from_entry(pde);
  if (!(pt[pt_index(vaddr)] & VMM_PRESENT)) return 0;

  /* Atomic clear (see paging_test_and_clear_accessed): `lock and` so a
   * concurrent hardware Accessed/Dirty set on another CPU is not lost. */
  u64 old = __atomic_fetch_and((u64 *)&pt[pt_index(vaddr)], ~VMM_DIRTY,
                               __ATOMIC_SEQ_CST);
  if (old & VMM_DIRTY) {
    /* See paging_test_and_clear_accessed. */
    addrspace_note_replaced(old);
    invalidate_page(vaddr);
    return 1;
  }
  return 0;
}

/* M72: physical frame backing a user vaddr in the given address space (0 if not
 * present). Used by msync to write an mmap'd page's frame straight to its file. */
u64 paging_user_frame(u64 pml4_phys, u64 vaddr) {
  u64 *pml4 = (u64 *)(usize)(pml4_phys ? (pml4_phys + DIRECT_MAP_BASE) : (u64)(usize)kernel_pml4_virt);
  u64 pml4e = pml4[pml4_index(vaddr)];
  if (!(pml4e & VMM_PRESENT)) return 0;
  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(vaddr)];
  if (!(pdpte & VMM_PRESENT) || (pdpte & HUGE_PAGE_FLAG)) return 0;
  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(vaddr)];
  if (!(pde & VMM_PRESENT) || (pde & HUGE_PAGE_FLAG)) return 0;
  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(vaddr)];
  if (!(pte & VMM_PRESENT)) return 0;
  return pte & 0x000FFFFFFFFFF000ULL;
}

/* Resident 4 KiB pages in [start, end) of the address space at `pml4_phys`.
 *
 * The same answer as calling paging_user_frame once per page — including its
 * deliberate blind spot, that a huge-mapped page counts as absent — but it
 * descends the tables instead of restarting the walk at the PML4 for every
 * page. That is the whole point: a caller counting a region asks about pages
 * in address order, so the upper levels it re-reads are almost always the ones
 * it just read.
 *
 * The difference is not a constant factor. A region that is reserved but not
 * mapped — which is most of a browser's address space, since V8's sandbox and
 * every thread's guard pages are reservations — has no page tables under it at
 * all, and the per-page version still walked it one page at a time: 262,144
 * lookups to learn that one absent PDPT entry covers a gigabyte. Here an
 * absent entry advances the cursor to the end of what it covers, so the same
 * gigabyte costs one read. Measured on a headless Chromium start-up, this walk
 * (through task_rss_sample and /proc/<pid>/statm) was 82% of all kernel CPU
 * time.
 */
u64 paging_user_resident(u64 pml4_phys, u64 start, u64 end) {
  if (end <= start)
    return 0;

  u64 *pml4 = (u64 *)(usize)(pml4_phys ? (pml4_phys + DIRECT_MAP_BASE)
                                       : (u64)(usize)kernel_pml4_virt);
  const u64 pml4_span = 1ULL << 39;
  const u64 pdpt_span = 1ULL << 30;
  const u64 pd_span = 1ULL << 21;
  u64 count = 0;
  u64 va = start & ~(PAGE_SIZE - 1);

  while (va < end) {
    /* Where the entry at each level stops covering. Computed before the reads
     * so an absent entry can skip its whole span in one step. */
    u64 next_pml4 = (va & ~(pml4_span - 1)) + pml4_span;
    u64 next_pdpt = (va & ~(pdpt_span - 1)) + pdpt_span;
    u64 next_pd = (va & ~(pd_span - 1)) + pd_span;

    u64 pml4e = pml4[pml4_index(va)];
    if (!(pml4e & VMM_PRESENT)) {
      va = next_pml4;
      continue;
    }
    u64 *pdpt = table_from_entry(pml4e);
    u64 pdpte = pdpt[pdpt_index(va)];
    /* A 1 GiB leaf counts as absent, exactly as paging_user_frame reports it. */
    if (!(pdpte & VMM_PRESENT) || (pdpte & HUGE_PAGE_FLAG)) {
      va = next_pdpt;
      continue;
    }
    u64 *pd = table_from_entry(pdpte);
    u64 pde = pd[pd_index(va)];
    if (!(pde & VMM_PRESENT) || (pde & HUGE_PAGE_FLAG)) {
      va = next_pd;
      continue;
    }
    /* One page table, resolved once: scan its entries directly. */
    u64 *pt = table_from_entry(pde);
    u64 stop = next_pd < end ? next_pd : end;

    for (usize i = pt_index(va); va < stop; i++, va += PAGE_SIZE)
      if (pt[i] & VMM_PRESENT)
        count++;
  }
  return count;
}

/* Physical address backing `vaddr`, resolving 1 GiB and 2 MiB mappings as well
 * as 4 KiB ones. paging_user_frame above answers only for a 4 KiB leaf and
 * reports "not mapped" for a huge page — fine for callers that want to install
 * a page, wrong for anyone reading memory: b1nix maps large runs with huge
 * pages, so a reader built on the 4 KiB-only walk (ptrace PEEK,
 * /proc/<pid>/mem, process_vm_readv) sees whole regions of a live process as
 * absent. Returns 0 when nothing is mapped. */
u64 paging_user_phys(u64 pml4_phys, u64 vaddr) {
  u64 *pml4 = (u64 *)(usize)(pml4_phys ? (pml4_phys + DIRECT_MAP_BASE)
                                       : (u64)(usize)kernel_pml4_virt);
  u64 pml4e = pml4[pml4_index(vaddr)];
  if (!(pml4e & VMM_PRESENT))
    return 0;
  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(vaddr)];
  if (!(pdpte & VMM_PRESENT))
    return 0;
  if (pdpte & HUGE_PAGE_FLAG) /* 1 GiB page */
    return (pdpte & 0x000FFFFFC0000000ULL) + (vaddr & 0x3FFFFFFFULL);
  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(vaddr)];
  if (!(pde & VMM_PRESENT))
    return 0;
  if (pde & HUGE_PAGE_FLAG) /* 2 MiB page */
    return (pde & 0x000FFFFFFFE00000ULL) + (vaddr & 0x1FFFFFULL);
  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(vaddr)];
  if (!(pte & VMM_PRESENT))
    return 0;
  return (pte & 0x000FFFFFFFFFF000ULL) + (vaddr & 0xFFFULL);
}

/* Raw 4 KiB leaf PTE backing `virtual_address`, or 0 when the address is not
 * mapped by a 4 KiB leaf (unmapped, or covered by a 1 GiB / 2 MiB page). The
 * memory-type bits (PAT/PCD/PWT) live in this entry, so M98's WC self-test
 * reads it back rather than trusting the flags it passed to vmm_map_page. */
/* M100: build the kernel-half page-table path for [vaddr, vaddr+size) without
 * mapping anything into it.
 *
 * paging_create_address_space() copies PML4 entries 256..511 by value into
 * every new address space, so everything below the PML4 level is shared — but
 * only for entries that already exist when the process is created. A window
 * whose PML4 entry is first created later (from an ioctl, i.e. on a user
 * address space) would exist in exactly that one process and be missing in
 * every other. Installing the path up front, at boot, is what makes such a
 * window genuinely global; the alternative previously used here — map one page
 * and immediately unmap it — happened to work only because unmapping a page
 * leaves the tables above it in place.
 *
 * Returns 0 on success. */
int paging_reserve_kernel_path(u64 virtual_address, u64 size) {
  if (size == 0)
    return -1;
  u64 flags;
  vmm_write_acquire(&flags);
  u64 *pml4 = get_current_pml4();
  u64 start = virtual_address & ~(u64)(PAGE_SIZE - 1);
  u64 end = virtual_address + size;
  /* One PD covers 1 GiB; walking by that stride installs every level the range
   * needs without touching a single leaf entry. */
  for (u64 va = start; va < end; va += (1ULL << 30)) {
    u64 *pdpt = ensure_child_table(pml4, pml4_index(va));
    (void)ensure_child_table(pdpt, pdpt_index(va));
  }
  vmm_write_release(flags);
  return 0;
}

/* The PML4 entry the CURRENT address space holds for `virtual_address`. */
u64 paging_pml4_entry_current(u64 virtual_address) {
  u64 *pml4 = get_current_pml4();
  return pml4[pml4_index(virtual_address)];
}

/* The PML4 entry an address space holds for `virtual_address`. Used to assert
 * that a kernel window really is shared: a fresh address space must carry the
 * same entry the kernel table has. Returns 0 when absent. */
u64 paging_pml4_entry_in(u64 pml4_phys, u64 virtual_address) {
  if (!pml4_phys)
    return 0;
  u64 *pml4 = (u64 *)(usize)(pml4_phys + DIRECT_MAP_BASE);
  return pml4[pml4_index(virtual_address)];
}

u64 paging_leaf_pte(u64 virtual_address) {
  u64 *pml4 = get_current_pml4();
  u64 pml4e = pml4[pml4_index(virtual_address)];
  if (!(pml4e & VMM_PRESENT))
    return 0;
  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(virtual_address)];
  if (!(pdpte & VMM_PRESENT) || (pdpte & HUGE_PAGE_FLAG))
    return 0;
  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(virtual_address)];
  if (!(pde & VMM_PRESENT) || (pde & HUGE_PAGE_FLAG))
    return 0;
  u64 *pt = table_from_entry(pde);
  return pt[pt_index(virtual_address)];
}

void paging_dump_entries(u64 virtual_address) {
  u64 *pml4 = get_current_pml4();
  u64 pml4e = pml4[pml4_index(virtual_address)];
  console_write("PML4E for "); console_write_hex64(virtual_address); console_write(": 0x"); console_write_hex64(pml4e); console_write("\n");
  if (!(pml4e & VMM_PRESENT)) return;

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(virtual_address)];
  console_write("  PDPTE: 0x"); console_write_hex64(pdpte); console_write("\n");
  if (!(pdpte & VMM_PRESENT)) return;

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(virtual_address)];
  console_write("  PDE:   0x"); console_write_hex64(pde); console_write("\n");
  if (!(pde & VMM_PRESENT)) return;

  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(virtual_address)];
  console_write("  PTE:   0x"); console_write_hex64(pte); console_write("\n");
}

static u64 freed_tables_count = 0;

static void free_table(u64 *table, int level) {
  tlb_shootdown_poll(); /* see clone_table: drain shootdowns from this IRQs-off walk */
  if (level >= 3) {
    /* PT level: free the leaf user data frames. VMA-backed pages were already
     * unmapped (PTE cleared) by user_address_space_cleanup, so they are skipped
     * here; what remains present is memory NOT covered by a VMA — chiefly the
     * brk heap (cc1 grows it by tens of MB per compile). Without freeing it here
     * those frames leak permanently across process teardown (OOM after many
     * spawns). pmm_free_frame is refcount-aware, so shared/COW frames are safe. */
    for (usize i = 0; i < 512; i++) {
      u64 entry = table[i];
      if ((entry & VMM_PRESENT) && (entry & VMM_USER)) {
        u64 frame = entry & PAGE_ENTRY_ADDRESS_MASK;
        if (frame) {
          pmm_free_frame(frame);
          freed_tables_count++;
        }
      } else if (!(entry & VMM_PRESENT) && (entry & VMM_SWAPPED)) {
        /* A swapped page that was never VMA-unmapped (e.g. brk frames past the
         * heap VMA): release its swap slot during the exclusive teardown. */
        extern void swap_free_slot_index(u32 slot);
        swap_free_slot_index((u32)((entry & PAGE_ENTRY_ADDRESS_MASK) >> 12));
      }
    }
    return;
  }
  for (usize i = 0; i < 512; i++) {
    u64 entry = table[i];
    if ((entry & VMM_PRESENT) && !(entry & HUGE_PAGE_FLAG)) {
      u64 child_phys = entry & PAGE_ENTRY_ADDRESS_MASK;

      /* The entry has to name a frame the allocator still knows as a page
       * table. When it does not, that frame was handed to someone else while
       * this space still pointed at it, and following the pointer reads their
       * data as page-table entries — which is how this surfaces: a "corrupted
       * page-table entry" holding ASCII, one level deeper and with the frame
       * number already lost. Report it here, where the frame and its history
       * are still known. */
      if (!pmm_frame_is_page_table(child_phys)) {
        extern void pmm_report_page_table_history_pub(u64 frame);

        console_write("paging: address space holds a frame the allocator no "
                      "longer calls a page table: 0x");
        console_write_hex64(child_phys);
        console_write(" at level ");
        console_write_dec((u64)level);
        console_write("\n");
        pmm_report_page_table_history_pub(child_phys);
        panic("paging: page table freed while still mapped");
      }
      u64 *child = table_from_entry(entry);
      free_table(child, level + 1);
      pmm_note_page_table(child_phys, 0); /* no longer a table */
      pmm_free_frame(child_phys);
      freed_tables_count++;
    }
  }
}

void paging_free_address_space(u64 pml4_phys) {
  if (pml4_phys == 0 || pml4_phys == kernel_pml4_phys) {
    return;
  }

  /* Exactly one caller may release this space.
   *
   * The check below finds OTHER live users, and two exiting threads of one
   * process each found none — the sibling was already past TASK_DEAD, so each
   * excluded the other and both went on to free the same tables. Claiming the
   * PML4's page-table bit settles it atomically: the thread that takes the bit
   * does the release, the other returns. Without this the frames went back to
   * the allocator twice ("double free ... already parked in a bucket") and were
   * reissued between the two, which is how a live address space came to hold
   * entries made of somebody else's data. */
  {
    extern int pmm_claim_page_table_release(u64 frame);

    if (!pmm_claim_page_table_release(pml4_phys))
      return; /* another thread is already tearing this space down */

    /* This space is going away and its PML4 frame is about to go back to the
     * allocator. Any CPU still recording it as "loaded" must be made to write
     * CR3 again rather than trust that record. */
    addrspace_epoch_bump();
  }

  /* Nobody may still be running here. A space released under a live thread
   * does not fault: the frames go back to the allocator, the heap writes text
   * into what is still that thread's PML4, and the corruption surfaces much
   * later as a page-table entry holding ASCII — which is how this check came
   * to exist. Name the survivor here, where the caller doing the release is
   * still on the stack. */
  {
    usize other_id = 0;
    const char *other_name = 0;
    usize users = scheduler_address_space_users(pml4_phys, &other_id, &other_name);

    if (users != 0) {
      console_write("paging: address space 0x");
      console_write_hex64(pml4_phys);
      console_write(" freed while ");
      console_write_dec(users);
      console_write(" task(s) still run in it, first id ");
      console_write_dec(other_id);
      console_write(" (");
      console_write(other_name ? other_name : "?");
      console_write(") caller 0x");
      console_write_hex64((u64)(usize)__builtin_return_address(0));
      console_write("\n");
      panic("paging: address space freed under a running task");
    }
  }

  freed_tables_count = 0;
  u64 *pml4 = (u64 *)(usize)(pml4_phys + DIRECT_MAP_BASE);

  // Free user-half entries (0-255)
  for (usize i = 0; i < 256; i++) {
    u64 entry = pml4[i];
    if ((entry & VMM_PRESENT) && !(entry & HUGE_PAGE_FLAG)) {
      /* An entry that does not address the direct map is not a table, and
       * following it here reads an unrelated page as one. Say which slot of
       * which space held it, and what the frame was last used for, instead of
       * panicking on the value alone — the value is the only thing the walk can
       * report, and it never names the writer. */
      u64 child_phys = entry & PAGE_ENTRY_ADDRESS_MASK;
      if (child_phys >= DIRECT_MAP_SIZE) {
        extern void pmm_report_page_table_history_pub(u64 frame);
        console_write("paging_free_address_space: pml4 0x");
        console_write_hex64(pml4_phys);
        console_write(" slot ");
        console_write_dec(i);
        console_write(" holds a non-table entry 0x");
        console_write_hex64(entry);
        console_write("\n  history of the pml4 frame:\n");
        pmm_report_page_table_history_pub(pml4_phys);
        if (current_task) {
          console_write("  releasing task ");
          console_write_dec(current_task->id);
          console_write(" ");
          console_write(current_task->name);
          console_write("\n");
        }
      }
      u64 *pdpt = table_from_entry(entry);
      u64 pdpt_phys = entry & PAGE_ENTRY_ADDRESS_MASK;
      free_table(pdpt, 1);
      pmm_note_page_table(pdpt_phys, 0); /* no longer a table */
      pmm_free_frame(pdpt_phys);
      freed_tables_count++;
    }
  }

  // Free the PML4 itself
  pmm_note_page_table(pml4_phys, 0); /* no longer a table */
  pmm_free_frame(pml4_phys);
  freed_tables_count++;
}

static void swap_in_recursive(u64 *table, int level, u64 base_addr, u64 pml4_phys) {
  tlb_shootdown_poll(); /* see clone_table: drain shootdowns from this IRQs-off walk */
  if (level >= 3) {
    for (usize i = 0; i < 512; i++) {
      u64 entry = table[i];
      if (!(entry & VMM_PRESENT) && (entry & VMM_SWAPPED)) {
        u64 vaddr = base_addr + (i * PAGE_SIZE);
        u64 new_frame = 0;
        extern int swap_in(u32 slot, u64 *out_physical_frame);
        u32 swslot = (u32)((entry & PAGE_ENTRY_ADDRESS_MASK) >> 12);
        if (swap_in(swslot, &new_frame) == 0) {
          u64 flags = VMM_PRESENT | VMM_WRITABLE;
          if (entry & VMM_USER) flags |= VMM_USER;
          if (entry & VMM_NO_EXECUTE) flags |= VMM_NO_EXECUTE;
          table[i] = new_frame | flags;
          invalidate_page(vaddr);
          
          extern void eviction_register_page(struct task *task, u64 vaddr, u64 frame);
          eviction_register_page(current_task, vaddr, new_frame);
        }
      }
    }
    return;
  }

  for (usize i = 0; i < 512; i++) {
    if (level == 0 && i >= 256) break; // Only check user space (0-255)

    u64 entry = table[i];
    if ((entry & VMM_PRESENT) && !(entry & HUGE_PAGE_FLAG)) {
      u64 *child = table_from_entry(entry);
      u64 step = 1ULL << (12 + (3 - level) * 9);
      swap_in_recursive(child, level + 1, base_addr + i * step, pml4_phys);
    }
  }
}

void paging_swap_in_all_swapped(u64 pml4_phys) {
  if (pml4_phys == 0 || pml4_phys == kernel_pml4_phys) {
    return;
  }
  u64 *pml4 = (u64 *)(usize)(pml4_phys + DIRECT_MAP_BASE);
  swap_in_recursive(pml4, 0, 0, pml4_phys);
}

/* Is any physical frame in this address space reachable through two mappings
 * that should have nothing to do with each other?
 *
 * The compositor died twice with a wl_list link holding 0xff242424ff242424 —
 * two pixels of the terminal's background colour, sitting in a heap object
 * several megabytes away from any buffer. Pixels written by one process
 * appearing inside another's heap means one physical page is reachable from
 * both, and no amount of reading the allocator says which page or which
 * mapping. This finds it: every frame under a shared mapping goes into a set,
 * then every frame under a private one is looked up in it. A hit prints both
 * virtual addresses and both mappings, which names the two owners.
 *
 * Only from the crash path, and only when b1nix.frame-alias asks: it walks the
 * whole address space and allocates a table proportional to the shared part.
 */
#define VMM_ALIAS_MAX_SHARED (1u << 16) /* 65536 pages = 256 MiB of sharing */

void vmm_report_frame_aliases(struct task *t) {
  if (!t || !t->pml4_phys || !t->vma_list)
    return;

  usize shared_pages = 0;
  for (struct vm_area *v = t->vma_list; v; v = v->next)
    if (v->flags & MAP_SHARED)
      shared_pages += (usize)((v->end - v->start) / PAGE_SIZE);

  if (shared_pages == 0)
    return;
  if (shared_pages > VMM_ALIAS_MAX_SHARED) {
    console_write("frame-alias: too much shared mapping to scan (");
    console_write_dec(shared_pages);
    console_write(" pages)\n");
    return;
  }

  /* Open-addressed set, power of two, half full at worst. 0 means empty, so a
   * frame of 0 (which is never handed to userspace) needs no special case. */
  usize slots = 1;
  while (slots < shared_pages * 2)
    slots <<= 1;
  u64 *set = kzalloc(slots * sizeof(u64));
  u64 *addr = kzalloc(slots * sizeof(u64));
  if (!set || !addr) {
    if (set)
      kfree(set);
    if (addr)
      kfree(addr);
    console_write("frame-alias: no memory for the scan\n");
    return;
  }

  usize mask = slots - 1;
  for (struct vm_area *v = t->vma_list; v; v = v->next) {
    if (!(v->flags & MAP_SHARED))
      continue;
    for (u64 va = v->start; va < v->end; va += PAGE_SIZE) {
      u64 f = paging_user_frame(t->pml4_phys, va);
      if (!f)
        continue;
      usize i = (usize)((f >> 12) * 0x9E3779B97F4A7C15ull >> 40) & mask;
      while (set[i] && set[i] != f)
        i = (i + 1) & mask;
      set[i] = f;
      addr[i] = va;
    }
  }

  unsigned found = 0;
  for (struct vm_area *v = t->vma_list; v && found < 8; v = v->next) {
    if (v->flags & MAP_SHARED)
      continue;
    for (u64 va = v->start; va < v->end && found < 8; va += PAGE_SIZE) {
      u64 f = paging_user_frame(t->pml4_phys, va);
      if (!f)
        continue;
      usize i = (usize)((f >> 12) * 0x9E3779B97F4A7C15ull >> 40) & mask;
      while (set[i] && set[i] != f)
        i = (i + 1) & mask;
      if (set[i] != f)
        continue;
      found++;
      console_write("frame-alias: frame 0x");
      console_write_hex64(f);
      console_write(" is mapped BOTH at 0x");
      console_write_hex64(va);
      console_write(" (private, ");
      console_write(v->node && v->node->name[0] ? v->node->name : "<anonymous>");
      console_write(") and at 0x");
      console_write_hex64(addr[i]);
      console_write(" (shared)\n");
    }
  }
  if (!found)
    console_write("frame-alias: no frame in this address space is reachable "
                  "from two of its own mappings\n");

  /* And the same question across processes.
   *
   * The pixels that landed in the compositor's heap were written by the
   * terminal, in another address space entirely — so a frame reachable twice
   * within one process was never going to find it. This rebuilds the set from
   * the faulting task's PRIVATE pages and asks every other task whether it
   * maps any of them. A private anonymous page of one process is nobody
   * else's business; a hit is the allocator handing one frame to two owners,
   * which is exactly the fault being hunted. */
  for (usize i = 0; i < slots; i++) {
    set[i] = 0;
    addr[i] = 0;
  }
  /* Anonymous only. A private mapping of a FILE legitimately shares the page
   * cache's frames with every other reader until someone writes — the first
   * version of this scan reported nine hundred such pages of libcairo and said
   * nothing at all. Anonymous memory has no such excuse: it belongs to one
   * address space, and a second mapper of it is the bug. */
  usize private_pages = 0;
  for (struct vm_area *v = t->vma_list; v; v = v->next)
    if (!(v->flags & MAP_SHARED) && !v->node)
      private_pages += (usize)((v->end - v->start) / PAGE_SIZE);

  if (private_pages == 0 || private_pages > VMM_ALIAS_MAX_SHARED) {
    console_write("frame-alias: private set too large to scan (");
    console_write_dec(private_pages);
    console_write(" pages)\n");
    kfree(set);
    kfree(addr);
    return;
  }
  if (private_pages * 2 > slots) {
    kfree(set);
    kfree(addr);
    slots = 1;
    while (slots < private_pages * 2)
      slots <<= 1;
    set = kzalloc(slots * sizeof(u64));
    addr = kzalloc(slots * sizeof(u64));
    if (!set || !addr) {
      if (set)
        kfree(set);
      if (addr)
        kfree(addr);
      console_write("frame-alias: no memory for the cross-process scan\n");
      return;
    }
    mask = slots - 1;
  }

  for (struct vm_area *v = t->vma_list; v; v = v->next) {
    if ((v->flags & MAP_SHARED) || v->node)
      continue;
    for (u64 va = v->start; va < v->end; va += PAGE_SIZE) {
      u64 f = paging_user_frame(t->pml4_phys, va);
      if (!f || f == pmm_zero_page())
        continue;
      usize i = (usize)((f >> 12) * 0x9E3779B97F4A7C15ull >> 40) & mask;
      while (set[i] && set[i] != f)
        i = (i + 1) & mask;
      set[i] = f;
      addr[i] = va;
    }
  }

  unsigned cross = 0;
  for (usize ti = 0; ti < scheduler_task_slots() && cross < 8; ti++) {
    struct task *o = scheduler_task_slot(ti);

    if (!o || o == t || !o->pml4_phys || !o->vma_list)
      continue;
    /* Threads of the same process share the tables — sharing there is the
     * point, not a fault. */
    if (o->pml4_phys == t->pml4_phys)
      continue;
    for (struct vm_area *v = o->vma_list; v && cross < 8; v = v->next) {
      for (u64 va = v->start; va < v->end && cross < 8; va += PAGE_SIZE) {
        u64 f = paging_user_frame(o->pml4_phys, va);

        if (!f || f == pmm_zero_page())
          continue;
        usize i = (usize)((f >> 12) * 0x9E3779B97F4A7C15ull >> 40) & mask;
        while (set[i] && set[i] != f)
          i = (i + 1) & mask;
        if (set[i] != f)
          continue;
        cross++;
        console_write("frame-alias: frame 0x");
        console_write_hex64(f);
        console_write(" is PRIVATE here at 0x");
        console_write_hex64(addr[i]);
        console_write(" and also mapped by pid ");
        console_write_dec(o->id);
        console_write(" (");
        console_write(o->name);
        console_write(") at 0x");
        console_write_hex64(va);
        console_write(" in ");
        console_write(v->node && v->node->name[0] ? v->node->name
                                                  : "<anonymous>");
        console_write("\n");
      }
    }
  }
  if (!cross)
    console_write("frame-alias: no private frame of this task is mapped by "
                  "another process\n");

  kfree(set);
  kfree(addr);
}
