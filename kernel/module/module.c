/* Loadable kernel module framework (M95/M96).
 *
 * Loads relocatable ELF objects (ET_REL) into the module region, resolves
 * their undefined symbols against the kernel's EXPORT_SYMBOL table and the
 * exports of already-loaded modules, applies the x86_64 relocations, and runs
 * the module's init function.
 *
 * Layout inside a module image:
 *
 *   [ text pages           ]  RX  (SHF_ALLOC | SHF_EXECINSTR)
 *   [ rodata / data / bss  ]  RW+NX (everything else that is SHF_ALLOC)
 *
 * W^X is real: the text span is re-protected read-only+executable once every
 * relocation has been applied, and the data span keeps the NX bit for its
 * whole lifetime.
 */

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/module.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── ELF64 relocatable-object structures ─────────────────────────────────── */

struct mod_ehdr {
  u8 e_ident[16];
  u16 e_type;
  u16 e_machine;
  u32 e_version;
  u64 e_entry;
  u64 e_phoff;
  u64 e_shoff;
  u32 e_flags;
  u16 e_ehsize;
  u16 e_phentsize;
  u16 e_phnum;
  u16 e_shentsize;
  u16 e_shnum;
  u16 e_shstrndx;
} __attribute__((packed));

struct mod_shdr {
  u32 sh_name;
  u32 sh_type;
  u64 sh_flags;
  u64 sh_addr;
  u64 sh_offset;
  u64 sh_size;
  u32 sh_link;
  u32 sh_info;
  u64 sh_addralign;
  u64 sh_entsize;
} __attribute__((packed));

struct mod_sym {
  u32 st_name;
  u8 st_info;
  u8 st_other;
  u16 st_shndx;
  u64 st_value;
  u64 st_size;
} __attribute__((packed));

struct mod_rela {
  u64 r_offset;
  u64 r_info;
  i64 r_addend;
} __attribute__((packed));

#define ELF_ST_TYPE(i) ((i) & 0xf)
#define ELF64_R_SYM(i) ((u32)((i) >> 32))
#define ELF64_R_TYPE(i) ((u32)((i) & 0xffffffffu))

#define ET_REL 1
#define EM_X86_64 62

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8

#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4

#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2

#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_PLT32 4
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_16 12
#define R_X86_64_PC16 13
#define R_X86_64_8 14
#define R_X86_64_PC8 15
#define R_X86_64_PC64 24
#define R_X86_64_GOTPCRELX 41
#define R_X86_64_REX_GOTPCRELX 42

/* Section names the loader treats specially. */
#define SEC_KSYMTAB ".ksymtab"
#define SEC_MODPARAM ".modparam"
#define SEC_MODINFO ".modinfo"
#define SEC_MODULE_INIT ".module_init"
#define SEC_MODULE_EXIT ".module_exit"

/* Kernel-side EXPORT_SYMBOL table, bracketed by the linker script. */
extern const struct kernel_symbol __ksymtab_start[];
extern const struct kernel_symbol __ksymtab_end[];

static struct module *module_list;
static spinlock_t module_lock = SPINLOCK_INIT;
static struct vfs_node *module_sysfs_root; /* /sys/module */

/* Forward declarations for the sysfs half (bottom of this file). */
static void module_sysfs_add(struct module *mod);
static void module_sysfs_remove(struct module *mod);
static int module_apply_params(struct module *mod, const char *params);

/* ── small helpers ───────────────────────────────────────────────────────── */

static int mod_isspace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Parse a signed decimal (or 0x-prefixed hex) integer. Returns 0 on success. */
static int mod_parse_i64(const char *s, usize len, i64 *out) {
  usize i = 0;
  int neg = 0;
  if (len == 0)
    return -1;
  if (s[0] == '-') {
    neg = 1;
    i = 1;
  } else if (s[0] == '+') {
    i = 1;
  }
  u64 base = 10;
  if (len - i > 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
    base = 16;
    i += 2;
  }
  if (i >= len)
    return -1;
  u64 v = 0;
  for (; i < len; i++) {
    char c = s[i];
    u64 d;
    if (c >= '0' && c <= '9')
      d = (u64)(c - '0');
    else if (base == 16 && c >= 'a' && c <= 'f')
      d = (u64)(c - 'a' + 10);
    else if (base == 16 && c >= 'A' && c <= 'F')
      d = (u64)(c - 'A' + 10);
    else
      return -1;
    if (d >= base)
      return -1;
    v = v * base + d;
  }
  *out = neg ? -(i64)v : (i64)v;
  return 0;
}

/* Iterate the NUL-separated "key=value" records of a .modinfo blob. Returns a
 * pointer to the value of the first record whose key matches, or NULL. */
static const char *modinfo_get(const char *blob, usize size, const char *key,
                               usize skip) {
  usize klen = strlen(key);
  usize pos = 0;
  usize seen = 0;
  while (pos < size) {
    const char *rec = blob + pos;
    usize rlen = 0;
    while (pos + rlen < size && rec[rlen] != '\0')
      rlen++;
    if (rlen > klen && rec[klen] == '=' && strncmp(rec, key, klen) == 0) {
      if (seen == skip)
        return rec + klen + 1;
      seen++;
    }
    pos += rlen + 1;
  }
  return 0;
}

/* ── symbol resolution ───────────────────────────────────────────────────── */

u64 module_symbol_lookup(const char *name, struct module **owner) {
  if (owner)
    *owner = 0;
  for (const struct kernel_symbol *k = __ksymtab_start; k < __ksymtab_end; k++) {
    if (k->name && strcmp(k->name, name) == 0)
      return k->value;
  }
  for (struct module *m = module_list; m; m = m->next) {
    if (m->state == MODULE_STATE_UNLOADING)
      continue;
    for (usize i = 0; i < m->num_syms; i++) {
      if (m->syms[i].name && strcmp(m->syms[i].name, name) == 0) {
        if (owner)
          *owner = m;
        return m->syms[i].value;
      }
    }
  }
  return 0;
}

struct module *module_find(const char *name) {
  for (struct module *m = module_list; m; m = m->next) {
    if (strcmp(m->name, name) == 0)
      return m;
  }
  return 0;
}

/* The module whose image contains `addr`, or NULL for a built-in address. Lets
 * a registry (the VFS filesystem list) discover the owner of a structure that
 * was handed to it, without every module having to name itself. */
struct module *module_owner_of(const void *addr) {
  if (!addr)
    return 0;
  usize a = (usize)addr;
  u64 flags;
  spin_lock_irqsave(&module_lock, &flags);
  struct module *found = 0;
  for (struct module *m = module_list; m; m = m->next) {
    usize base = (usize)m->core;
    if (a >= base && a < base + m->core_size) {
      found = m;
      break;
    }
  }
  spin_unlock_irqrestore(&module_lock, flags);
  return found;
}

int try_module_get(struct module *mod) {
  if (!mod)
    return 0;
  u64 flags;
  spin_lock_irqsave(&module_lock, &flags);
  int ok = 0;
  if (mod->state != MODULE_STATE_UNLOADING) {
    mod->refcnt++;
    ok = 1;
  }
  spin_unlock_irqrestore(&module_lock, flags);
  return ok;
}

void module_put(struct module *mod) {
  if (!mod)
    return;
  u64 flags;
  spin_lock_irqsave(&module_lock, &flags);
  if (mod->refcnt > 0)
    mod->refcnt--;
  spin_unlock_irqrestore(&module_lock, flags);
}

/* ── loader ──────────────────────────────────────────────────────────────── */

struct mod_load_ctx {
  const u8 *img;
  usize size;
  const struct mod_ehdr *eh;
  const struct mod_shdr *sh;
  const char *shstr;
  u64 *sec_addr; /* final VA of each allocated section (0 when not allocated) */
  u64 *symval;   /* resolved value of each symtab entry */
  usize nsyms;
};

static const char *sec_name(const struct mod_load_ctx *c, usize i) {
  return c->shstr + c->sh[i].sh_name;
}

static int reloc_fits_i32(u64 v) {
  i64 s = (i64)v;
  return s >= -2147483648LL && s <= 2147483647LL;
}

static int module_apply_relocs(struct mod_load_ctx *c, struct module *mod) {
  (void)mod;
  for (usize i = 0; i < c->eh->e_shnum; i++) {
    if (c->sh[i].sh_type != SHT_RELA)
      continue;
    usize target = c->sh[i].sh_info;
    if (target >= c->eh->e_shnum || c->sec_addr[target] == 0)
      continue; /* relocations for a non-allocated section (debug info) */
    usize count = c->sh[i].sh_entsize
                      ? (usize)(c->sh[i].sh_size / c->sh[i].sh_entsize)
                      : 0;
    const struct mod_rela *r =
        (const struct mod_rela *)(c->img + c->sh[i].sh_offset);
    for (usize j = 0; j < count; j++) {
      u32 symidx = ELF64_R_SYM(r[j].r_info);
      u32 type = ELF64_R_TYPE(r[j].r_info);
      if (symidx >= c->nsyms)
        return -EINVAL;
      u64 S = c->symval[symidx];
      i64 A = r[j].r_addend;
      u64 P = c->sec_addr[target] + r[j].r_offset;
      if (r[j].r_offset >= c->sh[target].sh_size)
        return -EINVAL;
      void *loc = (void *)(usize)P;
      switch (type) {
      case R_X86_64_NONE:
        break;
      case R_X86_64_64:
        *(u64 *)loc = S + (u64)A;
        break;
      case R_X86_64_PC64:
        *(u64 *)loc = S + (u64)A - P;
        break;
      case R_X86_64_PC32:
      case R_X86_64_PLT32:
      case R_X86_64_GOTPCRELX:
      case R_X86_64_REX_GOTPCRELX: {
        u64 v = S + (u64)A - P;
        if (!reloc_fits_i32(v))
          return -EOVERFLOW;
        *(u32 *)loc = (u32)v;
        break;
      }
      case R_X86_64_32: {
        u64 v = S + (u64)A;
        if (v > 0xffffffffULL)
          return -EOVERFLOW;
        *(u32 *)loc = (u32)v;
        break;
      }
      case R_X86_64_32S: {
        u64 v = S + (u64)A;
        if (!reloc_fits_i32(v))
          return -EOVERFLOW;
        *(u32 *)loc = (u32)v;
        break;
      }
      default: {
        char buf[80];
        snprintf(buf, sizeof(buf), "module: unsupported relocation type %u\n",
                 (unsigned)type);
        console_write(buf);
        return -ENOEXEC;
      }
      }
    }
  }
  return 0;
}

/* Resolve every symbol of the object, recording which already-loaded modules
 * supplied undefined ones (those become dependencies and hold a reference). */
static int module_resolve_syms(struct mod_load_ctx *c, struct module *mod,
                               usize symtab_idx) {
  const struct mod_shdr *symsh = &c->sh[symtab_idx];
  const struct mod_sym *syms = (const struct mod_sym *)(c->img + symsh->sh_offset);
  const char *strtab = (const char *)(c->img + c->sh[symsh->sh_link].sh_offset);

  for (usize i = 0; i < c->nsyms; i++) {
    const struct mod_sym *s = &syms[i];
    const char *name = strtab + s->st_name;
    switch (s->st_shndx) {
    case SHN_UNDEF: {
      if (name[0] == '\0') {
        c->symval[i] = 0;
        break;
      }
      struct module *owner = 0;
      u64 v = module_symbol_lookup(name, &owner);
      if (!v) {
        char buf[128];
        snprintf(buf, sizeof(buf), "module: unresolved symbol %s\n", name);
        console_write(buf);
        return -ENOENT;
      }
      c->symval[i] = v;
      if (owner && owner != mod) {
        int have = 0;
        for (usize d = 0; d < mod->num_deps; d++)
          if (mod->deps[d] == owner)
            have = 1;
        if (!have) {
          if (mod->num_deps >= MODULE_DEPS_MAX)
            return -ENOSPC;
          if (!try_module_get(owner))
            return -EBUSY;
          mod->deps[mod->num_deps++] = owner;
        }
      }
      break;
    }
    case SHN_ABS:
      c->symval[i] = s->st_value;
      break;
    case SHN_COMMON:
      /* Kernel objects are built with -fno-common. */
      return -ENOEXEC;
    default:
      if (s->st_shndx >= c->eh->e_shnum)
        return -EINVAL;
      c->symval[i] = c->sec_addr[s->st_shndx] + s->st_value;
      break;
    }
  }
  return 0;
}

/* Find an allocated section by name; returns its VA and size. */
static u64 module_section(struct mod_load_ctx *c, const char *name,
                          usize *out_size) {
  for (usize i = 0; i < c->eh->e_shnum; i++) {
    if (strcmp(sec_name(c, i), name) == 0 && c->sec_addr[i]) {
      if (out_size)
        *out_size = (usize)c->sh[i].sh_size;
      return c->sec_addr[i];
    }
  }
  if (out_size)
    *out_size = 0;
  return 0;
}

static void module_release(struct module *mod) {
  for (usize i = 0; i < mod->num_deps; i++)
    module_put(mod->deps[i]);
  mod->num_deps = 0;
  if (mod->core)
    module_free(mod->core);
  mod->core = 0;
  kfree(mod);
}

int module_load_image(const void *image, usize size, const char *params) {
  if (!image || size < sizeof(struct mod_ehdr))
    return -ENOEXEC;

  const u8 *img = (const u8 *)image;
  const struct mod_ehdr *eh = (const struct mod_ehdr *)img;
  if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' ||
      eh->e_ident[3] != 'F')
    return -ENOEXEC;
  if (eh->e_ident[4] != 2 /* ELFCLASS64 */ || eh->e_type != ET_REL ||
      eh->e_machine != EM_X86_64)
    return -ENOEXEC;
  if (eh->e_shoff == 0 || eh->e_shnum == 0 ||
      eh->e_shoff + (u64)eh->e_shnum * sizeof(struct mod_shdr) > size)
    return -ENOEXEC;

  const struct mod_shdr *sh = (const struct mod_shdr *)(img + eh->e_shoff);
  if (eh->e_shstrndx >= eh->e_shnum)
    return -ENOEXEC;
  const char *shstr = (const char *)(img + sh[eh->e_shstrndx].sh_offset);

  /* Bounds-check every section before anything is trusted. */
  for (usize i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type == SHT_NOBITS)
      continue;
    if (sh[i].sh_offset + sh[i].sh_size > size)
      return -ENOEXEC;
  }

  struct mod_load_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.img = img;
  ctx.size = size;
  ctx.eh = eh;
  ctx.sh = sh;
  ctx.shstr = shstr;

  /* --- vermagic + name, straight out of the file's .modinfo --- */
  const char *fmodinfo = 0;
  usize fmodinfo_size = 0;
  for (usize i = 0; i < eh->e_shnum; i++) {
    if (strcmp(shstr + sh[i].sh_name, SEC_MODINFO) == 0) {
      fmodinfo = (const char *)(img + sh[i].sh_offset);
      fmodinfo_size = (usize)sh[i].sh_size;
      break;
    }
  }
  if (!fmodinfo)
    return -ENOEXEC;
  const char *vermagic = modinfo_get(fmodinfo, fmodinfo_size, "vermagic", 0);
  if (!vermagic || strcmp(vermagic, MODULE_VERMAGIC_STRING) != 0) {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "module: version magic '%s' should be '%s'\n",
             vermagic ? vermagic : "(none)", MODULE_VERMAGIC_STRING);
    console_write(buf);
    return -ENOEXEC;
  }
  const char *modname = modinfo_get(fmodinfo, fmodinfo_size, "name", 0);
  if (!modname || modname[0] == '\0')
    return -ENOEXEC;
  if (module_find(modname))
    return -EEXIST;

  /* --- layout: text first (page aligned), then rodata/data/bss --- */
  ctx.sec_addr = kzalloc(sizeof(u64) * eh->e_shnum);
  if (!ctx.sec_addr)
    return -ENOMEM;

  u64 text_size = 0;
  for (usize i = 0; i < eh->e_shnum; i++) {
    if (!(sh[i].sh_flags & SHF_ALLOC) || !(sh[i].sh_flags & SHF_EXECINSTR))
      continue;
    u64 align = sh[i].sh_addralign ? sh[i].sh_addralign : 1;
    text_size = (text_size + align - 1) & ~(align - 1);
    ctx.sec_addr[i] = text_size; /* offset for now */
    text_size += sh[i].sh_size;
  }
  u64 text_span = (text_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  u64 total = text_span;
  for (usize i = 0; i < eh->e_shnum; i++) {
    if (!(sh[i].sh_flags & SHF_ALLOC) || (sh[i].sh_flags & SHF_EXECINSTR))
      continue;
    u64 align = sh[i].sh_addralign ? sh[i].sh_addralign : 1;
    total = (total + align - 1) & ~(align - 1);
    ctx.sec_addr[i] = total;
    total += sh[i].sh_size;
  }
  if (total == 0) {
    kfree(ctx.sec_addr);
    return -ENOEXEC;
  }

  struct module *mod = kzalloc(sizeof(*mod));
  if (!mod) {
    kfree(ctx.sec_addr);
    return -ENOMEM;
  }
  strncpy(mod->name, modname, MODULE_NAME_MAX - 1);
  mod->state = MODULE_STATE_LOADING;

  mod->core = module_alloc((usize)total);
  if (!mod->core) {
    kfree(mod);
    kfree(ctx.sec_addr);
    return -ENOMEM;
  }
  mod->core_size = (usize)total;
  mod->text = mod->core;
  mod->text_size = (usize)text_span;

  u64 base = (u64)(usize)mod->core;
  for (usize i = 0; i < eh->e_shnum; i++) {
    if (!(sh[i].sh_flags & SHF_ALLOC))
      continue;
    ctx.sec_addr[i] += base;
    if (sh[i].sh_type != SHT_NOBITS && sh[i].sh_size)
      memcpy((void *)(usize)ctx.sec_addr[i], img + sh[i].sh_offset,
             (usize)sh[i].sh_size);
  }

  /* --- symbols --- */
  usize symtab_idx = 0;
  for (usize i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type == SHT_SYMTAB) {
      symtab_idx = i;
      break;
    }
  }
  int rc;
  if (symtab_idx == 0 || sh[symtab_idx].sh_entsize == 0) {
    rc = -ENOEXEC;
    goto fail;
  }
  ctx.nsyms = (usize)(sh[symtab_idx].sh_size / sh[symtab_idx].sh_entsize);
  ctx.symval = kzalloc(sizeof(u64) * (ctx.nsyms ? ctx.nsyms : 1));
  if (!ctx.symval) {
    rc = -ENOMEM;
    goto fail;
  }

  rc = module_resolve_syms(&ctx, mod, symtab_idx);
  if (rc != 0)
    goto fail;

  rc = module_apply_relocs(&ctx, mod);
  if (rc != 0)
    goto fail;

  /* --- collect the module's own metadata sections --- */
  usize ksz = 0;
  u64 ksym = module_section(&ctx, SEC_KSYMTAB, &ksz);
  if (ksym) {
    mod->syms = (const struct kernel_symbol *)(usize)ksym;
    mod->num_syms = ksz / sizeof(struct kernel_symbol);
  }
  usize psz = 0;
  u64 par = module_section(&ctx, SEC_MODPARAM, &psz);
  if (par) {
    mod->params = (const struct module_param_desc *)(usize)par;
    mod->num_params = psz / sizeof(struct module_param_desc);
  }
  usize misz = 0;
  u64 mi = module_section(&ctx, SEC_MODINFO, &misz);
  mod->modinfo = (const char *)(usize)mi;
  mod->modinfo_size = misz;

  usize isz = 0;
  u64 initp = module_section(&ctx, SEC_MODULE_INIT, &isz);
  if (initp && isz >= sizeof(module_init_fn))
    mod->init = *(module_init_fn *)(usize)initp;
  usize esz = 0;
  u64 exitp = module_section(&ctx, SEC_MODULE_EXIT, &esz);
  if (exitp && esz >= sizeof(module_exit_fn))
    mod->exit = *(module_exit_fn *)(usize)exitp;

  /* --- parameters supplied on the insmod command line --- */
  rc = module_apply_params(mod, params);
  if (rc != 0)
    goto fail;

  /* --- W^X: text becomes read-only + executable, data stays NX --- */
  if (mod->text_size)
    module_set_prot(mod->text, mod->text_size, MODULE_PROT_RX);

  /* Publish before init so a module that calls request_module() or looks
   * itself up during init sees a consistent list. */
  u64 flags;
  spin_lock_irqsave(&module_lock, &flags);
  mod->next = module_list;
  module_list = mod;
  spin_unlock_irqrestore(&module_lock, flags);

  if (mod->init) {
    int irc = mod->init();
    if (irc != 0) {
      spin_lock_irqsave(&module_lock, &flags);
      struct module **pp = &module_list;
      while (*pp && *pp != mod)
        pp = &(*pp)->next;
      if (*pp)
        *pp = mod->next;
      spin_unlock_irqrestore(&module_lock, flags);
      /* Text must be writable again before the pages go back to the pmm. */
      module_set_prot(mod->text, mod->text_size, MODULE_PROT_RW);
      rc = irc;
      goto fail_unlinked;
    }
  }

  mod->state = MODULE_STATE_LIVE;
  module_sysfs_add(mod);

  kfree(ctx.symval);
  kfree(ctx.sec_addr);

  {
    char buf[96];
    snprintf(buf, sizeof(buf), "module: loaded %s at 0x%lx (%lu bytes)\n",
             mod->name, (unsigned long)base, (unsigned long)mod->core_size);
    console_write(buf);
  }
  return 0;

fail:
  if (mod->text_size)
    module_set_prot(mod->text, mod->text_size, MODULE_PROT_RW);
fail_unlinked:
  if (ctx.symval)
    kfree(ctx.symval);
  kfree(ctx.sec_addr);
  module_release(mod);
  return rc;
}

/* Read a whole file from the VFS into the kernel heap. */
static int module_read_file(const char *path, char **out_data, usize *out_size) {
  struct vfs_node *node = vfs_find_node(path);
  if (!node || IS_ERR(node))
    return -ENOENT;
  if (node->inode->type != VFS_FILE || node->inode->size == 0) {
    vfs_node_put(node);
    return -ENOENT;
  }
  usize file_size = node->inode->size;

  /* Fast path for a file whose contents are already a kernel buffer — which is
   * exactly the initramfs, where the boot-time modules live. Avoids needing a
   * file-descriptor table this early in kernel_main. */
  if (node->inode->data && !node->inode->read_cb) {
    char *copy = kmalloc(file_size);
    if (!copy) {
      vfs_node_put(node);
      return -ENOMEM;
    }
    memcpy(copy, node->inode->data, file_size);
    vfs_node_put(node);
    *out_data = copy;
    *out_size = file_size;
    return 0;
  }
  vfs_node_put(node);

  int fd = vfs_open(path);
  if (fd < 0)
    return -ENOENT;
  char *data = kmalloc(file_size);
  if (!data) {
    vfs_close(fd);
    return -ENOMEM;
  }
  usize total = 0;
  while (total < file_size) {
    isize got = vfs_read(fd, data + total, file_size - total);
    if (got < 0) {
      if (got == -EAGAIN || got == -EWOULDBLOCK) {
        scheduler_yield();
        continue;
      }
      vfs_close(fd);
      kfree(data);
      return -EIO;
    }
    if (got == 0)
      break;
    total += (usize)got;
  }
  vfs_close(fd);
  if (total != file_size) {
    kfree(data);
    return -EIO;
  }
  *out_data = data;
  *out_size = file_size;
  return 0;
}

int module_load_path(const char *path, const char *params) {
  char *data = 0;
  usize size = 0;
  int rc = module_read_file(path, &data, &size);
  if (rc != 0)
    return rc;
  rc = module_load_image(data, size, params);
  kfree(data);
  return rc;
}

int module_unload(const char *name, u32 flags) {
  (void)flags;
  u64 irq;
  spin_lock_irqsave(&module_lock, &irq);
  struct module *mod = 0;
  for (struct module *m = module_list; m; m = m->next) {
    if (strcmp(m->name, name) == 0) {
      mod = m;
      break;
    }
  }
  if (!mod) {
    spin_unlock_irqrestore(&module_lock, irq);
    return -ENOENT;
  }
  if (mod->refcnt > 0 || mod->state != MODULE_STATE_LIVE) {
    spin_unlock_irqrestore(&module_lock, irq);
    return -EBUSY;
  }
  mod->state = MODULE_STATE_UNLOADING;
  spin_unlock_irqrestore(&module_lock, irq);

  if (mod->exit)
    mod->exit();

  module_sysfs_remove(mod);

  spin_lock_irqsave(&module_lock, &irq);
  struct module **pp = &module_list;
  while (*pp && *pp != mod)
    pp = &(*pp)->next;
  if (*pp)
    *pp = mod->next;
  spin_unlock_irqrestore(&module_lock, irq);

  if (mod->text_size)
    module_set_prot(mod->text, mod->text_size, MODULE_PROT_RW);
  module_release(mod);
  return 0;
}

/* ── module parameters ───────────────────────────────────────────────────── */

static const struct module_param_desc *module_param_find(struct module *mod,
                                                         const char *name,
                                                         usize len) {
  for (usize i = 0; i < mod->num_params; i++) {
    const char *n = mod->params[i].name;
    if (n && strlen(n) == len && strncmp(n, name, len) == 0)
      return &mod->params[i];
  }
  return 0;
}

/* Store `value` (length `len`) into the parameter. Returns 0 or -errno. */
static int module_param_store(const struct module_param_desc *p,
                              const char *value, usize len) {
  i64 v = 0;
  switch (p->type) {
  case MODULE_PARAM_INT:
    if (mod_parse_i64(value, len, &v) != 0)
      return -EINVAL;
    *(int *)p->addr = (int)v;
    return 0;
  case MODULE_PARAM_UINT:
    if (mod_parse_i64(value, len, &v) != 0 || v < 0)
      return -EINVAL;
    *(unsigned int *)p->addr = (unsigned int)v;
    return 0;
  case MODULE_PARAM_LONG:
    if (mod_parse_i64(value, len, &v) != 0)
      return -EINVAL;
    *(i64 *)p->addr = v;
    return 0;
  case MODULE_PARAM_ULONG:
    if (mod_parse_i64(value, len, &v) != 0 || v < 0)
      return -EINVAL;
    *(u64 *)p->addr = (u64)v;
    return 0;
  case MODULE_PARAM_BOOL:
    if (len == 1 && (value[0] == '1' || value[0] == 'y' || value[0] == 'Y')) {
      *(int *)p->addr = 1;
      return 0;
    }
    if (len == 1 && (value[0] == '0' || value[0] == 'n' || value[0] == 'N')) {
      *(int *)p->addr = 0;
      return 0;
    }
    if (len == 4 && strncmp(value, "true", 4) == 0) {
      *(int *)p->addr = 1;
      return 0;
    }
    if (len == 5 && strncmp(value, "false", 5) == 0) {
      *(int *)p->addr = 0;
      return 0;
    }
    return -EINVAL;
  case MODULE_PARAM_STRING: {
    if (len + 1 > p->size)
      return -ERANGE;
    char *dst = (char *)p->addr;
    memcpy(dst, value, len);
    dst[len] = '\0';
    return 0;
  }
  default:
    return -EINVAL;
  }
}

static int module_param_show(const struct module_param_desc *p, char *buf,
                             usize cap) {
  switch (p->type) {
  case MODULE_PARAM_INT:
    return snprintf(buf, cap, "%d\n", *(int *)p->addr);
  case MODULE_PARAM_UINT:
    return snprintf(buf, cap, "%u\n", *(unsigned int *)p->addr);
  case MODULE_PARAM_LONG:
    return snprintf(buf, cap, "%ld\n", (long)*(i64 *)p->addr);
  case MODULE_PARAM_ULONG:
    return snprintf(buf, cap, "%lu\n", (unsigned long)*(u64 *)p->addr);
  case MODULE_PARAM_BOOL:
    return snprintf(buf, cap, "%c\n", *(int *)p->addr ? 'Y' : 'N');
  case MODULE_PARAM_STRING:
    return snprintf(buf, cap, "%s\n", (const char *)p->addr);
  default:
    return snprintf(buf, cap, "\n");
  }
}

/* Apply a whitespace-separated "key=value" list. Unknown keys are an error,
 * exactly as Linux's insmod reports one. */
static int module_apply_params(struct module *mod, const char *params) {
  if (!params)
    return 0;
  const char *p = params;
  while (*p) {
    while (*p && mod_isspace(*p))
      p++;
    if (!*p)
      break;
    const char *key = p;
    while (*p && *p != '=' && !mod_isspace(*p))
      p++;
    usize klen = (usize)(p - key);
    if (*p != '=')
      return -EINVAL;
    p++;
    const char *val = p;
    while (*p && !mod_isspace(*p))
      p++;
    usize vlen = (usize)(p - val);
    const struct module_param_desc *desc = module_param_find(mod, key, klen);
    if (!desc)
      return -EINVAL;
    int rc = module_param_store(desc, val, vlen);
    if (rc != 0)
      return rc;
  }
  return 0;
}

/* ── /proc/modules ───────────────────────────────────────────────────────── */

int module_proc_render(char *buf, usize cap) {
  usize off = 0;
  for (struct module *m = module_list; m; m = m->next) {
    char deps[160];
    usize dlen = 0;
    deps[0] = '\0';
    for (usize i = 0; i < m->num_deps; i++) {
      int n = snprintf(deps + dlen, sizeof(deps) - dlen, "%s%s",
                       dlen ? "," : "", m->deps[i]->name);
      if (n < 0)
        break;
      dlen += (usize)n;
      if (dlen >= sizeof(deps) - 1)
        break;
    }
    int n = snprintf(buf + off, cap - off, "%s %lu %d %s %s 0x%lx\n", m->name,
                     (unsigned long)m->core_size, m->refcnt,
                     dlen ? deps : "-",
                     m->state == MODULE_STATE_LIVE ? "Live" : "Loading",
                     (unsigned long)(usize)m->core);
    if (n < 0)
      break;
    off += (usize)n;
    if (off + 128 >= cap)
      break;
  }
  return (int)off;
}

/* ── /sys/module/<name>/… ────────────────────────────────────────────────── */

struct module_sysfs_file {
  struct module *mod;
  const struct module_param_desc *param; /* NULL for refcnt/initstate */
  int kind; /* 0 param, 1 refcnt, 2 initstate, 3 coresize */
};

static isize module_sysfs_read(struct vfs_node *node, u64 offset, char *buffer,
                               usize size, int flags) {
  (void)flags;
  struct module_sysfs_file *f = (struct module_sysfs_file *)node->inode->data;
  if (!f)
    return 0;
  char tmp[256];
  int len;
  switch (f->kind) {
  case 1:
    len = snprintf(tmp, sizeof(tmp), "%d\n", f->mod->refcnt);
    break;
  case 2:
    len = snprintf(tmp, sizeof(tmp), "%s\n",
                   f->mod->state == MODULE_STATE_LIVE ? "live" : "coming");
    break;
  case 3:
    len = snprintf(tmp, sizeof(tmp), "%lu\n", (unsigned long)f->mod->core_size);
    break;
  default:
    len = module_param_show(f->param, tmp, sizeof(tmp));
    break;
  }
  if (len < 0)
    return 0;
  if (offset >= (u64)len)
    return 0;
  usize avail = (usize)len - (usize)offset;
  usize n = avail < size ? avail : size;
  memcpy(buffer, tmp + (usize)offset, n);
  return (isize)n;
}

static isize module_sysfs_write(struct vfs_node *node, u64 offset,
                                const char *buffer, usize size, int flags) {
  (void)offset;
  (void)flags;
  struct module_sysfs_file *f = (struct module_sysfs_file *)node->inode->data;
  if (!f || f->kind != 0 || !f->param)
    return -EACCES;
  if (!(f->param->perm & 0200))
    return -EACCES;
  usize len = size;
  while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r'))
    len--;
  int rc = module_param_store(f->param, buffer, len);
  if (rc != 0)
    return rc;
  return (isize)size;
}

static struct vfs_node *module_sysfs_mkdir(struct vfs_node *parent,
                                           const char *name) {
  struct vfs_node *n = vfs_create_node(VFS_DIRECTORY);
  if (!n)
    return 0;
  strncpy(n->name, name, sizeof(n->name) - 1);
  n->name[sizeof(n->name) - 1] = '\0';
  n->inode->mode = 0555;
  n->inode->nlink = 2;
  n->parent = parent;
  n->refcount++;
  vfs_attach_child(parent, n);
  return n;
}

static void module_sysfs_mkfile(struct vfs_node *parent, const char *name,
                                struct module *mod,
                                const struct module_param_desc *param,
                                int kind, u16 mode) {
  struct vfs_node *n = vfs_create_node(VFS_DEVICE);
  if (!n)
    return;
  strncpy(n->name, name, sizeof(n->name) - 1);
  n->name[sizeof(n->name) - 1] = '\0';
  n->inode->mode = mode;
  n->inode->nlink = 1;
  struct module_sysfs_file *f = kzalloc(sizeof(*f));
  if (!f) {
    vfs_node_put(n);
    return;
  }
  f->mod = mod;
  f->param = param;
  f->kind = kind;
  n->inode->data = f;
  n->inode->read_cb = module_sysfs_read;
  if (mode & 0200)
    n->inode->write_cb = module_sysfs_write;
  n->parent = parent;
  n->refcount++;
  vfs_attach_child(parent, n);
}

static void module_sysfs_add(struct module *mod) {
  if (!module_sysfs_root)
    return;
  struct vfs_node *d = module_sysfs_mkdir(module_sysfs_root, mod->name);
  if (!d)
    return;
  module_sysfs_mkfile(d, "refcnt", mod, 0, 1, 0444);
  module_sysfs_mkfile(d, "initstate", mod, 0, 2, 0444);
  module_sysfs_mkfile(d, "coresize", mod, 0, 3, 0444);
  if (mod->num_params) {
    struct vfs_node *pd = module_sysfs_mkdir(d, "parameters");
    if (pd) {
      for (usize i = 0; i < mod->num_params; i++) {
        const struct module_param_desc *p = &mod->params[i];
        u16 mode = (u16)(p->perm & 0666);
        if (mode == 0)
          mode = 0444;
        module_sysfs_mkfile(pd, p->name, mod, p, 0, mode);
      }
    }
  }
}

static void module_sysfs_remove(struct module *mod) {
  if (!module_sysfs_root)
    return;
  struct vfs_node *d = 0;
  for (struct vfs_node *c = module_sysfs_root->first_child; c;
       c = c->next_sibling) {
    if (strcmp(c->name, mod->name) == 0) {
      d = c;
      break;
    }
  }
  if (!d)
    return;
  vfs_detach_child(module_sysfs_root, d);
  /* Drop the per-file state; the nodes themselves are released with the
   * subtree by the VFS refcount. */
  for (struct vfs_node *c = d->first_child; c; c = c->next_sibling) {
    if (c->inode && c->inode->data && c->inode->read_cb == module_sysfs_read) {
      kfree(c->inode->data);
      c->inode->data = 0;
      c->inode->read_cb = 0;
      c->inode->write_cb = 0;
    }
    for (struct vfs_node *g = c->first_child; g; g = g->next_sibling) {
      if (g->inode && g->inode->data &&
          g->inode->read_cb == module_sysfs_read) {
        kfree(g->inode->data);
        g->inode->data = 0;
        g->inode->read_cb = 0;
        g->inode->write_cb = 0;
      }
    }
  }
  vfs_node_put(d);
}

void module_sysfs_attach_root(struct vfs_node *sys_root) {
  if (!sys_root)
    return;
  module_sysfs_root = module_sysfs_mkdir(sys_root, "module");
  if (!module_sysfs_root)
    return;
  for (struct module *m = module_list; m; m = m->next)
    module_sysfs_add(m);
}

/* ── request_module / modules.dep ────────────────────────────────────────── */

/* /lib/modules/<kernel release>, the layout every modutils implementation
 * expects: BusyBox's modprobe chdir()s to $CONFIG_DEFAULT_MODULES_DIR/$(uname
 * -r) and reads modules.dep/modules.alias from there, and uname's release is
 * B1NIX_VERSION_STR. Keeping the kernel's own request_module() on the same
 * files is what lets the applets replace /bin/kmod. */
#define MODULE_DIR "/lib/modules/" B1NIX_RELEASE_STR

static void module_path_for(const char *name, char *out, usize cap) {
  snprintf(out, cap, MODULE_DIR "/%s.ko", name);
}

/* Look `key` up in modules.dep ("<key>.ko: <dep>.ko <dep>.ko"). Returns 0 on a
 * hit and copies the dependency list into `out`. */
static int module_lookup_line(const char *file, const char *key, char *out,
                              usize cap) {
  char *data = 0;
  usize size = 0;
  if (module_read_file(file, &data, &size) != 0)
    return -ENOENT;
  usize klen = strlen(key);
  int found = -ENOENT;
  usize pos = 0;
  while (pos < size) {
    usize eol = pos;
    while (eol < size && data[eol] != '\n')
      eol++;
    usize len = eol - pos;
    const char *line = data + pos;
    if (len > klen + 1 && strncmp(line, key, klen) == 0 && line[klen] == ':') {
      usize vstart = klen + 1;
      while (vstart < len && mod_isspace(line[vstart]))
        vstart++;
      usize vlen = len - vstart;
      if (vlen >= cap)
        vlen = cap - 1;
      memcpy(out, line + vstart, vlen);
      out[vlen] = '\0';
      found = 0;
      break;
    }
    pos = eol + 1;
  }
  kfree(data);
  return found;
}

/* Translate an alias to a module name via modules.alias, whose lines are
 * depmod's own "alias <pattern> <module>". */
static int module_resolve_alias(const char *alias, char *out, usize cap) {
  char *data = 0;
  usize size = 0;
  if (module_read_file(MODULE_DIR "/modules.alias", &data, &size) != 0)
    return -ENOENT;
  usize alen = strlen(alias);
  int found = -ENOENT;
  usize pos = 0;
  while (pos < size) {
    usize eol = pos;
    while (eol < size && data[eol] != '\n')
      eol++;
    const char *p = data + pos;
    const char *end = data + eol;
    if ((usize)(end - p) > 6 && strncmp(p, "alias ", 6) == 0) {
      p += 6;
      while (p < end && mod_isspace(*p))
        p++;
      const char *pat = p;
      while (p < end && !mod_isspace(*p))
        p++;
      if ((usize)(p - pat) == alen && strncmp(pat, alias, alen) == 0) {
        while (p < end && mod_isspace(*p))
          p++;
        const char *mod = p;
        while (p < end && !mod_isspace(*p))
          p++;
        usize n = (usize)(p - mod);
        if (n > 0 && n < cap) {
          memcpy(out, mod, n);
          out[n] = '\0';
          found = 0;
          break;
        }
      }
    }
    pos = eol + 1;
  }
  kfree(data);
  return found;
}

int request_module(const char *name) {
  if (!name || !name[0])
    return -EINVAL;
  char real[MODULE_NAME_MAX];
  strncpy(real, name, sizeof(real) - 1);
  real[sizeof(real) - 1] = '\0';

  if (module_find(real))
    return 0;

  /* An alias only resolves when no module of that name exists. */
  char aliased[MODULE_NAME_MAX];
  char path[128];
  module_path_for(real, path, sizeof(path));
  struct vfs_node *probe = vfs_find_node(path);
  if (!probe || IS_ERR(probe)) {
    if (module_resolve_alias(real, aliased, sizeof(aliased)) == 0) {
      strncpy(real, aliased, sizeof(real) - 1);
      real[sizeof(real) - 1] = '\0';
      if (module_find(real))
        return 0;
    }
  } else {
    vfs_node_put(probe);
  }

  /* Dependencies first, in the order modules.dep lists them. Both the key and
   * every dependency are filenames there ("ndp.ko: ipv6.ko"), which is the
   * format depmod writes and BusyBox's modprobe reads. */
  char deps[192];
  char depkey[MODULE_NAME_MAX + 4];
  snprintf(depkey, sizeof(depkey), "%s.ko", real);
  if (module_lookup_line(MODULE_DIR "/modules.dep", depkey, deps,
                         sizeof(deps)) == 0) {
    char *p = deps;
    while (*p) {
      while (*p && mod_isspace(*p))
        p++;
      if (!*p)
        break;
      char dep[MODULE_NAME_MAX];
      usize n = 0;
      while (*p && !mod_isspace(*p) && n < sizeof(dep) - 1)
        dep[n++] = *p++;
      dep[n] = '\0';
      /* "path/to/foo.ko" -> "foo": depmod may write a relative path, and the
       * loader keys everything by module name. */
      char *slash = dep;
      for (usize i = 0; dep[i]; i++)
        if (dep[i] == '/')
          slash = &dep[i + 1];
      if (slash != dep)
        memmove(dep, slash, strlen(slash) + 1);
      usize dlen = strlen(dep);
      if (dlen > 3 && strcmp(dep + dlen - 3, ".ko") == 0)
        dep[dlen - 3] = '\0';
      while (*p && !mod_isspace(*p))
        p++;
      if (dep[0] && !module_find(dep)) {
        int rc = request_module(dep);
        if (rc != 0)
          return rc;
      }
    }
  }

  module_path_for(real, path, sizeof(path));
  return module_load_path(path, "");
}

/* ── boot-time bring-up ──────────────────────────────────────────────────── */

/* The modules the kernel itself asks for at boot. Each is optional: a kernel
 * whose initramfs carries none of them still boots, just without the optional
 * filesystems, the sound device and the IPv6 stack. */
static const char *const module_boot_list[] = {
    "isofs", "ntfs", "btrfs", "hda", "ndp", "ipv6", "ntp",
};

void module_init_builtin_deps(void) {
  for (usize i = 0; i < sizeof(module_boot_list) / sizeof(module_boot_list[0]);
       i++) {
    int rc = request_module(module_boot_list[i]);
    if (rc != 0 && rc != -EEXIST) {
      char buf[96];
      snprintf(buf, sizeof(buf), "module: %s not loaded (%d)\n",
               module_boot_list[i], rc);
      console_write(buf);
    }
  }
}
