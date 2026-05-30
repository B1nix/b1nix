/* M31: SHA-512 (FIPS 180-4) for password verification in the in-kernel
 * login built-in. Standard 80-round Merkle-Damgård with 64-bit words.
 * Not constant-time — only used to verify static, attacker-known shadow
 * hashes during login, so timing side-channels against the bytewise
 * compare in login_main aren't load-bearing.
 *
 * Public API:
 *   sha512(msg, len, out64)            — one-shot
 *   sha512_init / update / final       — streaming
 *
 * No floating-point (kernel is -mno-sse), no memory allocation. Caller
 * provides the 64-byte output buffer. */

#include <b1nix/types.h>
#include <string.h>

struct sha512_ctx {
  u64 h[8];
  u64 bitlen;     /* total message bits */
  u8  buf[128];
  u32 buflen;
};

void sha512_init(struct sha512_ctx *c);
void sha512_update(struct sha512_ctx *c, const void *data, usize len);
void sha512_final(struct sha512_ctx *c, u8 out[64]);
void sha512(const void *data, usize len, u8 out[64]);

static const u64 K[80] = {
  0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
  0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
  0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
  0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
  0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
  0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
  0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
  0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
  0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
  0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
  0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
  0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
  0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
  0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
  0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
  0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
  0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
  0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
  0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
  0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static inline u64 rotr64(u64 x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha512_transform(struct sha512_ctx *c, const u8 *block) {
  u64 w[80];
  for (int i = 0; i < 16; i++) {
    w[i] = ((u64)block[i*8 + 0] << 56) | ((u64)block[i*8 + 1] << 48) |
           ((u64)block[i*8 + 2] << 40) | ((u64)block[i*8 + 3] << 32) |
           ((u64)block[i*8 + 4] << 24) | ((u64)block[i*8 + 5] << 16) |
           ((u64)block[i*8 + 6] <<  8) | ((u64)block[i*8 + 7]);
  }
  for (int i = 16; i < 80; i++) {
    u64 s0 = rotr64(w[i-15], 1) ^ rotr64(w[i-15], 8) ^ (w[i-15] >> 7);
    u64 s1 = rotr64(w[i-2], 19) ^ rotr64(w[i-2], 61) ^ (w[i-2] >> 6);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }

  u64 a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
  u64 e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];

  for (int i = 0; i < 80; i++) {
    u64 S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
    u64 ch = (e & f) ^ (~e & g);
    u64 t1 = h + S1 + ch + K[i] + w[i];
    u64 S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
    u64 mj = (a & b) ^ (a & cc) ^ (b & cc);
    u64 t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1;
    d = cc; cc = b; b = a; a = t1 + t2;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
  c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha512_init(struct sha512_ctx *c) {
  c->h[0] = 0x6a09e667f3bcc908ULL;
  c->h[1] = 0xbb67ae8584caa73bULL;
  c->h[2] = 0x3c6ef372fe94f82bULL;
  c->h[3] = 0xa54ff53a5f1d36f1ULL;
  c->h[4] = 0x510e527fade682d1ULL;
  c->h[5] = 0x9b05688c2b3e6c1fULL;
  c->h[6] = 0x1f83d9abfb41bd6bULL;
  c->h[7] = 0x5be0cd19137e2179ULL;
  c->bitlen = 0;
  c->buflen = 0;
}

void sha512_update(struct sha512_ctx *c, const void *data, usize len) {
  const u8 *p = (const u8 *)data;
  c->bitlen += (u64)len * 8;
  while (len > 0) {
    u32 take = 128 - c->buflen;
    if (take > len) take = (u32)len;
    memcpy(c->buf + c->buflen, p, take);
    c->buflen += take;
    p += take;
    len -= take;
    if (c->buflen == 128) {
      sha512_transform(c, c->buf);
      c->buflen = 0;
    }
  }
}

void sha512_final(struct sha512_ctx *c, u8 out[64]) {
  /* Pad: 0x80, then zeros, then 16-byte big-endian bit length. */
  c->buf[c->buflen++] = 0x80;
  if (c->buflen > 112) {
    while (c->buflen < 128) c->buf[c->buflen++] = 0;
    sha512_transform(c, c->buf);
    c->buflen = 0;
  }
  while (c->buflen < 112) c->buf[c->buflen++] = 0;
  /* Upper 64 bits of message length are zero — we don't support >2^64 bits. */
  for (int i = 0; i < 8; i++) c->buf[112 + i] = 0;
  for (int i = 0; i < 8; i++) {
    c->buf[120 + i] = (u8)(c->bitlen >> (56 - i * 8));
  }
  sha512_transform(c, c->buf);
  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 8; j++) {
      out[i * 8 + j] = (u8)(c->h[i] >> (56 - j * 8));
    }
  }
}

void sha512(const void *data, usize len, u8 out[64]) {
  struct sha512_ctx c;
  sha512_init(&c);
  sha512_update(&c, data, len);
  sha512_final(&c, out);
}
