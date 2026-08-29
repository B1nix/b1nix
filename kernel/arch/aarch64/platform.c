#include "platform.h"
#include <string.h>

struct platform_desc {
	const char *compatible;
	const char *name;
	int type;
	u64 uart;
	u64 aux;
	u64 gicd;
	u64 gicc;
};

static const struct platform_desc platforms[] = {
	{ "brcm,bcm2711",     "Raspberry Pi 4", PLATFORM_RPI4,      0xFE201000ULL, 0xFE215000ULL, 0xFF841000ULL, 0xFF842000ULL },
	{ "linux,dummy-virt", "QEMU virt",     PLATFORM_QEMU_VIRT, 0x09000000ULL, 0ULL,          0x08000000ULL, 0x08010000ULL },
	/* Snapdragon 855. No console UART base: the SoC tree marks every GENI
	 * serial engine `status = "disabled"`, and this kernel has no clock or
	 * pinctrl driver to bring one up. A zero here means serial.c prints
	 * nowhere, which on this board is correct — the console is the panel the
	 * bootloader left scanning. Guessing a base instead spins tens of
	 * thousands of MMIO reads per character against a block that never
	 * answers. The GIC is a v3, so "gicc" names the redistributor region
	 * rather than a CPU interface. */
	{ "qcom,sm8150",      "Qualcomm SM8150", PLATFORM_SM8150,  0ULL,          0ULL,          0x17A00000ULL, 0x17A60000ULL },
};

#define DEFAULT_PLATFORM_INDEX 1 /* QEMU virt fallback */

static u64 g_uart_base = 0x09000000ULL;
static u64 g_aux_base = 0;
static u64 g_gicd_base = 0x08000000ULL;
static u64 g_gicc_base = 0x08010000ULL;
static const char *g_platform_name = "QEMU virt";
static int g_platform_type = PLATFORM_QEMU_VIRT;
static int g_platform_detected = 0;

static u32 fdt32_to_cpu(u32 val)
{
	return ((val & 0xFF000000u) >> 24) |
	       ((val & 0x00FF0000u) >> 8) |
	       ((val & 0x0000FF00u) << 8) |
	       ((val & 0x000000FFu) << 24);
}

static u64 find_valid_fdt(u64 dtb_addr)
{
	if (dtb_addr && fdt32_to_cpu(*(const u32 *)dtb_addr) == 0xd00dfeed)
		return dtb_addr;
	if (fdt32_to_cpu(*(const u32 *)0x81F00000ULL) == 0xd00dfeed)
		return 0x81F00000ULL;
	if (fdt32_to_cpu(*(const u32 *)0x81E00000ULL) == 0xd00dfeed)
		return 0x81E00000ULL;
	for (u64 cand = 0x80000000ULL; cand <= 0x85000000ULL; cand += 0x100000ULL) {
		if (fdt32_to_cpu(*(const u32 *)cand) == 0xd00dfeed)
			return cand;
	}
	return 0;
}

void platform_detect(u64 dtb_addr)
{
	if (g_platform_detected)
		return;

	/* Set defaults */
	g_uart_base = platforms[DEFAULT_PLATFORM_INDEX].uart;
	g_gicd_base = platforms[DEFAULT_PLATFORM_INDEX].gicd;
	g_gicc_base = platforms[DEFAULT_PLATFORM_INDEX].gicc;
	g_platform_name = platforms[DEFAULT_PLATFORM_INDEX].name;
	g_platform_type = platforms[DEFAULT_PLATFORM_INDEX].type;

	u64 valid_dtb = find_valid_fdt(dtb_addr);
	if (!valid_dtb) {
		g_platform_detected = 1;
		return;
	}
	dtb_addr = valid_dtb;

	const u32 *fdt = (const u32 *)dtb_addr;
	if (fdt32_to_cpu(fdt[0]) != 0xd00dfeed) {
		g_platform_detected = 1;
		return;
	}

	u32 off_struct = fdt32_to_cpu(fdt[2]);
	u32 off_strings = fdt32_to_cpu(fdt[3]);
	const char *strings = (const char *)dtb_addr + off_strings;
	const u32 *p = (const u32 *)((const char *)dtb_addr + off_struct);

	int depth = 0;
	while (*p) {
		u32 tag = fdt32_to_cpu(*p++);
		if (tag == 1) { /* FDT_BEGIN_NODE */
			const char *name = (const char *)p;
			depth++;
			p += (strlen(name) + 1 + 3) / 4;
		} else if (tag == 3) { /* FDT_PROP */
			u32 len = fdt32_to_cpu(*p++);
			u32 nameoff = fdt32_to_cpu(*p++);
			const char *prop_name = strings + nameoff;
			const char *val = (const char *)p;

			if (depth == 1 && strcmp(prop_name, "compatible") == 0) {
				for (usize i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++) {
					u32 offset = 0;
					while (offset < len) {
						const char *entry = val + offset;
						if (strcmp(entry, platforms[i].compatible) == 0) {
							g_uart_base = platforms[i].uart;
							g_aux_base = platforms[i].aux;
							g_gicd_base = platforms[i].gicd;
							g_gicc_base = platforms[i].gicc;
							g_platform_name = platforms[i].name;
							g_platform_type = platforms[i].type;
							g_platform_detected = 1;
							return;
						}
						offset += (u32)strlen(entry) + 1;
					}
				}
			}
			p += (len + 3) / 4;
		} else if (tag == 2) { /* FDT_END_NODE */
			if (depth > 0)
				depth--;
			if (depth == 0)
				break;
		} else if (tag == 4) { /* FDT_NOP */
			continue;
		} else if (tag == 9) { /* FDT_END */
			break;
		}
	}

	g_platform_detected = 1;
}

u64 platform_uart_base(void) { return g_uart_base; }
u64 platform_aux_base(void) { return g_aux_base; }
u64 platform_gicd_base(void) { return g_gicd_base; }
u64 platform_gicc_base(void) { return g_gicc_base; }
const char *platform_name(void) { return g_platform_name; }
int platform_type(void) { return g_platform_type; }
void platform_set_uart_base(u64 base) { if (base) g_uart_base = base; }
void platform_set_aux_base(u64 base) { if (base) g_aux_base = base; }
