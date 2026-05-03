#include <tinyunix/console.h>

void virtio_blk_init(void)
{
	console_write("virtio-blk: no pci/virtio bus yet, registered stub device\n");
}
