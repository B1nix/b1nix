#ifndef _LINK_H
#define _LINK_H

/* <link.h>. b1nix links shared objects eagerly in-kernel (no userspace ld.so),
 * but the kernel records the executable + every loaded shared library, so
 * dl_iterate_phdr reports them all. This is what lets the libgcc_s.so DWARF
 * unwinder find each module's PT_GNU_EH_FRAME — required for C++ exceptions to
 * unwind across the shared-libstdc++/exe boundary. */
#include <elf.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__LP64__) || defined(_LP64) || __SIZEOF_POINTER__ == 8
#define ElfW(type) Elf64_##type
#else
#define ElfW(type) Elf32_##type
#endif

struct dl_phdr_info {
  ElfW(Addr) dlpi_addr;
  const char *dlpi_name;
  const ElfW(Phdr) * dlpi_phdr;
  ElfW(Half) dlpi_phnum;
};

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *info, size_t size,
                                    void *data),
                    void *data);

/* Backing data for dl_iterate_phdr. SYS_DL_PHDR_INFO copies an array of these
 * out: one per loaded module (executable + shared libraries). The layout MUST
 * match the kernel's copyout in syscall.c (SYS_DL_PHDR_INFO). */
#define B1NIX_DL_NAME_MAX 96
struct b1nix_dl_module {
  unsigned long long base;        /* dlpi_addr (load bias) */
  unsigned long long phdr_vaddr;  /* address of the program header table */
  unsigned long long phnum;       /* dlpi_phnum (widened for a stable layout) */
  unsigned long long eh_frame_va; /* in-process .eh_frame address (0 if none) */
  char name[B1NIX_DL_NAME_MAX];   /* soname / path (dlpi_name) */
};

#ifdef __cplusplus
}
#endif
#endif
