/* M36 userspace smoke test: emits start/done markers. The real GDB and ftrace
 * self-tests run in-kernel (m36_gdb_selftest, m36_ftrace_selftest) and emit
 * their ok/FAIL markers directly to the serial log. */

#include <string.h>
#include <unistd.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

int main(void) {
  emit("M36-USR: start\n");
  emit("M36-USR: done\n");
  return 0;
}
