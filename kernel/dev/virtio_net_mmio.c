/* virtio-net over the virtio-mmio transport (QEMU aarch64 "virt" machine).
 *
 * The mmio counterpart of kernel/dev/virtio_net.c, exactly as
 * virtio_blk_mmio.c is of virtio_blk.c: the netdev logic (TX buffer pool, RX
 * re-arming, used-ring draining) is transport-independent — only how the
 * device learns where the rings live, how it is kicked, and how its interrupt
 * is acknowledged differ. x86_64 reaches virtio-net over legacy virtio-PCI,
 * which needs inb/outb; those do not exist here, and QEMU's virt board has no
 * PCI host bridge wired up in this port anyway.
 *
 * Queue 0 is RX, queue 1 is TX (virtio spec 5.1.2). The device header in front
 * of every frame is 10 bytes on a legacy device and 12 on a modern one (the
 * extra num_buffers field), so its length is decided per device rather than by
 * sizeof(struct virtio_net_hdr).
 */
#include <b1nix/console.h>
#include <b1nix/irq.h>
#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/sched.h>
#include <b1nix/types.h>
#include <b1nix/virtio.h>
#include <string.h>

#define VIRTIO_MMIO_BASE   0x0a000000ULL
#define VIRTIO_MMIO_STRIDE 0x200ULL
#define VIRTIO_MMIO_SLOTS  32
#define VIRTIO_MMIO_IRQ(slot) (48 + (slot))

#define VIRTIO_MMIO_MAGIC_VALUE 0x74726976u /* "virt" */
#define VIRTIO_MMIO_DEVICE_ID_NET 1

struct vnet_mmio_regs {
  u32 magic_value;         /* 0x000 R */
  u32 version;             /* 0x004 R */
  u32 device_id;           /* 0x008 R */
  u32 vendor_id;           /* 0x00c R */
  u32 device_features;     /* 0x010 R */
  u32 device_features_sel; /* 0x014 W */
  u32 _pad1[2];
  u32 driver_features;     /* 0x020 W */
  u32 driver_features_sel; /* 0x024 W */
  u32 guest_page_size;     /* 0x028 W — legacy only */
  u32 _pad2[1];
  u32 queue_sel;           /* 0x030 W */
  u32 queue_num_max;       /* 0x034 R */
  u32 queue_num;           /* 0x038 W */
  u32 queue_align;         /* 0x03c W — legacy only */
  u32 queue_pfn;           /* 0x040 RW — legacy only */
  u32 queue_ready;         /* 0x044 RW — modern only */
  u32 _pad4[2];
  u32 queue_notify;        /* 0x050 W */
  u32 _pad5[3];
  u32 interrupt_status;    /* 0x060 R */
  u32 interrupt_ack;       /* 0x064 W */
  u32 _pad6[2];
  u32 status;              /* 0x070 RW */
  u32 _pad7[3];
  u32 queue_desc_low;      /* 0x080 W */
  u32 queue_desc_high;     /* 0x084 W */
  u32 _pad8[2];
  u32 queue_driver_low;    /* 0x090 W */
  u32 queue_driver_high;   /* 0x094 W */
  u32 _pad9[2];
  u32 queue_device_low;    /* 0x0a0 W */
  u32 queue_device_high;   /* 0x0a4 W */
  u32 _pad10[21];
  u32 config_generation;   /* 0x0fc R */
  u8 config[256];          /* 0x100+ — mac[6], status, ... */
  /* Not packed on purpose: every field is naturally aligned, and `packed`
   * would let the compiler split a 32-bit register access into byte accesses,
   * which Device memory does not tolerate. */
};

#define VIRTIO_STATUS_ACK         1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

#define RX_BUFFER_SIZE 2048
#define VNET_HDR_LEGACY 10
#define VNET_HDR_MODERN 12

static volatile struct vnet_mmio_regs *g_regs;
static struct virtqueue g_rx_vq;
static struct virtqueue g_tx_vq;
static usize g_hdr_len = VNET_HDR_MODERN;
static volatile int g_ready;
static volatile int g_tx_lock;
static volatile int g_rx_lock;

static void **tx_buffers;  /* one page each, pre-allocated */
static u8 *tx_inflight;
static u16 *tx_pool_free;
static u16 tx_pool_count;
static u16 *tx_pool_map;

static void **rx_buffers;  /* one page each: header + frame */
static u16 rx_buffer_count;

static struct netdev g_netdev;

static int is_power_of_two_u16(u16 v) { return v && ((v & (u16)(v - 1)) == 0); }

/* Lay a split virtqueue out in one physically contiguous allocation and tell
 * the device where it is. Identical shape to virtio_blk_mmio.c's version — the
 * ring layout is transport-independent, only these registers are not. */
static int vnet_setup_queue(volatile struct vnet_mmio_regs *regs, u32 qidx,
                            struct virtqueue *vq, int legacy) {
  regs->queue_sel = qidx;
  u32 qmax = regs->queue_num_max;
  if (qmax == 0)
    return -1;
  u16 qsize = qmax < 256 ? (u16)qmax : 256;
  regs->queue_num = qsize;

  vq->queue_idx = (u16)qidx;
  vq->queue_size = qsize;
  vq->last_used_idx = 0;

  usize desc_size = 16u * qsize;
  usize avail_size = 6u + 2u * qsize;
  usize offset_used = (desc_size + avail_size + 4095) & ~(usize)4095;
  usize used_size = 6u + 8u * qsize;
  usize total = offset_used + ((used_size + 4095) & ~(usize)4095);
  usize frames = (total + PAGE_SIZE - 1) / PAGE_SIZE;
  u64 paddr = pmm_alloc_frames(frames);
  if (!paddr)
    return -1;

  u8 *base = (u8 *)(usize)(paddr + vmm_direct_map_base());
  memset(base, 0, frames * PAGE_SIZE);
  vq->desc = (struct vring_desc *)base;
  vq->avail = (struct vring_avail *)(base + desc_size);
  vq->used = (struct vring_used *)(base + offset_used);

  if (legacy) {
    regs->queue_align = PAGE_SIZE;
    regs->queue_pfn = (u32)(paddr / PAGE_SIZE);
  } else {
    regs->queue_desc_low = (u32)(paddr & 0xffffffffu);
    regs->queue_desc_high = (u32)(paddr >> 32);
    u64 avail_phys = paddr + desc_size;
    regs->queue_driver_low = (u32)(avail_phys & 0xffffffffu);
    regs->queue_driver_high = (u32)(avail_phys >> 32);
    u64 used_phys = paddr + offset_used;
    regs->queue_device_low = (u32)(used_phys & 0xffffffffu);
    regs->queue_device_high = (u32)(used_phys >> 32);
    regs->queue_ready = 1;
  }
  return 0;
}

static void vnet_kick(u32 qidx) {
  __sync_synchronize();
  g_regs->queue_notify = qidx;
}

/* Hand RX descriptor `idx` back to the device pointing at its own buffer. */
static void fill_rx_buffer(u16 idx) {
  g_rx_vq.desc[idx].addr = vmm_virt_to_phys(rx_buffers[idx]);
  g_rx_vq.desc[idx].len = (u32)(g_hdr_len + RX_BUFFER_SIZE);
  g_rx_vq.desc[idx].flags = VRING_DESC_F_WRITE;
  g_rx_vq.desc[idx].next = 0;

  u16 avail_idx = g_rx_vq.avail->idx % g_rx_vq.queue_size;
  g_rx_vq.avail->ring[avail_idx] = idx;
  __sync_synchronize();
  g_rx_vq.avail->idx++;
  __sync_synchronize();
}

static int vnet_transmit(struct netdev *nd, const u8 hdr[14],
                         const void *payload, usize payload_len, u32 tx_flags) {
  (void)nd;
  /* No checksum offload here: the stack only sets NETDEV_TX_F_PARTIAL_CSUM
   * when every interface advertised NETDEV_F_TX_CSUM, and this one does not. */
  (void)tx_flags;
  if (!g_ready || g_tx_vq.queue_size == 0)
    return -1;

  usize packet_size = g_hdr_len + 14 + payload_len;
  if (packet_size > PAGE_SIZE)
    return -1;

  u8 *buffer = 0;
  u16 pool_idx = 0;
  for (int tries = 0; tries < 2; tries++) {
    while (__atomic_test_and_set(&g_tx_lock, __ATOMIC_ACQUIRE))
      scheduler_yield();
    if (tx_pool_count > 0) {
      tx_pool_count--;
      pool_idx = tx_pool_free[tx_pool_count];
      buffer = tx_buffers[pool_idx];
      __atomic_clear(&g_tx_lock, __ATOMIC_RELEASE);
      break;
    }
    __atomic_clear(&g_tx_lock, __ATOMIC_RELEASE);
    g_netdev.poll(&g_netdev); /* reap completions and retry once */
  }
  if (!buffer)
    return -1;

  memset(buffer, 0, g_hdr_len);
  memcpy(buffer + g_hdr_len, hdr, 14);
  memcpy(buffer + g_hdr_len + 14, payload, payload_len);

  for (int tries = 0; tries < 2; tries++) {
    while (__atomic_test_and_set(&g_tx_lock, __ATOMIC_ACQUIRE))
      scheduler_yield();

    u16 d0 = 0xFFFF;
    for (u16 i = 0; i < g_tx_vq.queue_size; i++) {
      if (!tx_inflight[i]) {
        d0 = i;
        break;
      }
    }
    if (d0 == 0xFFFF) {
      __atomic_clear(&g_tx_lock, __ATOMIC_RELEASE);
      g_netdev.poll(&g_netdev);
      continue;
    }

    tx_pool_map[d0] = pool_idx;
    tx_inflight[d0] = 1;
    g_tx_vq.desc[d0].addr = vmm_virt_to_phys(buffer);
    g_tx_vq.desc[d0].len = (u32)packet_size;
    g_tx_vq.desc[d0].flags = 0;
    g_tx_vq.desc[d0].next = 0;

    u16 avail_idx = g_tx_vq.avail->idx % g_tx_vq.queue_size;
    g_tx_vq.avail->ring[avail_idx] = d0;
    __sync_synchronize();
    g_tx_vq.avail->idx++;
    __sync_synchronize();
    vnet_kick(1);
    __atomic_clear(&g_tx_lock, __ATOMIC_RELEASE);
    return 0;
  }

  while (__atomic_test_and_set(&g_tx_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
  tx_pool_free[tx_pool_count++] = pool_idx;
  __atomic_clear(&g_tx_lock, __ATOMIC_RELEASE);
  return -1;
}

static void vnet_poll(struct netdev *nd) {
  (void)nd;
  if (!g_ready || g_tx_vq.queue_size == 0 || !g_rx_vq.used)
    return;

  /* Reap finished transmits. Bounded by queue_size so a device that keeps
   * used->idx ahead of us can never spin here holding the lock. */
  if (!__atomic_test_and_set(&g_tx_lock, __ATOMIC_ACQUIRE)) {
    u32 drained = 0;
    while (g_tx_vq.used && g_tx_vq.used->idx != g_tx_vq.last_used_idx &&
           drained < g_tx_vq.queue_size) {
      drained++;
      u16 used_idx = g_tx_vq.last_used_idx % g_tx_vq.queue_size;
      u32 id = g_tx_vq.used->ring[used_idx].id;
      if (id < g_tx_vq.queue_size && tx_inflight[id]) {
        tx_inflight[id] = 0;
        u16 pool_ret = tx_pool_map[id];
        if (pool_ret < g_tx_vq.queue_size)
          tx_pool_free[tx_pool_count++] = pool_ret;
      }
      g_tx_vq.last_used_idx++;
    }
    __atomic_clear(&g_tx_lock, __ATOMIC_RELEASE);
  }

  if (__atomic_test_and_set(&g_rx_lock, __ATOMIC_ACQUIRE))
    return;
  u32 rx_drained = 0;
  while (g_rx_vq.used->idx != g_rx_vq.last_used_idx &&
         rx_drained < g_rx_vq.queue_size) {
    rx_drained++;
    u16 used_idx = g_rx_vq.last_used_idx % g_rx_vq.queue_size;
    u32 id = g_rx_vq.used->ring[used_idx].id;
    u32 len = g_rx_vq.used->ring[used_idx].len;

    if (id < rx_buffer_count) {
      u8 *buf = (u8 *)rx_buffers[id];
      if (len > g_hdr_len) {
        usize payload_len = len - g_hdr_len;
        if (payload_len > RX_BUFFER_SIZE)
          payload_len = RX_BUFFER_SIZE;
        ethernet_receive(buf + g_hdr_len, payload_len);
      }
      fill_rx_buffer((u16)id);
      vnet_kick(0);
    }
    g_rx_vq.last_used_idx++;
  }
  __atomic_clear(&g_rx_lock, __ATOMIC_RELEASE);
}

static int vnet_irq_ack(struct netdev *nd) {
  (void)nd;
  if (!g_ready)
    return 0;
  u32 status = g_regs->interrupt_status;
  if (!status)
    return 0;
  g_regs->interrupt_ack = status;
  return (status & 1) ? 1 : 0;
}

static int vnet_link_up(struct netdev *nd) {
  (void)nd;
  return g_ready ? 1 : 0;
}

/* Used-buffer notification: the net task does the actual draining, so the
 * handler only clears the device's interrupt and reports that it was ours. */
static int vnet_mmio_irq(void *ctx) {
  (void)ctx;
  u32 status = g_regs->interrupt_status;
  if (!status)
    return 0;
  g_regs->interrupt_ack = status;
  return 1;
}

int virtio_net_mmio_init(void) {
  for (int slot = 0; slot < VIRTIO_MMIO_SLOTS; slot++) {
    u64 phys = VIRTIO_MMIO_BASE + (u64)slot * VIRTIO_MMIO_STRIDE;
    /* The low MMIO window is identity-mapped by boot.S, same as the GIC and
     * the UART — no mapping call needed (vmm_map_mmio is a stub here). */
    volatile struct vnet_mmio_regs *regs =
        (volatile struct vnet_mmio_regs *)(usize)phys;
    if (regs->magic_value != VIRTIO_MMIO_MAGIC_VALUE ||
        regs->device_id != VIRTIO_MMIO_DEVICE_ID_NET)
      continue;

    int legacy = (regs->version == 1);
    g_hdr_len = legacy ? VNET_HDR_LEGACY : VNET_HDR_MODERN;
    g_regs = regs;

    regs->status = 0;
    regs->status = VIRTIO_STATUS_ACK;
    regs->status |= VIRTIO_STATUS_DRIVER;

    /* VIRTIO_NET_F_MAC (word 0, bit 5): without acknowledging it the station
     * address in config space is not something the device promises to use, and
     * a device that is not told which address is ours has no reason to deliver
     * unicast frames to it — replies to our own ARP requests included. */
    regs->device_features_sel = 0;
    u32 dev_features0 = regs->device_features;
    u32 want0 = dev_features0 & (1u << 5);

    if (legacy) {
      regs->guest_page_size = PAGE_SIZE;
      regs->driver_features_sel = 0;
      regs->driver_features = want0;
    } else {
      regs->driver_features_sel = 0;
      regs->driver_features = want0;
      regs->driver_features_sel = 1;
      regs->driver_features = 1; /* VIRTIO_F_VERSION_1, mandatory for modern */
      regs->status |= VIRTIO_STATUS_FEATURES_OK;
      if (!(regs->status & VIRTIO_STATUS_FEATURES_OK)) {
        regs->status = VIRTIO_STATUS_FAILED;
        continue;
      }
    }

    if (vnet_setup_queue(regs, 0, &g_rx_vq, legacy) < 0 ||
        vnet_setup_queue(regs, 1, &g_tx_vq, legacy) < 0) {
      console_write("virtio-net-mmio: queue setup failed\n");
      regs->status = VIRTIO_STATUS_FAILED;
      continue;
    }
    if (!is_power_of_two_u16(g_rx_vq.queue_size) ||
        !is_power_of_two_u16(g_tx_vq.queue_size)) {
      console_write("virtio-net-mmio: queue size is not a power of two\n");
      regs->status = VIRTIO_STATUS_FAILED;
      continue;
    }
    /* Nothing waits on a TX completion interrupt — they are reaped by poll. */
    g_tx_vq.avail->flags = VRING_AVAIL_F_NO_INTERRUPT;

    tx_buffers = kzalloc(sizeof(void *) * g_tx_vq.queue_size);
    tx_inflight = kzalloc(sizeof(u8) * g_tx_vq.queue_size);
    tx_pool_free = kzalloc(sizeof(u16) * g_tx_vq.queue_size);
    tx_pool_map = kzalloc(sizeof(u16) * g_tx_vq.queue_size);
    if (!tx_buffers || !tx_inflight || !tx_pool_free || !tx_pool_map) {
      regs->status = VIRTIO_STATUS_FAILED;
      continue;
    }
    tx_pool_count = 0;
    for (u16 i = 0; i < g_tx_vq.queue_size; i++) {
      u64 frame = pmm_alloc_frame();
      if (!frame)
        break;
      tx_buffers[i] = (void *)(usize)(frame + vmm_direct_map_base());
      memset(tx_buffers[i], 0, PAGE_SIZE);
      tx_pool_free[tx_pool_count++] = i;
    }
    if (tx_pool_count == 0) {
      regs->status = VIRTIO_STATUS_FAILED;
      continue;
    }

    struct mac_addr mac;
    for (int i = 0; i < 6; i++)
      mac.bytes[i] = regs->config[i];

    rx_buffer_count = g_rx_vq.queue_size;
    rx_buffers = kzalloc(sizeof(void *) * rx_buffer_count);
    if (!rx_buffers) {
      regs->status = VIRTIO_STATUS_FAILED;
      continue;
    }
    for (u16 i = 0; i < rx_buffer_count; i++) {
      u64 frame = pmm_alloc_frame();
      if (!frame) {
        rx_buffer_count = i;
        break;
      }
      rx_buffers[i] = (void *)(usize)(frame + vmm_direct_map_base());
      memset(rx_buffers[i], 0, PAGE_SIZE);
      fill_rx_buffer(i);
    }

    u8 irq = (u8)VIRTIO_MMIO_IRQ(slot);
    irq_register_handler(irq, vnet_mmio_irq, 0);
    irq_unmask(irq);

    regs->status |= VIRTIO_STATUS_DRIVER_OK;
    g_ready = 1;
    vnet_kick(0);

    g_netdev.name = "virtio-net";
    g_netdev.mac = mac;
    g_netdev.irq = irq;
    g_netdev.transmit = vnet_transmit;
    g_netdev.poll = vnet_poll;
    g_netdev.irq_ack = vnet_irq_ack;
    g_netdev.link_up = vnet_link_up;
    g_netdev.priv = 0;
    netdev_register(&g_netdev);

    console_write("virtio-net-mmio: registered (slot ");
    console_write_dec(slot);
    console_write(", mac ");
    for (int i = 0; i < 6; i++) {
      const char *digits = "0123456789abcdef";
      console_putc(digits[(mac.bytes[i] >> 4) & 0xf]);
      console_putc(digits[mac.bytes[i] & 0xf]);
      if (i < 5)
        console_putc(':');
    }
    console_write(")\n");
    return 1;
  }

  console_write("virtio-net-mmio: no device found\n");
  return 0;
}
