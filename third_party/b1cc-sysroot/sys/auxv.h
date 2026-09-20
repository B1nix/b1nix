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

/* b1nix-private: pointer to the shared-library constructor descriptor table the
 * kernel builds during eager dynamic linking. Far above the standard auxv range
 * (0..51) so it never collides. Must match kernel/user/process.c. */
#ifndef AT_B1NIX_DSO_INIT
#define AT_B1NIX_DSO_INIT 0x1000
#endif

#ifdef __cplusplus
extern "C" {
#endif

unsigned long getauxval(unsigned long type);

/* Run the shared-library constructors (DT_INIT_ARRAY) the kernel collected for
 * this image, before the executable's own __init_array. Called once by crt0. */
void __b1nix_run_dso_init(int argc, char **argv, char **envp);

#ifdef __cplusplus
}
#endif

#endif
