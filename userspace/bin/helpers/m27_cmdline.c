/*
 * m27_cmdline — kernel command-line key=value parser self-test.
 * Ported from deleted kernel/user/programs.c (was inline in B1NXEXEC).
 * Reads /proc/cmdline and verifies: present key, absent key, prefix
 * non-matching, and value truncation.
 */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void marker(const char *t) { write(1, t, strlen(t)); }

/* Parse the kernel cmdline and find "key" → copy its value into `buf`.
 * Returns 1 on success, 0 if key not found. */
static int get_kv(const char *cmdline, const char *key, char *buf, int buflen) {
  int klen = strlen(key);
  const char *p = cmdline;
  while (*p) {
    /* Skip whitespace. */
    while (*p == ' ') p++;
    if (!*p) break;
    /* Check for key= prefix. */
    if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
      p += klen + 1;
      int i = 0;
      while (*p && *p != ' ' && i < buflen - 1)
        buf[i++] = *p++;
      buf[i] = '\0';
      return 1;
    }
    /* Skip to next token. */
    while (*p && *p != ' ') p++;
  }
  return 0;
}

int main(void) {
  char cmdline[512];
  int fd = open("/proc/cmdline", O_RDONLY);
  if (fd < 0) { marker("M27-CMDLINE: fail open\n"); return 1; }
  ssize_t n = read(fd, cmdline, sizeof(cmdline) - 1);
  close(fd);
  if (n <= 0) { marker("M27-CMDLINE: fail read\n"); return 1; }
  cmdline[n] = '\0';

  char v[16];
  char small[4];
  int ok = 1;

  /* b1nix.test=1 must be present. */
  if (!get_kv(cmdline, "b1nix.test", v, sizeof(v)) || strcmp(v, "1") != 0)
    ok = 0;
  /* b1nix.kvtest=abc123 must be present. */
  if (!get_kv(cmdline, "b1nix.kvtest", v, sizeof(v)) ||
      strcmp(v, "abc123") != 0)
    ok = 0;
  /* b1nix.absent must NOT be present. */
  if (get_kv(cmdline, "b1nix.absent", v, sizeof(v)))
    ok = 0;
  /* Prefix non-match: b1nix.tes should NOT match b1nix.test. */
  if (get_kv(cmdline, "b1nix.tes", v, sizeof(v)))
    ok = 0;
  /* Value truncation: reading b1nix.kvtest into a 4-byte buffer → "abc\0". */
  if (!get_kv(cmdline, "b1nix.kvtest", small, sizeof(small)) ||
      strcmp(small, "abc") != 0)
    ok = 0;

  marker(ok ? "M27-CMDLINE: ok kv-parse\n" : "M27-CMDLINE: fail kv-parse\n");
  return ok ? 0 : 1;
}
