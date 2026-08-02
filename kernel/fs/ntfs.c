/*
 * ntfs.c — read-only NTFS driver.
 *
 * Supports what is needed to mount a genuine NTFS volume and read its
 * directory tree and file contents:
 *   - boot sector (bytes/sector, sectors/cluster, $MFT location, record size)
 *   - FILE records with update-sequence (fixup) fixing
 *   - resident and non-resident attributes; data-run decoding
 *   - directory indexes: $INDEX_ROOT plus non-resident $INDEX_ALLOCATION
 *     (INDX blocks, also fixup'd)
 *   - file $DATA reads (resident inline, or via the cluster run list)
 *
 * The VFS tree is eagerly populated at mount time, mirroring the ext and exfat
 * drivers. Write support is intentionally absent (read-only).
 */
#include <b1nix/ntfs.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/blk.h>
#include <b1nix/errno.h>
#include <string.h>

/* ── little-endian field readers (NTFS structures are unaligned) ───────────── */
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 rd64(const u8 *p) {
  u64 v = 0;
  for (int i = 7; i >= 0; i--)
    v = (v << 8) | p[i];
  return v;
}

/* Attribute type codes. */
#define NTFS_AT_FILE_NAME        0x30
#define NTFS_AT_DATA             0x80
#define NTFS_AT_INDEX_ROOT       0x90
#define NTFS_AT_INDEX_ALLOCATION 0xA0
#define NTFS_AT_END              0xFFFFFFFF

#define NTFS_FILE_REC_IN_USE     0x01
#define NTFS_FILE_REC_IS_DIR     0x02

#define NTFS_IE_HAS_SUBNODE      0x01
#define NTFS_IE_LAST             0x02

#define NTFS_MAX_DEPTH           48
#define NTFS_MAX_DIR_ENTRIES     4096

struct ntfs_run {
  u64 vcn; /* first virtual cluster covered by this run */
  u64 lcn; /* first logical (on-disk) cluster; 0 = sparse hole */
  u64 len; /* length in clusters */
};

struct ntfs_fs {
  struct block_device *bdev;
  u32 bytes_per_sector;
  u32 sectors_per_cluster;
  u32 cluster_size;
  u32 mft_record_size;
  u64 mft_lcn;
  struct ntfs_run *mft_runs; /* $MFT $DATA run list */
  int mft_run_count;
  struct ntfs_fs *next;
};

struct ntfs_inode_info {
  struct ntfs_fs *fs;
  u64 size;          /* $DATA real size in bytes */
  int resident;      /* 1 = data is inline in resident_data */
  u8 *resident_data; /* owned (resident only) */
  struct ntfs_run *runs; /* owned (non-resident only) */
  int run_count;
};

static struct ntfs_fs *ntfs_instances = NULL;

/* Read `count` whole clusters starting at logical cluster `lcn` into buf. */
static int ntfs_read_clusters(struct ntfs_fs *fs, u64 lcn, u32 count,
                              void *buf) {
  u32 sec_per_blk = fs->bytes_per_sector / fs->bdev->block_size;
  if (sec_per_blk == 0)
    sec_per_blk = 1;
  u64 lba = lcn * (u64)fs->sectors_per_cluster * sec_per_blk;
  u32 blocks = count * fs->sectors_per_cluster * sec_per_blk;
  return blk_read_cached(fs->bdev, lba, blocks, buf);
}

/* Read `len` bytes at byte offset `off` from a cluster run list into `out`.
 * Sparse runs (lcn == 0) read as zeros. Returns bytes read, or -1 on error. */
static isize ntfs_runlist_read(struct ntfs_fs *fs, struct ntfs_run *runs,
                               int run_count, u64 off, usize len, u8 *out) {
  usize done = 0;
  u8 *cbuf = kmalloc(fs->cluster_size);
  if (!cbuf)
    return -1;
  while (done < len) {
    u64 vcn = (off + done) / fs->cluster_size;
    u32 in_cl = (u32)((off + done) % fs->cluster_size);
    /* locate the run covering this VCN */
    struct ntfs_run *r = 0;
    for (int i = 0; i < run_count; i++) {
      if (vcn >= runs[i].vcn && vcn < runs[i].vcn + runs[i].len) {
        r = &runs[i];
        break;
      }
    }
    u32 chunk = fs->cluster_size - in_cl;
    if (chunk > len - done)
      chunk = (u32)(len - done);
    if (!r || r->lcn == 0) {
      memset(out + done, 0, chunk); /* unmapped / sparse → zeros */
    } else {
      u64 lcn = r->lcn + (vcn - r->vcn);
      if (ntfs_read_clusters(fs, lcn, 1, cbuf) < 0) {
        kfree(cbuf);
        return -1;
      }
      memcpy(out + done, cbuf + in_cl, chunk);
    }
    done += chunk;
  }
  kfree(cbuf);
  return (isize)done;
}

/* Apply the NTFS update-sequence (fixup) to a FILE/INDX record in place. */
static void ntfs_apply_fixup(struct ntfs_fs *fs, u8 *rec, u32 rec_size) {
  u16 usa_off = rd16(rec + 0x04);
  u16 usa_count = rd16(rec + 0x06); /* USN + one entry per sector */
  if (usa_off == 0 || usa_count == 0)
    return;
  u16 usn = rd16(rec + usa_off);
  for (u16 i = 1; i < usa_count; i++) {
    u32 sec_end = i * fs->bytes_per_sector - 2;
    if (sec_end + 2 > rec_size)
      break;
    /* the last two bytes of each sector must currently hold the USN */
    if (rd16(rec + sec_end) != usn)
      break; /* not a valid fixup target; leave as-is */
    u8 *orig = rec + usa_off + i * 2;
    rec[sec_end] = orig[0];
    rec[sec_end + 1] = orig[1];
  }
}

/* Decode a data-run list (pointed at by `p`, bounded by `end`) into runs[].
 * Returns the run count. */
static int ntfs_decode_runs(const u8 *p, const u8 *end, struct ntfs_run *runs,
                            int max_runs) {
  int n = 0;
  u64 vcn = 0;
  i64 lcn = 0;
  while (p < end && *p != 0 && n < max_runs) {
    u8 hdr = *p++;
    u8 len_sz = hdr & 0x0F;
    u8 off_sz = (hdr >> 4) & 0x0F;
    if (len_sz == 0 || p + len_sz + off_sz > end)
      break;
    u64 run_len = 0;
    for (int i = 0; i < len_sz; i++)
      run_len |= (u64)p[i] << (8 * i);
    p += len_sz;
    i64 delta = 0;
    if (off_sz) {
      for (int i = 0; i < off_sz; i++)
        delta |= (i64)p[i] << (8 * i);
      /* sign-extend the offset delta */
      if (p[off_sz - 1] & 0x80)
        delta |= ~((i64)0) << (8 * off_sz);
      p += off_sz;
      lcn += delta;
      runs[n].lcn = (u64)lcn;
    } else {
      runs[n].lcn = 0; /* sparse run */
    }
    runs[n].vcn = vcn;
    runs[n].len = run_len;
    vcn += run_len;
    n++;
  }
  return n;
}

/* Read FILE record number `mft_no` into `out` (mft_record_size bytes), applying
 * fixup. `runs`/`run_count` describe the $MFT $DATA (pass fs->mft_runs); for the
 * bootstrap read of record 0 a temporary single run is used by the caller. */
static int ntfs_read_record(struct ntfs_fs *fs, struct ntfs_run *runs,
                            int run_count, u64 mft_no, u8 *out) {
  u64 byte_off = mft_no * fs->mft_record_size;
  if (ntfs_runlist_read(fs, runs, run_count, byte_off, fs->mft_record_size,
                        out) != (isize)fs->mft_record_size)
    return -1;
  if (memcmp(out, "FILE", 4) != 0)
    return -1;
  ntfs_apply_fixup(fs, out, fs->mft_record_size);
  return 0;
}

/* Find the first attribute of `type` in a FILE record. Returns pointer or 0. */
static u8 *ntfs_find_attr(u8 *rec, u32 rec_size, u32 type) {
  u16 off = rd16(rec + 0x14); /* first attribute offset */
  while (off + 8 <= rec_size) {
    u8 *a = rec + off;
    u32 atype = rd32(a);
    if (atype == NTFS_AT_END)
      break;
    u32 alen = rd32(a + 0x04);
    if (alen < 0x18 || off + alen > rec_size)
      break;
    if (atype == type)
      return a;
    off += alen;
  }
  return 0;
}

/* UTF-16LE → ASCII (best effort), into out (>= chars+1). */
static void ntfs_utf16_to_ascii(const u8 *src, int chars, char *out, int outsz) {
  int j = 0;
  for (int i = 0; i < chars && j < outsz - 1; i++) {
    u16 c = rd16(src + i * 2);
    out[j++] = (c < 0x80) ? (char)c : '?';
  }
  out[j] = '\0';
}

/* forward */
static void ntfs_populate_dir(struct ntfs_fs *fs, u64 dir_rec,
                              const char *base_path, int depth);
isize ntfs_vfs_read(struct vfs_node *node, u64 offset, char *buffer, usize size,
                    int flags);

/* Build a vfs node for a child file/dir record and recurse into directories. */
static void ntfs_add_child(struct ntfs_fs *fs, u64 child_rec,
                           const char *full_path, int depth) {
  u8 *rec = kmalloc(fs->mft_record_size);
  if (!rec)
    return;
  if (ntfs_read_record(fs, fs->mft_runs, fs->mft_run_count, child_rec, rec) != 0) {
    kfree(rec);
    return;
  }
  u16 flags = rd16(rec + 0x16);
  if (!(flags & NTFS_FILE_REC_IN_USE)) {
    kfree(rec);
    return;
  }
  int is_dir = (flags & NTFS_FILE_REC_IS_DIR) != 0;

  if (is_dir) {
    struct vfs_node *node = vfs_add_node(full_path, VFS_DIRECTORY, 0, 0, 0);
    if (node && !IS_ERR(node)) {
      struct ntfs_inode_info *info = kmalloc(sizeof(*info));
      memset(info, 0, sizeof(*info));
      info->fs = fs;
      node->inode->data = info;
      node->inode->flags |= VFS_NODE_OWNS_DATA;
      vfs_node_put(node);
    }
    kfree(rec);
    ntfs_populate_dir(fs, child_rec, full_path, depth + 1);
    return;
  }

  /* Regular file: locate the unnamed $DATA attribute. */
  u8 *data = ntfs_find_attr(rec, fs->mft_record_size, NTFS_AT_DATA);
  struct ntfs_inode_info *info = kmalloc(sizeof(*info));
  memset(info, 0, sizeof(*info));
  info->fs = fs;
  u64 size = 0;
  if (data) {
    if (data[0x08] == 0) { /* resident */
      u32 vlen = rd32(data + 0x10);
      u16 voff = rd16(data + 0x14);
      info->resident = 1;
      info->resident_data = kmalloc(vlen ? vlen : 1);
      memcpy(info->resident_data, data + voff, vlen);
      size = vlen;
    } else { /* non-resident */
      size = rd64(data + 0x30); /* real size */
      u16 runs_off = rd16(data + 0x20);
      u32 alen = rd32(data + 0x04);
      struct ntfs_run *runs = kmalloc(sizeof(struct ntfs_run) * 256);
      info->run_count =
          ntfs_decode_runs(data + runs_off, data + alen, runs, 256);
      info->runs = runs;
    }
  }
  info->size = size;
  kfree(rec);

  struct vfs_node *node = vfs_add_node(full_path, VFS_FILE, 0, size, 0);
  if (node && !IS_ERR(node)) {
    node->inode->data = info;
    node->inode->flags |= VFS_NODE_OWNS_DATA;
    node->inode->read_cb = ntfs_vfs_read;
    vfs_node_put(node);
  } else {
    if (info->resident_data)
      kfree(info->resident_data);
    if (info->runs)
      kfree(info->runs);
    kfree(info);
  }
}

/* Walk a block of index entries [p, end), adding each child once. `seen`
 * dedups records that appear both as B+tree separators and leaf entries. */
static void ntfs_walk_index(struct ntfs_fs *fs, u8 *p, u8 *end,
                            const char *base_path, u64 *seen, int *seen_n,
                            int depth) {
  while (p + 0x10 <= end) {
    u16 entry_len = rd16(p + 0x08);
    u16 key_len = rd16(p + 0x0A);
    u16 iflags = rd16(p + 0x0C);
    if (entry_len < 0x10)
      break;
    if (iflags & NTFS_IE_LAST)
      break; /* terminator entry has no key */
    if (key_len >= 0x42) {
      u8 *fn = p + 0x10; /* $FILE_NAME key */
      u8 name_len = fn[0x40];
      u8 ns = fn[0x41];
      u64 rec = rd64(p) & 0x0000FFFFFFFFFFFFULL;
      /* skip pure-DOS 8.3 aliases and the system metafiles (recs < 16) */
      if (ns != 2 && rec >= 16 && name_len > 0) {
        int dup = 0;
        for (int i = 0; i < *seen_n; i++)
          if (seen[i] == rec) {
            dup = 1;
            break;
          }
        if (!dup && *seen_n < NTFS_MAX_DIR_ENTRIES) {
          seen[(*seen_n)++] = rec;
          char name[256];
          ntfs_utf16_to_ascii(fn + 0x42, name_len, name, sizeof(name));
          if (strcmp(name, ".") != 0) {
            char full[512];
            usize len = strlen(base_path);
            if (len + 1 + strlen(name) + 1 < sizeof(full)) {
              memcpy(full, base_path, len);
              if (len == 0 || full[len - 1] != '/')
                full[len++] = '/';
              strcpy(full + len, name);
              ntfs_add_child(fs, rec, full, depth);
            }
          }
        }
      }
    }
    p += entry_len;
  }
}

static void ntfs_populate_dir(struct ntfs_fs *fs, u64 dir_rec,
                              const char *base_path, int depth) {
  if (depth > NTFS_MAX_DEPTH)
    return;
  u8 *rec = kmalloc(fs->mft_record_size);
  if (!rec)
    return;
  if (ntfs_read_record(fs, fs->mft_runs, fs->mft_run_count, dir_rec, rec) != 0) {
    kfree(rec);
    return;
  }

  u64 *seen = kmalloc(sizeof(u64) * NTFS_MAX_DIR_ENTRIES);
  int seen_n = 0;
  if (!seen) {
    kfree(rec);
    return;
  }

  /* $INDEX_ROOT (resident): the root index node lives at value + 0x10. */
  u8 *iroot = ntfs_find_attr(rec, fs->mft_record_size, NTFS_AT_INDEX_ROOT);
  if (iroot && iroot[0x08] == 0) {
    u16 voff = rd16(iroot + 0x14);
    u8 *val = iroot + voff;
    u8 *node_hdr = val + 0x10;
    u32 entries_off = rd32(node_hdr + 0x00);
    u32 index_len = rd32(node_hdr + 0x04);
    u8 *p = node_hdr + entries_off;
    u8 *e = node_hdr + index_len;
    if (e > rec + fs->mft_record_size)
      e = rec + fs->mft_record_size;
    ntfs_walk_index(fs, p, e, base_path, seen, &seen_n, depth);
  }

  /* $INDEX_ALLOCATION (non-resident): a series of INDX blocks. */
  u8 *ialloc = ntfs_find_attr(rec, fs->mft_record_size, NTFS_AT_INDEX_ALLOCATION);
  if (ialloc && ialloc[0x08] != 0) {
    u64 total = rd64(ialloc + 0x30); /* real size of the allocation */
    u16 runs_off = rd16(ialloc + 0x20);
    u32 alen = rd32(ialloc + 0x04);
    struct ntfs_run *iruns = kmalloc(sizeof(struct ntfs_run) * 256);
    int ircnt = ntfs_decode_runs(ialloc + runs_off, ialloc + alen, iruns, 256);
    /* index block size: INDX records are typically one cluster; derive from the
     * first block's header rather than guessing. */
    u32 blk = fs->cluster_size;
    u8 *ib = kmalloc(blk);
    if (ib && iruns && ircnt > 0) {
      for (u64 off = 0; off + blk <= total; off += blk) {
        if (ntfs_runlist_read(fs, iruns, ircnt, off, blk, ib) != (isize)blk)
          break;
        if (memcmp(ib, "INDX", 4) != 0)
          continue; /* unallocated block */
        ntfs_apply_fixup(fs, ib, blk);
        u8 *node_hdr = ib + 0x18; /* INDX node header */
        u32 entries_off = rd32(node_hdr + 0x00);
        u32 index_len = rd32(node_hdr + 0x04);
        u8 *p = node_hdr + entries_off;
        u8 *e = node_hdr + index_len;
        if (e > ib + blk)
          e = ib + blk;
        ntfs_walk_index(fs, p, e, base_path, seen, &seen_n, depth);
      }
    }
    if (ib)
      kfree(ib);
    if (iruns)
      kfree(iruns);
  }

  kfree(seen);
  kfree(rec);
}

isize ntfs_vfs_read(struct vfs_node *node, u64 offset, char *buffer, usize size,
                    int flags) {
  (void)flags;
  struct ntfs_inode_info *info = (struct ntfs_inode_info *)node->inode->data;
  if (!info)
    return -EIO;
  if (offset >= info->size)
    return 0;
  if (offset + size > info->size)
    size = (usize)(info->size - offset);
  if (size == 0)
    return 0;
  if (info->resident) {
    memcpy(buffer, info->resident_data + offset, size);
    return (isize)size;
  }
  return ntfs_runlist_read(info->fs, info->runs, info->run_count, offset, size,
                           (u8 *)buffer);
}

static struct vfs_node *ntfs_vfs_mount_cb(const char *source, u64 flags,
                                          void *data) {
  (void)flags;
  struct block_device *dev = blk_get(source);
  if (!dev)
    return ERR_PTR(-ENODEV);

  u8 *bs = kmalloc(512);
  if (!bs)
    return ERR_PTR(-ENOMEM);
  if (blk_read_cached(dev, 0, 1, bs) < 0) {
    kfree(bs);
    return ERR_PTR(-EIO);
  }
  if (memcmp(bs + 3, "NTFS    ", 8) != 0) {
    kfree(bs);
    return ERR_PTR(-EINVAL);
  }

  struct ntfs_fs *fs = kmalloc(sizeof(struct ntfs_fs));
  memset(fs, 0, sizeof(*fs));
  fs->bdev = dev;
  fs->bytes_per_sector = rd16(bs + 0x0B);
  fs->sectors_per_cluster = bs[0x0D];
  fs->cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
  fs->mft_lcn = rd64(bs + 0x30);
  i8 cpr = (i8)bs[0x40]; /* clusters per MFT record (signed) */
  if (cpr > 0)
    fs->mft_record_size = cpr * fs->cluster_size;
  else
    fs->mft_record_size = 1u << (-cpr);
  kfree(bs);

  if (fs->cluster_size == 0 || fs->mft_record_size == 0 ||
      fs->bytes_per_sector < dev->block_size) {
    kfree(fs);
    return ERR_PTR(-EINVAL);
  }

  /* Bootstrap: read $MFT record 0 from a temporary run anchored at mft_lcn,
   * then adopt its real $DATA run list so any record becomes reachable. */
  struct ntfs_run boot_run = {.vcn = 0, .lcn = fs->mft_lcn, .len = ~0ULL};
  u8 *rec0 = kmalloc(fs->mft_record_size);
  if (!rec0 || ntfs_read_record(fs, &boot_run, 1, 0, rec0) != 0) {
    if (rec0)
      kfree(rec0);
    kfree(fs);
    return ERR_PTR(-EIO);
  }
  u8 *mdata = ntfs_find_attr(rec0, fs->mft_record_size, NTFS_AT_DATA);
  if (!mdata || mdata[0x08] == 0) {
    kfree(rec0);
    kfree(fs);
    return ERR_PTR(-EINVAL);
  }
  {
    u16 runs_off = rd16(mdata + 0x20);
    u32 alen = rd32(mdata + 0x04);
    fs->mft_runs = kmalloc(sizeof(struct ntfs_run) * 256);
    fs->mft_run_count =
        ntfs_decode_runs(mdata + runs_off, mdata + alen, fs->mft_runs, 256);
  }
  kfree(rec0);
  if (fs->mft_run_count == 0) {
    kfree(fs->mft_runs);
    kfree(fs);
    return ERR_PTR(-EINVAL);
  }

  struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
  if (!root) {
    kfree(fs->mft_runs);
    kfree(fs);
    return ERR_PTR(-ENOMEM);
  }
  struct ntfs_inode_info *rinfo = kmalloc(sizeof(*rinfo));
  memset(rinfo, 0, sizeof(*rinfo));
  rinfo->fs = fs;
  root->inode->data = rinfo;
  root->inode->flags |= VFS_NODE_OWNS_DATA;

  fs->next = ntfs_instances;
  ntfs_instances = fs;

  vfs_set_currently_mounting_root(root);
  if (data) {
    /* The NTFS root directory is fixed FILE record 5. */
    ntfs_populate_dir(fs, 5, (const char *)data, 0);
  }
  return root;
}

static int ntfs_vfs_umount_cb(struct vfs_node *root_node) {
  (void)root_node;
  return 0;
}

static struct vfs_fs ntfs_vfs = {
    .name = "ntfs",
    .mount = ntfs_vfs_mount_cb,
    .umount = ntfs_vfs_umount_cb,
};

void ntfs_init(void) { vfs_register_fs(&ntfs_vfs); }

/* ── M95: ntfs is a loadable module ──────────────────────────────────────── */
#include <b1nix/module.h>

MODULE_NAME("ntfs");
MODULE_LICENSE("MIT");
MODULE_AUTHOR("b1nix");
MODULE_DESCRIPTION("NTFS read-only filesystem");
MODULE_ALIAS("fs-ntfs");
MODULE_ALIAS("fs-ntfs3");

static int ntfs_module_init(void) {
	ntfs_init();
	return 0;
}

static void ntfs_module_exit(void) { vfs_unregister_fs(&ntfs_vfs); }

module_init(ntfs_module_init);
module_exit(ntfs_module_exit);
