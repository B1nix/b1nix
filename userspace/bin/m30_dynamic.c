#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

static int emit(const char *s) {
  size_t n = strlen(s);
  return write(1, s, n) == (ssize_t)n ? 0 : 1;
}

int main(void) {
  const char *message = "M30-DYN: ok shared-libc\n";
  size_t length = strlen(message);
  if (length != sizeof("M30-DYN: ok shared-libc\n") - 1)
    return 1;
  if (write(1, message, length) != (ssize_t)length)
    return 1;

  /* Phase 1 — resolve and call a symbol from the startup-loaded libc.so.1. */
  void *libc = dlopen("libc.so.1", RTLD_NOW);
  size_t (*loaded_strlen)(const char *) =
      (size_t (*)(const char *))dlsym(libc, "strlen");
  if (!libc || !loaded_strlen || loaded_strlen("M69") != 3 || dlclose(libc))
    return 1;
  if (emit("M69-DL: dlsym ok\n"))
    return 1;

  /* Phase 2 — load a brand-new shared object at runtime, resolve an exported
   * function, and call it. The plugin's constructor prints "M69-PLUGIN: ctor"
   * (proving DT_INIT_ARRAY ran) and its write() call proves a JUMP_SLOT
   * relocation against libc.so.1 was applied. */
  void *plugin = dlopen("/lib/m69_plugin.so", RTLD_NOW);
  if (!plugin)
    return 1;
  int (*add)(int, int) = (int (*)(int, int))dlsym(plugin, "m69_plugin_add");
  int *ctor_ran = (int *)dlsym(plugin, "m69_plugin_ctor_ran");
  if (!add || !ctor_ran || *ctor_ran != 1 || add(40, 2) != 42)
    return 1;
  if (emit("M69-DL2: dlopen-run ok\n"))
    return 1;

  /* Phase 3 — reference counting and lookup scopes. A second dlopen of the
   * same path returns the same handle (no re-construction); one dlclose drops
   * the count without unmapping; RTLD_DEFAULT finds the export globally; the
   * final dlclose runs the destructor ("M69-PLUGIN: dtor") and unmaps. */
  void *plugin2 = dlopen("/lib/m69_plugin.so", RTLD_NOW);
  if (plugin2 != plugin)
    return 1;
  if (dlclose(plugin2) != 0) /* refcount 2 -> 1, still mapped */
    return 1;
  if (add(2, 3) != 5) /* still callable */
    return 1;
  void *global_add = dlsym(RTLD_DEFAULT, "m69_plugin_add");
  void *missing = dlsym(RTLD_DEFAULT, "m69_no_such_symbol_zzz");
  if (global_add != (void *)add || missing != 0)
    return 1;

  /* Phase 4 — matured relocations: general-dynamic TLS in a dlopen'd object
   * (R_X86_64_DTPMOD64/DTPOFF64 filled by the loader, reached through
   * __tls_get_addr). The plugin's __thread counter is private to this access. */
  int (*tls_bump)(void) = (int (*)(void))dlsym(plugin, "m69_plugin_tls_bump");
  int (*tls_get)(void) = (int (*)(void))dlsym(plugin, "m69_plugin_tls_get");
  if (!tls_bump || !tls_get)
    return 1;
  if (tls_get() != 100 || tls_bump() != 101 || tls_bump() != 102 ||
      tls_get() != 102)
    return 1;
  if (emit("M69-DL5: tls-gd ok\n"))
    return 1;

  if (dlclose(plugin) != 0) /* refcount 1 -> 0, runs destructor + munmap */
    return 1;
  if (emit("M69-DL3: refcount-scope ok\n"))
    return 1;

  return 0;
}
