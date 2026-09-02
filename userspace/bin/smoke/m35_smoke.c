/* M35 core-dump smoke. Forks a child that dereferences an unmapped address;
 * the kernel's fatal-signal path writes an ELF core to /tmp/core before the
 * child dies. The parent then validates that core:
 *
 *   - child terminated by a signal (not a clean exit);
 *   - /tmp/core begins with the ELF magic;
 *   - e_type == ET_CORE, e_machine == EM_X86_64;
 *   - it carries at least one program header and a PT_NOTE segment.
 *
 * Markers (`M35-CORE: ok <name>`) are consumed by tests/smoke.sh. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

static void ok(const char *name) {
  char buf[128];
  int n = 0;
  const char *p = "M35-CORE: ok ";
  while (*p) buf[n++] = *p++;
  while (*name) buf[n++] = *name++;
  buf[n++] = '\n';
  write(1, buf, n);
}

static void fail(const char *name) {
  char buf[128];
  int n = 0;
  const char *p = "M35-CORE: FAIL ";
  while (*p) buf[n++] = *p++;
  while (*name) buf[n++] = *name++;
  buf[n++] = '\n';
  write(1, buf, n);
}

/* ELF header/phdr layout differs between the 64-bit and 32-bit cores the kernel
 * writes (ELF64 vs ELF32). Select the right offsets, machine type, and program
 * header stride per arch so the same checker validates both. */
#ifdef __x86_64__
#define CORE_EM       62 /* EM_X86_64 */
#define OFF_E_PHOFF   32 /* Elf64_Ehdr.e_phoff */
#define OFF_E_PHNUM   56 /* Elf64_Ehdr.e_phnum */
#define PHDR_SIZE     56 /* sizeof(Elf64_Phdr) */
#elif defined(__aarch64__)
#define CORE_EM       183 /* EM_AARCH64 */
#define OFF_E_PHOFF   32  /* Elf64_Ehdr.e_phoff */
#define OFF_E_PHNUM   56  /* Elf64_Ehdr.e_phnum */
#define PHDR_SIZE     56  /* sizeof(Elf64_Phdr) */
#else
#define CORE_EM       3  /* EM_386 */
#define OFF_E_PHOFF   28 /* Elf32_Ehdr.e_phoff */
#define OFF_E_PHNUM   44 /* Elf32_Ehdr.e_phnum */
#define PHDR_SIZE     32 /* sizeof(Elf32_Phdr) */
#endif

static unsigned rd16(const unsigned char *p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static unsigned rd32(const unsigned char *p) {
  return rd16(p) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

int main(void) {
  emit("M35-DIAG: start\n");

  /* Real test: read /proc/kallsyms and verify known symbols resolve. */
  int kfd = open("/proc/kallsyms", O_RDONLY);
  if (kfd < 0) {
    emit("M35-DIAG: FAIL kallsyms-open\n");
    return 1;
  }
  char buf[8192];
  ssize_t kn = read(kfd, buf, sizeof(buf) - 1);
  close(kfd);
  if (kn <= 0) {
    emit("M35-DIAG: FAIL kallsyms-read\n");
    return 1;
  }
  buf[kn] = '\0';

  /* Check 1: kallsyms contains at least one symbol with a non-zero address. */
  int found_any = 0;
  for (ssize_t i = 0; i < kn - 18; i++) {
    if (buf[i] != '0' && buf[i] >= '1' && buf[i] <= '9') {
      found_any = 1;
      break;
    }
  }
  if (!found_any) {
    emit("M35-DIAG: FAIL kallsyms (no symbols)\n");
    return 1;
  }
  emit("M35-DIAG: ok kallsyms\n");

  /* Check 2: known symbol "kernel_main" exists at some address. */
  if (!strstr(buf, "kernel_main")) {
    emit("M35-DIAG: FAIL kallsyms-offset\n");
    return 1;
  }
  emit("M35-DIAG: ok kallsyms-offset\n");

  /* Check 3: a second distinct symbol "panic" also exists. */
  if (!strstr(buf, " panic\n") && !strstr(buf, " panic\t")) {
    int found = 0;
    for (ssize_t i = 0; i < kn; i++) {
      if ((i == 0 || buf[i-1] == ' ' || buf[i-1] == '\t') &&
          strncmp(buf + i, "panic", 5) == 0 &&
          (i + 5 >= kn || buf[i+5] == ' ' || buf[i+5] == '\t' || buf[i+5] == '\n')) {
        found = 1;
        break;
      }
    }
    if (!found) {
      emit("M35-DIAG: FAIL kallsyms-multi\n");
      return 1;
    }
  }
  emit("M35-DIAG: ok kallsyms-multi\n");
  emit("M35-DIAG: done\n");

  emit("M35-CORE: start\n");

  int pid = fork();
  if (pid < 0) {
    fail("fork");
    return 1;
  }
  if (pid == 0) {
    /* Child: fault on an unmapped address (below the 0x400000 load base). */
    volatile long *bad = (volatile long *)0xdead0000UL;
    *bad = 1;
    _exit(0); /* unreachable */
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFSIGNALED(status)) {
    fail("crash-signal");
    return 1;
  }
  ok("crash-signal");

  /* Read back the core the kernel wrote. */
  int fd = open("/tmp/core", O_RDONLY);
  if (fd < 0) {
    fail("core-open");
    return 1;
  }
  unsigned char hdr[64];
  int n = read(fd, hdr, sizeof(hdr));
  if (n < 64 || hdr[0] != 0x7f || hdr[1] != 'E' || hdr[2] != 'L' ||
      hdr[3] != 'F') {
    close(fd);
    fail("core-elf");
    return 1;
  }
  unsigned e_type = rd16(hdr + 16);
  unsigned e_machine = rd16(hdr + 18);
  unsigned e_phnum = rd16(hdr + OFF_E_PHNUM);
  unsigned long e_phoff = (unsigned long)rd32(hdr + OFF_E_PHOFF); /* low 32 bits suffice */
  if (e_type != 4 /*ET_CORE*/ || e_machine != CORE_EM ||
      e_phnum < 1) {
    close(fd);
    fail("core-elf");
    return 1;
  }
  ok("core-elf");

  /* Walk the program headers looking for PT_NOTE (the NT_PRSTATUS reg file). */
  int found_note = 0;
  for (unsigned i = 0; i < e_phnum && i < 64; i++) {
    unsigned char ph[PHDR_SIZE];
    lseek(fd, (long)(e_phoff + (unsigned long)i * PHDR_SIZE), SEEK_SET);
    if (read(fd, ph, PHDR_SIZE) != PHDR_SIZE)
      break;
    if (rd32(ph) == 4 /*PT_NOTE*/)
      found_note = 1;
  }
  close(fd);
  if (!found_note) {
    fail("core-prstatus");
    return 1;
  }
  ok("core-prstatus");

  emit("M35-CORE: done\n");
  return 0;
}
