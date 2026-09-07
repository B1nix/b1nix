/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: synchronous hashes, as btrfs asks for them.
 *
 * btrfs picks its checksum algorithm from the superblock — crc32c, xxhash64,
 * sha256 or blake2b — and then uses it entirely through this interface: one
 * `crypto_shash` per filesystem, allocated by name at mount, and a `shash_desc`
 * on the stack per checksum. The on-disk field is 32 bytes whatever the
 * algorithm, with the shorter digests zero-padded.
 *
 * This is a small dispatch table, not Linux's crypto framework: no templates,
 * no async, no priorities. Allocating an unknown name FAILS, and that failure
 * is the feature — btrfs refuses to mount a filesystem whose checksum it cannot
 * compute rather than mounting it and reading every block as corrupt.
 *
 * What is implemented: crc32c (the default, and what mkfs.btrfs uses unless
 * told otherwise) and xxhash64. sha256 and blake2b are NOT, and they report
 * that by name — a filesystem made with `mkfs.btrfs --csum sha256` will refuse
 * to mount here, which is the correct outcome and a stated gap.
 */

#include <lkpi/env.h>
#include <lkpi/types.h>
#include <linux/printk.h>
#include <string.h>

/* From kernel/lkpi/crc32.c. */
u32 crc32c(u32 crc, const void *address, unsigned int length);

/* From the imported lib/xxhash.c. Declared here rather than through
 * <linux/xxhash.h> because this file is on the b1nix side of the boundary. */
struct xxh64_state {
	u64 total_len;
	u64 v1, v2, v3, v4;
	u64 mem64[4];
	u32 memsize;
	u32 reserved[2];
};
void xxh64_reset(struct xxh64_state *state, u64 seed);
int xxh64_update(struct xxh64_state *state, const void *input, usize length);
u64 xxh64_digest(const struct xxh64_state *state);

enum lkpi_shash_alg {
	LKPI_SHASH_CRC32C,
	LKPI_SHASH_XXHASH64,
};

struct crypto_shash {
	enum lkpi_shash_alg alg;
	unsigned int digestsize;
	unsigned int descsize;
	const char *name;
	const char *driver_name;
};

/*
 * The running state, which lives in the descriptor's flexible tail.
 *
 * A union so a descriptor can be declared before the algorithm is known;
 * SHASH_DESC_ON_STACK reserves SHASH_MAX_DESCSIZE and does not depend on it.
 * What `crypto_shash_descsize` reports is NOT this union but the size of the
 * one member the chosen algorithm uses: ext4 declares its descriptor as a
 * shash_desc followed by exactly four bytes and asserts that the driver agrees
 * (BUG_ON in ext4_chksum), then seeds those bytes with the running crc and
 * reads the result straight back out of them. Reporting the union's size there
 * both fails that assertion and describes a state twice the size of the space
 * the caller actually reserved.
 */
union lkpi_shash_state {
	u32 crc;
	struct xxh64_state xxh;
};

struct lkpi_shash_desc {
	struct crypto_shash *tfm;
	union lkpi_shash_state st;
};

static struct crypto_shash shash_crc32c = {
	.alg = LKPI_SHASH_CRC32C,
	.digestsize = 4,
	.descsize = sizeof(u32),
	.name = "crc32c",
	.driver_name = "crc32c-b1nix",
};

static struct crypto_shash shash_xxhash64 = {
	.alg = LKPI_SHASH_XXHASH64,
	.digestsize = 8,
	.descsize = sizeof(struct xxh64_state),
	.name = "xxhash64",
	.driver_name = "xxhash64-b1nix",
};

struct crypto_shash *crypto_alloc_shash(const char *alg_name, u32 type, u32 mask)
{
	(void)type;
	(void)mask;
	if (!alg_name)
		return (struct crypto_shash *)(long)-22; /* ERR_PTR(-EINVAL) */
	if (!strcmp(alg_name, "crc32c"))
		return &shash_crc32c;
	if (!strcmp(alg_name, "xxhash64"))
		return &shash_xxhash64;
	/*
	 * sha256 and blake2b are the two btrfs also offers, and neither is
	 * implemented. Naming them in the log is worth the two lines: the caller
	 * turns this into a mount failure whose message is only "failed to init
	 * csum driver", and the reason belongs where somebody will read it.
	 */
	pr_warn("lkpi: no shash implementation for this algorithm; a filesystem "
	          "using it cannot be mounted");
	pr_warn("%s\n", alg_name);
	return (struct crypto_shash *)(long)-2; /* ERR_PTR(-ENOENT) */
}

void crypto_free_shash(struct crypto_shash *tfm)
{
	/* The transforms are static: there is one per algorithm, shared by every
	 * filesystem that asks for it, and none of them carries per-caller state
	 * (that lives in the descriptor). So there is nothing to free. */
	(void)tfm;
}

unsigned int crypto_shash_descsize(struct crypto_shash *tfm)
{
	return tfm ? tfm->descsize : 0;
}

unsigned int crypto_shash_digestsize(struct crypto_shash *tfm)
{
	return tfm ? tfm->digestsize : 0;
}

const char *crypto_shash_driver_name(struct crypto_shash *tfm)
{
	return tfm ? tfm->driver_name : "none";
}

const char *crypto_shash_alg_name(struct crypto_shash *tfm)
{
	return tfm ? tfm->name : "none";
}

int crypto_shash_init(struct lkpi_shash_desc *desc)
{
	if (!desc || !desc->tfm)
		return -22;
	switch (desc->tfm->alg) {
	case LKPI_SHASH_CRC32C:
		/*
		 * Starts from ~0, and the result is inverted at the end. That is the
		 * convention btrfs's on-disk checksum was computed with; starting
		 * from 0 produces a different value for the same bytes, and every
		 * block would read as corrupt.
		 */
		desc->st.crc = ~0u;
		break;
	case LKPI_SHASH_XXHASH64:
		/* btrfs seeds xxhash with zero. */
		xxh64_reset(&desc->st.xxh, 0);
		break;
	}
	return 0;
}

int crypto_shash_update(struct lkpi_shash_desc *desc, const u8 *data,
                        unsigned int len)
{
	if (!desc || !desc->tfm)
		return -22;
	switch (desc->tfm->alg) {
	case LKPI_SHASH_CRC32C:
		desc->st.crc = crc32c(desc->st.crc, data, len);
		break;
	case LKPI_SHASH_XXHASH64:
		xxh64_update(&desc->st.xxh, data, len);
		break;
	}
	return 0;
}

int crypto_shash_final(struct lkpi_shash_desc *desc, u8 *out)
{
	if (!desc || !desc->tfm || !out)
		return -22;
	switch (desc->tfm->alg) {
	case LKPI_SHASH_CRC32C: {
		u32 v = ~desc->st.crc;

		/* Little-endian, because that is how it sits on disk. */
		out[0] = (u8)(v);
		out[1] = (u8)(v >> 8);
		out[2] = (u8)(v >> 16);
		out[3] = (u8)(v >> 24);
		break;
	}
	case LKPI_SHASH_XXHASH64: {
		u64 v = xxh64_digest(&desc->st.xxh);
		int i;

		for (i = 0; i < 8; i++)
			out[i] = (u8)(v >> (8 * i));
		break;
	}
	}
	return 0;
}

int crypto_shash_finup(struct lkpi_shash_desc *desc, const u8 *data,
                       unsigned int len, u8 *out)
{
	int ret = crypto_shash_update(desc, data, len);

	if (ret)
		return ret;
	return crypto_shash_final(desc, out);
}

int crypto_shash_digest(struct lkpi_shash_desc *desc, const u8 *data,
                        unsigned int len, u8 *out)
{
	int ret = crypto_shash_init(desc);

	if (ret)
		return ret;
	return crypto_shash_finup(desc, data, len, out);
}
