#ifndef _LINK_H
#define _LINK_H

/* Minimal <link.h>. b1nix is statically linked with no dynamic loader, so
 * dl_iterate_phdr reports no shared objects (returns 0 without invoking the
 * callback). Enough for symbolizers/backtrace code that probe it but tolerate
 * an empty result. */
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

#ifdef __cplusplus
}
#endif
#endif
