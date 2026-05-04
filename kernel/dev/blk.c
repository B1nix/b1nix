#include <b1nix/blk.h>
#include <string.h>

#define MAX_BLK_DEVICES 8

static struct block_device *blk_devices[MAX_BLK_DEVICES];
static usize blk_device_count = 0;

void blk_register(struct block_device *dev)
{
	if (blk_device_count < MAX_BLK_DEVICES) {
		blk_devices[blk_device_count++] = dev;
	}
}

struct block_device *blk_get(const char *name)
{
	for (usize i = 0; i < blk_device_count; i++) {
		if (strcmp(blk_devices[i]->name, name) == 0) {
			return blk_devices[i];
		}
	}
	return 0;
}
