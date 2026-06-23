#ifndef B1NIX_U_SYS_AUXV_H
#define B1NIX_U_SYS_AUXV_H

/* getauxval() — read an entry from the ELF auxiliary vector the kernel placed
 * on the initial stack. AT_* type constants live in <elf.h>. Returns 0 when
 * the requested type is absent. */

#ifdef __cplusplus
extern "C" {
#endif

unsigned long getauxval(unsigned long type);

#ifdef __cplusplus
}
#endif

#endif
