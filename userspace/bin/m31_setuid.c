/* M31: setuid binary. Marked INITRAMFS_SETUID in initramfs.c so any
 * execve of this file by a non-root task elevates the new task's euid
 * to the file's owner (uid 0). Body just prints the live uid/euid so
 * the parent (m31_smoke) can verify the elevation took effect. */

#include <stdio.h>
#include <unistd.h>
#include "syscall.h"

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  long uid = syscall(SYS_GETUID);
  long euid = syscall(SYS_GETEUID);
  /* Output is consumed via stdout; the parent uses a pipe (or writes to
   * a file). Format is deterministic: "M31-SETUID: uid=N euid=N\n". */
  char buf[64];
  int n = 0;
  const char *p = "M31-SETUID: uid=";
  while (*p) buf[n++] = *p++;
  /* Tiny decimal formatter — uid fits in 5 digits for our use. */
  if (uid >= 10000) buf[n++] = '0' + (char)((uid / 10000) % 10);
  if (uid >= 1000)  buf[n++] = '0' + (char)((uid / 1000) % 10);
  if (uid >= 100)   buf[n++] = '0' + (char)((uid / 100) % 10);
  if (uid >= 10)    buf[n++] = '0' + (char)((uid / 10) % 10);
  buf[n++] = '0' + (char)(uid % 10);
  p = " euid=";
  while (*p) buf[n++] = *p++;
  if (euid >= 10000) buf[n++] = '0' + (char)((euid / 10000) % 10);
  if (euid >= 1000)  buf[n++] = '0' + (char)((euid / 1000) % 10);
  if (euid >= 100)   buf[n++] = '0' + (char)((euid / 100) % 10);
  if (euid >= 10)    buf[n++] = '0' + (char)((euid / 10) % 10);
  buf[n++] = '0' + (char)(euid % 10);
  buf[n++] = '\n';
  write(1, buf, n);
  /* Exit code 0 if euid is root (suid bit took effect), nonzero
   * otherwise — so the m31_smoke driver can also verify via waitpid. */
  return euid == 0 ? 0 : 1;
}
