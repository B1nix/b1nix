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
