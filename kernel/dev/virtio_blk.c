#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <b1nix/sched.h>
#include <b1nix/types.h>
#include <b1nix/virtio.h>

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_BLK_DEVICE_ID 0x1001

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1

/* Support multiple virtio-blk devices */
#define MAX_VIRTIO_BLK 8

struct virtio_blk_instance {
  struct virtio_device dev;
  struct virtqueue vq;
  struct block_device blk;
};

static struct virtio_blk_instance instances[MAX_VIRTIO_BLK];
static int instance_count = 0;

struct virtio_blk_req {
  u32 type;
  u32 reserved;
  u64 sector;
} __attribute__((packed));

static volatile u8 virtio_blk_status;

static int do_virtio_blk_req(struct virtio_blk_instance *inst, u64 lba,
                             u32 count, void *buffer, u32 type) {
  struct virtio_blk_req *req = kzalloc(sizeof(struct virtio_blk_req));
  if (!req)
    return -1;

  req->type = type;
  req->reserved = 0;
  req->sector = lba;

  virtio_blk_status = 0xFF;

  // Set up descriptors
  u16 d0 = 0;
  u16 d1 = 1;
  u16 d2 = 2;

  inst->vq.desc[d0].addr = vmm_virt_to_phys(req);
  inst->vq.desc[d0].len = sizeof(struct virtio_blk_req);
  inst->vq.desc[d0].flags = VRING_DESC_F_NEXT;
  inst->vq.desc[d0].next = d1;

  inst->vq.desc[d1].addr = vmm_virt_to_phys(buffer);
  inst->vq.desc[d1].len = count * 512;
  inst->vq.desc[d1].flags =
      VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
  inst->vq.desc[d1].next = d2;

  inst->vq.desc[d2].addr = vmm_virt_to_phys((void *)&virtio_blk_status);
  inst->vq.desc[d2].len = 1;
  inst->vq.desc[d2].flags = VRING_DESC_F_WRITE;
  inst->vq.desc[d2].next = 0;

  u16 avail_idx = inst->vq.avail->idx % inst->vq.queue_size;
  inst->vq.avail->ring[avail_idx] = d0;

  // Full memory barrier is usually needed here, but since it's a simple hobby
  // OS on single core, we just enforce compiler barrier
  __asm__ volatile("" ::: "memory");

  inst->vq.avail->idx++;

  __asm__ volatile("" ::: "memory");

  virtq_kick(&inst->dev, &inst->vq);

  while (inst->vq.used->idx == inst->vq.last_used_idx) {
    scheduler_yield();
  }

  inst->vq.last_used_idx++;

  int ret = (virtio_blk_status == 0) ? (int)count : -1;
  // Memory leaks here in a real OS since bump allocator cannot free, but fine
  // for now
  return ret;
}

static int virtio_blk_read(struct block_device *dev, u64 lba, u32 count,
                           void *buffer) {
  struct virtio_blk_instance *inst = (struct virtio_blk_instance *)dev->priv;
  return do_virtio_blk_req(inst, lba, count, buffer, VIRTIO_BLK_T_IN);
}

static int virtio_blk_write(struct block_device *dev, u64 lba, u32 count,
                            const void *buffer) {
  struct virtio_blk_instance *inst = (struct virtio_blk_instance *)dev->priv;
  return do_virtio_blk_req(inst, lba, count, (void *)(usize)buffer,
                           VIRTIO_BLK_T_OUT);
}

/* Find nth virtio-blk device on PCI bus */
static int virtio_find_nth_device(int n, struct pci_device_info *info) {
  int found = 0;
  for (u16 bus = 0; bus < 256 && found <= n; bus++) {
    for (u8 slot = 0; slot < 32 && found <= n; slot++) {
      u16 vendor = pci_config_read16((u8)bus, slot, 0, 0);
      if (vendor == 0xFFFF)
        continue;

      u8 header_type = pci_config_read8((u8)bus, slot, 0, 0x0E);
      u8 max_func = (header_type & 0x80) ? 8 : 1;

      for (u8 func = 0; func < max_func; func++) {
        vendor = pci_config_read16((u8)bus, slot, func, 0);
        if (vendor == 0xFFFF)
          continue;

        u16 dev = pci_config_read16((u8)bus, slot, func, 2);
        if (vendor == VIRTIO_VENDOR_ID && dev == VIRTIO_BLK_DEVICE_ID) {
          if (found == n) {
            if (info) {
              info->bus = (u8)bus;
              info->slot = slot;
              info->func = func;
              info->vendor_id = vendor;
              info->device_id = dev;
              info->class_code = pci_config_read8((u8)bus, slot, func, 0x0B);
              info->subclass = pci_config_read8((u8)bus, slot, func, 0x0A);
              info->prog_if = pci_config_read8((u8)bus, slot, func, 0x09);
            }
            return 1;
          }
          found++;
        }
      }
    }
  }
  return 0;
}

void virtio_blk_init(void) {
  instance_count = 0;
  int dev_idx = 0;

  while (instance_count < MAX_VIRTIO_BLK) {
    struct pci_device_info pci_info;
    if (!virtio_find_nth_device(dev_idx, &pci_info))
      break;

    struct virtio_blk_instance *inst = &instances[instance_count];

    // Read BAR0 to get I/O port base (legacy virtio)
    u32 bar0 =
        pci_config_read32(pci_info.bus, pci_info.slot, pci_info.func, 0x10);
    if ((bar0 & 1) == 0) {
      dev_idx++;
      continue;
    }

    inst->dev.port_base = (u16)(bar0 & ~3);
    inst->dev.irq =
        pci_config_read8(pci_info.bus, pci_info.slot, pci_info.func, 0x3C);

    // Reset device
    virtio_set_status(&inst->dev, 0);
    virtio_set_status(&inst->dev,
                      VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    virtio_set_guest_features(&inst->dev, 0);
    virtio_set_status(&inst->dev, VIRTIO_STATUS_ACKNOWLEDGE |
                                      VIRTIO_STATUS_DRIVER |
                                      VIRTIO_STATUS_FEATURES_OK);

    if (!virtq_init(&inst->dev, 0, &inst->vq)) {
      console_write("virtio-blk: failed to initialize virtqueue for device ");
      console_write_dec(dev_idx);
      console_write("\n");
      dev_idx++;
      continue;
    }

    virtio_set_status(&inst->dev,
                      virtio_get_status(&inst->dev) | VIRTIO_STATUS_DRIVER_OK);

    /* Build device name: virtio-blk0, virtio-blk1, ... */
    char *name = kmalloc(16);
    name[0] = 'v';
    name[1] = 'i';
    name[2] = 'r';
    name[3] = 't';
    name[4] = 'i';
    name[5] = 'o';
    name[6] = '-';
    name[7] = 'b';
    name[8] = 'l';
    name[9] = 'k';
    name[10] = '0' + (char)instance_count;
    name[11] = '\0';

    inst->blk.name = name;
    inst->blk.block_size = 512;
    inst->blk.block_count = 0;
    inst->blk.read_blocks = virtio_blk_read;
    inst->blk.write_blocks = virtio_blk_write;
    inst->blk.priv = inst;
    blk_register(&inst->blk);

    console_write("virtio-blk: registered ");
    console_write(name);
    console_write(" (PCI ");
    console_write_dec(pci_info.bus);
    console_write(":");
    console_write_dec(pci_info.slot);
    console_write(".");
    console_write_dec(pci_info.func);
    console_write(")\n");

    instance_count++;
    dev_idx++;
  }

  if (instance_count == 0) {
    console_write("virtio-blk: no device found\n");
  } else {
    console_write("virtio-blk: initialized ");
    console_write_dec(instance_count);
    console_write(" devices\n");
  }
}
