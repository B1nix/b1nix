#include <b1nix/vfs.h>
#include <b1nix/page_cache.h>
#include <b1nix/rwlock.h>
#include <b1nix/lockdep.h>
#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/sched.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/user.h>
#include <b1nix/module.h>
#include <string.h>

extern u8 __kernel_start[];
extern u8 __kernel_end[];

/* AArch64 VMSAv8-64, 4KB granule, 4-level (L0..L3) translation through
 * TTBR0_EL1 only (TCR_EL1.T0SZ=16 -> 48-bit VA, see boot.S; TTBR1_EL1 is left
 * pointing at the same table boot.S built and is never consulted again,
 * since every VA this kernel hands out has bit 47 clear).
 *
 * Address-space split, chosen so every process's table can share the kernel
 * half by pointer instead of deep-copying it (same trick x86_64 uses for
 * PML4[256:511]):
 *   L0[0]      kernel half: RAM identity map (0x40000000+, mirrors
 *              boot_l1[1..3]) + GIC/UART device blocks (mirrors boot_l1[0],
 *              split to 2MB granularity at the exact device offsets so the
 *              rest of that 1GB is free). EL1-only. Built once by vmm_init,
 *              shared BY POINTER across every process's L0 table.
 *   L0[1..511] user half: private per process, built lazily by vmm_map_page
 *              as the ELF loader/mmap map segments in. This is where
 *              PIE_LOAD_BASE / USER_LDSO_LOAD_BASE / USER_STACK_TOP land
 *              (kernel/user/process.c, kernel/include/b1nix/user.h) — same
 *              numeric addresses as x86_64, which fits because T0SZ=16 gives
 *              the same 48-bit VA budget x86_64 has.
 *
 * paging_clone_address_space is copy-on-write: a private writable page is
 * downgraded to read-only + SW_COW in both parent and child and the frame
 * refcounted, and the first write from either side is resolved by cow_fault.
 * Swapped-out pages are recorded as a non-present SW leaf carrying the swap
 * slot, and faulted back in by swap_in_fault.
 *
 * ponytail: still no file-backed demand paging here — the fault handler runs
 * from the exception vectors with IRQs masked, so it cannot do the blocking
 * read x86_64's handler does. Every case implemented above is allocation and
 * memcpy only. See docs/aarch64-parity.md.
 */

#define IDX_MASK 0x1ffULL
#define ADDR_MASK 0x0000fffffffff000ULL

#define D_TABLE 0x3ULL /* table descriptor, L0-L2 */
#define D_PAGE  0x3ULL /* page descriptor, L3 (leaf) */
#define D_BLOCK 0x1ULL /* block descriptor, L1/L2 (leaf) */

#define D_AF  (1ULL << 10)
#define D_PXN (1ULL << 53)
#define D_UXN (1ULL << 54)
/* Software-only bits (ARM ignores 58:55 in hardware translation) — used to
 * remember generic VMM_* state that has no AArch64 PTE bit of its own. */
#define SW_COW    (1ULL << 55)
#define SW_SHARED (1ULL << 56)
#define SW_USER   (1ULL << 57)

#define AP_EL1_RW     (0ULL << 6)
#define AP_EL1_EL0_RW (1ULL << 6)
#define AP_EL1_RO     (2ULL << 6)
#define AP_EL1_EL0_RO (3ULL << 6)
#define SH_INNER (3ULL << 8)

/* MAIR_EL1 indices set up in boot.S: 0 = Device-nGnRE, 1 = Normal WBWA,
 * 2 = Normal Non-cacheable (this arch's write-combining). */
#define ATTR_DEVICE (0ULL << 2)
#define ATTR_NORMAL (1ULL << 2)
#define ATTR_NORMAL_NC (2ULL << 2)
#define ATTR_INDX_MASK (7ULL << 2)

/* Kernel RAM/device blocks are identity-mapped from boot (boot.S, then
 * aarch64_boot_map_rebuild below) — this "direct map" covers every RAM bank
 * the device tree reports plus the low 32-bit register space, so unlike
 * x86_64 there is no separate bring-up phase before it's usable. */
int direct_map_ready = 1;

static u64 *kernel_l0_virt;
/* Page-table lock. Mirrors kernel/arch/x86_64/paging.c: writer for every
 * mutation, reader for the walks that only report. It was genuinely optional
 * while this port ran one CPU; the moment a secondary runs a process, two CPUs
 * fault at once and `ensure_child` races itself — one of the two installs a
 * table the other has already replaced, and the loser's page comes back as a
 * translation fault in userspace. That is what killed init on the first
 * two-CPU boot.
 *
 * DAG position: HEAP (700) -> VMM (750) -> PMM (800). kheap_grow holds the
 * heap lock and calls vmm_map_page, which then takes a frame from the pmm;
 * nothing takes them the other way round. No path holds this across blocking
 * work — the swap-in case below drops it around the device read for exactly
 * that reason. */
static rwlock_t vmm_lock = RWLOCK_INIT;

static inline void vmm_write_acquire(u64 *flags) {
  rw_write_lock_irqsave(&vmm_lock, flags);
  LOCKDEP_ACQUIRE(LOCKDEP_LVL_VMM);
}
static inline void vmm_write_release(u64 flags) {
  LOCKDEP_RELEASE(LOCKDEP_LVL_VMM);
  rw_write_unlock_irqrestore(&vmm_lock, flags);
}

static u64 kernel_l0_phys;

static usize l0_index(u64 va) { return (va >> 39) & IDX_MASK; }
static usize l1_index(u64 va) { return (va >> 30) & IDX_MASK; }
static usize l2_index(u64 va) { return (va >> 21) & IDX_MASK; }
static usize l3_index(u64 va) { return (va >> 12) & IDX_MASK; }

/* Kernel range is identity-mapped (VA == PA) and every page-table frame this
 * file allocates is touched through that same identity map — no separate
 * "direct map window" bookkeeping needed. */
static inline u64 *phys_to_virt(u64 phys) { return (u64 *)(usize)phys; }
static inline u64 virt_to_phys(void *ptr) { return (u64)(usize)ptr; }

static u64 *get_l0_for_va(u64 va) {
  if (current_task && current_task->pml4_phys && va >= 0x0000000002000000ULL) {
    return phys_to_virt(current_task->pml4_phys);
  }
  return kernel_l0_virt;
}

static u64 *get_current_l0(void) {
  if (current_task && current_task->pml4_phys) {
    return phys_to_virt(current_task->pml4_phys);
  }
  return kernel_l0_virt;
}

static void tlb_flush_all(void) {
  __asm__ volatile("dsb ishst\n\t"
                    "tlbi vmalle1is\n\t"
                    "dsb ish\n\t"
                    "isb" ::: "memory");
}

/* Invalidate a single page rather than the whole TLB. `tlbi vaae1is` takes the
 * VA scaled to pages and applies to every ASID, which is what this port wants:
 * it runs on one ASID and shares the kernel half by pointer, so an
 * unqualified-by-ASID invalidate is correct — VMALLE1IS is simply a far bigger
 * hammer. Every mapping change used to flush everything, which on a fault-heavy
 * workload (demand paging, copy-on-write) discards the entire translation
 * working set on each individual page. */
static void tlb_flush_page(u64 va) {
  __asm__ volatile("dsb ishst\n\t"
                    "tlbi vaae1is, %0\n\t"
                    "dsb ish\n\t"
                    "isb" ::"r"(va >> 12)
                    : "memory");
}

static u64 *alloc_table(void) {
  u64 frame = pmm_alloc_frame();
  if (!frame) {
    panic("aarch64 vmm: OOM allocating page table");
  }
  u64 *table = phys_to_virt(frame);
  memset(table, 0, PAGE_SIZE);
  return table;
}

static u64 *table_from_entry(u64 entry) {
  return phys_to_virt(entry & ADDR_MASK);
}

/* Walk to the L3 (leaf) table covering `vaddr` in `space` without allocating
 * anything, or NULL if any level is absent or is a block. `space` of 0 means
 * the kernel's own table. */
static u64 *leaf_table_for(u64 space, u64 vaddr) {
  /* "NULL if any level is absent" includes the root: `space` can name an
   * address space that is being torn down, and kernel_l0_virt is null until
   * paging_init builds it. Reading through one is a data abort, not an answer. */
  u64 *l0 = space ? phys_to_virt(space) : kernel_l0_virt;
  if (!l0) return 0;
  if ((l0[l0_index(vaddr)] & 0x3ULL) != D_TABLE) return 0;
  u64 *l1 = table_from_entry(l0[l0_index(vaddr)]);
  if (!l1) return 0;
  if ((l1[l1_index(vaddr)] & 0x3ULL) != D_TABLE) return 0;
  u64 *l2 = table_from_entry(l1[l1_index(vaddr)]);
  if (!l2) return 0;
  if ((l2[l2_index(vaddr)] & 0x3ULL) != D_TABLE) return 0;
  return table_from_entry(l2[l2_index(vaddr)]);
}

#define BLOCK_SIZE_L1 0x40000000ULL /* 1GB, split into 512 L2 2MB blocks */
#define BLOCK_SIZE_L2 0x200000ULL   /* 2MB, split into 512 L3 4KB pages */

/* Split an existing block descriptor into a freshly allocated child table
 * covering the identical physical range with equivalent leaf descriptors —
 * same attrs, finer granularity. Used when a caller (e.g. kheap growth)
 * needs to remap a single page inside build_kernel_half's static 1GB RAM
 * blocks: splitting preserves every other sub-region's mapping instead of
 * clobbering the whole block. */
static u64 *split_block(u64 old_entry, u64 child_size, int child_is_page) {
  u64 *child = alloc_table();
  u64 base = old_entry & ADDR_MASK;
  u64 attrs = old_entry & ~ADDR_MASK & ~0x3ULL;
  u64 leaf_bits = child_is_page ? D_PAGE : D_BLOCK;
  for (usize i = 0; i < 512; i++) {
    child[i] = (base + i * child_size) | attrs | leaf_bits;
  }
  return child;
}

/* Walk to (allocating or splitting as needed) the child table one level
 * below `parent`. `parent_level` is the level of `parent` itself (1=L1,
 * 2=L2, 0=L0 — L0 entries are never blocks, only tables). */
static u64 *ensure_child(u64 *parent, usize index, int parent_level) {
  u64 entry = parent[index];
  if ((entry & 0x3ULL) == D_TABLE) {
    return table_from_entry(entry);
  }
  if ((entry & 0x3ULL) == D_BLOCK) {
    u64 child_size = (parent_level == 1) ? BLOCK_SIZE_L2 : PAGE_SIZE;
    int child_is_page = (parent_level == 2);
    u64 *child = split_block(entry, child_size, child_is_page);
    /* NOTE: deliberately NOT break-before-make. The architecture wants the old
     * BLOCK invalidated before the TABLE that replaces it is installed, but the
     * very first split this kernel performs is of the 1GB block holding its own
     * running code and stack (kheap growth at 0x48000000), so zeroing the entry
     * would unmap the instruction being executed. vmm_map_page's trailing
     * tlb_flush_all() invalidates the stale block entry before the new mapping
     * is used. Splitting at page granularity in build_kernel_half would let
     * this become a proper break-before-make. */
    parent[index] = virt_to_phys(child) | D_TABLE;
    return child;
  }
  u64 *child = alloc_table();
  parent[index] = virt_to_phys(child) | D_TABLE;
  return child;
}

/* Encode a present leaf (L3 page) descriptor from the generic VMM_* flags. */
static u64 encode_leaf(u64 phys, u64 flags) {
  int user = (flags & VMM_USER) != 0;
  int writable = (flags & VMM_WRITABLE) != 0;
  u64 ap = user ? (writable ? AP_EL1_EL0_RW : AP_EL1_EL0_RO)
                : (writable ? AP_EL1_RW : AP_EL1_RO);
  /* EL1 never executes user-writable memory; EL0 never reaches kernel-only
   * pages anyway (AP blocks it) but UXN is set there too for hygiene. */
  u64 pxn = user ? D_PXN : 0;
  u64 uxn = (!user || (flags & VMM_NO_EXECUTE)) ? D_UXN : 0;
  u64 sw = (flags & VMM_COW ? SW_COW : 0) |
           (flags & VMM_SHARED ? SW_SHARED : 0) |
           (user ? SW_USER : 0);
  /* Memory type. Only write-combining is translated: VMM_WC (x86 spells it
   * PAT|PWT) means Normal Non-cacheable here, MAIR slot 2. VMM_PCD is
   * deliberately NOT mapped to Device — the boot map already covers every MMIO
   * window with the right type, and turning existing PCD mappings into Device
   * would change the memory type under live callers for no gain. Normal-NC
   * carries no shareability of its own, so SH is left as-is. */
  u64 attr = ATTR_NORMAL;
  if ((flags & VMM_WC) == VMM_WC)
    attr = ATTR_NORMAL_NC; /* write-combining: MAIR slot 2, Normal-NC */
  else if ((flags & (VMM_PCD | VMM_PWT)) == (VMM_PCD | VMM_PWT))
    attr = ATTR_DEVICE; /* x86's UC (PCD|PWT): Device-nGnRE, MAIR slot 0 */
  return (phys & ADDR_MASK) | attr | ap | SH_INNER | D_AF | pxn | uxn |
         sw | D_PAGE;
}

/* ── The identity map, from the board's own memory map ───────────────────────
 *
 * Both maps this file builds — the boot one in boot.S's static tables and the
 * kernel half of every process's L0 — describe the same thing: physical
 * memory, addressed by its own address. What differs per board is which parts
 * of it are RAM and which are registers, and that is not something a constant
 * can know. QEMU virt has its RAM at 0x40000000 and its GIC and UART below it;
 * a Raspberry Pi 4 has RAM from 0 (this image included) and its peripherals at
 * 0xfc000000. The rule below is the whole of it:
 *
 *   a 1 GiB block entirely inside a RAM bank -> one Normal-WB block
 *   a 1 GiB block with no RAM in it at all   -> one Device-nGnRE block
 *   anything in between                      -> 512 2 MiB blocks, same rule
 *
 * with one exception: the first 2 MiB is left unmapped unless it really is
 * RAM, so that a null pointer dereference in the kernel still faults instead
 * of quietly reading whatever lives at physical 0.
 *
 * Registers therefore end up Device without anyone listing them: the GIC, the
 * UART, virtio-mmio, the PCIe window and its ECAM are all simply "not RAM". */

static int range_is_ram(u64 base, u64 len) {
  u64 k_start = (u64)(usize)__kernel_start;
  u64 k_end = (u64)(usize)__kernel_end;
  if (base >= k_start && base + len <= k_end) return 1;

  const struct boot_info *bi = bootinfo_get();

  for (usize i = 0; i < bi->memory_region_count; i++) {
    const struct boot_memory_region *r = &bi->memory_regions[i];

    if (r->type != BOOT_MEMORY_AVAILABLE) continue;
    if (base >= r->base && base + len <= r->base + r->length) return 1;
  }
  return 0;
}

static int range_has_ram(u64 base, u64 len) {
  u64 k_start = (u64)(usize)__kernel_start;
  u64 k_end = (u64)(usize)__kernel_end;
  if (base < k_end && k_start < base + len) return 1;

#if defined(__aarch64__)
  if (base < MODULE_REGION_BASE + MODULE_REGION_SIZE && MODULE_REGION_BASE < base + len) return 1;
#endif

  const struct boot_info *bi = bootinfo_get();

  for (usize i = 0; i < bi->memory_region_count; i++) {
    const struct boot_memory_region *r = &bi->memory_regions[i];

    if (r->type != BOOT_MEMORY_AVAILABLE) continue;
    if (base < r->base + r->length && r->base < base + len) return 1;
  }
  return 0;
}

#define DIRECT_LEAF_FLAGS (AP_EL1_RW | D_AF | D_UXN)

/* Fill `l1` (an L1 table, one entry per GiB) so that [0, limit) is mapped by
 * the rule above. `alloc_l2` supplies the tables the mixed blocks need, and
 * `l2_phys` turns one of those back into the physical address the descriptor
 * has to carry — the boot map's tables are static and identity-addressed, the
 * kernel's come from the pmm. */
static void build_direct_map(u64 *l1, u64 limit, u64 *(*alloc_l2)(void),
                             u64 (*l2_phys)(u64 *)) {
  usize blocks = (usize)((limit + BLOCK_SIZE_L1 - 1) / BLOCK_SIZE_L1);

  if (blocks > 512) blocks = 512;
  for (usize i = 0; i < blocks; i++) {
    u64 base = (u64)i * BLOCK_SIZE_L1;

    if (range_is_ram(base, BLOCK_SIZE_L1)) {
      l1[i] = base | ATTR_NORMAL | DIRECT_LEAF_FLAGS | D_BLOCK;
      continue;
    }
    if (!range_has_ram(base, BLOCK_SIZE_L1)) {
      l1[i] = base | ATTR_DEVICE | DIRECT_LEAF_FLAGS | D_BLOCK;
      continue;
    }

    /* Mixed: RAM and registers inside the same GiB, which is what a board with
     * its peripherals just above its last RAM bank looks like. */
    u64 *l2 = alloc_l2();

    if (!l2) {
      /* Only the boot map's static pool can run out. Keep the RAM half
       * working — a bank the pmm will allocate from matters more than the
       * memory type of the registers that share this gigabyte. */
      l1[i] = base | ATTR_NORMAL | DIRECT_LEAF_FLAGS | D_BLOCK;
      continue;
    }
    for (usize j = 0; j < 512; j++) {
      u64 a = base + (u64)j * BLOCK_SIZE_L2;
      u64 attr = range_has_ram(a, BLOCK_SIZE_L2) ? ATTR_NORMAL : ATTR_DEVICE;

      l2[j] = a | attr | DIRECT_LEAF_FLAGS | D_BLOCK;
    }
    l1[i] = l2_phys(l2) | D_TABLE;
  }

  /* Physical 0, when it is not RAM: leave it unmapped so a null dereference is
   * still a fault. */
  if (!range_is_ram(0, BLOCK_SIZE_L2) && (l1[0] & 0x3ULL) == D_TABLE) {
    u64 *l2 = table_from_entry(l1[0]);

    l2[0] = 0;
  } else if (!range_has_ram(0, BLOCK_SIZE_L1)) {
    /* The whole first GiB is registers (QEMU virt): split it so the first
     * 2 MiB can stay invalid while the rest keeps its Device mapping. */
    u64 *l2 = alloc_l2();

    if (!l2)
      return; /* out of static tables: the 1 GiB Device block above stands */
    for (usize j = 1; j < 512; j++) {
      u64 a = (u64)j * BLOCK_SIZE_L2;

      l2[j] = a | ATTR_DEVICE | DIRECT_LEAF_FLAGS | D_BLOCK;
    }
    l2[0] = 0;
    l1[0] = l2_phys(l2) | D_TABLE;
  }
}

/* The boot map's L2 tables. Two are enough for every layout seen so far (the
 * unmapped first 2 MiB, and one GiB shared between RAM and registers); a board
 * needing more keeps the coarser 1 GiB mapping for the rest, which is what it
 * had before this existed. */
#define BOOT_L2_POOL 8
static u64 boot_l2_pool[BOOT_L2_POOL][512] __attribute__((aligned(PAGE_SIZE)));
static usize boot_l2_used;

static u64 *boot_l2_alloc(void) {
  if (boot_l2_used >= BOOT_L2_POOL) return 0;
  return boot_l2_pool[boot_l2_used++];
}

/* The boot tables are reached with the MMU on but before any translation this
 * file set up, so a table's virtual address is its physical one. */
static u64 boot_l2_phys(u64 *table) { return (u64)(usize)table; }

/* Rewrite boot.S's identity map now that the device tree has said where RAM
 * is. Runs before the console, the pmm and the heap: boot.S maps only the
 * blocks holding this image and the device tree as Normal, everything else as
 * Device, which is enough to walk the tree and no more. */
void aarch64_boot_map_rebuild(void) {
  extern u64 boot_l1[512];

  boot_l2_used = 0;
  build_direct_map(boot_l1, 512ULL * BLOCK_SIZE_L1, boot_l2_alloc, boot_l2_phys);
  tlb_flush_all();
}

/* One 2 MiB block of the map may need to be missing rather than Device — see
 * build_direct_map. Called with the kernel's own allocator. */
static u64 kernel_l2_phys(u64 *table) { return virt_to_phys(table); }

static void build_kernel_half(u64 *l0) {
  u64 *l1 = alloc_table();
  l0[0] = virt_to_phys(l1) | D_TABLE;

  /* Map the entire 0-512 GiB L1 table: RAM is Normal memory, everything else is Device memory
   * so high MMIO regions (like ECAM at 256 GiB) are reachable. */
  build_direct_map(l1, 512ULL * BLOCK_SIZE_L1, alloc_table, kernel_l2_phys);
}

void vmm_init(void) {
  kernel_l0_virt = alloc_table();
  kernel_l0_phys = virt_to_phys(kernel_l0_virt);
  build_kernel_half(kernel_l0_virt);

  __asm__ volatile("msr ttbr0_el1, %0\n\t"
                   "msr ttbr1_el1, %0\n\t"
                   "isb" ::"r"(kernel_l0_phys)
                   : "memory");
  tlb_flush_all();

  console_write("aarch64: vmm_init (real per-process 4-level paging active)\n");
}

static void map_page_locked(u64 virtual_address, u64 physical_address,
                            u64 flags) {
  u64 *l0 = get_l0_for_va(virtual_address);
  u64 *l1 = ensure_child(l0, l0_index(virtual_address), 0);
  u64 *l2 = ensure_child(l1, l1_index(virtual_address), 1);
  u64 *l3 = ensure_child(l2, l2_index(virtual_address), 2);
  l3[l3_index(virtual_address)] = encode_leaf(physical_address, flags | VMM_PRESENT);
  tlb_flush_page(virtual_address);
}

void vmm_map_page(u64 virtual_address, u64 physical_address, u64 flags) {
  u64 f;

  vmm_write_acquire(&f);
  map_page_locked(virtual_address, physical_address, flags);
  vmm_write_release(f);
}

void vmm_unmap_page(u64 virtual_address) {
  paging_unmap_page_from_space(current_task ? current_task->pml4_phys : 0,
                                virtual_address);
}

static void unmap_from_space_locked(u64 pml4_phys, u64 virtual_address) {
  u64 *l0 = (virtual_address >= 0x8000000000000000ULL || !pml4_phys)
                ? kernel_l0_virt
                : phys_to_virt(pml4_phys);
  usize i0 = l0_index(virtual_address);
  if ((l0[i0] & 0x3ULL) != D_TABLE) return;
  u64 *l1 = table_from_entry(l0[i0]);
  usize i1 = l1_index(virtual_address);
  if ((l1[i1] & 0x3ULL) != D_TABLE) return;
  u64 *l2 = table_from_entry(l1[i1]);
  usize i2 = l2_index(virtual_address);
  if ((l2[i2] & 0x3ULL) != D_TABLE) return;
  u64 *l3 = table_from_entry(l2[i2]);
  usize i3 = l3_index(virtual_address);
  u64 entry = l3[i3];
  if ((entry & 0x3ULL) == D_PAGE) {
    u64 frame = entry & ADDR_MASK;
    if (frame && (entry & SW_USER)) {
      pmm_free_frame(frame);
    }
  }
  l3[i3] = 0;
  tlb_flush_page(virtual_address);
}

void paging_unmap_page_from_space(u64 pml4_phys, u64 virtual_address) {
  u64 f;

  vmm_write_acquire(&f);
  unmap_from_space_locked(pml4_phys, virtual_address);
  vmm_write_release(f);
}

usize vmm_unmap_range_nosync(u64 base, usize npages, u64 *frames_out) {
  u64 _vmflags;
  u64 pml4_phys = current_task ? current_task->pml4_phys : 0;

  vmm_write_acquire(&_vmflags);
  for (usize i = 0; i < npages; i++) {
    u64 va = base + i * PAGE_SIZE;
    u64 *l0 = (va >= 0x8000000000000000ULL || !pml4_phys)
                  ? kernel_l0_virt
                  : phys_to_virt(pml4_phys);
    usize i0 = l0_index(va);
    if ((l0[i0] & 0x3ULL) != D_TABLE) {
      frames_out[i] = 0;
      continue;
    }
    u64 *l1 = table_from_entry(l0[i0]);
    usize i1 = l1_index(va);
    if ((l1[i1] & 0x3ULL) != D_TABLE) {
      frames_out[i] = 0;
      continue;
    }
    u64 *l2 = table_from_entry(l1[i1]);
    usize i2 = l2_index(va);
    if ((l2[i2] & 0x3ULL) != D_TABLE) {
      frames_out[i] = 0;
      continue;
    }
    u64 *l3 = table_from_entry(l2[i2]);
    usize i3 = l3_index(va);
    u64 entry = l3[i3];
    if ((entry & 0x3ULL) == D_PAGE) {
      frames_out[i] = entry & ADDR_MASK;
    } else {
      frames_out[i] = 0;
    }
    l3[i3] = 0;
  }
  vmm_write_release(_vmflags);
  return npages;
}

void vmm_remap_page(u64 virtual_address, u64 physical_address, u64 flags) {
  u64 f;

  /* One critical section, not two: a fault taken between the unmap and the
   * map would otherwise see a hole that is about to be filled. */
  vmm_write_acquire(&f);
  unmap_from_space_locked(current_task ? current_task->pml4_phys : 0,
                          virtual_address);
  map_page_locked(virtual_address, physical_address, flags);
  vmm_write_release(f);
}

u64 vmm_direct_map_base(void) { return 0; }

static void set_lazy_locked(u64 virtual_address) {
  u64 *l0 = get_l0_for_va(virtual_address);
  u64 *l1 = ensure_child(l0, l0_index(virtual_address), 0);
  u64 *l2 = ensure_child(l1, l1_index(virtual_address), 1);
  u64 *l3 = ensure_child(l2, l2_index(virtual_address), 2);
  /* bit0 (valid) stays clear — hardware ignores the rest, so this is a pure
   * software marker consulted by vmm_handle_page_fault. */
  l3[l3_index(virtual_address)] = VMM_LAZY | VMM_USER | VMM_WRITABLE;
}

void vmm_set_lazy(u64 virtual_address) {
  u64 f;

  vmm_write_acquire(&f);
  set_lazy_locked(virtual_address);
  vmm_write_release(f);
}

/* Anonymous demand paging for the user heap/mmap region, matching x86_64's
 * vmm_handle_page_fault: brk() and mmap(MAP_ANONYMOUS) only record a VMA and
 * grow the break — the pages themselves are materialised here on first touch.
 * Without this every malloc'd page past the initially-mapped ones faulted, and
 * the fault was fatal (musl-linked ash died dereferencing its own globals). */
static int fault_anon_user_page(u64 va) {
  if (!current_task)
    return -1;
  /* Only inside a mapping the task actually owns, and never a PROT_NONE
   * reservation. The VMA list is sorted by start. */
  struct vm_area *hit = 0;
  for (struct vm_area *v = current_task->vma_list; v && v->start <= va;
       v = v->next) {
    if (va < v->end) {
      if (v->prot == PROT_NONE) {
        console_write("pf-protnone: va=0x");
        console_write_hex64(va);
        console_write(" vma=0x");
        console_write_hex64(v->start);
        console_write("-0x");
        console_write_hex64(v->end);
        console_write("\n");
        return -1;
      }
      hit = v;
      break;
    }
  }
  /* Fallbacks for the two regions a task legitimately touches without a VMA
   * describing the exact page: its brk heap and the stack growth window. A
   * blanket "anything above the load base" rule was materialising a fresh page
   * for ANY wild pointer, so a userspace null-ish/garbage dereference silently
   * succeeded instead of raising SIGSEGV (M35-CORE's deliberate crash never
   * crashed). */
  if (!hit) {
    u64 brk_end = (current_task->user_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    int in_brk = current_task->heap_start && va >= current_task->heap_start &&
                 va < brk_end;
    int in_stack = va < USER_STACK_TOP &&
                   va >= USER_STACK_TOP - USER_STACK_MAX_SIZE;
    if (!in_brk && !in_stack)
      return -1;
  }

  u64 frame = pmm_alloc_frame();
  if (!frame)
    return -1;
  memset(phys_to_virt(frame), 0, PAGE_SIZE);

  u64 flags = VMM_PRESENT | VMM_USER;
  /* Honour the mapping's protection instead of always mapping writable. */
  if (!hit || (hit->prot & PROT_WRITE))
    flags |= VMM_WRITABLE;
  map_page_locked(va, frame, flags); /* invalidates the page itself */
  return 0;
}

/* Resolve a copy-on-write fault on a present, read-only leaf carrying SW_COW.
 * Restores the writable AP encoding, copying the frame first unless this task
 * turns out to be its last owner. Allocation and memcpy only — no blocking
 * work, which is what makes this safe to run straight from the exception
 * vectors (they enter with IRQs masked). */
static int cow_fault(u64 *l3, usize i3, u64 va) {
  u64 entry = l3[i3];
  u64 old_frame = entry & ADDR_MASK;
  u64 writable_ap = (entry & SW_USER) ? AP_EL1_EL0_RW : AP_EL1_RW;
  u64 base = (entry & ~ADDR_MASK & ~AP_EL1_EL0_RO & ~SW_COW) | writable_ap;

  if (pmm_get_refcount(old_frame) <= 1) {
    /* Sole owner — nothing to copy, just take the write permission back. */
    l3[i3] = old_frame | base;
    tlb_flush_page(va);
    return 0;
  }

  u64 new_frame = pmm_alloc_frame();
  if (!new_frame)
    return -1;
  memcpy(phys_to_virt(new_frame), phys_to_virt(old_frame), PAGE_SIZE);
  l3[i3] = (new_frame & ADDR_MASK) | base;
  pmm_free_frame(old_frame); /* drops this mapping's reference */
  tlb_flush_page(va);
  return 0;
}

/* Fault on a page whose contents were pushed to swap. The slot index lives in
 * the address field of the (non-present) leaf, exactly as paging_mark_swapped
 * put it there, so there is no reverse-map scan to do. */
static int swap_in_fault(u64 *l3, usize i3, u64 va, u64 entry) {
  u64 frame = 0;
  u32 slot = (u32)((entry & ADDR_MASK) >> 12);
  u64 f;

  /* Called with the page-table lock NOT held: swap_in is a block read, and
   * this kernel's block reads sleep. The leaf is re-checked under the lock
   * afterwards — another CPU may have faulted the same page in meanwhile. */
  if (swap_in(slot, &frame) < 0)
    return -1;

  u64 flags = VMM_PRESENT | VMM_WRITABLE;
  if (entry & SW_USER) flags |= VMM_USER;

  vmm_write_acquire(&f);
  if ((l3[i3] & 0x3ULL) == D_PAGE) {
    /* Someone else won the race and the page is already present; drop the
     * copy this read produced rather than leaking the frame. */
    vmm_write_release(f);
    pmm_free_frame(frame);
    return 0;
  }
  l3[i3] = encode_leaf(frame, flags);
  tlb_flush_page(va);
  vmm_write_release(f);
  return 0;
}

/* The VMA covering `va`, or 0. The list is sorted by start. */
static struct vm_area *vma_for(u64 va) {
  if (!current_task)
    return 0;
  for (struct vm_area *v = current_task->vma_list; v && v->start <= va;
       v = v->next) {
    if (va < v->end)
      return v;
  }
  return 0;
}

/* Is the lazy page at `va` backed by a file rather than by nothing? */
static int lazy_is_file_backed(u64 va) {
  struct vm_area *v = vma_for(va);
  struct vfs_inode *in = (v && v->node) ? v->node->inode : 0;

  return in && (in->type == VFS_FILE || in->read_cb || in->data);
}

/* Fill a file-backed lazy page and install it. Runs with the page-table lock
 * NOT held: page_cache_get_page and read_cb both do block I/O.
 *
 * This is what "the fault handler cannot read a file" used to mean on this
 * arch: a lazily mapped file page was materialised as zeroes, so ELF segments
 * had to be loaded eagerly and every mmap of a file read back empty. The
 * semantics here are x86_64's (kernel/arch/x86_64/paging.c): one page-cache
 * frame shared by every mapper, MAP_SHARED mappings marked shared so fork does
 * not copy them, and a writable MAP_PRIVATE mapping of a cached frame mapped
 * read-only + COW so the first store copies the page out of the cache instead
 * of rewriting the file for everyone else. */
static int file_fill_fault(u64 *l3, usize i3, u64 va, u64 lazy_entry) {
  struct vm_area *vma = vma_for(va);
  struct vfs_inode *in = (vma && vma->node) ? vma->node->inode : 0;
  int vma_shared = vma && (vma->flags & MAP_SHARED);
  int cache_frame = 0;
  u64 f;

  if (!in)
    return -1;

  u64 frame = pmm_alloc_frame();
  if (!frame)
    return -1;
  memset(phys_to_virt(frame), 0, PAGE_SIZE);

  u64 file_offset = (u64)vma->offset + (va - vma->start);
  u64 file_page = file_offset & ~(PAGE_SIZE - 1);

  if (in->type == VFS_FILE) {
    int mark_dirty = vma_shared && (vma->prot & PROT_WRITE);
    struct page_cache_entry *page = page_cache_get_page(in, file_page);

    if (page) {
      pmm_free_frame(frame);          /* the cache already has this page */
      frame = page->frame;
      pmm_ref_frame(frame);           /* this mapping's reference */
      if (mark_dirty)
        page_cache_mark_dirty(page);
      page_cache_put_page(page);
      cache_frame = 1;
    } else if (in->read_cb) {
      if (in->read_cb(vma->node, file_page, (char *)phys_to_virt(frame),
                      PAGE_SIZE, 0) < 0) {
        pmm_free_frame(frame);
        return -1;
      }
      if (page_cache_add_page(in, file_page, frame) == 0) {
        pmm_ref_frame(frame);         /* cache reference + this mapping's */
        cache_frame = 1;
        if (mark_dirty) {
          struct page_cache_entry *pe = page_cache_get_page(in, file_page);
          if (pe) {
            page_cache_mark_dirty(pe);
            page_cache_put_page(pe);
          }
        }
      }
    } else if (in->data) {
      /* A file whose contents live in memory: initramfs, and everything the
       * VFS creates — which includes POSIX shared memory, since shm_open is
       * open() under /dev/shm. It has to reach the page cache too, or two
       * mappers of one object each get a private copy and never see each
       * other's writes. */
      if (file_page < in->size) {
        usize n = in->size - file_page;

        if (n > PAGE_SIZE)
          n = PAGE_SIZE;
        memcpy(phys_to_virt(frame), (const char *)in->data + file_page, n);
      }
      if (page_cache_add_page(in, file_page, frame) == 0) {
        pmm_ref_frame(frame);
        cache_frame = 1;
      }
    }
  } else if (in->read_cb) {
    /* Not a regular file (a device, say): no page cache, read straight in. */
    if (in->read_cb(vma->node, file_offset, (char *)phys_to_virt(frame),
                    PAGE_SIZE, 0) < 0) {
      pmm_free_frame(frame);
      return -1;
    }
  }

  vmm_write_acquire(&f);
  /* Only install if the marker is still there: another CPU may have serviced
   * the same address while the read was in flight. */
  if ((l3[i3] & 0x3ULL) == D_PAGE || !(l3[i3] & VMM_LAZY)) {
    int present = (l3[i3] & 0x3ULL) == D_PAGE;

    vmm_write_release(f);
    if (cache_frame)
      pmm_unref_frame(frame);
    else
      pmm_free_frame(frame);
    return present ? 0 : -1;
  }

  u64 flags = VMM_PRESENT | VMM_USER;
  if (lazy_entry & VMM_WRITABLE)
    flags |= VMM_WRITABLE;
  if (vma_shared)
    flags |= VMM_SHARED;
  if (cache_frame && !vma_shared && (flags & VMM_WRITABLE)) {
    flags &= ~VMM_WRITABLE;
    flags |= VMM_COW;
  }
  l3[i3] = encode_leaf(frame, flags);
  tlb_flush_page(va);
  vmm_write_release(f);
  return 0;
}

/* The lock is held by the caller. Returns a positive value for the cases that
 * must be finished outside it: a page read back from swap, or one read from a
 * file — both are block I/O, and both block. */
#define PF_NEEDS_SWAP_IN 1
#define PF_NEEDS_FILE_FILL 2
static int handle_page_fault_locked(u64 fault_addr, u64 error_code,
                                    u64 **swap_l3, usize *swap_i3,
                                    u64 *swap_entry) {
  u64 va = fault_addr & ~(PAGE_SIZE - 1);

  u64 *l0 = get_l0_for_va(va);
  usize i0 = l0_index(va);
  u64 entry = 0;
  int have_leaf = 0;
  if ((l0[i0] & 0x3ULL) == D_TABLE) {
    u64 *l1 = table_from_entry(l0[i0]);
    usize i1 = l1_index(va);
    if ((l1[i1] & 0x3ULL) == D_TABLE) {
      u64 *l2 = table_from_entry(l1[i1]);
      usize i2 = l2_index(va);
      if ((l2[i2] & 0x3ULL) == D_TABLE) {
        u64 *l3 = table_from_entry(l2[i2]);
        usize i3 = l3_index(va);
        entry = l3[i3];
        have_leaf = 1;
        if ((entry & 0x3ULL) == D_PAGE) {
          /* Access Flag fault: the page is mapped, but its AF was cleared by
           * paging_test_and_clear_accessed to sample the reference bit. Set it
           * and retry — that IS the "page was touched" report. */
          if (!(entry & D_AF)) {
            l3[i3] = entry | D_AF;
            tlb_flush_page(va);
            return 0;
          }
          /* Otherwise the only serviceable case is a write to a page fork()
           * downgraded for copy-on-write; anything else is a genuine
           * protection violation. */
          if ((error_code & 2) && (entry & SW_COW))
            return cow_fault(l3, i3, va);
          /* The hardware found no translation, yet the tables hold a valid
           * leaf with its access flag set: another CPU serviced this very
           * fault while we were on our way here. Threads share one address
           * space, so two of them faulting on the same page at the same
           * moment is ordinary — and killing the process for it is not.
           * Publish the entry to this CPU's walker and let the instruction
           * retry; the fault-loop detector above still catches a page that
           * genuinely cannot be serviced. (Only a *permission* fault,
           * error_code bit 0, is a real violation here.) */
          if (!(error_code & 1)) {
            tlb_flush_page(va);
            return 0;
          }
          /* A present leaf that is not serviceable. */
          console_write("pf-prot: va=0x");
          console_write_hex64(va);
          console_write(" leaf=0x");
          console_write_hex64(entry);
          console_write(" err=0x");
          console_write_hex64(error_code);
          {
            u64 ttbr0;
            __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
            console_write(" ttbr0=0x");
            console_write_hex64(ttbr0);
            console_write(" pml4=0x");
            console_write_hex64(current_task ? current_task->pml4_phys : 0);
            console_write(" task=");
            console_write(current_task && current_task->name ? current_task->name : "?");
          }
          console_write("\n");
          return -1;
        }
        if (entry & VMM_SWAPPED) {
          *swap_l3 = l3;
          *swap_i3 = i3;
          *swap_entry = entry;
          return PF_NEEDS_SWAP_IN;
        }
        if (entry & VMM_LAZY) {
          /* File-backed pages are filled outside this lock, like the swap
           * case: reading them is block I/O. Anonymous ones are just a zeroed
           * frame and are finished here. */
          if (lazy_is_file_backed(va)) {
            *swap_l3 = l3;
            *swap_i3 = i3;
            *swap_entry = entry;
            return PF_NEEDS_FILE_FILL;
          }
          u64 frame = pmm_alloc_frame();
          if (!frame) return -1;
          memset(phys_to_virt(frame), 0, PAGE_SIZE);
          l3[i3] = encode_leaf(frame, entry | VMM_PRESENT);
          tlb_flush_page(va);
          return 0;
        }
      }
    }
  }
  (void)have_leaf;

  /* Not present and no lazy marker: anonymous user memory that was never
   * backed.
   *
   * A fault taken at EL1 on a USER address is serviced too, exactly as x86_64
   * services one without PF_USER: the kernel writes into user memory on the
   * task's own behalf — the signal frame, copyout — and the target page may
   * legitimately never have been touched yet. Refusing those made
   * arch_build_signal_frame's first store to a fresh sigaltstack page a fatal
   * EL1 abort: crashpad installs a SIGSEGV handler on an mmap'd alternate
   * stack, and delivering that signal panicked the kernel.
   *
   * A kernel-mode fault on a KERNEL address stays fatal — that is a real bug in
   * the kernel, not a page waiting to be materialised. And the user case still
   * goes through fault_anon_user_page, which demands a VMA (or brk/stack), so a
   * wild kernel pointer that happens to be low is refused rather than backed. */
  if (!(error_code & 4) && va >= USER_SPACE_LIMIT)
    return -1;
  return fault_anon_user_page(va);
}

int vmm_handle_page_fault(u64 fault_addr, u64 error_code) {
  u64 *swap_l3 = 0;
  usize swap_i3 = 0;
  u64 swap_entry = 0;
  u64 f;
  int rc;

  vmm_write_acquire(&f);
  rc = handle_page_fault_locked(fault_addr, error_code, &swap_l3, &swap_i3,
                               &swap_entry);
  vmm_write_release(f);

  /* Loop detection, on the way OUT and only for faults that made no progress.
   *
   * A handler that reports success while leaving the address still unmapped is
   * re-entered on the same instruction for ever: the task never returns to
   * userspace, burns the CPU, and reads as a hang rather than a fault. That is
   * the condition -- rc == 0 with the leaf still not present -- and it is the
   * only thing worth counting.
   *
   * This used to count every fault at the same address, from a static shared by
   * all tasks, and that is not the same question at all. mmap(NULL, ...) hands
   * back the same base address each time once the previous mapping is gone, so
   * a plain `mmap / touch one page / munmap` loop faults the identical address
   * on every iteration -- 100,000 times in m_posixmm_smoke's SIGKILL test --
   * all of it forward progress. It panicked the machine for it. */
  if (rc == 0) {
    static u64 stuck_va;
    static u64 stuck_count;
    u64 va = fault_addr & ~(PAGE_SIZE - 1);

    if (paging_user_frame(current_task ? current_task->pml4_phys : 0, va)) {
      stuck_count = 0; /* mapped: real progress */
    } else if (va == stuck_va && ++stuck_count > 4096) {
      u64 *l3 = leaf_table_for(current_task ? current_task->pml4_phys : 0, va);
      console_write("pf: serviced but still unmapped at 0x");
      console_write_hex64(va);
      console_write(" err=0x");
      console_write_hex64(error_code);
      console_write(" leaf=0x");
      console_write_hex64(l3 ? l3[l3_index(va)] : 0);
      console_write(" task='");
      console_write(current_task && current_task->name ? current_task->name
                                                       : "?");
      console_write("'\n");
      panic("page-fault loop");
    } else if (va != stuck_va) {
      stuck_va = va;
      stuck_count = 0;
    }
  }

  if (rc == PF_NEEDS_SWAP_IN)
    return swap_in_fault(swap_l3, swap_i3, fault_addr & ~(PAGE_SIZE - 1),
                         swap_entry);
  if (rc == PF_NEEDS_FILE_FILL)
    return file_fill_fault(swap_l3, swap_i3, fault_addr & ~(PAGE_SIZE - 1),
                           swap_entry);
  return rc;
}

u64 paging_user_frame(u64 pml4_phys, u64 vaddr) {
  u64 *l0 = pml4_phys ? phys_to_virt(pml4_phys) : kernel_l0_virt;
  usize i0 = l0_index(vaddr);
  if ((l0[i0] & 0x3ULL) != D_TABLE) return 0;
  u64 *l1 = table_from_entry(l0[i0]);
  usize i1 = l1_index(vaddr);
  if ((l1[i1] & 0x3ULL) != D_TABLE) return 0;
  u64 *l2 = table_from_entry(l1[i1]);
  usize i2 = l2_index(vaddr);
  if ((l2[i2] & 0x3ULL) != D_TABLE) return 0;
  u64 *l3 = table_from_entry(l2[i2]);
  u64 entry = l3[l3_index(vaddr)];
  if ((entry & 0x3ULL) != D_PAGE) return 0;
  return entry & ADDR_MASK;
}

u64 paging_user_phys(u64 pml4_phys, u64 vaddr) {
  u64 frame = paging_user_frame(pml4_phys, vaddr);
  if (!frame) return 0;
  return frame | (vaddr & (PAGE_SIZE - 1));
}

static void mprotect_page_in_l0(u64 *l0, u64 virtual_address, u64 flags) {
  usize i0 = l0_index(virtual_address);
  if ((l0[i0] & 0x3ULL) != D_TABLE) return;
  u64 *l1 = table_from_entry(l0[i0]);
  usize i1 = l1_index(virtual_address);
  if ((l1[i1] & 0x3ULL) != D_TABLE) return;
  u64 *l2 = table_from_entry(l1[i1]);
  usize i2 = l2_index(virtual_address);
  if ((l2[i2] & 0x3ULL) != D_TABLE) return;
  u64 *l3 = table_from_entry(l2[i2]);
  usize i3 = l3_index(virtual_address);
  u64 entry = l3[i3];
  if ((entry & 0x3ULL) == D_PAGE) {
    u64 frame = entry & ADDR_MASK;
    /* Re-encoding from `flags` alone drops the software bits, and two of them
     * must survive a protection change:
     *
     *   SW_COW    — the page is somebody else's frame (a fork sibling's, or a
     *               shared page-cache page) and this mapping only borrows it
     *               read-only. Granting write here would let the process
     *               scribble straight into that frame: for a file mapping that
     *               is the page cache's copy of the library, so the write lands
     *               in every other mapper's text and in what read() returns.
     *               Keep it read-only and COW; the first store faults and gets
     *               a private copy, which is what the caller actually asked for.
     *   SW_SHARED — MAP_SHARED and device mappings are shared on purpose, and
     *               the bit is also what stops address-space teardown from
     *               freeing a frame it does not own.
     *
     * ld.so mprotects the segments it just mapped, so this is on the path of
     * every dynamically linked program. */
    u64 keep = entry & (SW_COW | SW_SHARED);
    if (keep & SW_COW)
      flags &= ~(u64)VMM_WRITABLE;
    l3[i3] = encode_leaf(frame, flags | VMM_PRESENT) | keep;
    tlb_flush_page(virtual_address);
  } else if (entry & VMM_LAZY) {
    /* VMM_SHARED belongs in this set too: a lazily committed MAP_SHARED
     * anonymous mapping records the bit here and nowhere else, and the fault
     * that materialises the page copies the marker's flags verbatim. Dropping
     * it left the page looking private, so fork marked it copy-on-write and a
     * child's stores never reached the parent. */
    const u64 mutable_bits = VMM_WRITABLE | VMM_USER | VMM_SHARED;
    l3[i3] = (entry & ~mutable_bits) | (flags & mutable_bits) | VMM_LAZY;
  }
}

void paging_mprotect_page(u64 virtual_address, u64 flags) {
  mprotect_page_in_l0(get_current_l0(), virtual_address, flags);
}

/* The same, in an address space that is not the one currently installed —
 * futex's watch page pokes a sleeping task's mapping. A kernel address always
 * lives in the shared kernel L0 no matter whose space was named. */
void paging_mprotect_page_in_space(u64 pml4_phys, u64 vaddr, u64 flags) {
  u64 *l0 = (vaddr >= 0x8000000000000000ULL || !pml4_phys) ? kernel_l0_virt
                                                           : phys_to_virt(pml4_phys);
  u64 f;

  vmm_write_acquire(&f);
  mprotect_page_in_l0(l0, vaddr, flags);
  vmm_write_release(f);
}

void paging_mprotect_range(u64 start, u64 end, u64 flags) {
  u64 f;
  u64 *l0 = get_current_l0();

  vmm_write_acquire(&f);
  for (u64 va = start & ~(u64)(PAGE_SIZE - 1); va < end; va += PAGE_SIZE)
    mprotect_page_in_l0(l0, va, flags);
  vmm_write_release(f);
}

void paging_unmap_range_from_space(u64 pml4_phys, u64 base, usize npages) {
  u64 f;

  /* One critical section for the whole range rather than npages of them: exit
   * tears down thousands of pages and the lock round-trip dominated. */
  vmm_write_acquire(&f);
  for (usize i = 0; i < npages; i++)
    unmap_from_space_locked(pml4_phys, base + i * PAGE_SIZE);
  vmm_write_release(f);
}

void vmm_map_range(u64 base, const u64 *frames, usize n, u64 flags) {
  u64 f;

  vmm_write_acquire(&f);
  for (usize i = 0; i < n; i++)
    map_page_locked(base + i * PAGE_SIZE, frames[i], flags);
  vmm_write_release(f);
}

/* Unmap a range and hand back the frames the caller now owns.
 *
 * NOT vmm_unmap_range_nosync with a different name, which is what this was.
 * The two have different contracts and the difference is not cosmetic: nosync
 * fills one slot per page, holes included, and reports npages, while this one
 * returns a packed array and a count of it. Forwarding to nosync therefore told
 * sys_munmap to free every hole -- pmm_free_frame(0x0) on every unmapped page
 * in the range, which the allocator refused and reported, eight times before it
 * gave up counting.
 *
 * Freeing the zeros was the visible half. The other half is that nosync reports
 * every present frame, and munmap freed those too: a page-cache page or a
 * shared segment mapped into this address space would have been handed back to
 * the allocator while its owner still held it. unmap_from_space_locked has
 * always tested SW_USER before freeing; so does this now, matching x86_64.
 *
 * Two things x86_64's version does that were missing entirely: a frame going
 * back to the allocator leaves the eviction registry first, and a leaf holding
 * a swap slot rather than a frame releases the slot instead of leaking it.
 *
 * No shootdown IPI: aarch64 broadcasts TLB maintenance across the
 * inner-shareable domain in hardware (TLBI ...IS), so the unmap is complete
 * when the instruction retires.
 */
usize vmm_unmap_range_collect(u64 base, usize npages, u64 *frames_out) {
  extern void eviction_unregister_page(u64 frame);
  extern void swap_free_slot_index(u32 slot);
  u64 pml4_phys = current_task ? current_task->pml4_phys : 0;
  usize nframes = 0;
  u64 _vmflags;

  vmm_write_acquire(&_vmflags);
  for (usize i = 0; i < npages; i++) {
    u64 va = base + i * PAGE_SIZE;
    u64 *l0 = (va >= 0x8000000000000000ULL || !pml4_phys)
                  ? kernel_l0_virt
                  : phys_to_virt(pml4_phys);
    usize i0 = l0_index(va);

    if ((l0[i0] & 0x3ULL) != D_TABLE)
      continue;
    u64 *l1 = table_from_entry(l0[i0]);
    usize i1 = l1_index(va);
    if ((l1[i1] & 0x3ULL) != D_TABLE)
      continue;
    u64 *l2 = table_from_entry(l1[i1]);
    usize i2 = l2_index(va);
    if ((l2[i2] & 0x3ULL) != D_TABLE)
      continue;
    u64 *l3 = table_from_entry(l2[i2]);
    usize i3 = l3_index(va);
    u64 entry = l3[i3];

    if (!entry)
      continue;
    if ((entry & 0x3ULL) == D_PAGE) {
      u64 frame = entry & ADDR_MASK;

      if (frame && (entry & SW_USER)) {
        eviction_unregister_page(frame);
        frames_out[nframes++] = frame;
      }
    } else if (entry & VMM_SWAPPED) {
      swap_free_slot_index((u32)((entry & ADDR_MASK) >> 12));
    }
    l3[i3] = 0;
    tlb_flush_page(va);
  }
  vmm_write_release(_vmflags);
  return nframes;
}

void vmm_set_lazy_flags(u64 virtual_address, u64 flags) {
  u64 f;

  vmm_write_acquire(&f);
  set_lazy_locked(virtual_address);
  {
    u64 *l0 = get_l0_for_va(virtual_address);
    u64 *l1 = ensure_child(l0, l0_index(virtual_address), 0);
    u64 *l2 = ensure_child(l1, l1_index(virtual_address), 1);
    u64 *l3 = ensure_child(l2, l2_index(virtual_address), 2);
    usize i3 = l3_index(virtual_address);

    /* set_lazy_locked plants USER|WRITABLE; keep the marker bit and take the
     * permissions the caller actually asked for (a read-only file mapping must
     * not fault in writable). */
    l3[i3] = VMM_LAZY | (flags & ~(u64)VMM_PRESENT);
  }
  vmm_write_release(f);
}

/* x86_64 counts the COW shootdowns it had to send; aarch64 sends none — see
 * vmm_unmap_range_collect. Reported as zero rather than left undefined so
 * /proc's futex diagnostics read the same on both. */
void cow_shootdown_stats(u64 *done, u64 *skipped) {
  if (done)
    *done = 0;
  if (skipped)
    *skipped = 0;
}

u64 paging_create_address_space(void) {
  u64 *l0 = alloc_table();
  l0[0] = kernel_l0_virt[0]; /* share the kernel half by pointer */
  return virt_to_phys(l0);
}

/* Recursively free every table/frame under a user-half subtree (level 1..3,
 * where level 3 tables hold leaf pages). */
static void free_user_subtree(u64 *table, int level) {
  for (usize i = 0; i < 512; i++) {
    u64 entry = table[i];
    if (level < 3) {
      if ((entry & 0x3ULL) == D_TABLE) {
        free_user_subtree(table_from_entry(entry), level + 1);
        pmm_free_frame(entry & ADDR_MASK);
      }
    } else if ((entry & 0x3ULL) == D_PAGE) {
      u64 frame = entry & ADDR_MASK;
      if (frame && (entry & SW_USER)) {
        pmm_free_frame(frame);
      }
    }
  }
}

void paging_free_address_space(u64 pml4_phys) {
  if (!pml4_phys) return;
  u64 *l0 = phys_to_virt(pml4_phys);
  for (usize i = 1; i < 512; i++) {
    if ((l0[i] & 0x3ULL) == D_TABLE) {
      free_user_subtree(table_from_entry(l0[i]), 1);
      pmm_free_frame(l0[i] & ADDR_MASK);
    }
  }
  pmm_free_frame(pml4_phys);
}

/* Clone one leaf table for fork(2), copy-on-write.
 *
 * A private writable page is installed read-only + SW_COW in BOTH the parent
 * and the child, and the frame gains a reference. The first write from either
 * side takes a permission fault, which vmm_handle_page_fault resolves by
 * copying (or, when the writer turns out to be the last owner, by just
 * restoring write permission). Previously this copied every present user page
 * eagerly, so a fork of a large process duplicated its whole address space up
 * front — the cost fork exists to avoid, paid on every shell pipeline. */
static u64 *clone_leaf_table(u64 *src_l3, int *cow_marked) {
  u64 *dst_l3 = alloc_table();
  for (usize i = 0; i < 512; i++) {
    u64 entry = src_l3[i];
    if ((entry & 0x3ULL) != D_PAGE) {
      dst_l3[i] = entry; /* invalid, or a lazy/swap marker: copy verbatim */
      continue;
    }
    if (entry & SW_SHARED) {
      /* MAP_SHARED / SysV shm / device mappings must stay SHARED across fork,
       * not be duplicated: the whole point is that both processes see each
       * other's writes. Copying them broke shmat() across fork — the parent
       * spun forever waiting for a flag the child had written into its own
       * private copy. Take a reference so the frame outlives either side. */
      dst_l3[i] = entry;
      pmm_ref_frame(entry & ADDR_MASK);
      continue;
    }

    u64 frame = entry & ADDR_MASK;
    /* Writability lives in the AP field, so "make it read-only" means forcing
     * AP to the RO encoding of whichever privilege level this page belongs to,
     * and SW_COW remembers that it was writable so the fault can put it back. */
    u64 shared_entry = entry;
    if ((entry & AP_EL1_EL0_RO) != AP_EL1_EL0_RO) { /* i.e. currently writable */
      shared_entry &= ~AP_EL1_EL0_RO;
      shared_entry |= (entry & SW_USER) ? AP_EL1_EL0_RO : AP_EL1_RO;
      shared_entry |= SW_COW;
      src_l3[i] = shared_entry; /* the parent loses write access too */
      *cow_marked = 1;
    }
    dst_l3[i] = shared_entry;
    pmm_ref_frame(frame);
  }
  return dst_l3;
}

u64 paging_clone_address_space(u64 src_pml4_phys) {
  u64 *src_l0 = src_pml4_phys ? phys_to_virt(src_pml4_phys) : kernel_l0_virt;
  u64 *dst_l0 = alloc_table();
  dst_l0[0] = kernel_l0_virt[0];
  int cow_marked = 0;

  for (usize i0 = 1; i0 < 512; i0++) {
    if ((src_l0[i0] & 0x3ULL) != D_TABLE) continue;
    u64 *src_l1 = table_from_entry(src_l0[i0]);
    u64 *dst_l1 = alloc_table();
    dst_l0[i0] = virt_to_phys(dst_l1) | D_TABLE;
    for (usize i1 = 0; i1 < 512; i1++) {
      if ((src_l1[i1] & 0x3ULL) != D_TABLE) continue;
      u64 *src_l2 = table_from_entry(src_l1[i1]);
      u64 *dst_l2 = alloc_table();
      dst_l1[i1] = virt_to_phys(dst_l2) | D_TABLE;
      for (usize i2 = 0; i2 < 512; i2++) {
        if ((src_l2[i2] & 0x3ULL) != D_TABLE) continue;
        u64 *dst_l3 =
            clone_leaf_table(table_from_entry(src_l2[i2]), &cow_marked);
        dst_l2[i2] = virt_to_phys(dst_l3) | D_TABLE;
      }
    }
  }
  /* The parent keeps running on this same translation table and its TLB still
   * holds the writable entries we just downgraded. Without a flush its very
   * next stack write bypasses the COW fault through a stale entry and scribbles
   * into the frame the child now shares. One full flush is right here: the
   * downgrade touched an unbounded number of pages. */
  if (cow_marked)
    tlb_flush_all();
  return virt_to_phys(dst_l0);
}

/* The kernel's own translation-table root. A secondary CPU has to be handed
 * this before it can run anything but the boot identity map. */
u64 paging_kernel_root_phys(void) { return kernel_l0_phys; }

void paging_switch_address_space(u64 pml4_phys) {
  u64 target = pml4_phys ? pml4_phys : kernel_l0_phys;
  __asm__ volatile("msr ttbr0_el1, %0\n\t"
                    "isb" ::"r"(target)
                    : "memory");
  tlb_flush_all();
}

/* MMIO window: a real kernel mapping per request, so a caller can ask for a
 * memory type the boot identity block does not carry (ioremap_wc on a RAM
 * frame) and get it.
 *
 * The window sits at 416 GiB: inside L0[0] — the half every address space
 * shares by pointer — and clear of the heap (64 GiB), the large-allocation
 * arena (128-320 GiB), the two vmap windows (320/352 GiB) and the M98
 * write-combining test page (400 GiB). Bump-allocated and never reclaimed,
 * which is what iounmap's comment in kernel/lkpi/lkpi_core.c already promises.
 *
 * A first attempt at this was reverted in v0.106.63 because mappings stopped
 * existing later (e1000 read its MAC at probe, then took a level-3 translation
 * fault on the same registers from process context). Several address-space
 * bugs have been fixed since; if it returns, the thing to chase is who frees
 * the tables — see docs/aarch64-parity.md. */
#define MMIO_WINDOW_BASE 0x6800000000ULL /* 416 GiB */
#define MMIO_WINDOW_END  0x7000000000ULL /* 448 GiB */

static u64 mmio_next = MMIO_WINDOW_BASE;

void *vmm_map_mmio(u64 physical_address, usize size, u64 flags) {
  if (!size)
    return 0;
  u64 pa = physical_address & ~(u64)(PAGE_SIZE - 1);
  u64 offset = physical_address - pa;
  u64 len = (size + offset + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
  if (mmio_next + len > MMIO_WINDOW_END)
    return 0; /* window exhausted — callers already handle a NULL mapping */
  u64 va = mmio_next;
  mmio_next += len;

  /* Device memory unless the caller asked for write-combining. This window
   * exists to reach registers, and encode_leaf only produces Device from the
   * x86 pair VMM_PCD|VMM_PWT — a caller that passes VMM_PCD alone (which is
   * what "uncacheable" looks like on x86) would otherwise get Normal
   * write-back memory here. That is not a performance detail: an MSI-X table
   * mapped cacheable reads back exactly what was written and the device never
   * sees any of it, so the interrupt is programmed, verified, and never
   * arrives. */
  u64 attrs = flags;
  if ((attrs & VMM_WC) != VMM_WC)
    attrs |= VMM_PCD | VMM_PWT;

  for (u64 i = 0; i < len; i += PAGE_SIZE)
    vmm_map_page(va + i, pa + i, attrs | VMM_PRESENT);
  return (void *)(usize)(va + offset);
}

int paging_pte_is_wc(u64 pte) {
  /* MAIR slot 2 — Normal Non-cacheable, set up in boot.S. */
  return (pte & 0x3ULL) == D_PAGE && (pte & ATTR_INDX_MASK) == ATTR_NORMAL_NC;
}

/* Replace a present leaf with a non-present VMM_SWAPPED marker carrying the
 * swap slot in its address field — the shape swap_in_fault above decodes. The
 * frame itself is freed by the eviction path that called us, not here. */
void paging_mark_swapped(u64 pml4_phys, u64 vaddr, u64 slot) {
  u64 *l0 = pml4_phys ? phys_to_virt(pml4_phys) : kernel_l0_virt;
  usize i0 = l0_index(vaddr);
  if ((l0[i0] & 0x3ULL) != D_TABLE) return;
  u64 *l1 = table_from_entry(l0[i0]);
  usize i1 = l1_index(vaddr);
  if ((l1[i1] & 0x3ULL) != D_TABLE) return;
  u64 *l2 = table_from_entry(l1[i1]);
  usize i2 = l2_index(vaddr);
  if ((l2[i2] & 0x3ULL) != D_TABLE) return;
  u64 *l3 = table_from_entry(l2[i2]);
  usize i3 = l3_index(vaddr);
  u64 entry = l3[i3];
  if ((entry & 0x3ULL) != D_PAGE) return;
  /* bit0 (valid) clear, so hardware ignores every other bit: this is purely a
   * software record. Keep SW_USER so the fault path can restore the mapping's
   * privilege level. */
  l3[i3] = ((slot << 12) & ADDR_MASK) | VMM_SWAPPED | (entry & SW_USER);
  tlb_flush_page(vaddr);
}

/* Bring every swapped-out page of an address space back into memory. fork(2)
 * and execve(2) call this because both walk the page tables directly and
 * cannot fault a page in on the caller's behalf. */
extern void eviction_register_page(struct task *task, u64 vaddr, u64 frame);

static void swap_in_subtree(u64 *table, int level, u64 base) {
  u64 step = 1ULL << (12 + (3 - level) * 9);
  for (usize i = 0; i < 512; i++) {
    u64 entry = table[i];
    u64 va = base + i * step;
    if (level < 3) {
      if ((entry & 0x3ULL) == D_TABLE)
        swap_in_subtree(table_from_entry(entry), level + 1, va);
      continue;
    }
    if ((entry & 0x3ULL) == D_PAGE || !(entry & VMM_SWAPPED))
      continue;
    u64 frame = 0;
    if (swap_in((u32)((entry & ADDR_MASK) >> 12), &frame) < 0)
      continue;
    u64 flags = VMM_PRESENT | VMM_WRITABLE;
    if (entry & SW_USER) flags |= VMM_USER;
    table[i] = encode_leaf(frame, flags);
    tlb_flush_page(va);
    eviction_register_page(current_task, va, frame);
  }
}

void paging_swap_in_all_swapped(u64 pml4_phys) {
  if (!pml4_phys || pml4_phys == kernel_l0_phys)
    return;
  u64 *l0 = phys_to_virt(pml4_phys);
  /* L0[0] is the kernel half, shared by pointer — never swapped, never walked. */
  for (usize i0 = 1; i0 < 512; i0++) {
    if ((l0[i0] & 0x3ULL) == D_TABLE)
      swap_in_subtree(table_from_entry(l0[i0]), 1, i0 << 39);
  }
}

/* Slots are released by the generic swap code as it walks the tables; x86_64
 * leaves this a no-op for the same reason. */
void paging_free_swap_slots(u64 space) { (void)space; }

/* Reference bit. AArch64 without FEAT_HAFDBS never sets the Access Flag in
 * hardware — instead, touching a page whose AF is clear raises an Access Flag
 * fault, which vmm_handle_page_fault answers by setting the bit. That gives the
 * eviction LRU exactly the "was this page touched since I last asked" signal
 * x86_64 gets from its hardware A bit. */
int paging_test_and_clear_accessed(u64 space, u64 vaddr) {
  u64 *l3 = leaf_table_for(space, vaddr);
  if (!l3)
    return 0;
  usize i3 = l3_index(vaddr);
  u64 entry = l3[i3];
  if ((entry & 0x3ULL) != D_PAGE || !(entry & D_AF))
    return 0;
  l3[i3] = entry & ~D_AF;
  tlb_flush_page(vaddr);
  return 1;
}

/* Dirty bit. There is no hardware one here either, and emulating it (map
 * read-only, mark dirty on the write fault) would collide with the COW bit
 * this port already overloads the AP field for. Report every writable resident
 * page as potentially dirty instead: msync(2) then writes back a little more
 * than it must, which is the safe direction. Reporting 0 — what this returned
 * before — made msync skip every page, so a MAP_SHARED file mapping's stores
 * were never written back at all. */
int paging_test_and_clear_dirty(u64 space, u64 vaddr) {
  u64 *l3 = leaf_table_for(space, vaddr);
  if (!l3)
    return 0;
  u64 entry = l3[l3_index(vaddr)];
  if ((entry & 0x3ULL) != D_PAGE)
    return 0;
  return (entry & AP_EL1_EL0_RO) != AP_EL1_EL0_RO; /* i.e. writable */
}

void tlb_shootdown_all(void) { tlb_flush_all(); }

void tlb_shootdown_page(u64 vaddr) { tlb_flush_page(vaddr); }

u64 vmm_virt_to_phys(void *virt) {
  u64 va = (u64)(usize)virt;
  u64 page_off = va & (PAGE_SIZE - 1);
  u64 *l0 = get_l0_for_va(va);
  if (!l0)
    return 0;
  usize i0 = l0_index(va);
  if ((l0[i0] & 0x3ULL) != D_TABLE) return va; /* untouched: identity */
  u64 *l1 = table_from_entry(l0[i0]);
  usize i1 = l1_index(va);
  u64 l1e = l1[i1];
  if ((l1e & 0x3ULL) == D_BLOCK) return (l1e & ADDR_MASK) | (va & (BLOCK_SIZE_L1 - 1));
  if ((l1e & 0x3ULL) != D_TABLE) return va;
  u64 *l2 = table_from_entry(l1e);
  usize i2 = l2_index(va);
  u64 l2e = l2[i2];
  if ((l2e & 0x3ULL) == D_BLOCK) return (l2e & ADDR_MASK) | (va & (BLOCK_SIZE_L2 - 1));
  if ((l2e & 0x3ULL) != D_TABLE) return va;
  u64 *l3 = table_from_entry(l2e);
  u64 l3e = l3[l3_index(va)];
  if ((l3e & 0x3ULL) == D_PAGE) return (l3e & ADDR_MASK) | page_off;
  return va; /* not present: nothing better to report */
}

/* M100/DRM: pre-install the intermediate tables covering [va, va+size) without
 * mapping any leaf, so a later mapping of that range never has to allocate a
 * page-table frame at a point where it cannot block (drm.c reserves its
 * aperture this way). x86_64 walks by 1 GiB because one PD covers that much;
 * the same holds here (an L2 table covers 1 GiB). */
int paging_reserve_kernel_path(u64 virtual_address, u64 size) {
  if (size == 0)
    return -1;
  u64 *l0 = get_l0_for_va(virtual_address);
  u64 start = virtual_address & ~(u64)(PAGE_SIZE - 1);
  u64 end = virtual_address + size;
  for (u64 va = start; va < end; va += (1ULL << 30)) {
    u64 *l1 = ensure_child(l0, l0_index(va), 0);
    (void)ensure_child(l1, l1_index(va), 1);
  }
  return 0;
}

/* ── page-table introspection the M98-M100 self-tests use ─────────────────
 * x86_64 names these after its PML4; here the top level is L0 and the leaf is
 * an L3 page descriptor. The contract is the same: report the raw entry, or 0
 * when nothing is installed. */

/* The top-level (L0) entry the CURRENT address space holds for `virtual_address`. */
u64 paging_pml4_entry_current(u64 virtual_address) {
  u64 *l0 = get_l0_for_va(virtual_address);
  return l0[l0_index(virtual_address)];
}

/* The L0 entry an address space holds for `virtual_address`, 0 when absent.
 * Every process shares the kernel half by pointer here (L0[0] is copied into
 * each new table), so a kernel window really is the same entry in all of them. */
u64 paging_pml4_entry_in(u64 pml4_phys, u64 virtual_address) {
  if (!pml4_phys)
    return 0;
  u64 *l0 = phys_to_virt(pml4_phys);
  return l0[l0_index(virtual_address)];
}

/* The raw leaf (L3) descriptor backing `virtual_address`, or 0 when the range
 * is unmapped or mapped by a block descriptor rather than a 4 KiB page. */
u64 paging_leaf_pte(u64 virtual_address) {
  /* A walk answers "no mapping" for a table that does not exist; it does not
   * fault. get_l0_for_va can hand back a null root -- a task whose address
   * space is gone, or the kernel root before paging_init has built it -- and
   * every level below can be absent for the same reason. Reading through one
   * was a data abort at a small offset (FAR 0x7f8 = entry 255 of a null table),
   * reported as a kernel crash in whatever happened to be walking. */
  u64 *l0 = get_l0_for_va(virtual_address);
  if (!l0)
    return 0;
  u64 l0e = l0[l0_index(virtual_address)];
  if ((l0e & 0x3ULL) != D_TABLE)
    return 0;
  u64 *l1 = table_from_entry(l0e);
  if (!l1)
    return 0;
  u64 l1e = l1[l1_index(virtual_address)];
  if ((l1e & 0x3ULL) != D_TABLE)
    return 0;
  u64 *l2 = table_from_entry(l1e);
  if (!l2)
    return 0;
  u64 l2e = l2[l2_index(virtual_address)];
  if ((l2e & 0x3ULL) != D_TABLE)
    return 0;
  u64 *l3 = table_from_entry(l2e);
  if (!l3)
    return 0;
  return l3[l3_index(virtual_address)];
}

/* Pointer to the leaf (L3) descriptor for `virtual_address`, or 0 when no
 * table describes it. Walks, never allocates. */
static u64 *leaf_pte_ptr(u64 virtual_address) {
  /* Same null-table rule as paging_leaf_pte. */
  u64 *l0 = get_l0_for_va(virtual_address);
  if (!l0)
    return 0;
  u64 l0e = l0[l0_index(virtual_address)];
  if ((l0e & 0x3ULL) != D_TABLE)
    return 0;
  u64 *l1 = table_from_entry(l0e);
  if (!l1)
    return 0;
  u64 l1e = l1[l1_index(virtual_address)];
  if ((l1e & 0x3ULL) != D_TABLE)
    return 0;
  u64 *l2 = table_from_entry(l1e);
  if (!l2)
    return 0;
  u64 l2e = l2[l2_index(virtual_address)];
  if ((l2e & 0x3ULL) != D_TABLE)
    return 0;
  u64 *l3 = table_from_entry(l2e);
  if (!l3)
    return 0;
  return &l3[l3_index(virtual_address)];
}

/* Same reading as paging_leaf_pte — the fault reporter wants the entry itself,
 * because "absent" and "present but not writable" are different faults. */
/* The leaf as the ARCH-NEUTRAL callers expect it: generic VMM_* flags, not the
 * raw ARM descriptor.
 *
 * Returning the descriptor was silently wrong on every one of them, because
 * the two encodings overlap in exactly the bits they test. A page descriptor
 * always has bits 0 and 1 set, so VMM_PRESENT AND VMM_WRITABLE read as true
 * for a page that is in fact read-only, and VMM_USER lands on an attribute
 * index bit. syscall_copyout's pre-check therefore passed every read-only user
 * page straight through to the memcpy, which took an unfixable EL1 abort and
 * panicked the machine instead of returning EFAULT. futex's shared-key test
 * read a memory-attribute bit as VMM_SHARED for the same reason.
 *
 * Non-present entries are the kernel's own markers (VMM_LAZY, VMM_SWAPPED),
 * which are stored in the generic encoding already, so they pass through. */
u64 vmm_query_leaf_pte(u64 vaddr) {
  u64 entry = paging_leaf_pte(vaddr);

  if ((entry & 0x3ULL) != D_PAGE)
    return entry;

  u64 ap = entry & (3ULL << 6);
  u64 out = (entry & ADDR_MASK) | VMM_PRESENT;

  if (ap == AP_EL1_RW || ap == AP_EL1_EL0_RW)
    out |= VMM_WRITABLE;
  if (entry & SW_USER)
    out |= VMM_USER;
  if (entry & SW_SHARED)
    out |= VMM_SHARED;
  if (entry & SW_COW)
    out |= VMM_COW;
  return out;
}

/* The leaf descriptor for `vaddr` in the space rooted at `pml4_phys` (the L0
 * table here), 0 when nothing 4 KiB-mapped is there. */
u64 paging_user_pte(u64 pml4_phys, u64 vaddr) {
  u64 *l0 = pml4_phys ? phys_to_virt(pml4_phys) : kernel_l0_virt;
  u64 l0e = l0[l0_index(vaddr)];
  if ((l0e & 0x3ULL) != D_TABLE)
    return 0;
  u64 *l1 = table_from_entry(l0e);
  u64 l1e = l1[l1_index(vaddr)];
  if ((l1e & 0x3ULL) != D_TABLE)
    return 0;
  u64 *l2 = table_from_entry(l1e);
  u64 l2e = l2[l2_index(vaddr)];
  if ((l2e & 0x3ULL) != D_TABLE)
    return 0;
  u64 entry = table_from_entry(l2e)[l3_index(vaddr)];
  return ((entry & 0x3ULL) == D_PAGE) ? entry : 0;
}

/* x86_64 rewrites CR3 with its own value to flush the space it is already on;
 * here the equivalent is simply invalidating the TLB for the running ASID. */
void paging_reload_cr3(void) { tlb_flush_all(); }

/* Move a range's leaf entries to another address in the current space, leaving
 * the frames themselves where they are (mremap). The destination tables are
 * built by vmm_set_lazy, so nothing has to be allocated under the walk. */
void paging_move_range(u64 old_start, u64 new_start, u64 len) {
  for (u64 off = 0; off < len; off += PAGE_SIZE) {
    u64 from = old_start + off;
    u64 to = new_start + off;

    vmm_set_lazy(to);

    u64 *src = leaf_pte_ptr(from);
    u64 *dst = leaf_pte_ptr(to);
    if (src && dst && *src) {
      *dst = *src;
      *src = 0;
      tlb_flush_page(from);
      tlb_flush_page(to);
    }
  }
}

/* Pages actually resident in one user VA range — /proc/<pid>/statm's RSS and
 * the scheduler's RSS sampler. Absent entries skip their whole span rather
 * than being stepped page by page, so a sparse 4 GiB mapping costs a handful
 * of reads. Lazy markers do not count: no frame is backing them yet. */
u64 paging_user_resident(u64 pml4_phys, u64 start, u64 end) {
  if (end <= start)
    return 0;

  u64 *l0 = pml4_phys ? phys_to_virt(pml4_phys) : kernel_l0_virt;
  const u64 l0_span = 1ULL << 39;
  const u64 l1_span = 1ULL << 30;
  const u64 l2_span = 1ULL << 21;
  u64 count = 0;
  u64 va = start & ~(u64)(PAGE_SIZE - 1);

  while (va < end) {
    u64 next_l0 = (va & ~(l0_span - 1)) + l0_span;
    u64 next_l1 = (va & ~(l1_span - 1)) + l1_span;
    u64 next_l2 = (va & ~(l2_span - 1)) + l2_span;

    u64 e0 = l0[l0_index(va)];
    if ((e0 & 0x3ULL) != D_TABLE) { va = next_l0; continue; }

    u64 *l1 = table_from_entry(e0);
    u64 e1 = l1[l1_index(va)];
    if ((e1 & 0x3ULL) == D_BLOCK) {
      count += (next_l1 - va) / PAGE_SIZE;
      va = next_l1;
      continue;
    }
    if ((e1 & 0x3ULL) != D_TABLE) { va = next_l1; continue; }

    u64 *l2 = table_from_entry(e1);
    u64 e2 = l2[l2_index(va)];
    if ((e2 & 0x3ULL) == D_BLOCK) {
      count += (next_l2 - va) / PAGE_SIZE;
      va = next_l2;
      continue;
    }
    if ((e2 & 0x3ULL) != D_TABLE) { va = next_l2; continue; }

    u64 *l3 = table_from_entry(e2);
    if ((l3[l3_index(va)] & 0x3ULL) == D_PAGE)
      count++;
    va += PAGE_SIZE;
  }
  return count;
}
