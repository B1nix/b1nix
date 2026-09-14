/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_ASM_UNALIGNED_H
#define LKPI_ASM_UNALIGNED_H
#include <linux/types.h>
/* <linux/byteorder.h> spells these as macros over a cast-and-deref, which is
 * exactly the assumption this header exists to avoid. Drop those and use the
 * memcpy form below; the two agree on every value, they differ only in what
 * the compiler is allowed to assume about the pointer. */
#undef get_unaligned_le16
#undef get_unaligned_le32
#undef get_unaligned_le64
#undef put_unaligned_le16
#undef put_unaligned_le32
#undef put_unaligned_le64
/* Loads and stores that do not assume alignment. x86 tolerates unaligned
 * access, but the compiler is still free to assume a pointer's declared
 * alignment and vectorise accordingly — so these go through memcpy, which is
 * the portable way to say "these bytes, whatever they are aligned to". */
static inline u16 get_unaligned_le16(const void *p)
{ u16 v; __builtin_memcpy(&v, p, sizeof(v)); return v; }
static inline u32 get_unaligned_le32(const void *p)
{ u32 v; __builtin_memcpy(&v, p, sizeof(v)); return v; }
static inline u64 get_unaligned_le64(const void *p)
{ u64 v; __builtin_memcpy(&v, p, sizeof(v)); return v; }
static inline void put_unaligned_le16(u16 v, void *p)
{ __builtin_memcpy(p, &v, sizeof(v)); }
static inline void put_unaligned_le32(u32 v, void *p)
{ __builtin_memcpy(p, &v, sizeof(v)); }
static inline void put_unaligned_le64(u64 v, void *p)
{ __builtin_memcpy(p, &v, sizeof(v)); }
/*
 * The type-generic pair.
 *
 * Unlike the sized forms above, these take their width from the pointer's own
 * type — btrfs reads a `__le64` field out of an on-disk directory item with
 * `get_unaligned(&entry->offset)`. The access goes through a packed struct,
 * which is what tells the compiler not to assume alignment it cannot have; a
 * plain dereference would be free to use an instruction that requires it.
 */
#ifndef get_unaligned
#define get_unaligned(ptr)                                                     \
	__extension__({                                                            \
		struct b1nix_unaligned_load {                                          \
			__typeof__(*(ptr)) v;                                              \
		} __attribute__((packed, may_alias));                                  \
		((const struct b1nix_unaligned_load *)(ptr))->v;                       \
	})
#endif

#ifndef put_unaligned
#define put_unaligned(val, ptr)                                                \
	do {                                                                       \
		struct b1nix_unaligned_store {                                         \
			__typeof__(*(ptr)) v;                                              \
		} __attribute__((packed, may_alias));                                  \
		((struct b1nix_unaligned_store *)(ptr))->v = (val);                    \
	} while (0)
#endif

#endif
