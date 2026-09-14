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
 * CRC-32C also has an instruction on both architectures -- x86-64's `crc32`
 * (SSE4.2 in CPUID, but it works on general-purpose registers and touches no
 * vector state) and ARMv8's CRC32 extension. btrfs checksums every data block
 * it reads, and a desktop start-up reads a quarter of a gigabyte, so the byte
 * loop showed up in the kernel profile. The instruction is used only after it
 * has given the same answer as the table on a test buffer.
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

static u32 crc32c_table_loop(u32 crc, const u8 *p, unsigned int length)
{
	while (length--)
		crc = crc32c_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return crc;
}

#if defined(__x86_64__)
static int crc32c_hw_present(void)
{
	u32 a, b, c, d;

	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
	(void)a; (void)b; (void)d;
	return (c >> 20) & 1; /* SSE4.2 */
}

static u32 crc32c_hw(u32 crc, const u8 *p, unsigned int length)
{
	u64 c = crc;

	while (length >= 8) {
		u64 v = (u64)p[0] | (u64)p[1] << 8 | (u64)p[2] << 16 | (u64)p[3] << 24 |
		        (u64)p[4] << 32 | (u64)p[5] << 40 | (u64)p[6] << 48 |
		        (u64)p[7] << 56;
		u64 out;

		__asm__ volatile("movq %1, %0\n\tcrc32q %2, %0"
		                 : "=r"(out) : "r"(c), "r"(v));
		c = out;
		p += 8;
		length -= 8;
	}
	return crc32c_table_loop((u32)c, p, length);
}
#elif defined(__aarch64__)
static int crc32c_hw_present(void)
{
	u64 isar0;

	__asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
	return ((isar0 >> 16) & 0xf) >= 1;
}

static u32 crc32c_hw(u32 crc, const u8 *p, unsigned int length)
{
	u64 c = crc;

	while (length >= 8) {
		u64 v = (u64)p[0] | (u64)p[1] << 8 | (u64)p[2] << 16 | (u64)p[3] << 24 |
		        (u64)p[4] << 32 | (u64)p[5] << 40 | (u64)p[6] << 48 |
		        (u64)p[7] << 56;
		u64 out;

		/* crc32cx w9, w9, x10, spelled as its encoding: the kernel is built
		 * for the base architecture, where the assembler has no CRC32. */
		__asm__ volatile("mov x9, %1\n\tmov x10, %2\n\t.inst 0x9aca5d29\n\tmov %0, x9"
		                 : "=r"(out) : "r"(c), "r"(v) : "x9", "x10");
		c = out;
		p += 8;
		length -= 8;
	}
	return crc32c_table_loop((u32)c, p, length);
}
#endif

/* 0 unknown, 1 use the instruction, -1 use the table. */
static int crc32c_mode;

u32 crc32c(u32 crc, const void *address, unsigned int length)
{
	const u8 *p = (const u8 *)address;

	ensure_tables();
#if defined(__x86_64__) || defined(__aarch64__)
	if (crc32c_mode == 0) {
		int mode = -1;

		if (crc32c_hw_present()) {
			u8 probe[61];

			for (unsigned int i = 0; i < sizeof(probe); i++)
				probe[i] = (u8)(i * 131u + 7u);
			if (crc32c_hw(0xFFFFFFFFu, probe, sizeof(probe)) ==
			    crc32c_table_loop(0xFFFFFFFFu, probe, sizeof(probe)))
				mode = 1;
		}
		crc32c_mode = mode;
	}
	if (crc32c_mode > 0)
		return crc32c_hw(crc, p, length);
#endif
	return crc32c_table_loop(crc, p, length);
}

/* The boot check: the published check value of CRC-32C, then the instruction
 * against the table over every length and alignment a block read can hand it.
 * `*hw` says which one crc32c() is using. */
int lkpi_crc32c_selftest(u64 *hw)
{
	static const u8 check[] = "123456789";
	u8 buf[320];
	int ok = 1;

	if ((crc32c(0xFFFFFFFFu, check, 9) ^ 0xFFFFFFFFu) != 0xE3069283u)
		ok = 0;
	*hw = crc32c_mode > 0;
#if defined(__x86_64__) || defined(__aarch64__)
	if (crc32c_mode > 0) {
		for (unsigned int i = 0; i < sizeof(buf); i++)
			buf[i] = (u8)(i * 197u + 31u);
		for (unsigned int off = 0; off < 8; off++)
			for (unsigned int len = 0; len + off < 300; len++)
				if (crc32c_hw(0x12345678u, buf + off, len) !=
				    crc32c_table_loop(0x12345678u, buf + off, len))
					ok = 0;
	}
#endif
	return ok;
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
