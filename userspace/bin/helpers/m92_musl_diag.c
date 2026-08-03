/*
 * M92: Diagnostic test — prints auxv values and tests syscalls individually.
 * Uses only raw Linux x86_64 syscalls (no libc) to avoid malloc.
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/auxv.h>

static int slen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void raw_write(const char *s) {
    write(1, s, slen(s));
}

static void write_hex(unsigned long v) {
    char buf[20] = "0x0000000000000000";
    const char *hex = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        buf[2 + i] = hex[v & 0xf];
        v >>= 4;
    }
    write(1, buf, 18);
}

static void write_dec(unsigned long v) {
    char buf[20];
    int i = 19;
    buf[i] = 0;
    if (v == 0) { buf[--i] = '0'; }
    else { while (v) { buf[--i] = '0' + (v % 10); v /= 10; } }
    write(1, buf + i, 19 - i);
}

static long sys_write(int fd, const void *buf, unsigned long count) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(1), "D"(fd), "S"(buf), "d"(count) : "rcx", "r11", "memory");
    return ret;
}

static long sys_raw(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(a3), "r"(a4), "r"(a5) : "rcx", "r11", "memory");
    return ret;
}

#define TEST(name, expr) do { \
    raw_write("M92-DIAG: "); \
    raw_write(name); \
    raw_write(" = "); \
    unsigned long _r = (unsigned long)(long)(expr); \
    write_hex(_r); \
    raw_write(" ("); \
    write_dec(_r); \
    raw_write(")\n"); \
} while(0)

int main(int argc, char *argv[], char *envp[]) {
    raw_write("M92-DIAG: start\n");

    /* Print auxv values */
    TEST("AT_PHDR", getauxval(AT_PHDR));
    TEST("AT_PHENT", getauxval(AT_PHENT));
    TEST("AT_PHNUM", getauxval(AT_PHNUM));
    TEST("AT_ENTRY", getauxval(AT_ENTRY));
    TEST("AT_PAGESZ", getauxval(AT_PAGESZ));
    TEST("AT_BASE", getauxval(AT_BASE));
    TEST("AT_CLKTCK", getauxval(AT_CLKTCK));
    TEST("AT_HWCAP", getauxval(AT_HWCAP));
    TEST("AT_SECURE", getauxval(AT_SECURE));
    TEST("AT_RANDOM", getauxval(AT_RANDOM));
    TEST("argc", (long)argc);

    /* Test raw syscalls with Linux NR */
    raw_write("M92-DIAG: raw syscalls\n");
    TEST("write(1,'.',1)", sys_write(1, ".", 1));
    TEST("brk(0) [NR=12]", sys_raw(12, 0, 0, 0, 0, 0, 0));
    {
        long old = sys_raw(12, 0, 0, 0, 0, 0, 0);
        TEST("brk(+4096)", sys_raw(12, old + 4096, 0, 0, 0, 0, 0));
    }
    /* mmap: NR=9, addr=0, len=4096, prot=3, flags=0x22, fd=-1, off=0 */
    long p = sys_raw(9, 0, 4096, 3, 0x22, -1, 0);
    TEST("mmap", p);
    if (p > 0 && p < 0x7fffffffffff) {
        *(volatile char*)p = 'A';
        TEST("mmap write", *(volatile char*)p);
        sys_raw(11, p, 4096, 0, 0, 0, 0); /* munmap */
    }
    /* clock_gettime: NR=228, clk=1(CLOCK_MONOTONIC), ts_ptr */
    unsigned long ts[2] = {0};
    TEST("clock_gettime [NR=228]", sys_raw(228, 1, (long)ts, 0, 0, 0, 0));
    TEST("clock.tv_sec", ts[0]);
    /* getpid: NR=39 */
    TEST("getpid [NR=39]", sys_raw(39, 0, 0, 0, 0, 0, 0));
    /* arch_prctl: NR=158, code=0x1002(ARCH_GET_FS) */
    unsigned long fs_val = 0;
    TEST("arch_prctl(GET_FS) [NR=158]", sys_raw(158, 0x1003, (long)&fs_val, 0, 0, 0, 0));
    TEST("FS base", fs_val);

    raw_write("M92-DIAG: done\n");
    return 0;
}
