/* Minimal __b1nix_run_dso_init stub for musl-linked dynamic binaries.
 *
 * When b1nix-musl-cc links against musl's libc.so using b1nix's crt0-dynamic.o,
 * crt0 calls __b1nix_run_dso_init before main(). This stub reads the kernel's
 * AT_B1NIX_DSO_INIT auxiliary vector entry and calls the shared-library
 * constructor functions it describes. b1nix_register_dso_frames is a no-op
 * (musl binaries don't use libgcc's DWARF unwinder). */

#ifndef AT_B1NIX_DSO_INIT
#define AT_B1NIX_DSO_INIT 0x1000
#endif

typedef void (*b1nix_init_fn)(int, char **, char **);

/* Stub: no-op for DWARF frame registration (not needed for musl binaries). */
void b1nix_register_dso_frames(void) {}

/* Read AT_B1NIX_DSO_INIT from the ELF auxiliary vector on the initial stack.
 * The auxv sits after the NULL-terminated envp array: { type, value } pairs
 * terminated by { AT_NULL, 0 }. We locate it by scanning from envp. */
static unsigned long get_dso_init_ptr(char **envp) {
  /* Skip past envp to find the auxv */
  char **p = envp;
  while (*p) p++;
  p++; /* skip NULL terminator of envp */
  unsigned long *aux = (unsigned long *)p;
  for (;;) {
    unsigned long a_type = aux[0];
    unsigned long a_val = aux[1];
    if (a_type == 0 /* AT_NULL */)
      return 0;
    if (a_type == AT_B1NIX_DSO_INIT)
      return a_val;
    aux += 2;
  }
}

void __b1nix_run_dso_init(int argc, char **argv, char **envp) {
  (void)argc; (void)argv;
  unsigned long t = get_dso_init_ptr(envp);
  if (!t)
    return;
  unsigned long *desc = (unsigned long *)t;
  for (; desc[0]; desc += 2) {
    b1nix_init_fn *arr = (b1nix_init_fn *)(void *)desc[0];
    unsigned long count = desc[1];
    for (unsigned long i = 0; i < count; i++)
      if (arr[i])
        arr[i](argc, argv, envp);
  }
}
