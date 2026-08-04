/* Loadable Kernel Modules (M95/M96).
 *
 * b1nix modules are plain relocatable ELF objects (ET_REL, ".ko") built from
 * kernel sources with -DMODULE. The loader copies each allocated section into
 * the module region at 0xFFFFFFFFC0000000, resolves the object's undefined
 * symbols against the kernel's EXPORT_SYMBOL table (plus the exports of the
 * modules already loaded), applies the x86_64 relocations, and calls the
 * function registered with module_init().
 *
 * The region sits in the same top-2 GiB window as the kernel image, so module
 * code compiles with the ordinary -mcmodel=kernel model and every R_X86_64_32S
 * / PC32 / PLT32 relocation to a kernel symbol resolves in range.
 */

#ifndef B1NIX_MODULE_H
#define B1NIX_MODULE_H

#include <b1nix/types.h>
#include <b1nix/version.h>

/* ── Module virtual-address region ─────────────────────────────────────────
 * 0xFFFFFFFFC0000000 .. 0xFFFFFFFFC8000000 (128 MiB). Below the kernel image
 * (0xFFFFFFFF80000000) in the same PML4 slot 511 / PDPT slot 511, so the page
 * tables backing it are shared by every address space automatically. */
#define MODULE_REGION_BASE 0xFFFFFFFFC0000000ULL
#define MODULE_REGION_SIZE (128ULL * 1024ULL * 1024ULL)

/* Text is mapped RX, everything else RW+NX: W^X inside a module image. */
#define MODULE_PROT_RX 0x1
#define MODULE_PROT_RW 0x2

/* Reserve `size` bytes of module virtual address space, backed by fresh
 * physical frames, initially mapped RW+NX so the loader can write into it.
 * Page granular. Returns NULL when the region or physical memory is exhausted. */
void *module_alloc(usize size);
/* Re-protect a previously module_alloc'd range: MODULE_PROT_RX maps it
 * read-only + executable, MODULE_PROT_RW read-write + no-execute. `addr` must
 * be the value returned by module_alloc or a page-aligned address inside it. */
int module_set_prot(void *addr, usize size, int prot);
/* Release a module_alloc'd range (unmaps and frees the frames). */
void module_free(void *addr);
/* Bytes currently handed out by module_alloc (for /proc and diagnostics). */
usize module_alloc_used(void);
/* 1 when `addr` falls inside the module region. */
int module_region_contains(u64 addr);

/* ── EXPORT_SYMBOL ─────────────────────────────────────────────────────────
 * Kernel objects place one record per exported symbol into .ksymtab; the
 * linker script brackets the section with __ksymtab_start/__ksymtab_end.
 * Modules use the same macro — their records are collected from the loaded
 * section and published while the module is live. */
struct kernel_symbol {
  u64 value;
  const char *name;
};

#define EXPORT_SYMBOL(sym)                                                     \
  static const char __ksymname_##sym[]                                         \
      __attribute__((section(".ksymtab_strings"), used)) = #sym;               \
  static const struct kernel_symbol __ksymtab_##sym                            \
      __attribute__((section(".ksymtab"), used, aligned(8))) = {               \
          (u64)(usize)&sym, __ksymname_##sym}

/* ── Module metadata ───────────────────────────────────────────────────────
 * Every tag lands in .modinfo as a NUL-terminated "key=value" string. The
 * loader parses that section before it touches anything else, which is how a
 * .ko built for a different kernel is rejected (vermagic) and how modinfo(8)
 * reports a module it has not loaded. */
#define MODULE_VERMAGIC_STRING B1NIX_VERSION_STR " x86_64 SMP"

#define __MODULE_INFO_CAT2(a, b) a##b
#define __MODULE_INFO_CAT(a, b) __MODULE_INFO_CAT2(a, b)
#define MODULE_INFO(tag, value)                                                \
  static const char __MODULE_INFO_CAT(__modinfo_, __COUNTER__)[]               \
      __attribute__((section(".modinfo"), used, aligned(1))) = #tag "=" value

#define MODULE_LICENSE(v) MODULE_INFO(license, v)
#define MODULE_AUTHOR(v) MODULE_INFO(author, v)
#define MODULE_DESCRIPTION(v) MODULE_INFO(description, v)
#define MODULE_VERSION(v) MODULE_INFO(version, v)
/* An alias is an extra name request_module()/modprobe will match. */
#define MODULE_ALIAS(v) MODULE_INFO(alias, v)
/* Mandatory: the loader takes the module's identity from this tag. */
#define MODULE_NAME(v) MODULE_INFO(name, v)
/* Comma-separated module names this one needs loaded first, exactly the tag
 * depmod(8) reads to build modules.dep. It is declared rather than inferred so
 * the applet can regenerate the index from the .ko files alone — but it is not
 * trusted: tools/kernel/gen_modules_initramfs.sh recomputes the real
 * dependencies from the symbol graph and fails the build when the two
 * disagree, so a stale tag cannot ship. */
#define MODULE_DEPENDS(v) MODULE_INFO(depends, v)

/* Emitted by every module translation unit. The loader refuses any .ko whose
 * vermagic does not match the running kernel's, so a stale module can never be
 * relocated against a kernel whose symbols have moved. */
#ifdef MODULE
MODULE_INFO(vermagic, MODULE_VERMAGIC_STRING);
#endif

/* ── Init / exit ───────────────────────────────────────────────────────────
 * The pointers live in their own sections; the loader relocates them like any
 * other data and then calls through them. */
typedef int (*module_init_fn)(void);
typedef void (*module_exit_fn)(void);

#define module_init(fn)                                                        \
  static const module_init_fn __module_init_ptr                                \
      __attribute__((section(".module_init"), used, aligned(8))) = (fn)
#define module_exit(fn)                                                        \
  static const module_exit_fn __module_exit_ptr                                \
      __attribute__((section(".module_exit"), used, aligned(8))) = (fn)

/* ── Module parameters ─────────────────────────────────────────────────────
 * Exposed at /sys/module/<name>/parameters/<name>. `perm` uses the usual
 * octal file bits: 0444 read-only, 0644 also writable. */
enum module_param_type {
  MODULE_PARAM_INT = 0,
  MODULE_PARAM_UINT,
  MODULE_PARAM_LONG,
  MODULE_PARAM_ULONG,
  MODULE_PARAM_BOOL,
  MODULE_PARAM_STRING,
};

struct module_param_desc {
  const char *name;
  const char *description;
  u32 type;  /* enum module_param_type */
  u32 perm;  /* file mode bits */
  void *addr;
  u64 size;  /* byte size of the target object */
};

#define __module_param_named(pname, var, ptype, pperm, desc)                   \
  static const char __modparam_name_##var[]                                    \
      __attribute__((section(".modparam_strings"), used)) = #pname;            \
  static const char __modparam_desc_##var[]                                    \
      __attribute__((section(".modparam_strings"), used)) = desc;              \
  static const struct module_param_desc __modparam_##var                       \
      __attribute__((section(".modparam"), used, aligned(8))) = {              \
          __modparam_name_##var, __modparam_desc_##var, (ptype), (pperm),      \
          &(var), sizeof(var)}

#define module_param(var, ptype, pperm)                                        \
  __module_param_named(var, var, ptype, pperm, "")
#define module_param_desc(var, ptype, pperm, desc)                             \
  __module_param_named(var, var, ptype, pperm, desc)

/* ── struct module ─────────────────────────────────────────────────────── */
#define MODULE_NAME_MAX 48
#define MODULE_DEPS_MAX 8

enum module_state {
  MODULE_STATE_LOADING = 0,
  MODULE_STATE_LIVE,
  MODULE_STATE_UNLOADING,
};

struct module {
  char name[MODULE_NAME_MAX];
  int state;
  int refcnt; /* users holding try_module_get() */

  void *core;      /* module_alloc base of the whole image */
  usize core_size; /* bytes reserved */
  void *text;      /* start of the RX span (== core) */
  usize text_size;

  module_init_fn init;
  module_exit_fn exit;

  const struct kernel_symbol *syms;
  usize num_syms;

  const struct module_param_desc *params;
  usize num_params;

  const char *modinfo;  /* .modinfo blob (inside the image) */
  usize modinfo_size;

  /* Modules this one resolved symbols from; each holds a reference. */
  struct module *deps[MODULE_DEPS_MAX];
  usize num_deps;

  struct module *next;
};

/* ── Loader / syscall backends ─────────────────────────────────────────── */
/* Load a module image already in kernel memory. `params` is the (possibly
 * empty) whitespace-separated "key=value" string from init_module(2). Returns
 * 0 or a negative errno. */
int module_load_image(const void *image, usize size, const char *params);
/* Read a .ko from the VFS and load it. */
int module_load_path(const char *path, const char *params);
/* Unload by name. flags carries O_NONBLOCK/O_TRUNC as Linux's delete_module. */
int module_unload(const char *name, u32 flags);

struct module *module_find(const char *name);
/* Owner of an address inside a loaded module image, or NULL when the address
 * belongs to the kernel itself. */
struct module *module_owner_of(const void *addr);
/* Reference counting: try_module_get fails (returns 0) once a module has begun
 * unloading; a module with a non-zero refcnt cannot be removed. */
int try_module_get(struct module *mod);
void module_put(struct module *mod);

/* In-kernel "load this if it is not there yet". Resolves aliases and the
 * dependency list from /lib/modules/modules.dep + modules.alias, so a caller
 * only ever names the capability it wants. Returns 0 when the module is live. */
int request_module(const char *name);

/* Resolve an exported symbol (kernel first, then loaded modules). Returns 0
 * when unknown; *owner is set to the providing module (NULL = kernel). */
u64 module_symbol_lookup(const char *name, struct module **owner);

/* /proc/modules body. Writes at most `cap` bytes, returns the length. */
int module_proc_render(char *buf, usize cap);

/* Called from sysfs_init's mount callback so /sys/module reflects whatever is
 * loaded at mount time; later loads/unloads update the tree directly. */
struct vfs_node;
void module_sysfs_attach_root(struct vfs_node *sys_root);

/* Boot-time bring-up: loads the modules the kernel itself needs (filesystems,
 * the sound driver, the IPv6 stack) from the initramfs. */
void module_init_builtin_deps(void);

#endif /* B1NIX_MODULE_H */
