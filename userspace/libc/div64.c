#include <stdint.h>
#include <stddef.h>

#ifndef __x86_64__

uint64_t __udivmod64(uint64_t num, uint64_t den, uint64_t *rem_p) {
    if (den == 0) {
        if (rem_p) *rem_p = 0;
        return 0;
    }
    uint64_t quot = 0;
    uint64_t rem = 0;
    for (int i = 63; i >= 0; i--) {
        rem = (rem << 1) | ((num >> i) & 1);
        if (rem >= den) {
            rem -= den;
            quot |= (1ULL << i);
        }
    }
    if (rem_p) *rem_p = rem;
    return quot;
}

uint64_t __udivdi3(uint64_t num, uint64_t den) {
    return __udivmod64(num, den, NULL);
}

uint64_t __umoddi3(uint64_t num, uint64_t den) {
    uint64_t rem;
    __udivmod64(num, den, &rem);
    return rem;
}

int64_t __divdi3(int64_t num, int64_t den) {
    int minus = 0;
    uint64_t n = num;
    uint64_t d = den;
    if (num < 0) {
        n = -num;
        minus ^= 1;
    }
    if (den < 0) {
        d = -den;
        minus ^= 1;
    }
    uint64_t q = __udivmod64(n, d, NULL);
    return minus ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t num, int64_t den) {
    int minus = 0;
    uint64_t n = num;
    uint64_t d = den;
    if (num < 0) {
        n = -num;
        minus ^= 1;
    }
    if (den < 0) {
        d = -den;
    }
    uint64_t r;
    __udivmod64(n, d, &r);
    return minus ? -(int64_t)r : (int64_t)r;
}

#endif
