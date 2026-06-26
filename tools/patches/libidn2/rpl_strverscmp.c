/* b1nix: self-contained rpl_strverscmp (gnulib leaves it undefined). */
#define VS_ISDIGIT(c) ((c) >= '0' && (c) <= '9')

int rpl_strverscmp(const char *s1, const char *s2) {
  enum { S_N = 0x0, S_I = 0x3, S_F = 0x6, S_Z = 0x9, VS_CMP = 2, VS_LEN = 3 };
  static const unsigned char next_state[] = {
      S_N, S_I, S_Z, S_N, S_I, S_I, S_N, S_F, S_F, S_N, S_F, S_Z};
  static const signed char result_type[] = {
      VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_LEN, VS_CMP, VS_CMP, VS_CMP, VS_CMP,
      VS_CMP, -1, -1, +1, VS_LEN, VS_LEN, +1, VS_LEN, VS_LEN,
      VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_CMP,
      VS_CMP, +1, +1, -1, VS_CMP, VS_CMP, -1, VS_CMP, VS_CMP};
  const unsigned char *p1 = (const unsigned char *)s1;
  const unsigned char *p2 = (const unsigned char *)s2;
  unsigned char c1, c2;
  int state, diff;
  if (p1 == p2) return 0;
  c1 = *p1++; c2 = *p2++;
  state = S_N + ((c1 == '0') + (VS_ISDIGIT(c1) != 0));
  while ((diff = c1 - c2) == 0) {
    if (c1 == '\0') return diff;
    state = next_state[state];
    c1 = *p1++; c2 = *p2++;
    state += (c1 == '0') + (VS_ISDIGIT(c1) != 0);
  }
  state = result_type[state * 3 + ((c2 == '0') + (VS_ISDIGIT(c2) != 0))];
  switch (state) {
  case VS_CMP: return diff;
  case VS_LEN:
    while (VS_ISDIGIT(*p1++))
      if (!VS_ISDIGIT(*p2++)) return 1;
    return VS_ISDIGIT(*p2) ? -1 : diff;
  default: return state;
  }
}
