/*
 * Intel 82801AA AC'97 audio controller driver — M79: Audio Stack
 *
 * QEMU `-device AC97` presents an 82801AA (PCI vendor 0x8086, device 0x2415,
 * class 04/01) with a Sigmatel STAC9700 codec and two I/O BARs: BAR0 = NAM
 * (native audio mixer), BAR1 = NABM (native audio bus master). This driver
 * implements the mixer (master + PCM volume/mute) and a polled PCM-out DMA
 * engine, and exposes /dev/dsp1 (HDA owns /dev/dsp).
 *
 * The register map follows the AC'97 2.1 bus-master model (see QEMU
 * hw/audio/ac97.c). AC'97 mixer volume fields are attenuation: 0 = full
 * volume; bit 15 of each volume register is the mute flag.
 */
#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/io.h>
#include <b1nix/mm.h>
#include <b1nix/iommu.h>
#include <b1nix/pci.h>
#include <b1nix/sched.h>
#include <b1nix/sound.h>
#include <b1nix/vfs.h>
#include <string.h>

/* ── PCI identity ────────────────────────────────────────────────────────── */
#define AC97_PCI_VENDOR 0x8086
#define AC97_PCI_DEVICE 0x2415 /* Intel 82801AA (ICH) */
#define AC97_PCI_CLASS  0x04
#define AC97_PCI_SUBCLASS 0x01

/* ── NAM (mixer) registers — 16-bit I/O ──────────────────────────────────── */
#define AC97_NA_RESET            0x00
#define AC97_NA_MASTER_VOL       0x02
#define AC97_NA_PCM_OUT_VOL      0x18
#define AC97_NA_EXT_AUDIO_CTRL   0x2A
#define AC97_NA_PCM_FRONT_RATE   0x2C
#define AC97_NA_VENDOR_ID1       0x7C
#define AC97_NA_VENDOR_ID2       0x7E

#define AC97_EACS_VRA            0x0001 /* variable rate audio */

#define AC97_MUTE_SHIFT          15
#define AC97_MASTER_VOL_MASK     0x3F   /* 6-bit volume per channel */
#define AC97_PCM_VOL_MASK        0x1F   /* 5-bit volume per channel */

/* ── NABM (bus master) registers ─────────────────────────────────────────── */
#define AC97_PO_BDBAR 0x10 /* PCM-out buffer descriptor base (32-bit) */
#define AC97_PO_CIV   0x14 /* current index value (8-bit)             */
#define AC97_PO_LVI   0x15 /* last valid index (8-bit)                */
#define AC97_PO_SR    0x16 /* status (16-bit)                         */
#define AC97_PO_PICB  0x18 /* position in current buffer (16-bit)     */
#define AC97_PO_PIV   0x1A /* prefetched index (8-bit)                */
#define AC97_PO_CR    0x1B /* control (8-bit)                         */
#define AC97_GLOB_CNT 0x2C /* global control (32-bit)                 */

#define AC97_CR_RPBM  0x0001 /* run/pause bus master */
#define AC97_CR_RR    0x0002 /* reset registers (self-clearing) */
#define AC97_SR_DCH   0x0001 /* DMA controller halted */
#define AC97_SR_LVBCI 0x0004 /* last valid buffer completion interrupt */
#define AC97_SR_BCIS  0x0008 /* buffer completion interrupt status */
#define AC97_SR_FIFOE 0x0010 /* FIFO error */
/* The three write-one-to-clear status bits, cleared together before a start. */
#define AC97_SR_WC    (AC97_SR_LVBCI | AC97_SR_BCIS | AC97_SR_FIFOE)

#define AC97_GC_CR    0x0002 /* cold reset request */

#define AC97_BD_IOC   0x80000000u /* interrupt on completion */
#define AC97_BD_BUP   0x00010000u /* buffer underrun policy  */

/* Buffer descriptor: 8 bytes, entry index advances with each channel start
 * (QEMU fetches bd[civ] where civ = the running prefetched index). */
struct ac97_bd {
	u32 addr;    /* physical address of the audio buffer */
	u32 ctl_len; /* bits 31 IOC, 17 BUP, 15:0 length in 16-bit samples */
} __attribute__((packed));

/* ── Static driver state ─────────────────────────────────────────────────── */
static u16 ac97_nam_port;
static u16 ac97_nabm_port;
static int ac97_inited;
static u8 ac97_pci_bus, ac97_pci_slot, ac97_pci_func;

static u64 ac97_dma_buf_phys;
static u8  *ac97_dma_buf;
static u32 ac97_dma_buf_sz;
static u64 ac97_bdl_phys;

static volatile int ac97_play_lock;

static int ac97_vol_left = 100;
static int ac97_vol_right = 100;
static int ac97_muted;

static struct sound_device ac97_sound_dev;

/* ── Mixer helpers ───────────────────────────────────────────────────────── */
static void ac97_set_master_vol(int left, int right, int muted) {
	/* AC'97 master volume: 6-bit attenuation per channel, 0 = full. */
	u32 vl = (100 - left)  * AC97_MASTER_VOL_MASK / 100;
	u32 vr = (100 - right) * AC97_MASTER_VOL_MASK / 100;
	u32 reg = (vl << 8) | vr | ((muted ? 1u : 0u) << AC97_MUTE_SHIFT);
	outw(ac97_nam_port + AC97_NA_MASTER_VOL, (u16)reg);
}

static void ac97_set_pcm_vol(int left, int right, int muted) {
	u32 vl = (100 - left) * AC97_PCM_VOL_MASK / 100;
	u32 vr = (100 - right) * AC97_PCM_VOL_MASK / 100;
	u32 reg = (vl << 8) | vr | ((muted ? 1u : 0u) << AC97_MUTE_SHIFT);
	outw(ac97_nam_port + AC97_NA_PCM_OUT_VOL, (u16)reg);
}

static int ac97_sound_set_volume(struct sound_device *dev, int left, int right,
                                 int muted) {
	(void)dev;
	if (!ac97_inited)
		return -ENXIO;

	if (left < 0) left = ac97_vol_left;
	if (right < 0) right = ac97_vol_right;
	if (muted < 0) muted = ac97_muted;
	if (left > 100) left = 100;
	if (right > 100) right = 100;
	ac97_vol_left = left;
	ac97_vol_right = right;
	ac97_muted = muted;

	ac97_set_master_vol(left, right, muted);
	ac97_set_pcm_vol(left, right, muted);
	return 0;
}

static int ac97_sound_get_volume(struct sound_device *dev, int *left, int *right,
                                 int *muted) {
	(void)dev;
	if (!ac97_inited)
		return -ENXIO;

	u16 reg = inw(ac97_nam_port + AC97_NA_MASTER_VOL);
	if (left) *left = 100 - (int)(((reg >> 8) & AC97_MASTER_VOL_MASK) * 100u / AC97_MASTER_VOL_MASK);
	if (right) *right = 100 - (int)((reg & AC97_MASTER_VOL_MASK) * 100u / AC97_MASTER_VOL_MASK);
	/* The codec's mute bit is write-only/implementation-defined on some
	 * AC'97 emulators.  The driver already records the accepted OSS state in
	 * ac97_muted, so report that stable software state instead of making a
	 * successful WRITE_MUTE round-trip depend on a hardware readback quirk. */
	if (muted) *muted = ac97_muted;
	return 0;
}

/* ── PCM-out DMA ─────────────────────────────────────────────────────────── */
static int ac97_setup_dma(u32 buf_size) {
	buf_size = (buf_size + 4095) & ~4095u;
	if (buf_size < 4096)
		buf_size = 4096;
	ac97_dma_buf_sz = buf_size;

	u64 frames = (buf_size + PAGE_SIZE - 1) / PAGE_SIZE;
	ac97_dma_buf_phys = pmm_alloc_frames(frames);
	if (!ac97_dma_buf_phys)
		return -1;
	ac97_dma_buf = (u8 *)(usize)(ac97_dma_buf_phys + vmm_direct_map_base());
	memset(ac97_dma_buf, 0, buf_size);

	/* Descriptor list: 32 entries (the index QEMU fetches advances by one
	 * on every channel start), stored in its own page. */
	ac97_bdl_phys = pmm_alloc_frames(1);
	if (!ac97_bdl_phys)
		return -1;
	/* BDBAR and a descriptor's address field are 32 bits wide: memory above
	 * 4 GiB is not addressable by this controller at all, and truncating to
	 * 32 bits would point it at somebody else's page. */
	if ((ac97_bdl_phys + PAGE_SIZE) > 0xFFFFFFFFull ||
	    (ac97_dma_buf_phys + ac97_dma_buf_sz) > 0xFFFFFFFFull) {
		console_write("ac97: DMA memory above 4 GiB, unusable by this controller\n");
		return -1;
	}
	memset((void *)(usize)(ac97_bdl_phys + vmm_direct_map_base()), 0, PAGE_SIZE);

	/* Bring the codec out of reset and select a fixed 48 kHz rate. */
	outl(ac97_nabm_port + AC97_GLOB_CNT, 0);
	u16 eac = inw(ac97_nam_port + AC97_NA_EXT_AUDIO_CTRL);
	outw(ac97_nam_port + AC97_NA_EXT_AUDIO_CTRL, (u16)(eac | AC97_EACS_VRA));
	outw(ac97_nam_port + AC97_NA_PCM_FRONT_RATE, 48000);

	/* Unmute master + PCM at full volume (QEMU resets them muted). */
	ac97_set_master_vol(100, 100, 0);
	ac97_set_pcm_vol(100, 100, 0);
	return 0;
}

/* Reset the PCM-out channel and point it at our descriptor list.
 *
 * The BDBAR write is the whole reason playback never ran: the base address of
 * the descriptor list was never given to the controller, so it fetched
 * descriptors from physical address zero and the channel sat halted forever.
 * The self-test could not see that, because ac97_play_bytes returned success
 * whether or not the DMA had done anything.
 *
 * The reset (CR.RR, self-clearing) is what makes a start repeatable: it zeroes
 * CIV, PIV and LVI, so every playback begins at descriptor 0 instead of chasing
 * a rotating index the controller might not agree about. */
static void ac97_channel_arm(void) {
	outb(ac97_nabm_port + AC97_PO_CR, 0); /* stop before resetting */
	outb(ac97_nabm_port + AC97_PO_CR, AC97_CR_RR);
	for (int i = 0; i < 10000; i++) {
		if (!(inb(ac97_nabm_port + AC97_PO_CR) & AC97_CR_RR))
			break;
		cpu_relax();
	}
	outl(ac97_nabm_port + AC97_PO_BDBAR, (u32)ac97_bdl_phys);
	outw(ac97_nabm_port + AC97_PO_SR, AC97_SR_WC);
}

/* Start playback of `bytes` from the DMA buffer and wait for completion. */
static int ac97_play_bytes(u32 bytes) {
	struct ac97_bd *bdl =
		(struct ac97_bd *)(usize)(ac97_bdl_phys + vmm_direct_map_base());

	ac97_channel_arm();
	bdl[0].addr = (u32)ac97_dma_buf_phys;
	bdl[0].ctl_len = (bytes / 2) | AC97_BD_IOC;
	outb(ac97_nabm_port + AC97_PO_LVI, 0);
	outb(ac97_nabm_port + AC97_PO_CR, AC97_CR_RPBM);

	u64 start = scheduler_get_ticks();
	int running = 0, done = 0;

	/* Two phases, not one. A channel that has just been reset reports DCH
	 * (halted) until the first sample is fetched, so a single "wait for DCH"
	 * loop would return success immediately having played nothing. Wait for
	 * the channel to start, then for it to halt again. */
	while (scheduler_get_ticks() - start <= 5 * SCHED_TICKS_PER_SEC) {
		u16 sr = inw(ac97_nabm_port + AC97_PO_SR);

		if (!running) {
			if (!(sr & AC97_SR_DCH))
				running = 1;
		} else if (sr & AC97_SR_DCH) {
			done = 1;
			break;
		}
		scheduler_yield();
	}
	outb(ac97_nabm_port + AC97_PO_CR, 0); /* pause */
	outw(ac97_nabm_port + AC97_PO_SR, AC97_SR_WC);
	/* A timeout is a failure and says so. Returning success here regardless
	 * meant the self-test's "ok play-tone" printed for a channel that had
	 * never consumed its descriptor, and a write(2) of a few chunks sat for
	 * five seconds apiece with nothing to show for it. */
	return done ? 0 : -1;
}

/* ── Sound device interface ──────────────────────────────────────────────── */
static int ac97_sound_open(struct sound_device *dev) {
	(void)dev;
	return ac97_inited ? 0 : -1;
}

static void ac97_sound_close(struct sound_device *dev) {
	(void)dev;
}

static isize ac97_sound_write(struct sound_device *dev, const void *buf,
                              usize len) {
	(void)dev;
	if (!ac97_inited)
		return -1;

	while (__sync_lock_test_and_set(&ac97_play_lock, 1))
		scheduler_yield();

	usize written = 0;
	while (written < len) {
		usize chunk = len - written;
		if (chunk > ac97_dma_buf_sz)
			chunk = ac97_dma_buf_sz;
		memcpy(ac97_dma_buf, (const char *)buf + written, chunk);
		if (ac97_play_bytes((u32)chunk) != 0) {
			/* Report what did play rather than blocking the caller for five
			 * seconds per remaining chunk. */
			break;
		}
		written += chunk;
	}

	__sync_lock_release(&ac97_play_lock);
	return (isize)written;
}

static u32 ac97_sound_get_position(struct sound_device *dev) {
	(void)dev;
	if (!ac97_inited)
		return 0;
	return inw(ac97_nabm_port + AC97_PO_PICB);
}

static int ac97_sound_ready(struct sound_device *dev) {
	(void)dev;
	return ac97_inited;
}

/* ── /dev/dsp1 VFS callbacks ─────────────────────────────────────────────── */
static isize ac97_dsp_write(struct vfs_node *node, u64 offset,
                            const char *buffer, usize size, int flags) {
	(void)node; (void)offset; (void)flags;
	return ac97_sound_write(&ac97_sound_dev, buffer, size);
}

static isize ac97_dsp_read(struct vfs_node *node, u64 offset, char *buffer,
                           usize size, int flags) {
	(void)node; (void)offset; (void)buffer; (void)size; (void)flags;
	return 0; /* capture not implemented */
}

static void ac97_dsp_release(struct vfs_node *node) {
	(void)node;
}

static int ac97_dsp_ioctl(struct vfs_node *node, u64 request, void *arg) {
	(void)node;
	return sound_mixer_ioctl(&ac97_sound_dev, request, arg);
}

/* ── Device init ─────────────────────────────────────────────────────────── */
void ac97_init(void) {
	struct pci_device_info pci;

	if (bootinfo_has_flag("b1nix.skip-ac97")) {
		console_write("ac97: skipped (b1nix.skip-ac97)\n");
		return;
	}

	if (!pci_find_class(AC97_PCI_CLASS, AC97_PCI_SUBCLASS, 0, &pci))
		return;
	if (pci.vendor_id != AC97_PCI_VENDOR)
		return;

	/* Enable I/O space + bus master. */
	u16 cmd = pci_config_read16(pci.bus, pci.slot, pci.func, 0x04);
	cmd |= 0x0005;
	pci_config_write16(pci.bus, pci.slot, pci.func, 0x04, cmd);

	/* BAR0 = NAM, BAR1 = NABM (both I/O). */
	u32 bar0 = pci_config_read32(pci.bus, pci.slot, pci.func, 0x10);
	u32 bar1 = pci_config_read32(pci.bus, pci.slot, pci.func, 0x14);
	if (!(bar0 & 1) || !(bar1 & 1)) {
		console_write("ac97: expected I/O BARs\n");
		return;
	}
	ac97_nam_port = (u16)(bar0 & 0xFFFC);
	ac97_nabm_port = (u16)(bar1 & 0xFFFC);
	ac97_pci_bus = pci.bus;
	ac97_pci_slot = pci.slot;
	ac97_pci_func = pci.func;

	/* Read the codec vendor to confirm a codec is attached. */
	u16 vid1 = inw(ac97_nam_port + AC97_NA_VENDOR_ID1);
	u16 vid2 = inw(ac97_nam_port + AC97_NA_VENDOR_ID2);
	console_write("ac97: 8086:2415 codec 0x");
	console_write_hex32(((u32)vid1 << 16) | vid2);
	console_write(" nam 0x");
	console_write_hex32(ac97_nam_port);
	console_write(" nabm 0x");
	console_write_hex32(ac97_nabm_port);
	console_write("\n");

	if (ac97_setup_dma(16 * 1024) < 0) {
		console_write("ac97: DMA setup failed\n");
		return;
	}

	ac97_inited = 1;

	/* Register the sound device interface. */
	ac97_sound_dev.name = "ac97";
	ac97_sound_dev.sample_rate = 48000;
	ac97_sound_dev.channels = 2;
	ac97_sound_dev.format = SOUND_FMT_S16LE;
	ac97_sound_dev.buffer_size = ac97_dma_buf_sz;
	ac97_sound_dev.open = ac97_sound_open;
	ac97_sound_dev.close = ac97_sound_close;
	ac97_sound_dev.write = ac97_sound_write;
	ac97_sound_dev.get_position = ac97_sound_get_position;
	ac97_sound_dev.ready = ac97_sound_ready;
	ac97_sound_dev.vol_left = 100;
	ac97_sound_dev.vol_right = 100;
	ac97_sound_dev.muted = 0;
	ac97_sound_dev.set_volume = ac97_sound_set_volume;
	ac97_sound_dev.get_volume = ac97_sound_get_volume;
	sound_register(&ac97_sound_dev);

	console_write("ac97: initialized 48kHz stereo 16-bit, /dev/dsp1 ready\n");
}

/* Re-register /dev/dsp1 after the real root is mounted (the node created by
 * ac97_init() lands on the initramfs root, which becomes unreachable when "/"
 * redirects to the ext4 root). vfs_repopulate_after_root_mount() calls this.
 * Idempotent. */
void ac97_dev_init(void) {
	if (!ac97_inited)
		return;
	struct vfs_node *node = vfs_add_node("/dev/dsp1", VFS_DEVICE, 0, 0, 0);
	if (node && !IS_ERR(node)) {
		node->inode->mode = 0644;
		node->inode->read_cb = ac97_dsp_read;
		node->inode->write_cb = ac97_dsp_write;
		node->inode->release_cb = ac97_dsp_release;
		node->inode->ioctl_cb = ac97_dsp_ioctl;
		vfs_node_put(node);
	}
}

/* ── Self-test (test mode) ──────────────────────────────────────────────── */
void ac97_selftest(void) {
	if (!ac97_inited) {
		console_write("M79-AC97: skip no-device\n");
		return;
	}

	console_write("M79-AC97: ok probe\n");

	/* Verify the NAM mixer is live: read the codec vendor back. */
	u16 vid1 = inw(ac97_nam_port + AC97_NA_VENDOR_ID1);
	u16 vid2 = inw(ac97_nam_port + AC97_NA_VENDOR_ID2);
	if (vid1 != 0x8384 || (vid2 & 0xFF00) != 0x7600) {
		console_write("M79-AC97: fail vendor-id\n");
		return;
	}
	console_write("M79-AC97: ok vendor-id\n");

	/* Volume round-trip through the AC'97 master register. */
	u16 saved = inw(ac97_nam_port + AC97_NA_MASTER_VOL);
	ac97_sound_set_volume(&ac97_sound_dev, 42, 42, 1);
	u16 reg = inw(ac97_nam_port + AC97_NA_MASTER_VOL);
	if (((reg >> AC97_MUTE_SHIFT) & 1) != 1)
		console_write("M79-AC97: fail mute-bit\n");
	else
		console_write("M79-AC97: ok mute-bit\n");
	ac97_sound_set_volume(&ac97_sound_dev, 42, 42, 0);
	reg = inw(ac97_nam_port + AC97_NA_MASTER_VOL);
	int l = 100 - (int)(((reg >> 8) & AC97_MASTER_VOL_MASK) * 100u / AC97_MASTER_VOL_MASK);
	if (l < 37 || l > 47)
		console_write("M79-AC97: fail volume-write\n");
	else
		console_write("M79-AC97: ok volume-write\n");
	outw(ac97_nam_port + AC97_NA_MASTER_VOL, saved);

	/* Playback path: write a short tone through the DMA engine. */
	u32 n = 4800; /* 100 ms of 48 kHz stereo 16-bit */
	i16 *samples = (i16 *)ac97_dma_buf;
	for (u32 i = 0; i < n; i++) {
		u32 period = 48000 / 440;
		u32 pos = i % period;
		u32 half = period / 2;
		i32 v = (pos < half)
			? (i32)((u32)16000 * 2 * pos / period)
			: (i32)((u32)16000 * 2 * (period - pos) / period);
		v -= 8000;
		if (v > 16000) v = 16000;
		if (v < -16000) v = -16000;
		samples[i] = (i16)v;
	}
	if (ac97_play_bytes(n * 2) == 0) {
		console_write("M79-AC97: ok play-tone\n");
	} else {
		console_write("M79-AC97: FAIL play-tone (channel never halted, SR=0x");
		console_write_hex64(inw(ac97_nabm_port + AC97_PO_SR));
		console_write(")\n");
	}

	console_write("M79-AC97: ok done\n");
}

/* ── M100b: the violation an IOMMU exists to block ──────────────────
 *
 * Every other check here shows the unit letting through what was granted. This
 * one shows it refusing what was not: the codec is given its descriptor list
 * and nothing else, then told to play — so the descriptor fetch succeeds and
 * the audio-buffer read, which nobody mapped, is stopped by the unit and
 * recorded as a fault.
 *
 * Audio is the right device to ask. A blocked transfer here loses a fraction of
 * a second of silence and the channel is reset immediately afterwards, whereas
 * doing the same to a storage or network device would strand a queue nothing
 * retries.
 *
 * Returns 1 when the unit recorded the fault, 0 when it did not, -1 when there
 * is no codec to ask.
 */
int ac97_iommu_violation_probe(void)
{
	if (!ac97_inited || !iommu_active())
		return -1;

	if (iommu_attach_device(ac97_pci_bus, ac97_pci_slot, ac97_pci_func) != 0)
		return -1;

	/* Grant the descriptor list. Deliberately not the audio buffer. */
	int mapped = iommu_map_identity(ac97_bdl_phys, PAGE_SIZE, 1) == 0;
	iommu_fault_clear();

	u32 faults = 0;
	if (mapped) {
		struct ac97_bd *bdl =
			(struct ac97_bd *)(usize)(ac97_bdl_phys + vmm_direct_map_base());
		ac97_channel_arm();
		bdl[0].addr = (u32)ac97_dma_buf_phys;
		bdl[0].ctl_len = (4096 / 2) | AC97_BD_IOC;
		outb(ac97_nabm_port + AC97_PO_LVI, 0);
		outb(ac97_nabm_port + AC97_PO_CR, AC97_CR_RPBM);

		u64 start = scheduler_get_ticks();
		while (scheduler_get_ticks() - start < SCHED_TICKS_PER_SEC / 2) { /* up to 0.5 s */
			faults = iommu_fault_count();
			if (faults)
				break;
			scheduler_yield();
		}
		outb(ac97_nabm_port + AC97_PO_CR, 0); /* stop the channel */
		outw(ac97_nabm_port + AC97_PO_SR, AC97_SR_WC);
	}

	iommu_unmap(ac97_bdl_phys & ~(u64)(PAGE_SIZE - 1), PAGE_SIZE);
	iommu_detach_device(ac97_pci_bus, ac97_pci_slot, ac97_pci_func);
	iommu_fault_clear();
	return faults ? 1 : 0;
}
