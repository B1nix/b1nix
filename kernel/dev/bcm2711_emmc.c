/*
 * BCM2711 EMMC2 — the SD card controller a Raspberry Pi 4 boots from.
 *
 * Despite the Broadcom compatible string this is an ordinary SD Host
 * Controller (SDHCI 3.0): the same register block, the same command/response
 * protocol and the same card initialisation sequence as any other SDHCI part.
 * The only Broadcom-specific thing about it is where it lives, and that comes
 * out of the device tree.
 *
 * What this driver does:
 *   - resets the host, powers the bus, and runs the SD card initialisation
 *     sequence (CMD0, CMD8, ACMD41, CMD2, CMD3, CMD9, CMD7, CMD16);
 *   - reads the capacity out of the CSD, both the v2 (SDHC/SDXC) and v1
 *     (standard capacity) encodings;
 *   - switches to a four-bit bus and steps the clock up from the 400 kHz
 *     identification rate to 25 MHz;
 *   - reads and writes multiple blocks with CMD18/CMD25 and auto-CMD12;
 *   - registers the card as a block device named mmcblk*.
 *
 * What it does not do:
 *   - DMA. Every transfer goes through the buffer data port a word at a time.
 *     SDMA and ADMA2 are what a fast driver would use; this one is correct and
 *     simple, and the block cache above it does the batching that matters.
 *   - Interrupts. The controller's IRQ is left masked at the GIC and every wait
 *     is a bounded poll of the interrupt STATUS register, which is how the rest
 *     of this kernel's storage bring-up paths work.
 *   - High-speed timing beyond 25 MHz (no SDR50/SDR104/DDR50 tuning), eMMC
 *     devices (this is the SD protocol only), write protect, or hotplug —
 *     the card is enumerated once at boot.
 */

#include <b1nix/blk.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/types.h>
#include <string.h>

#if defined(__aarch64__)

/* ── SD Host Controller register block ────────────────────────────────────── */

#define SDHCI_ARG2            0x00
#define SDHCI_BLOCK_SIZE      0x04  /* [11:0] block size, [31:16] block count */
#define SDHCI_ARGUMENT        0x08
#define SDHCI_TRANSFER_MODE   0x0c  /* [15:0] transfer mode, [31:16] command */
#define SDHCI_RESPONSE0       0x10
#define SDHCI_BUFFER          0x20
#define SDHCI_PRESENT_STATE   0x24
#define SDHCI_HOST_CONTROL    0x28  /* b0 host ctl1, b1 power, b2 block gap */
#define SDHCI_CLOCK_CONTROL   0x2c  /* [15:0] clock, [23:16] timeout, [31:24] reset */
#define SDHCI_INT_STATUS      0x30  /* [15:0] normal, [31:16] error */
#define SDHCI_INT_ENABLE      0x34
#define SDHCI_SIGNAL_ENABLE   0x38
#define SDHCI_CAPABILITIES    0x40

/* Present state */
#define PRESENT_CMD_INHIBIT   (1u << 0)
#define PRESENT_DATA_INHIBIT  (1u << 1)
#define PRESENT_WRITE_READY   (1u << 10)
#define PRESENT_READ_READY    (1u << 11)
#define PRESENT_CARD_INSERTED (1u << 16)

/* Normal interrupt status (the low half of SDHCI_INT_STATUS) */
#define INT_CMD_COMPLETE      (1u << 0)
#define INT_XFER_COMPLETE     (1u << 1)
#define INT_SPACE_AVAIL       (1u << 4)
#define INT_DATA_AVAIL        (1u << 5)
#define INT_ERROR             (1u << 15)
/* Error interrupt status lives in the high half; this is its mask there. */
#define INT_ERROR_MASK        0xffff0000u

/* Clock control */
#define CLOCK_INTERNAL_EN     (1u << 0)
#define CLOCK_INTERNAL_STABLE (1u << 1)
#define CLOCK_SD_EN           (1u << 2)

/* Software reset, the top byte of SDHCI_CLOCK_CONTROL */
#define RESET_ALL             (1u << 24)
#define RESET_CMD             (1u << 25)
#define RESET_DATA            (1u << 26)

/* Power control, the second byte of SDHCI_HOST_CONTROL */
#define POWER_ON              (1u << 8)
#define POWER_330             (7u << 9)  /* 3.3 V */

/* Host control 1, the first byte of SDHCI_HOST_CONTROL */
#define HOSTCTL_4BIT          (1u << 1)
#define HOSTCTL_HISPEED       (1u << 2)

/* Transfer mode */
#define XFER_BLKCNT_EN        (1u << 1)
#define XFER_AUTO_CMD12       (1u << 2)
#define XFER_READ             (1u << 4)
#define XFER_MULTI            (1u << 5)

/* Command register, the high half of SDHCI_TRANSFER_MODE */
#define CMD_RESP_NONE         0u
#define CMD_RESP_136          1u
#define CMD_RESP_48           2u
#define CMD_RESP_48_BUSY      3u
#define CMD_CRC_CHECK         (1u << 3)
#define CMD_INDEX_CHECK       (1u << 4)
#define CMD_DATA_PRESENT      (1u << 5)

/* SD commands used here. */
#define SD_GO_IDLE            0
#define SD_ALL_SEND_CID       2
#define SD_SEND_RELATIVE_ADDR 3
#define SD_SELECT_CARD        7
#define SD_SEND_IF_COND       8
#define SD_SEND_CSD           9
#define SD_SET_BLOCKLEN       16
#define SD_READ_SINGLE        17
#define SD_READ_MULTIPLE      18
#define SD_WRITE_SINGLE       24
#define SD_WRITE_MULTIPLE     25
#define SD_APP_CMD            55
#define SD_APP_SET_BUS_WIDTH  6
#define SD_APP_SEND_OP_COND   41

#define SD_BLOCK_SIZE         512

struct emmc_host {
	u64 base;
	u32 rca;              /* the card's relative address, from CMD3 */
	int high_capacity;    /* SDHC/SDXC: block-addressed rather than byte */
	u32 base_clock_hz;    /* the controller's own clock, from Capabilities */
	struct block_device blk;
};

static struct emmc_host g_host;
static int g_present;

static inline volatile u32 *emmc_reg(u32 off)
{
	return (volatile u32 *)(usize)(g_host.base + off);
}

static inline u32 emmc_read(u32 off) { return *emmc_reg(off); }
static inline void emmc_write(u32 off, u32 val) { *emmc_reg(off) = val; }

/* The transfer mode and command registers share a word, and a controller
 * starts the command the moment the top byte of that word is written. So the
 * transfer mode has to go in on its own, as a halfword, or the store that sets
 * it up would also fire a CMD0. */
static inline void emmc_write16(u32 off, u16 val)
{
	*(volatile u16 *)(usize)(g_host.base + off) = val;
}

/* A bounded spin. Nothing here knows the CPU's frequency, so like the xHCI
 * driver's the unit is loops rather than microseconds; the counts below are
 * chosen to be generous on any machine that can run this kernel at all. */
static void emmc_delay(u32 loops)
{
	for (u32 i = 0; i < loops; i++)
		__asm__ volatile("yield");
}

#define EMMC_POLL_LIMIT 500000u

/* Wait for every bit in `mask` of the present-state register to be clear. */
static int emmc_wait_present_clear(u32 mask)
{
	for (u32 i = 0; i < EMMC_POLL_LIMIT; i++) {
		if (!(emmc_read(SDHCI_PRESENT_STATE) & mask))
			return 0;
		emmc_delay(1);
	}
	return -1;
}

/* Wait for any bit in `mask` of the interrupt status, or for an error. The
 * bits that were waited for are acknowledged; an error acknowledges the whole
 * error half and resets the command and data lines, because a controller left
 * holding an error refuses every subsequent command. */
static int emmc_wait_int(u32 mask)
{
	for (u32 i = 0; i < EMMC_POLL_LIMIT; i++) {
		u32 status = emmc_read(SDHCI_INT_STATUS);

		if (status & INT_ERROR_MASK) {
			emmc_write(SDHCI_INT_STATUS, status);
			emmc_write(SDHCI_CLOCK_CONTROL,
			           emmc_read(SDHCI_CLOCK_CONTROL) | RESET_CMD | RESET_DATA);
			return -1;
		}
		if (status & mask) {
			emmc_write(SDHCI_INT_STATUS, status & mask);
			return 0;
		}
		emmc_delay(1);
	}
	return -1;
}

/*
 * Issue one command. `resp` receives the response: one word for a 48-bit one,
 * four for the 136-bit CID/CSD. `data_blocks` is zero for a command with no
 * data phase — the caller moves the data itself afterwards, because the
 * buffer-port loop differs between reading and writing.
 */
static int emmc_command(u32 index, u32 arg, u32 resp_type, u32 flags,
                        u32 *resp)
{
	if (emmc_wait_present_clear(PRESENT_CMD_INHIBIT) != 0)
		return -1;
	/* A command with a data phase, or one that answers with busy, also needs
	 * the data line free. */
	if ((flags & CMD_DATA_PRESENT) || resp_type == CMD_RESP_48_BUSY) {
		if (emmc_wait_present_clear(PRESENT_DATA_INHIBIT) != 0)
			return -1;
	}

	/* Clear anything left over, then arm the command. */
	emmc_write(SDHCI_INT_STATUS, emmc_read(SDHCI_INT_STATUS));
	emmc_write(SDHCI_ARGUMENT, arg);
	/* The transfer mode half has already been written by the caller when
	 * there is a data phase; writing the command half is what starts it, so
	 * it is a read-modify-write rather than a plain store. */
	u32 xfer = emmc_read(SDHCI_TRANSFER_MODE) & 0xffffu;
	if (!(flags & CMD_DATA_PRESENT))
		xfer = 0;
	u32 cmd = (index << 8) | resp_type | flags;
	emmc_write(SDHCI_TRANSFER_MODE, (cmd << 16) | xfer);

	if (emmc_wait_int(INT_CMD_COMPLETE) != 0)
		return -1;

	if (resp) {
		if (resp_type == CMD_RESP_136) {
			for (u32 i = 0; i < 4; i++)
				resp[i] = emmc_read(SDHCI_RESPONSE0 + i * 4);
		} else if (resp_type != CMD_RESP_NONE) {
			resp[0] = emmc_read(SDHCI_RESPONSE0);
		}
	}
	return 0;
}

/* An application command: CMD55 with the card's address, then the ACMD. */
static int emmc_app_command(u32 index, u32 arg, u32 resp_type, u32 flags,
                            u32 *resp)
{
	u32 r1;

	if (emmc_command(SD_APP_CMD, g_host.rca << 16, CMD_RESP_48,
	                 CMD_CRC_CHECK | CMD_INDEX_CHECK, &r1) != 0)
		return -1;
	return emmc_command(index, arg, resp_type, flags, resp);
}

/*
 * Set the card clock. `hz` is what is asked for; the controller divides its own
 * base clock by an even number, so what comes out is the first available rate
 * at or below it. SDHCI 3.0's divider is ten bits split across two fields.
 */
static int emmc_set_clock(u32 hz)
{
	u32 base = g_host.base_clock_hz;
	u32 divider = 0;

	if (!base || !hz)
		return -1;
	/* value = base / (2 * divider); divider 0 means "base clock, undivided". */
	while (base / ((divider ? divider : 1) * 2) > hz && divider < 0x3ff)
		divider = divider ? divider + 1 : 1;

	/* Stop the clock before changing it: the card samples on its edges. */
	emmc_write(SDHCI_CLOCK_CONTROL,
	           emmc_read(SDHCI_CLOCK_CONTROL) & ~(CLOCK_SD_EN | CLOCK_INTERNAL_EN));

	u32 ctl = ((divider & 0xff) << 8) | (((divider >> 8) & 0x3) << 6) |
	          CLOCK_INTERNAL_EN;
	emmc_write(SDHCI_CLOCK_CONTROL,
	           (emmc_read(SDHCI_CLOCK_CONTROL) & 0xffff0000u) | ctl);

	for (u32 i = 0; i < EMMC_POLL_LIMIT; i++) {
		if (emmc_read(SDHCI_CLOCK_CONTROL) & CLOCK_INTERNAL_STABLE) {
			emmc_write(SDHCI_CLOCK_CONTROL,
			           emmc_read(SDHCI_CLOCK_CONTROL) | CLOCK_SD_EN);
			return 0;
		}
		emmc_delay(1);
	}
	return -1;
}

/* One field out of a 136-bit response.
 *
 * The controller drops the CRC byte and left-justifies what is left, so
 * RESPONSE0..3 hold CSD bits [127:8] — CSD bit n is response bit n - 8. Every
 * field this driver reads is narrower than 32 bits. */
static u32 csd_field(const u32 r[4], u32 msb, u32 lsb)
{
	u32 width = msb - lsb + 1;
	u32 shift = lsb - 8;
	u32 word = shift / 32;
	u32 off = shift % 32;
	u64 v;

	if (word >= 4)
		return 0;
	v = (u64)r[word] >> off;
	if (off && word + 1 < 4)
		v |= (u64)r[word + 1] << (32 - off);
	if (width >= 32)
		return (u32)v;
	return (u32)(v & (((u64)1 << width) - 1));
}

static u64 emmc_capacity_blocks(const u32 csd[4])
{
	u32 version = csd_field(csd, 127, 126);

	if (version == 1) {
		/* CSD 2.0 (SDHC/SDXC): capacity in 512 KiB units. */
		u64 c_size = csd_field(csd, 69, 48);
		return (c_size + 1) * 1024;
	}
	/* CSD 1.0 (standard capacity). */
	u64 c_size = csd_field(csd, 73, 62);
	u32 mult = csd_field(csd, 49, 47);
	u32 read_bl_len = csd_field(csd, 83, 80);
	u64 bytes = (c_size + 1) * ((u64)1 << (mult + 2)) * ((u64)1 << read_bl_len);

	return bytes / SD_BLOCK_SIZE;
}

/*
 * One transfer of `count` blocks. Reading and writing differ only in the
 * direction bit and which way the buffer-port loop runs, so they share this.
 */
static int emmc_transfer(u64 lba, u32 count, void *buffer, int write)
{
	u8 *bytes = (u8 *)buffer;
	u32 index;
	u32 xfer = XFER_BLKCNT_EN;

	if (!g_present || count == 0)
		return -1;
	if (lba + count > g_host.blk.block_count)
		return -1;

	if (count > 1) {
		xfer |= XFER_MULTI | XFER_AUTO_CMD12;
		index = write ? SD_WRITE_MULTIPLE : SD_READ_MULTIPLE;
	} else {
		index = write ? SD_WRITE_SINGLE : SD_READ_SINGLE;
	}
	if (!write)
		xfer |= XFER_READ;

	if (emmc_wait_present_clear(PRESENT_CMD_INHIBIT | PRESENT_DATA_INHIBIT) != 0)
		return -1;

	emmc_write(SDHCI_BLOCK_SIZE, (count << 16) | SD_BLOCK_SIZE);
	emmc_write16(SDHCI_TRANSFER_MODE, (u16)xfer);

	/* A standard-capacity card is addressed in bytes, a high-capacity one in
	 * blocks. Getting this backwards reads the right data from the wrong place
	 * on one of them and nowhere at all on the other. */
	u32 arg = g_host.high_capacity ? (u32)lba : (u32)(lba * SD_BLOCK_SIZE);

	if (emmc_command(index, arg, CMD_RESP_48,
	                 CMD_CRC_CHECK | CMD_INDEX_CHECK | CMD_DATA_PRESENT,
	                 0) != 0)
		return -1;

	for (u32 b = 0; b < count; b++) {
		if (emmc_wait_int(write ? INT_SPACE_AVAIL : INT_DATA_AVAIL) != 0)
			return -1;
		for (u32 w = 0; w < SD_BLOCK_SIZE / 4; w++) {
			if (write) {
				u32 v;

				memcpy(&v, bytes, 4);
				emmc_write(SDHCI_BUFFER, v);
			} else {
				u32 v = emmc_read(SDHCI_BUFFER);

				memcpy(bytes, &v, 4);
			}
			bytes += 4;
		}
	}
	/* The card may still be programming after the last word went in; the
	 * transfer is not over until the controller says so. */
	return emmc_wait_int(INT_XFER_COMPLETE);
}

static int emmc_read_blocks(struct block_device *dev, u64 lba, u32 count,
                            void *buffer)
{
	(void)dev;
	return emmc_transfer(lba, count, buffer, 0);
}

static int emmc_write_blocks(struct block_device *dev, u64 lba, u32 count,
                             const void *buffer)
{
	(void)dev;
	return emmc_transfer(lba, count, (void *)(usize)buffer, 1);
}

/* Bring the host controller itself to a known state. */
static int emmc_host_reset(void)
{
	emmc_write(SDHCI_CLOCK_CONTROL, RESET_ALL);
	for (u32 i = 0; i < EMMC_POLL_LIMIT; i++) {
		if (!(emmc_read(SDHCI_CLOCK_CONTROL) & (RESET_ALL | RESET_CMD | RESET_DATA)))
			break;
		emmc_delay(1);
		if (i == EMMC_POLL_LIMIT - 1)
			return -1;
	}

	/* The controller's own clock, in MHz, from the low byte of Capabilities.
	 * A part that reports zero there expects the driver to know; 100 MHz is
	 * what a BCM2711's EMMC2 runs at. */
	u32 caps = emmc_read(SDHCI_CAPABILITIES);
	u32 mhz = (caps >> 8) & 0xff;

	g_host.base_clock_hz = mhz ? mhz * 1000000u : 100000000u;

	/* Power the bus at 3.3 V, and take the maximum data timeout. */
	emmc_write(SDHCI_HOST_CONTROL, POWER_330);
	emmc_write(SDHCI_HOST_CONTROL, POWER_330 | POWER_ON);
	emmc_write(SDHCI_CLOCK_CONTROL,
	           (emmc_read(SDHCI_CLOCK_CONTROL) & ~0x00ff0000u) | 0x000e0000u);

	/* Every status bit is latched so the polls below can see it; no bit is
	 * allowed to signal, because this driver never enables the IRQ. */
	emmc_write(SDHCI_INT_ENABLE, 0xffffffffu);
	emmc_write(SDHCI_SIGNAL_ENABLE, 0);
	emmc_write(SDHCI_INT_STATUS, 0xffffffffu);

	/* 400 kHz is the rate the SD specification requires until the card has
	 * been identified. */
	return emmc_set_clock(400000);
}

/* The SD card identification sequence. */
static int emmc_card_init(void)
{
	u32 resp[4];

	if (emmc_command(SD_GO_IDLE, 0, CMD_RESP_NONE, 0, 0) != 0)
		return -1;

	/* CMD8 tells a v2 card which voltage the host runs at and asks it to echo
	 * a check pattern back. A v1 card does not answer at all, which is not an
	 * error — it only means the ACMD41 below must not ask for high capacity. */
	int v2 = (emmc_command(SD_SEND_IF_COND, 0x1aa, CMD_RESP_48,
	                       CMD_CRC_CHECK | CMD_INDEX_CHECK, resp) == 0 &&
	          (resp[0] & 0xff) == 0xaa);

	/* ACMD41 both starts the card's power-up and reports when it is done.
	 * Bit 31 of the response is "initialisation complete"; bit 30 is
	 * "this card is high capacity", and is only meaningful once bit 31 is set. */
	u32 ocr = 0;
	int ready = 0;

	for (u32 i = 0; i < 1000 && !ready; i++) {
		u32 arg = 0x00ff8000u | (v2 ? (1u << 30) : 0u);

		if (emmc_app_command(SD_APP_SEND_OP_COND, arg, CMD_RESP_48, 0,
		                     resp) != 0)
			return -1;
		ocr = resp[0];
		ready = (ocr & (1u << 31)) != 0;
		if (!ready)
			emmc_delay(2000);
	}
	if (!ready)
		return -1;
	g_host.high_capacity = v2 && (ocr & (1u << 30)) != 0;

	if (emmc_command(SD_ALL_SEND_CID, 0, CMD_RESP_136, CMD_CRC_CHECK, resp) != 0)
		return -1;
	if (emmc_command(SD_SEND_RELATIVE_ADDR, 0, CMD_RESP_48,
	                 CMD_CRC_CHECK | CMD_INDEX_CHECK, resp) != 0)
		return -1;
	g_host.rca = resp[0] >> 16;

	if (emmc_command(SD_SEND_CSD, g_host.rca << 16, CMD_RESP_136,
	                 CMD_CRC_CHECK, resp) != 0)
		return -1;
	g_host.blk.block_count = emmc_capacity_blocks(resp);
	if (!g_host.blk.block_count)
		return -1;

	if (emmc_command(SD_SELECT_CARD, g_host.rca << 16, CMD_RESP_48_BUSY,
	                 CMD_CRC_CHECK | CMD_INDEX_CHECK, resp) != 0)
		return -1;
	if (emmc_command(SD_SET_BLOCKLEN, SD_BLOCK_SIZE, CMD_RESP_48,
	                 CMD_CRC_CHECK | CMD_INDEX_CHECK, resp) != 0)
		return -1;

	/* Four data lines instead of one. The card has to be told before the host
	 * is, or the two disagree about how wide the next response is. A card that
	 * refuses is left at one bit rather than failing the probe. */
	if (emmc_app_command(SD_APP_SET_BUS_WIDTH, 2, CMD_RESP_48,
	                     CMD_CRC_CHECK | CMD_INDEX_CHECK, resp) == 0) {
		emmc_write(SDHCI_HOST_CONTROL,
		           emmc_read(SDHCI_HOST_CONTROL) | HOSTCTL_4BIT);
	}

	/* Identification is over, so the clock can come up to the 25 MHz every SD
	 * card supports. Anything faster needs the timing modes this driver does
	 * not implement. */
	emmc_write(SDHCI_HOST_CONTROL,
	           emmc_read(SDHCI_HOST_CONTROL) | HOSTCTL_HISPEED);
	return emmc_set_clock(25000000);
}

void bcm2711_emmc_init(void)
{
	u64 emmc2 = fdt_emmc2_base();
	/* A Pi 4 boots from EMMC2, but the older EMMC controller sits 256 KiB
	 * below it on the same SoC and is where the card lands on a Pi 2/3 - and
	 * on QEMU's raspi4b model. Take whichever one reports a card. */
	u64 candidates[2];
	u32 i;

	if (!emmc2)
		return; /* not this board */

	candidates[0] = emmc2;
	candidates[1] = emmc2 - 0x40000;

	for (i = 0; i < 2; i++) {
		g_host.base = candidates[i];
		if (emmc_read(SDHCI_PRESENT_STATE) & PRESENT_CARD_INSERTED)
			break;
	}
	if (i == 2) {
		console_write("emmc2: no card in the slot\n");
		return;
	}

	console_write("emmc2: controller at 0x");
	console_write_hex64(g_host.base);
	console_write("\n");

	if (emmc_host_reset() != 0) {
		console_write("emmc2: the host controller did not reset\n");
		return;
	}
	if (emmc_card_init() != 0) {
		console_write("emmc2: the card did not initialise\n");
		return;
	}

	g_present = 1;
	g_host.blk.block_size = SD_BLOCK_SIZE;
	g_host.blk.read_blocks = emmc_read_blocks;
	g_host.blk.write_blocks = emmc_write_blocks;
	g_host.blk.priv = &g_host;
	blk_register_disk(&g_host.blk, "mmcblk", BLK_BUS_MMC);

	console_write("emmc2: ");
	console_write(g_host.blk.name);
	console_write(" ");
	console_write_dec(g_host.blk.block_count / 2048);
	console_write(" MiB, ");
	console_write(g_host.high_capacity ? "high capacity" : "standard capacity");
	console_write("\n");
}

#else  /* !__aarch64__ */

void bcm2711_emmc_init(void) {}

#endif
