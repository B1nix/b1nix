/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_CRYPTO_HASH_H
#define LKPI_CRYPTO_HASH_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/err.h>

/*
 * Synchronous hashes, as btrfs asks for them.
 *
 * btrfs picks its checksum algorithm from the superblock — crc32c, xxhash64,
 * sha256 or blake2b — and then treats it entirely through this interface: one
 * `crypto_shash` per filesystem, allocated by name at mount, and a
 * `shash_desc` on the stack per checksum. The digest size is fixed at 32 bytes
 * on disk regardless of which algorithm produced it, with the shorter ones
 * zero-padded, so the *interface* is uniform even though the algorithms are
 * not.
 *
 * This is a small dispatch table rather than Linux's crypto framework: no
 * templates, no async, no priorities, no fallbacks. `crypto_alloc_shash`
 * matches a name against the algorithms compiled in and returns ERR_PTR(-ENOENT)
 * for anything else — which is the honest answer, and one btrfs handles: it
 * refuses to mount a filesystem whose checksum algorithm it cannot compute
 * rather than mounting it and reading garbage.
 */

struct crypto_shash;

/*
 * The per-operation state.
 *
 * `__ctx` is a flexible array upstream and every algorithm keeps its running
 * state there, which is why SHASH_DESC_ON_STACK has to size the allocation from
 * the algorithm rather than from the struct. The same arrangement is kept here,
 * because imported code declares its descriptors with that macro and reads
 * `desc->tfm` directly.
 */
struct shash_desc {
	struct crypto_shash *tfm;
	void *__ctx[] __attribute__((aligned(8)));
};

/* The largest state any algorithm here needs, in bytes. blake2b's is the
 * biggest: two 128-byte buffers plus the chain. Checked against each
 * algorithm's own descsize at registration, so adding an algorithm with a
 * larger state is a startup failure rather than a stack overflow. */
#define SHASH_MAX_DESCSIZE 384

#define SHASH_DESC_ON_STACK(shash, ctx)                                        \
	char __##shash##_desc[sizeof(struct shash_desc) + SHASH_MAX_DESCSIZE]      \
		__attribute__((aligned(8)));                                           \
	struct shash_desc *shash = (struct shash_desc *)__##shash##_desc

struct crypto_shash *crypto_alloc_shash(const char *alg_name, u32 type,
                                        u32 mask);
void crypto_free_shash(struct crypto_shash *tfm);

unsigned int crypto_shash_descsize(struct crypto_shash *tfm);
unsigned int crypto_shash_digestsize(struct crypto_shash *tfm);
const char *crypto_shash_driver_name(struct crypto_shash *tfm);
const char *crypto_shash_alg_name(struct crypto_shash *tfm);

int crypto_shash_init(struct shash_desc *desc);
int crypto_shash_update(struct shash_desc *desc, const u8 *data,
                        unsigned int len);
int crypto_shash_final(struct shash_desc *desc, u8 *out);
int crypto_shash_finup(struct shash_desc *desc, const u8 *data,
                       unsigned int len, u8 *out);
/* init + update + final in one call, which is what btrfs uses for everything
 * except the multi-page metadata checksum. */
int crypto_shash_digest(struct shash_desc *desc, const u8 *data,
                        unsigned int len, u8 *out);

#endif
