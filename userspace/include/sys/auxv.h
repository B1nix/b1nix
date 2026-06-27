#ifndef B1NIX_U_SYS_AUXV_H
#define B1NIX_U_SYS_AUXV_H

/* getauxval() — read an entry from the ELF auxiliary vector the kernel placed
 * on the initial stack. AT_* type constants live in <elf.h>. Returns 0 when
 * the requested type is absent. */

#ifndef AT_PAGESZ
#define AT_PAGESZ    6
#endif
#ifndef AT_HWCAP
#define AT_HWCAP    16
#endif
#ifndef AT_HWCAP2
#define AT_HWCAP2   26
#endif
#ifndef AT_RANDOM
#define AT_RANDOM   25
#endif
#ifndef AT_SECURE
#define AT_SECURE   23
#endif
#ifndef AT_SYSINFO_EHDR
#define AT_SYSINFO_EHDR 33
#endif

#ifdef __cplusplus
extern "C" {
#endif

unsigned long getauxval(unsigned long type);

#ifdef __cplusplus
}
#endif

#endif
