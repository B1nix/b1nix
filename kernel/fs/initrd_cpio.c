/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The boot loader's initramfs, unpacked the way Linux unpacks it.
 *
 * Linux's boot protocol: when the initrd is a cpio archive, the kernel
 * extracts it into a RAM filesystem at "/" and runs /init from it, and it is
 * that program -- not the kernel -- that finds the real root, mounts it and
 * moves onto it. `init=` then names the program /init hands over to, and
 * `root=` is read by /init rather than by the kernel.
 *
 * A Debian system depends on that: initramfs-tools puts everything that must
 * happen before the root is mounted into /init's hooks. b1nix's boot counting
 * is one of them (its hook spends a try of the booting kernel before the root
 * is touched), and a kernel that mounted the root itself and started
 * init= directly skipped every hook -- the fallback to the last good kernel
 * never happened.
 *
 * The format is "newc" (magic 070701, or 070702 with checksums), possibly
 * compressed, possibly several archives end to end: initramfs-tools prepends
 * an uncompressed archive for early microcode. An image in ram0 that is none
 * of these (an ext4 image, as the tree's own lanes use) is left alone and
 * mounted as a block device, as before.
 */

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/vfs.h>
#include <b1nix/bootinfo.h>
#include <b1nix/initrd.h>
#include <stdio.h>
#include <string.h>

typedef int (*initrd_sink_t)(const void *data, usize len, void *ctx);
long lkpi_initrd_decompress(const void *in, usize in_len, initrd_sink_t sink,
                            void *ctx);

#define CPIO_HDR_LEN 110
#define CPIO_MAX_LINKS 256

enum cpio_phase {
	CPIO_SKIP_ZEROS, /* between archives: padding before the next magic */
	CPIO_HEADER,
	CPIO_NAME,
	CPIO_DATA,
	CPIO_PAD,
};

/* A regular file with more than one name: the first name seen, so the later
 * ones become links to it (newc stores the data once, with the last name). */
struct cpio_link {
	u32 ino, major, minor;
	char name[VFS_MAX_PATH];
};

struct cpio_state {
	enum cpio_phase phase;
	u64 off;            /* bytes of the current archive so far: padding is relative to it */
	u8 hdr[CPIO_HDR_LEN];
	usize have;         /* bytes of the header or name gathered */
	usize pad;          /* bytes of padding left to skip */
	u32 ino, mode, uid, gid, nlink, mtime, filesize, devmajor, devminor;
	u32 rdevmajor, rdevminor, namesize;
	char name[VFS_MAX_PATH];
	char path[VFS_MAX_PATH];
	usize remaining;    /* data bytes left of this entry */
	int fd;             /* the regular file being filled, or -1 */
	char link[VFS_MAX_PATH]; /* a symlink's target, gathered from its data */
	usize linklen;
	int skip;           /* this entry is not being created; its data is dropped */
	struct cpio_link *links;
	usize nlinks;
	u32 files, errors;
	int trailers;
	int stop_at_trailer; /* an uncompressed archive: the caller wants the length */
	int stopped;
};

static u32 hex8(const u8 *p)
{
	u32 v = 0;

	for (int i = 0; i < 8; i++) {
		u8 c = p[i];

		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (u32)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (u32)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (u32)(c - 'A' + 10);
	}
	return v;
}

static usize pad4(u64 off) { return (usize)((4 - (off & 3)) & 3); }

static void cpio_error(struct cpio_state *st, const char *what, int rc)
{
	char buf[VFS_MAX_PATH + 64];

	st->errors++;
	/* The first few, not a flood: one bad entry usually means many. */
	if (st->errors > 8)
		return;
	snprintf(buf, sizeof(buf), "initrd: %s %s: %d\n", what, st->path, rc);
	console_write(buf);
}

/* The Linux encoding of a device number, which vfs_mknod takes (it is what
 * mknod(2) hands it). */
static u64 linux_dev(u32 major, u32 minor)
{
	return ((u64)(minor & 0xff)) | ((u64)(major & 0xfff) << 8) |
	       ((u64)(minor & ~0xffu) << 12) | ((u64)(major & ~0xfffu) << 32);
}

static void cpio_set_attrs(struct cpio_state *st, int is_link)
{
	int rc;

	rc = is_link ? vfs_lchown(st->path, st->uid, st->gid)
	             : vfs_chown(st->path, st->uid, st->gid);
	if (rc < 0)
		cpio_error(st, "chown", rc);
	if (is_link)
		return;
	/* After chown: a chown clears set-id bits the mode may carry. */
	rc = vfs_chmod(st->path, (u16)(st->mode & 07777));
	if (rc < 0)
		cpio_error(st, "chmod", rc);
}

/* A name that already exists is replaced, unless both are directories:
 * a later archive overrides an earlier one entry by entry. */
static void cpio_clear_path(struct cpio_state *st, u32 type)
{
	struct b1nix_stat sb;

	if (vfs_lstat(st->path, &sb) < 0)
		return;
	if ((sb.st_mode & B1NIX_S_IFMT) == B1NIX_S_IFDIR) {
		if (type != B1NIX_S_IFDIR)
			(void)vfs_rmdir(st->path);
		return;
	}
	(void)vfs_unlink(st->path);
}

/* Start an entry once its header and name are in. */
static void cpio_begin_entry(struct cpio_state *st)
{
	u32 type = st->mode & B1NIX_S_IFMT;
	int rc;

	st->fd = -1;
	st->linklen = 0;
	st->skip = 1;
	st->remaining = st->filesize;

	if (strcmp(st->name, "TRAILER!!!") == 0) {
		st->trailers++;
		return;
	}
	/* Names are relative to the archive's root, which is "/" here. */
	{
		const char *n = st->name;

		while (n[0] == '.' && n[1] == '/')
			n += 2;
		while (*n == '/')
			n++;
		if (!*n || strcmp(n, ".") == 0)
			return; /* the root itself */
		snprintf(st->path, sizeof(st->path), "/%s", n);
	}

	switch (type) {
	case B1NIX_S_IFDIR:
		cpio_clear_path(st, type);
		rc = vfs_mkdir(st->path, st->mode & 07777);
		if (rc < 0 && rc != -EEXIST) {
			cpio_error(st, "mkdir", rc);
			return;
		}
		cpio_set_attrs(st, 0);
		(void)vfs_utime(st->path, st->mtime, st->mtime);
		st->files++;
		return;
	case B1NIX_S_IFREG:
		if (st->nlink > 1) {
			for (usize i = 0; i < st->nlinks; i++) {
				struct cpio_link *l = &st->links[i];

				if (l->ino == st->ino && l->major == st->devmajor &&
				    l->minor == st->devminor) {
					cpio_clear_path(st, type);
					rc = vfs_link(l->name, st->path);
					if (rc < 0) {
						cpio_error(st, "link", rc);
						return;
					}
					st->files++;
					if (!st->filesize)
						return;
					/* The data arrives with this name: it fills the
					 * inode every name shares. */
					st->fd = vfs_open_flags(st->path, B1NIX_O_WRONLY |
					                        B1NIX_O_TRUNC | B1NIX_O_NOFOLLOW);
					if (st->fd < 0)
						cpio_error(st, "open", st->fd);
					else
						st->skip = 0;
					return;
				}
			}
			if (st->nlinks < CPIO_MAX_LINKS) {
				struct cpio_link *l = &st->links[st->nlinks++];

				l->ino = st->ino;
				l->major = st->devmajor;
				l->minor = st->devminor;
				snprintf(l->name, sizeof(l->name), "%s", st->path);
			}
		}
		cpio_clear_path(st, type);
		st->fd = vfs_open_flags_mode(st->path, B1NIX_O_WRONLY | B1NIX_O_CREAT |
		                             B1NIX_O_TRUNC | B1NIX_O_NOFOLLOW,
		                             (u16)(st->mode & 07777));
		if (st->fd < 0) {
			cpio_error(st, "create", st->fd);
			return;
		}
		st->skip = 0;
		st->files++;
		return;
	case B1NIX_S_IFLNK:
		if (st->filesize >= sizeof(st->link)) {
			cpio_error(st, "symlink target too long", -ENAMETOOLONG);
			return;
		}
		st->skip = 0; /* the data is the target */
		return;
	case B1NIX_S_IFCHR:
	case B1NIX_S_IFBLK:
	case B1NIX_S_IFIFO:
	case B1NIX_S_IFSOCK:
		cpio_clear_path(st, type);
		rc = vfs_mknod(st->path, st->mode,
		               linux_dev(st->rdevmajor, st->rdevminor));
		if (rc < 0) {
			cpio_error(st, "mknod", rc);
			return;
		}
		cpio_set_attrs(st, 0);
		st->files++;
		return;
	default:
		cpio_error(st, "unknown type", -EINVAL);
		return;
	}
}

static void cpio_end_entry(struct cpio_state *st)
{
	u32 type = st->mode & B1NIX_S_IFMT;

	if (st->fd >= 0) {
		vfs_close(st->fd);
		st->fd = -1;
		cpio_set_attrs(st, 0);
		(void)vfs_utime(st->path, st->mtime, st->mtime);
	} else if (type == B1NIX_S_IFLNK && !st->skip) {
		int rc;

		st->link[st->linklen] = '\0';
		cpio_clear_path(st, type);
		rc = vfs_symlink(st->link, st->path);
		if (rc < 0) {
			cpio_error(st, "symlink", rc);
		} else {
			cpio_set_attrs(st, 1);
			st->files++;
		}
	}
	st->skip = 1;
}

/* Take the bytes of one archive stream. Returns how many were consumed, which
 * is all of them unless `stop_at_trailer` ended the archive early. */
static long cpio_feed(struct cpio_state *st, const u8 *p, usize len)
{
	usize i = 0;

	while (i < len && !st->stopped) {
		switch (st->phase) {
		case CPIO_SKIP_ZEROS:
			if (p[i] == 0) {
				i++;
				continue;
			}
			st->off = 0;
			st->have = 0;
			st->phase = CPIO_HEADER;
			continue;
		case CPIO_HEADER: {
			usize n = CPIO_HDR_LEN - st->have;

			if (n > len - i)
				n = len - i;
			memcpy(st->hdr + st->have, p + i, n);
			st->have += n;
			st->off += n;
			i += n;
			if (st->have < CPIO_HDR_LEN)
				continue;
			if (memcmp(st->hdr, "07070", 5) != 0 ||
			    (st->hdr[5] != '1' && st->hdr[5] != '2')) {
				console_write("initrd: not a newc cpio header -- the archive is damaged\n");
				return -EINVAL;
			}
			st->ino = hex8(st->hdr + 6);
			st->mode = hex8(st->hdr + 14);
			st->uid = hex8(st->hdr + 22);
			st->gid = hex8(st->hdr + 30);
			st->nlink = hex8(st->hdr + 38);
			st->mtime = hex8(st->hdr + 46);
			st->filesize = hex8(st->hdr + 54);
			st->devmajor = hex8(st->hdr + 62);
			st->devminor = hex8(st->hdr + 70);
			st->rdevmajor = hex8(st->hdr + 78);
			st->rdevminor = hex8(st->hdr + 86);
			st->namesize = hex8(st->hdr + 94);
			if (!st->namesize || st->namesize > sizeof(st->name)) {
				console_write("initrd: a cpio name is longer than a path -- the archive is damaged\n");
				return -EINVAL;
			}
			st->have = 0;
			st->phase = CPIO_NAME;
			continue;
		}
		case CPIO_NAME: {
			usize n = st->namesize - st->have;

			if (n > len - i)
				n = len - i;
			memcpy(st->name + st->have, p + i, n);
			st->have += n;
			st->off += n;
			i += n;
			if (st->have < st->namesize)
				continue;
			st->name[st->namesize - 1] = '\0';
			cpio_begin_entry(st);
			st->pad = pad4(st->off);
			st->phase = CPIO_PAD;
			/* After the name's padding comes the data, then its own. */
			st->have = 1; /* marks "the data is next" for CPIO_PAD */
			continue;
		}
		case CPIO_PAD:
			while (st->pad && i < len) {
				st->pad--;
				st->off++;
				i++;
			}
			if (st->pad)
				continue;
			if (st->have) { /* the name's padding is done */
				st->have = 0;
				st->phase = CPIO_DATA;
				continue;
			}
			/* The data's padding is done: the entry is complete. */
			if (st->trailers && strcmp(st->name, "TRAILER!!!") == 0) {
				st->phase = CPIO_SKIP_ZEROS;
				if (st->stop_at_trailer)
					st->stopped = 1;
				continue;
			}
			st->phase = CPIO_HEADER;
			continue;
		case CPIO_DATA: {
			usize n = st->remaining;

			if (n > len - i)
				n = len - i;
			if (n && !st->skip) {
				if (st->fd >= 0) {
					isize w = vfs_write(st->fd, (const char *)p + i, n);

					if (w != (isize)n) {
						cpio_error(st, "write", w < 0 ? (int)w : -EIO);
						vfs_close(st->fd);
						st->fd = -1;
						st->skip = 1;
					}
				} else if ((st->mode & B1NIX_S_IFMT) == B1NIX_S_IFLNK) {
					memcpy(st->link + st->linklen, p + i, n);
					st->linklen += n;
				}
			}
			st->remaining -= n;
			st->off += n;
			i += n;
			if (st->remaining)
				continue;
			cpio_end_entry(st);
			st->pad = pad4(st->off);
			st->have = 0;
			st->phase = CPIO_PAD;
			continue;
		}
		}
	}
	return (long)i;
}

static int cpio_sink(const void *data, usize len, void *ctx)
{
	long n = cpio_feed(ctx, data, len);

	return n < 0 ? (int)n : 0;
}

static int is_cpio_magic(const u8 *p, usize len)
{
	return len >= 6 && memcmp(p, "07070", 5) == 0 && (p[5] == '1' || p[5] == '2');
}

static int is_compressed_magic(const u8 *p, usize len)
{
	if (len >= 2 && p[0] == 0x1f && p[1] == 0x8b)
		return 1; /* gzip */
	if (len >= 4 && p[0] == 0x28 && p[1] == 0xb5 && p[2] == 0x2f && p[3] == 0xfd)
		return 1; /* zstd */
	if (len >= 6 && p[0] == 0xfd && memcmp(p + 1, "7zXZ", 4) == 0)
		return 1; /* xz: recognised, so it is refused by name below */
	if (len >= 4 && p[0] == 0x02 && p[1] == 0x21 && p[2] == 0x4c && p[3] == 0x18)
		return 1; /* lz4 legacy */
	return 0;
}

static const u8 *initrd_bytes(usize *len)
{
	const struct boot_info *bi = bootinfo_get();

	if (!bi || !bi->has_ramdisk || !bi->ramdisk_size)
		return 0;
	*len = (usize)bi->ramdisk_size;
	return (const u8 *)(usize)(bi->ramdisk_addr + DIRECT_MAP_BASE);
}

int initrd_is_initramfs(void)
{
	usize len;
	const u8 *p = initrd_bytes(&len);

	if (!p || bootinfo_has_flag("noinitrd"))
		return 0;
	return is_cpio_magic(p, len) || is_compressed_magic(p, len);
}

int initrd_unpack_to_rootfs(void)
{
	usize len, pos = 0;
	const u8 *p = initrd_bytes(&len);
	struct cpio_state *st;
	char buf[128];
	int rc;

	if (!p)
		return -ENOENT;
	st = kzalloc(sizeof(*st));
	if (!st)
		return -ENOMEM;
	st->links = kzalloc(sizeof(*st->links) * CPIO_MAX_LINKS);
	if (!st->links) {
		kfree(st);
		return -ENOMEM;
	}
	st->fd = -1;
	st->skip = 1;

	/* A RAM filesystem over "/", as Linux's rootfs: the archive goes into it,
	 * and /init's run-init later moves the real root over it. */
	rc = vfs_mount("rootfs", "/", "tmpfs", 0);
	if (rc < 0) {
		snprintf(buf, sizeof(buf), "initrd: cannot mount a tmpfs at /: %d\n", rc);
		console_write(buf);
		goto out;
	}

	while (pos < len) {
		long n;

		if (p[pos] == 0) { /* padding between archives */
			pos++;
			continue;
		}
		if (is_cpio_magic(p + pos, len - pos)) {
			st->phase = CPIO_HEADER;
			st->off = 0;
			st->have = 0;
			st->stop_at_trailer = 1;
			st->stopped = 0;
			n = cpio_feed(st, p + pos, len - pos);
		} else if (is_compressed_magic(p + pos, len - pos)) {
			st->phase = CPIO_SKIP_ZEROS;
			st->stop_at_trailer = 0;
			st->stopped = 0;
			n = lkpi_initrd_decompress(p + pos, len - pos, cpio_sink, st);
			if (n == -ENOEXEC)
				console_write("initrd: compressed with a method this kernel does not decompress (it takes gzip and zstd)\n");
			if (n >= 0 && st->phase != CPIO_SKIP_ZEROS &&
			    st->phase != CPIO_HEADER)
				n = -EINVAL; /* the stream ended inside an entry */
		} else {
			snprintf(buf, sizeof(buf),
			         "initrd: unknown data at offset %lu, ignored\n",
			         (unsigned long)pos);
			console_write(buf);
			break;
		}
		if (n <= 0) {
			rc = n < 0 ? (int)n : -EINVAL;
			snprintf(buf, sizeof(buf),
			         "initrd: unpacking failed at offset %lu: %d\n",
			         (unsigned long)pos, rc);
			console_write(buf);
			break;
		}
		pos += (usize)n;
	}
	if (st->fd >= 0)
		vfs_close(st->fd);

	snprintf(buf, sizeof(buf), "initrd: %u entries unpacked to / (%u errors)\n",
	         st->files, st->errors);
	console_write(buf);
	/* Linux boots whatever did unpack; /init decides whether it is enough. */
	if (rc == 0 && !st->files)
		rc = -ENOENT;
out:
	kfree(st->links);
	kfree(st);
	return rc;
}
