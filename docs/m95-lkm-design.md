# M96/M97: Loadable Kernel Modules (LKM) Subsystem Design

## 1. Executive Summary

This document specifies the architecture, memory layout, relocation engine, lifecycle management, and subsystem integrations for Loadable Kernel Modules (LKM) in **b1nix** (milestones M96 and M97).

Historically, b1nix has compiled all filesystem drivers (`ext4`, `ntfs`, `btrfs`, `isofs`, `fat32`), network protocols (`ipv6`, `ndp`, `ntp`), and hardware drivers (`hda`, `virtio-net`) directly into `kernel.elf`. While simple and deterministic, this inflates the resident kernel image size and forces a full kernel rebuild/reboot to update or add any ring-0 driver.

The b1nix LKM architecture mirrors Linux's **`ET_REL` ELF runtime relocation model**:
- Modules are compiled as relocatable ELF object files (`.ko` / `ET_REL`) using `tools/b1nix-cc -r`.
- Loaded via `init_module(2)` or `finit_module(2)` syscalls.
- Allocated in a dedicated higher-half **Module Memory Region** (`0xFFFFFFFFC0000000 - 0xFFFFFFFFE0000000`, ~512 MB) situated within 32-bit PC-relative displacement (`R_X86_64_PC32`) of the kernel core.
- Symbols are resolved dynamically against an `EXPORT_SYMBOL` lookup table (`__ksymtab`).
- Protected by strict W^X memory permissions (`module_enable_ro()`) and per-subsystem reference counting (`try_module_get()` / `module_put()`).

---

## 2. Memory Layout and `module_alloc`

### 2.1 Virtual Address Space Allocation (x86_64)

In b1nix, the kernel core is linked at `0xFFFFFFFF80000000` (top -2 GB canonical window, `-mcmodel=kernel`). Because AMD64 `call` and `jmp` instructions using 32-bit relative offsets (`R_X86_64_PC32`) can only reach targets within `±2 GB`, module code **must** be allocated in virtual memory geographically adjacent to the main kernel image.

```
0xFFFFFFFF80000000 +------------------------------------------+
                   | Kernel Image (.text, .rodata, .data, .bss) |
                   | (pad to 512K boundary for TCC layout)    |
0xFFFFFFFFBFFFFFFF +------------------------------------------+
0xFFFFFFFFC0000000 +------------------------------------------+
                   | MODULE REGION (512 MB)                   |
                   | Allocated via module_alloc()             |
                   | .text   -> RX (after module_enable_ro)   |
                   | .rodata -> RO                            |
                   | .data   -> RW                            |
0xFFFFFFFFE0000000 +------------------------------------------+
```

### 2.2 `module_alloc()` and Strict W^X Enforcement

- `void *module_alloc(usize size)`:
  Allocates page-aligned physical frames from `pmm_alloc_frames()` and maps them into the `0xFFFFFFFFC0000000 - 0xFFFFFFFFE0000000` window using `vmm_map_page()`.
  *During load time*: Mapped as `PAGE_KERNEL_EXEC` (RWX) to allow ELF parsing, section copying, and in-place relocation patching.
- `void module_enable_ro(struct module *mod)`:
  Called immediately after `apply_relocate_add()` and before `mod->init()`.
  Updates page table entry (PTE) flags:
  - Code sections (`.text`, `.init.text`): cleared `RW` -> **RX** (Read-Execute).
  - Read-only data (`.rodata`, `.modinfo`, `__ksymtab`): cleared `RW` & `NX` -> **RO** (Read-Only).
  - Writable data (`.data`, `.bss`): set `NX`, kept `RW` -> **RW-NX**.
- `void module_memfree(struct module *mod)`:
  Unmaps PTEs, returns physical frames to PMM, and frees the virtual allocation bitmap slot.

---

## 3. Data Structures and Lifecycle State Machine

### 3.1 `struct module` (`kernel/include/b1nix/module.h`)

```c
enum module_state {
    MODULE_STATE_UNFORMED, /* Loading, parsing ELF & resolving symbols */
    MODULE_STATE_COMING,   /* Relocated, W^X applied, executing mod->init() */
    MODULE_STATE_LIVE,     /* Active and operational */
    MODULE_STATE_GOING,    /* Unloading, mod->exit() running, rejecting new refs */
};

struct module {
    enum module_state state;
    char name[64];
    struct list_head list;  /* Member of global `modules` list */

    /* Execution entry points */
    int (*init)(void);
    void (*exit)(void);

    /* Memory tracking */
    void *core_layout_base;
    usize core_layout_size;
    void *init_layout_base; /* `.init.text` freed after init() returns */
    usize init_layout_size;

    /* Reference count protecting against unload while in use */
    atomic_t refcount;

    /* Exported symbols by this module */
    usize num_syms;
    const struct kernel_symbol *syms;

    /* String parameters parsed from insmod command line */
    usize num_kp;
    struct kernel_param *kp;
};
```

### 3.2 State Transitions & SMP Synchronization

The global `modules` list and state transitions are protected by `module_spinlock` (`spin_lock_irqsave`).

```
[insmod] ---> UNFORMED --(relocate & W^X)--> COMING --(mod->init() == 0)--> LIVE
                 |                             |                              |
            (error abort)                 (init failed)                 [rmmod]
                 |                             |                              v
                 v                             v                            GOING
            [free_module] <--------------------+------------(refcount==0)-----+
```

---

## 4. ELF `ET_REL` Relocation Engine

### 4.1 Section Processing (`kernel/module/elf.c`)

When `load_module()` receives an ELF byte buffer:
1. Validates `e_ident` (ELF magic, `ELFCLASS64`, `ELFDATA2LSB`, `EM_X86_64`).
2. Confirms `e_type == ET_REL` (Relocatable object).
3. Locates key sections by name:
   - `.gnu.linkonce.this_module`: Contains default `struct module` placeholder.
   - `.modinfo`: Contains `vermagic=`, `license=`, `depends=`, `parm=`.
   - `.text`, `.rodata`, `.data`, `.bss`, `.init.text`.
   - `.symtab` and `.strtab`.
   - `.rela.text`, `.rela.rodata`, `.rela.data`, etc.

### 4.2 Relocation Types (`kernel/module/symbol.c`)

`apply_relocate_add(struct elf64_shdr *sechdrs, const char *strtab, usize symindex, usize relsec, struct module *mod)` handles x86_64 relocations:

| Relocation Type | Formula | Usage in LKM |
|---|---|---|
| `R_X86_64_64` | `S + A` | 64-bit absolute pointers in `.data` / function pointer tables |
| `R_X86_64_32S` | `S + A` | Signed 32-bit absolute pointers (requires `-mcmodel=kernel`) |
| `R_X86_64_PC32` | `S + A - P` | 32-bit PC-relative offset for `call` / `jmp` / RIP-relative data |
| `R_X86_64_PLT32` | `S + A - P` | Handled identically to `PC32` (no PLT in kernel space) |
| `R_X86_64_NONE` | N/A | No-op padding |

Where:
- `S`: Absolute address of the target symbol (from kernel `__ksymtab` or module internal section).
- `A`: Addend specified in `Elf64_Rela::r_addend`.
- `P`: Virtual address of the location being patched (`sechdr->sh_addr + rela->r_offset`).

---

## 5. Symbol Export and Reference Counting

### 5.1 `EXPORT_SYMBOL` Infrastructure

Kernel APIs intended for module use are exported via macros in `kernel/include/b1nix/export.h`:

```c
struct kernel_symbol {
    u64 value;
    const char *name;
};

#define EXPORT_SYMBOL(sym) \
    static const char __kstrtab_##sym[] \
    __attribute__((section("__ksymtab_strings"), aligned(1))) = #sym; \
    static const struct kernel_symbol __ksymtab_##sym \
    __attribute__((section("__ksymtab"), aligned(8))) = { \
        (u64)&sym, __kstrtab_##sym \
    }
```

At boot, `symbol_init()` sorts the core `__ksymtab` entries for $O(\log N)$ binary search symbol lookup during module load.

### 5.2 Subsystem Reference Counting Integration

To prevent unloading a module while its code is executing or referenced:
- Every subsystem registration struct is augmented with a `struct module *owner` field:
  - `struct vfs_fs { const char *name; ...; struct module *owner; };`
  - `struct block_device { ...; struct module *owner; };`
  - `struct net_device { ...; struct module *owner; };`
- Entry points perform `try_module_get(owner)` before dispatching:
  ```c
  bool try_module_get(struct module *mod) {
      if (!mod) return true;
      if (mod->state != MODULE_STATE_LIVE) return false;
      atomic_inc(&mod->refcount);
      return true;
  }
  ```
- Exit points call `module_put(owner)`:
  ```c
  void module_put(struct module *mod) {
      if (mod) atomic_dec(&mod->refcount);
  }
  ```
- `sys_delete_module()` rejects unload if `atomic_read(&mod->refcount) > 0`.

---

## 6. System Calls and User-Kernel Interface

### 6.1 Syscall Definitions

Added to `kernel/include/b1nix/syscall.h` and `userspace/include/syscall.h`:

```c
#define SYS_INIT_MODULE   110
#define SYS_FINIT_MODULE  111
#define SYS_DELETE_MODULE 112
```

1. `long sys_init_module(void __user *umod, usize len, const char __user *uargs)`:
   Copies user memory buffer into kernel staging, invokes `load_module()`.
2. `long sys_finit_module(int fd, const char __user *uargs, int flags)`:
   Reads directly from VFS file descriptor into staging buffer (used by `modprobe`).
3. `long sys_delete_module(const char __user *uname, int flags)`:
   Looks up module by name, checks `refcount == 0`, transition state to `GOING`, calls `mod->exit()`, unlinks from global list, frees memory via `module_memfree()`.

### 6.2 Vermagic and Safety Guards

Each module's `.modinfo` section includes `vermagic="0.96.0 SMP"`.
During `load_module()`:
1. `vermagic` string must match `B1NIX_VERSION_STR`.
2. Unsigned/tainted modules emit a warning to klog (`/proc/kmsg`).

---

## 7. Procfs and Utilities Interface

### 7.1 `/proc/modules` Format

`/proc/modules` (implemented in `kernel/fs/procfs.c`) outputs line-by-line:
```
<module_name> <size_bytes> <refcount> <state> <dependencies>
ntfs 65536 1 Live -
sound 49152 0 Live -
```

### 7.2 Shell Commands (`kernel/user/busybox.c`)

- `insmod <file.ko> [args...]`: Calls `finit_module(open(file), args, 0)`.
- `rmmod <name>`: Calls `delete_module(name, 0)`.
- `lsmod`: Reads and formats `/proc/modules`.
- `modprobe <name>`: Consults `/lib/modules/<ver>/modules.dep` for dependency resolution, calling `finit_module` sequentially.

---

## 8. Milestone M96/M97 Roadmap Breakout

- **M96**: Core LKM Infra (`module_alloc`, ELF relocation engine, `EXPORT_SYMBOL`, `init_module`/`delete_module` syscalls, `/proc/modules`, conversion of `ntfs`, `btrfs`, `isofs`, `sound` to `.ko`).
- **M97**: Network protocol modules (`ipv6`, `ndp`, `ntp`), `module_param` sysfs integration (`/sys/module/<name>/parameters/`), `request_module()` auto-loading, and `depmod.sh` dependency generation.
