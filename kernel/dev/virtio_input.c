/* virtio-input driver — binds every virtio-input PCI device QEMU exposes.
 *
 * The first device is treated as the absolute pointer (virtio-tablet-pci) so
 * the mouse tracks the host cursor grab-free; its events go to
 * /dev/input/event1 and EV_ABS values are scaled to the live framebuffer
 * resolution (displayd consumes them as absolute cursor positions).
 *
 * The second device is treated as a touchscreen and routed to
 * /dev/input/event2: EV_ABS values pass through unscaled in the device's
 * native 0..32767 range (the contract the compositor expects for wl_touch),
 * and the device's pointer button (BTN_LEFT) is translated to BTN_TOUCH so a
 * single-touch contact-down/up sequence is emitted. A QEMU virtio-tablet-pci
 * already advertises EV_ABS ABS_X/ABS_Y plus a contact button, so it doubles
 * as a single-touch touchscreen without any host evdev passthrough.
 *
 * The device delivers Linux-style input events on its event virtqueue
 * (queue 0); each struct virtio_input_event maps 1:1 onto b1nix's input layer
 * (EV_REL/EV_ABS/EV_KEY/EV_SYN use the same codes).
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

/* At most a pointer (event1) and a touchscreen (event2). */
#define VI_MAX_DEVS 2

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

/* Per-device context. input_dev selects how raw events are translated before
 * they reach the input layer (see vi_translate). */
struct vi_dev {
    volatile struct virtio_pci_common_cfg *common_cfg;
    volatile u8 *notify_base;
    u32 notify_off_multiplier;
    volatile u16 *eventq_notify;

    struct virtqueue eventq;
    struct virtio_input_event *buf; /* EVENTQ_DEPTH events, DMA */
    u16 avail_idx;
    int ready;
    int input_dev; /* INPUT_DEV_MOUSE or INPUT_DEV_TOUCH */
};

static struct vi_dev vi_devs[VI_MAX_DEVS];
static int vi_ndevs;

static int vi_setup_eventq(struct vi_dev *d)
{
    if (!d->common_cfg)
        return -1;
    d->common_cfg->queue_select = 0;
    u16 qsize = d->common_cfg->queue_size;
    if (qsize == 0)
        return -1;
    if (qsize > EVENTQ_DEPTH)
        qsize = EVENTQ_DEPTH;

    d->eventq.queue_idx = 0;
    d->eventq.queue_size = qsize;
    d->eventq.last_used_idx = 0;

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
    d->eventq.desc = (struct vring_desc *)base;
    d->eventq.avail = (struct vring_avail *)(base + desc_size);
    d->eventq.used = (struct vring_used *)(base + offset_used);

    /* Event buffers: one page holds EVENTQ_DEPTH * 8 bytes comfortably. */
    u64 buf_phys = pmm_alloc_frames(1);
    if (!buf_phys)
        return -1;
    d->buf = (struct virtio_input_event *)(usize)(buf_phys + vmm_direct_map_base());
    memset(d->buf, 0, PAGE_SIZE);

    /* Post every buffer as device-writable and make it available. */
    for (u16 i = 0; i < qsize; i++) {
        d->eventq.desc[i].addr = buf_phys + (u64)i * sizeof(struct virtio_input_event);
        d->eventq.desc[i].len = sizeof(struct virtio_input_event);
        d->eventq.desc[i].flags = VRING_DESC_F_WRITE;
        d->eventq.desc[i].next = 0;
        d->eventq.avail->ring[i] = i;
    }
    d->avail_idx = qsize;
    d->eventq.avail->idx = d->avail_idx;

    u64 desc_phys = paddr;
    u64 avail_phys = paddr + desc_size;
    u64 used_phys = paddr + offset_used;
    d->common_cfg->queue_desc_lo = (u32)(desc_phys & 0xffffffffU);
    d->common_cfg->queue_desc_hi = (u32)(desc_phys >> 32);
    d->common_cfg->queue_avail_lo = (u32)(avail_phys & 0xffffffffU);
    d->common_cfg->queue_avail_hi = (u32)(avail_phys >> 32);
    d->common_cfg->queue_used_lo = (u32)(used_phys & 0xffffffffU);
    d->common_cfg->queue_used_hi = (u32)(used_phys >> 32);
    d->common_cfg->queue_enable = 1;

    u16 notify_off = d->common_cfg->queue_notify_off;
    if (!d->notify_base)
        return -1;
    d->eventq_notify =
        (volatile u16 *)(d->notify_base + (u32)notify_off * d->notify_off_multiplier);
    return 0;
}

/* Bring up a single virtio-input PCI function into the context `d`.
 * Returns 0 on success, -1 if the function could not be brought up. */
static int vi_bind(struct vi_dev *d, const struct pci_device_info *pci)
{
    u16 cmd = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
    cmd |= 0x0002; /* memory space */
    cmd |= 0x0004; /* bus master */
    pci_config_write16(pci->bus, pci->slot, pci->func, 0x04, cmd);

    u16 status = pci_config_read16(pci->bus, pci->slot, pci->func, 0x06);
    if (!(status & PCI_STATUS_CAP_LIST))
        return -1;

    u8 cap = pci_config_read8(pci->bus, pci->slot, pci->func, 0x34);
    volatile u8 *bar_map[6] = {0};
    while (cap && cap != 0xff) {
        u8 cap_id = pci_config_read8(pci->bus, pci->slot, pci->func, cap + 0);
        u8 next = pci_config_read8(pci->bus, pci->slot, pci->func, cap + 1);
        if (cap_id == PCI_CAP_ID_VENDOR) {
            u8 cfg_type = pci_config_read8(pci->bus, pci->slot, pci->func, cap + 3);
            u8 bar = pci_config_read8(pci->bus, pci->slot, pci->func, cap + 4);
            u32 off = pci_config_read32(pci->bus, pci->slot, pci->func, cap + 8);
            u32 len = pci_config_read32(pci->bus, pci->slot, pci->func, cap + 12);
            if (bar < 6 && len > 0) {
                if (!bar_map[bar]) {
                    u8 bar_off = (u8)(0x10 + bar * 4);
                    u32 lo = pci_config_read32(pci->bus, pci->slot, pci->func, bar_off);
                    if ((lo & 1) == 0) {
                        u64 phys = (u64)(lo & ~0xfU);
                        if ((lo & 0x6) == 0x4 && bar < 5) {
                            u32 hi = pci_config_read32(pci->bus, pci->slot, pci->func,
                                                       (u8)(bar_off + 4));
                            phys |= ((u64)hi << 32);
                        }
                        bar_map[bar] = (volatile u8 *)vmm_map_mmio(
                            phys, 2 * 1024 * 1024, VMM_WRITABLE | VMM_PCD);
                    }
                }
                if (bar_map[bar]) {
                    if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG)
                        d->common_cfg =
                            (volatile struct virtio_pci_common_cfg *)(bar_map[bar] + off);
                    if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                        d->notify_base = bar_map[bar] + off;
                        d->notify_off_multiplier =
                            pci_config_read32(pci->bus, pci->slot, pci->func, cap + 16);
                    }
                }
            }
        }
        cap = next;
    }

    if (!d->common_cfg || !d->notify_base) {
        console_write("virtio-input: missing config caps\n");
        return -1;
    }

    d->common_cfg->device_status = 0;
    d->common_cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    d->common_cfg->device_status |= VIRTIO_STATUS_DRIVER;
    d->common_cfg->driver_feature_select = 0;
    d->common_cfg->driver_feature = 0;
    d->common_cfg->driver_feature_select = 1;
    d->common_cfg->driver_feature = 0;
    d->common_cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;
    if (!(d->common_cfg->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        console_write("virtio-input: features rejected\n");
        return -1;
    }
    if (vi_setup_eventq(d) != 0) {
        console_write("virtio-input: eventq setup failed\n");
        return -1;
    }
    d->common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;
    d->ready = 1;
    return 0;
}

/* Enumerate every virtio-input PCI function in bus order and capture the
 * `index`-th one (mirrors pci_find_device but selects by occurrence). */
static int vi_find_nth(int index, struct pci_device_info *info)
{
    int seen = 0;
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
                if (dev != VIRTIO_INPUT_DEVICE_ID_MODERN)
                    continue;
                if (seen++ != index)
                    continue;
                if (info) {
                    info->bus = (u8)bus;
                    info->slot = slot;
                    info->func = func;
                    info->vendor_id = vendor;
                    info->device_id = dev;
                    info->class_code = pci_config_read8((u8)bus, slot, func, 0x0B);
                    info->subclass = pci_config_read8((u8)bus, slot, func, 0x0A);
                    info->prog_if = pci_config_read8((u8)bus, slot, func, 0x09);
                    info->irq_line = pci_config_read8((u8)bus, slot, func, 0x3C);
                }
                return 1;
            }
        }
    }
    return 0;
}

void virtio_input_init(void)
{
    vi_ndevs = 0;
    memset(vi_devs, 0, sizeof(vi_devs));

    for (int i = 0; i < VI_MAX_DEVS; i++) {
        struct pci_device_info pci;
        if (!vi_find_nth(i, &pci))
            break; /* no more virtio-input devices */

        struct vi_dev *d = &vi_devs[vi_ndevs];
        memset(d, 0, sizeof(*d));
        /* First device → absolute pointer (event1); second → touch (event2). */
        d->input_dev = (i == 0) ? INPUT_DEV_MOUSE : INPUT_DEV_TOUCH;
        if (vi_bind(d, &pci) != 0)
            continue; /* leave this slot unused, try the next device */
        vi_ndevs++;
        if (d->input_dev == INPUT_DEV_TOUCH)
            console_write("virtio-input: touchscreen ready (/dev/input/event2)\n");
        else
            console_write("virtio-input: tablet ready (absolute pointer)\n");
    }

    if (vi_ndevs == 0)
        return; /* no tablet attached — silent; PS/2 mouse still works */
}

/* Translate one raw virtio-input event and push it to the input layer.
 *
 * Pointer (event1): EV_ABS values are scaled device-range → framebuffer
 * pixels; everything else passes through (Linux-identical codes).
 *
 * Touch (event2): EV_ABS values pass through unscaled in the native
 * 0..32767 range, and the pointer button (BTN_LEFT) becomes BTN_TOUCH so the
 * report is a single-touch contact-down/up sequence. */
static void vi_translate(struct vi_dev *d, const struct virtio_input_event *ev)
{
    if (d->input_dev == INPUT_DEV_TOUCH) {
        if (ev->type == B1NIX_EV_ABS &&
            (ev->code == B1NIX_ABS_X || ev->code == B1NIX_ABS_Y)) {
            /* Pass through in the device's native 0..32767 range. */
            input_event_push(INPUT_DEV_TOUCH, ev->type, ev->code, (i32)ev->value);
        } else if (ev->type == B1NIX_EV_KEY && ev->code == B1NIX_BTN_LEFT) {
            /* Contact button → BTN_TOUCH (down = 1, up = 0). */
            input_event_push(INPUT_DEV_TOUCH, B1NIX_EV_KEY, B1NIX_BTN_TOUCH,
                             (i32)ev->value);
        } else if (ev->type == B1NIX_EV_SYN) {
            input_event_sync(INPUT_DEV_TOUCH);
        }
        /* Drop EV_REL and other axes — a touchscreen reports absolute only. */
        return;
    }

    /* Pointer (event1). */
    if (ev->type == B1NIX_EV_ABS) {
        /* Scale device range → live framebuffer pixels. */
        const struct boot_info *bi = bootinfo_get();
        i32 v = (i32)ev->value;
        if (ev->code == B1NIX_ABS_X)
            v = (i32)((u64)ev->value * bi->framebuffer.width /
                      (VIRTIO_INPUT_ABS_MAX + 1));
        else if (ev->code == B1NIX_ABS_Y)
            v = (i32)((u64)ev->value * bi->framebuffer.height /
                      (VIRTIO_INPUT_ABS_MAX + 1));
        input_event_push(INPUT_DEV_MOUSE, ev->type, ev->code, v);
    } else if (ev->type == B1NIX_EV_SYN) {
        input_event_sync(INPUT_DEV_MOUSE);
    } else {
        /* EV_REL / EV_KEY pass straight through (Linux-identical codes). */
        input_event_push(INPUT_DEV_MOUSE, ev->type, ev->code, (i32)ev->value);
    }
}

/* Drain one device's event virtqueue, translate each event, and re-post its
 * buffer. */
static void vi_poll_dev(struct vi_dev *d)
{
    if (!d->ready)
        return;
    int kicked = 0;
    while (d->eventq.last_used_idx != d->eventq.used->idx) {
        struct vring_used_elem *e =
            &d->eventq.used->ring[d->eventq.last_used_idx % d->eventq.queue_size];
        u16 desc_id = (u16)e->id;
        if (desc_id < d->eventq.queue_size) {
            struct virtio_input_event ev = d->buf[desc_id];
            vi_translate(d, &ev);
            /* Re-post this buffer as available. */
            d->eventq.avail->ring[d->avail_idx % d->eventq.queue_size] = desc_id;
            d->avail_idx++;
            d->eventq.avail->idx = d->avail_idx;
            kicked = 1;
        }
        d->eventq.last_used_idx++;
    }
    if (kicked && d->eventq_notify)
        *d->eventq_notify = 0; /* notify queue 0 */
}

/* Drain every bound virtio-input device. Called from the timer tick. */
void virtio_input_poll(void)
{
    for (int i = 0; i < vi_ndevs; i++)
        vi_poll_dev(&vi_devs[i]);
}
