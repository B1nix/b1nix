#include <b1nix/console.h>
#include <b1nix/virtio.h>
#include <b1nix/blk.h>
#include <b1nix/mm.h>
#include <b1nix/types.h>

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_BLK_DEVICE_ID 0x1001

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1

static struct virtio_device blk_dev;
static struct virtqueue blk_vq;
static struct block_device virtio_blk_device;

struct virtio_blk_req {
	u32 type;
	u32 reserved;
	u64 sector;
} __attribute__((packed));

static volatile u8 virtio_blk_status;

static int do_virtio_blk_req(u64 lba, u32 count, void *buffer, u32 type)
{
	struct virtio_blk_req *req = kzalloc(sizeof(struct virtio_blk_req));
	if (!req) return -1;
	
	req->type = type;
	req->reserved = 0;
	req->sector = lba;

	virtio_blk_status = 0xFF;

	// Set up descriptors
	u16 d0 = 0;
	u16 d1 = 1;
	u16 d2 = 2;

	blk_vq.desc[d0].addr = (u64)(usize)req;
	blk_vq.desc[d0].len = sizeof(struct virtio_blk_req);
	blk_vq.desc[d0].flags = VRING_DESC_F_NEXT;
	blk_vq.desc[d0].next = d1;

	blk_vq.desc[d1].addr = (u64)(usize)buffer;
	blk_vq.desc[d1].len = count * 512;
	blk_vq.desc[d1].flags = VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
	blk_vq.desc[d1].next = d2;

	blk_vq.desc[d2].addr = (u64)(usize)&virtio_blk_status;
	blk_vq.desc[d2].len = 1;
	blk_vq.desc[d2].flags = VRING_DESC_F_WRITE;
	blk_vq.desc[d2].next = 0;

	u16 avail_idx = blk_vq.avail->idx % blk_vq.queue_size;
	blk_vq.avail->ring[avail_idx] = d0;

	// Full memory barrier is usually needed here, but since it's a simple hobby OS on single core, we just enforce compiler barrier
	__asm__ volatile("" ::: "memory");
	
	blk_vq.avail->idx++;
	
	__asm__ volatile("" ::: "memory");

	virtq_kick(&blk_dev, &blk_vq);

	while (blk_vq.used->idx == blk_vq.last_used_idx) {
		__asm__ volatile("pause" ::: "memory");
	}

	blk_vq.last_used_idx++;

	int ret = (virtio_blk_status == 0) ? (int)count : -1;
	// Memory leaks here in a real OS since bump allocator cannot free, but fine for now
	return ret;
}

static int virtio_blk_read(struct block_device *dev, u64 lba, u32 count, void *buffer)
{
	(void)dev;
	return do_virtio_blk_req(lba, count, buffer, VIRTIO_BLK_T_IN);
}

static int virtio_blk_write(struct block_device *dev, u64 lba, u32 count, const void *buffer)
{
	(void)dev;
	// Casting away const is required because of our do_virtio_blk_req signature, but it's safe since T_OUT won't mutate buffer
	return do_virtio_blk_req(lba, count, (void *)(usize)buffer, VIRTIO_BLK_T_OUT);
}

void virtio_blk_init(void)
{
	if (!virtio_init_device(&blk_dev, VIRTIO_VENDOR_ID, VIRTIO_BLK_DEVICE_ID)) {
		console_write("virtio-blk: no device found\n");
		return;
	}

	virtio_set_guest_features(&blk_dev, 0);
	virtio_set_status(&blk_dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

	if (!virtq_init(&blk_dev, 0, &blk_vq)) {
		console_write("virtio-blk: failed to initialize virtqueue\n");
		return;
	}

	virtio_set_status(&blk_dev, virtio_get_status(&blk_dev) | VIRTIO_STATUS_DRIVER_OK);

	virtio_blk_device.name = "virtio-blk0";
	virtio_blk_device.block_size = 512;
	virtio_blk_device.block_count = 0; // Proper capacity can be read from PCI config space
	virtio_blk_device.read_blocks = virtio_blk_read;
	virtio_blk_device.write_blocks = virtio_blk_write;
	blk_register(&virtio_blk_device);

	console_write("virtio-blk: initialized successfully\n");
}
