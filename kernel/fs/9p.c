#include <b1nix/virtio_9p.h>
#include <b1nix/console.h>
#include <b1nix/dirent.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/kprintf.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/vfs.h>
#include <string.h>

#define P9_DOTL_AT_REMOVEDIR 0x200

struct p9_inode_info {
  struct virtio_9p_dev *p9dev;
  u32 fid;
  struct p9_qid qid;
  int opened;
};

static u32 g_p9_next_fid = 2;

static u32 p9_alloc_fid(void) {
  return __sync_fetch_and_add(&g_p9_next_fid, 1);
}

/* Marshalling helpers */
static void p9_buf_init(struct p9_buffer *buf, void *data, usize cap) {
  buf->data = (u8 *)data;
  buf->capacity = cap;
  buf->offset = 0;
}

static int p9_put_u8(struct p9_buffer *buf, u8 val) {
  if (buf->offset + 1 > buf->capacity)
    return -1;
  buf->data[buf->offset++] = val;
  return 0;
}

static int p9_put_u16(struct p9_buffer *buf, u16 val) {
  if (buf->offset + 2 > buf->capacity)
    return -1;
  buf->data[buf->offset++] = (u8)(val & 0xFF);
  buf->data[buf->offset++] = (u8)((val >> 8) & 0xFF);
  return 0;
}

static int p9_put_u32(struct p9_buffer *buf, u32 val) {
  if (buf->offset + 4 > buf->capacity)
    return -1;
  buf->data[buf->offset++] = (u8)(val & 0xFF);
  buf->data[buf->offset++] = (u8)((val >> 8) & 0xFF);
  buf->data[buf->offset++] = (u8)((val >> 16) & 0xFF);
  buf->data[buf->offset++] = (u8)((val >> 24) & 0xFF);
  return 0;
}

static int p9_put_u64(struct p9_buffer *buf, u64 val) {
  if (buf->offset + 8 > buf->capacity)
    return -1;
  for (int i = 0; i < 8; i++) {
    buf->data[buf->offset++] = (u8)((val >> (i * 8)) & 0xFF);
  }
  return 0;
}

static int p9_put_str(struct p9_buffer *buf, const char *s) {
  u16 len = (u16)(s ? strlen(s) : 0);
  if (p9_put_u16(buf, len) < 0)
    return -1;
  if (buf->offset + len > buf->capacity)
    return -1;
  if (len > 0)
    memcpy(buf->data + buf->offset, s, len);
  buf->offset += len;
  return 0;
}

static int p9_put_data(struct p9_buffer *buf, const void *data, usize len) {
  if (buf->offset + len > buf->capacity)
    return -1;
  if (len > 0)
    memcpy(buf->data + buf->offset, data, len);
  buf->offset += len;
  return 0;
}

static u8 p9_get_u8(struct p9_buffer *buf) {
  if (buf->offset + 1 > buf->capacity)
    return 0;
  return buf->data[buf->offset++];
}

static u16 p9_get_u16(struct p9_buffer *buf) {
  if (buf->offset + 2 > buf->capacity)
    return 0;
  u16 val =
      (u16)buf->data[buf->offset] | ((u16)buf->data[buf->offset + 1] << 8);
  buf->offset += 2;
  return val;
}

static u32 p9_get_u32(struct p9_buffer *buf) {
  if (buf->offset + 4 > buf->capacity)
    return 0;
  u32 val = (u32)buf->data[buf->offset] |
            ((u32)buf->data[buf->offset + 1] << 8) |
            ((u32)buf->data[buf->offset + 2] << 16) |
            ((u32)buf->data[buf->offset + 3] << 24);
  buf->offset += 4;
  return val;
}

static u64 p9_get_u64(struct p9_buffer *buf) {
  if (buf->offset + 8 > buf->capacity)
    return 0;
  u64 val = 0;
  for (int i = 0; i < 8; i++) {
    val |= ((u64)buf->data[buf->offset + i]) << (i * 8);
  }
  buf->offset += 8;
  return val;
}

static int p9_get_qid(struct p9_buffer *buf, struct p9_qid *qid) {
  if (buf->offset + 13 > buf->capacity)
    return -1;
  qid->type = p9_get_u8(buf);
  qid->version = p9_get_u32(buf);
  qid->path = p9_get_u64(buf);
  return 0;
}

static int p9_get_str(struct p9_buffer *buf, char *out, usize max_len) {
  u16 len = p9_get_u16(buf);
  if (buf->offset + len > buf->capacity)
    return -1;
  usize to_copy = (len < max_len - 1) ? len : max_len - 1;
  memcpy(out, buf->data + buf->offset, to_copy);
  out[to_copy] = '\0';
  buf->offset += len;
  return 0;
}

/* 9P Protocol Actions */

static int p9_proto_version(struct virtio_9p_dev *p9dev) {
  struct p9_buffer req, resp;
  p9_buf_init(&req, p9dev->req_buf, p9dev->msize);

  p9_put_u32(&req, 0); /* Size placeholder */
  p9_put_u8(&req, P9_TVERSION);
  p9_put_u16(&req, P9_NOTAG);
  p9_put_u32(&req, p9dev->msize);
  p9_put_str(&req, "9P2000.L");

  usize actual_resp_len = 0;
  int err = virtio_9p_transact(p9dev, req.offset, p9dev->msize, &actual_resp_len);
  if (err < 0)
    return err;

  p9_buf_init(&resp, p9dev->resp_buf, actual_resp_len);
  p9_get_u32(&resp); /* size */
  u8 type = p9_get_u8(&resp);
  p9_get_u16(&resp); /* tag */

  if (type != P9_RVERSION)
    return -EIO;

  u32 agreed_msize = p9_get_u32(&resp);
  char ver[32];
  p9_get_str(&resp, ver, sizeof(ver));

  if (agreed_msize > 0 && agreed_msize < p9dev->msize)
    p9dev->msize = agreed_msize;

  k_info("virtio-9p", "negotiated version='%s' msize=%u", ver, p9dev->msize);
  return 0;
}

static int p9_proto_attach(struct virtio_9p_dev *p9dev, u32 root_fid,
                           struct p9_qid *root_qid) {
  struct p9_buffer req, resp;
  p9_buf_init(&req, p9dev->req_buf, p9dev->msize);

  p9_put_u32(&req, 0);
  p9_put_u8(&req, P9_TATTACH);
  p9_put_u16(&req, 1);
  p9_put_u32(&req, root_fid);
  p9_put_u32(&req, P9_NOFID);
  p9_put_str(&req, "root");
  p9_put_str(&req, "");
  p9_put_u32(&req, 0); /* n_uname */

  usize actual_resp = 0;
  int err = virtio_9p_transact(p9dev, req.offset, p9dev->msize, &actual_resp);
  if (err < 0)
    return err;

  p9_buf_init(&resp, p9dev->resp_buf, actual_resp);
  p9_get_u32(&resp);
  u8 type = p9_get_u8(&resp);
  p9_get_u16(&resp);

  if (type != P9_RATTACH)
    return -EIO;

  return p9_get_qid(&resp, root_qid);
}

static int p9_proto_walk(struct virtio_9p_dev *p9dev, u32 fid, u32 new_fid,
                         const char *name, struct p9_qid *out_qid) {
  struct p9_buffer req, resp;
  p9_buf_init(&req, p9dev->req_buf, p9dev->msize);

  p9_put_u32(&req, 0);
  p9_put_u8(&req, P9_TWALK);
  p9_put_u16(&req, 1);
  p9_put_u32(&req, fid);
  p9_put_u32(&req, new_fid);

  if (name && name[0]) {
    p9_put_u16(&req, 1);
    p9_put_str(&req, name);
  } else {
    p9_put_u16(&req, 0);
  }

  usize actual_resp = 0;
  int err = virtio_9p_transact(p9dev, req.offset, p9dev->msize, &actual_resp);
  if (err < 0)
    return err;

  p9_buf_init(&resp, p9dev->resp_buf, actual_resp);
  p9_get_u32(&resp);
  u8 type = p9_get_u8(&resp);
  p9_get_u16(&resp);

  if (type != P9_RWALK)
    return -EIO;

  u16 nwqid = p9_get_u16(&resp);
  if (name && name[0] && nwqid == 0)
    return -ENOENT;

  if (nwqid > 0 && out_qid) {
    p9_get_qid(&resp, out_qid);
  }
  return 0;
}

static int p9_proto_clunk(struct virtio_9p_dev *p9dev, u32 fid) {
  struct p9_buffer req;
  p9_buf_init(&req, p9dev->req_buf, p9dev->msize);

  p9_put_u32(&req, 0);
  p9_put_u8(&req, P9_TCLUNK);
  p9_put_u16(&req, 1);
  p9_put_u32(&req, fid);

  usize actual_resp = 0;
  return virtio_9p_transact(p9dev, req.offset, p9dev->msize, &actual_resp);
}

static int p9_proto_fsync(struct virtio_9p_dev *p9dev, u32 fid) {
  struct p9_buffer req, resp;
  p9_buf_init(&req, p9dev->req_buf, p9dev->msize);

  p9_put_u32(&req, 0);
  p9_put_u8(&req, P9_TFSYNC);
  p9_put_u16(&req, 1);
  p9_put_u32(&req, fid);
  p9_put_u32(&req, 0); /* datasync */

  usize actual_resp = 0;
  int err = virtio_9p_transact(p9dev, req.offset, p9dev->msize, &actual_resp);
  if (err < 0)
    return err;

  p9_buf_init(&resp, p9dev->resp_buf, actual_resp);
  p9_get_u32(&resp);
  u8 type = p9_get_u8(&resp);
  if (type != P9_RFSYNC)
    return -EIO;
  return 0;
}

static int p9_proto_getattr(struct virtio_9p_dev *p9dev, u32 fid, u64 mask,
                            struct p9_rgetattr *out) {
  struct p9_buffer req, resp;
  p9_buf_init(&req, p9dev->req_buf, p9dev->msize);

  p9_put_u32(&req, 0);
  p9_put_u8(&req, P9_TGETATTR);
  p9_put_u16(&req, 1);
  p9_put_u32(&req, fid);
  p9_put_u64(&req, mask);

  usize actual_resp = 0;
  int err = virtio_9p_transact(p9dev, req.offset, p9dev->msize, &actual_resp);
  if (err < 0)
    return err;

  p9_buf_init(&resp, p9dev->resp_buf, actual_resp);
  p9_get_u32(&resp);
  u8 type = p9_get_u8(&resp);
  p9_get_u16(&resp);

  if (type != P9_RGETATTR)
    return -EIO;

  memset(out, 0, sizeof(*out));
  out->valid = p9_get_u64(&resp);
  p9_get_qid(&resp, &out->qid);
  out->mode = p9_get_u32(&resp);
  out->uid = p9_get_u32(&resp);
  out->gid = p9_get_u32(&resp);
  out->nlink = p9_get_u64(&resp);
  out->rdev = p9_get_u64(&resp);
  out->size = p9_get_u64(&resp);
  out->blksize = p9_get_u64(&resp);
  out->blocks = p9_get_u64(&resp);
  out->atime_sec = p9_get_u64(&resp);
  out->atime_nsec = p9_get_u64(&resp);
  out->mtime_sec = p9_get_u64(&resp);
  out->mtime_nsec = p9_get_u64(&resp);
  out->ctime_sec = p9_get_u64(&resp);
  out->ctime_nsec = p9_get_u64(&resp);
  return 0;
}

static int p9_proto_setattr(struct virtio_9p_dev *p9dev, u32 fid, u32 valid,
                            u32 mode, u32 uid, u32 gid, u64 size,
                            u64 atime_sec, u64 atime_nsec,
                            u64 mtime_sec, u64 mtime_nsec) {
  struct p9_buffer req;
  p9_buf_init(&req, p9dev->req_buf, p9dev->msize);

  p9_put_u32(&req, 0);
  p9_put_u8(&req, P9_TSETATTR);
  p9_put_u16(&req, 1);
  p9_put_u32(&req, fid);
  p9_put_u32(&req, valid);
  p9_put_u32(&req, mode);
  p9_put_u32(&req, uid);
  p9_put_u32(&req, gid);
  p9_put_u64(&req, size);
  p9_put_u64(&req, atime_sec);
  p9_put_u64(&req, atime_nsec);
  p9_put_u64(&req, mtime_sec);
  p9_put_u64(&req, mtime_nsec);

  usize actual_resp = 0;
  return virtio_9p_transact(p9dev, req.offset, p9dev->msize, &actual_resp);
}

static int p9_proto_lopen(struct virtio_9p_dev *p9dev, u32 fid, u32 flags) {
  struct p9_buffer req, resp;
  p9_buf_init(&req, p9dev->req_buf, p9dev->msize);

  p9_put_u32(&req, 0);
  p9_put_u8(&req, P9_TLOPEN);
  p9_put_u16(&req, 1);
  p9_put_u32(&req, fid);
  p9_put_u32(&req, flags);

  usize actual_resp = 0;
  int err = virtio_9p_transact(p9dev, req.offset, p9dev->msize, &actual_resp);
  if (err < 0)
    return err;

  p9_buf_init(&resp, p9dev->resp_buf, actual_resp);
  p9_get_u32(&resp);
  u8 type = p9_get_u8(&resp);
  p9_get_u16(&resp);

  if (type != P9_RLOPEN)
    return -EIO;

  return 0;
}

static int p9_proto_lcreate(struct virtio_9p_dev *p9dev, u32 fid,
                            const char *name, u32 flags, u32 mode, u32 gid,
                            struct p9_qid *out_qid) {
  struct p9_buffer req, resp;
  p9_buf_init(&req, p9dev->req_buf, p9dev->msize);

  p9_put_u32(&req, 0);
  p9_put_u8(&req, P9_TLCREATE);
  p9_put_u16(&req, 1);
  p9_put_u32(&req, fid);
  p9_put_str(&req, name);
  p9_put_u32(&req, flags);
  p9_put_u32(&req, mode);
  p9_put_u32(&req, gid);

  usize actual_resp = 0;
  int err = virtio_9p_transact(p9dev, req.offset, p9dev->msize, &actual_resp);
  if (err < 0)
    return err;

  p9_buf_init(&resp, p9dev->resp_buf, actual_resp);
  p9_get_u32(&resp);
  u8 type = p9_get_u8(&resp);
  p9_get_u16(&resp);

  if (type != P9_RLCREATE)
    return -EIO;

  if (out_qid) {
    p9_get_qid(&resp, out_qid);
  }
  return 0;
}

static int p9_proto_mkdir(struct virtio_9p_dev *p9dev, u32 fid,
                          const char *name, u32 mode, u32 gid,
                          struct p9_qid *out_qid) {
  struct p9_buffer req, resp;
  p9_buf_init(&req, p9dev->req_buf, p9dev->msize);

  p9_put_u32(&req, 0);
  p9_put_u8(&req, P9_TMKDIR);
  p9_put_u16(&req, 1);
  p9_put_u32(&req, fid);
  p9_put_str(&req, name);
  p9_put_u32(&req, mode);
  p9_put_u32(&req, gid);

  usize actual_resp = 0;
  int err = virtio_9p_transact(p9dev, req.offset, p9dev->msize, &actual_resp);
  if (err < 0)
    return err;

  p9_buf_init(&resp, p9dev->resp_buf, actual_resp);
  p9_get_u32(&resp);
  u8 type = p9_get_u8(&resp);
  p9_get_u16(&resp);

  if (type != P9_RMKDIR)
    return -EIO;

  if (out_qid) {
    p9_get_qid(&resp, out_qid);
  }
  return 0;
}

static int p9_proto_unlinkat(struct virtio_9p_dev *p9dev, u32 dir_fid,
                             const char *name, u32 flags) {
  struct p9_buffer req;
  p9_buf_init(&req, p9dev->req_buf, p9dev->msize);

  p9_put_u32(&req, 0);
  p9_put_u8(&req, P9_TUNLINKAT);
  p9_put_u16(&req, 1);
  p9_put_u32(&req, dir_fid);
  p9_put_str(&req, name);
  p9_put_u32(&req, flags);

  usize actual_resp = 0;
  return virtio_9p_transact(p9dev, req.offset, p9dev->msize, &actual_resp);
}

static isize p9_proto_read(struct virtio_9p_dev *p9dev, u32 fid, u64 offset,
                           char *buffer, usize size) {
  struct p9_buffer req, resp;
  p9_buf_init(&req, p9dev->req_buf, p9dev->msize);

  u32 max_chunk = p9dev->msize > 24 ? p9dev->msize - 24 : 4096;
  if (size > max_chunk)
    size = max_chunk;

  p9_put_u32(&req, 0);
  p9_put_u8(&req, P9_TREAD);
  p9_put_u16(&req, 1);
  p9_put_u32(&req, fid);
  p9_put_u64(&req, offset);
  p9_put_u32(&req, (u32)size);

  usize actual_resp = 0;
  int err = virtio_9p_transact(p9dev, req.offset, p9dev->msize, &actual_resp);
  if (err < 0)
    return err;

  p9_buf_init(&resp, p9dev->resp_buf, actual_resp);
  p9_get_u32(&resp);
  u8 type = p9_get_u8(&resp);
  p9_get_u16(&resp);

  if (type != P9_RREAD)
    return -EIO;

  u32 count = p9_get_u32(&resp);
  if (count > size)
    count = (u32)size;

  if (count > 0 && resp.offset + count <= resp.capacity) {
    memcpy(buffer, resp.data + resp.offset, count);
  }
  return (isize)count;
}

static isize p9_proto_write(struct virtio_9p_dev *p9dev, u32 fid, u64 offset,
                            const char *buffer, usize size) {
  struct p9_buffer req, resp;
  p9_buf_init(&req, p9dev->req_buf, p9dev->msize);

  u32 max_chunk = p9dev->msize > 24 ? p9dev->msize - 24 : 4096;
  if (size > max_chunk)
    size = max_chunk;

  p9_put_u32(&req, 0);
  p9_put_u8(&req, P9_TWRITE);
  p9_put_u16(&req, 1);
  p9_put_u32(&req, fid);
  p9_put_u64(&req, offset);
  p9_put_u32(&req, (u32)size);
  p9_put_data(&req, buffer, size);

  usize actual_resp = 0;
  int err = virtio_9p_transact(p9dev, req.offset, p9dev->msize, &actual_resp);
  if (err < 0)
    return err;

  p9_buf_init(&resp, p9dev->resp_buf, actual_resp);
  p9_get_u32(&resp);
  u8 type = p9_get_u8(&resp);
  p9_get_u16(&resp);

  if (type != P9_RWRITE)
    return -EIO;

  u32 written = p9_get_u32(&resp);
  return (isize)written;
}

/* VFS Callbacks */

static int p9_vfs_lookup(struct vfs_node *dir, const char *name);
static isize p9_vfs_read(struct vfs_node *node, u64 offset, char *buffer,
                         usize size, int flags);
static isize p9_vfs_write(struct vfs_node *node, u64 offset, const char *buffer,
                          usize size, int flags);
static isize p9_vfs_readdir(struct vfs_node *dir, usize offset,
                            struct dirent *buf, usize max_entries);
static int p9_vfs_create(struct vfs_node *dir, const char *name,
                         const char *full_path, u32 mode);
static int p9_vfs_mkdir(struct vfs_node *dir, const char *name, u32 mode);
static int p9_vfs_unlink(struct vfs_node *dir, const char *name);
static int p9_vfs_rmdir(struct vfs_node *dir, const char *name);
static void p9_vfs_getattr(struct vfs_node *node);
static int p9_vfs_setattr(struct vfs_node *node);
static int p9_vfs_statfs(struct vfs_node *node, struct b1nix_statfs *st);
static void p9_vfs_release(struct vfs_node *node);
static int p9_vfs_fsync(struct vfs_node *node);

static void p9_setup_node_ops(struct vfs_node *node, struct virtio_9p_dev *p9dev,
                              u32 fid, struct p9_qid *qid, u32 mode, u64 size) {
  struct p9_inode_info *info = kzalloc(sizeof(struct p9_inode_info));
  if (info) {
    info->p9dev = p9dev;
    info->fid = fid;
    info->qid = *qid;
    node->inode->data = info;
    node->inode->flags |= VFS_NODE_OWNS_DATA;
  }

  node->inode->mode = (u16)mode;
  node->inode->size = (usize)size;
  node->inode->read_cb = p9_vfs_read;
  node->inode->write_cb = p9_vfs_write;
  node->inode->readdir_cb = p9_vfs_readdir;
  node->inode->lookup_cb = p9_vfs_lookup;
  node->inode->create_cb = p9_vfs_create;
  node->inode->mkdir_cb = p9_vfs_mkdir;
  node->inode->unlink_cb = p9_vfs_unlink;
  node->inode->rmdir_cb = p9_vfs_rmdir;
  node->inode->getattr_cb = p9_vfs_getattr;
  node->inode->setattr_cb = p9_vfs_setattr;
  node->inode->statfs_cb = p9_vfs_statfs;
  node->inode->release_cb = p9_vfs_release;
  node->inode->fsync_cb = p9_vfs_fsync;
}

static int p9_vfs_lookup(struct vfs_node *dir, const char *name) {
  if (!dir || !dir->inode || !dir->inode->data || !name)
    return -EINVAL;

  struct p9_inode_info *parent_info = (struct p9_inode_info *)dir->inode->data;
  struct virtio_9p_dev *p9dev = parent_info->p9dev;

  u32 new_fid = p9_alloc_fid();
  struct p9_qid qid;
  int err = p9_proto_walk(p9dev, parent_info->fid, new_fid, name, &qid);
  if (err < 0) {
    return err;
  }

  struct p9_rgetattr ga;
  err = p9_proto_getattr(p9dev, new_fid, P9_GETATTR_BASIC, &ga);
  if (err < 0) {
    p9_proto_clunk(p9dev, new_fid);
    return err;
  }

  enum vfs_node_type type = VFS_FILE;
  if ((ga.mode & B1NIX_S_IFMT) == B1NIX_S_IFDIR || (qid.type & P9_QTDIR)) {
    type = VFS_DIRECTORY;
  } else if ((ga.mode & B1NIX_S_IFMT) == B1NIX_S_IFLNK || (qid.type & P9_QTSYMLINK)) {
    type = VFS_SYMLINK;
  }

  /* Open the fid if regular file */
  if (type == VFS_FILE) {
    p9_proto_lopen(p9dev, new_fid, B1NIX_O_RDWR);
  }

  struct vfs_node *child = vfs_create_node(type);
  if (!child) {
    p9_proto_clunk(p9dev, new_fid);
    return -ENOMEM;
  }

  strncpy(child->name, name, VFS_NAME_MAX - 1);
  child->name[VFS_NAME_MAX - 1] = '\0';
  child->inode->uid = (u16)ga.uid;
  child->inode->gid = (u16)ga.gid;
  child->inode->nlink = (int)ga.nlink;
  child->inode->atime = ga.atime_sec;
  child->inode->atime_nsec = (u32)ga.atime_nsec;
  child->inode->mtime = ga.mtime_sec;
  child->inode->mtime_nsec = (u32)ga.mtime_nsec;
  child->inode->ctime = ga.ctime_sec;
  child->inode->ctime_nsec = (u32)ga.ctime_nsec;

  p9_setup_node_ops(child, p9dev, new_fid, &qid, ga.mode, ga.size);
  vfs_attach_child(dir, child);
  return 0;
}

static isize p9_vfs_read(struct vfs_node *node, u64 offset, char *buffer,
                         usize size, int flags) {
  (void)flags;
  if (!node || !node->inode || !node->inode->data)
    return -EINVAL;

  struct p9_inode_info *info = (struct p9_inode_info *)node->inode->data;
  usize total = 0;

  while (total < size) {
    usize chunk = size - total;
    isize n = p9_proto_read(info->p9dev, info->fid, offset + total,
                            buffer + total, chunk);
    if (n < 0) {
      return total > 0 ? (isize)total : n;
    }
    if (n == 0)
      break;
    total += (usize)n;
  }
  return (isize)total;
}

static isize p9_vfs_write(struct vfs_node *node, u64 offset, const char *buffer,
                          usize size, int flags) {
  (void)flags;
  if (!node || !node->inode || !node->inode->data)
    return -EINVAL;

  struct p9_inode_info *info = (struct p9_inode_info *)node->inode->data;
  usize total = 0;

  while (total < size) {
    usize chunk = size - total;
    isize n = p9_proto_write(info->p9dev, info->fid, offset + total,
                             buffer + total, chunk);
    if (n < 0) {
      return total > 0 ? (isize)total : n;
    }
    if (n == 0)
      break;
    total += (usize)n;
  }

  if (offset + total > node->inode->size) {
    node->inode->size = (usize)(offset + total);
  }
  return (isize)total;
}

static isize p9_vfs_readdir(struct vfs_node *dir, usize offset,
                            struct dirent *buf, usize max_entries) {
  if (!dir || !dir->inode || !dir->inode->data || !buf)
    return -EINVAL;

  struct p9_inode_info *info = (struct p9_inode_info *)dir->inode->data;
  struct virtio_9p_dev *p9dev = info->p9dev;

  /* Open dir if needed */
  p9_proto_lopen(p9dev, info->fid, B1NIX_O_RDONLY | B1NIX_O_DIRECTORY);

  struct p9_buffer req, resp;
  p9_buf_init(&req, p9dev->req_buf, p9dev->msize);

  p9_put_u32(&req, 0);
  p9_put_u8(&req, P9_TREADDIR);
  p9_put_u16(&req, 1);
  p9_put_u32(&req, info->fid);
  p9_put_u64(&req, (u64)offset);
  p9_put_u32(&req, (u32)(p9dev->msize - 24));

  usize actual_resp = 0;
  int err = virtio_9p_transact(p9dev, req.offset, p9dev->msize, &actual_resp);
  if (err < 0)
    return err;

  p9_buf_init(&resp, p9dev->resp_buf, actual_resp);
  p9_get_u32(&resp);
  u8 type = p9_get_u8(&resp);
  p9_get_u16(&resp);

  if (type != P9_RREADDIR)
    return -EIO;

  u32 count = p9_get_u32(&resp);
  usize entries_read = 0;
  usize start_offset = resp.offset;

  while (resp.offset < start_offset + count && entries_read < max_entries) {
    struct p9_qid qid;
    if (p9_get_qid(&resp, &qid) < 0)
      break;
    u64 next_off = p9_get_u64(&resp);
    (void)next_off;
    u8 dtype = p9_get_u8(&resp);
    char name[VFS_NAME_MAX];
    if (p9_get_str(&resp, name, sizeof(name)) < 0)
      break;

    strncpy(buf[entries_read].name, name, 63);
    buf[entries_read].name[63] = '\0';
    buf[entries_read].ino = qid.path;
    buf[entries_read].type = (dtype == 4) ? (u32)VFS_DIRECTORY : (u32)VFS_FILE;
    buf[entries_read].is_dir = (dtype == 4);
    buf[entries_read].size = 0;
    entries_read++;
  }

  return (isize)entries_read;
}

static int p9_vfs_create(struct vfs_node *dir, const char *name,
                         const char *full_path, u32 mode) {
  (void)full_path;
  if (!dir || !dir->inode || !dir->inode->data || !name)
    return -EINVAL;

  struct p9_inode_info *parent_info = (struct p9_inode_info *)dir->inode->data;
  struct virtio_9p_dev *p9dev = parent_info->p9dev;

  u32 new_fid = p9_alloc_fid();
  /* Clone parent fid */
  int err = p9_proto_walk(p9dev, parent_info->fid, new_fid, NULL, NULL);
  if (err < 0)
    return err;

  struct p9_qid qid;
  err = p9_proto_lcreate(p9dev, new_fid, name,
                         B1NIX_O_RDWR | B1NIX_O_CREAT | B1NIX_O_TRUNC,
                         mode, 0, &qid);
  if (err < 0) {
    p9_proto_clunk(p9dev, new_fid);
    return err;
  }

  struct vfs_node *child = vfs_create_node(VFS_FILE);
  if (!child) {
    p9_proto_clunk(p9dev, new_fid);
    return -ENOMEM;
  }

  strncpy(child->name, name, VFS_NAME_MAX - 1);
  child->name[VFS_NAME_MAX - 1] = '\0';
  p9_setup_node_ops(child, p9dev, new_fid, &qid, mode, 0);
  vfs_attach_child(dir, child);
  return 0;
}

static int p9_vfs_mkdir(struct vfs_node *dir, const char *name, u32 mode) {
  if (!dir || !dir->inode || !dir->inode->data || !name)
    return -EINVAL;

  struct p9_inode_info *parent_info = (struct p9_inode_info *)dir->inode->data;
  struct virtio_9p_dev *p9dev = parent_info->p9dev;

  struct p9_qid qid;
  int err = p9_proto_mkdir(p9dev, parent_info->fid, name, mode, 0, &qid);
  if (err < 0)
    return err;

  u32 new_fid = p9_alloc_fid();
  err = p9_proto_walk(p9dev, parent_info->fid, new_fid, name, &qid);
  if (err < 0)
    return err;

  struct vfs_node *child = vfs_create_node(VFS_DIRECTORY);
  if (!child) {
    p9_proto_clunk(p9dev, new_fid);
    return -ENOMEM;
  }

  strncpy(child->name, name, VFS_NAME_MAX - 1);
  child->name[VFS_NAME_MAX - 1] = '\0';
  p9_setup_node_ops(child, p9dev, new_fid, &qid, mode | B1NIX_S_IFDIR, 0);
  vfs_attach_child(dir, child);
  return 0;
}

static int p9_vfs_unlink(struct vfs_node *dir, const char *name) {
  if (!dir || !dir->inode || !dir->inode->data || !name)
    return -EINVAL;

  struct p9_inode_info *info = (struct p9_inode_info *)dir->inode->data;
  return p9_proto_unlinkat(info->p9dev, info->fid, name, 0);
}

static int p9_vfs_rmdir(struct vfs_node *dir, const char *name) {
  if (!dir || !dir->inode || !dir->inode->data || !name)
    return -EINVAL;

  struct p9_inode_info *info = (struct p9_inode_info *)dir->inode->data;
  return p9_proto_unlinkat(info->p9dev, info->fid, name, P9_DOTL_AT_REMOVEDIR);
}

static void p9_vfs_getattr(struct vfs_node *node) {
  if (!node || !node->inode || !node->inode->data)
    return;

  struct p9_inode_info *info = (struct p9_inode_info *)node->inode->data;
  struct p9_rgetattr ga;
  if (p9_proto_getattr(info->p9dev, info->fid, P9_GETATTR_ALL, &ga) == 0) {
    node->inode->size = (usize)ga.size;
    node->inode->uid = (u16)ga.uid;
    node->inode->gid = (u16)ga.gid;
    node->inode->mode = (u16)ga.mode;
    node->inode->nlink = (int)ga.nlink;
    node->inode->atime = ga.atime_sec;
    node->inode->atime_nsec = (u32)ga.atime_nsec;
    node->inode->mtime = ga.mtime_sec;
    node->inode->mtime_nsec = (u32)ga.mtime_nsec;
    node->inode->ctime = ga.ctime_sec;
    node->inode->ctime_nsec = (u32)ga.ctime_nsec;
  }
}

static int p9_vfs_setattr(struct vfs_node *node) {
  if (!node || !node->inode || !node->inode->data)
    return -EINVAL;

  struct p9_inode_info *info = (struct p9_inode_info *)node->inode->data;
  u32 valid = P9_SETATTR_MODE | P9_SETATTR_UID | P9_SETATTR_GID |
              P9_SETATTR_SIZE | P9_SETATTR_ATIME | P9_SETATTR_MTIME;

  return p9_proto_setattr(info->p9dev, info->fid, valid,
                          node->inode->mode, node->inode->uid,
                          node->inode->gid, node->inode->size,
                          node->inode->atime, node->inode->atime_nsec,
                          node->inode->mtime, node->inode->mtime_nsec);
}

static int p9_vfs_statfs(struct vfs_node *node, struct b1nix_statfs *st) {
  if (!st)
    return -EINVAL;

  (void)node;
  memset(st, 0, sizeof(*st));
  st->f_type = 0x01021997; /* V9FS_MAGIC */
  st->f_bsize = 4096;
  st->f_blocks = 1048576;
  st->f_bfree = 524288;
  st->f_bavail = 524288;
  st->f_namelen = 255;
  return 0;
}

static int p9_vfs_fsync(struct vfs_node *node) {
  if (!node || !node->inode || !node->inode->data)
    return -EINVAL;

  struct p9_inode_info *info = (struct p9_inode_info *)node->inode->data;
  return p9_proto_fsync(info->p9dev, info->fid);
}

static void p9_vfs_release(struct vfs_node *node) {
  if (!node || !node->inode || !node->inode->data)
    return;

  struct p9_inode_info *info = (struct p9_inode_info *)node->inode->data;
  p9_proto_clunk(info->p9dev, info->fid);
}

/* Mount entrypoint */

static struct vfs_node *p9_mount_cb(const char *source, u64 flags, void *data) {
  (void)flags;
  (void)data;

  struct virtio_9p_dev *p9dev = virtio_9p_find_by_tag(source);
  if (!p9dev) {
    k_warn("virtio-9p", "device not found");
    return ERR_PTR(-ENODEV);
  }

  int err = p9_proto_version(p9dev);
  if (err < 0) {
    k_warn("virtio-9p", "failed TVERSION handshake");
    return ERR_PTR(err);
  }

  u32 root_fid = 1;
  struct p9_qid root_qid;
  err = p9_proto_attach(p9dev, root_fid, &root_qid);
  if (err < 0) {
    k_warn("virtio-9p", "failed TATTACH");
    return ERR_PTR(err);
  }

  struct p9_rgetattr ga;
  err = p9_proto_getattr(p9dev, root_fid, P9_GETATTR_BASIC, &ga);
  if (err < 0) {
    ga.mode = 0755 | B1NIX_S_IFDIR;
    ga.size = 0;
  }

  struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
  if (!root) {
    p9_proto_clunk(p9dev, root_fid);
    return ERR_PTR(-ENOMEM);
  }

  p9_setup_node_ops(root, p9dev, root_fid, &root_qid, ga.mode, ga.size);
  k_info("virtio-9p", "mounted tag '%s' successfully", p9dev->tag);
  return root;
}

static struct vfs_fs p9_fs = {
    .name = "9p",
    .mount = p9_mount_cb,
    .flags = VFS_FS_NODEV,
};

void p9_fs_init(void) {
  vfs_register_fs(&p9_fs);
}
