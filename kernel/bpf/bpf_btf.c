/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * BTF — the BPF Type Format (M133).
 *
 * Two things live here. The first is the kernel's own type information: pahole
 * encodes the kernel's DWARF as BTF at build time and the blob is linked into
 * .BTF (tools/toolchain/kernel/gen_btf.sh). /sys/kernel/btf/vmlinux serves it,
 * and that file is what a CO-RE loader relocates a program against: libbpf
 * reads it, finds each structure a program touches, and rewrites the field
 * offsets to the ones this kernel has.
 *
 * The second is BTF a loader hands in with BPF_BTF_LOAD -- a program's own
 * types, which it then names in BPF_PROG_LOAD (prog_btf_fd, func_info,
 * line_info). The kernel checks it before keeping it: the header, every type
 * record against its kind's size, every name against the string section, and
 * every type reference against the number of types. What it does not do is
 * Linux's full semantic pass (member offsets against sizes, loops through
 * typedefs); a blob that passes here is well-formed, not proven meaningful.
 */

#include <b1nix/bpf.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/spinlock.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <string.h>

#define BTF_MAGIC 0xeB9F
#define BTF_VERSION 1

enum {
	BTF_KIND_INT = 1, BTF_KIND_PTR, BTF_KIND_ARRAY, BTF_KIND_STRUCT,
	BTF_KIND_UNION, BTF_KIND_ENUM, BTF_KIND_FWD, BTF_KIND_TYPEDEF,
	BTF_KIND_VOLATILE, BTF_KIND_CONST, BTF_KIND_RESTRICT, BTF_KIND_FUNC,
	BTF_KIND_FUNC_PROTO, BTF_KIND_VAR, BTF_KIND_DATASEC, BTF_KIND_FLOAT,
	BTF_KIND_DECL_TAG, BTF_KIND_TYPE_TAG, BTF_KIND_ENUM64,
	BTF_KIND_MAX = BTF_KIND_ENUM64,
};

struct btf_header {
	u16 magic;
	u8 version;
	u8 flags;
	u32 hdr_len;
	u32 type_off;
	u32 type_len;
	u32 str_off;
	u32 str_len;
};

struct btf_type {
	u32 name_off;
	u32 info; /* vlen 0..15, kind 24..28, kind_flag 31 */
	u32 size_or_type;
};

#define BTF_INFO_KIND(i) (((i) >> 24) & 0x1f)
#define BTF_INFO_VLEN(i) ((i) & 0xffff)

/* Bytes that follow a type record of each kind, for `vlen` members. */
static long btf_extra(u32 kind, u32 vlen) {
	switch (kind) {
	case BTF_KIND_INT: return 4;
	case BTF_KIND_ARRAY: return 12;
	case BTF_KIND_STRUCT:
	case BTF_KIND_UNION: return 12L * vlen;
	case BTF_KIND_ENUM: return 8L * vlen;
	case BTF_KIND_FUNC_PROTO: return 8L * vlen;
	case BTF_KIND_VAR: return 4;
	case BTF_KIND_DATASEC: return 12L * vlen;
	case BTF_KIND_DECL_TAG: return 4;
	case BTF_KIND_ENUM64: return 12L * vlen;
	case BTF_KIND_PTR: case BTF_KIND_FWD: case BTF_KIND_TYPEDEF:
	case BTF_KIND_VOLATILE: case BTF_KIND_CONST: case BTF_KIND_RESTRICT:
	case BTF_KIND_FUNC: case BTF_KIND_FLOAT: case BTF_KIND_TYPE_TAG:
		return 0;
	default: return -1;
	}
}

/* Kinds whose third word is a type id rather than a size. */
static int btf_refers(u32 kind) {
	switch (kind) {
	case BTF_KIND_PTR: case BTF_KIND_TYPEDEF: case BTF_KIND_VOLATILE:
	case BTF_KIND_CONST: case BTF_KIND_RESTRICT: case BTF_KIND_FUNC:
	case BTF_KIND_FUNC_PROTO: case BTF_KIND_VAR: case BTF_KIND_DECL_TAG:
	case BTF_KIND_TYPE_TAG:
		return 1;
	default:
		return 0;
	}
}

static void btf_log(char *log, u32 log_size, const char *msg, u32 at) {
	if (log && log_size)
		snprintf(log, log_size, "btf: %s (at %u)\n", msg, at);
}

int btf_parse(const u8 *data, u32 size, u32 *ntypes_out, char *log,
              u32 log_size) {
	const struct btf_header *h = (const struct btf_header *)data;

	if (size < sizeof(*h)) {
		btf_log(log, log_size, "shorter than a header", 0);
		return -EINVAL;
	}
	if (h->magic != BTF_MAGIC || h->version != BTF_VERSION || h->flags) {
		btf_log(log, log_size, "bad magic, version or flags", 0);
		return -EINVAL;
	}
	if (h->hdr_len < sizeof(*h) || h->hdr_len > size) {
		btf_log(log, log_size, "header length out of range", h->hdr_len);
		return -EINVAL;
	}
	/* Everything after the header is the two sections, each in bounds. */
	u64 body = size - h->hdr_len;

	if ((u64)h->type_off + h->type_len > body ||
	    (u64)h->str_off + h->str_len > body || (h->type_off & 3)) {
		btf_log(log, log_size, "section out of range", h->type_off);
		return -EINVAL;
	}
	const u8 *types = data + h->hdr_len + h->type_off;
	const char *strs = (const char *)data + h->hdr_len + h->str_off;

	/* The string section starts with the empty name and every name in it
	 * ends: a lookup can then never run off the end. */
	if (!h->str_len || strs[0] || strs[h->str_len - 1]) {
		btf_log(log, log_size, "string section not NUL-delimited", h->str_off);
		return -EINVAL;
	}

	/* First pass: the records, one by one, and how many there are. */
	u32 ntypes = 0;
	u32 off = 0;

	while (off < h->type_len) {
		if (h->type_len - off < sizeof(struct btf_type)) {
			btf_log(log, log_size, "truncated type record", off);
			return -EINVAL;
		}
		const struct btf_type *t = (const struct btf_type *)(types + off);
		u32 kind = BTF_INFO_KIND(t->info);
		long extra = btf_extra(kind, BTF_INFO_VLEN(t->info));

		if (!kind || kind > BTF_KIND_MAX || extra < 0) {
			btf_log(log, log_size, "unknown kind", off);
			return -EINVAL;
		}
		if (t->name_off >= h->str_len) {
			btf_log(log, log_size, "name past the string section", off);
			return -EINVAL;
		}
		if ((u64)off + sizeof(*t) + (u64)extra > h->type_len) {
			btf_log(log, log_size, "members past the type section", off);
			return -EINVAL;
		}
		off += sizeof(*t) + (u32)extra;
		ntypes++;
	}

	/* Second pass: every reference names a type that exists (ids start at 1;
	 * 0 is void). Members and parameters carry their own names and types. */
	off = 0;
	while (off < h->type_len) {
		const struct btf_type *t = (const struct btf_type *)(types + off);
		u32 kind = BTF_INFO_KIND(t->info);
		u32 vlen = BTF_INFO_VLEN(t->info);
		const u32 *m = (const u32 *)(t + 1);

		if (btf_refers(kind) && t->size_or_type > ntypes) {
			btf_log(log, log_size, "reference to a type that does not exist", off);
			return -EINVAL;
		}
		switch (kind) {
		case BTF_KIND_ARRAY:
			if (m[0] > ntypes || m[1] > ntypes) {
				btf_log(log, log_size, "array of a missing type", off);
				return -EINVAL;
			}
			break;
		case BTF_KIND_STRUCT:
		case BTF_KIND_UNION:
			for (u32 i = 0; i < vlen; i++)
				if (m[i * 3] >= h->str_len || m[i * 3 + 1] > ntypes) {
					btf_log(log, log_size, "bad member", off);
					return -EINVAL;
				}
			break;
		case BTF_KIND_FUNC_PROTO:
			for (u32 i = 0; i < vlen; i++)
				if (m[i * 2] >= h->str_len || m[i * 2 + 1] > ntypes) {
					btf_log(log, log_size, "bad parameter", off);
					return -EINVAL;
				}
			break;
		case BTF_KIND_ENUM:
			for (u32 i = 0; i < vlen; i++)
				if (m[i * 2] >= h->str_len) {
					btf_log(log, log_size, "bad enumerator", off);
					return -EINVAL;
				}
			break;
		case BTF_KIND_ENUM64:
			for (u32 i = 0; i < vlen; i++)
				if (m[i * 3] >= h->str_len) {
					btf_log(log, log_size, "bad enumerator", off);
					return -EINVAL;
				}
			break;
		case BTF_KIND_DATASEC:
			for (u32 i = 0; i < vlen; i++)
				if (!m[i * 3] || m[i * 3] > ntypes) {
					btf_log(log, log_size, "section variable missing", off);
					return -EINVAL;
				}
			break;
		default:
			break;
		}
		off += sizeof(*t) + (u32)btf_extra(kind, vlen);
	}
	if (ntypes_out)
		*ntypes_out = ntypes;
	return 0;
}

/* The kind of type `id` (1-based) in a blob btf_parse accepted, or 0. */
u32 btf_type_kind(const u8 *data, u32 id) {
	const struct btf_header *h = (const struct btf_header *)data;
	const u8 *types = data + h->hdr_len + h->type_off;
	u32 off = 0, n = 0;

	while (off < h->type_len) {
		const struct btf_type *t = (const struct btf_type *)(types + off);

		if (++n == id)
			return BTF_INFO_KIND(t->info);
		off += sizeof(*t) +
		       (u32)btf_extra(BTF_INFO_KIND(t->info), BTF_INFO_VLEN(t->info));
	}
	return 0;
}

u32 btf_str_len(const u8 *data) {
	return ((const struct btf_header *)data)->str_len;
}

/* ── the objects ─────────────────────────────────────────────────────────── */

#define BPF_MAX_BTF 64

static struct bpf_btf g_btf[BPF_MAX_BTF];
static spinlock_t g_btf_lock;
static u32 g_btf_next_id = 1;

extern const u8 __start_BTF[], __stop_BTF[];

struct bpf_btf *btf_vmlinux(void) {
	usize len = (usize)(__stop_BTF - __start_BTF);
	u64 flags;

	if (!len)
		return 0;
	spin_lock_irqsave(&g_btf_lock, &flags);
	for (int i = 0; i < BPF_MAX_BTF; i++)
		if (g_btf[i].used && g_btf[i].kernel) {
			spin_unlock_irqrestore(&g_btf_lock, flags);
			return &g_btf[i];
		}
	/* The first to ask makes it: the kernel's is id 1 when nothing loaded a
	 * BTF before, as on Linux where vmlinux's is always there first. It is
	 * never freed. */
	for (int i = 0; i < BPF_MAX_BTF; i++)
		if (!g_btf[i].used) {
			struct bpf_btf *b = &g_btf[i];

			memset(b, 0, sizeof(*b));
			b->used = 1;
			b->kernel = 1;
			b->refs = 1;
			b->id = g_btf_next_id++;
			b->data = __start_BTF;
			b->size = (u32)len;
			spin_unlock_irqrestore(&g_btf_lock, flags);
			return b;
		}
	spin_unlock_irqrestore(&g_btf_lock, flags);
	return 0;
}

struct bpf_btf *btf_new(u8 *data, u32 size) {
	u64 flags;

	btf_vmlinux(); /* the kernel's takes the first id */
	spin_lock_irqsave(&g_btf_lock, &flags);
	for (int i = 0; i < BPF_MAX_BTF; i++)
		if (!g_btf[i].used) {
			struct bpf_btf *b = &g_btf[i];

			memset(b, 0, sizeof(*b));
			b->used = 1;
			b->refs = 1;
			b->id = g_btf_next_id++;
			b->data = data;
			b->owned = data;
			b->size = size;
			spin_unlock_irqrestore(&g_btf_lock, flags);
			return b;
		}
	spin_unlock_irqrestore(&g_btf_lock, flags);
	return 0;
}

void btf_get(struct bpf_btf *b) {
	if (b)
		__atomic_add_fetch(&b->refs, 1, __ATOMIC_ACQ_REL);
}

void btf_put(struct bpf_btf *b) {
	if (!b || b->kernel)
		return;
	if (__atomic_sub_fetch(&b->refs, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	kfree(b->owned);
	b->owned = 0;
	b->data = 0;
	__atomic_store_n(&b->used, 0, __ATOMIC_RELEASE);
}

/* By id, referenced; 0 if there is none. */
struct bpf_btf *btf_by_id(u32 id) {
	u64 flags;

	if (id == 1 || !id)
		btf_vmlinux();
	spin_lock_irqsave(&g_btf_lock, &flags);
	for (int i = 0; i < BPF_MAX_BTF; i++)
		if (g_btf[i].used && g_btf[i].id == id) {
			btf_get(&g_btf[i]);
			spin_unlock_irqrestore(&g_btf_lock, flags);
			return &g_btf[i];
		}
	spin_unlock_irqrestore(&g_btf_lock, flags);
	return 0;
}

/* The smallest id above `after`, or 0. */
u32 btf_next_id(u32 after) {
	u32 best = 0;
	u64 flags;

	btf_vmlinux();
	spin_lock_irqsave(&g_btf_lock, &flags);
	for (int i = 0; i < BPF_MAX_BTF; i++)
		if (g_btf[i].used && g_btf[i].id > after &&
		    (!best || g_btf[i].id < best))
			best = g_btf[i].id;
	spin_unlock_irqrestore(&g_btf_lock, flags);
	return best;
}

/* /sys/kernel/btf/vmlinux: the blob as the kernel was built with it. */
isize btf_vmlinux_read(struct vfs_node *node, u64 offset, char *buf,
                       usize size, int flags) {
	usize len = (usize)(__stop_BTF - __start_BTF);

	(void)node;
	(void)flags;
	if (offset >= len)
		return 0;
	if (size > len - offset)
		size = len - (usize)offset;
	memcpy(buf, __start_BTF + offset, size);
	return (isize)size;
}

usize btf_vmlinux_size(void) {
	return (usize)(__stop_BTF - __start_BTF);
}
