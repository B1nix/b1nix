/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: CRC-32 and CRC-32C.
 *
 * Two different polynomials with confusingly similar names, and the filesystems
 * use both: CRC-32C (Castagnoli, 0x1EDC6F41, reflected 0x82F63B78) for btrfs
 * and ext4 metadata checksums, CRC-32 (0x04C11DB7, reflected 0xEDB88320) for
 * ext4's directory hashes.
 *
 * Table-driven, one byte at a time, tables built at first use rather than
 * written out as 2 KiB of constants apiece. The generated tables are checked
 * against a known answer by the self-test — a table that is merely
 * self-consistent produces a filesystem that only this kernel can read, which
 * is the failure mode worth guarding against.
 *
 * CRC-16 (0xA001) is here too. It is a third polynomial with a third table, and
 * ext4 needs it for the group descriptor checksum on filesystems without
 * metadata_csum — which is not a legacy case to skip: those images exist, are
 * mounted, and look corrupt if this is wrong.
 *
 * x86-64 has a `crc32` instruction for the Castagnoli polynomial and it is
 * roughly an order of magnitude faster. It is not used yet: it lives in SSE4.2
 * and needs a CPUID check plus a second code path, and the correct slow version
 * has to exist first to check the fast one against.
 */

#include <lkpi/env.h>
#include <linux/crc16.h>
#include <linux/crc32.h>

static u16 crc16_table[256];
static u32 crc32c_table[256];
static u32 crc32_le_table[256];
static int tables_ready;

static void build_table(u32 *table, u32 poly)
{
	for (u32 i = 0; i < 256; i++) {
		u32 c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
		table[i] = c;
	}
}

static void ensure_tables(void)
{
	/*
	 * No lock. Two CPUs racing here both write the same bytes to the same
	 * addresses — the tables are a pure function of the polynomial — so the
	 * only hazard would be a reader seeing a half-built table, and the flag is
	 * set after the tables are complete. A reader that sees the flag unset
	 * simply rebuilds them.
	 */
	if (tables_ready)
		return;
	build_table(crc32c_table, 0x82F63B78u);
	build_table(crc32_le_table, 0xEDB88320u);
	for (u32 i = 0; i < 256; i++) {
		u16 c = (u16)i;

		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (u16)(0xA001u ^ (c >> 1)) : (u16)(c >> 1);
		crc16_table[i] = c;
	}
	tables_ready = 1;
}

u32 crc32c(u32 crc, const void *address, unsigned int length)
{
	const u8 *p = (const u8 *)address;

	ensure_tables();
	while (length--)
		crc = crc32c_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return crc;
}

u32 __crc32c_le(u32 crc, const unsigned char *p, size_t len)
{
	return crc32c(crc, p, (unsigned int)len);
}

u32 crc32_le(u32 crc, const unsigned char *p, size_t len)
{
	ensure_tables();
	while (len--)
		crc = crc32_le_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return crc;
}

u32 crc32_be(u32 crc, const unsigned char *p, size_t len)
{
	/* Big-endian bit order: the same polynomial walked from the top bit down,
	 * computed directly rather than from a second table. Nothing in the
	 * filesystems is on a hot path through it. */
	while (len--) {
		crc ^= (u32)(*p++) << 24;
		for (int i = 0; i < 8; i++)
			crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u)
			                          : (crc << 1);
	}
	return crc;
}

u16 crc16(u16 crc, const u8 *buffer, size_t len)
{
	ensure_tables();
	while (len--)
		crc = (u16)(crc16_table[(crc ^ *buffer++) & 0xFF] ^ (crc >> 8));
	return crc;
}
