/*
 * M92: Raw diagnostic — no libc, no mallocng. Tests mmap/brk directly.
 * Uses Linux x86_64 syscall numbers via inline asm.
 */
static long sys(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2),
                      "r"(a3), "r"(a4), "r"(a5) : "rcx", "r11", "memory");
    return ret;
}

static int slen(const char *s) { int n=0; while(s[n])n++; return n; }
static void msg(const char *s) { sys(1, 1, (long)s, slen(s), 0, 0, 0); }
static void write_hex(unsigned long v) {
    char buf[18]="0x"; const char *h="0123456789abcdef";
    for(int i=15;i>=0;i--) buf[2+15-i]=h[(v>>(i*4))&0xf];
    sys(1, 1, (long)buf, 18, 0, 0, 0);
}

int main(void) {
    msg("M92-RAW: start\n");

    /* 1. brk(0) to get current break */
    long brk0 = sys(12, 0, 0, 0, 0, 0, 0);
    msg("M92-RAW: brk(0) = "); write_hex(brk0); msg("\n");

    /* 2. brk(brk0 + 4096) to extend */
    long brk1 = sys(12, brk0 + 4096, 0, 0, 0, 0, 0);
    msg("M92-RAW: brk(+4096) = "); write_hex(brk1); msg("\n");

    /* 3. mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) */
    long p = sys(9, 0, 4096, 3, 0x22, -1, 0);
    msg("M92-RAW: mmap(4096) = "); write_hex(p); msg("\n");
    if (p > 0 && p < 0x7fffffffffffL) {
        *(volatile char*)p = 'X';
        msg("M92-RAW: mmap write ok\n");
        sys(11, p, 4096, 0, 0, 0, 0); /* munmap */
    } else {
        msg("M92-RAW: mmap FAILED\n");
    }

    /* 4. mmap a larger region like mallocng does (128K) */
    long p2 = sys(9, 0, 0x20000, 3, 0x22, -1, 0);
    msg("M92-RAW: mmap(128K) = "); write_hex(p2); msg("\n");
    if (p2 > 0 && p2 < 0x7fffffffffffL) {
        /* Write to first and last byte */
        *(volatile char*)p2 = 'A';
        *(volatile char*)(p2 + 0x1FFF) = 'B';
        msg("M92-RAW: mmap 128K write ok\n");
        sys(11, p2, 0x20000, 0, 0, 0, 0); /* munmap */
    } else {
        msg("M92-RAW: mmap 128K FAILED\n");
    }

    /* 5. Multiple small mmaps (mallocng pattern) */
    long addrs[4];
    for (int i = 0; i < 4; i++) {
        addrs[i] = sys(9, 0, 4096, 3, 0x22, -1, 0);
        msg("M92-RAW: mmap #"); sys(1,1,(long)(i+'0'),1,0,0,0);
        msg(" = "); write_hex(addrs[i]); msg("\n");
        if (addrs[i] > 0 && addrs[i] < 0x7fffffffffffL)
            *(volatile char*)addrs[i] = 'C';
    }
    for (int i = 0; i < 4; i++) {
        if (addrs[i] > 0 && addrs[i] < 0x7fffffffffffL)
            sys(11, addrs[i], 4096, 0, 0, 0, 0);
    }

    /* 6. Test mmap with MAP_FIXED (like mallocng's internal use) */
    long p3 = sys(9, 0x100000000L, 0x1000, 3, 0x32, -1, 0); /* MAP_FIXED|PRIVATE|ANON */
    msg("M92-RAW: mmap(MAP_FIXED @1G) = "); write_hex(p3); msg("\n");

    /* 7. Check syscall numbers */
    /* getrandom: NR=318 */
    char rnd[16] = {0};
    long gr = sys(318, (long)rnd, 16, 0, 0, 0, 0);
    msg("M92-RAW: getrandom = "); write_hex(gr); msg("\n");
    msg("M92-RAW: random bytes: ");
    for(int i=0;i<8;i++){sys(1,1,(long)(&"0123456789abcdef"[(rnd[i]>>4)&0xf]),1,0,0,0);
                          sys(1,1,(long)(&"0123456789abcdef"[rnd[i]&0xf]),1,0,0,0);}
    msg("\n");

    /* 8. Test that the stack is accessible */
    volatile int stack_var = 42;
    msg("M92-RAW: stack var = "); 
    char sv[4] = {'0'+(stack_var/10), '0'+(stack_var%10), '\n', 0};
    sys(1, 1, (long)sv, 3, 0, 0, 0);

    msg("M92-RAW: done\n");
    return 0;
}
