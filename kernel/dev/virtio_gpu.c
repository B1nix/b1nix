#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/virtio.h>
#include <b1nix/virtio_gpu.h>
#include <string.h>

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_GPU_DEVICE_ID_LEGACY 0x1010
#define VIRTIO_GPU_DEVICE_ID_MODERN 0x1050

#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_FLAG_FENCE 1
#define PCI_STATUS_CAP_LIST 0x10
#define PCI_CAP_ID_VENDOR 0x09
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG 3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

enum virtio_gpu_ctrl_type {
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D = 0x0101,
    VIRTIO_GPU_CMD_RESOURCE_UNREF = 0x0102,
    VIRTIO_GPU_CMD_SET_SCANOUT = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO = 0x1101,
    VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR = 0x0301,
};

struct virtio_gpu_ctrl_hdr {
    u32 type;
    u32 flags;
    u64 fence_id;
    u32 ctx_id;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_rect {
    u32 x;
    u32 y;
    u32 width;
    u32 height;
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 resource_id;
    u32 format;
    u32 width;
    u32 height;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 resource_id;
    u32 nr_entries;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    u64 addr;
    u32 length;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect rect;
    u32 scanout_id;
    u32 resource_id;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect rect;
    u64 offset;
    u32 resource_id;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect rect;
    u32 resource_id;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_attach_backing_cmd {
    struct virtio_gpu_resource_attach_backing req;
    struct virtio_gpu_mem_entry entry;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct {
        struct virtio_gpu_rect rect;
        u32 enabled;
        u32 flags;
    } pmodes[16];
} __attribute__((packed));

struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 resource_id;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_cursor_pos {
    u32 scanout_id;
    u32 x;
    u32 y;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_update_cursor {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_cursor_pos pos;
    u32 resource_id;
    u32 hot_x;
    u32 hot_y;
    u32 padding;
} __attribute__((packed));

static struct virtio_device gpu_dev;
static struct virtqueue controlq;
static struct virtqueue cursorq;
static int gpu_ready;
static u64 gpu_fence_id = 1;
static u16 controlq_next_pair;
static u16 cursorq_next_pair;

static u32 gpu_width;
static u32 gpu_height;
static u32 gpu_resource_id = 1;
static u64 gpu_surface_phys;
static u32 *gpu_surface_virt;
static usize gpu_surface_bytes;
static u32 gpu_scanout_width;
static u32 gpu_scanout_height;
static int gpu_hw_cursor_ready;
static u32 gpu_cursor_resource_id = 2;
static u32 *gpu_cursor_surface_virt;
static u64 gpu_cursor_surface_phys;
static usize gpu_cursor_surface_bytes;
static int gpu_modern;
static volatile u16 *gpu_notify_addr[2];
static spinlock_t gpu_present_lock;
static u8 *gpu_control_req_dma;
static u8 *gpu_control_resp_dma;
static u8 *gpu_cursor_req_dma;
static u8 *gpu_cursor_resp_dma;

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

static volatile struct virtio_pci_common_cfg *gpu_common_cfg;
static volatile u8 *gpu_notify_base;
static volatile u8 *gpu_isr_cfg;
static volatile u8 *gpu_device_cfg;
static u32 gpu_notify_off_multiplier;

static inline u64 virt_to_phys_direct(void *ptr)
{
    u64 v = (u64)(usize)ptr;
    u64 dm = vmm_direct_map_base();
    if (v >= dm) {
        return v - dm;
    }
    return v;
}

static void virtio_gpu_reap_used(struct virtqueue *vq)
{
    if (!gpu_ready || !vq) return;
    vq->last_used_idx = vq->used->idx;
}

static inline u64 gpu_rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static int virtio_gpu_wait_used(struct virtqueue *vq, u16 target_used)
{
    /* Wall-clock-bounded, not iteration-bounded. A fixed spin count is
     * calibrated for one CPU speed: under fast KVM 400k iterations elapse in
     * microseconds — before the device finishes a large (e.g. full-frame 4 MiB)
     * transfer — so the wait falsely times out (M50 setcrtc / DRM present).
     * Bound by TSC cycles instead so it gives the device the same real time on
     * TCG and KVM. ~4e9 cycles is ~1-4 s at any reasonable frequency — far more
     * than a GPU command needs, and only that long on a genuine failure. */
    u64 start = gpu_rdtsc();
    const u64 budget = 4000000000ULL;
    while (vq->used->idx != target_used) {
        if (gpu_rdtsc() - start > budget) {
            return -1;
        }
    }
    vq->last_used_idx = vq->used->idx;
    return 0;
}

static int virtio_gpu_setup_modern_queue(struct virtqueue *vq, u16 queue_idx)
{
    if (!gpu_common_cfg) return -1;
    gpu_common_cfg->queue_select = queue_idx;
    u16 qsize = gpu_common_cfg->queue_size;
    if (qsize == 0) return -1;

    vq->queue_idx = queue_idx;
    vq->queue_size = qsize;
    vq->last_used_idx = 0;

    usize desc_size = 16 * qsize;
    usize avail_size = 6 + 2 * qsize;
    usize offset_used = (desc_size + avail_size + 4095) & ~4095ULL;
    usize used_size = 6 + 8 * qsize;
    usize total_size = offset_used + ((used_size + 4095) & ~4095ULL);
    usize frames = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 paddr = pmm_alloc_frames(frames);
    if (!paddr) return -1;

    u8 *base = (u8 *)(usize)(paddr + vmm_direct_map_base());
    memset(base, 0, frames * PAGE_SIZE);
    vq->desc = (struct vring_desc *)base;
    vq->avail = (struct vring_avail *)(base + desc_size);
    vq->avail->flags = VRING_AVAIL_F_NO_INTERRUPT;
    vq->used = (struct vring_used *)(base + offset_used);

    u64 desc_phys = paddr;
    u64 avail_phys = paddr + desc_size;
    u64 used_phys = paddr + offset_used;
    gpu_common_cfg->queue_desc_lo = (u32)(desc_phys & 0xffffffffU);
    gpu_common_cfg->queue_desc_hi = (u32)(desc_phys >> 32);
    gpu_common_cfg->queue_avail_lo = (u32)(avail_phys & 0xffffffffU);
    gpu_common_cfg->queue_avail_hi = (u32)(avail_phys >> 32);
    gpu_common_cfg->queue_used_lo = (u32)(used_phys & 0xffffffffU);
    gpu_common_cfg->queue_used_hi = (u32)(used_phys >> 32);
    gpu_common_cfg->queue_enable = 1;

    u16 notify_off = gpu_common_cfg->queue_notify_off;
    if (!gpu_notify_base) return -1;
    gpu_notify_addr[queue_idx] = (volatile u16 *)(gpu_notify_base + (u32)notify_off * gpu_notify_off_multiplier);
    return 0;
}

static int virtio_gpu_submit_pair(struct virtio_device *dev, struct virtqueue *vq, u16 *next_pair,
                                  const void *req, usize req_len, void *resp, usize resp_len, u16 *out_target_used)
{
    if (!gpu_ready || !dev || !vq || !next_pair || vq->queue_size < 2 || req_len == 0 || resp_len == 0) {
        return -1;
    }

    virtio_gpu_reap_used(vq);
    u16 qsize = vq->queue_size;
    u16 desc_head = (u16)(*next_pair % qsize);
    u16 desc_resp = (u16)((desc_head + 1) % qsize);
    *next_pair = (u16)((*next_pair + 2) % qsize);

    vq->desc[desc_head].addr = virt_to_phys_direct((void *)req);
    vq->desc[desc_head].len = (u32)req_len;
    vq->desc[desc_head].flags = VRING_DESC_F_NEXT;
    vq->desc[desc_head].next = desc_resp;

    vq->desc[desc_resp].addr = virt_to_phys_direct(resp);
    vq->desc[desc_resp].len = (u32)resp_len;
    vq->desc[desc_resp].flags = VRING_DESC_F_WRITE;
    vq->desc[desc_resp].next = 0;

    u16 avail_idx = vq->avail->idx;
    vq->avail->ring[avail_idx % qsize] = desc_head;
    vq->avail->idx = (u16)(avail_idx + 1);
    if (gpu_modern && vq->queue_idx < 2 && gpu_notify_addr[vq->queue_idx]) {
        *gpu_notify_addr[vq->queue_idx] = vq->queue_idx;
    } else {
        virtq_kick(dev, vq);
    }

    if (out_target_used) {
        *out_target_used = (u16)(vq->last_used_idx + 1);
    }
    return 0;
}

static int virtio_gpu_send_cmd(const void *req, usize req_len, void *resp, usize resp_len)
{
    if (!gpu_control_req_dma || !gpu_control_resp_dma ||
        req_len > PAGE_SIZE || resp_len > PAGE_SIZE)
        return -1;
    memcpy(gpu_control_req_dma, req, req_len);
    memset(gpu_control_resp_dma, 0, resp_len);
    u16 target_used;
    if (virtio_gpu_submit_pair(&gpu_dev, &controlq, &controlq_next_pair,
                               gpu_control_req_dma, req_len,
                               gpu_control_resp_dma, resp_len,
                               &target_used) < 0) {
        return -1;
    }
    if (virtio_gpu_wait_used(&controlq, target_used) < 0)
        return -1;
    memcpy(resp, gpu_control_resp_dma, resp_len);
    return 0;
}

static void virtio_gpu_draw_cursor_surface(int x, int y)
{
    if (!gpu_surface_virt || gpu_width == 0 || gpu_height == 0) return;
    for (int i = 0; i < 10; i++) {
        int px1 = x + i;
        int py1 = y;
        if (px1 >= 0 && py1 >= 0 && (u32)px1 < gpu_width && (u32)py1 < gpu_height) {
            gpu_surface_virt[(usize)py1 * gpu_width + (u32)px1] = 0x00ffffff;
        }
        int px2 = x;
        int py2 = y + i;
        if (px2 >= 0 && py2 >= 0 && (u32)px2 < gpu_width && (u32)py2 < gpu_height) {
            gpu_surface_virt[(usize)py2 * gpu_width + (u32)px2] = 0x00ffffff;
        }
    }
    for (int i = 0; i < 5; i++) {
        int px = x + i;
        int py = y + i;
        if (px >= 0 && py >= 0 && (u32)px < gpu_width && (u32)py < gpu_height) {
            gpu_surface_virt[(usize)py * gpu_width + (u32)px] = 0x00ffffff;
        }
    }
}

static int virtio_gpu_cursor_submit(u32 cmd_type, int x, int y, u32 resource_id)
{
    struct virtio_gpu_update_cursor req;
    struct virtio_gpu_ctrl_hdr resp;
    u16 target_used;

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    req.hdr.type = cmd_type;
    req.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    req.hdr.fence_id = gpu_fence_id++;
    req.pos.scanout_id = 0;
    req.pos.x = (x < 0) ? 0U : (u32)x;
    req.pos.y = (y < 0) ? 0U : (u32)y;
    req.resource_id = resource_id;
    req.hot_x = 0;
    req.hot_y = 0;

    if (!gpu_cursor_req_dma || !gpu_cursor_resp_dma)
        return -1;
    memcpy(gpu_cursor_req_dma, &req, sizeof(req));
    memset(gpu_cursor_resp_dma, 0, sizeof(resp));
    if (virtio_gpu_submit_pair(&gpu_dev, &cursorq, &cursorq_next_pair,
                               gpu_cursor_req_dma, sizeof(req),
                               gpu_cursor_resp_dma, sizeof(resp),
                               &target_used) < 0) {
        return -1;
    }
    if (virtio_gpu_wait_used(&cursorq, target_used) < 0) {
        return -1;
    }
    memcpy(&resp, gpu_cursor_resp_dma, sizeof(resp));
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

static int virtio_gpu_create_scanout_resource(u32 width, u32 height)
{
    struct virtio_gpu_resource_create_2d create;
    struct virtio_gpu_attach_backing_cmd attach;
    struct virtio_gpu_set_scanout scanout;
    struct virtio_gpu_ctrl_hdr resp;

    memset(&create, 0, sizeof(create));
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    create.hdr.fence_id = gpu_fence_id++;
    create.resource_id = gpu_resource_id;
    create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    create.width = width;
    create.height = height;
    if (virtio_gpu_send_cmd(&create, sizeof(create), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return -1;
    }

    memset(&attach, 0, sizeof(attach));
    attach.req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.req.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    attach.req.hdr.fence_id = gpu_fence_id++;
    attach.req.resource_id = gpu_resource_id;
    attach.req.nr_entries = 1;
    attach.entry.addr = gpu_surface_phys;
    attach.entry.length = (u32)gpu_surface_bytes;
    if (virtio_gpu_send_cmd(&attach, sizeof(attach), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return -1;
    }

    memset(&scanout, 0, sizeof(scanout));
    scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    scanout.hdr.fence_id = gpu_fence_id++;
    scanout.rect.x = 0;
    scanout.rect.y = 0;
    scanout.rect.width = width;
    scanout.rect.height = height;
    scanout.scanout_id = 0;
    scanout.resource_id = gpu_resource_id;
    if (virtio_gpu_send_cmd(&scanout, sizeof(scanout), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return -1;
    }

    return 0;
}

static void virtio_gpu_query_display_info(void)
{
    struct virtio_gpu_ctrl_hdr req;
    struct virtio_gpu_resp_display_info resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));

    req.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    req.flags = VIRTIO_GPU_FLAG_FENCE;
    req.fence_id = gpu_fence_id++;
    if (virtio_gpu_send_cmd(&req, sizeof(req), &resp, sizeof(resp)) < 0 ||
        resp.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        return;
    }

    if (resp.pmodes[0].enabled) {
        gpu_scanout_width = resp.pmodes[0].rect.width;
        gpu_scanout_height = resp.pmodes[0].rect.height;
        console_write("virtio-gpu: scanout0 ");
        console_write_dec(gpu_scanout_width);
        console_write("x");
        console_write_dec(gpu_scanout_height);
        console_write("\n");
    }
}

static void virtio_gpu_unref_resource(void)
{
    if (gpu_resource_id == 0) return;

    struct virtio_gpu_resource_unref unref;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&unref, 0, sizeof(unref));
    unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    unref.hdr.fence_id = gpu_fence_id++;
    unref.resource_id = gpu_resource_id;
    (void)virtio_gpu_send_cmd(&unref, sizeof(unref), &resp, sizeof(resp));
}

static int virtio_gpu_init_hw_cursor(void)
{
    usize cursor_px = 64;
    gpu_cursor_surface_bytes = cursor_px * cursor_px * sizeof(u32);
    usize pages = (gpu_cursor_surface_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    gpu_cursor_surface_phys = pmm_alloc_frames(pages);
    if (!gpu_cursor_surface_phys) return -1;
    gpu_cursor_surface_virt = (u32 *)(usize)(gpu_cursor_surface_phys + vmm_direct_map_base());
    memset(gpu_cursor_surface_virt, 0, pages * PAGE_SIZE);
    for (int i = 0; i < 16; i++) {
        gpu_cursor_surface_virt[(usize)0 * 64 + (usize)i] = 0x00ffffff;
        gpu_cursor_surface_virt[(usize)i * 64 + (usize)0] = 0x00ffffff;
    }
    for (int i = 0; i < 8; i++) {
        gpu_cursor_surface_virt[(usize)i * 64 + (usize)i] = 0x00ffffff;
    }

    struct virtio_gpu_resource_create_2d create;
    struct virtio_gpu_attach_backing_cmd attach;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&create, 0, sizeof(create));
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    create.hdr.fence_id = gpu_fence_id++;
    create.resource_id = gpu_cursor_resource_id;
    create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    create.width = 64;
    create.height = 64;
    if (virtio_gpu_send_cmd(&create, sizeof(create), &resp, sizeof(resp)) < 0 || resp.type != VIRTIO_GPU_RESP_OK_NODATA) return -1;

    memset(&attach, 0, sizeof(attach));
    attach.req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.req.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    attach.req.hdr.fence_id = gpu_fence_id++;
    attach.req.resource_id = gpu_cursor_resource_id;
    attach.req.nr_entries = 1;
    attach.entry.addr = gpu_cursor_surface_phys;
    attach.entry.length = (u32)(pages * PAGE_SIZE);
    if (virtio_gpu_send_cmd(&attach, sizeof(attach), &resp, sizeof(resp)) < 0 || resp.type != VIRTIO_GPU_RESP_OK_NODATA) return -1;

    if (virtio_gpu_cursor_submit(VIRTIO_GPU_CMD_UPDATE_CURSOR, 0, 0, gpu_cursor_resource_id) < 0) {
        return -1;
    }
    return 0;
}

void virtio_gpu_init(void)
{
    struct pci_device_info pci;
    u16 device_id = 0;

    gpu_ready = 0;
    if (pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_GPU_DEVICE_ID_LEGACY, &pci)) {
        device_id = VIRTIO_GPU_DEVICE_ID_LEGACY;
    } else if (pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_GPU_DEVICE_ID_MODERN, &pci)) {
        device_id = VIRTIO_GPU_DEVICE_ID_MODERN;
    } else {
        console_write("virtio-gpu: device not found\n");
        return;
    }

    int modern_ok = 0;
    if (device_id == VIRTIO_GPU_DEVICE_ID_MODERN) {
        u16 cmd = pci_config_read16(pci.bus, pci.slot, pci.func, 0x04);
        cmd |= 0x0002;
        cmd |= 0x0004;
        pci_config_write16(pci.bus, pci.slot, pci.func, 0x04, cmd);

        u16 status = pci_config_read16(pci.bus, pci.slot, pci.func, 0x06);
        if (status & PCI_STATUS_CAP_LIST) {
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
                                    u32 hi = pci_config_read32(pci.bus, pci.slot, pci.func, (u8)(bar_off + 4));
                                    phys |= ((u64)hi << 32);
                                }
                                bar_map[bar] = (volatile u8 *)vmm_map_mmio(phys, 2 * 1024 * 1024, VMM_WRITABLE | VMM_PCD);
                            }
                        }
                        if (bar_map[bar]) {
                            if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) gpu_common_cfg = (volatile struct virtio_pci_common_cfg *)(bar_map[bar] + off);
                            if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                                gpu_notify_base = bar_map[bar] + off;
                                gpu_notify_off_multiplier = pci_config_read32(pci.bus, pci.slot, pci.func, cap + 16);
                            }
                            if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG) gpu_isr_cfg = bar_map[bar] + off;
                            if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) gpu_device_cfg = bar_map[bar] + off;
                        }
                    }
                }
                cap = next;
            }

            if (gpu_common_cfg && gpu_notify_base && gpu_isr_cfg && gpu_device_cfg) {
                gpu_common_cfg->device_status = 0;
                gpu_common_cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
                gpu_common_cfg->device_status |= VIRTIO_STATUS_DRIVER;

                gpu_common_cfg->device_feature_select = 0;
                u32 _devfeat0 = gpu_common_cfg->device_feature;
                (void)_devfeat0;
                gpu_common_cfg->driver_feature_select = 0;
                gpu_common_cfg->driver_feature = 0;
                gpu_common_cfg->driver_feature_select = 1;
                gpu_common_cfg->driver_feature = 0;

                gpu_common_cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;
                if (gpu_common_cfg->device_status & VIRTIO_STATUS_FEATURES_OK) {
                    gpu_modern = 1;
                    if (virtio_gpu_setup_modern_queue(&controlq, 0) == 0 &&
                        virtio_gpu_setup_modern_queue(&cursorq, 1) == 0) {
                        gpu_common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;
                        modern_ok = 1;
                    }
                }
            }
        }
    }

    if (!modern_ok) {
        if (!virtio_init_device(&gpu_dev, VIRTIO_VENDOR_ID, device_id)) {
            console_write("virtio-gpu: transport init failed\n");
            return;
        }
        virtio_set_guest_features(&gpu_dev, 0);
        virtio_set_status(&gpu_dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
        if ((virtio_get_status(&gpu_dev) & VIRTIO_STATUS_FEATURES_OK) == 0) {
            virtio_set_status(&gpu_dev, VIRTIO_STATUS_FAILED);
            console_write("virtio-gpu: features rejected\n");
            return;
        }
        if (!virtq_init(&gpu_dev, 0, &controlq) || !virtq_init(&gpu_dev, 1, &cursorq)) {
            virtio_set_status(&gpu_dev, VIRTIO_STATUS_FAILED);
            console_write("virtio-gpu: queue init failed\n");
            return;
        }
        controlq.avail->flags = VRING_AVAIL_F_NO_INTERRUPT;
        cursorq.avail->flags = VRING_AVAIL_F_NO_INTERRUPT;
        virtio_set_status(&gpu_dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                                    VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    }

    u64 dma_phys = pmm_alloc_frames(4);
    if (!dma_phys) {
        console_write("virtio-gpu: DMA scratch allocation failed\n");
        return;
    }
    u8 *dma = (u8 *)(usize)(dma_phys + vmm_direct_map_base());
    memset(dma, 0, 4 * PAGE_SIZE);
    gpu_control_req_dma = dma;
    gpu_control_resp_dma = dma + PAGE_SIZE;
    gpu_cursor_req_dma = dma + 2 * PAGE_SIZE;
    gpu_cursor_resp_dma = dma + 3 * PAGE_SIZE;

    gpu_ready = 1;
    controlq_next_pair = 0;
    cursorq_next_pair = 0;
    if (gpu_modern) {
        console_write("virtio-gpu: modern transport enabled\n");
    }
    virtio_gpu_query_display_info();
    if (virtio_gpu_init_hw_cursor() == 0) {
        gpu_hw_cursor_ready = 1;
        console_write("virtio-gpu: hw cursor enabled\n");
    }
    console_write("virtio-gpu: ready\n");
}

int virtio_gpu_ready(void)
{
    return gpu_ready;
}

void virtio_gpu_get_mode(u32 *width, u32 *height)
{
    if (width) *width = gpu_scanout_width;
    if (height) *height = gpu_scanout_height;
}

static int virtio_gpu_present_locked(const u32 *src, u32 width, u32 height,
                                     u32 dirty_x, u32 dirty_y, u32 dirty_w,
                                     u32 dirty_h, int cursor_x, int cursor_y,
                                     int cursor_visible)
{
    if (!gpu_ready || !src || width == 0 || height == 0) {
        return -1;
    }

    usize bytes = (usize)width * (usize)height * sizeof(u32);
    if (!gpu_surface_virt || width != gpu_width || height != gpu_height) {
        if (gpu_surface_virt && (width != gpu_width || height != gpu_height)) {
            virtio_gpu_unref_resource();
            gpu_surface_virt = 0;
            gpu_surface_phys = 0;
            gpu_surface_bytes = 0;
        }
        usize pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        gpu_surface_phys = pmm_alloc_frames(pages);
        if (!gpu_surface_phys) {
            return -1;
        }
        gpu_surface_virt = (u32 *)(usize)(gpu_surface_phys + vmm_direct_map_base());
        gpu_surface_bytes = pages * PAGE_SIZE;
        gpu_width = width;
        gpu_height = height;
        memset(gpu_surface_virt, 0, gpu_surface_bytes);
        if (virtio_gpu_create_scanout_resource(width, height) < 0) {
            return -1;
        }
    }

    if (dirty_x >= width || dirty_y >= height || dirty_w == 0 || dirty_h == 0) {
        return 0;
    }
    if (dirty_x + dirty_w > width) dirty_w = width - dirty_x;
    if (dirty_y + dirty_h > height) dirty_h = height - dirty_y;

    for (u32 row = 0; row < dirty_h; row++) {
        usize off = ((usize)(dirty_y + row) * width + dirty_x);
        memcpy(gpu_surface_virt + off, src + off, (usize)dirty_w * sizeof(u32));
    }
    if (cursor_visible) {
        if (gpu_hw_cursor_ready) {
            (void)virtio_gpu_cursor_submit(VIRTIO_GPU_CMD_MOVE_CURSOR, cursor_x, cursor_y, 0);
        } else {
            virtio_gpu_draw_cursor_surface(cursor_x, cursor_y);
        }
    }

    struct virtio_gpu_transfer_to_host_2d transfer;
    struct virtio_gpu_resource_flush flush;
    struct virtio_gpu_ctrl_hdr resp;

    memset(&transfer, 0, sizeof(transfer));
    transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    transfer.hdr.fence_id = gpu_fence_id++;
    transfer.rect.x = dirty_x;
    transfer.rect.y = dirty_y;
    transfer.rect.width = dirty_w;
    transfer.rect.height = dirty_h;
    transfer.offset = ((u64)dirty_y * width + dirty_x) * sizeof(u32);
    transfer.resource_id = gpu_resource_id;
    if (virtio_gpu_send_cmd(&transfer, sizeof(transfer), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return -1;
    }

    memset(&flush, 0, sizeof(flush));
    flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    flush.hdr.fence_id = gpu_fence_id++;
    flush.rect.x = dirty_x;
    flush.rect.y = dirty_y;
    flush.rect.width = dirty_w;
    flush.rect.height = dirty_h;
    flush.resource_id = gpu_resource_id;
    if (virtio_gpu_send_cmd(&flush, sizeof(flush), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return -1;
    }

    return 0;
}

int virtio_gpu_present(const u32 *src, u32 width, u32 height, u32 dirty_x,
                       u32 dirty_y, u32 dirty_w, u32 dirty_h, int cursor_x,
                       int cursor_y, int cursor_visible)
{
    /* ponytail: one queue lock; split control/cursor locks if throughput matters. */
    u64 flags;
    spin_lock_irqsave(&gpu_present_lock, &flags);
    int rc = virtio_gpu_present_locked(src, width, height, dirty_x, dirty_y,
                                       dirty_w, dirty_h, cursor_x, cursor_y,
                                       cursor_visible);
    spin_unlock_irqrestore(&gpu_present_lock, flags);
    return rc;
}
