/*
 * Network block device — a remote export used as a local disk.
 *
 * The protocol is NBD's oldstyle handshake plus the simple request format,
 * which is what nbd-server speaks by default and what a plain `nbd-client
 * host port /dev/nbd0` sets up:
 *
 *   handshake: "NBDMAGIC" + 0x00420281861253 + u64 size + u32 flags + 124 pad
 *   request:   0x25609513 u32 magic, u32 type, u64 handle, u64 offset, u32 len
 *   reply:     0x67446698 u32 magic, u32 error, u64 handle [+ data for READ]
 *
 * Everything is big-endian on the wire.
 *
 * One request is in flight at a time and the caller waits for its reply. That
 * is slower than NBD allows and it is deliberate: the block layer above calls
 * read_blocks/write_blocks synchronously, so a queue of outstanding handles
 * would buy nothing until there is an asynchronous path to feed it.
 */

#include <b1nix/blk.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/nbd.h>
#include <b1nix/net.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/vfs.h>
#include <string.h>

#define NBD_MAX_DEVICES 4

#define NBD_INIT_MAGIC   0x4e42444d41474943ULL /* "NBDMAGIC" */
#define NBD_OLD_MAGIC    0x00420281861253ULL
#define NBD_REQUEST_MAGIC 0x25609513u
#define NBD_REPLY_MAGIC   0x67446698u

#define NBD_CMD_READ  0
#define NBD_CMD_WRITE 1
#define NBD_CMD_DISC  2

/*
 * Two ways in, one protocol.
 *
 * `sock` is the descriptor nbd-client connected and handed over through
 * NBD_SET_SOCK -- the Linux interface, and the one the applet speaks.
 * `conn` is a connection this kernel made itself, for nbd_attach(). Exactly
 * one of them is set, and every transfer goes through nbd_io_* below so the
 * request code does not care which.
 */
struct nbd_device {
	struct block_device dev;
	struct tcp_conn *conn;
	struct vfs_handle *sock;
	u64 handle_seq;
	spinlock_t lock;
	int used;
	int registered;
	volatile int serving;  /* NBD_DO_IT is parked in this device */
};

static struct nbd_device g_nbd[NBD_MAX_DEVICES];

static u64 be64(u64 v)
{
	return ((v & 0xffULL) << 56) | ((v & 0xff00ULL) << 40) |
	       ((v & 0xff0000ULL) << 24) | ((v & 0xff000000ULL) << 8) |
	       ((v >> 8) & 0xff000000ULL) | ((v >> 24) & 0xff0000ULL) |
	       ((v >> 40) & 0xff00ULL) | ((v >> 56) & 0xffULL);
}

static u32 be32(u32 v)
{
	return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) |
	       ((v >> 8) & 0xff00u) | ((v >> 24) & 0xffu);
}

/* Read exactly `len` bytes or fail: a short read on a block device is not a
 * partial answer, it is a wrong one. */
static int nbd_io_recv(struct nbd_device *nd, void *buf, usize len)
{
	if (nd->sock)
		return (int)vfs_handle_read(nd->sock, buf, len);
	return tcp_recv(nd->conn, buf, len, 0);
}

static int nbd_io_send(struct nbd_device *nd, const void *buf, usize len)
{
	if (nd->sock)
		return (int)vfs_handle_write(nd->sock, buf, len);
	return tcp_send(nd->conn, buf, len);
}

static int nbd_io_closed(struct nbd_device *nd)
{
	/* A handed-over socket reports its own end of stream through a read
	 * returning 0, which the caller sees; only our own connection has a
	 * separate "is it closed" question to ask. */
	return nd->sock ? 0 : tcp_is_closed(nd->conn);
}

static int nbd_recv_all(struct nbd_device *nd, void *buf, usize len)
{
	u8 *p = buf;
	usize got = 0;
	int idle = 0;

	while (got < len) {
		int n = nbd_io_recv(nd, p + got, len - got);

		if (n > 0) {
			got += (usize)n;
			idle = 0;
			continue;
		}
		if (n == 0 || nbd_io_closed(nd))
			return -1;
		if (++idle > 500) /* 5 s with nothing arriving */
			return -1;
		scheduler_sleep_ticks(SCHED_MS_TO_TICKS(10));
	}
	return 0;
}

static int nbd_send_all(struct nbd_device *nd, const void *buf, usize len)
{
	const u8 *p = buf;
	usize sent = 0;

	while (sent < len) {
		int n = nbd_io_send(nd, p + sent, len - sent);

		if (n > 0) {
			sent += (usize)n;
			continue;
		}
		if (n == 0 || nbd_io_closed(nd))
			return -1;
		scheduler_sleep_ticks(SCHED_MS_TO_TICKS(10));
	}
	return 0;
}

static int nbd_request(struct nbd_device *nd, u32 type, u64 offset, u32 len,
		       void *rbuf, const void *wbuf)
{
	struct {
		u32 magic;
		u32 type;
		u64 handle;
		u64 offset;
		u32 len;
	} __attribute__((packed)) req;
	struct {
		u32 magic;
		u32 error;
		u64 handle;
	} __attribute__((packed)) rep;
	u64 flags;
	int rc = -1;

	spin_lock_irqsave(&nd->lock, &flags);
	u64 handle = ++nd->handle_seq;
	spin_unlock_irqrestore(&nd->lock, flags);

	req.magic = be32(NBD_REQUEST_MAGIC);
	req.type = be32(type);
	req.handle = be64(handle);
	req.offset = be64(offset);
	req.len = be32(len);

	if (nbd_send_all(nd, &req, sizeof(req)) != 0)
		return -1;
	if (type == NBD_CMD_WRITE && nbd_send_all(nd, wbuf, len) != 0)
		return -1;
	if (nbd_recv_all(nd, &rep, sizeof(rep)) != 0)
		return -1;
	if (be32(rep.magic) != NBD_REPLY_MAGIC)
		return -1;
	if (be32(rep.error) != 0)
		return -1;
	/* The handle comes back so a pipelined client can match replies to
	 * requests. Only one is outstanding here, but checking it costs nothing
	 * and catches a stream that has lost sync. */
	if (be64(rep.handle) != handle)
		return -1;
	if (type == NBD_CMD_READ && nbd_recv_all(nd, rbuf, len) != 0)
		return -1;
	rc = 0;
	return rc;
}

static int nbd_read_blocks(struct block_device *dev, u64 lba, u32 count,
			   void *buffer)
{
	struct nbd_device *nd = (struct nbd_device *)dev;

	return nbd_request(nd, NBD_CMD_READ, lba * dev->block_size,
			   count * (u32)dev->block_size, buffer, 0);
}

static int nbd_write_blocks(struct block_device *dev, u64 lba, u32 count,
			    const void *buffer)
{
	struct nbd_device *nd = (struct nbd_device *)dev;

	return nbd_request(nd, NBD_CMD_WRITE, lba * dev->block_size,
			   count * (u32)dev->block_size, 0, buffer);
}

int nbd_attach(struct ipv4_addr server, u16 port)
{
	struct nbd_device *nd = 0;

	for (int i = 0; i < NBD_MAX_DEVICES; i++) {
		if (!g_nbd[i].used) {
			nd = &g_nbd[i];
			break;
		}
	}
	if (!nd)
		return -ENOSPC;

	memset(nd, 0, sizeof(*nd));
	nd->conn = tcp_connect(server, port);
	if (!nd->conn)
		return -EHOSTUNREACH;

	/* Oldstyle handshake: the server speaks first and tells us the size. */
	struct {
		u64 magic;
		u64 opts;
		u64 size;
		u32 flags;
		u8 pad[124];
	} __attribute__((packed)) hs;

	if (nbd_recv_all(nd, &hs, sizeof(hs)) != 0) {
		tcp_close(nd->conn);
		return -EIO;
	}
	if (be64(hs.magic) != NBD_INIT_MAGIC || be64(hs.opts) != NBD_OLD_MAGIC) {
		klog_warn("nbd: server did not speak the oldstyle handshake");
		tcp_close(nd->conn);
		return -EIO;
	}

	u64 bytes = be64(hs.size);
	if (bytes < 512) {
		tcp_close(nd->conn);
		return -EINVAL;
	}

	nd->used = 1;
	nd->dev.block_size = 512;
	nd->dev.block_count = bytes / 512;
	nd->dev.read_blocks = nbd_read_blocks;
	nd->dev.write_blocks = nbd_write_blocks;
	{
		char *nm = kmalloc(8);

		if (!nm) {
			tcp_close(nd->conn);
			nd->used = 0;
			return -ENOMEM;
		}
		nm[0] = 'n'; nm[1] = 'b'; nm[2] = 'd';
		nm[3] = (char)('0' + (int)(nd - g_nbd));
		nm[4] = '\0';
		nd->dev.name = nm;
		nd->dev.bus = BLK_BUS_NBD;
		blk_register(&nd->dev);
	}
	return 0;
}

int nbd_detach(const char *name)
{
	for (int i = 0; i < NBD_MAX_DEVICES; i++) {
		struct nbd_device *nd = &g_nbd[i];

		if (!nd->used || !nd->dev.name || strcmp(nd->dev.name, name) != 0)
			continue;
		/* Tell the server we are done, so it can close its file rather
		 * than wait for the connection to time out. */
		nbd_request(nd, NBD_CMD_DISC, 0, 0, 0, 0);
		tcp_close(nd->conn);
		blk_unregister(&nd->dev);
		nd->used = 0;
		return 0;
	}
	return -ENODEV;
}

/*
 * The Linux nbd ioctl interface, which is what BusyBox's nbd-client speaks.
 *
 * The client connects, completes the handshake itself, tells the kernel the
 * geometry it learned, hands over the socket and then parks in NBD_DO_IT until
 * the device is disconnected. From that moment the block layer's reads and
 * writes travel over that socket.
 *
 * Requests are issued by whichever task is doing the I/O rather than by the
 * parked thread: with one request outstanding at a time there is nothing for a
 * separate worker to overlap, and a queue that cannot be deeper than one is a
 * queue in name only.
 */
struct nbd_device *nbd_device_at(unsigned index)
{
	if (index >= NBD_MAX_DEVICES)
		return 0;
	return &g_nbd[index];
}

int nbd_set_socket(struct nbd_device *nd, struct vfs_handle *sock)
{
	u64 flags;

	if (!nd || !sock)
		return -EINVAL;
	spin_lock_irqsave(&nd->lock, &flags);
	if (nd->sock || nd->conn) {
		spin_unlock_irqrestore(&nd->lock, flags);
		return -EBUSY;
	}
	nd->sock = sock;
	nd->used = 1;
	spin_unlock_irqrestore(&nd->lock, flags);
	return 0;
}

int nbd_set_geometry(struct nbd_device *nd, u32 block_size, u64 blocks)
{
	if (!nd || block_size < 512 || blocks == 0)
		return -EINVAL;
	nd->dev.block_size = block_size;
	nd->dev.block_count = blocks;
	return 0;
}

int nbd_run(struct nbd_device *nd)
{
	if (!nd || !nd->sock)
		return -EINVAL;
	if (nd->dev.block_count == 0)
		return -EINVAL;

	/* Park until NBD_DISCONNECT or NBD_CLEAR_SOCK. Sleeping in the caller is
	 * what Linux does too: nbd-client's main thread lives here for the life
	 * of the device. */
	nd->serving = 1;
	while (nd->serving && nd->sock)
		scheduler_sleep_ticks(SCHED_MS_TO_TICKS(100));
	return 0;
}

int nbd_clear_socket(struct nbd_device *nd)
{
	struct vfs_handle *sock;
	u64 flags;

	if (!nd)
		return -EINVAL;
	spin_lock_irqsave(&nd->lock, &flags);
	sock = nd->sock;
	nd->sock = 0;
	nd->serving = 0;
	spin_unlock_irqrestore(&nd->lock, flags);
	if (sock)
		vfs_handle_release(sock);
	/* The device node stays: Linux keeps /dev/nbdN present and empty when
	 * nothing is attached, and a client that reconnects expects to find it. */
	nd->dev.block_count = 0;
	nd->used = 0;
	return 0;
}

int nbd_disconnect(struct nbd_device *nd)
{
	if (!nd || !nd->sock)
		return -EINVAL;
	/* Ask the server to close its file first; then tear the device down. */
	nbd_request(nd, NBD_CMD_DISC, 0, 0, 0, 0);
	return nbd_clear_socket(nd);
}

/* Geometry read back, so the ioctls can set one half without losing the other:
 * NBD_SET_BLKSIZE and NBD_SET_SIZE_BLOCKS arrive as separate calls. */
u32 nbd_block_size(struct nbd_device *nd)
{
	return nd ? (u32)nd->dev.block_size : 0;
}

u64 nbd_block_count(struct nbd_device *nd)
{
	return nd ? nd->dev.block_count : 0;
}

/*
 * Publish the devices at boot, empty.
 *
 * nbd-client opens /dev/nbd0 and only then tells the kernel what is behind it,
 * so the node has to exist before anything is attached -- exactly as on Linux,
 * where nbd0..15 are present and zero-sized until used. A read of an empty one
 * fails, which is the truth about it.
 */
void nbd_init(void)
{
	for (int i = 0; i < NBD_MAX_DEVICES; i++) {
		struct nbd_device *nd = &g_nbd[i];

		memset(nd, 0, sizeof(*nd));
		nd->dev.block_size = 512;
		nd->dev.block_count = 0;
		nd->dev.read_blocks = nbd_read_blocks;
		nd->dev.write_blocks = nbd_write_blocks;
		/* nbd0, nbd1, ... -- named here rather than through
		 * blk_register_disk, whose sequence is the sd-style letter one
		 * (that produced "nbda", which nothing looks for). */
		char *nm = kmalloc(8);
		if (!nm)
			continue;
		nm[0] = 'n'; nm[1] = 'b'; nm[2] = 'd';
		nm[3] = (char)('0' + i);
		nm[4] = '\0';
		nd->dev.name = nm;
		nd->dev.bus = BLK_BUS_NBD;
		blk_register(&nd->dev);
		nd->registered = 1;
	}
}
