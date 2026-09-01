#include <b1nix/virtio_9p.h>
#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/io.h>
#include <b1nix/klog.h>
#include <b1nix/kprintf.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <b1nix/sched.h>
#include <string.h>

#define VIRTIO_VENDOR_ID 0x1AF4
#define MAX_VIRTIO_9P_DEVS 8

static struct virtio_9p_dev g_p9_devs[MAX_VIRTIO_9P_DEVS];
static int g_p9_dev_count = 0;

static void p9_lock(struct virtio_9p_dev *p9dev) {
  while (__sync_lock_test_and_set(&p9dev->busy, 1)) {
    scheduler_yield();
  }
  scheduler_kcrit_enter();
}

static void p9_unlock(struct virtio_9p_dev *p9dev) {
  __sync_lock_release(&p9dev->busy);
  scheduler_kcrit_leave();
}

struct virtio_9p_dev *virtio_9p_find_by_tag(const char *tag) {
  if (!tag || !tag[0]) {
    return g_p9_dev_count > 0 ? &g_p9_devs[0] : NULL;
  }
  for (int i = 0; i < g_p9_dev_count; i++) {
    if (strcmp(g_p9_devs[i].tag, tag) == 0) {
      return &g_p9_devs[i];
    }
  }
  return NULL;
}

int virtio_9p_transact(struct virtio_9p_dev *p9dev, usize req_len,
                       usize max_resp_len, usize *actual_resp_len) {
  if (!p9dev || req_len < sizeof(struct p9_header) ||
      max_resp_len < sizeof(struct p9_header)) {
    return -EINVAL;
  }

  p9_lock(p9dev);

  /* Update request header size */
  *(u32 *)p9dev->req_buf = (u32)req_len;

  struct virtqueue *vq = &p9dev->vq;
  u16 desc0 = 0;
  u16 desc1 = 1;

  vq->desc[desc0].addr = p9dev->req_buf_phys;
  vq->desc[desc0].len = (u32)req_len;
  vq->desc[desc0].flags = VRING_DESC_F_NEXT;
  vq->desc[desc0].next = desc1;

  vq->desc[desc1].addr = p9dev->resp_buf_phys;
  vq->desc[desc1].len = (u32)max_resp_len;
  vq->desc[desc1].flags = VRING_DESC_F_WRITE;
  vq->desc[desc1].next = 0;

  u16 avail_idx = vq->avail->idx;
  vq->avail->ring[avail_idx % vq->queue_size] = desc0;
  __sync_synchronize();
  vq->avail->idx = avail_idx + 1;
  __sync_synchronize();

  virtq_kick(&p9dev->dev, vq);

  u32 timeout = 50000000;
  while (vq->last_used_idx == vq->used->idx) {
    if (--timeout == 0) {
      k_warn("virtio-9p", "timeout waiting for response");
      p9_unlock(p9dev);
      return -EIO;
    }
    cpu_relax();
  }

  struct vring_used_elem *elem =
      &vq->used->ring[vq->last_used_idx % vq->queue_size];
  u32 bytes_written = elem->len;
  vq->last_used_idx++;

  if (actual_resp_len) {
    *actual_resp_len = bytes_written;
  }

  if (bytes_written < sizeof(struct p9_header)) {
    p9_unlock(p9dev);
    return -EIO;
  }

  u8 resp_type = p9dev->resp_buf[4];
  if (resp_type == P9_RLERROR) {
    u32 ecode = *(u32 *)(p9dev->resp_buf + 7);
    p9_unlock(p9dev);
    return -(int)ecode;
  }

  p9_unlock(p9dev);
  return 0;
}

static int init_one_9p_device(struct pci_device_info *pci) {
  if (g_p9_dev_count >= MAX_VIRTIO_9P_DEVS)
    return -1;

  struct virtio_9p_dev *p9dev = &g_p9_devs[g_p9_dev_count];
  memset(p9dev, 0, sizeof(*p9dev));

  u16 cmd = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
  cmd |= 0x0001; /* I/O space */
  cmd |= 0x0002; /* memory space */
  cmd |= 0x0004; /* bus master */
  /* This driver polls its virtqueue: it installs no interrupt handler and
   * never reads the ISR. So INTx must stay DISABLED -- clearing that bit was
   * arming a level-triggered line nothing would ever deassert.
   *
   * The line is shared (GIC 37 on QEMU virt, with virtio-gpu), so the effect
   * was not confined to this device: every 9P completion left the line
   * asserted, the CPU re-entered aarch64_irq_handler immediately, virtio-gpu's
   * handler read its own clean ISR and correctly answered "not mine", and
   * nothing acknowledged anything. Measured on the sys lane: ~155000
   * unclaimed interrupts a second on the boot CPU against a correct 100 Hz
   * timer tick on the other one, with handlers nested seven deep. The boot CPU
   * stopped making forward progress at the first 9P access and the lane died
   * on the harness stall timer with ~900 checks unrun. */
  cmd |= PCI_CMD_INTX_DISABLE;
  pci_config_write16(pci->bus, pci->slot, pci->func, 0x04, cmd);

  u32 bar0 = pci_config_read32(pci->bus, pci->slot, pci->func, 0x10);
  if ((bar0 & 1) == 0) {
    return -1;
  }

  p9dev->dev.port_base = (u16)(bar0 & ~3);
  p9dev->dev.irq = pci_intx_line(pci->bus, pci->slot, pci->func);

  virtio_set_status(&p9dev->dev, 0);
  virtio_set_status(&p9dev->dev,
                    VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

  /* Read mount tag from device config space at port_base + 0x14 */
  u16 tag_len = inw((u16)(p9dev->dev.port_base + 0x14));
  if (tag_len >= VIRTIO_9P_MAX_TAG_LEN)
    tag_len = VIRTIO_9P_MAX_TAG_LEN - 1;

  for (u16 i = 0; i < tag_len; i++) {
    p9dev->tag[i] = (char)inb((u16)(p9dev->dev.port_base + 0x16 + i));
  }
  p9dev->tag[tag_len] = '\0';

  if (!virtq_init(&p9dev->dev, 0, &p9dev->vq)) {
    k_warn("virtio-9p", "failed to init virtqueue 0");
    virtio_set_status(&p9dev->dev, VIRTIO_STATUS_FAILED);
    return -1;
  }

  p9dev->msize = VIRTIO_9P_DEFAULT_MSIZE;
  usize frames_needed = (p9dev->msize + 4095) / 4096;

  p9dev->req_buf_phys = pmm_alloc_frames(frames_needed);
  p9dev->resp_buf_phys = pmm_alloc_frames(frames_needed);

  if (!p9dev->req_buf_phys || !p9dev->resp_buf_phys) {
    k_warn("virtio-9p", "failed to allocate DMA buffers");
    virtio_set_status(&p9dev->dev, VIRTIO_STATUS_FAILED);
    return -1;
  }

  p9dev->req_buf = (u8 *)(usize)(p9dev->req_buf_phys + vmm_direct_map_base());
  p9dev->resp_buf = (u8 *)(usize)(p9dev->resp_buf_phys + vmm_direct_map_base());

  virtio_set_status(&p9dev->dev,
                    virtio_get_status(&p9dev->dev) | VIRTIO_STATUS_DRIVER_OK);

  k_info("virtio-9p", "probed device tag='%s' msize=%u port=0x%x",
         p9dev->tag, p9dev->msize, p9dev->dev.port_base);

  g_p9_dev_count++;
  return 0;
}

void virtio_9p_init(void) {
  for (u16 bus = 0; bus < 256; bus++) {
    for (u8 slot = 0; slot < 32; slot++) {
      u16 vendor = pci_config_read16((u8)bus, slot, 0, 0);
      if (vendor == 0xFFFF)
        continue;
      u8 header_type = pci_config_read8((u8)bus, slot, 0, 0x0E);
      u8 max_func = (header_type & 0x80) ? 8 : 1;
      for (u8 func = 0; func < max_func; func++) {
        vendor = pci_config_read16((u8)bus, slot, func, 0);
        if (vendor != VIRTIO_VENDOR_ID)
          continue;
        u16 dev = pci_config_read16((u8)bus, slot, func, 2);
        if (dev == VIRTIO_9P_DEVICE_ID_LEGACY ||
            dev == VIRTIO_9P_DEVICE_ID_MODERN) {
          struct pci_device_info info;
          info.bus = (u8)bus;
          info.slot = slot;
          info.func = func;
          info.vendor_id = vendor;
          info.device_id = dev;
          init_one_9p_device(&info);
        }
      }
    }
  }
}
