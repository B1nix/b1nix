/*
 * m69_plugin.c — a real shared object dlopen'd at runtime by the M69 smoke.
 *
 * Exercises the runtime loader end to end:
 *   - exported function  (m69_plugin_add)      → resolved via dlsym
 *   - exported variable  (m69_plugin_ctor_ran) → proves the constructor ran
 *   - absolute data ptr  (g_ctor_msg)          → forces an R_X86_64_RELATIVE
 *   - external call       write()              → forces a JUMP_SLOT relocation
 *                                                 resolved against libc.so.1
 *   - constructor/destructor → DT_INIT_ARRAY / DT_FINI_ARRAY are run
 */

#include <unistd.h>

static const char g_ctor_text[] = "M69-PLUGIN: ctor\n";
static const char g_dtor_text[] = "M69-PLUGIN: dtor\n";
/* Stored in .data as an absolute pointer → emits an R_X86_64_RELATIVE the
 * runtime loader must rebase before the constructor dereferences it. */
static const char *const g_ctor_msg = g_ctor_text;
static const char *const g_dtor_msg = g_dtor_text;

int m69_plugin_ctor_ran = 0;

int m69_plugin_add(int a, int b) { return a + b; }

/* General-dynamic TLS → R_X86_64_DTPMOD64 / R_X86_64_DTPOFF64 + __tls_get_addr:
 * a __thread counter reached only through these accessors, so each access in the
 * dlopen'd object exercises the runtime loader's matured TLS path. */
static __thread int g_tls_counter = 100;
int m69_plugin_tls_bump(void) { return ++g_tls_counter; }
int m69_plugin_tls_get(void) { return g_tls_counter; }

__attribute__((constructor)) static void m69_plugin_init(void) {
  m69_plugin_ctor_ran = 1;
  write(1, g_ctor_msg, sizeof(g_ctor_text) - 1);
}

__attribute__((destructor)) static void m69_plugin_fini(void) {
  write(1, g_dtor_msg, sizeof(g_dtor_text) - 1);
}
