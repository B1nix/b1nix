#ifndef B1NIX_CRYPT_H
#define B1NIX_CRYPT_H

/* M31: SHA-512 + b1nix password hash. See kernel/lib/{sha512,crypt}.c. */

#include <b1nix/types.h>

struct sha512_ctx {
  u64 h[8];
  u64 bitlen;
  u8  buf[128];
  u32 buflen;
};

void sha512(const void *data, usize len, u8 out[64]);
void sha512_init(struct sha512_ctx *c);
void sha512_update(struct sha512_ctx *c, const void *data, usize len);
void sha512_final(struct sha512_ctx *c, u8 out[64]);

int b1nix_crypt(const char *password, const char *salt, char *out, usize outsz);
int b1nix_crypt_equal(const char *a, const char *b);

#endif
