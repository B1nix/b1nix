#include <b1nix/console.h>
#include <b1nix/pci.h>
#include <b1nix/virtio.h>

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_GPU_DEVICE_ID_LEGACY 0x1010
#define VIRTIO_GPU_DEVICE_ID_MODERN 0x1050

static struct virtio_device gpu_dev;
static struct virtqueue controlq;
static struct virtqueue cursorq;

void virtio_gpu_init(void)
{
	struct pci_device_info pci;
	u16 device_id = 0;

	if (pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_GPU_DEVICE_ID_LEGACY, &pci)) {
		device_id = VIRTIO_GPU_DEVICE_ID_LEGACY;
	} else if (pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_GPU_DEVICE_ID_MODERN, &pci)) {
		device_id = VIRTIO_GPU_DEVICE_ID_MODERN;
	} else {
		console_write("virtio-gpu: device not found\n");
		return;
	}

	console_write("virtio-gpu: pci ");
	console_write_hex32(pci.bus);
	console_write(":");
	console_write_hex32(pci.slot);
	console_write(".");
	console_write_hex32(pci.func);
	console_write(" device 0x");
	console_write_hex32(device_id);
	console_write("\n");

	if (!virtio_init_device(&gpu_dev, VIRTIO_VENDOR_ID, device_id)) {
		console_write("virtio-gpu: transport init failed\n");
		return;
	}

	u32 features = virtio_get_host_features(&gpu_dev);
	virtio_set_guest_features(&gpu_dev, features & 0);

	virtio_set_status(&gpu_dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
	if ((virtio_get_status(&gpu_dev) & VIRTIO_STATUS_FEATURES_OK) == 0) {
		console_write("virtio-gpu: features not accepted\n");
		virtio_set_status(&gpu_dev, VIRTIO_STATUS_FAILED);
		return;
	}

	if (!virtq_init(&gpu_dev, 0, &controlq)) {
		console_write("virtio-gpu: controlq init failed\n");
		virtio_set_status(&gpu_dev, VIRTIO_STATUS_FAILED);
		return;
	}

	if (!virtq_init(&gpu_dev, 1, &cursorq)) {
		console_write("virtio-gpu: cursorq init failed\n");
		virtio_set_status(&gpu_dev, VIRTIO_STATUS_FAILED);
		return;
	}

	virtio_set_status(&gpu_dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
	                            VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
	console_write("virtio-gpu: queues initialized (controlq/cursorq)\n");
}
