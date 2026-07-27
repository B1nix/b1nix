#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#define MAX_LINE 512

static void emit(const char *s) { write(1, s, strlen(s)); }

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  emit("M26-SELFHOST-USER: start\n");

  int fd = open("/mnt/build/srcs.txt", O_RDONLY);
  if (fd < 0) {
    emit("M26-SELFHOST-USER: fail no-srcs\n");
    return 1;
  }

  char buf[65536];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    emit("M26-SELFHOST-USER: fail read-srcs\n");
    return 1;
  }
  buf[n] = 0;

  int total = 0, failed = 0;
  char *p = buf;
  while (*p) {
    char *eol = p;
    while (*eol && *eol != '\n') eol++;
    char saved_eol = *eol;
    *eol = 0;

    char *tab = p;
    while (*tab && *tab != '\t') tab++;
    if (*tab == '\t') {
      *tab = 0;
      const char *src_rel = p;
      const char *obj_abs = tab + 1;
      if (*src_rel && *obj_abs) {
        char src_abs[256];
        snprintf(src_abs, sizeof(src_abs), "/mnt/build/src/%s", src_rel);

        pid_t pid = fork();
        if (pid == 0) {
          execl("/mnt/build/bin/clang", "/mnt/build/bin/clang",
                "-no-canonical-prefixes", "-B/mnt/build/bin",
                "-resource-dir", "/mnt/build/lib/clang/22",
                "-integrated-as", "-std=c11", "-ffreestanding", "-fno-builtin",
                "-fno-stack-protector", "-fno-pic", "-mno-red-zone",
                "-I/mnt/build/src/kernel/include",
                "-I/mnt/build/src/build/x86_64",
                "-mcmodel=kernel",
                "-mno-sse", "-mno-mmx", "-mno-sse2", "-mno-3dnow",
                "-c", src_abs, "-o", obj_abs,
                (char *)0);
          _exit(127);
        }
        if (pid > 0) {
          int st = 0;
          waitpid(pid, &st, 0);
          total++;
          if (st != 0) {
            failed++;
            char msg[256];
            snprintf(msg, sizeof(msg), "M26-SELFHOST-USER: cc-fail %s\n", src_rel);
            emit(msg);
          }
          if ((total % 10) == 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "M26-SELFHOST-USER: progress %d compiled\n", total);
            emit(msg);
          }
        } else {
          failed++;
          total++;
        }
      }
    }
    *eol = saved_eol;
    p = (*eol) ? eol + 1 : eol;
  }

  char sb[80];
  snprintf(sb, sizeof(sb), "M26-SELFHOST-USER: compiled %d/%d (failed=%d)\n",
           total - failed, total, failed);
  emit(sb);

  if (failed == 0 && total > 0) {
    emit("M26-SELFHOST-USER: ok compile\n");

    pid_t lpid = fork();
    if (lpid == 0) {
      execl("/mnt/build/bin/ld.lld", "/mnt/build/bin/ld.lld",
            "-m", "elf_x86_64", "-z", "max-page-size=0x1000",
            "-T", "/mnt/build/src/kernel/arch/x86_64/linker.ld",
            "-o", "/mnt/build/kernel.elf",
            "@/mnt/build/kernel.rsp",
            (char *)0);
      _exit(127);
    }
    int lst = -1;
    if (lpid > 0) {
      waitpid(lpid, &lst, 0);
    }

    unsigned char mag[4] = {0};
    int ofd = open("/mnt/build/kernel.elf", O_RDONLY);
    if (ofd >= 0) {
      read(ofd, mag, sizeof(mag));
      close(ofd);
    }
    snprintf(sb, sizeof(sb), "M26-SELFHOST-USER: link exit=%d magic=%02x%02x%02x%02x\n",
             lst, mag[0], mag[1], mag[2], mag[3]);
    emit(sb);
    if (lpid > 0 && mag[0] == 0x7f && mag[1] == 'E' && mag[2] == 'L' && mag[3] == 'F')
      emit("M26-SELFHOST-USER: ok kernel-elf\n");
    else
      emit("M26-SELFHOST-USER: fail link\n");
  } else {
    emit("M26-SELFHOST-USER: fail compile\n");
  }

  return 0;
}
