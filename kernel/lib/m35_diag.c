/* M35 diagnostic self-test: verifies kallsyms symbolication (the post-link
 * symbol blob the two-pass link embeds) resolves kernel text addresses to the
 * right function name and offset. Runs only in test mode. Emits M35-DIAG
 * markers consumed by tests/smoke.sh.
 *
 * The core-dump half of M35 is exercised from userspace (m35_smoke.c). */

#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/klog.h>
#include <b1nix/sched.h>
#include <b1nix/vfs.h>
#include <string.h>

void m35_diag_run(void) {
  if (!bootinfo_has_flag("b1nix.test=1"))
    return;

  console_write("M35-DIAG: start\n");

  /* A global function's entry address must resolve to its own name at
   * offset 0. vfs_init is a stable, non-static text symbol. */
  u64 off = 0;
  const char *name = ksym_lookup((u64)(usize)&vfs_init, &off);
  if (name && strcmp(name, "vfs_init") == 0 && off == 0)
    console_write("M35-DIAG: ok kallsyms\n");
  else
    console_write("M35-DIAG: fail kallsyms\n");

  /* An address a few bytes into the same function resolves to the same
   * symbol with the matching offset (nearest-symbol-below lookup). */
  off = 0;
  name = ksym_lookup((u64)(usize)&vfs_init + 8, &off);
  if (name && strcmp(name, "vfs_init") == 0 && off == 8)
    console_write("M35-DIAG: ok kallsyms-offset\n");
  else
    console_write("M35-DIAG: fail kallsyms-offset\n");

  /* A second distinct symbol resolves independently. */
  off = 0;
  name = ksym_lookup((u64)(usize)&scheduler_init, &off);
  if (name && strcmp(name, "scheduler_init") == 0 && off == 0)
    console_write("M35-DIAG: ok kallsyms-multi\n");
  else
    console_write("M35-DIAG: fail kallsyms-multi\n");

  console_write("M35-DIAG: done\n");
}
