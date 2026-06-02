#ifndef B1NIX_U_CRYPT_H
#define B1NIX_U_CRYPT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* POSIX crypt(): hashes `key` using the salt/scheme parsed from `setting`
 * (b1nix "$b1$<salt>$..." format) and returns the full "$b1$<salt>$<hash>"
 * in a static buffer. Returns NULL on malformed input. */
char *crypt(const char *key, const char *setting);

/* b1nix native helper (also used by the in-kernel login built-in). */
int b1nix_crypt(const char *password, const char *salt, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif
