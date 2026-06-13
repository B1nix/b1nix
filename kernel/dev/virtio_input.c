/* virtio-input driver — absolute pointer (virtio-tablet-pci) so the mouse tracks
 * the host cursor grab-free in QEMU. The device delivers Linux-style input
 * events on its event virtqueue (queue 0); each struct virtio_input_event maps
 * 1:1 onto b1nix's input layer (EV_REL/EV_ABS/EV_KEY/EV_SYN use the same codes).
 * EV_ABS values arrive in the device's 0..32767 range and are scaled to the
 * live framebuffer resolution before being pushed to /dev/input/event1, where
 * displayd consumes them as absolute cursor positions.
 *
 * Modern (virtio 1.0) transport only — virtio-input has no legacy variant. The
 * event queue is drained by polling the used ring from the timer tick
 * (virtio_input_poll), mirroring how the PS/2 keyboard is polled, so no MSI-X /
 * INTx routing is required. */
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/input.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <b1nix/virtio.h>
#include <string.h>

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_INPUT_DEVICE_ID_MODERN 0x1052 /* virtio device type 18 (input) */

#define PCI_STATUS_CAP_LIST 0x10
#define PCI_CAP_ID_VENDOR 0x09
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG 3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

/* virtio-tablet reports ABS axes in [0, 32767]. */
#define VIRTIO_INPUT_ABS_MAX 32767

struct virtio_input_event {
    u16 type;
    u16 code;
    u32 value;
} __attribute__((packed));

struct virtio_pci_common_cfg {
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

#define EVENTQ_DEPTH 64

static volatile struct virtio_pci_common_cfg *vi_common_cfg;
static volatile u8 *vi_notify_base;
static u32 vi_notify_off_multiplier;
static volatile u16 *vi_eventq_notify;

static struct virtqueue eventq;
static struct virtio_input_event *vi_buf; /* EVENTQ_DEPTH events, DMA */
static u16 vi_avail_idx;
static int vi_ready;

static int vi_setup_eventq(void)
{
    if (!vi_common_cfg)
        return -1;
    vi_common_cfg->queue_select = 0;
    u16 qsize = vi_common_cfg->queue_size;
    if (qsize == 0)
        return -1;
    if (qsize > EVENTQ_DEPTH)
        qsize = EVENTQ_DEPTH;

    eventq.queue_idx = 0;
    eventq.queue_size = qsize;
    eventq.last_used_idx = 0;

    usize desc_size = 16 * qsize;
    usize avail_size = 6 + 2 * qsize;
    usize offset_used = (desc_size + avail_size + 4095) & ~4095ULL;
    usize used_size = 6 + 8 * qsize;
    usize total_size = offset_used + ((used_size + 4095) & ~4095ULL);
    usize frames = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 paddr = pmm_alloc_frames(frames);
    if (!paddr)
        return -1;
    u8 *base = (u8 *)(usize)(paddr + vmm_direct_map_base());
    memset(base, 0, frames * PAGE_SIZE);
    eventq.desc = (struct vring_desc *)base;
    eventq.avail = (struct vring_avail *)(base + desc_size);
    eventq.used = (struct vring_used *)(base + offset_used);

    /* Event buffers: one page holds EVENTQ_DEPTH * 8 bytes comfortably. */
    u64 buf_phys = pmm_alloc_frames(1);
    if (!buf_phys)
        return -1;
    vi_buf = (struct virtio_input_event *)(usize)(buf_phys + vmm_direct_map_base());
    memset(vi_buf, 0, PAGE_SIZE);

    /* Post every buffer as device-writable and make it available. */
    for (u16 i = 0; i < qsize; i++) {
        eventq.desc[i].addr = buf_phys + (u64)i * sizeof(struct virtio_input_event);
        eventq.desc[i].len = sizeof(struct virtio_input_event);
        eventq.desc[i].flags = VRING_DESC_F_WRITE;
        eventq.desc[i].next = 0;
        eventq.avail->ring[i] = i;
    }
    vi_avail_idx = qsize;
    eventq.avail->idx = vi_avail_idx;

    u64 desc_phys = paddr;
    u64 avail_phys = paddr + desc_size;
    u64 used_phys = paddr + offset_used;
    vi_common_cfg->queue_desc_lo = (u32)(desc_phys & 0xffffffffU);
    vi_common_cfg->queue_desc_hi = (u32)(desc_phys >> 32);
    vi_common_cfg->queue_avail_lo = (u32)(avail_phys & 0xffffffffU);
    vi_common_cfg->queue_avail_hi = (u32)(avail_phys >> 32);
    vi_common_cfg->queue_used_lo = (u32)(used_phys & 0xffffffffU);
    vi_common_cfg->queue_used_hi = (u32)(used_phys >> 32);
    vi_common_cfg->queue_enable = 1;

    u16 notify_off = vi_common_cfg->queue_notify_off;
    if (!vi_notify_base)
        return -1;
    vi_eventq_notify =
        (volatile u16 *)(vi_notify_base + (u32)notify_off * vi_notify_off_multiplier);
    return 0;
}

void virtio_input_init(void)
{
    struct pci_device_info pci;
    vi_ready = 0;
    if (!pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_INPUT_DEVICE_ID_MODERN, &pci))
        return; /* no tablet attached — silent; PS/2 mouse still works */

    u16 cmd = pci_config_read16(pci.bus, pci.slot, pci.func, 0x04);
    cmd |= 0x0002; /* memory space */
    cmd |= 0x0004; /* bus master */
    pci_config_write16(pci.bus, pci.slot, pci.func, 0x04, cmd);

    u16 status = pci_config_read16(pci.bus, pci.slot, pci.func, 0x06);
    if (!(status & PCI_STATUS_CAP_LIST))
        return;

    u8 cap = pci_config_read8(pci.bus, pci.slot, pci.func, 0x34);
    volatile u8 *bar_map[6] = {0};
    while (cap && cap != 0xff) {
        u8 cap_id = pci_config_read8(pci.bus, pci.slot, pci.func, cap + 0);
        u8 next = pci_config_read8(pci.bus, pci.slot, pci.func, cap + 1);
        if (cap_id == PCI_CAP_ID_VENDOR) {
            u8 cfg_type = pci_config_read8(pci.bus, pci.slot, pci.func, cap + 3);
            u8 bar = pci_config_read8(pci.bus, pci.slot, pci.func, cap + 4);
            u32 off = pci_config_read32(pci.bus, pci.slot, pci.func, cap + 8);
            u32 len = pci_config_read32(pci.bus, pci.slot, pci.func, cap + 12);
            if (bar < 6 && len > 0) {
                if (!bar_map[bar]) {
                    u8 bar_off = (u8)(0x10 + bar * 4);
                    u32 lo = pci_config_read32(pci.bus, pci.slot, pci.func, bar_off);
                    if ((lo & 1) == 0) {
                        u64 phys = (u64)(lo & ~0xfU);
                        if ((lo & 0x6) == 0x4 && bar < 5) {
                            u32 hi = pci_config_read32(pci.bus, pci.slot, pci.func,
                                                       (u8)(bar_off + 4));
                            phys |= ((u64)hi << 32);
                        }
                        bar_map[bar] = (volatile u8 *)vmm_map_mmio(
                            phys, 2 * 1024 * 1024, VMM_WRITABLE | VMM_PCD);
                    }
                }
                if (bar_map[bar]) {
                    if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG)
                        vi_common_cfg =
                            (volatile struct virtio_pci_common_cfg *)(bar_map[bar] + off);
                    if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                        vi_notify_base = bar_map[bar] + off;
                        vi_notify_off_multiplier =
                            pci_config_read32(pci.bus, pci.slot, pci.func, cap + 16);
                    }
                }
            }
        }
        cap = next;
    }

    if (!vi_common_cfg || !vi_notify_base) {
        console_write("virtio-input: missing config caps\n");
        return;
    }

    vi_common_cfg->device_status = 0;
    vi_common_cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    vi_common_cfg->device_status |= VIRTIO_STATUS_DRIVER;
    vi_common_cfg->driver_feature_select = 0;
    vi_common_cfg->driver_feature = 0;
    vi_common_cfg->driver_feature_select = 1;
    vi_common_cfg->driver_feature = 0;
    vi_common_cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;
    if (!(vi_common_cfg->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        console_write("virtio-input: features rejected\n");
        return;
    }
    if (vi_setup_eventq() != 0) {
        console_write("virtio-input: eventq setup failed\n");
        return;
    }
    vi_common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;
    vi_ready = 1;
    console_write("virtio-input: tablet ready (absolute pointer)\n");
}

/* Drain the event virtqueue, translate each event, and re-post its buffer.
 * Called from the timer tick. */
void virtio_input_poll(void)
{
    if (!vi_ready)
        return;
    int kicked = 0;
    while (eventq.last_used_idx != eventq.used->idx) {
        struct vring_used_elem *e =
            &eventq.used->ring[eventq.last_used_idx % eventq.queue_size];
        u16 desc_id = (u16)e->id;
        if (desc_id < eventq.queue_size) {
            struct virtio_input_event ev = vi_buf[desc_id];
            if (ev.type == B1NIX_EV_ABS) {
                /* Scale device range -> live framebuffer pixels. */
                const struct boot_info *bi = bootinfo_get();
                i32 v = (i32)ev.value;
                if (ev.code == B1NIX_ABS_X)
                    v = (i32)((u64)ev.value * bi->framebuffer.width /
                              (VIRTIO_INPUT_ABS_MAX + 1));
                else if (ev.code == B1NIX_ABS_Y)
                    v = (i32)((u64)ev.value * bi->framebuffer.height /
                              (VIRTIO_INPUT_ABS_MAX + 1));
                input_event_push(INPUT_DEV_MOUSE, ev.type, ev.code, v);
            } else if (ev.type == B1NIX_EV_SYN) {
                input_event_sync(INPUT_DEV_MOUSE);
            } else {
                /* EV_REL / EV_KEY pass straight through (Linux-identical codes). */
                input_event_push(INPUT_DEV_MOUSE, ev.type, ev.code, (i32)ev.value);
            }
            /* Re-post this buffer as available. */
            eventq.avail->ring[vi_avail_idx % eventq.queue_size] = desc_id;
            vi_avail_idx++;
            eventq.avail->idx = vi_avail_idx;
            kicked = 1;
        }
        eventq.last_used_idx++;
    }
    if (kicked && vi_eventq_notify)
        *vi_eventq_notify = 0; /* notify queue 0 */
}
