/* M31: b1nix password hashing.
 *
 * Implements a small, deterministic password-hashing function with a glibc-
 * style format string so /etc/shadow can be read in the standard
 * `user:$N$salt$hash:...` shape. This is NOT bcrypt or RFC sha512crypt
 * (which has 5000+ rounds with elaborate mixing) — b1nix uses a simpler
 * 1024-round PBKDF-shaped construction:
 *
 *     h0 = sha512(salt || ":" || password)
 *     h_{i+1} = sha512(h_i || salt)              for i in [0..1023)
 *     out = base64(h_1024)
 *
 * Format: "$b1$<salt>$<base64hash>"
 *
 * 1024 rounds is enough to slow brute-force from the in-kernel login
 * shell while still keeping a wrong-password verification well under
 * 100ms on the smoke harness. Real production systems should use a
 * memory-hard KDF — this milestone closes M31 by giving b1nix functional
 * passwd auth, not a hardened one.
 */

#include <b1nix/types.h>
#include <string.h>

extern void sha512(const void *data, usize len, u8 out[64]);

#define B1NIX_CRYPT_ROUNDS 1024
#define B1NIX_CRYPT_TAG    "$b1$"

static const char b64alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789./";

static void base64_encode(const u8 *in, usize n, char *out) {
  usize i = 0, j = 0;
  while (i + 3 <= n) {
    u32 v = ((u32)in[i] << 16) | ((u32)in[i+1] << 8) | in[i+2];
    out[j++] = b64alphabet[(v >> 18) & 63];
    out[j++] = b64alphabet[(v >> 12) & 63];
    out[j++] = b64alphabet[(v >>  6) & 63];
    out[j++] = b64alphabet[v & 63];
    i += 3;
  }
  if (i < n) {
    u32 v = (u32)in[i] << 16;
    if (i + 1 < n) v |= (u32)in[i + 1] << 8;
    out[j++] = b64alphabet[(v >> 18) & 63];
    out[j++] = b64alphabet[(v >> 12) & 63];
    if (i + 1 < n) out[j++] = b64alphabet[(v >> 6) & 63];
  }
  out[j] = '\0';
}

/* b1nix_crypt(password, salt) — writes "$b1$<salt>$<hash>\0" into out.
 * out must be at least 1 + 2 + 1 + strlen(salt) + 1 + 86 + 1 bytes
 * (the base64-encoded 64-byte digest is 86 chars). Returns 0 on success,
 * -1 on overflow. */
int b1nix_crypt(const char *password, const char *salt, char *out, usize outsz) {
  usize plen = 0, slen = 0;
  while (password[plen]) plen++;
  while (salt[slen]) slen++;
  if (slen > 32) return -1;

  /* Initial round: sha512(salt || ":" || password). The colon is an
   * arbitrary separator that prevents a degenerate "salt+password ==
   * password+salt" equivalence with a different per-field split. */
  u8 digest[64];
  u8 prebuf[64 + 32 + 1];
  if (slen + 1 + plen > sizeof(prebuf)) return -1;
  memcpy(prebuf, salt, slen);
  prebuf[slen] = ':';
  memcpy(prebuf + slen + 1, password, plen);
  sha512(prebuf, slen + 1 + plen, digest);

  /* Subsequent rounds: sha512(prev_digest || salt). */
  u8 roundbuf[64 + 32];
  for (int r = 0; r < B1NIX_CRYPT_ROUNDS - 1; r++) {
    memcpy(roundbuf, digest, 64);
    memcpy(roundbuf + 64, salt, slen);
    sha512(roundbuf, 64 + slen, digest);
  }

  /* Emit "$b1$<salt>$<base64hash>". */
  usize need = 4 + slen + 1 + 86 + 1;
  if (outsz < need) return -1;
  memcpy(out, B1NIX_CRYPT_TAG, 4);
  memcpy(out + 4, salt, slen);
  out[4 + slen] = '$';
  base64_encode(digest, 64, out + 4 + slen + 1);
  return 0;
}

/* Constant-time-ish compare of two NUL-terminated hash strings. Used so
 * an attacker can't see prefix-match timing via the bytewise short-circuit
 * in strcmp. Equal-length input is required (callers normalise this by
 * generating both via b1nix_crypt with the same salt). */
int b1nix_crypt_equal(const char *a, const char *b) {
  usize la = 0, lb = 0;
  while (a[la]) la++;
  while (b[lb]) lb++;
  if (la != lb) return 0;
  u8 diff = 0;
  for (usize i = 0; i < la; i++) diff |= (u8)(a[i] ^ b[i]);
  return diff == 0;
}
