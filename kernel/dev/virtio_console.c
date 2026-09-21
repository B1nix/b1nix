/* SPDX-License-Identifier: GPL-2.0-only */
/* virtio-console: /dev/hvc0, and a kernel console that is not a UART.
 *
 * A 16550 under a hypervisor costs an exit per byte, and the emulator behind it
 * handles each byte on its own: the boot log and a shell on the serial line ran
 * at a few tens of KiB a second however the guest batched its writes. A virtio
 * console hands the host a page of text per notification instead.
 *
 * One port (no VIRTIO_CONSOLE_F_MULTIPORT): receiveq 0, transmitq 1, modern PCI
 * transport. Output is copied into a ring and pumped into page-sized transmit
 * buffers; the device returns them at once on the notification, so the pump
 * reclaims what the previous batch used before filling more. Input is polled
 * from the timer tick, as the virtio tablet is, and fed through the serial tty
 * line discipline that serves /dev/hvc0.
 *
 * `console=hvc0` on the command line moves the kernel console here as soon as
 * the device is up; everything printed before that went to the UART. */
#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <b1nix/serial.h>
#include <b1nix/serial_tty.h>
#include <b1nix/spinlock.h>
#include <b1nix/virtio.h>
#include <b1nix/virtio_console.h>
#include <string.h>
#include <b1nix/suspend.h>

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_CONSOLE_DEVICE_ID_MODERN 0x1043 /* virtio device type 3 */

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2

#define VIRTIO_F_VERSION_1_BIT 32

#define VC_QDEPTH 32
#define VC_RING_SIZE (256u * 1024u)

struct vc_common_cfg {
	volatile u32 device_feature_select;
	volatile u32 device_feature;
	volatile u32 driver_feature_select;
	volatile u32 driver_feature;
	volatile u16 msix_config;
	volatile u16 num_queues;
	volatile u8 device_status;
	volatile u8 config_generation;
	volatile u16 queue_select;
	volatile u16 queue_size;
	volatile u16 queue_msix_vector;
	volatile u16 queue_enable;
	volatile u16 queue_notify_off;
	volatile u32 queue_desc_lo;
	volatile u32 queue_desc_hi;
	volatile u32 queue_avail_lo;
	volatile u32 queue_avail_hi;
	volatile u32 queue_used_lo;
	volatile u32 queue_used_hi;
} __attribute__((packed));

struct vc_queue {
	struct virtqueue vq;
	volatile u16 *notify;
	u16 avail_idx;
	u64 buf_phys;      /* one page per descriptor, contiguous */
	u8 *buf;
	u16 free[VC_QDEPTH]; /* transmit: descriptors not in the device's hands */
	u16 nfree;
};

static volatile struct vc_common_cfg *vc_cfg;
static volatile u8 *vc_notify_base;
static u32 vc_notify_mult;
static struct vc_queue vc_rx, vc_tx;
static int vc_ready;

static spinlock_t vc_lock = SPINLOCK_INIT;
static char vc_ring[VC_RING_SIZE];
static usize vc_head, vc_tail; /* bytes in [head, tail), modulo size */
static u64 vc_dropped;

static usize vc_used_bytes(void)
{
	return (vc_tail + VC_RING_SIZE - vc_head) % VC_RING_SIZE;
}

static int vc_find(struct pci_device_info *info)
{
	for (u16 bus = 0; bus < 256; bus++) {
		for (u8 slot = 0; slot < 32; slot++) {
			if (pci_config_read16((u8)bus, slot, 0, 0) == 0xFFFF)
				continue;
			u8 header_type = pci_config_read8((u8)bus, slot, 0, 0x0E);
			u8 max_func = (header_type & 0x80) ? 8 : 1;
			for (u8 func = 0; func < max_func; func++) {
				if (pci_config_read16((u8)bus, slot, func, 0) != VIRTIO_VENDOR_ID ||
				    pci_config_read16((u8)bus, slot, func, 2) != VIRTIO_CONSOLE_DEVICE_ID_MODERN)
					continue;
				info->bus = (u8)bus;
				info->slot = slot;
				info->func = func;
				info->vendor_id = VIRTIO_VENDOR_ID;
				info->device_id = VIRTIO_CONSOLE_DEVICE_ID_MODERN;
				return 1;
			}
		}
	}
	return 0;
}

static int vc_map_caps(const struct pci_device_info *pci)
{
	volatile u8 *bar_map[6] = {0};
	u16 status = pci_config_read16(pci->bus, pci->slot, pci->func, 0x06);

	if (!(status & PCI_STATUS_CAP_LIST))
		return -1;
	for (u8 cap = pci_config_read8(pci->bus, pci->slot, pci->func, 0x34);
	     cap && cap != 0xff;
	     cap = pci_config_read8(pci->bus, pci->slot, pci->func, cap + 1)) {
		if (pci_config_read8(pci->bus, pci->slot, pci->func, cap) != PCI_CAP_ID_VENDOR)
			continue;
		u8 type = pci_config_read8(pci->bus, pci->slot, pci->func, cap + 3);
		u8 bar = pci_config_read8(pci->bus, pci->slot, pci->func, cap + 4);
		u32 off = pci_config_read32(pci->bus, pci->slot, pci->func, cap + 8);

		if (bar >= 6 || (type != VIRTIO_PCI_CAP_COMMON_CFG && type != VIRTIO_PCI_CAP_NOTIFY_CFG))
			continue;
		if (!bar_map[bar]) {
			u8 bar_off = (u8)(0x10 + bar * 4);
			u32 lo = pci_config_read32(pci->bus, pci->slot, pci->func, bar_off);
			u64 phys;

			if (lo & 1)
				continue; /* an I/O BAR carries no modern capability */
			phys = (u64)(lo & ~0xfU);
			if ((lo & 0x6) == 0x4 && bar < 5)
				phys |= (u64)pci_config_read32(pci->bus, pci->slot, pci->func,
				                               (u8)(bar_off + 4)) << 32;
			bar_map[bar] = (volatile u8 *)vmm_map_mmio(phys, 2 * 1024 * 1024,
			                                           VMM_WRITABLE | VMM_PCD);
		}
		if (!bar_map[bar])
			continue;
		if (type == VIRTIO_PCI_CAP_COMMON_CFG) {
			vc_cfg = (volatile struct vc_common_cfg *)(bar_map[bar] + off);
		} else {
			vc_notify_base = bar_map[bar] + off;
			vc_notify_mult = pci_config_read32(pci->bus, pci->slot, pci->func, cap + 16);
		}
	}
	return (vc_cfg && vc_notify_base) ? 0 : -1;
}

static int vc_setup_queue(u16 index, struct vc_queue *q, int device_writes)
{
	vc_cfg->queue_select = index;
	u16 qsize = vc_cfg->queue_size;

	if (qsize == 0)
		return -1;
	if (qsize > VC_QDEPTH)
		qsize = VC_QDEPTH;
	vc_cfg->queue_size = qsize;

	q->vq.queue_idx = index;
	q->vq.queue_size = qsize;
	q->vq.last_used_idx = 0;

	usize desc_size = 16u * qsize;
	usize avail_size = 6u + 2u * qsize;
	usize used_off = (desc_size + avail_size + PAGE_SIZE - 1) & ~(usize)(PAGE_SIZE - 1);
	usize used_size = 6u + 8u * qsize;
	usize frames = (used_off + used_size + PAGE_SIZE - 1) / PAGE_SIZE;
	u64 ring_phys = pmm_alloc_frames(frames);

	if (!ring_phys)
		return -1;
	u8 *ring = (u8 *)(usize)(ring_phys + vmm_direct_map_base());
	memset(ring, 0, frames * PAGE_SIZE);
	q->vq.desc = (struct vring_desc *)ring;
	q->vq.avail = (struct vring_avail *)(ring + desc_size);
	q->vq.used = (struct vring_used *)(ring + used_off);

	q->buf_phys = pmm_alloc_frames(qsize);
	if (!q->buf_phys)
		return -1;
	q->buf = (u8 *)(usize)(q->buf_phys + vmm_direct_map_base());

	for (u16 i = 0; i < qsize; i++) {
		q->vq.desc[i].addr = q->buf_phys + (u64)i * PAGE_SIZE;
		q->vq.desc[i].len = PAGE_SIZE;
		q->vq.desc[i].flags = device_writes ? VRING_DESC_F_WRITE : 0;
		q->vq.desc[i].next = 0;
		if (device_writes)
			q->vq.avail->ring[i] = i; /* every receive buffer posted */
		else
			q->free[q->nfree++] = i;
	}
	q->avail_idx = device_writes ? qsize : 0;
	q->vq.avail->idx = q->avail_idx;

	u64 avail_phys = ring_phys + desc_size;
	u64 used_phys = ring_phys + used_off;
	vc_cfg->queue_desc_lo = (u32)ring_phys;
	vc_cfg->queue_desc_hi = (u32)(ring_phys >> 32);
	vc_cfg->queue_avail_lo = (u32)avail_phys;
	vc_cfg->queue_avail_hi = (u32)(avail_phys >> 32);
	vc_cfg->queue_used_lo = (u32)used_phys;
	vc_cfg->queue_used_hi = (u32)(used_phys >> 32);
	vc_cfg->queue_enable = 1;
	q->notify = (volatile u16 *)(vc_notify_base +
	                             (u32)vc_cfg->queue_notify_off * vc_notify_mult);
	return 0;
}

/* Give back what the device has used, then hand it as much of the ring as the
 * free buffers hold. Lock held. */
static void vc_pump(void)
{
	struct vc_queue *q = &vc_tx;
	int kick = 0;

	while (q->vq.last_used_idx != q->vq.used->idx) {
		u16 id = (u16)q->vq.used->ring[q->vq.last_used_idx % q->vq.queue_size].id;

		if (id < q->vq.queue_size && q->nfree < VC_QDEPTH)
			q->free[q->nfree++] = id;
		q->vq.last_used_idx++;
	}
	while (q->nfree && vc_used_bytes()) {
		u16 id = q->free[--q->nfree];
		u8 *page = q->buf + (usize)id * PAGE_SIZE;
		usize n = 0;

		while (n < PAGE_SIZE && vc_head != vc_tail) {
			usize run = (vc_tail > vc_head ? vc_tail : VC_RING_SIZE) - vc_head;

			if (run > PAGE_SIZE - n)
				run = PAGE_SIZE - n;
			memcpy(page + n, vc_ring + vc_head, run);
			n += run;
			vc_head = (vc_head + run) % VC_RING_SIZE;
		}
		q->vq.desc[id].len = (u32)n;
		q->vq.avail->ring[q->avail_idx % q->vq.queue_size] = id;
		q->avail_idx++;
		q->vq.avail->idx = q->avail_idx;
		kick = 1;
	}
	if (kick)
		*q->notify = q->vq.queue_idx;
}

int virtio_console_ready(void)
{
	return vc_ready;
}

void virtio_console_write(const char *buf, usize len)
{
	if (!vc_ready || !len)
		return;

	u64 flags = interrupts_save();
	spin_lock(&vc_lock);
	while (len) {
		usize space = VC_RING_SIZE - 1 - vc_used_bytes();

		if (!space) {
			/* Full: the device returns buffers as it takes them, so pump
			 * until there is room, for a bounded time -- a host that stops
			 * reading must cost this CPU text, not the machine. */
			for (u32 spins = 0; spins < 200000 && !(VC_RING_SIZE - 1 - vc_used_bytes()); spins++) {
				vc_pump();
				cpu_relax();
			}
			space = VC_RING_SIZE - 1 - vc_used_bytes();
			if (!space) {
				vc_dropped += len;
				break;
			}
		}
		usize run = VC_RING_SIZE - vc_tail;

		if (run > space)
			run = space;
		if (run > len)
			run = len;
		memcpy(vc_ring + vc_tail, buf, run);
		vc_tail = (vc_tail + run) % VC_RING_SIZE;
		buf += run;
		len -= run;
	}
	vc_pump();
	spin_unlock(&vc_lock);
	interrupts_restore(flags);
}

/* Timer tick: return transmit buffers and collect input. */
void virtio_console_poll(void)
{
	char in[256];
	usize got = 0;

	if (!vc_ready)
		return;
	u64 flags = interrupts_save();
	spin_lock(&vc_lock);
	vc_pump();
	struct vc_queue *q = &vc_rx;
	int repost = 0;

	while (q->vq.last_used_idx != q->vq.used->idx && got < sizeof(in)) {
		struct vring_used_elem *e = &q->vq.used->ring[q->vq.last_used_idx % q->vq.queue_size];
		u16 id = (u16)e->id;

		if (id < q->vq.queue_size) {
			usize n = e->len < PAGE_SIZE ? e->len : PAGE_SIZE;

			if (n > sizeof(in) - got)
				n = sizeof(in) - got; /* the rest of a large paste is lost */
			memcpy(in + got, q->buf + (usize)id * PAGE_SIZE, n);
			got += n;
			q->vq.avail->ring[q->avail_idx % q->vq.queue_size] = id;
			q->avail_idx++;
			q->vq.avail->idx = q->avail_idx;
			repost = 1;
		}
		q->vq.last_used_idx++;
	}
	if (repost)
		*q->notify = q->vq.queue_idx;
	spin_unlock(&vc_lock);
	interrupts_restore(flags);
	if (got) {
		serial_tty_hvc_input(in, got);
		/* Typing on the virtual console ends a suspend, as typing on a real
		 * one does (M129). */
		suspend_wake_event("console");
	}
}

void virtio_console_init(void)
{
	struct pci_device_info pci;

	if (!vc_find(&pci))
		return;
	/* Memory and bus mastering on, and the legacy interrupt OFF: this driver
	 * polls. Left on, every returned buffer raises a level-triggered line
	 * that nothing acknowledges (the ISR register is never read), and a line
	 * shared with the disk or the network card stays asserted for good. */
	u16 cmd = pci_config_read16(pci.bus, pci.slot, pci.func, 0x04);
	pci_config_write16(pci.bus, pci.slot, pci.func, 0x04,
	                   (u16)(cmd | 0x0006 | PCI_CMD_INTX_DISABLE));
	if (vc_map_caps(&pci) != 0) {
		console_write("virtio-console: missing config capabilities\n");
		return;
	}
	vc_cfg->device_status = 0;
	vc_cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
	vc_cfg->device_status |= VIRTIO_STATUS_DRIVER;
	/* Nothing but VERSION_1: no multiport, no size, no emergency write. */
	vc_cfg->device_feature_select = 1;
	u32 hi = vc_cfg->device_feature;
	vc_cfg->driver_feature_select = 0;
	vc_cfg->driver_feature = 0;
	vc_cfg->driver_feature_select = 1;
	vc_cfg->driver_feature = hi & (1u << (VIRTIO_F_VERSION_1_BIT - 32));
	vc_cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;
	if (!(vc_cfg->device_status & VIRTIO_STATUS_FEATURES_OK)) {
		console_write("virtio-console: features rejected\n");
		return;
	}
	if (vc_setup_queue(0, &vc_rx, 1) != 0 || vc_setup_queue(1, &vc_tx, 0) != 0) {
		console_write("virtio-console: queue setup failed\n");
		vc_cfg->device_status |= VIRTIO_STATUS_FAILED;
		return;
	}
	vc_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;
	pci_bind_driver(&pci, "virtio-console");
	vc_ready = 1;
	serial_tty_hvc_attach();
	console_write("virtio-console: /dev/hvc0 ready\n");
	/* Proof for the harness, which reads the host side of the console. */
	if (bootinfo_has_flag("b1nix.test=1"))
		virtio_console_write("HVC-SMOKE: ok kernel-write\n", 27);

	char value[16];

	if (bootinfo_get_kv("console", value, sizeof(value)) && strcmp(value, "hvc0") == 0) {
		console_write("virtio-console: kernel console moves to hvc0\n");
		serial_console_divert(virtio_console_write);
	}
}
