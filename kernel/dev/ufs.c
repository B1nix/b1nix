/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * UFS — Universal Flash Storage, the internal storage of every recent phone.
 *
 * A UFS host controller (UFSHCI) is a register block with two request lists in
 * host memory, much like AHCI: a transfer request list whose entries point at a
 * command descriptor holding a UPIU (the UFS protocol frame), a response UPIU
 * and a scatter-gather table. The device behind it speaks SCSI inside those
 * UPIUs and exposes up to eight logical units (LUNs), each an independent disk
 * with its own GPT.
 *
 * Two ways in:
 *   - PCI (class 01/09). What QEMU's `-device ufs` provides, and what the smoke
 *     suite drives.
 *   - The device tree's `qcom,ufshc` node (Snapdragon 855 / SM8150, the Xperia
 *     5). Opt-in with `b1nix.ufs`: the SoC tree marks the node disabled, and a
 *     register read of an unclocked Qualcomm block does not return.
 *
 * Bring-up keeps the bootloader's link when it can. Qualcomm's ABL reads the
 * boot image off this very controller, so on entry the link is normally up at
 * a high-speed gear with the PHY calibrated. Re-running the full sequence would
 * need the PHY calibration tables, the RPMh regulators and the GCC resets that
 * this kernel has no drivers for. So when the controller is enabled and reports
 * a device present and both lists ready, only the list bases are re-pointed at
 * our memory (legal once run-stop is cleared) and a NOP proves the device still
 * answers. Only when that fails, or on a controller that was never enabled
 * (QEMU, or `b1nix.ufs-reset`), does the full sequence run: HCE reset, UIC link
 * startup, fDeviceInit. That path leaves the link in PWM gear 1 — correct but
 * slow; no power-mode change is attempted.
 *
 * I/O is one request at a time on slot 0, polled, through a DMA bounce buffer
 * below 4 GiB. The block layer gets 512-byte sectors, translated to the LUN's
 * logical block (4 KiB on every phone, and on QEMU); a partial logical block is
 * read, patched and written back.
 *
 * WRITES ARE FENCED. A phone's UFS holds the bootloaders, the modem firmware and
 * the partition tables; a stray write there does not corrupt a filesystem, it
 * bricks the handset. A LUN refuses every write except inside a partition that
 * is either named in `b1nix.ufs-rw=<gptname>[,<gptname>...]` or already carries
 * an ext filesystem labelled `b1nix-root` — i.e. one b1nix itself put there.
 */

#include <b1nix/blk.h>
#include <b1nix/bootmark.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/irq.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <b1nix/sched.h>
#include <b1nix/types.h>
#include <b1nix/arch.h>
#include <b1nix/ufs.h>
#include <string.h>

/* ── UFSHCI register block ───────────────────────────────────────────────── */

#define REG_CAP        0x00
#define REG_VER        0x08
#define REG_IS         0x20
#define REG_IE         0x24
#define REG_HCS        0x30
#define REG_HCE        0x34
#define REG_UTRLBA     0x50
#define REG_UTRLBAU    0x54
#define REG_UTRLDBR    0x58
#define REG_UTRLCLR    0x5c
#define REG_UTRLRSR    0x60
#define REG_UTMRLBA    0x70
#define REG_UTMRLBAU   0x74
#define REG_UTMRLDBR   0x78
#define REG_UTMRLRSR   0x80
#define REG_UICCMD     0x90
#define REG_UCMDARG1   0x94
#define REG_UCMDARG2   0x98
#define REG_UCMDARG3   0x9c

/* Qualcomm vendor registers (ufs-qcom). */
#define QCOM_REG_CFG1        0xdc
#define QCOM_REG_HW_VERSION  0xe4
#define QCOM_QUNIPRO_SEL     (1u << 0)

#define CAP_NUTRS_MASK  0xffu
#define CAP_64AS        (1u << 24)

#define HCS_DP          (1u << 0)
#define HCS_UTRLRDY     (1u << 1)
#define HCS_UTMRLRDY    (1u << 2)
#define HCS_UCRDY       (1u << 3)
#define HCS_READY       (HCS_DP | HCS_UTRLRDY | HCS_UTMRLRDY | HCS_UCRDY)

#define IS_UTRCS        (1u << 0)
#define IE_UTRCE        (1u << 0)
#define IS_UE           (1u << 2)
#define IS_ULSS         (1u << 8)
#define IS_UCCS         (1u << 10)

#define UIC_DME_GET          0x01
#define UIC_DME_SET          0x02
#define UIC_DME_LINKSTARTUP  0x16

/* UniPro attribute: PA_Local_TX_LCC_Enable. */
#define PA_LOCAL_TX_LCC_ENABLE 0x155e

/* ── UTP transfer request descriptor / UPIU ─────────────────────────────── */

#define UTRD_CT_UFS      1u     /* command type, UFS storage */
#define UTRD_DD_NONE     0u
#define UTRD_DD_TO_DEV   1u
#define UTRD_DD_FROM_DEV 2u
#define OCS_SUCCESS      0x00
#define OCS_INVALID      0x0f

#define UPIU_NOP_OUT     0x00
#define UPIU_COMMAND     0x01
#define UPIU_QUERY_REQ   0x16
#define UPIU_NOP_IN      0x20
#define UPIU_RESPONSE    0x21
#define UPIU_QUERY_RSP   0x36

#define UPIU_FLAG_READ   0x40
#define UPIU_FLAG_WRITE  0x20

#define QUERY_FN_READ    0x01
#define QUERY_FN_WRITE   0x81
#define QUERY_OP_READ_FLAG 0x05
#define QUERY_OP_SET_FLAG  0x06
#define QUERY_FLAG_DEVICE_INIT 0x01

/* Layout of the one command descriptor: command UPIU, response UPIU, PRD
 * table, each where Linux puts them. The descriptor offsets are in 32-bit
 * words; the PRD length is an entry count. */
#define UCD_CMD_OFF   0
#define UCD_RSP_OFF   512
#define UCD_PRD_OFF   1024
#define UCD_UPIU_SIZE 512
#define UCD_PRD_ENTRY 16

#define UFS_PAGE      4096u
/* 64 frames: 256 KiB per command. Contiguous and below 4 GiB, so neither the
 * controller's addressing capability nor a scatter list matters. */
#define UFS_BOUNCE_FRAMES 64u
#define UFS_MAX_LUNS  8u
/* DMA memory the SM8150 controller may reach; see probe_host. */
#define QCOM_UFS_DMA_BASE 0xf0000000ull
/* ufshc@1d84000: GIC SPI 265, from the Xperia 5 device tree. */
#define QCOM_UFS_IRQ (32 + 265)

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2a
#define SCSI_SYNC_CACHE10    0x35
#define SCSI_READ16          0x88
#define SCSI_WRITE16         0x8a
#define SCSI_SERVICE_IN16    0x9e
#define SCSI_SAI_READ_CAP16  0x10

struct ufs_host {
	u64 base;
	int is_qcom;
	int inherited;          /* the bootloader's link was kept */
	u32 cap;
	u64 utrl_phys, utmrl_phys, ucd_phys, bounce_phys;
	u8 *utrl, *ucd, *bounce;
	volatile int io_busy;
	u32 index;              /* ufs<N>, for messages */
	int quiet;              /* probing LUNs that may not exist */
	u32 bounce_bytes;       /* DMA bounce buffer size */
	int irq;                /* completion interrupt, -1 = polled only */
	volatile u32 irq_hits;  /* completions the interrupt reported */
};

/* Logical blocks a GPT takes at either end of a LUN: the protective MBR or
 * nothing, the header, and the entries (128 of 128 bytes fit in 4 of 4 KiB). */
#define UFS_GPT_BLOCKS 6

struct ufs_rw_window {
	u64 start;              /* 512-byte sectors on the LUN */
	u64 count;
};

struct ufs_lu {
	struct block_device blk;
	struct ufs_host *host;
	u8 lun;
	u32 lb_size;            /* logical block size, bytes */
	u32 ratio;              /* lb_size / 512 */
	u64 lb_count;
	struct ufs_rw_window rw[8];
	u32 rw_count;
	int refused_reported;
};

static struct ufs_host g_hosts[2];
static u32 g_host_count;
static struct ufs_lu g_lus[2 * UFS_MAX_LUNS];
static u32 g_lu_count;

/* Register accesses are written out as single ldr/str on aarch64, the way
 * Linux's readl/writel are. As plain volatile dereferences clang folded the
 * address arithmetic into the access (`ldr w9, [x8, #0x20]!`), and a hypervisor
 * that emulates MMIO from the fault syndrome (HVF) cannot decode a writeback
 * form: QEMU aborted in uic_cmd. */
static inline u32 rd(struct ufs_host *h, u32 off)
{
#if defined(__aarch64__)
	u32 v;

	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(h->base + off) : "memory");
	return v;
#else
	return *(volatile u32 *)(usize)(h->base + off);
#endif
}

static inline void wr(struct ufs_host *h, u32 off, u32 v)
{
#if defined(__aarch64__)
	__asm__ volatile("str %w0, [%1]" : : "rZ"(v), "r"(h->base + off) : "memory");
#else
	*(volatile u32 *)(usize)(h->base + off) = v;
#endif
}

static void put_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put_be32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}
static void put_be64(u8 *p, u64 v)
{
	put_be32(p, (u32)(v >> 32));
	put_be32(p + 4, (u32)v);
}
static u32 get_be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static u64 get_be64(const u8 *p) { return ((u64)get_be32(p) << 32) | get_be32(p + 4); }

static void put_le16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put_le32(u8 *p, u32 v) { put_le16(p, (u16)v); put_le16(p + 2, (u16)(v >> 16)); }
static void put_le64(u8 *p, u64 v) { put_le32(p, (u32)v); put_le32(p + 4, (u32)(v >> 32)); }

static void ufs_msg(struct ufs_host *h, const char *what)
{
	console_write("ufs");
	console_write_dec(h->index);
	console_write(": ");
	console_write(what);
}

static void ufs_msg_hex(struct ufs_host *h, const char *what, u64 v)
{
	ufs_msg(h, what);
	console_write("0x");
	console_write_hex64(v);
	console_write("\n");
}

/* Poll `off` until (value & mask) == want, for up to `ms` milliseconds. */
static int wait_reg(struct ufs_host *h, u32 off, u32 mask, u32 want, u32 ms)
{
	for (u32 t = 0; t < ms * 10u; t++) {
		if ((rd(h, off) & mask) == want)
			return 0;
		arch_udelay(100);
	}
	return (rd(h, off) & mask) == want ? 0 : -1;
}

static u64 alloc_dma(u32 frames)
{
	u64 p = pmm_alloc_frames_below(frames, 1ull << 32);

	if (p)
		memset((void *)(usize)(p + vmm_direct_map_base()), 0,
		       (usize)frames * UFS_PAGE);
	return p;
}

/* ── UIC (UniPro) commands ───────────────────────────────────────────────── */

static int uic_cmd(struct ufs_host *h, u32 opcode, u32 arg1, u32 arg2, u32 arg3,
                   u32 *out3)
{
	if (wait_reg(h, REG_HCS, HCS_UCRDY, HCS_UCRDY, 1000) != 0) {
		ufs_msg(h, "UIC not ready\n");
		return -1;
	}
	/* Qualcomm's UniPro needs a breath between DME commands
	 * (UFSHCD_QUIRK_DELAY_BEFORE_DME_CMDS). */
	if (h->is_qcom)
		arch_udelay(1000);
	wr(h, REG_IS, IS_UCCS);
	wr(h, REG_UCMDARG1, arg1);
	wr(h, REG_UCMDARG2, arg2);
	wr(h, REG_UCMDARG3, arg3);
	wr(h, REG_UICCMD, opcode);
	if (wait_reg(h, REG_IS, IS_UCCS, IS_UCCS, 1000) != 0) {
		ufs_msg_hex(h, "UIC command timed out, opcode ", opcode);
		return -1;
	}
	wr(h, REG_IS, IS_UCCS);
	u32 result = rd(h, REG_UCMDARG2) & 0xff;
	if (out3)
		*out3 = rd(h, REG_UCMDARG3);
	return result == 0 ? 0 : -(int)result;
}

static int dme_set(struct ufs_host *h, u16 attr, u32 value)
{
	return uic_cmd(h, UIC_DME_SET, (u32)attr << 16, 0, value, 0);
}

/* ── Transfer requests (slot 0 only) ─────────────────────────────────────── */

/* Transfer-request completion: acknowledge it (the line is level-triggered
 * and stays up until IS is cleared) and wake whoever waits in utp_exec. */
static int ufs_irq(void *ctx)
{
	struct ufs_host *h = ctx;

	if (!(rd(h, REG_IS) & IS_UTRCS))
		return 0;
	wr(h, REG_IS, IS_UTRCS);
	/* Said once: whether the interrupt arrives at all is the question on a
	 * board whose hypervisor routes it, and requests wait asleep from now. */
	if (h->irq_hits++ == 0)
		ufs_msg(h, "completion interrupts arrive; requests now sleep\n");
	scheduler_wake_all(h);
	return 1;
}

/* Wait for slot 0's doorbell to clear. Polled until the interrupt has shown
 * that it arrives -- on a phone the hypervisor decides whether an SPI reaches
 * us, and a sleep that only a watchdog tick ended would cost 10 ms a
 * request -- and asleep from then on, so a single core runs something else
 * while the flash works. */
static int utp_wait(struct ufs_host *h, u32 timeout_ms)
{
	u64 deadline = scheduler_get_ticks() + SCHED_MS_TO_TICKS(timeout_ms);

	for (u32 t = 0; t < 20; t++) {
		if (!(rd(h, REG_UTRLDBR) & 1u))
			return 0;
		arch_udelay(5);
	}
	if (h->irq >= 0 && h->irq_hits && scheduler_can_block()) {
		while (rd(h, REG_UTRLDBR) & 1u) {
			if (scheduler_get_ticks() > deadline)
				return -1;
			scheduler_wait_prepare_timeout(h, 1);
			if (!(rd(h, REG_UTRLDBR) & 1u)) {
				scheduler_wait_cancel();
				break;
			}
			scheduler_wait_commit();
		}
		return 0;
	}
	for (u32 t = 0; t < timeout_ms * 10u; t++) {
		if (!(rd(h, REG_UTRLDBR) & 1u))
			return 0;
		arch_udelay(100);
	}
	return (rd(h, REG_UTRLDBR) & 1u) ? -1 : 0;
}

/*
 * Build UTRD 0 around whatever the caller put in the command UPIU, ring the
 * doorbell and wait. `dd` is the data direction, `data_len` the bytes the
 * bounce buffer carries. Returns 0 when the controller reports OCS success;
 * the response UPIU is left in h->ucd + UCD_RSP_OFF for the caller to read.
 */
static int utp_exec(struct ufs_host *h, u32 dd, u32 data_len, u32 timeout_ms)
{
	u8 *utrd = h->utrl;

	memset(utrd, 0, 32);
	utrd[3] = (u8)((UTRD_CT_UFS << 4) | (dd << 1));
	utrd[8] = OCS_INVALID;
	put_le64(utrd + 8 + 8, h->ucd_phys);
	put_le16(utrd + 24, UCD_UPIU_SIZE >> 2);  /* response length, words */
	put_le16(utrd + 26, UCD_RSP_OFF >> 2);    /* response offset, words */
	put_le16(utrd + 30, UCD_PRD_OFF >> 2);    /* PRD offset, words */

	memset(h->ucd + UCD_RSP_OFF, 0, UCD_UPIU_SIZE);
	u32 entries = 0;
	if (data_len) {
		u8 *prd = h->ucd + UCD_PRD_OFF;

		memset(prd, 0, UCD_PRD_ENTRY);
		put_le64(prd, h->bounce_phys);
		put_le32(prd + 12, data_len - 1);
		entries = 1;
	}
	put_le16(utrd + 28, (u16)entries);

	if (rd(h, REG_UTRLDBR) & 1u) {
		ufs_msg(h, "slot 0 still busy before submit\n");
		return -1;
	}
	wr(h, REG_IS, IS_UTRCS);
	wr(h, REG_UTRLDBR, 1u);

	if (utp_wait(h, timeout_ms) != 0) {
		ufs_msg(h, "request timed out, clearing slot\n");
		wr(h, REG_UTRLCLR, ~1u);
		wait_reg(h, REG_UTRLDBR, 1u, 0, 1000);
		return -1;
	}
	wr(h, REG_IS, IS_UTRCS);
	if (utrd[8] != OCS_SUCCESS) {
		if (!h->quiet)
			ufs_msg_hex(h, "request failed, OCS ", utrd[8]);
		return -1;
	}
	return 0;
}

static void upiu_header(u8 *u, u8 type, u8 flags, u8 lun, u8 fn)
{
	memset(u, 0, UCD_UPIU_SIZE);
	u[0] = type;
	u[1] = flags;
	u[2] = lun;
	u[3] = 0;       /* task tag: one request at a time */
	u[5] = fn;
}

static int ufs_nop(struct ufs_host *h)
{
	upiu_header(h->ucd + UCD_CMD_OFF, UPIU_NOP_OUT, 0, 0, 0);
	if (utp_exec(h, UTRD_DD_NONE, 0, 1500) != 0)
		return -1;
	return (h->ucd[UCD_RSP_OFF] & 0x3f) == UPIU_NOP_IN ? 0 : -1;
}

static int query_flag(struct ufs_host *h, u8 fn, u8 op, u8 idn, u8 *value)
{
	u8 *u = h->ucd + UCD_CMD_OFF;

	upiu_header(u, UPIU_QUERY_REQ, 0, 0, fn);
	u[12] = op;
	u[13] = idn;
	if (utp_exec(h, UTRD_DD_NONE, 0, 1500) != 0)
		return -1;
	u8 *r = h->ucd + UCD_RSP_OFF;
	if ((r[0] & 0x3f) != UPIU_QUERY_RSP || r[6] != 0)
		return -1;
	if (value)
		*value = r[23];
	return 0;
}

/*
 * One SCSI command. `dir` is UTRD_DD_*; data moves through the bounce buffer.
 * Returns the SCSI status (0 = GOOD) or -1 when the transport failed.
 */
static int scsi_exec(struct ufs_host *h, u8 lun, const u8 *cdb, u32 cdb_len,
                     u32 dir, u32 len)
{
	u8 *u = h->ucd + UCD_CMD_OFF;
	u8 flags = dir == UTRD_DD_FROM_DEV ? UPIU_FLAG_READ :
	           dir == UTRD_DD_TO_DEV ? UPIU_FLAG_WRITE : 0;

	upiu_header(u, UPIU_COMMAND, flags, lun, 0);
	put_be32(u + 12, len);
	memcpy(u + 16, cdb, cdb_len);
	if (utp_exec(h, dir, len, 20000) != 0)
		return -1;
	u8 *r = h->ucd + UCD_RSP_OFF;
	if ((r[0] & 0x3f) != UPIU_RESPONSE)
		return -1;
	return r[7];
}

static void ufs_io_lock(struct ufs_host *h)
{
	while (__sync_lock_test_and_set(&h->io_busy, 1))
		scheduler_yield();
}

static void ufs_io_unlock(struct ufs_host *h)
{
	__sync_lock_release(&h->io_busy);
}

/* ── Block device ────────────────────────────────────────────────────────── */

/* Read or write `n` logical blocks at `lb` through the bounce buffer. */
static int lu_rw_blocks(struct ufs_lu *lu, u64 lb, u32 n, int write)
{
	u8 cdb[16];
	u32 len;

	memset(cdb, 0, sizeof(cdb));
	if (lb + n <= 0xffffffffull && n <= 0xffff) {
		cdb[0] = write ? SCSI_WRITE10 : SCSI_READ10;
		put_be32(cdb + 2, (u32)lb);
		put_be16(cdb + 7, (u16)n);
		len = 10;
	} else {
		cdb[0] = write ? SCSI_WRITE16 : SCSI_READ16;
		put_be64(cdb + 2, lb);
		put_be32(cdb + 10, n);
		len = 16;
	}
	return scsi_exec(lu->host, lu->lun, cdb, len,
	                 write ? UTRD_DD_TO_DEV : UTRD_DD_FROM_DEV,
	                 n * lu->lb_size) == 0 ? 0 : -1;
}

static int lu_write_allowed(struct ufs_lu *lu, u64 lba, u32 count)
{
	for (u32 i = 0; i < lu->rw_count; i++) {
		const struct ufs_rw_window *w = &lu->rw[i];

		if (lba >= w->start && lba + count <= w->start + w->count)
			return 1;
	}
	return 0;
}

/* The shared read/write path, in 512-byte sectors. */
static int lu_transfer(struct block_device *dev, u64 lba, u32 count, u8 *buf,
                       int write)
{
	struct ufs_lu *lu = (struct ufs_lu *)dev->priv;
	struct ufs_host *h = lu->host;
	u32 bounce_blocks = h->bounce_bytes / lu->lb_size;

	if (!count || lba + count > dev->block_count)
		return -1;
	if (write && !lu_write_allowed(lu, lba, count)) {
		if (!lu->refused_reported) {
			lu->refused_reported = 1;
			console_write("ufs: refused write to ");
			console_write(dev->name);
			console_write(" sector ");
			console_write_dec(lba);
			console_write(" (outside every writable partition)\n");
		}
		return -1;
	}

	ufs_io_lock(h);
	int rc = 0;
	while (count) {
		u64 lb = lba / lu->ratio;
		u32 off = (u32)(lba % lu->ratio);
		u32 nblk = (off + count + lu->ratio - 1) / lu->ratio;
		if (nblk > bounce_blocks)
			nblk = bounce_blocks;
		u32 sectors = nblk * lu->ratio - off;
		if (sectors > count)
			sectors = count;
		usize bytes = (usize)sectors * 512u;

		if (!write) {
			if (lu_rw_blocks(lu, lb, nblk, 0) != 0) { rc = -1; break; }
			memcpy(buf, h->bounce + (usize)off * 512u, bytes);
		} else {
			/* A write that does not cover whole logical blocks keeps the
			 * bytes around it. */
			if ((off != 0 || sectors != nblk * lu->ratio) &&
			    lu_rw_blocks(lu, lb, nblk, 0) != 0) { rc = -1; break; }
			memcpy(h->bounce + (usize)off * 512u, buf, bytes);
			if (lu_rw_blocks(lu, lb, nblk, 1) != 0) { rc = -1; break; }
		}
		buf += bytes;
		lba += sectors;
		count -= sectors;
	}
	ufs_io_unlock(h);
	return rc;
}

static int lu_read(struct block_device *dev, u64 lba, u32 count, void *buffer)
{
	return lu_transfer(dev, lba, count, buffer, 0);
}

static int lu_write(struct block_device *dev, u64 lba, u32 count, const void *buffer)
{
	return lu_transfer(dev, lba, count, (u8 *)(usize)buffer, 1);
}

static int lu_flush(struct block_device *dev)
{
	struct ufs_lu *lu = (struct ufs_lu *)dev->priv;
	u8 cdb[10];

	if (!lu->rw_count)
		return 0;       /* nothing on this LUN can have been written */
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = SCSI_SYNC_CACHE10;
	ufs_io_lock(lu->host);
	int rc = scsi_exec(lu->host, lu->lun, cdb, 10, UTRD_DD_NONE, 0);
	ufs_io_unlock(lu->host);
	return rc == 0 ? 0 : -1;
}

/* Is `name` one of the comma-separated entries of `list`? */
static int in_list(const char *list, const char *name)
{
	usize n = strlen(name);

	if (!n)
		return 0;
	for (const char *p = list; *p;) {
		const char *e = p;
		while (*e && *e != ',')
			e++;
		if ((usize)(e - p) == n && memcmp(p, name, n) == 0)
			return 1;
		p = *e ? e + 1 : e;
	}
	return 0;
}

/* Open the write fence for the partitions that may be written: see the comment
 * at the top of this file. */
static void lu_open_writable_partitions(struct ufs_lu *lu)
{
	char allow[256];
	u32 parts = 0;

	if (!bootinfo_get_kv("b1nix.ufs-rw", allow, sizeof(allow)))
		allow[0] = 0;
	for (usize i = 0; i < blk_count(); i++) {
		struct block_device *p = blk_at(i);
		char label[32];

		if (!p || !p->name || blk_partition_parent(p) != &lu->blk)
			continue;
		parts++;
		const char *gpt = blk_partition_label(p);
		int named = gpt && in_list(allow, gpt);
		int ours = blk_probe_label(p, label, sizeof(label)) == 0 &&
		           strcmp(label, "b1nix-root") == 0;
		if (!named && !ours) {
			/* Read-only to everything above: a write the fence
			 * refuses only at write-back stays dirty in the block
			 * cache for ever, and every later flush and every read
			 * of the partition waits on it. */
			p->write_blocks = 0;
			continue;
		}
		if (lu->rw_count >= sizeof(lu->rw) / sizeof(lu->rw[0]))
			break;
		lu->rw[lu->rw_count].start = blk_partition_start(p);
		lu->rw[lu->rw_count].count = p->block_count;
		lu->rw_count++;
		console_write("ufs: ");
		console_write(p->name);
		console_write(" (");
		console_write(gpt ? gpt : "");
		console_write(") writable: ");
		console_write(named ? "b1nix.ufs-rw" : "label b1nix-root");
		console_write("\n");
	}
	/* `gpt` in the list opens the partition tables themselves: the header
	 * and entries at the front of the LUN and their backup at its end.
	 * That is where an A/B bootloader keeps the slot's tries and success
	 * bits, which only a write there can reset. */
	if (in_list(allow, "gpt") && lu->rw_count + 2 <= sizeof(lu->rw) / sizeof(lu->rw[0]) &&
	    lu->lb_count > 2 * UFS_GPT_BLOCKS) {
		lu->rw[lu->rw_count].start = (u64)lu->ratio;
		lu->rw[lu->rw_count].count = (u64)(UFS_GPT_BLOCKS - 1) * lu->ratio;
		lu->rw_count++;
		lu->rw[lu->rw_count].start = (lu->lb_count - UFS_GPT_BLOCKS) * lu->ratio;
		lu->rw[lu->rw_count].count = (u64)UFS_GPT_BLOCKS * lu->ratio;
		lu->rw_count++;
		console_write("ufs: ");
		console_write(lu->blk.name);
		console_write(" GPT writable: b1nix.ufs-rw\n");
	}
	console_write("ufs: ");
	console_write(lu->blk.name);
	console_write(" lun ");
	console_write_dec(lu->lun);
	console_write(": ");
	console_write_dec(parts);
	console_write(" partitions, ");
	console_write_dec(lu->lb_count * lu->lb_size / (1024u * 1024u));
	console_write(" MiB, block ");
	console_write_dec(lu->lb_size);
	console_write("\n");
}

static int lu_read_capacity(struct ufs_host *h, u8 lun, u64 *count, u32 *bsize)
{
	u8 cdb[16];

	/* The first command to a LUN after a reset reports UNIT ATTENTION; that
	 * is a status, not an absence. */
	memset(cdb, 0, sizeof(cdb));
	int st = -1;
	for (int tries = 0; tries < 4 && st != 0; tries++) {
		st = scsi_exec(h, lun, cdb, 6, UTRD_DD_NONE, 0);
		if (st < 0)
			return -1;
	}
	if (st != 0)
		return -1;

	cdb[0] = SCSI_READ_CAPACITY10;
	memset(h->bounce, 0, 32);
	if (scsi_exec(h, lun, cdb, 10, UTRD_DD_FROM_DEV, 8) != 0)
		return -1;
	u64 last = get_be32(h->bounce);
	u32 bs = get_be32(h->bounce + 4);
	if (last == 0xffffffffull) {
		memset(cdb, 0, sizeof(cdb));
		cdb[0] = SCSI_SERVICE_IN16;
		cdb[1] = SCSI_SAI_READ_CAP16;
		put_be32(cdb + 10, 32);
		if (scsi_exec(h, lun, cdb, 16, UTRD_DD_FROM_DEV, 32) != 0)
			return -1;
		last = get_be64(h->bounce);
		bs = get_be32(h->bounce + 8);
	}
	if (bs < 512 || bs > UFS_PAGE || (bs & (bs - 1)) || last == 0)
		return -1;
	*count = last + 1;
	*bsize = bs;
	return 0;
}

/* ── Controller bring-up ─────────────────────────────────────────────────── */

static void program_lists(struct ufs_host *h)
{
	wr(h, REG_UTRLRSR, 0);
	wr(h, REG_UTMRLRSR, 0);
	wr(h, REG_UTRLBA, (u32)h->utrl_phys);
	wr(h, REG_UTRLBAU, (u32)(h->utrl_phys >> 32));
	wr(h, REG_UTMRLBA, (u32)h->utmrl_phys);
	wr(h, REG_UTMRLBAU, (u32)(h->utmrl_phys >> 32));
	wr(h, REG_UTRLRSR, 1);
	wr(h, REG_UTMRLRSR, 1);
}

/* Keep the link the bootloader left up: stop both lists, point them at our
 * memory, start them, and prove the device still answers. */
static int try_inherit(struct ufs_host *h)
{
	u32 hce = rd(h, REG_HCE), hcs = rd(h, REG_HCS);

#define H_MARK(n) do { if (h->is_qcom) BOOTMARK(n); } while (0)
	if (!(hce & 1u) || (hcs & HCS_READY) != HCS_READY)
		return -1;
	H_MARK(220);

	/* Whatever the previous owner left in flight is not ours to finish. */
	if (rd(h, REG_UTRLDBR)) {
		wr(h, REG_UTRLCLR, 0);
		wait_reg(h, REG_UTRLDBR, 0xffffffffu, 0, 1000);
	}
	program_lists(h);
	wr(h, REG_IE, 0);
	wr(h, REG_IS, rd(h, REG_IS));
	for (int i = 0; i < 3; i++) {
		if (ufs_nop(h) == 0) {
			h->inherited = 1;
			return 0;
		}
	}
	ufs_msg(h, "live link did not answer a NOP\n");
	return -1;
}

static int full_init(struct ufs_host *h)
{
	if (rd(h, REG_HCE) & 1u) {
		wr(h, REG_HCE, 0);
		if (wait_reg(h, REG_HCE, 1u, 0, 1000) != 0) {
			ufs_msg(h, "HCE did not clear\n");
			return -1;
		}
	}
	if (h->is_qcom)
		wr(h, QCOM_REG_CFG1, rd(h, QCOM_REG_CFG1) | QCOM_QUNIPRO_SEL);
	wr(h, REG_HCE, 1);
	if (wait_reg(h, REG_HCE, 1u, 1u, 1000) != 0) {
		ufs_msg(h, "HCE did not set\n");
		return -1;
	}
	wr(h, REG_IE, 0);

	int linked = 0;
	for (int attempt = 0; attempt < 10 && !linked; attempt++) {
		if (h->is_qcom)
			dme_set(h, PA_LOCAL_TX_LCC_ENABLE, 0);
		if (uic_cmd(h, UIC_DME_LINKSTARTUP, 0, 0, 0, 0) != 0)
			continue;
		if (wait_reg(h, REG_HCS, HCS_DP, HCS_DP, 100) == 0)
			linked = 1;
	}
	if (!linked) {
		ufs_msg_hex(h, "link startup failed, HCS ", rd(h, REG_HCS));
		return -1;
	}
	if (wait_reg(h, REG_HCS, HCS_READY, HCS_READY, 1000) != 0) {
		ufs_msg_hex(h, "lists never became ready, HCS ", rd(h, REG_HCS));
		return -1;
	}
	wr(h, REG_IS, rd(h, REG_IS));
	program_lists(h);

	int nop = -1;
	for (int i = 0; i < 10 && nop != 0; i++)
		nop = ufs_nop(h);
	if (nop != 0) {
		ufs_msg(h, "device did not answer a NOP\n");
		return -1;
	}
	/* fDeviceInit: set it, then wait for the device to clear it. */
	if (query_flag(h, QUERY_FN_WRITE, QUERY_OP_SET_FLAG,
	               QUERY_FLAG_DEVICE_INIT, 0) != 0) {
		ufs_msg(h, "fDeviceInit set failed\n");
		return -1;
	}
	for (int i = 0; i < 1000; i++) {
		u8 v = 1;

		if (query_flag(h, QUERY_FN_READ, QUERY_OP_READ_FLAG,
		               QUERY_FLAG_DEVICE_INIT, &v) == 0 && v == 0)
			return 0;
		arch_udelay(1000);
	}
	ufs_msg(h, "device never finished initialising\n");
	return -1;
}

/* Qualcomm: make sure the controller's power domain and clocks are on before
 * its first register read. ABL normally leaves them so; if it did not, the
 * read would never return. GDSC: clear SW_COLLAPSE (bit 0); this domain reports
 * "powered" in its CFG register (GDSCR + 4, POWER_UP_COMPLETE bit 16 — the
 * POLL_CFG_GDSCR flag in Linux's gcc-sm8150), older ones in PWR_ON (bit 31).
 * Branch clocks: set CLK_ENABLE (bit 0). */
static void qcom_ufs_clocks_on(u64 gcc)
{
	static const u32 branches[] = {
		0x77014, /* gcc_ufs_phy_ahb_clk */
		0x77010, /* gcc_ufs_phy_axi_clk */
		0x77058, /* gcc_ufs_phy_unipro_core_clk */
		0x77090, /* gcc_ufs_phy_phy_aux_clk */
		0x77018, /* gcc_ufs_phy_tx_symbol_0_clk */
		0x7701c, /* gcc_ufs_phy_rx_symbol_0_clk */
		0x770ac, /* gcc_ufs_phy_rx_symbol_1_clk */
		0x7705c, /* gcc_ufs_phy_ice_core_clk */
	};
	volatile u32 *gdsc = (volatile u32 *)(usize)(gcc + 0x77004);
	volatile u32 *cfg = (volatile u32 *)(usize)(gcc + 0x77008);

	console_write("ufs: gcc ufs_phy_gdsc=0x");
	console_write_hex64(*gdsc);
	console_write(" cfg=0x");
	console_write_hex64(*cfg);
	if (*gdsc & 1u) {
		*gdsc &= ~1u;
		for (int i = 0; i < 1500 && !(*gdsc & (1u << 31)) && !(*cfg & (1u << 16)); i++)
			arch_udelay(100);
	}
	if (!(*gdsc & (1u << 31)) && !(*cfg & (1u << 16)))
		console_write(" (power-up not reported; trying anyway)");
	for (u32 i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		volatile u32 *cbcr = (volatile u32 *)(usize)(gcc + branches[i]);

		if (!(*cbcr & 1u))
			*cbcr |= 1u;
	}
	arch_udelay(1000);
	console_write(" axi=0x");
	console_write_hex64(*(volatile u32 *)(usize)(gcc + 0x77010));
	console_write("\n");
}

/* Read-only report of the Qualcomm apps SMMU (0x15000000, ARM SMMUv2/500) as the
 * bootloader left it, for the streams of UFS (0x300) and USB (0x140): which
 * context bank each is routed to, that bank's translation setup, and the
 * page-table entries covering a page the controller may reach (the
 * bootloader's 0xffffe000) and one it may not (kernel memory at 0xab4e4000).
 * This is the data needed to add mappings instead of living in two pages. */
static u64 smmu_pte_read(u64 table, u32 index)
{
	return *(volatile u64 *)(usize)(table + vmm_direct_map_base() + (u64)index * 8);
}

static void smmu_walk(u64 ttbr, u32 t0sz, u64 iova)
{
	/* 4 KiB granule. The first level is decided by T0SZ as in the ARM ARM:
	 * 25..33 starts at level 1, 16..24 at level 0. */
	int level = t0sz >= 25 ? 1 : 0;
	u64 table = ttbr & 0x0000fffffffff000ull;

	console_write("smmu:   walk 0x");
	console_write_hex64(iova);
	console_write("\n");
	for (; level <= 3 && table; level++) {
		u32 shift = 39 - 9 * (u32)level;
		u32 idx = (u32)((iova >> shift) & 0x1ff);
		u64 e = smmu_pte_read(table, idx);

		console_write("smmu:     L");
		console_write_dec((u64)level);
		console_write("[");
		console_write_dec(idx);
		console_write("] @0x");
		console_write_hex64(table);
		console_write(" = 0x");
		console_write_hex64(e);
		console_write("\n");
		if (!(e & 1) || level == 3 || !(e & 2))
			return;
		table = e & 0x0000fffffffff000ull;
	}
}

static void qcom_smmu_report(void)
{
	const u64 base = 0x15000000ull;
	const u64 cb_base = base + 0x80000ull;
	const u16 sids[2] = { 0x300, 0x140 };
	u32 id0 = *(volatile u32 *)(usize)(base + 0x20);
	u32 id1 = *(volatile u32 *)(usize)(base + 0x24);

	console_write("smmu: sCR0=0x");
	console_write_hex64(*(volatile u32 *)(usize)base);
	console_write(" ID0=0x");
	console_write_hex64(id0);
	console_write(" ID1=0x");
	console_write_hex64(id1);
	console_write(" ID2=0x");
	console_write_hex64(*(volatile u32 *)(usize)(base + 0x28));
	console_write("\n");

	u32 nsmr = id0 & 0xff;
	for (u32 i = 0; i < nsmr; i++) {
		u32 smr = *(volatile u32 *)(usize)(base + 0x800 + i * 4);

		if (!(smr & (1u << 31)))
			continue;
		u16 id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
		for (u32 k = 0; k < 2; k++) {
			if ((id & ~mask) != (sids[k] & ~mask))
				continue;
			u32 s2cr = *(volatile u32 *)(usize)(base + 0xc00 + i * 4);
			u32 cbndx = s2cr & 0xff;

			console_write("smmu: sid 0x");
			console_write_hex64(sids[k]);
			console_write(" smr[");
			console_write_dec(i);
			console_write("]=0x");
			console_write_hex64(smr);
			console_write(" s2cr=0x");
			console_write_hex64(s2cr);
			console_write("\n");
			if (((s2cr >> 16) & 3) != 0)
				continue; /* not translating */
			u64 cb = cb_base + (u64)cbndx * 0x1000;
			u32 sctlr = *(volatile u32 *)(usize)(cb + 0x0);
			u32 tcr = *(volatile u32 *)(usize)(cb + 0x30);
			u64 ttbr0 = *(volatile u64 *)(usize)(cb + 0x20);

			console_write("smmu:   cb");
			console_write_dec(cbndx);
			console_write(" cbar=0x");
			console_write_hex64(*(volatile u32 *)(usize)(base + 0x1000 + cbndx * 4));
			console_write(" sctlr=0x");
			console_write_hex64(sctlr);
			console_write(" tcr=0x");
			console_write_hex64(tcr);
			console_write(" tcr2=0x");
			console_write_hex64(*(volatile u32 *)(usize)(cb + 0x10));
			console_write(" ttbr0=0x");
			console_write_hex64(ttbr0);
			console_write(" mair0=0x");
			console_write_hex64(*(volatile u32 *)(usize)(cb + 0x38));
			console_write(" fsr=0x");
			console_write_hex64(*(volatile u32 *)(usize)(cb + 0x58));
			console_write("\n");
			if ((sctlr & 1) && (tcr & (1u << 31))) {
				smmu_walk(ttbr0, tcr & 0x3f, 0xffffe000ull);
				smmu_walk(ttbr0, tcr & 0x3f, 0xab4e4000ull);
			}
		}
	}
}

static void probe_host(u64 base, int is_qcom, int irq)
{
	if (g_host_count >= sizeof(g_hosts) / sizeof(g_hosts[0]))
		return;
	struct ufs_host *h = &g_hosts[g_host_count];

	memset(h, 0, sizeof(*h));
	h->base = base;
	h->is_qcom = is_qcom;
	h->index = g_host_count;
	h->irq = -1;

	/* Panel readout on the phone: 2xx is the UFS step reached. A step is held
	 * on screen for 300 ms, because a register the secure world forbids does
	 * not fault — the phone resets, and a number painted a moment before the
	 * reset never reaches the panel. */
#define UFS_MARK(n) do { if (is_qcom) BOOTMARK(n); } while (0)
	UFS_MARK(203);
	h->cap = rd(h, REG_CAP);
	UFS_MARK(204);
	u32 ver = rd(h, REG_VER);
	UFS_MARK(210);
	u32 hce = rd(h, REG_HCE);
	UFS_MARK(211);
	u32 hcs = rd(h, REG_HCS);
	UFS_MARK(212);
	u32 hw = 0;
	if (is_qcom) {
		/* Rows 1-3 of the readout: HCE, HCS low bits, then (below) where
		 * the DMA memory came from. */
		fb_boot_num(1, (int)hce);
		fb_boot_num(2, (int)(hcs & 0x1ff));
		hw = rd(h, QCOM_REG_HW_VERSION);
		UFS_MARK(213);
	}
	ufs_msg_hex(h, "controller at ", base);
	console_write("ufs: cap=0x");
	console_write_hex64(h->cap);
	console_write(" ver=0x");
	console_write_hex64(ver);
	console_write(" hce=");
	console_write_dec(hce);
	console_write(" hcs=0x");
	console_write_hex64(hcs);
	if (is_qcom) {
		console_write(" qcom-hw=0x");
		console_write_hex64(hw);
	}
	console_write("\n");
	UFS_MARK(214);
	if (h->cap == 0 || h->cap == 0xffffffffu) {
		ufs_msg(h, "no controller answers there\n");
		return;
	}

	if (is_qcom) {
		/* Measured on the Xperia 5 (b1nix.ufs-dma-probe, since removed): the
		 * controller's DMA reaches 0xe9000000 and everything above it that
		 * was tried up to the bootloader's own pages at 0xffffe000, and a
		 * DMA to 0xc0000000 — or to the page allocator's gigabyte at
		 * 0xa9000000 — resets the phone. The apps SMMU does not translate
		 * this stream (context bank SCTLR.M=0), so the fence is the
		 * hypervisor's. Carve the lists and the bounce buffer out of the
		 * allowed hole, which nothing else in this kernel touches: it is
		 * above the page allocator and below the ramoops/debug carveouts at
		 * 0xffb00000. */
		const u64 win = QCOM_UFS_DMA_BASE;

		h->utrl_phys = win;
		h->utmrl_phys = win + UFS_PAGE;
		h->ucd_phys = win + 2 * UFS_PAGE;
		h->bounce_phys = win + 4 * UFS_PAGE;
		memset((void *)(usize)(win + vmm_direct_map_base()), 0,
		       (4 + UFS_BOUNCE_FRAMES) * UFS_PAGE);
	} else {
		h->utrl_phys = alloc_dma(1);
		h->utmrl_phys = alloc_dma(1);
		h->ucd_phys = alloc_dma(1);
		h->bounce_phys = alloc_dma(UFS_BOUNCE_FRAMES);
	}
	h->bounce_bytes = UFS_BOUNCE_FRAMES * UFS_PAGE;
	UFS_MARK(217);
	if (!h->utrl_phys || !h->utmrl_phys || !h->ucd_phys || !h->bounce_phys) {
		ufs_msg(h, "no DMA memory below 4 GiB\n");
		return;
	}
	h->utrl = (u8 *)(usize)(h->utrl_phys + vmm_direct_map_base());
	h->ucd = (u8 *)(usize)(h->ucd_phys + vmm_direct_map_base());
	h->bounce = (u8 *)(usize)(h->bounce_phys + vmm_direct_map_base());

	int up = -1;
	UFS_MARK(205);
	if (!bootinfo_has_flag("b1nix.ufs-reset"))
		up = try_inherit(h);
	UFS_MARK(up == 0 ? 207 : 206);
	/* Not on Qualcomm: the full sequence programs lists in kernel memory,
	 * which the SMMU there refuses (see try_inherit) by resetting the phone. */
	if (up != 0 && !is_qcom)
		up = full_init(h);
	if (up != 0)
		return;
	UFS_MARK(208);
	ufs_msg(h, h->inherited ? "kept the bootloader's link\n" :
	                          "link started from reset\n");
	g_host_count++;

	/* Completion interrupts, and only those: IE is written whole, because a
	 * source left enabled by the bootloader (a UIC error, say) is one this
	 * handler never acknowledges, and a level-triggered line nobody clears
	 * keeps the CPU in the interrupt handler for good. */
	if (irq > 0 && irq_register_handler((u32)irq, ufs_irq, h) == 0) {
		h->irq = irq;
		wr(h, REG_IS, IS_UTRCS);
		wr(h, REG_IE, IE_UTRCE);
		irq_unmask((u32)irq);
		ufs_msg_hex(h, "completion interrupt ", (u64)irq);
	}

	for (u8 lun = 0; lun < UFS_MAX_LUNS; lun++) {
		u64 count;
		u32 bsize;

		if (g_lu_count >= sizeof(g_lus) / sizeof(g_lus[0]))
			break;
		h->quiet = 1;
		int absent = lu_read_capacity(h, lun, &count, &bsize) != 0;
		h->quiet = 0;
		if (absent)
			continue;
		struct ufs_lu *lu = &g_lus[g_lu_count++];

		memset(lu, 0, sizeof(*lu));
		lu->host = h;
		lu->lun = lun;
		lu->lb_size = bsize;
		lu->ratio = bsize / 512u;
		lu->lb_count = count;
		lu->blk.block_size = 512;
		lu->blk.lb_size = bsize;
		lu->blk.block_count = count * lu->ratio;
		lu->blk.read_blocks = lu_read;
		lu->blk.write_blocks = lu_write;
		lu->blk.flush = lu_flush;
		lu->blk.limits.max_sectors = h->bounce_bytes / 512u;
		lu->blk.limits.max_segments = 1;
		lu->blk.priv = lu;
		/* Registration scans the GPT, so the partitions exist once it
		 * returns and the fence can be opened around them. */
		blk_register_disk(&lu->blk, "sd", BLK_BUS_UFS);
		lu_open_writable_partitions(lu);
	}
}

/* ── Kernel log on the phone's flash ───────────────────────────────────────
 *
 * The Xperia 5 has no serial port, a forced power-off clears RAM, and userspace
 * may never get as far as writing a file. So the kernel copies its console log
 * (the ramoops zone, header and all) straight onto the b1nix-root partition,
 * past the end of the filesystem, every two seconds: raw blocks, no ext4, no
 * userspace. Android reads it back with
 *   dd if=/dev/block/by-name/system_a bs=1M skip=<fs size in MiB> count=1
 * The area starts at the filesystem's end rounded up to 1 MiB and is only used
 * when the partition has at least 1 MiB to spare there. */
static struct block_device *g_log_part;
static u64 g_log_sector;

static void ufs_log_flush(void)
{
	u32 size = 0;
	extern const u8 *console_ramoops_zone(u32 *size);
	const u8 *zone = console_ramoops_zone(&size);

	if (!g_log_part || !zone || !size)
		return;
	/* Only when something was logged since the last copy: the header's
	 * write position moves with every byte. 256 KiB every two seconds of an
	 * idle phone was flash wear for nothing. */
	static u32 last_pos = ~0u;
	u32 pos = ((const volatile u32 *)zone)[1];

	if (pos == last_pos)
		return;
	last_pos = pos;
	static u8 buf[0x40000];

	memcpy(buf, zone, size);
	/* Through the cache like any write, then pushed out now: the point is
	 * that it is on the flash before the power goes. */
	g_log_part->write_blocks(g_log_part, g_log_sector, size / 512, buf);
	blk_cache_flush(blk_partition_parent(g_log_part));
}

/* The same copy from a panic: no block cache, no sleeping. If the UFS host is
 * mid-command (possibly the very command that panicked), leave it alone --
 * waiting would hang, and a half-issued command cannot be interleaved with. */
void ufs_log_panic_flush(void)
{
	u32 size = 0;
	extern const u8 *console_ramoops_zone(u32 *size);
	const u8 *zone = console_ramoops_zone(&size);
	static u8 buf[0x40000];

	if (!g_log_part || !zone || !size || size > sizeof(buf))
		return;
	struct block_device *disk = blk_partition_parent(g_log_part);
	struct ufs_lu *lu = disk ? (struct ufs_lu *)disk->priv : 0;

	if (!lu || __atomic_load_n(&lu->host->io_busy, __ATOMIC_ACQUIRE))
		return;
	memcpy(buf, zone, size);
	g_log_part->write_blocks(g_log_part, g_log_sector, size / 512, buf);
}

/* The same copy from the reboot path, after the other CPUs were parked: one
 * of them may have been mid-command and holds the I/O lock for ever, so this
 * waits for the doorbell to clear (the command itself finishes in hardware)
 * and takes the lock over. */
void ufs_log_reboot_flush(void)
{
	u32 size = 0;
	extern const u8 *console_ramoops_zone(u32 *size);
	const u8 *zone = console_ramoops_zone(&size);
	static u8 buf[0x40000];

	if (!g_log_part || !zone || !size || size > sizeof(buf))
		return;
	struct block_device *disk = blk_partition_parent(g_log_part);
	struct ufs_lu *lu = disk ? (struct ufs_lu *)disk->priv : 0;

	if (!lu)
		return;
	for (u32 t = 0; t < 5000 && (rd(lu->host, REG_UTRLDBR) & 1u); t++)
		arch_udelay(100);
	if (rd(lu->host, REG_UTRLDBR) & 1u)
		return;
	__sync_lock_release(&lu->host->io_busy);
	memcpy(buf, zone, size);
	g_log_part->write_blocks(g_log_part, g_log_sector, size / 512, buf);
}

static void ufs_log_thread(void *arg)
{
	(void)arg;
	for (;;) {
		ufs_log_flush();
		scheduler_sleep_ticks(2 * sched_tick_hz());
	}
}


static void ufs_log_start(void)
{
	for (usize i = 0; i < blk_count() && !g_log_part; i++) {
		struct block_device *p = blk_at(i);
		char label[32];

		if (!p || p->bus != BLK_BUS_UFS || !blk_is_partition(p) ||
		    blk_probe_label(p, label, sizeof(label)) != 0 ||
		    strcmp(label, "b1nix-root") != 0)
			continue;
		u8 sb[512];
		/* ext superblock at byte 1024: blocks count (0x04), log block size (0x18). */
		if (p->read_blocks(p, 2, 1, sb) != 0)
			continue;
		u64 blocks = (u32)sb[4] | ((u32)sb[5] << 8) | ((u32)sb[6] << 16) | ((u32)sb[7] << 24);
		u32 logbs = (u32)sb[0x18];
		u64 fs_bytes = blocks << (10 + logbs);
		u64 start = (fs_bytes + 0xfffff) & ~0xfffffull;
		if (start / 512 + 0x40000 / 512 > p->block_count)
			continue;
		g_log_part = p;
		g_log_sector = start / 512;
		/* What is there now is the previous boot's last mirror: keep it
		 * before the first flush replaces it (/proc/last_kmsg). */
#ifdef __aarch64__
		{
			extern void console_keep_previous_zone(const u8 *zone);
			static u8 prev[0x40000];

			if (p->read_blocks(p, g_log_sector, sizeof(prev) / 512, prev) == 0)
				console_keep_previous_zone(prev);
		}
#endif
		console_write("ufs: kernel log mirrored to ");
		console_write(p->name);
		console_write(" at MiB ");
		console_write_dec(start >> 20);
		console_write("\n");
		kthread_create("ufs-log", ufs_log_thread, 0);
	}
}

void ufs_init(void)
{
	struct pci_device_info pci;

	for (u8 idx = 0;; idx++) {
		if (!pci_find_class(0x01, 0x09, idx, &pci))
			break;
		struct pci_bar bar;

		if (pci_bar_read(pci.bus, pci.slot, pci.func, 0, &bar) != 0 ||
		    !bar.valid || bar.is_io || !bar.base)
			continue;
		pci_enable_decode(pci.bus, pci.slot, pci.func);
		pci_enable_bus_master(pci.bus, pci.slot, pci.func);
		pci_bind_driver(&pci, "ufshcd");
		probe_host(vmm_direct_map_base() + bar.base, 0,
		           pci_intx_line(pci.bus, pci.slot, pci.func));
	}

#if defined(__aarch64__)
	if (fdt_ufshc_base() && bootinfo_has_flag("b1nix.ufs")) {
		/* Through a mapping, not the physical address: GCC starts at
		 * 1 MiB, inside the first 2 MiB the identity map leaves unmapped
		 * so a null dereference still faults. Read directly it took a
		 * level-2 translation fault on the first GDSC read. */
		u64 gcc = fdt_qcom_gcc_base() ?
			(u64)(usize)vmm_map_mmio(fdt_qcom_gcc_base(), 0x1f0000, VMM_WRITABLE) : 0;

		console_write("ufs: qcom,ufshc at 0x");
		console_write_hex64(fdt_ufshc_base());
		console_write("\n");
		BOOTMARK(201);
		if (gcc)
			qcom_ufs_clocks_on(gcc);
		BOOTMARK(202);
		qcom_smmu_report();
		probe_host(fdt_ufshc_base(), 1, QCOM_UFS_IRQ);
		BOOTMARK(209);
		ufs_log_start();
	}
#endif
}

/* ── Self-test (b1nix.test=1) ────────────────────────────────────────────── */

static struct block_device *find_partition(const char *gpt_name)
{
	for (usize i = 0; i < blk_count(); i++) {
		struct block_device *p = blk_at(i);
		const char *n = p ? blk_partition_label(p) : 0;

		if (n && p->bus == BLK_BUS_UFS && strcmp(n, gpt_name) == 0)
			return p;
	}
	return 0;
}

static void mark(int ok, const char *what)
{
	console_write(ok ? "UFS-SMOKE: ok " : "UFS-SMOKE: FAIL ");
	console_write(what);
	console_write("\n");
}

/*
 * The smoke disk is a 4 KiB-block LUN with a GPT holding `ufsroot` (an ext4
 * made on the host, label ufs-smoke) and `ufsfenced`, with
 * b1nix.ufs-rw=ufsroot on the command line.
 */
void ufs_selftest(void)
{
	if (!bootinfo_has_flag("b1nix.test=1") || g_lu_count == 0)
		return;

	struct ufs_lu *lu = &g_lus[0];
	mark(lu->lb_size == 4096, "lun-4k-block");

	struct block_device *root = find_partition("ufsroot");
	struct block_device *fenced = find_partition("ufsfenced");
	mark(root && fenced, "gpt-4k-names");
	if (!root || !fenced)
		return;

	char label[32];
	mark(blk_probe_label(root, label, sizeof(label)) == 0 &&
	     strcmp(label, "ufs-smoke") == 0, "ext4-label-through-gpt");

	/* Partitions are cached under their parent LUN, which is what gets
	 * dropped to force a read back off the medium. */
	struct block_device *disk = blk_partition_parent(root);
	u8 *pat = kmalloc(1536), *back = kmalloc(1536);
	if (!pat || !back || !disk)
		return;
	/* A run starting mid logical block, so the partial-block write path
	 * (read, patch, write back) is the one exercised. */
	u64 s = root->block_count - 16 + 3;
	for (u32 i = 0; i < 1536; i++)
		pat[i] = (u8)(i * 7 + 13);
	int wrote = blk_write_cached(root, s, 3, pat) >= 0;
	blk_cache_flush(disk);
	blk_cache_invalidate(disk);
	int readback = wrote && blk_read_cached(root, s, 3, back) >= 0 &&
	               memcmp(pat, back, 1536) == 0;
	mark(readback, "write-readback-partial-block");

	/* Straight at the driver, below the cache: a sector inside ufsroot is
	 * accepted, the first sector of ufsfenced and sector 0 of the LUN (the
	 * GPT itself) are not. The refused writes carry the bytes already there,
	 * so a broken fence cannot damage the disk it is failing to protect. */
	u64 fstart = blk_partition_start(fenced);
	int refused = disk->read_blocks(disk, fstart, 1, back) == 0 &&
	              disk->write_blocks(disk, fstart, 1, back) < 0 &&
	              disk->read_blocks(disk, 0, 1, back) == 0 &&
	              disk->write_blocks(disk, 0, 1, back) < 0;
	u64 rstart = blk_partition_start(root) + s;
	int allowed = disk->read_blocks(disk, rstart, 1, back) == 0 &&
	              disk->write_blocks(disk, rstart, 1, back) == 0;
	mark(refused && allowed && !fenced->write_blocks,
	     "fence-refuses-unlisted-partition");

	/* Hand the live controller to probe again, as a kernel booted by a
	 * bootloader that used the disk finds it: the inherit path must keep the
	 * link and still read the same bytes. */
	struct ufs_host *h = lu->host;
	ufs_io_lock(h);
	h->inherited = 0;
	int kept = try_inherit(h) == 0 && h->inherited;
	ufs_io_unlock(h);
	blk_cache_invalidate(disk);
	memset(back, 0, 1536);
	mark(kept && blk_read_cached(root, s, 3, back) >= 0 &&
	     memcmp(pat, back, 1536) == 0, "inherit-live-link");
	kfree(pat);
	kfree(back);
}
