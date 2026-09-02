/*
 * The BCM2835/2711 GPIO block.
 *
 * 54 pins, each with eight possible functions selected three bits at a time
 * out of six GPFSEL registers, and set/clear/level registers that are two
 * words wide because 54 does not fit in one. Writing a one to a bit of GPSET
 * or GPCLR drives that pin; writing a zero does nothing at all, which is what
 * makes the pair safe to use from two places without a read-modify-write.
 */

#include <b1nix/bcm2835.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/types.h>

#if defined(__aarch64__)

#define GPIO_PINS      54

#define GPFSEL0        0x00
#define GPSET0         0x1c
#define GPCLR0         0x28
#define GPLEV0         0x34

static u64 g_gpio;

static inline u32 gpio_read(u32 off)
{
	return *(volatile u32 *)(usize)(g_gpio + off);
}

static inline void gpio_write(u32 off, u32 val)
{
	*(volatile u32 *)(usize)(g_gpio + off) = val;
}

int bcm2835_gpio_ready(void) { return g_gpio != 0; }

int bcm2835_gpio_set_function(u32 pin, u32 function)
{
	u32 reg, shift, val;

	if (!g_gpio || pin >= GPIO_PINS || function > 7)
		return -1;
	reg = GPFSEL0 + (pin / 10) * 4;
	shift = (pin % 10) * 3;
	val = gpio_read(reg);
	val &= ~(7u << shift);
	val |= function << shift;
	gpio_write(reg, val);
	return 0;
}

int bcm2835_gpio_get_function(u32 pin, u32 *function)
{
	if (!g_gpio || pin >= GPIO_PINS || !function)
		return -1;
	*function = (gpio_read(GPFSEL0 + (pin / 10) * 4) >> ((pin % 10) * 3)) & 7u;
	return 0;
}

int bcm2835_gpio_set(u32 pin, int high)
{
	if (!g_gpio || pin >= GPIO_PINS)
		return -1;
	gpio_write((high ? GPSET0 : GPCLR0) + (pin / 32) * 4, 1u << (pin % 32));
	return 0;
}

int bcm2835_gpio_level(u32 pin)
{
	if (!g_gpio || pin >= GPIO_PINS)
		return -1;
	return (gpio_read(GPLEV0 + (pin / 32) * 4) >> (pin % 32)) & 1u;
}

void bcm2835_gpio_init(void)
{
	g_gpio = fdt_gpio_base();
	if (!g_gpio)
		return;
	console_write("gpio: BCM2835 GPIO at 0x");
	console_write_hex64(g_gpio);
	console_write("\n");
}

/*
 * Test mode: drive a pin and read it back.
 *
 * GPIO 21 is on the 40-pin header and belongs to nothing on a stock board, so
 * driving it disturbs no peripheral. An output pin reads back what it is
 * driving, which is what makes this a real check rather than a register echo:
 * the level register is not the one that was written.
 */
void bcm2835_gpio_selftest(void)
{
	const u32 pin = 21;
	u32 func = 0;
	int high, low;

	if (!g_gpio)
		return;

	if (bcm2835_gpio_set_function(pin, BCM_GPIO_OUT) != 0 ||
	    bcm2835_gpio_get_function(pin, &func) != 0 || func != BCM_GPIO_OUT) {
		console_write("M109-RPI: FAIL gpio-function\n");
		return;
	}
	console_write("M109-RPI: ok gpio-function\n");

	bcm2835_gpio_set(pin, 1);
	high = bcm2835_gpio_level(pin);
	bcm2835_gpio_set(pin, 0);
	low = bcm2835_gpio_level(pin);

	if (high == 1 && low == 0)
		console_write("M109-RPI: ok gpio-drive\n");
	else
		console_write("M109-RPI: FAIL gpio-drive\n");

	bcm2835_gpio_set_function(pin, BCM_GPIO_IN);
}

#else

void bcm2835_gpio_init(void) {}
void bcm2835_gpio_selftest(void) {}
int bcm2835_gpio_ready(void) { return 0; }

#endif
