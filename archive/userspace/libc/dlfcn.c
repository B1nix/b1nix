/*
 * dlfcn.c — B1NIX userspace dynamic loader (M69).
 *
 * A real, self-contained ld.so-style implementation that lives entirely in
 * userspace and uses only the existing syscalls (open/read/close/lseek/mmap/
 * mprotect). No kernel changes: the kernel still loads the main PIE binary and
 * its startup DT_NEEDED objects eagerly (M30); this code handles everything
 * that happens *after* the process is already running.
 *
 * Phase 1 — symbol lookup in the startup-loaded libc.so.1 (the "base object",
 *           seeded from this object's own _DYNAMIC + __b1nix_image_base).
 * Phase 2 — load a *new* ET_DYN object at runtime: map PT_LOAD segments, parse
 *           PT_DYNAMIC, resolve undefined symbols against the loaded objects,
 *           apply RELATIVE/GLOB_DAT/JUMP_SLOT/64 relocations, run DT_INIT /
 *           DT_INIT_ARRAY constructors.
 * Phase 3 — reference counting (re-dlopen returns the same handle, dlclose
 *           unloads at zero after running DT_FINI_ARRAY / DT_FINI), and the
 *           RTLD_DEFAULT / RTLD_NEXT lookup scopes.
 */

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h> /* SEEK_SET / SEEK_END */
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* This process's own dynamic section + load base, provided by linker_shared.ld
 * for libc.so.1. Weak: in a statically linked ET_EXEC they resolve to 0, in
 * which case there is no base object and dlopen of new files still works but
 * has no shared libc to resolve against. */
extern const Elf64_Dyn _DYNAMIC[] __attribute__((weak));
extern const char __b1nix_image_base[] __attribute__((weak));

#define DL_PAGE 4096UL
#define DL_PATH_MAX 128

struct dl_object {
  uintptr_t base; /* load bias: base + st_value == runtime address      */
  const Elf64_Sym *symtab;
  const char *strtab;
  const uint32_t *hash; /* DT_HASH: [nbucket][nchain][buckets...][chain...] */
  size_t strsz;
  void *map_addr; /* mmap region to munmap (NULL for the base object)    */
  size_t map_len;
  int refcount;
  int is_base; /* libc.so.1: process-lifetime, never unmapped         */
  void (*init)(void);
  void (**init_array)(void);
  size_t init_arrayn;
  void (*fini)(void);
  void (**fini_array)(void);
  size_t fini_arrayn;
  /* PT_TLS of this object, for general-dynamic TLS in dlopen'd modules:
   * DTPMOD64 fills a GOT slot with `tls_module`, DTPOFF64 fills the next with
   * the variable's offset in the template, and __tls_get_addr(module,offset)
   * returns tls_block+offset — lazily allocating the per-process block from the
   * template on first use. (Single-thread/main-thread model; b1nix dlopen has
   * no per-thread DTV yet, which is sufficient for every current consumer.) */
  int tls_module;       /* >0 once this object carries a PT_TLS, else 0    */
  const void *tls_image; /* template bytes in the mapped image (filesz)    */
  size_t tls_memsz;
  size_t tls_filesz;
  void *tls_block;      /* lazily allocated block for __tls_get_addr        */
  char path[DL_PATH_MAX];
  struct dl_object *next;
};

static struct dl_object *g_objects;  /* head of the loaded-object list */
static struct dl_object g_base;      /* libc.so.1 base object storage  */
static int g_base_tried;
static const char *_dl_errmsg;
static int g_next_tls_module = 1;    /* unique DTPMOD64 ids for dlopen'd TLS */

/* General-dynamic TLS entry point. The compiler emits a call to this for every
 * __thread access in a shared object (the default GD model): the argument is a
 * GOT pair { module_id, offset } filled by R_X86_64_DTPMOD64 / R_X86_64_DTPOFF64.
 * Return the address of that thread-local for the calling thread. b1nix dlopen
 * has no per-thread DTV, so we allocate one block per module on first use and
 * hand out block+offset — correct for the main thread (the only one that runs a
 * dlopen'd module's TLS in current usage). */
struct __tls_index { unsigned long ti_module; unsigned long ti_offset; };

/* Bit-63 flag the kernel eager loader sets in a DTPMOD64 GOT slot for a STATIC
 * (load-time) module: the low bits then hold that module's static-TLS distance
 * below the thread pointer, so the thread-local address is (TP - dist + offset)
 * with no per-thread DTV needed. MUST match TLS_STATIC_MODULE_FLAG in
 * kernel/user/process.c. */
#define TLS_STATIC_MODULE_FLAG (1UL << 63)

void *__tls_get_addr(struct __tls_index *ti) {
  if (ti->ti_module & TLS_STATIC_MODULE_FLAG) {
    unsigned long dist = ti->ti_module & ~TLS_STATIC_MODULE_FLAG;
    unsigned long tp;
    __asm__ __volatile__("movq %%fs:0, %0" : "=r"(tp)); /* variant-II TP self-ptr */
    return (void *)(tp - dist + ti->ti_offset);
  }
  for (struct dl_object *o = g_objects; o; o = o->next) {
    if (o->tls_module != (int)ti->ti_module || o->tls_module == 0)
      continue;
    if (!o->tls_block && o->tls_memsz) {
      o->tls_block = calloc(1, o->tls_memsz);
      if (o->tls_block && o->tls_image && o->tls_filesz)
        memcpy(o->tls_block, o->tls_image, o->tls_filesz);
    }
    return (char *)o->tls_block + ti->ti_offset;
  }
  return NULL;
}

static int is_libc_name(const char *n) {
  return strcmp(n, "libc.so") == 0 || strcmp(n, "libc.so.1") == 0 ||
         strcmp(n, "/lib/libc.so.1") == 0 || strcmp(n, "/lib/libc.so") == 0;
}

/* Resolve a symbol within one object's own symbol table. The DT_HASH second
 * word (nchain) is exactly the number of symtab entries, so a linear scan is
 * both correct and simple — good enough for the object sizes b1nix loads. */
static void *obj_lookup(const struct dl_object *o, const char *name) {
  if (!o->symtab || !o->strtab || !o->hash)
    return NULL;
  uint32_t nchain = o->hash[1];
  for (uint32_t i = 1; i < nchain; i++) {
    const Elf64_Sym *s = &o->symtab[i];
    if (s->st_shndx == 0) /* undefined here */
      continue;
    if (o->strsz && s->st_name >= o->strsz)
      continue;
    if (strcmp(o->strtab + s->st_name, name) == 0)
      return (void *)(o->base + s->st_value);
  }
  return NULL;
}

/* Global (RTLD_DEFAULT) scope: search every loaded object in list order. */
static void *global_lookup(const char *name) {
  for (struct dl_object *o = g_objects; o; o = o->next) {
    void *p = obj_lookup(o, name);
    if (p)
      return p;
  }
  return NULL;
}

/* Like global_lookup but skips `self` — an R_X86_64_COPY in the (rare) non-PIC
 * dlopen'd object must find the *defining* library's copy, not its own. */
static void *global_lookup_excluding(const struct dl_object *self,
                                     const char *name) {
  for (struct dl_object *o = g_objects; o; o = o->next) {
    if (o == self)
      continue;
    void *p = obj_lookup(o, name);
    if (p)
      return p;
  }
  return NULL;
}

/* Lazily seed the base object (libc.so.1) from this process's own _DYNAMIC. */
static struct dl_object *ensure_base(void) {
  if (g_base_tried)
    return g_base.is_base ? &g_base : NULL;
  g_base_tried = 1;
  if (!_DYNAMIC || !__b1nix_image_base)
    return NULL;
  uintptr_t base = (uintptr_t)__b1nix_image_base;
  g_base.base = base;
  for (const Elf64_Dyn *d = _DYNAMIC; d->d_tag != DT_NULL; d++) {
    switch (d->d_tag) {
    case DT_HASH:
      g_base.hash = (const uint32_t *)(base + d->d_un.d_ptr);
      break;
    case DT_STRTAB:
      g_base.strtab = (const char *)(base + d->d_un.d_ptr);
      break;
    case DT_SYMTAB:
      g_base.symtab = (const Elf64_Sym *)(base + d->d_un.d_ptr);
      break;
    case DT_STRSZ:
      g_base.strsz = d->d_un.d_val;
      break;
    }
  }
  if (!g_base.hash || !g_base.symtab || !g_base.strtab)
    return NULL;
  g_base.is_base = 1;
  g_base.refcount = 1;
  strcpy(g_base.path, "libc.so.1");
  g_base.next = g_objects;
  g_objects = &g_base;
  return &g_base;
}

static unsigned long page_down(unsigned long v) { return v & ~(DL_PAGE - 1); }
static unsigned long page_up(unsigned long v) {
  return (v + DL_PAGE - 1) & ~(DL_PAGE - 1);
}

static int prot_of(uint32_t pf) {
  int p = 0;
  if (pf & PF_R)
    p |= PROT_READ;
  if (pf & PF_W)
    p |= PROT_WRITE;
  if (pf & PF_X)
    p |= PROT_EXEC;
  return p;
}

/* Apply one RELA table. `o` must already be on g_objects so its own exported
 * symbols participate in resolution. Returns 0 on success, -1 if a strong
 * (non-weak) symbol is unresolved or a relocation type is unsupported. */
static int apply_rela(struct dl_object *o, const Elf64_Rela *rela, size_t bytes) {
  if (!rela || !bytes)
    return 0;
  size_t n = bytes / sizeof(Elf64_Rela);
  for (size_t i = 0; i < n; i++) {
    uint64_t type = ELF64_R_TYPE(rela[i].r_info);
    uint64_t symi = ELF64_R_SYM(rela[i].r_info);
    uint64_t *where = (uint64_t *)(o->base + rela[i].r_offset);
    switch (type) {
    case R_X86_64_NONE:
      break;
    case R_X86_64_RELATIVE:
      *where = (uint64_t)o->base + rela[i].r_addend;
      break;
    case R_X86_64_64:
    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT: {
      const Elf64_Sym *s = &o->symtab[symi];
      const char *name = o->strtab + s->st_name;
      void *val = obj_lookup(o, name);
      if (!val)
        val = global_lookup(name);
      if (!val && ELF64_ST_BIND(s->st_info) != STB_WEAK) {
        _dl_errmsg = "dlopen: unresolved symbol";
        return -1;
      }
      uint64_t addend = (type == R_X86_64_64) ? (uint64_t)rela[i].r_addend : 0;
      *where = (uint64_t)val + addend;
      break;
    }
    case R_X86_64_IRELATIVE: {
      /* IFUNC: the addend is a resolver function (a load-base-relative offset);
       * call it now — in this running process, ring 3 — and store the address
       * it returns. The selected implementation thus reflects the real runtime
       * CPU, exactly as glibc's ld.so does it. */
      uint64_t (*resolver)(void) =
          (uint64_t(*)(void))(o->base + rela[i].r_addend);
      *where = resolver();
      break;
    }
    case R_X86_64_COPY: {
      /* Copy a data object out of the defining library into this object's own
       * storage (only a non-PIC dlopen'd ET_DYN emits these — rare, but cheap
       * and correct to support). */
      const Elf64_Sym *s = &o->symtab[symi];
      const char *name = o->strtab + s->st_name;
      void *src = global_lookup_excluding(o, name);
      if (src && s->st_size)
        memcpy(where, src, s->st_size);
      break;
    }
    case R_X86_64_DTPMOD64: {
      /* GD TLS, first GOT word: the module id of the object that owns the
       * thread-local. A local (symi==0) ref names this object; a named symbol is
       * resolved to its defining object so cross-object __thread refs work too. */
      int mod = o->tls_module;
      if (symi) {
        const Elf64_Sym *s = &o->symtab[symi];
        const char *name = o->strtab + s->st_name;
        for (struct dl_object *d = g_objects; d; d = d->next)
          if (d->tls_module && obj_lookup(d, name)) { mod = d->tls_module; break; }
      }
      *where = (uint64_t)mod;
      break;
    }
    case R_X86_64_DTPOFF64: {
      /* GD TLS, second GOT word: the variable's offset inside its module's TLS
       * block (template-relative st_value + addend). */
      const Elf64_Sym *s = &o->symtab[symi];
      *where = (uint64_t)s->st_value + (uint64_t)rela[i].r_addend;
      break;
    }
    default:
      _dl_errmsg = "dlopen: unsupported relocation type";
      return -1;
    }
  }
  return 0;
}

static void run_init(struct dl_object *o) {
  if (o->init)
    o->init();
  for (size_t i = 0; i < o->init_arrayn; i++)
    if (o->init_array[i])
      o->init_array[i]();
}

static void run_fini(struct dl_object *o) {
  for (size_t i = o->fini_arrayn; i-- > 0;)
    if (o->fini_array[i])
      o->fini_array[i]();
  if (o->fini)
    o->fini();
}

static void unlink_object(struct dl_object *o) {
  struct dl_object **pp = &g_objects;
  while (*pp) {
    if (*pp == o) {
      *pp = o->next;
      return;
    }
    pp = &(*pp)->next;
  }
}

/* Read an entire file into a freshly malloc'd buffer. */
static char *read_file(const char *path, long *out_size) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0)
    return NULL;
  long size = lseek(fd, 0, SEEK_END);
  if (size <= 0) {
    close(fd);
    return NULL;
  }
  lseek(fd, 0, SEEK_SET);
  char *buf = malloc(size);
  if (!buf) {
    close(fd);
    return NULL;
  }
  long got = 0;
  while (got < size) {
    long r = read(fd, buf + got, size - got);
    if (r <= 0)
      break;
    got += r;
  }
  close(fd);
  if (got != size) {
    free(buf);
    return NULL;
  }
  *out_size = size;
  return buf;
}

static void *load_object(const char *path, int flag);

/* Map PT_LOAD segments, parse PT_DYNAMIC, relocate, and run constructors. */
static void *load_object(const char *path, int flag) {
  long fsize = 0;
  char *file = read_file(path, &fsize);
  if (!file) {
    _dl_errmsg = "dlopen: cannot read object file";
    return NULL;
  }

  Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
  if (fsize < (long)sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
      eh->e_ident[4] != 2 /* ELFCLASS64 */ || eh->e_type != ET_DYN ||
      eh->e_machine != EM_X86_64 ||
      eh->e_phentsize != sizeof(Elf64_Phdr) ||
      eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > (uint64_t)fsize) {
    free(file);
    _dl_errmsg = "dlopen: not a valid x86-64 shared object";
    return NULL;
  }
  Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff);

  /* Span of the load image (lowest page .. highest page across all PT_LOAD). */
  uint64_t minv = ~0ULL, maxv = 0;
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD)
      continue;
    if (ph[i].p_vaddr < minv)
      minv = page_down(ph[i].p_vaddr);
    if (ph[i].p_vaddr + ph[i].p_memsz > maxv)
      maxv = ph[i].p_vaddr + ph[i].p_memsz;
  }
  if (minv == ~0ULL) {
    free(file);
    _dl_errmsg = "dlopen: no loadable segments";
    return NULL;
  }
  maxv = page_up(maxv);
  size_t span = maxv - minv;

  void *map = mmap(NULL, span, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (map == MAP_FAILED) {
    free(file);
    _dl_errmsg = "dlopen: mmap failed";
    return NULL;
  }
  uintptr_t base = (uintptr_t)map - minv;

  /* Copy file contents of each PT_LOAD; the .bss tail is already zero (anon). */
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0)
      continue;
    if (ph[i].p_offset + ph[i].p_filesz > (uint64_t)fsize) {
      munmap(map, span);
      free(file);
      _dl_errmsg = "dlopen: segment out of bounds";
      return NULL;
    }
    memcpy((void *)(base + ph[i].p_vaddr), file + ph[i].p_offset,
           ph[i].p_filesz);
  }

  struct dl_object *o = calloc(1, sizeof(*o));
  if (!o) {
    munmap(map, span);
    free(file);
    _dl_errmsg = "dlopen: out of memory";
    return NULL;
  }
  o->base = base;
  o->map_addr = map;
  o->map_len = span;
  o->refcount = 1;
  strncpy(o->path, path, DL_PATH_MAX - 1);

  /* Parse the dynamic section. */
  const Elf64_Rela *rela = NULL, *jmprel = NULL;
  size_t relasz = 0, jmprelsz = 0;
  const uint64_t *needed = NULL; /* DT_NEEDED string offsets, collected inline */
  Elf64_Dyn *dyn = NULL;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_DYNAMIC)
      dyn = (Elf64_Dyn *)(base + ph[i].p_vaddr);
  (void)needed;
  if (!dyn) {
    free(o);
    munmap(map, span);
    free(file);
    _dl_errmsg = "dlopen: no PT_DYNAMIC";
    return NULL;
  }
  for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
    switch (d->d_tag) {
    case DT_HASH:    o->hash = (const uint32_t *)(base + d->d_un.d_ptr); break;
    case DT_STRTAB:  o->strtab = (const char *)(base + d->d_un.d_ptr); break;
    case DT_SYMTAB:  o->symtab = (const Elf64_Sym *)(base + d->d_un.d_ptr); break;
    case DT_STRSZ:   o->strsz = d->d_un.d_val; break;
    case DT_RELA:    rela = (const Elf64_Rela *)(base + d->d_un.d_ptr); break;
    case DT_RELASZ:  relasz = d->d_un.d_val; break;
    case DT_JMPREL:  jmprel = (const Elf64_Rela *)(base + d->d_un.d_ptr); break;
    case DT_PLTRELSZ: jmprelsz = d->d_un.d_val; break;
    case DT_INIT:    o->init = (void (*)(void))(base + d->d_un.d_ptr); break;
    case DT_FINI:    o->fini = (void (*)(void))(base + d->d_un.d_ptr); break;
    case DT_INIT_ARRAY:
      o->init_array = (void (**)(void))(base + d->d_un.d_ptr);
      break;
    case DT_INIT_ARRAYSZ:
      o->init_arrayn = d->d_un.d_val / sizeof(void *);
      break;
    case DT_FINI_ARRAY:
      o->fini_array = (void (**)(void))(base + d->d_un.d_ptr);
      break;
    case DT_FINI_ARRAYSZ:
      o->fini_arrayn = d->d_un.d_val / sizeof(void *);
      break;
    }
  }

  /* Capture a PT_TLS template (general-dynamic TLS for dlopen'd modules). The
   * module gets a unique id; DTPMOD64/DTPOFF64 relocations and __tls_get_addr
   * use it to materialise the block lazily. */
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type != PT_TLS)
      continue;
    o->tls_module = g_next_tls_module++;
    o->tls_image = (const void *)(base + ph[i].p_vaddr);
    o->tls_memsz = ph[i].p_memsz;
    o->tls_filesz = ph[i].p_filesz;
    break;
  }

  /* Make this object visible (its own exports resolve self-references and
   * become available to later lookups) before relocating. */
  o->next = g_objects;
  g_objects = o;

  /* Load any DT_NEEDED dependency that is not the already-present libc base. */
  ensure_base();
  for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
    if (d->d_tag != DT_NEEDED || !o->strtab)
      continue;
    const char *dep = o->strtab + d->d_un.d_val;
    if (is_libc_name(dep))
      continue;
    char deppath[DL_PATH_MAX];
    if (dep[0] == '/')
      strncpy(deppath, dep, DL_PATH_MAX - 1), deppath[DL_PATH_MAX - 1] = 0;
    else {
      strcpy(deppath, "/lib/");
      strncat(deppath, dep, DL_PATH_MAX - 6);
    }
    if (!global_lookup(dep) && !load_object(deppath, flag)) {
      /* dependency missing: unwind */
      unlink_object(o);
      munmap(map, span);
      free(o);
      free(file);
      return NULL;
    }
  }

  if (apply_rela(o, rela, relasz) != 0 ||
      apply_rela(o, jmprel, jmprelsz) != 0) {
    unlink_object(o);
    munmap(map, span);
    free(o);
    free(file);
    return NULL;
  }

  /* Apply final segment protections (the image was mapped RW for loading). */
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD)
      continue;
    unsigned long start = page_down(base + ph[i].p_vaddr);
    unsigned long end = page_up(base + ph[i].p_vaddr + ph[i].p_memsz);
    mprotect((void *)start, end - start, prot_of(ph[i].p_flags));
  }

  free(file);
  run_init(o);
  return o;
}

void *dlopen(const char *filename, int flag) {
  ensure_base();
  if (!filename) /* RTLD_DEFAULT main handle: the global scope */
    return ensure_base();
  if (is_libc_name(filename)) {
    struct dl_object *b = ensure_base();
    if (b)
      return b;
    _dl_errmsg = "dlopen: no dynamic object in this process";
    return NULL;
  }
  /* Already loaded? Bump the reference count and return the same handle. */
  for (struct dl_object *o = g_objects; o; o = o->next)
    if (!o->is_base && strcmp(o->path, filename) == 0) {
      o->refcount++;
      return o;
    }
  if (flag & RTLD_NOLOAD) {
    _dl_errmsg = "dlopen: object not already loaded (RTLD_NOLOAD)";
    return NULL;
  }
  return load_object(filename, flag);
}

char *dlerror(void) {
  const char *m = _dl_errmsg;
  _dl_errmsg = NULL;
  return (char *)m;
}

void *dlsym(void *handle, const char *symbol) {
  ensure_base();
  void *result = NULL;
  if (handle == RTLD_DEFAULT) {
    result = global_lookup(symbol);
  } else if (handle == RTLD_NEXT) {
    /* Best effort: the first definition past the base object. */
    int seen_base = 0;
    for (struct dl_object *o = g_objects; o; o = o->next) {
      if (o->is_base) {
        seen_base = 1;
        continue;
      }
      if (seen_base && (result = obj_lookup(o, symbol)))
        break;
    }
  } else {
    struct dl_object *o = (struct dl_object *)handle;
    result = obj_lookup(o, symbol);
    if (!result) /* handle scope includes its dependencies */
      result = global_lookup(symbol);
  }
  if (!result)
    _dl_errmsg = "dlsym: symbol not found";
  return result;
}

int dlclose(void *handle) {
  if (!handle)
    return -1;
  struct dl_object *o = (struct dl_object *)handle;
  if (o == &g_base || o->is_base)
    return 0; /* base object lives for the whole process */
  /* Validate the handle is one of ours. */
  int known = 0;
  for (struct dl_object *p = g_objects; p; p = p->next)
    if (p == o) {
      known = 1;
      break;
    }
  if (!known) {
    _dl_errmsg = "dlclose: invalid handle";
    return -1;
  }
  if (--o->refcount > 0)
    return 0;
  run_fini(o);
  unlink_object(o);
  if (o->tls_block)
    free(o->tls_block);
  munmap(o->map_addr, o->map_len);
  free(o);
  return 0;
}

/* dladdr: b1nix keeps no global address→symbol index; report "not found"
 * (the glibc convention of returning 0), which callers degrade gracefully on. */
int dladdr(const void *addr, Dl_info *info) {
  if (!info)
    return 0;
  memset(info, 0, sizeof(*info));
  uintptr_t a = (uintptr_t)addr;
  /* If the address falls inside a dlopen'd object's mapped image, report it. */
  for (struct dl_object *o = g_objects; o; o = o->next) {
    if (o->map_addr && o->map_len && a >= (uintptr_t)o->map_addr &&
        a < (uintptr_t)o->map_addr + o->map_len) {
      info->dli_fname = o->path;
      info->dli_fbase = o->map_addr;
      return 1; /* dli_sname/dli_saddr: nearest-symbol lookup is future work */
    }
  }
  /* Otherwise the address is in the main executable (placed by the M69 in-kernel
   * loader, which dlfcn does not map). Report the main exe path from
   * /proc/self/exe — this is what llvm::sys::getMainExecutable's HAVE_DLOPEN
   * branch (dladdr) needs to locate clang's own install dir; without it clang's
   * InstalledDir is empty and the in-guest cc1as re-exec for .S files fails. */
  static char exe[DL_PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0)
    return 0;
  exe[n] = '\0';
  info->dli_fname = exe;
  return 1;
}
