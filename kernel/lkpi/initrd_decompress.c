// SPDX-License-Identifier: GPL-2.0-only
/*
 * Decompression for an initramfs handed over by the boot loader.
 *
 * A distribution's initramfs is a cpio archive, usually compressed and often
 * several archives end to end (an uncompressed microcode one, then the rest).
 * The kernel unpacks it with the same libraries the imported filesystems
 * already carry: zlib's inflate and zstd. This file is the only b1nix code
 * that calls them outside a filesystem, and it is here rather than in
 * kernel/fs because those libraries' headers only make sense on the lkpi side.
 *
 * The output is streamed: each decompressed chunk goes to the caller's sink
 * as soon as it exists, so a 40 MiB archive never needs 40 MiB of buffer.
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/zlib.h>
#include <linux/zstd.h>
#include <lkpi/env.h>

#define INITRD_CHUNK (64u * 1024u)

typedef int (*initrd_sink_t)(const void *data, size_t len, void *ctx);

/* gzip (RFC 1952): a header, a raw deflate stream, CRC32 and the size. */
static long initrd_gunzip(const u8 *in, size_t in_len, initrd_sink_t sink,
                          void *ctx)
{
	enum { FHCRC = 0x02, FEXTRA = 0x04, FNAME = 0x08, FCOMMENT = 0x10 };
	struct z_stream_s strm;
	size_t pos = 10;
	u8 flags;
	u8 *out;
	long ret;
	int zr;

	if (in_len < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
		return -EINVAL;
	flags = in[3];
	if (flags & FEXTRA) {
		if (pos + 2 > in_len)
			return -EINVAL;
		pos += 2 + (in[pos] | (in[pos + 1] << 8));
	}
	if (flags & FNAME)
		while (pos < in_len && in[pos++])
			;
	if (flags & FCOMMENT)
		while (pos < in_len && in[pos++])
			;
	if (flags & FHCRC)
		pos += 2;
	if (pos >= in_len)
		return -EINVAL;

	memset(&strm, 0, sizeof(strm));
	strm.workspace = kmalloc(zlib_inflate_workspacesize(), GFP_KERNEL);
	out = kmalloc(INITRD_CHUNK, GFP_KERNEL);
	if (!strm.workspace || !out) {
		ret = -ENOMEM;
		goto out;
	}
	if (zlib_inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
		ret = -EINVAL;
		goto out;
	}
	strm.next_in = in + pos;
	strm.avail_in = in_len - pos;
	do {
		strm.next_out = out;
		strm.avail_out = INITRD_CHUNK;
		zr = zlib_inflate(&strm, Z_SYNC_FLUSH);
		if (zr != Z_OK && zr != Z_STREAM_END) {
			ret = -EINVAL;
			goto end;
		}
		if (INITRD_CHUNK - strm.avail_out) {
			int sr = sink(out, INITRD_CHUNK - strm.avail_out, ctx);

			if (sr < 0) {
				ret = sr;
				goto end;
			}
		}
	} while (zr != Z_STREAM_END);
	/* What was consumed: the header, the deflate stream, the 8-byte trailer. */
	ret = (long)(pos + strm.total_in + 8);
	if ((size_t)ret > in_len)
		ret = (long)in_len;
end:
	zlib_inflateEnd(&strm);
out:
	kfree(out);
	kfree(strm.workspace);
	return ret;
}

/* One zstd frame; `in` starts at its magic. */
static long initrd_unzstd(const u8 *in, size_t in_len, initrd_sink_t sink,
                          void *ctx)
{
	zstd_frame_header fh;
	zstd_in_buffer ib = { .src = in, .size = in_len, .pos = 0 };
	zstd_dstream *ds;
	size_t window, wsize, zr;
	void *ws = NULL;
	u8 *out;
	long ret;

	if (zstd_get_frame_header(&fh, in, in_len) != 0)
		return -EINVAL;
	window = (size_t)fh.windowSize;
	if (!window || window > (1u << 27)) /* the format's own limit, 128 MiB */
		return -EINVAL;
	wsize = zstd_dstream_workspace_bound(window);
	ws = kvmalloc(wsize, GFP_KERNEL);
	out = kmalloc(INITRD_CHUNK, GFP_KERNEL);
	if (!ws || !out) {
		ret = -ENOMEM;
		goto out;
	}
	ds = zstd_init_dstream(window, ws, wsize);
	if (!ds) {
		ret = -EINVAL;
		goto out;
	}
	do {
		zstd_out_buffer ob = { .dst = out, .size = INITRD_CHUNK, .pos = 0 };

		zr = zstd_decompress_stream(ds, &ob, &ib);
		if (zstd_is_error(zr)) {
			ret = -EINVAL;
			goto out;
		}
		if (ob.pos) {
			int sr = sink(out, ob.pos, ctx);

			if (sr < 0) {
				ret = sr;
				goto out;
			}
		}
		/* No progress and not finished: the input ends inside the frame. */
		if (zr && ib.pos == ib.size && ob.pos < ob.size) {
			ret = -EINVAL;
			goto out;
		}
	} while (zr != 0);
	ret = (long)ib.pos;
out:
	kfree(out);
	kvfree(ws);
	return ret;
}

/*
 * Decompress the one stream at the start of `in`, whatever its format.
 * Returns how many input bytes the stream took, or a negative errno: -ENOEXEC
 * for a format this kernel does not decompress (xz, lzma, lz4, bzip2), so the
 * caller can say which one it was handed.
 */
long lkpi_initrd_decompress(const void *in, size_t in_len, initrd_sink_t sink,
                            void *ctx)
{
	const u8 *p = in;

	if (in_len >= 2 && p[0] == 0x1f && p[1] == 0x8b)
		return initrd_gunzip(p, in_len, sink, ctx);
	if (in_len >= 4 && p[0] == 0x28 && p[1] == 0xb5 && p[2] == 0x2f &&
	    p[3] == 0xfd) {
		long total = 0;

		/* A zstd stream may be several frames back to back. */
		while ((size_t)total + 4 <= in_len && p[total] == 0x28 &&
		       p[total + 1] == 0xb5 && p[total + 2] == 0x2f &&
		       p[total + 3] == 0xfd) {
			long n = initrd_unzstd(p + total, in_len - (size_t)total,
			                       sink, ctx);

			if (n < 0)
				return n;
			total += n;
		}
		return total;
	}
	return -ENOEXEC;
}
