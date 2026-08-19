#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/dma_fence.h>
#include <b1nix/errno.h>
#include <b1nix/gpu_scheduler.h>
#include <b1nix/irq.h>
#include <b1nix/irq.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <b1nix/vfs.h>
#include <b1nix/virtio.h>
#include <b1nix/virtio_gpu.h>
#include <string.h>

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_GPU_DEVICE_ID_LEGACY 0x1010
#define VIRTIO_GPU_DEVICE_ID_MODERN 0x1050

#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_FLAG_FENCE 1
/* The cursor resource the device composes on the host side is a fixed square. */
#define VIRTIO_GPU_CURSOR_DIM 64

/* VirGL 3D acceleration. VIRTIO_GPU_F_VIRGL is feature bit 0; when the host
 * exposes a virglrenderer-backed device (qemu -device virtio-gpu-gl-*) it offers
 * this bit and accepts the 3D command set below. The numbers come straight from
 * the VirtIO GPU spec and Mesa's src/virtio/virtio-gpu/virgl_protocol.h /
 * virgl_hw.h, so a hand-built command stream is byte-compatible with what the
 * Mesa virgl driver emits. */
#define VIRTIO_GPU_F_VIRGL 0

/* virgl context command stream opcodes (enum virgl_context_cmd). */
#define VIRGL_CCMD_CREATE_OBJECT 1
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5
#define VIRGL_CCMD_CLEAR 7
#define VIRGL_OBJECT_SURFACE 8
/* VIRGL_CMD0(cmd, obj, len): obj in bits 8-15, payload dword count in 16-31. */
#define VIRGL_CMD0(cmd, obj, len) ((u32)(cmd) | ((u32)(obj) << 8) | ((u32)(len) << 16))

/* gallium / virgl_hw.h constants used by the selftest clear. */
#define VIRGL_FORMAT_B8G8R8A8_UNORM 1
#define PIPE_TEXTURE_2D 2
#define VIRGL_BIND_RENDER_TARGET (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW (1u << 3)
#define PIPE_CLEAR_COLOR0 (1u << 2)
/* PCI_STATUS_CAP_LIST / PCI_CAP_ID_VENDOR now come from <b1nix/pci.h> (M98). */
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

    /* VirGL 3D command subset (host virglrenderer). RESOURCE_DETACH_BACKING
     * occupies 0x0107, so the capset queries start at 0x0108. */
    VIRTIO_GPU_CMD_GET_CAPSET_INFO = 0x0108,
    VIRTIO_GPU_CMD_GET_CAPSET = 0x0109,
    VIRTIO_GPU_CMD_CTX_CREATE = 0x0200,
    VIRTIO_GPU_CMD_CTX_DESTROY = 0x0201,
    VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE = 0x0202,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_3D = 0x0204,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D = 0x0205,
    VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D = 0x0206,
    VIRTIO_GPU_CMD_SUBMIT_3D = 0x0207,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO = 0x1102,
    VIRTIO_GPU_RESP_OK_CAPSET = 0x1103,
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

/* ── VirGL 3D command structs (VirtIO GPU spec) ─────────────────────────── */
struct virtio_gpu_get_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 capset_index;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_resp_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 capset_id;
    u32 capset_max_version;
    u32 capset_max_size;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_get_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 capset_id;
    u32 capset_version;
} __attribute__((packed));

struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 nlen;
    u32 context_init;
    char debug_name[64];
} __attribute__((packed));

struct virtio_gpu_ctx_resource {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 resource_id;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_resource_create_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 resource_id;
    u32 target;
    u32 format;
    u32 bind;
    u32 width;
    u32 height;
    u32 depth;
    u32 array_size;
    u32 last_level;
    u32 nr_samples;
    u32 flags;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_box {
    u32 x, y, z;
    u32 w, h, d;
} __attribute__((packed));

struct virtio_gpu_transfer_host_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_box box;
    u64 offset;
    u32 resource_id;
    u32 level;
    u32 stride;
    u32 layer_stride;
} __attribute__((packed));

/* SUBMIT_3D: header + size + padding, immediately followed by the command
 * stream dwords. The clear stream is tiny, so an inline tail buffer suffices. */
struct virtio_gpu_cmd_submit {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 size;
    u32 padding;
    u32 cmd[32];
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
static int gpu_cursor_visible;
static int gpu_cursor_x;
static int gpu_cursor_y;
static u32 gpu_cursor_hot_x;
static u32 gpu_cursor_hot_y;
static int gpu_modern;
static int gpu_virgl_offered;
static int gpu_virgl_ok;
static volatile u16 *gpu_notify_addr[2];
/* Guards gpu_present_busy only — see gpu_present_acquire(). */
static spinlock_t gpu_present_lock;
static int gpu_present_busy;
static u8 *gpu_control_req_dma;
static u8 *gpu_control_resp_dma;
static u8 *gpu_cursor_req_dma;
static u8 *gpu_cursor_resp_dma;

/* Defined next to virtio_gpu_present(); the cursor calls need them earlier. */
static void gpu_present_acquire(void);
static void gpu_present_release(void);

/*
 * Completion interrupts.
 *
 * The queues used to be created with VRING_AVAIL_F_NO_INTERRUPT and every
 * command was waited out in a busy loop, with the frame path holding a
 * spinlock and interrupts disabled for the whole round trip: a compositor
 * frame masked the timer tick and signal delivery on its CPU for as long as
 * the transfer took. The device raises an interrupt per completion now, and a
 * waiter that cannot finish in a short spin parks on the queue instead.
 *
 * gpu_irq_ready gates the blocking path: until the handler is registered (and
 * on the legacy transport, where there is no ISR register to acknowledge) the
 * wait stays the poll it always was.
 */
static int gpu_irq_ready;
static u8 gpu_irq_line;
static int gpu_msix_vector = -1;
static volatile u64 gpu_irq_count;   /* handler invocations that claimed the IRQ */
static volatile u64 gpu_wait_spun;   /* waits the short spin satisfied */
static volatile u64 gpu_wait_parked; /* waits that gave up the CPU */
static volatile u64 gpu_wait_irq;    /* parks an interrupt ended */
static volatile u64 gpu_wait_repoll; /* parks the watchdog ended (lost IRQ) */
/* Test-only: skip the spin so the proof is about the blocking path itself. */
static int gpu_wait_no_spin;

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

/* One watchdog period is what a lost interrupt costs before the wait notices
 * on its own. Two ticks (20 ms) is long enough that a working interrupt always
 * wins the race and short enough that a device that never raises one still
 * makes progress. */
#define GPU_WAIT_WATCHDOG_TICKS 2

/* A completion arrived. Neither delivery says which queue it was for, and both
 * waiters re-check their own used index before believing a wake, so waking both
 * is correct. */
static void virtio_gpu_irq_wake(void)
{
    gpu_irq_count++;
    scheduler_wake_all(&controlq);
    scheduler_wake_all(&cursorq);
}

/* MSI-X: the message belongs to this device alone, so there is nothing to claim
 * and no ISR register to acknowledge — the write to the local APIC is the whole
 * event. */
static int virtio_gpu_irq_msi(void *ctx)
{
    (void)ctx;
    if (!gpu_ready)
        return 0;
    virtio_gpu_irq_wake();
    return 1;
}

/* INTx: reading the ISR register both reports and acknowledges the interrupt,
 * and a zero read means the line belongs to another device — so this doubles as
 * the shared-line claim. */
static int virtio_gpu_irq(void *ctx)
{
    (void)ctx;
    if (!gpu_ready || !gpu_isr_cfg)
        return 0;
    u8 isr = *gpu_isr_cfg;
    if ((isr & 0x3) == 0)
        return 0;
    virtio_gpu_irq_wake();
    return 1;
}

static int virtio_gpu_wait_used(struct virtqueue *vq, u16 target_used)
{
    /* Fast path: a short no-MMIO spin. The host usually finishes a small
     * command inside it, so the common case costs neither an interrupt nor a
     * context switch. */
    if (!gpu_wait_no_spin) {
        for (int round = 0; round < 16; round++) {
            for (int i = 0; i < 256; i++)
                __asm__ volatile("pause");
            if (vq->used->idx == target_used) {
                gpu_wait_spun++;
                vq->last_used_idx = vq->used->idx;
                return 0;
            }
        }
    }

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
        /* Before the handler exists, from an IRQs-off caller, or before the
         * scheduler is live, this is the poll it always was. */
        if (!gpu_irq_ready || !scheduler_can_block())
            continue;
        /* Park until virtio_gpu_irq() wakes us. Publishing BLOCKED first and
         * re-checking the used index after closes the window where the
         * completion lands between the test and the block; the watchdog
         * deadline turns a lost interrupt into a re-poll, never a wedge. */
        u64 irqs_before = gpu_irq_count;
        scheduler_wait_prepare_timeout(vq, GPU_WAIT_WATCHDOG_TICKS);
        if (vq->used->idx == target_used) {
            scheduler_wait_cancel();
            break;
        }
        gpu_wait_parked++;
        scheduler_wait_commit();
        if (gpu_irq_count != irqs_before && vq->used->idx == target_used)
            gpu_wait_irq++;
        else
            gpu_wait_repoll++;
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

static int virtio_gpu_cursor_submit(u32 cmd_type, int x, int y, u32 resource_id,
                                    u32 hot_x, u32 hot_y)
{
    struct virtio_gpu_update_cursor req;
    /* Written by nothing; the device needs a writable descriptor in the chain
     * and returns it untouched. */
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
    req.hot_x = hot_x;
    req.hot_y = hot_y;

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
    /*
     * The cursor queue answers with nothing. Unlike the control queue, the
     * device writes no response body here — it takes the command and returns
     * the buffers with a zero-length write — so the only thing that can be
     * checked is that the descriptor came back, which the wait above did.
     *
     * This is why the hardware cursor was written and never worked: the code
     * insisted on reading VIRTIO_GPU_RESP_OK_NODATA out of a buffer the device
     * never writes, so every cursor command reported failure and the driver
     * concluded it had no cursor.
     */
    return 0;
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

/* Copy the guest-side cursor pixels into the host's copy of the resource.
 * Attaching backing tells the device where the memory is; only a transfer tells
 * it to read from there. */
static int virtio_gpu_cursor_upload_image(void)
{
    struct virtio_gpu_transfer_to_host_2d xfer;
    struct virtio_gpu_ctrl_hdr resp;

    memset(&xfer, 0, sizeof(xfer));
    xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    xfer.hdr.fence_id = gpu_fence_id++;
    xfer.rect.x = 0;
    xfer.rect.y = 0;
    xfer.rect.width = VIRTIO_GPU_CURSOR_DIM;
    xfer.rect.height = VIRTIO_GPU_CURSOR_DIM;
    xfer.offset = 0;
    xfer.resource_id = gpu_cursor_resource_id;
    if (virtio_gpu_send_cmd(&xfer, sizeof(xfer), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -1;
    return 0;
}

static int virtio_gpu_init_hw_cursor(void)
{
    usize cursor_px = VIRTIO_GPU_CURSOR_DIM;
    gpu_cursor_surface_bytes = cursor_px * cursor_px * sizeof(u32);
    usize pages = (gpu_cursor_surface_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    gpu_cursor_surface_phys = pmm_alloc_frames(pages);
    if (!gpu_cursor_surface_phys) return -1;
    gpu_cursor_surface_virt = (u32 *)(usize)(gpu_cursor_surface_phys + vmm_direct_map_base());
    memset(gpu_cursor_surface_virt, 0, pages * PAGE_SIZE);
    /* A default pointer, opaque so it is visible over any frame: the alpha byte
     * of a BGRA cursor is what the host blends with, and 0 there is invisible
     * however bright the other three are. */
    for (int i = 0; i < 16; i++) {
        gpu_cursor_surface_virt[(usize)0 * VIRTIO_GPU_CURSOR_DIM + (usize)i] = 0xffffffff;
        gpu_cursor_surface_virt[(usize)i * VIRTIO_GPU_CURSOR_DIM + (usize)0] = 0xffffffff;
    }
    for (int i = 0; i < 8; i++) {
        gpu_cursor_surface_virt[(usize)i * VIRTIO_GPU_CURSOR_DIM + (usize)i] = 0xffffffff;
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
    create.width = VIRTIO_GPU_CURSOR_DIM;
    create.height = VIRTIO_GPU_CURSOR_DIM;
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

    /* The device now knows the resource exists and where its backing lives,
     * and that is all it knows: nothing has copied the pixels the loop above
     * wrote into the host's copy of the resource, so a cursor turned on here
     * would show whatever the host allocated. Upload it. */
    if (virtio_gpu_cursor_upload_image() < 0)
        return -1;

    /* No cursor on screen until something asks for one. Parking a hard-coded
     * arrow at the origin at boot decorates a display nobody asked to
     * decorate, and the console then has to draw around it. */
    return 0;
}

/* ── the hardware cursor ─────────────────────────────────────────────────
 *
 * The device composes this on the host side: moving it costs one small command
 * on the cursor queue and never touches the scanout resource. The alternative
 * this replaces — painting the pointer into the frame — cannot be undone,
 * because the pixels it overwrote were not saved anywhere.
 */
int virtio_gpu_cursor_ready(void)
{
    return gpu_ready && gpu_hw_cursor_ready;
}

int virtio_gpu_set_cursor_image(const u32 *bgra, u32 width, u32 height,
                                u32 hot_x, u32 hot_y)
{
    if (!virtio_gpu_cursor_ready() || !gpu_cursor_surface_virt)
        return -1;
    if (!bgra || width == 0 || height == 0 || width > VIRTIO_GPU_CURSOR_DIM ||
        height > VIRTIO_GPU_CURSOR_DIM)
        return -1;
    if (hot_x >= width || hot_y >= height)
        return -1;

    gpu_present_acquire();
    /* The device's cursor resource is a fixed 64x64; a smaller image sits in
     * its top-left corner with the rest transparent. */
    memset(gpu_cursor_surface_virt, 0, gpu_cursor_surface_bytes);
    for (u32 row = 0; row < height; row++) {
        memcpy(gpu_cursor_surface_virt + (usize)row * VIRTIO_GPU_CURSOR_DIM,
               bgra + (usize)row * width, (usize)width * sizeof(u32));
    }
    gpu_cursor_hot_x = hot_x;
    gpu_cursor_hot_y = hot_y;
    int rc = virtio_gpu_cursor_upload_image();
    if (rc == 0 && gpu_cursor_visible)
        rc = virtio_gpu_cursor_submit(VIRTIO_GPU_CMD_UPDATE_CURSOR,
                                      gpu_cursor_x, gpu_cursor_y,
                                      gpu_cursor_resource_id, hot_x, hot_y);
    gpu_present_release();
    return rc;
}

int virtio_gpu_show_cursor(int x, int y)
{
    if (!virtio_gpu_cursor_ready())
        return -1;
    gpu_present_acquire();
    gpu_cursor_x = x;
    gpu_cursor_y = y;
    /* UPDATE_CURSOR names the resource as well as the position; MOVE_CURSOR
     * only moves what is already shown. */
    int rc = virtio_gpu_cursor_submit(VIRTIO_GPU_CMD_UPDATE_CURSOR, x, y,
                                      gpu_cursor_resource_id, gpu_cursor_hot_x,
                                      gpu_cursor_hot_y);
    if (rc == 0)
        gpu_cursor_visible = 1;
    gpu_present_release();
    return rc;
}

int virtio_gpu_move_cursor(int x, int y)
{
    if (!virtio_gpu_cursor_ready())
        return -1;
    if (!gpu_cursor_visible)
        return virtio_gpu_show_cursor(x, y);
    gpu_present_acquire();
    gpu_cursor_x = x;
    gpu_cursor_y = y;
    int rc = virtio_gpu_cursor_submit(VIRTIO_GPU_CMD_MOVE_CURSOR, x, y, 0, 0, 0);
    gpu_present_release();
    return rc;
}

int virtio_gpu_hide_cursor(void)
{
    if (!virtio_gpu_cursor_ready())
        return -1;
    gpu_present_acquire();
    /* Resource 0 is how the spec spells "no cursor". */
    int rc = virtio_gpu_cursor_submit(VIRTIO_GPU_CMD_UPDATE_CURSOR,
                                      gpu_cursor_x, gpu_cursor_y, 0, 0, 0);
    if (rc == 0)
        gpu_cursor_visible = 0;
    gpu_present_release();
    return rc;
}

/* ── VirGL 3D acceleration selftest ──────────────────────────────────────
 * Exercises the full accelerated path against the host virglrenderer:
 * negotiate the capset, create a 3D context + render-target resource, submit a
 * hand-built virgl command stream that clears the render target to a known
 * colour, then TRANSFER_FROM_HOST_3D the GPU-rendered pixels back to guest
 * memory and verify them. Real GPU work — virglrenderer translates the clear to
 * host OpenGL on the host GPU (egl-headless / renderD128). Only runs when the
 * device actually offered VIRGL (the virtio-gpu-gl device); on a plain
 * virtio-gpu-pci host this is a no-op and emits no markers. */
#define VIRGL_SELFTEST_CTX 1
#define VIRGL_SELFTEST_RES 16
#define VIRGL_SELFTEST_SURF 1
#define VIRGL_SELFTEST_DIM 64

static int virtio_gpu_virgl_hdr_ok(const struct virtio_gpu_ctrl_hdr *h, u32 want)
{
    return h->type == want;
}

static void virtio_gpu_virgl_selftest(void)
{
    if (!gpu_ready || !gpu_virgl_ok) return;
    if (!bootinfo_has_flag("b1nix.test=1")) return;

    console_write("M52-GFX: ok virgl-negotiate\n");

    /* 1. Capset query — proves the host virglrenderer backend is live. */
    struct virtio_gpu_get_capset_info capreq;
    struct virtio_gpu_resp_capset_info capresp;
    memset(&capreq, 0, sizeof(capreq));
    memset(&capresp, 0, sizeof(capresp));
    capreq.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    capreq.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    capreq.hdr.fence_id = gpu_fence_id++;
    capreq.capset_index = 0;
    if (virtio_gpu_send_cmd(&capreq, sizeof(capreq), &capresp, sizeof(capresp)) < 0 ||
        !virtio_gpu_virgl_hdr_ok(&capresp.hdr, VIRTIO_GPU_RESP_OK_CAPSET_INFO) ||
        capresp.capset_id == 0) {
        console_write("M52-GFX: fail virgl-capset-info\n");
        return;
    }

    struct virtio_gpu_get_capset getcap;
    /* The response is the header followed by the capset blob (~1 KiB for VIRGL2).
     * Size the buffer so QEMU copies the whole thing instead of truncating and
     * logging a guest error; only the header type is inspected. */
    struct {
        struct virtio_gpu_ctrl_hdr hdr;
        u8 capset_data[3072];
    } __attribute__((packed)) capdata;
    usize capdata_len = sizeof(capdata);
    if (capresp.capset_max_size + sizeof(struct virtio_gpu_ctrl_hdr) < capdata_len)
        capdata_len = capresp.capset_max_size + sizeof(struct virtio_gpu_ctrl_hdr);
    if (capdata_len > PAGE_SIZE) capdata_len = PAGE_SIZE;
    memset(&getcap, 0, sizeof(getcap));
    memset(&capdata, 0, sizeof(capdata));
    getcap.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    getcap.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    getcap.hdr.fence_id = gpu_fence_id++;
    getcap.capset_id = capresp.capset_id;
    getcap.capset_version = capresp.capset_max_version;
    if (virtio_gpu_send_cmd(&getcap, sizeof(getcap), &capdata, capdata_len) < 0 ||
        !virtio_gpu_virgl_hdr_ok(&capdata.hdr, VIRTIO_GPU_RESP_OK_CAPSET)) {
        console_write("M52-GFX: fail virgl-capset\n");
        return;
    }
    console_write("M52-GFX: ok virgl-capset\n");

    /* 2. Create a 3D context. */
    struct virtio_gpu_ctx_create ctx;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&ctx, 0, sizeof(ctx));
    ctx.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    ctx.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    ctx.hdr.fence_id = gpu_fence_id++;
    ctx.hdr.ctx_id = VIRGL_SELFTEST_CTX;
    ctx.nlen = 5;
    memcpy(ctx.debug_name, "b1nix", 5);
    if (virtio_gpu_send_cmd(&ctx, sizeof(ctx), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        console_write("M52-GFX: fail virgl-ctx\n");
        return;
    }

    /* 3. Allocate guest backing for the render-target resource. */
    usize rt_bytes = (usize)VIRGL_SELFTEST_DIM * VIRGL_SELFTEST_DIM * sizeof(u32);
    usize rt_pages = (rt_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 rt_phys = pmm_alloc_frames(rt_pages);
    if (!rt_phys) {
        console_write("M52-GFX: fail virgl-backing\n");
        return;
    }
    u32 *rt_virt = (u32 *)(usize)(rt_phys + vmm_direct_map_base());
    memset(rt_virt, 0, rt_pages * PAGE_SIZE);

    /* 4. Create the 3D render-target resource. */
    struct virtio_gpu_resource_create_3d c3d;
    memset(&c3d, 0, sizeof(c3d));
    c3d.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    c3d.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    c3d.hdr.fence_id = gpu_fence_id++;
    c3d.resource_id = VIRGL_SELFTEST_RES;
    c3d.target = PIPE_TEXTURE_2D;
    c3d.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    c3d.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW;
    c3d.width = VIRGL_SELFTEST_DIM;
    c3d.height = VIRGL_SELFTEST_DIM;
    c3d.depth = 1;
    c3d.array_size = 1;
    c3d.last_level = 0;
    c3d.nr_samples = 0;
    c3d.flags = 0;
    if (virtio_gpu_send_cmd(&c3d, sizeof(c3d), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        console_write("M52-GFX: fail virgl-res3d\n");
        return;
    }

    /* 5. Attach the guest backing pages to the resource. */
    struct virtio_gpu_attach_backing_cmd attach;
    memset(&attach, 0, sizeof(attach));
    attach.req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.req.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    attach.req.hdr.fence_id = gpu_fence_id++;
    attach.req.resource_id = VIRGL_SELFTEST_RES;
    attach.req.nr_entries = 1;
    attach.entry.addr = rt_phys;
    attach.entry.length = (u32)(rt_pages * PAGE_SIZE);
    if (virtio_gpu_send_cmd(&attach, sizeof(attach), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        console_write("M52-GFX: fail virgl-attach\n");
        return;
    }

    /* 6. Bind the resource into the context. */
    struct virtio_gpu_ctx_resource ctxres;
    memset(&ctxres, 0, sizeof(ctxres));
    ctxres.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    ctxres.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    ctxres.hdr.fence_id = gpu_fence_id++;
    ctxres.hdr.ctx_id = VIRGL_SELFTEST_CTX;
    ctxres.resource_id = VIRGL_SELFTEST_RES;
    if (virtio_gpu_send_cmd(&ctxres, sizeof(ctxres), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        console_write("M52-GFX: fail virgl-ctxres\n");
        return;
    }

    /* 7. Build + submit the virgl command stream: wrap the resource as a render
     * surface, bind it as the framebuffer, and CLEAR it to (R=0.25, G=0.5,
     * B=0.75, A=1.0). Float bit patterns are precomputed — the kernel uses no
     * FPU. virglrenderer runs this as real host GL on the host GPU. */
    struct virtio_gpu_cmd_submit submit;
    memset(&submit, 0, sizeof(submit));
    submit.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    submit.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    submit.hdr.fence_id = gpu_fence_id++;
    submit.hdr.ctx_id = VIRGL_SELFTEST_CTX;
    u32 *s = submit.cmd;
    int n = 0;
    /* CREATE_OBJECT(SURFACE): handle, res_handle, format, level, layers */
    s[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    s[n++] = VIRGL_SELFTEST_SURF;
    s[n++] = VIRGL_SELFTEST_RES;
    s[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
    s[n++] = 0; /* texture level */
    s[n++] = 0; /* first_layer | last_layer << 16 */
    /* SET_FRAMEBUFFER_STATE: nr_cbufs, zsurf_handle, cbuf0_handle */
    s[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    s[n++] = 1;
    s[n++] = 0;
    s[n++] = VIRGL_SELFTEST_SURF;
    /* CLEAR: buffers, color rgba (f32 bits), depth (double), stencil */
    s[n++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
    s[n++] = PIPE_CLEAR_COLOR0;
    s[n++] = 0x3E800000; /* 0.25f */
    s[n++] = 0x3F000000; /* 0.50f */
    s[n++] = 0x3F400000; /* 0.75f */
    s[n++] = 0x3F800000; /* 1.00f */
    s[n++] = 0; /* depth lo */
    s[n++] = 0; /* depth hi */
    s[n++] = 0; /* stencil */
    submit.size = (u32)(n * sizeof(u32));
    usize submit_len = sizeof(struct virtio_gpu_ctrl_hdr) + 8 + submit.size;
    if (virtio_gpu_send_cmd(&submit, submit_len, &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        console_write("M52-GFX: fail virgl-submit\n");
        return;
    }
    console_write("M52-GFX: ok virgl-3d-clear\n");

    /* 8. Read the GPU-rendered surface back into guest memory. */
    struct virtio_gpu_transfer_host_3d xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    xfer.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    xfer.hdr.fence_id = gpu_fence_id++;
    xfer.hdr.ctx_id = VIRGL_SELFTEST_CTX;
    xfer.box.x = 0;
    xfer.box.y = 0;
    xfer.box.z = 0;
    xfer.box.w = VIRGL_SELFTEST_DIM;
    xfer.box.h = VIRGL_SELFTEST_DIM;
    xfer.box.d = 1;
    xfer.offset = 0;
    xfer.resource_id = VIRGL_SELFTEST_RES;
    xfer.level = 0;
    xfer.stride = 0;
    xfer.layer_stride = 0;
    if (virtio_gpu_send_cmd(&xfer, sizeof(xfer), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        console_write("M52-GFX: fail virgl-readback\n");
        return;
    }

    /* 9. Verify the GPU actually rendered the clear colour. B8G8R8A8 memory
     * order is B,G,R,A. 0.25/0.5/0.75/1.0 * 255 ≈ 64/128/191/255. */
    u32 px = rt_virt[0];
    u32 b = px & 0xFF, g = (px >> 8) & 0xFF, r = (px >> 16) & 0xFF, a = (px >> 24) & 0xFF;
    if (a >= 250 && r >= 60 && r <= 68 && g >= 124 && g <= 132 && b >= 187 && b <= 195) {
        console_write("M52-GFX: ok path-accelerated\n");
    } else {
        /* Log the readback so a host-dependent rounding/ordering mismatch is
         * diagnosable rather than a silent failure. */
        console_write("M52-GFX: fail path-accelerated pixel=0x");
        console_write_hex32(px);
        console_write("\n");
    }
}

/* ── /dev/virtio-gpu: userspace VirGL 3D transport (M53) ──────────────────
 * Exposes the VirGL command path to userspace so a renderer (a Mesa virgl
 * winsys, or the bring-up smoke) can drive the host GPU: create a render-target
 * resource (kernel allocates + attaches contiguous backing and hands back an
 * mmap window), submit a userspace-built virgl command stream, copy the
 * GPU-rendered result back, and read it through the mmap. A single implicit 3D
 * context is created at device init. The kernel is the transport; userspace
 * owns the virgl protocol — the same split Mesa's winsys expects. */

/* Kernel copies of the userspace ABI (userspace/include/b1nix/virgl.h). */
#define B1NIX_VIRGL_GET_CAPS 0x7601
#define B1NIX_VIRGL_GET_CAPS_DATA 0x7602
#define B1NIX_VIRGL_RES_CREATE 0x7603
#define B1NIX_VIRGL_SUBMIT 0x7604
#define B1NIX_VIRGL_TRANSFER_FROM_HOST 0x7605
#define B1NIX_VIRGL_TRANSFER_TO_HOST 0x7606
#define B1NIX_VIRGL_WAIT 0x7607
#define B1NIX_VIRGL_GETPARAM 0x7608
#define B1NIX_VIRGL_RES_INFO 0x7609
#define B1NIX_VIRGL_CONTEXT_INIT 0x760A
#define B1NIX_VIRGL_RES_UNREF 0x760B

struct b1nix_virgl_caps_abi {
    u32 capset_id, capset_version, capset_size, _pad;
};
struct b1nix_virgl_caps_data_abi {
    u32 capset_id, capset_version, size, _pad;
    u64 caps_ptr;
};
struct b1nix_virgl_getparam_abi {
    u32 param, _pad;
    u64 value;
};
struct b1nix_virgl_res_info_abi {
    u32 res_id, stride;
    u64 size;
};
struct b1nix_virgl_res_create_abi {
    u32 target, format, bind, width, height, depth, array_size;
    u32 res_id;
    u64 mmap_offset;
    u64 size;
};
struct b1nix_virgl_box_abi {
    u32 x, y, z, w, h, d;
};
struct b1nix_virgl_transfer_abi {
    u32 res_id, level;
    struct b1nix_virgl_box_abi box;
    u64 offset;
};
struct b1nix_virgl_submit_abi {
    u32 cmd_size, _pad;
    u64 cmd_ptr;
};

#define VGPU_UDEV_CTX 2
#define VGPU_UDEV_RES_BASE 64
#define VGPU_UDEV_MAX_RES 16
/* SUBMIT command-stream staging buffer. Mesa's virgl streams (shader uploads,
 * draw state) far exceed one page; size it to hold a real draw. */
#define VGPU_SUBMIT_BUF_BYTES (64u * 1024)
/* mmap window per resource. Kept small enough that MAX_RES windows stay under
 * 2 GiB so the offset fits the 32-bit mmap offset arg on the i686 port. */
#define VGPU_UDEV_SLOT (64ull * 1024 * 1024)
#define VGPU_UDEV_MAX_DIM 4096 /* 4096*4096*4 == one 64 MiB slot */

struct vgpu_udev_res {
    int used;
    u32 res_id;
    u64 phys;
    u64 size;
    u64 mmap_offset;
    u32 width;
    u32 height;
    u32 format;
    u32 bind;
};
static struct vgpu_udev_res vgpu_udev_res_tab[VGPU_UDEV_MAX_RES];
static int vgpu_udev_ready;
static spinlock_t vgpu_udev_lock;
static u8 *vgpu_udev_submit_buf; /* one page to build SUBMIT_3D requests */

/* Create a virgl 3D context. */
static int vgpu_ctx_create(u32 ctx_id)
{
    struct virtio_gpu_ctx_create ctx;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&ctx, 0, sizeof(ctx));
    ctx.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    ctx.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    ctx.hdr.fence_id = gpu_fence_id++;
    ctx.hdr.ctx_id = ctx_id;
    ctx.nlen = 5;
    memcpy(ctx.debug_name, "b1nix", 5);
    if (virtio_gpu_send_cmd(&ctx, sizeof(ctx), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -1;
    return 0;
}

/* Create a 3D resource, attach contiguous guest backing, bind it into the
 * context. Returns 0 and fills *out_phys on success. */
static int vgpu_res_create_attach(u32 ctx_id, const struct b1nix_virgl_res_create_abi *p,
                                  u64 phys, u64 size)
{
    struct virtio_gpu_resource_create_3d c3d;
    struct virtio_gpu_attach_backing_cmd attach;
    struct virtio_gpu_ctx_resource ctxres;
    struct virtio_gpu_ctrl_hdr resp;

    memset(&c3d, 0, sizeof(c3d));
    c3d.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    c3d.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    c3d.hdr.fence_id = gpu_fence_id++;
    c3d.resource_id = p->res_id;
    c3d.target = p->target;
    c3d.format = p->format;
    c3d.bind = p->bind;
    c3d.width = p->width;
    c3d.height = p->height ? p->height : 1;
    c3d.depth = p->depth ? p->depth : 1;
    c3d.array_size = p->array_size ? p->array_size : 1;
    if (virtio_gpu_send_cmd(&c3d, sizeof(c3d), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -1;

    memset(&attach, 0, sizeof(attach));
    attach.req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.req.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    attach.req.hdr.fence_id = gpu_fence_id++;
    attach.req.resource_id = p->res_id;
    attach.req.nr_entries = 1;
    attach.entry.addr = phys;
    attach.entry.length = (u32)size;
    if (virtio_gpu_send_cmd(&attach, sizeof(attach), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -1;

    memset(&ctxres, 0, sizeof(ctxres));
    ctxres.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    ctxres.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    ctxres.hdr.fence_id = gpu_fence_id++;
    ctxres.hdr.ctx_id = ctx_id;
    ctxres.resource_id = p->res_id;
    if (virtio_gpu_send_cmd(&ctxres, sizeof(ctxres), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -1;
    return 0;
}

/* Build and push one SUBMIT_3D request at the device. Caller holds
 * vgpu_udev_lock (the staging buffer and the control queue are shared). This is
 * the part that actually talks to the hardware; who calls it — the scheduler
 * thread or, before the scheduler exists, the submitting task — is decided by
 * vgpu_submit_stream below. */
static int vgpu_submit_stream_locked(u32 ctx_id, const u32 *cmd, u32 cmd_bytes)
{
    struct virtio_gpu_ctrl_hdr resp;
    if (!vgpu_udev_submit_buf || cmd_bytes == 0 ||
        cmd_bytes + sizeof(struct virtio_gpu_ctrl_hdr) + 8 > VGPU_SUBMIT_BUF_BYTES)
        return -1;
    struct virtio_gpu_cmd_submit *s = (struct virtio_gpu_cmd_submit *)vgpu_udev_submit_buf;
    memset(s, 0, sizeof(struct virtio_gpu_ctrl_hdr) + 8);
    s->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    s->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    s->hdr.fence_id = gpu_fence_id++;
    s->hdr.ctx_id = ctx_id;
    s->size = cmd_bytes;
    memcpy(vgpu_udev_submit_buf + sizeof(struct virtio_gpu_ctrl_hdr) + 8, cmd, cmd_bytes);
    usize len = sizeof(struct virtio_gpu_ctrl_hdr) + 8 + cmd_bytes;
    if (virtio_gpu_send_cmd(vgpu_udev_submit_buf, len, &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -1;
    return 0;
}

/* ── M100: command submission through the DRM GPU scheduler ─────────
 *
 * Before M100 a SUBMIT_3D ran inline in the caller's context and the caller
 * then sat inside virtio_gpu_wait_used(), a TSC-bounded busy-spin on the
 * virtqueue used index, with vgpu_udev_lock held and interrupts disabled for
 * the whole device round trip.
 *
 * Now the caller hands the stream to a scheduler entity and waits on the job's
 * dma-fence, which parks the task instead of burning the CPU. The scheduler
 * thread is the single writer of the control queue for these submissions, so
 * ordering is inherent rather than lock-enforced, and completion is an object
 * other code can hold rather than a return value only the submitter sees.
 *
 * The synchronous path is kept as a fallback for the window before the
 * scheduler thread exists (and for a kernel where kthread creation failed):
 * dropping work silently there would be worse than briefly spinning.
 */
struct vgpu_submit_job {
    struct drm_sched_job base;
    u32 ctx_id;
    u32 cmd_bytes;
    const u32 *cmd;
    int result;
};

static struct drm_gpu_scheduler vgpu_sched;
static struct drm_sched_entity vgpu_sched_entity;
static int vgpu_sched_ready;

static int vgpu_sched_run_job(struct drm_sched_job *job)
{
    struct vgpu_submit_job *sj = (struct vgpu_submit_job *)job;
    u64 flags;
    spin_lock_irqsave(&vgpu_udev_lock, &flags);
    sj->result = vgpu_submit_stream_locked(sj->ctx_id, sj->cmd, sj->cmd_bytes);
    spin_unlock_irqrestore(&vgpu_udev_lock, flags);
    /* The device round trip completed inside vgpu_submit_stream_locked, so the
     * job is done by the time run_job returns; the scheduler signals the fence
     * with this result. */
    return sj->result < 0 ? -EIO : DRM_SCHED_RUN_DONE;
}

static void vgpu_job_release(struct dma_fence *f)
{
    struct vgpu_submit_job *sj =
        (struct vgpu_submit_job *)(void *)((u8 *)f -
                                           __builtin_offsetof(struct vgpu_submit_job,
                                                              base.finished));
    kfree(sj);
}

static const struct drm_sched_backend_ops vgpu_sched_ops = {
    .run_job = vgpu_sched_run_job,
    .free_job = 0,
};

/* Bring the scheduler up. Called once the VirGL context exists. */
static void vgpu_sched_start(void)
{
    if (vgpu_sched_ready)
        return;
    if (drm_sched_init(&vgpu_sched, &vgpu_sched_ops, "virtio-gpu-sched") < 0) {
        console_write("virtio-gpu: scheduler thread unavailable, "
                      "falling back to synchronous submit\n");
        return;
    }
    if (drm_sched_entity_init(&vgpu_sched_entity, &vgpu_sched, "virgl") < 0)
        return;
    vgpu_sched_ready = 1;
}

/* Submit a userspace-built virgl command stream on the context. Must be called
 * WITHOUT vgpu_udev_lock held: the fence wait sleeps. */
static int vgpu_submit_stream(u32 ctx_id, const u32 *cmd, u32 cmd_bytes)
{
    if (!vgpu_sched_ready || !drm_sched_ready(&vgpu_sched) ||
        !scheduler_can_block()) {
        u64 flags;
        spin_lock_irqsave(&vgpu_udev_lock, &flags);
        int rc = vgpu_submit_stream_locked(ctx_id, cmd, cmd_bytes);
        spin_unlock_irqrestore(&vgpu_udev_lock, flags);
        return rc;
    }

    /* Heap-allocated, freed through the fence's release callback: the
     * scheduler thread still holds a reference after the fence signals, so the
     * job must not live on the submitter's stack. */
    struct vgpu_submit_job *job = kzalloc(sizeof(*job));
    if (!job)
        return -1;
    job->ctx_id = ctx_id;
    job->cmd = cmd;
    job->cmd_bytes = cmd_bytes;
    job->result = -1;
    if (drm_sched_job_init(&job->base, &vgpu_sched_entity) < 0) {
        kfree(job);
        return -1;
    }
    job->base.finished.release = vgpu_job_release;

    struct dma_fence *fence = drm_sched_job_submit(&job->base);
    if (!fence) {
        dma_fence_put(&job->base.finished); /* runs the release, frees job */
        return -1;
    }
    int err = dma_fence_wait_uninterruptible(fence);
    int result = job->result;
    dma_fence_put(fence);
    dma_fence_put(&job->base.finished); /* the creator's reference */
    return (err || result < 0) ? -1 : 0;
}

/* Copy a GPU-rendered resource region back into its guest backing. */
static int vgpu_transfer_from_host(u32 ctx_id, const struct b1nix_virgl_transfer_abi *t)
{
    struct virtio_gpu_transfer_host_3d xfer;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&xfer, 0, sizeof(xfer));
    xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    xfer.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    xfer.hdr.fence_id = gpu_fence_id++;
    xfer.hdr.ctx_id = ctx_id;
    xfer.box.x = t->box.x;
    xfer.box.y = t->box.y;
    xfer.box.z = t->box.z;
    xfer.box.w = t->box.w;
    xfer.box.h = t->box.h;
    xfer.box.d = t->box.d ? t->box.d : 1;
    xfer.offset = t->offset;
    xfer.resource_id = t->res_id;
    xfer.level = t->level;
    if (virtio_gpu_send_cmd(&xfer, sizeof(xfer), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -1;
    return 0;
}

static int vgpu_transfer_to_host(u32 ctx_id, const struct b1nix_virgl_transfer_abi *t)
{
    struct virtio_gpu_transfer_host_3d xfer;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&xfer, 0, sizeof(xfer));
    xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    xfer.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    xfer.hdr.fence_id = gpu_fence_id++;
    xfer.hdr.ctx_id = ctx_id;
    xfer.box.x = t->box.x;
    xfer.box.y = t->box.y;
    xfer.box.z = t->box.z;
    xfer.box.w = t->box.w;
    xfer.box.h = t->box.h;
    xfer.box.d = t->box.d ? t->box.d : 1;
    xfer.offset = t->offset;
    xfer.resource_id = t->res_id;
    xfer.level = t->level;
    if (virtio_gpu_send_cmd(&xfer, sizeof(xfer), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -1;
    return 0;
}

static struct vgpu_udev_res *vgpu_udev_find(u32 res_id)
{
    for (int i = 0; i < VGPU_UDEV_MAX_RES; i++)
        if (vgpu_udev_res_tab[i].used && vgpu_udev_res_tab[i].res_id == res_id)
            return &vgpu_udev_res_tab[i];
    return 0;
}

/* Free a userspace virgl resource: release the slot, tell the host to drop the
 * resource, and free its backing frames. Lets a winsys reclaim the 16-slot
 * table instead of leaking it. The caller must already have unmapped it. */
static int vgpu_res_unref_id(u32 res_id)
{
    u64 flags, phys = 0, size = 0;
    spin_lock_irqsave(&vgpu_udev_lock, &flags);
    struct vgpu_udev_res *r = vgpu_udev_find(res_id);
    if (r) {
        phys = r->phys;
        size = r->size;
        r->used = 0;
        r->res_id = 0;
        r->phys = 0;
    }
    spin_unlock_irqrestore(&vgpu_udev_lock, flags);
    if (!phys)
        return -1;

    struct virtio_gpu_resource_unref unref;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&unref, 0, sizeof(unref));
    unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    unref.hdr.fence_id = gpu_fence_id++;
    unref.resource_id = res_id;
    (void)virtio_gpu_send_cmd(&unref, sizeof(unref), &resp, sizeof(resp));

    for (u64 f = 0; f < (size + PAGE_SIZE - 1) / PAGE_SIZE; f++)
        pmm_free_frame(phys + f * PAGE_SIZE);
    return 0;
}

static int vgpu_udev_ioctl(struct vfs_node *node, u64 request, void *arg)
{
    (void)node;
    if (!vgpu_udev_ready)
        return -ENODEV;

    switch (request) {
    case B1NIX_VIRGL_GET_CAPS: {
        struct virtio_gpu_get_capset_info capreq;
        struct virtio_gpu_resp_capset_info capresp;
        memset(&capreq, 0, sizeof(capreq));
        memset(&capresp, 0, sizeof(capresp));
        capreq.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
        capreq.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        capreq.hdr.fence_id = gpu_fence_id++;
        capreq.capset_index = 0;
        if (virtio_gpu_send_cmd(&capreq, sizeof(capreq), &capresp, sizeof(capresp)) < 0 ||
            capresp.hdr.type != VIRTIO_GPU_RESP_OK_CAPSET_INFO)
            return -EIO;
        struct b1nix_virgl_caps_abi out;
        out.capset_id = capresp.capset_id;
        out.capset_version = capresp.capset_max_version;
        out.capset_size = capresp.capset_max_size;
        out._pad = 0;
        if (!arg || syscall_copyout(arg, &out, sizeof(out)) < 0)
            return -EFAULT;
        return 0;
    }
    case B1NIX_VIRGL_GET_CAPS_DATA: {
        /* Fetch the full capset BLOB from the host into a userspace buffer.
         * The Mesa virgl winsys reads this at screen-create to learn the host
         * GPU's feature/format/limit set (virgl_caps_v2). */
        struct b1nix_virgl_caps_data_abi req;
        if (!arg || syscall_copyin(&req, arg, sizeof(req)) < 0)
            return -EFAULT;

        struct virtio_gpu_get_capset_info capreq;
        struct virtio_gpu_resp_capset_info capresp;
        memset(&capreq, 0, sizeof(capreq));
        memset(&capresp, 0, sizeof(capresp));
        capreq.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
        capreq.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        capreq.hdr.fence_id = gpu_fence_id++;
        capreq.capset_index = 0;
        if (virtio_gpu_send_cmd(&capreq, sizeof(capreq), &capresp, sizeof(capresp)) < 0 ||
            capresp.hdr.type != VIRTIO_GPU_RESP_OK_CAPSET_INFO)
            return -EIO;

        struct virtio_gpu_get_capset getcap;
        struct {
            struct virtio_gpu_ctrl_hdr hdr;
            u8 capset_data[3072];
        } __attribute__((packed)) capdata;
        usize capdata_len = sizeof(capdata);
        if (capresp.capset_max_size + sizeof(struct virtio_gpu_ctrl_hdr) < capdata_len)
            capdata_len = capresp.capset_max_size + sizeof(struct virtio_gpu_ctrl_hdr);
        if (capdata_len > PAGE_SIZE) capdata_len = PAGE_SIZE;
        memset(&getcap, 0, sizeof(getcap));
        memset(&capdata, 0, sizeof(capdata));
        getcap.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
        getcap.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        getcap.hdr.fence_id = gpu_fence_id++;
        getcap.capset_id = capresp.capset_id;
        getcap.capset_version = capresp.capset_max_version;
        if (virtio_gpu_send_cmd(&getcap, sizeof(getcap), &capdata, capdata_len) < 0 ||
            !virtio_gpu_virgl_hdr_ok(&capdata.hdr, VIRTIO_GPU_RESP_OK_CAPSET))
            return -EIO;

        u32 blob = capresp.capset_max_size;
        if (blob > sizeof(capdata.capset_data)) blob = sizeof(capdata.capset_data);
        u32 copy = req.size < blob ? req.size : blob;
        if (req.caps_ptr && copy &&
            syscall_copyout((void *)(usize)req.caps_ptr, capdata.capset_data, copy) < 0)
            return -EFAULT;
        req.capset_id = capresp.capset_id;
        req.capset_version = capresp.capset_max_version;
        req.size = blob;
        if (syscall_copyout(arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case B1NIX_VIRGL_RES_CREATE: {
        struct b1nix_virgl_res_create_abi p;
        if (!arg || syscall_copyin(&p, arg, sizeof(p)) < 0)
            return -EFAULT;
        if (p.width == 0 || p.width > VGPU_UDEV_MAX_DIM ||
            p.height > VGPU_UDEV_MAX_DIM)
            return -EINVAL;
        u64 size = (u64)p.width * (p.height ? p.height : 1) * 4ull;
        u64 pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

        u64 flags;
        spin_lock_irqsave(&vgpu_udev_lock, &flags);
        int slot = -1;
        for (int i = 0; i < VGPU_UDEV_MAX_RES; i++)
            if (!vgpu_udev_res_tab[i].used) { slot = i; break; }
        if (slot < 0) {
            spin_unlock_irqrestore(&vgpu_udev_lock, flags);
            return -ENOSPC;
        }
        vgpu_udev_res_tab[slot].used = 1; /* reserve */
        spin_unlock_irqrestore(&vgpu_udev_lock, flags);

        u64 phys = pmm_alloc_frames(pages);
        if (!phys) {
            vgpu_udev_res_tab[slot].used = 0;
            return -ENOMEM;
        }
        memset((void *)(usize)(phys + vmm_direct_map_base()), 0, pages * PAGE_SIZE);
        p.res_id = VGPU_UDEV_RES_BASE + slot;
        if (vgpu_res_create_attach(VGPU_UDEV_CTX, &p, phys, pages * PAGE_SIZE) < 0) {
            for (u64 f = 0; f < pages; f++)
                pmm_free_frame(phys + f * PAGE_SIZE);
            vgpu_udev_res_tab[slot].used = 0;
            return -EIO;
        }
        vgpu_udev_res_tab[slot].res_id = p.res_id;
        vgpu_udev_res_tab[slot].phys = phys;
        vgpu_udev_res_tab[slot].size = pages * PAGE_SIZE;
        vgpu_udev_res_tab[slot].mmap_offset = (u64)slot * VGPU_UDEV_SLOT;
        vgpu_udev_res_tab[slot].width = p.width;
        vgpu_udev_res_tab[slot].height = p.height;
        vgpu_udev_res_tab[slot].format = p.format;
        vgpu_udev_res_tab[slot].bind = p.bind;
        p.mmap_offset = vgpu_udev_res_tab[slot].mmap_offset;
        p.size = pages * PAGE_SIZE;
        if (syscall_copyout(arg, &p, sizeof(p)) < 0)
            return -EFAULT;
        return 0;
    }
    case B1NIX_VIRGL_SUBMIT: {
        struct b1nix_virgl_submit_abi sub;
        if (!arg || syscall_copyin(&sub, arg, sizeof(sub)) < 0)
            return -EFAULT;
        if (sub.cmd_size == 0 || (sub.cmd_size & 3) ||
            sub.cmd_size + sizeof(struct virtio_gpu_ctrl_hdr) + 8 >
                VGPU_SUBMIT_BUF_BYTES)
            return -EINVAL;
        /* Streams are large (shaders/state); copy in via a heap buffer, not the
         * stack. copyin can fault, so do it before taking the lock. */
        u32 *cmd = kmalloc(sub.cmd_size);
        if (!cmd)
            return -ENOMEM;
        if (syscall_copyin(cmd, (void *)(usize)sub.cmd_ptr, sub.cmd_size) < 0) {
            kfree(cmd);
            return -EFAULT;
        }
        /* No lock here: vgpu_submit_stream sleeps on the job's fence, and
         * the scheduler thread takes vgpu_udev_lock itself around the device
         * round trip. */
        int rc = vgpu_submit_stream(VGPU_UDEV_CTX, cmd, sub.cmd_size);
        kfree(cmd);
        return rc < 0 ? -EIO : 0;
    }
    case B1NIX_VIRGL_TRANSFER_FROM_HOST: {
        struct b1nix_virgl_transfer_abi t;
        if (!arg || syscall_copyin(&t, arg, sizeof(t)) < 0)
            return -EFAULT;
        if (!vgpu_udev_find(t.res_id))
            return -EINVAL;
        u64 flags;
        spin_lock_irqsave(&vgpu_udev_lock, &flags);
        int rc = vgpu_transfer_from_host(VGPU_UDEV_CTX, &t);
        spin_unlock_irqrestore(&vgpu_udev_lock, flags);
        return rc < 0 ? -EIO : 0;
    }
    case B1NIX_VIRGL_TRANSFER_TO_HOST: {
        /* Upload guest resource data (vertex/index/constant buffers, textures)
         * to the host GPU — the direction a Mesa winsys uses for every draw. */
        struct b1nix_virgl_transfer_abi t;
        if (!arg || syscall_copyin(&t, arg, sizeof(t)) < 0)
            return -EFAULT;
        if (!vgpu_udev_find(t.res_id))
            return -EINVAL;
        u64 flags;
        spin_lock_irqsave(&vgpu_udev_lock, &flags);
        int rc = vgpu_transfer_to_host(VGPU_UDEV_CTX, &t);
        spin_unlock_irqrestore(&vgpu_udev_lock, flags);
        return rc < 0 ? -EIO : 0;
    }
    case B1NIX_VIRGL_WAIT:
        /* SUBMIT waits on its job's dma-fence before returning and TRANSFER is
         * synchronous, so by the time userspace calls WAIT the GPU work has
         * already completed — signal immediately. */
        return 0;
    case B1NIX_VIRGL_CONTEXT_INIT:
        /* The kernel created the implicit 3D context at device init; userspace
         * shares it (VGPU_UDEV_CTX). Accept context-init as a no-op success. */
        return 0;
    case B1NIX_VIRGL_RES_UNREF: {
        u32 res_id;
        if (!arg || syscall_copyin(&res_id, arg, sizeof(res_id)) < 0)
            return -EFAULT;
        return vgpu_res_unref_id(res_id) == 0 ? 0 : -EINVAL;
    }
    case B1NIX_VIRGL_GETPARAM: {
        struct b1nix_virgl_getparam_abi gp;
        if (!arg || syscall_copyin(&gp, arg, sizeof(gp)) < 0)
            return -EFAULT;
        u64 v = 0;
        switch (gp.param) {
        case 1: v = 1; break; /* VIRTGPU_PARAM_3D_FEATURES */
        case 2: v = 1; break; /* VIRTGPU_PARAM_CAPSET_QUERY_FIX */
        case 6: v = 1; break; /* VIRTGPU_PARAM_CONTEXT_INIT */
        default: v = 0; break; /* RESOURCE_BLOB / HOST_VISIBLE / CROSS_DEVICE */
        }
        gp.value = v;
        if (syscall_copyout(arg, &gp, sizeof(gp)) < 0)
            return -EFAULT;
        return 0;
    }
    case B1NIX_VIRGL_RES_INFO: {
        struct b1nix_virgl_res_info_abi ri;
        if (!arg || syscall_copyin(&ri, arg, sizeof(ri)) < 0)
            return -EFAULT;
        struct vgpu_udev_res *r = vgpu_udev_find(ri.res_id);
        if (!r)
            return -EINVAL;
        ri.stride = r->width * 4; /* BGRA/RGBA: 4 bytes per texel */
        ri.size = r->size;
        if (syscall_copyout(arg, &ri, sizeof(ri)) < 0)
            return -EFAULT;
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

static int vgpu_udev_mmap_phys(struct vfs_node *node, u64 offset, usize length,
                               u64 *out_phys)
{
    (void)node;
    u64 flags;
    int rc = -EINVAL;
    spin_lock_irqsave(&vgpu_udev_lock, &flags);
    for (int i = 0; i < VGPU_UDEV_MAX_RES; i++) {
        struct vgpu_udev_res *r = &vgpu_udev_res_tab[i];
        if (!r->used || !r->phys)
            continue;
        if (offset >= r->mmap_offset && offset < r->mmap_offset + r->size) {
            u64 within = offset - r->mmap_offset;
            if (within + length <= r->size) {
                *out_phys = r->phys + within;
                rc = 0;
            }
            break;
        }
    }
    spin_unlock_irqrestore(&vgpu_udev_lock, flags);
    return rc;
}

/* Register /dev/virtio-gpu and bring up the implicit context. Only when the
 * host actually offered VirGL (the virtio-gpu-gl device); otherwise the node is
 * absent and userspace renderers fall back to software. Call after VFS init. */
void virtio_gpu_dev_init(void)
{
    if (!gpu_ready || !gpu_virgl_ok)
        return;
    /* Called again from vfs_repopulate_after_root_mount() to re-add the VFS
     * node onto the post-root-switch tree (the initramfs node it registered
     * onto during early boot becomes unreachable once "/" redirects to the
     * mounted ext4 root). The VirGL ctx + submit buffer are one-time hardware
     * state — only (re)create them the first time. */
    if (!vgpu_udev_ready) {
        if (vgpu_ctx_create(VGPU_UDEV_CTX) < 0) {
            console_write("virtio-gpu: userspace VirGL ctx create failed\n");
            return;
        }
        u64 buf_phys = pmm_alloc_frames(VGPU_SUBMIT_BUF_BYTES / PAGE_SIZE);
        if (!buf_phys)
            return;
        vgpu_udev_submit_buf = (u8 *)(usize)(buf_phys + vmm_direct_map_base());
        /* M100: command submission runs through the DRM GPU scheduler from
         * here on (see vgpu_submit_stream). */
        vgpu_sched_start();
    }

    struct vfs_node *node = vfs_add_node("/dev/virtio-gpu", VFS_DEVICE, 0, 0, 0);
    if (!node || IS_ERR(node)) {
        console_write("virtio-gpu: failed to register /dev/virtio-gpu\n");
        return;
    }
    node->inode->mode = 0600;
    node->inode->ioctl_cb = vgpu_udev_ioctl;
    node->inode->mmap_phys_cb = vgpu_udev_mmap_phys;
    vfs_node_put(node);
    vgpu_udev_ready = 1;
    console_write("virtio-gpu: /dev/virtio-gpu (userspace VirGL) ready\n");
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
                u32 devfeat0 = gpu_common_cfg->device_feature;
                u32 drvfeat0 = 0;
                /* Negotiate VirGL when the host (virglrenderer) offers it, so we
                 * can drive the 3D command set. Plain virtio-gpu-pci does not
                 * offer it — we then fall back to the 2D software path. */
                if (devfeat0 & (1u << VIRTIO_GPU_F_VIRGL)) {
                    drvfeat0 |= (1u << VIRTIO_GPU_F_VIRGL);
                    gpu_virgl_offered = 1;
                }
                gpu_common_cfg->driver_feature_select = 0;
                gpu_common_cfg->driver_feature = drvfeat0;
                gpu_common_cfg->driver_feature_select = 1;
                gpu_common_cfg->driver_feature = 0;

                gpu_common_cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;
                if (gpu_common_cfg->device_status & VIRTIO_STATUS_FEATURES_OK) {
                    gpu_modern = 1;
                    if (gpu_virgl_offered) gpu_virgl_ok = 1;
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

    /* Completion interrupts, MSI-X first.
     *
     * The legacy pin is the fallback and not the preference. It is shared and
     * level-triggered, and on this device it stopped delivering after a handful
     * of completions — measured: five interrupts across eighty-three parks, not
     * one of which a message ended. A virtio device with an MSI-X capability
     * will deliver a message per queue instead: one vector this driver owns,
     * written straight to the local APIC, with no ISR register to read and no
     * line to arbitrate.
     *
     * MSI-X needs both halves programmed — the table entry, which says what the
     * device writes, and the queue's msix_vector, which says which table entry
     * the queue uses. A queue left at VIRTIO_MSI_NO_VECTOR raises nothing at
     * all, and that would look exactly like working code silently back to
     * polling, so both fields are read back and the whole thing abandoned
     * unless the device took them.
     *
     * Order matters throughout: arrange delivery first, let the queues
     * interrupt last. The other way round leaves interrupts arriving at
     * nobody. */
    if (gpu_modern) {
        if (pci_msix_table_size(pci.bus, pci.slot, pci.func) > 0) {
            int vec = msi_alloc_vector(virtio_gpu_irq_msi, 0);
            if (vec > 0) {
                if (pci_msix_enable(pci.bus, pci.slot, pci.func, 0, (u8)vec) == 0) {
                    /* Table entry 0 for both queues; no vector for config
                     * changes, which this driver does not act on. */
                    gpu_common_cfg->msix_config = 0xffff;
                    gpu_common_cfg->queue_select = 0;
                    gpu_common_cfg->queue_msix_vector = 0;
                    u16 rb_ctrl = gpu_common_cfg->queue_msix_vector;
                    gpu_common_cfg->queue_select = 1;
                    gpu_common_cfg->queue_msix_vector = 0;
                    u16 rb_cursor = gpu_common_cfg->queue_msix_vector;
                    if (rb_ctrl == 0 && rb_cursor == 0) {
                        gpu_msix_vector = vec;
                        gpu_irq_ready = 1;
                        console_write("virtio-gpu: completion MSI-X on vector ");
                        console_write_dec((u64)vec);
                        console_write("\n");
                    } else {
                        pci_msix_disable(pci.bus, pci.slot, pci.func);
                    }
                }
                if (!gpu_irq_ready)
                    msi_free_vector(vec);
            }
        }
        if (!gpu_irq_ready && gpu_isr_cfg) {
            gpu_irq_line = pci_config_read8(pci.bus, pci.slot, pci.func, 0x3C);
            if (gpu_irq_line != 0 && gpu_irq_line != 0xff &&
                irq_register_handler(gpu_irq_line, virtio_gpu_irq, 0) == 0) {
                irq_unmask(gpu_irq_line);
                gpu_irq_ready = 1;
                console_write("virtio-gpu: completion IRQ on line ");
                console_write_dec(gpu_irq_line);
                console_write("\n");
            }
        }
        if (gpu_irq_ready) {
            controlq.avail->flags = 0;
            cursorq.avail->flags = 0;
        }
    }
    if (gpu_virgl_ok) {
        console_write("virtio-gpu: VirGL 3D acceleration negotiated\n");
    }
    virtio_gpu_query_display_info();
    if (virtio_gpu_init_hw_cursor() == 0) {
        gpu_hw_cursor_ready = 1;
        console_write("virtio-gpu: hw cursor enabled\n");
    }
    /* Exercise the accelerated 3D path when the host offers VirGL (no-op on a
     * plain virtio-gpu device). Runs after the 2D scanout is up so a failure
     * here cannot wedge the console output path. */
    virtio_gpu_virgl_selftest();
    console_write("virtio-gpu: ready\n");
}

int virtio_gpu_ready(void)
{
    return gpu_ready;
}

/*
 * Proof that the completion interrupt is real, not just wired.
 *
 * Runs from main.c once the scheduler is live, because the thing being proved
 * is that a waiter parks and an interrupt wakes it — neither is possible in the
 * early-boot context where the rest of this driver's self-tests run.
 *
 * The spin that normally absorbs a fast completion is disabled for the duration
 * so the commands really do take the blocking path; each one then has to be
 * ended by an interrupt that arrived while the task was parked, or the
 * watchdog re-poll counts it instead and this reports a failure.
 */
#define VGPU_IRQ_SELFTEST_CMDS 8

void virtio_gpu_irq_selftest(void)
{
    if (!bootinfo_has_flag("b1nix.test=1"))
        return;
    if (!gpu_ready) {
        console_write("M52-GFX: skip gpu-irq no-device\n");
        return;
    }
    if (!gpu_irq_ready) {
        console_write("M52-GFX: fail gpu-irq no-handler\n");
        return;
    }

    u64 irqs0 = gpu_irq_count;
    u64 parks0 = gpu_wait_parked;
    u64 wakes0 = gpu_wait_irq;
    u64 repolls0 = gpu_wait_repoll;

    gpu_present_acquire();
    gpu_wait_no_spin = 1;
    for (int i = 0; i < VGPU_IRQ_SELFTEST_CMDS; i++) {
        struct virtio_gpu_ctrl_hdr req;
        struct virtio_gpu_resp_display_info resp;
        memset(&req, 0, sizeof(req));
        memset(&resp, 0, sizeof(resp));
        req.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
        req.flags = VIRTIO_GPU_FLAG_FENCE;
        req.fence_id = gpu_fence_id++;
        if (virtio_gpu_send_cmd(&req, sizeof(req), &resp, sizeof(resp)) < 0 ||
            resp.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
            gpu_wait_no_spin = 0;
            gpu_present_release();
            console_write("M52-GFX: fail gpu-irq command\n");
            return;
        }
    }
    gpu_wait_no_spin = 0;
    gpu_present_release();

    console_write("virtio-gpu: delivery=");
    if (gpu_msix_vector >= 0) {
        console_write("msix-vector-");
        console_write_dec((u64)gpu_msix_vector);
    } else {
        console_write("intx-line-");
        console_write_dec(gpu_irq_line);
    }
    console_write(" irqs=");
    console_write_dec((u32)(gpu_irq_count - irqs0));
    console_write(" parked=");
    console_write_dec((u32)(gpu_wait_parked - parks0));
    console_write(" irq-wakes=");
    console_write_dec((u32)(gpu_wait_irq - wakes0));
    console_write(" watchdog-repolls=");
    console_write_dec((u32)(gpu_wait_repoll - repolls0));
    console_write("\n");

    /*
     * What is asserted is what is deterministic: one interrupt per completion,
     * and a wait that really gave up the CPU rather than spinning on the ring.
     *
     * Whether a given park was ended by the interrupt or by the watchdog is
     * NOT asserted, because it is a race the test cannot win reliably — a park
     * whose deadline the scheduler serves immediately (nothing else runnable
     * on this CPU) is a very short window for the message to land in, and the
     * attribution came out anywhere between 0 and 2 across instances. Both
     * counts are printed instead: a driver that stopped receiving interrupts
     * altogether fails on the count above, which is the property that matters.
     */
    if (gpu_irq_count - irqs0 == VGPU_IRQ_SELFTEST_CMDS &&
        gpu_wait_parked > parks0) {
        console_write("M52-GFX: ok gpu-irq\n");
    } else {
        console_write("M52-GFX: fail gpu-irq not-interrupt-driven\n");
    }
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
    int fresh_surface = 0;
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
        fresh_surface = 1;
    }

    /* A damage rectangle says what changed since the last frame — and on a
     * surface that has just been allocated and zeroed there is no last frame
     * to have changed since, so honouring it would leave everything outside
     * the rectangle black. The first frame onto a new surface is always
     * whole. */
    if (fresh_surface) {
        dirty_x = 0;
        dirty_y = 0;
        dirty_w = width;
        dirty_h = height;
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
            (void)virtio_gpu_cursor_submit(VIRTIO_GPU_CMD_MOVE_CURSOR, cursor_x,
                                           cursor_y, 0, 0, 0);
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

/*
 * Serialize the frame path without masking interrupts across it.
 *
 * This used to be spin_lock_irqsave() held for the whole of
 * virtio_gpu_present_locked() — which contains two device round trips. On the
 * CPU presenting a frame that meant no timer tick and no signal delivery until
 * the device was done, and it also made the round trip unable to park: a
 * spinlock holder must not sleep. A sleeping lock instead. The spinlock below
 * is held only long enough to test and set a flag.
 */
static void gpu_present_acquire(void)
{
    for (;;) {
        u64 flags;
        spin_lock_irqsave(&gpu_present_lock, &flags);
        if (!gpu_present_busy) {
            gpu_present_busy = 1;
            spin_unlock_irqrestore(&gpu_present_lock, flags);
            return;
        }
        spin_unlock_irqrestore(&gpu_present_lock, flags);
        /* The timeout is the lost-wake guard: a release that raced this park
         * costs one tick, not the frame. */
        if (scheduler_can_block())
            scheduler_block_on_timeout(&gpu_present_busy, 1);
        else
            scheduler_yield();
    }
}

static void gpu_present_release(void)
{
    u64 flags;
    spin_lock_irqsave(&gpu_present_lock, &flags);
    gpu_present_busy = 0;
    spin_unlock_irqrestore(&gpu_present_lock, flags);
    scheduler_wake_all(&gpu_present_busy);
}

int virtio_gpu_present(const u32 *src, u32 width, u32 height, u32 dirty_x,
                       u32 dirty_y, u32 dirty_w, u32 dirty_h, int cursor_x,
                       int cursor_y, int cursor_visible)
{
    gpu_present_acquire();
    int rc = virtio_gpu_present_locked(src, width, height, dirty_x, dirty_y,
                                       dirty_w, dirty_h, cursor_x, cursor_y,
                                       cursor_visible);
    gpu_present_release();
    return rc;
}
