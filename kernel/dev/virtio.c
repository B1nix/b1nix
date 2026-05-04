#include <b1nix/virtio.h>
#include <b1nix/pci.h>
#include <b1nix/io.h>
#include <b1nix/mm.h>
#include <b1nix/console.h>
#include <string.h>

int virtio_init_device(struct virtio_device *dev, u16 vendor, u16 device)
{
	struct pci_device_info pci_info;
	if (!pci_find_device(vendor, device, &pci_info)) {
		return 0; // Not found
	}

	// Read BAR0 to get I/O port base (legacy virtio)
	u32 bar0 = pci_config_read32(pci_info.bus, pci_info.slot, pci_info.func, 0x10);
	if ((bar0 & 1) == 0) {
		console_write("virtio: BAR0 is not I/O space\n");
		return 0;
	}

	dev->port_base = (u16)(bar0 & ~3);
	dev->irq = pci_config_read8(pci_info.bus, pci_info.slot, pci_info.func, 0x3C);

	// Reset device
	virtio_set_status(dev, 0);

	// Acknowledge and Driver
	virtio_set_status(dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

	return 1;
}

void virtio_set_status(struct virtio_device *dev, u8 status)
{
	outb((u16)(dev->port_base + VIRTIO_PCI_STATUS), status);
}

u8 virtio_get_status(struct virtio_device *dev)
{
	return inb((u16)(dev->port_base + VIRTIO_PCI_STATUS));
}

u32 virtio_get_host_features(struct virtio_device *dev)
{
	return inl((u16)(dev->port_base + VIRTIO_PCI_HOST_FEATURES));
}

void virtio_set_guest_features(struct virtio_device *dev, u32 features)
{
	outl((u16)(dev->port_base + VIRTIO_PCI_GUEST_FEATURES), features);
}

int virtq_init(struct virtio_device *dev, u16 queue_idx, struct virtqueue *vq)
{
	// Select queue
	outw((u16)(dev->port_base + VIRTIO_PCI_QUEUE_SEL), queue_idx);

	u16 qsize = inw((u16)(dev->port_base + VIRTIO_PCI_QUEUE_SIZE));
	if (qsize == 0) {
		console_write("virtq: queue size is 0\n");
		return 0;
	}

	vq->queue_idx = queue_idx;
	vq->queue_size = qsize;
	vq->last_used_idx = 0;

	// Calculate memory requirements
	// Descriptor table: 16 * qsize
	// Available ring: 6 + 2 * qsize
	// Used ring: 4 + 8 * qsize
	usize desc_size = 16 * qsize;
	usize avail_size = 6 + 2 * qsize;
	
	// Page align
	usize offset = (desc_size + avail_size + 4095) & ~4095;
	usize used_size = 4 + 8 * qsize;
	usize total_size = offset + ((used_size + 4095) & ~4095);

	// Allocate frames
	usize frames = total_size / 4096;
	u64 paddr = pmm_alloc_frames(frames);
	if (!paddr) {
		console_write("virtq: failed to allocate memory\n");
		return 0;
	}

	memset((void*)paddr, 0, total_size);

	vq->pfn = (u32)(paddr / 4096);
	vq->desc = (struct vring_desc *)paddr;
	vq->avail = (struct vring_avail *)(paddr + desc_size);
	vq->used = (struct vring_used *)(paddr + offset);

	outl((u16)(dev->port_base + VIRTIO_PCI_QUEUE_PFN), vq->pfn);

	return 1;
}

void virtq_kick(struct virtio_device *dev, struct virtqueue *vq)
{
	outw((u16)(dev->port_base + VIRTIO_PCI_QUEUE_NOTIFY), vq->queue_idx);
}
