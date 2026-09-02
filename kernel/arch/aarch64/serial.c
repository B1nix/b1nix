#include <b1nix/serial.h>
#include <b1nix/errno.h>
#include <b1nix/types.h>
#include <b1nix/bootinfo.h>
#include "platform.h"

/*
 * Two console controllers, because two boards put the console on different
 * ones.
 *
 * The base address is retrieved via platform_uart_base(): initialized by
 * platform_detect() from DTB at the very start of kernel_main().
 *
 *  - PL011 (QEMU virt at 0x09000000, a Raspberry Pi 4 at 0xfe201000). An ARM
 *    PrimeCell: data register at + 0x00, flags at + 0x18.
 *
 *  - BCM2835 mini-UART, the "aux" UART. On a Pi 4 with the firmware's default
 *    config the PL011 belongs to the Bluetooth controller and this is the port
 *    a serial cable sees, so on that board it is the difference between a
 *    kernel that boots silently and one that boots. It is an 8250-shaped
 *    device — receive/transmit register, line status register — sitting 0x40
 *    into a block it shares with two SPI masters, and none of its registers
 *    answer until AUX_ENABLES (block + 0x04) has the bit for it set.
 *
 * g_aarch64_uart_is_mini says which one platform_uart_base() names.
 */

#define UART_BASE       (platform_uart_base())

/* PL011 */
#define PL011_DR        (*(volatile u32 *)(UART_BASE + 0x00))
#define PL011_FR        (*(volatile u32 *)(UART_BASE + 0x18))
#define PL011_FR_TXFF   (1u << 5)
#define PL011_FR_RXFE   (1u << 4)

/* BCM2835 AUX block, and the mini-UART inside it. */
#define AUX_BASE        (platform_aux_base() ? platform_aux_base() : g_aarch64_aux_base)
#define AUX_ENABLES     (*(volatile u32 *)(AUX_BASE + 0x04))
#define AUX_ENABLE_MU   (1u << 0)

#define MU_IO           (*(volatile u32 *)(UART_BASE + 0x00))
#define MU_IER          (*(volatile u32 *)(UART_BASE + 0x04))
#define MU_IIR          (*(volatile u32 *)(UART_BASE + 0x08))
#define MU_LCR          (*(volatile u32 *)(UART_BASE + 0x0c))
#define MU_MCR          (*(volatile u32 *)(UART_BASE + 0x10))
#define MU_LSR          (*(volatile u32 *)(UART_BASE + 0x14))
#define MU_CNTL         (*(volatile u32 *)(UART_BASE + 0x20))
#define MU_BAUD         (*(volatile u32 *)(UART_BASE + 0x28))

#define MU_LSR_RX_READY (1u << 0)  /* a byte is waiting in the receive FIFO */
#define MU_LSR_TX_EMPTY (1u << 5)  /* the transmit FIFO can take a byte */

#define MU_LCR_8BIT     0x3u       /* the datasheet's "7-bit" bit is an erratum:
                                    * bits 1:0 are the word length and 3 is 8. */
#define MU_IIR_CLEAR_FIFOS 0xc6u
#define MU_CNTL_RX_EN   (1u << 0)
#define MU_CNTL_TX_EN   (1u << 1)

/* The mini-UART's baud divisor is derived from the VPU core clock, which this
 * kernel has no way to ask about — reading it means talking to the firmware
 * over the mailbox. So the firmware's own divisor is kept whenever there is
 * one, and this is only the fallback for a port that was left disabled: 500 MHz
 * is the core clock the Pi 4 firmware runs by default, and
 * divisor = clock / (8 * baud) - 1. */
#define MU_CORE_CLOCK_HZ 500000000u
#define MU_FALLBACK_BAUD 115200u


/* ── Qualcomm GENI QUP UART (Snapdragon 855 / SM8150 ttyMSM0) ────────────────
 *
 * Neither a PL011 nor an 8250: bytes are not stored into a data register, they
 * are handed to a serial engine that runs a TX *command* over a FIFO. The
 * bootloader has already clocked, pinmuxed and configured the port (it prints
 * its own log through it), so none of that is repeated here - this only issues
 * commands, which is all a console needs.
 */
#define GENI_STATUS            0x040
#define GENI_STATUS_M_ACTIVE   (1u << 0)
#define GENI_M_CMD0            0x600
#define GENI_M_IRQ_STATUS      0x610
#define GENI_M_IRQ_CLEAR       0x618
#define GENI_TX_FIFO           0x700
#define GENI_RX_FIFO           0x780
#define GENI_RX_FIFO_STATUS    0x804
#define GENI_TX_WATERMARK      0x80c
#define GENI_UART_TX_TRANS_LEN 0x270
#define GENI_M_CMD_DONE        (1u << 0)
#define GENI_M_TX_WATERMARK    (1u << 30)
#define GENI_UART_START_TX     (1u << 27) /* opcode 1, M_OPCODE_SHFT = 27 */

/* Set by bootinfo when the tree's console node is a "qcom,msm-geni-console". */
int g_aarch64_uart_is_geni;

#define GENI_REG(off) (*(volatile u32 *)(UART_BASE + (off)))

/* Bounded spin: a wedged serial engine must not wedge the kernel with it. */
static int geni_wait(u32 off, u32 mask, int set)
{
	for (u32 i = 0; i < 100000; i++) {
		u32 v = GENI_REG(off);

		if (set ? (v & mask) : !(v & mask))
			return 1;
	}
	return 0;
}

/* ponytail: one command per byte. A console is not a throughput path, and
 * batching means tracking a partially-drained FIFO across calls. */
static void geni_putc(char c)
{
	geni_wait(GENI_STATUS, GENI_STATUS_M_ACTIVE, 0);
	GENI_REG(GENI_UART_TX_TRANS_LEN) = 1;
	GENI_REG(GENI_TX_WATERMARK) = 1;
	GENI_REG(GENI_M_CMD0) = GENI_UART_START_TX;

	if (!geni_wait(GENI_M_IRQ_STATUS, GENI_M_TX_WATERMARK, 1))
		return;
	GENI_REG(GENI_TX_FIFO) = (u32)(u8)c;
	GENI_REG(GENI_M_IRQ_CLEAR) = GENI_M_TX_WATERMARK;

	if (geni_wait(GENI_M_IRQ_STATUS, GENI_M_CMD_DONE, 1))
		GENI_REG(GENI_M_IRQ_CLEAR) = GENI_M_CMD_DONE;
}

/* Some boards have no console UART this kernel can drive: every serial engine
 * the tree describes is marked disabled, and bringing one up needs the clock
 * and pinctrl drivers this port does not have. platform_uart_base() is zero
 * there, and every entry point below turns into a no-op rather than reading
 * and writing a register window that belongs to a powered-down block. */
static inline int serial_absent(void)
{
	return UART_BASE == 0;
}

void serial_init(void)
{
	if (serial_absent())
		return;

	/* The GENI engine is left exactly as the bootloader configured it. */
	if (g_aarch64_uart_is_geni)
		return;

	/* Always initialize PL011 on QEMU and RPi4 */
	volatile u32 *pl011_cr = (volatile u32 *)((platform_type() == PLATFORM_RPI4 ? 0xfe201000ULL : UART_BASE) + 0x30);
	*pl011_cr |= (1u << 0) | (1u << 8) | (1u << 9);

	if (platform_type() == PLATFORM_RPI4 || g_aarch64_uart_is_mini) {
		/* Also initialize Mini-UART on Raspberry Pi 4 */
		u64 aux = platform_aux_base() ? platform_aux_base() : (g_aarch64_aux_base ? g_aarch64_aux_base : 0xfe215000ULL);
		u64 mu = aux + 0x40;

		/*
		 * Leave a port the firmware already set up exactly as it is.
		 *
		 * With enable_uart=1 the firmware enables this UART, programs its
		 * divisor from the core clock it actually chose, and prints its own
		 * log through it - a log that is readable, which is proof the
		 * settings are right. Re-deriving the divisor here cannot improve on
		 * that and can easily be wrong: the divisor follows the VPU core
		 * clock, which this code has no way to read and which the firmware
		 * scales unless it is pinned. Getting it wrong does not silence the
		 * port, it garbles it - the same bytes at the wrong bit rate, which
		 * is exactly what a bring-up sees as "the strobe was readable and
		 * everything after it is noise".
		 *
		 * So: touch nothing when the mini-UART is already enabled. A port the
		 * firmware left disabled still gets the full sequence below, because
		 * then there is no configuration to preserve.
		 */
		if (*(volatile u32 *)(aux + 0x04) & AUX_ENABLE_MU) {
			/* Enabled by the firmware: make sure the transmitter is on and
			 * leave the divisor, the line control and the FIFOs alone. */
			*(volatile u32 *)(mu + 0x20) |= MU_CNTL_RX_EN | MU_CNTL_TX_EN;
		} else {
			*(volatile u32 *)(aux + 0x04) |= AUX_ENABLE_MU;
			*(volatile u32 *)(mu + 0x04) = 0;
			*(volatile u32 *)(mu + 0x20) = 0;
			*(volatile u32 *)(mu + 0x0c) = MU_LCR_8BIT;
			*(volatile u32 *)(mu + 0x10) = 0;
			*(volatile u32 *)(mu + 0x08) = MU_IIR_CLEAR_FIFOS;
			if (*(volatile u32 *)(mu + 0x28) == 0)
				*(volatile u32 *)(mu + 0x28) = MU_CORE_CLOCK_HZ / (8 * MU_FALLBACK_BAUD) - 1;
			*(volatile u32 *)(mu + 0x20) = MU_CNTL_RX_EN | MU_CNTL_TX_EN;
		}
	}
}

void serial_putc(char c)
{
	if (serial_absent())
		return;

	if (platform_type() == PLATFORM_RPI4) {
		/* Dual-write to both PL011 and Mini-UART on RPi4 */
		volatile u32 *pl011_fr = (volatile u32 *)(0xfe201000ULL + 0x18);
		volatile u32 *pl011_dr = (volatile u32 *)(0xfe201000ULL + 0x00);
		for (u32 i = 0; i < 10000; i++) {
			if (!(*pl011_fr & PL011_FR_TXFF))
				break;
		}
		*pl011_dr = (u32)(u8)c;

		volatile u32 *mu_lsr = (volatile u32 *)(0xfe215040ULL + 0x14);
		volatile u32 *mu_io  = (volatile u32 *)(0xfe215040ULL + 0x00);
		for (u32 i = 0; i < 10000; i++) {
			if (*mu_lsr & MU_LSR_TX_EMPTY)
				break;
		}
		*mu_io = (u32)(u8)c;
		return;
	}

	if (g_aarch64_uart_is_geni) {
		geni_putc(c);
		return;
	}

	if (g_aarch64_uart_is_mini) {
		for (u32 i = 0; i < 100000; i++) {
			if (MU_LSR & MU_LSR_TX_EMPTY)
				break;
		}
		MU_IO = (u32)(u8)c;
		return;
	}
	for (u32 i = 0; i < 100000; i++) {
		if (!(PL011_FR & PL011_FR_TXFF))
			break;
	}
	PL011_DR = (u32)(u8)c;
}


void serial_write(const char *text)
{
	if (!text) return;
	while (*text) {
		if (*text == '\n')
			serial_putc('\r');
		serial_putc(*text++);
	}
}

int serial_has_data(void)
{
	if (serial_absent())
		return 0;
	/* GENI receive needs its own RX command sequence; a phone has no serial
	 * cable attached anyway, so the console there is output-only.
	 * ponytail: add the RX path when something actually types into it. */
	if (g_aarch64_uart_is_geni)
		return 0;
	if (g_aarch64_uart_is_mini)
		return (MU_LSR & MU_LSR_RX_READY) != 0;
	return !(PL011_FR & PL011_FR_RXFE);
}

char serial_getc(void)
{
	if (!serial_has_data())
		return 0;
	if (g_aarch64_uart_is_mini)
		return (char)(MU_IO & 0xff);
	return (char)(PL011_DR & 0xFF);
}

int serial_port_present(int idx)
{
	return (idx == 0) && !serial_absent();
}

void serial_port_putc(int idx, char ch)
{
	if (idx == 0) serial_putc(ch);
}

char serial_port_getc(int idx)
{
	return (idx == 0) ? serial_getc() : 0;
}

int serial_port_has_data(int idx)
{
	return (idx == 0) ? serial_has_data() : 0;
}

/*
 * The tty layer's chip-level knobs. Every one of them is an 8250 idea — a
 * divisor latch, a modem control register, an I/O port number — and none of
 * the three controllers here has any of them: a PL011 has no port address, the
 * mini-UART's divisor follows a clock this kernel cannot read, and a GENI's
 * line settings live behind the serial engine's own command interface.
 *
 * ponytail: report "not configurable" rather than invent registers. The tty
 * layer already handles a port that refuses, and nothing on these boards has a
 * modem attached to the other end of the line.
 */
int serial_port_set_line(int idx, u32 baud, u8 data_bits, u8 parity,
                         u8 stop_bits)
{
	(void)idx; (void)baud; (void)data_bits; (void)parity; (void)stop_bits;
	/* -ENOSYS, not -EINVAL: nothing is wrong with what was asked, this
	 * controller simply has no divisor this kernel can reach. The tty layer
	 * keeps the rest of the termios on this code and fails the call on
	 * -EINVAL. */
	return -ENOSYS;
}

int serial_port_get_line(int idx, u32 *baud, u8 *data_bits, u8 *parity,
                         u8 *stop_bits)
{
	if (idx != 0)
		return -1;
	if (baud) *baud = 115200;
	if (data_bits) *data_bits = 8;
	if (parity) *parity = 0;
	if (stop_bits) *stop_bits = 1;
	return 0;
}

u8 serial_port_get_mcr(int idx) { (void)idx; return 0; }
void serial_port_set_mcr(int idx, u8 mcr) { (void)idx; (void)mcr; }
u8 serial_port_get_msr(int idx) { (void)idx; return 0; }

/* TIOCGSERIAL's port field. There is no I/O port space on arm64. */
u16 serial_port_base(int idx) { (void)idx; return 0; }
