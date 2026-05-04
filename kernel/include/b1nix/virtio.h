#ifndef B1NIX_VIRTIO_H
#define B1NIX_VIRTIO_H

#include <b1nix/types.h>

#define VIRTIO_PCI_HOST_FEATURES  0x00
#define VIRTIO_PCI_GUEST_FEATURES 0x04
#define VIRTIO_PCI_QUEUE_PFN      0x08
#define VIRTIO_PCI_QUEUE_SIZE     0x0C
#define VIRTIO_PCI_QUEUE_SEL      0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY   0x10
#define VIRTIO_PCI_STATUS         0x12
#define VIRTIO_PCI_ISR            0x13

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

// Virtqueue structures (Legacy, split virtqueues)
struct vring_desc {
	u64 addr;
	u32 len;
	u16 flags;
	u16 next;
} __attribute__((packed));

struct vring_avail {
	u16 flags;
	u16 idx;
	u16 ring[];
} __attribute__((packed));

struct vring_used_elem {
	u32 id;
	u32 len;
} __attribute__((packed));

struct vring_used {
	u16 flags;
	u16 idx;
	struct vring_used_elem ring[];
} __attribute__((packed));

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

struct virtqueue {
	u16 queue_idx;
	u16 queue_size;
	u32 pfn;
	u16 last_used_idx;
	
	struct vring_desc *desc;
	struct vring_avail *avail;
	struct vring_used *used;
};

struct virtio_device {
	u16 port_base;
	u16 irq;
};

int virtio_init_device(struct virtio_device *dev, u16 vendor, u16 device);
void virtio_set_status(struct virtio_device *dev, u8 status);
u8 virtio_get_status(struct virtio_device *dev);
u32 virtio_get_host_features(struct virtio_device *dev);
void virtio_set_guest_features(struct virtio_device *dev, u32 features);
int virtq_init(struct virtio_device *dev, u16 queue_idx, struct virtqueue *vq);
void virtq_kick(struct virtio_device *dev, struct virtqueue *vq);

#endif
