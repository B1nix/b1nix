#if defined(__x86_64__) || defined(__aarch64__)
#include <stdint.h>

typedef unsigned __int128 u128;

static u128 udivmod128(u128 n, u128 d, u128 *rem) {
  if (d == 0) {
    if (rem) *rem = 0;
    return 0;
  }
  u128 q = 0;
  u128 r = 0;
  for (int i = 127; i >= 0; --i) {
    int carry = (int)(r >> 127);
    r = (r << 1) | ((n >> i) & 1);
    if (carry || r >= d) {
      r -= d;
      q |= ((u128)1 << i);
    }
  }
  if (rem) *rem = r;
  return q;
}

u128 __udivti3(u128 n, u128 d) {
  return udivmod128(n, d, 0);
}

u128 __umodti3(u128 n, u128 d) {
  u128 r;
  (void)udivmod128(n, d, &r);
  return r;
}
#endif

